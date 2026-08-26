#include "universe/ui/simulation_controller.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace {

using universe::BiologicalSex;
using universe::Household;
using universe::HouseholdId;
using universe::KnownLifeStatus;
using universe::KnowledgeFact;
using universe::KnowledgeFactKind;
using universe::CommunicationMethod;
using universe::MemorySource;
using universe::MemoryId;
using universe::Person;
using universe::PersonId;
using universe::Settlement;
using universe::SettlementId;
using universe::AgentMemory;
using universe::EventId;
using universe::EventType;
using universe::SimulationEvent;
using universe::ui::SimulationController;
using universe::ui::SettlementSummaryView;
using universe::ui::SimulationSummary;

struct UiHistory {
    std::deque<float> population;
    std::deque<float> food_stored;
    std::deque<float> unmet_food;
    std::deque<float> average_health;

    void push(const SimulationSummary& summary) {
        constexpr std::size_t kMaxPoints = 600;

        population.push_back(static_cast<float>(summary.population));
        food_stored.push_back(static_cast<float>(summary.food_stored));
        unmet_food.push_back(static_cast<float>(summary.unmet_food));
        average_health.push_back(static_cast<float>(summary.average_health));

        while (population.size() > kMaxPoints) {
            population.pop_front();
            food_stored.pop_front();
            unmet_food.pop_front();
            average_health.pop_front();
        }
    }
};

struct UiState {
    bool paused{true};
    int run_step_index{1};
    std::uint64_t seed_input{42};
    std::uint64_t population_input{200};

    std::array<char, 512> snapshot_path{"build/ui_snapshot.ucore"};
    std::string snapshot_message{};

    std::optional<HouseholdId> selected_household{};
    std::optional<PersonId> selected_person{};
    std::optional<SettlementId> selected_settlement{};

    bool show_birth_events{true};
    bool show_death_events{true};
    bool show_settlement_founded_events{true};
    bool show_settlement_extinct_events{true};

    UiHistory history{};
};

[[nodiscard]] std::uint32_t person_age_years(const SimulationSummary& summary, const Person& person) {
    const auto current_year = static_cast<std::int64_t>(summary.year);
    if (current_year <= person.birth_year) {
        return 0;
    }
    return static_cast<std::uint32_t>(current_year - person.birth_year);
}

[[nodiscard]] const Household* find_household(const std::span<const Household> households, const HouseholdId id) {
    for (const auto& household : households) {
        if (household.id == id) {
            return &household;
        }
    }
    return nullptr;
}

[[nodiscard]] const Settlement* find_settlement(const std::span<const Settlement> settlements, const SettlementId id) {
    for (const auto& settlement : settlements) {
        if (settlement.id == id) {
            return &settlement;
        }
    }
    return nullptr;
}

[[nodiscard]] const Person* find_person(const std::span<const Person> people, const PersonId id) {
    for (const auto& person : people) {
        if (person.id == id) {
            return &person;
        }
    }
    return nullptr;
}

[[nodiscard]] const SimulationEvent* find_event(const std::span<const SimulationEvent> events, const EventId id) {
    for (const auto& event : events) {
        if (event.id == id) {
            return &event;
        }
    }
    return nullptr;
}

[[nodiscard]] const char* event_type_label(const EventType event_type) {
    switch (event_type) {
        case EventType::Birth:
            return "Birth";
        case EventType::Death:
            return "Death";
        case EventType::SettlementFounded:
            return "SettlementFounded";
        case EventType::SettlementExtinct:
            return "SettlementExtinct";
    }
    return "Unknown";
}

[[nodiscard]] const char* memory_source_label(const MemorySource source) {
    switch (source) {
        case MemorySource::DirectObservation:
            return "Direct";
        case MemorySource::Communication:
            return "Communication";
        case MemorySource::Teaching:
            return "Teaching";
    }
    return "Unknown";
}

[[nodiscard]] const char* occupation_label(const universe::Occupation occupation) {
    switch (occupation) {
        case universe::Occupation::None:
            return "None";
        case universe::Occupation::SubsistenceCultivator:
            return "SubsistenceCultivator";
        case universe::Occupation::WildFoodGatherer:
            return "WildFoodGatherer";
        case universe::Occupation::WoodGatherer:
            return "WoodGatherer";
        case universe::Occupation::StoneExtractor:
            return "StoneExtractor";
        case universe::Occupation::CraftWorker:
            return "CraftWorker";
    }
    return "Unknown";
}

[[nodiscard]] const char* known_life_status_label(const KnownLifeStatus status) {
    switch (status) {
        case KnownLifeStatus::Alive:
            return "Alive";
        case KnownLifeStatus::Dead:
            return "Dead";
    }
    return "Unknown";
}

[[nodiscard]] const char* knowledge_source_label(const universe::KnowledgeSource source) {
    switch (source) {
        case universe::KnowledgeSource::MemoryDerived:
            return "MemoryDerived";
    }
    return "Unknown";
}

[[nodiscard]] const char* knowledge_fact_kind_label(const KnowledgeFactKind kind) {
    switch (kind) {
        case KnowledgeFactKind::PersonLifeStatus:
            return "PersonLifeStatus";
        case KnowledgeFactKind::PersonHousehold:
            return "PersonHousehold";
        case KnowledgeFactKind::SettlementStatus:
            return "SettlementStatus";
    }
    return "Unknown";
}

[[nodiscard]] KnowledgeFactKind knowledge_fact_kind(const KnowledgeFact& fact) {
    if (std::holds_alternative<universe::PersonLifeStatusKnowledgeFact>(fact)) {
        return KnowledgeFactKind::PersonLifeStatus;
    }
    if (std::holds_alternative<universe::PersonHouseholdKnowledgeFact>(fact)) {
        return KnowledgeFactKind::PersonHousehold;
    }
    if (std::holds_alternative<universe::SettlementStatusKnowledgeFact>(fact)) {
        return KnowledgeFactKind::SettlementStatus;
    }
    throw std::invalid_argument("Unknown knowledge fact variant");
}

void apply_time_step(SimulationController& controller, const int run_step_index) {
    switch (run_step_index) {
        case 0:
            controller.advance_days(1);
            break;
        case 1:
            controller.advance_days(30);
            break;
        case 2:
            controller.advance_years(1);
            break;
        case 3:
            controller.advance_years(10);
            break;
        default:
            controller.advance_days(30);
            break;
    }
}

[[nodiscard]] float plot_deque_getter(void* data, int index) {
    const auto* values = static_cast<const std::deque<float>*>(data);
    return (*values)[static_cast<std::size_t>(index)];
}

[[nodiscard]] bool run_smoke_test() {
    SimulationController first{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    SimulationController second{universe::SimulationConfig{.seed = 42, .initial_population = 200}};

    first.advance_years(1);
    first.advance_years(1);
    first.advance_years(1);

    second.advance_years(3);

    const auto summary_a = first.summary();
    const auto summary_b = second.summary();
    if (summary_a.year != summary_b.year ||
        summary_a.population != summary_b.population ||
        summary_a.births != summary_b.births ||
        summary_a.deaths != summary_b.deaths) {
        return false;
    }

    const auto save_result = first.save_snapshot_file("ui_smoke_test.ucore");
    if (!save_result.ok) {
        return false;
    }

    SimulationController loaded{universe::SimulationConfig{.seed = 999, .initial_population = 10}};
    const auto load_result = loaded.load_snapshot_file("ui_smoke_test.ucore");
    if (!load_result.ok) {
        return false;
    }

    loaded.advance_years(2);
    first.advance_years(2);

    const auto summary_loaded = loaded.summary();
    const auto summary_expected = first.summary();
    return summary_loaded.year == summary_expected.year &&
           summary_loaded.population == summary_expected.population &&
           summary_loaded.births == summary_expected.births &&
           summary_loaded.deaths == summary_expected.deaths;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke-test") == 0) {
            return run_smoke_test() ? 0 : 1;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("UniverseCore Simulation Inspector", 1400, 900, SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SimulationController controller{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    UiState ui{};
    ui.history.push(controller.summary());

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        try {
            if (!ui.paused) {
                apply_time_step(controller, ui.run_step_index);
                ui.history.push(controller.summary());
            }
        } catch (const std::exception& ex) {
            ui.snapshot_message = std::string{"Simulation advance failed: "} + ex.what();
            ui.paused = true;
        }

        const auto summary = controller.summary();

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        ImGui::Begin("UniverseCore", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        ImGui::Text("Seed %llu | Time %lld | Year %llu",
            static_cast<unsigned long long>(summary.seed),
            static_cast<long long>(summary.time.ticks),
            static_cast<unsigned long long>(summary.year));
        ImGui::Separator();

        ImGui::Columns(4);
        ImGui::Text("Population\n%llu", static_cast<unsigned long long>(summary.population)); ImGui::NextColumn();
        ImGui::Text("Households\n%llu", static_cast<unsigned long long>(summary.households)); ImGui::NextColumn();
        ImGui::Text("Births\n%llu", static_cast<unsigned long long>(summary.births)); ImGui::NextColumn();
        ImGui::Text("Deaths\n%llu", static_cast<unsigned long long>(summary.deaths)); ImGui::NextColumn();
        ImGui::Columns(1);
        ImGui::Separator();

        if (ImGui::Button(ui.paused ? "Run" : "Pause")) {
            ui.paused = !ui.paused;
        }
        ImGui::SameLine();
        if (ImGui::Button("+1 day")) {
            controller.advance_days(1);
            ui.history.push(controller.summary());
        }
        ImGui::SameLine();
        if (ImGui::Button("+30 days")) {
            controller.advance_days(30);
            ui.history.push(controller.summary());
        }
        ImGui::SameLine();
        if (ImGui::Button("+1 year")) {
            controller.advance_years(1);
            ui.history.push(controller.summary());
        }
        ImGui::SameLine();
        if (ImGui::Button("+10 years")) {
            controller.advance_years(10);
            ui.history.push(controller.summary());
        }

        const char* run_steps[] = {"1 day/update", "30 days/update", "1 year/update", "10 years/update"};
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("##runstep", &ui.run_step_index, run_steps, IM_ARRAYSIZE(run_steps));

        ImGui::Separator();
        ImGui::Text("New Simulation");
        ImGui::SetNextItemWidth(180);
        ImGui::InputScalar("Seed", ImGuiDataType_U64, &ui.seed_input);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::InputScalar("Initial population", ImGuiDataType_U64, &ui.population_input);
        ImGui::SameLine();
        if (ImGui::Button("Start / Reset")) {
            controller.reset(universe::SimulationConfig{
                .seed = ui.seed_input,
                .initial_population = ui.population_input,
            });
            ui.selected_settlement.reset();
            ui.selected_household.reset();
            ui.selected_person.reset();
            ui.history = UiHistory{};
            ui.history.push(controller.summary());
            ui.paused = true;
        }

        ImGui::SetNextItemWidth(420);
        ImGui::InputText("Snapshot path", ui.snapshot_path.data(), ui.snapshot_path.size());
        ImGui::SameLine();
        if (ImGui::Button("Save Snapshot")) {
            const auto save_result = controller.save_snapshot_file(ui.snapshot_path.data());
            if (save_result.ok) {
                ui.snapshot_message = "Saved snapshot hash=" + std::to_string(save_result.hash);
            } else {
                ui.snapshot_message = "Save failed: " + save_result.message;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Snapshot")) {
            const auto load_result = controller.load_snapshot_file(ui.snapshot_path.data());
            if (load_result.ok) {
                ui.snapshot_message = "Loaded snapshot hash=" + std::to_string(load_result.hash);
                ui.history.push(controller.summary());
            } else {
                ui.snapshot_message = "Load failed: " + load_result.message;
            }
        }
        if (!ui.snapshot_message.empty()) {
            ImGui::TextUnformatted(ui.snapshot_message.c_str());
        }

        ImGui::Separator();

        if (!ui.history.population.empty()) {
            ImGui::Text("History");
            ImGui::PlotLines("Population", plot_deque_getter, &ui.history.population, static_cast<int>(ui.history.population.size()), 0, nullptr, 0.0f, FLT_MAX, ImVec2(0, 80));
            ImGui::PlotLines("Food stored", plot_deque_getter, &ui.history.food_stored, static_cast<int>(ui.history.food_stored.size()), 0, nullptr, 0.0f, FLT_MAX, ImVec2(0, 80));
            ImGui::PlotLines("Unmet food", plot_deque_getter, &ui.history.unmet_food, static_cast<int>(ui.history.unmet_food.size()), 0, nullptr, 0.0f, FLT_MAX, ImVec2(0, 80));
            ImGui::PlotLines("Average health", plot_deque_getter, &ui.history.average_health, static_cast<int>(ui.history.average_health.size()), 0, nullptr, 0.0f, 1000.0f, ImVec2(0, 80));
        }

        ImGui::Separator();
        ImGui::Text("Food: stored=%lld produced=%lld consumed=%lld unmet=%lld shortage_households=%llu",
            static_cast<long long>(summary.food_stored),
            static_cast<long long>(summary.food_produced),
            static_cast<long long>(summary.food_consumed),
            static_cast<long long>(summary.unmet_food),
            static_cast<unsigned long long>(summary.households_in_shortage));
        ImGui::Text("Health: healthy=%llu mild=%llu severe=%llu critical=%llu avg=%d",
            static_cast<unsigned long long>(summary.healthy_population),
            static_cast<unsigned long long>(summary.mildly_malnourished),
            static_cast<unsigned long long>(summary.severely_malnourished),
            static_cast<unsigned long long>(summary.critical_health),
            summary.average_health);
        ImGui::Text("Labour: capacity=%lld demand=%lld constrained_households=%llu zero_capable_households=%llu",
            static_cast<long long>(summary.total_labour_capacity),
            static_cast<long long>(summary.total_labour_demand),
            static_cast<unsigned long long>(summary.labour_constrained_households),
            static_cast<unsigned long long>(summary.zero_capable_worker_households));
        ImGui::Text("Workers: eligible=%llu active=%llu unallocated=%llu cultivators=%llu gatherers=%llu",
            static_cast<unsigned long long>(summary.eligible_workers),
            static_cast<unsigned long long>(summary.active_workers),
            static_cast<unsigned long long>(summary.unallocated_workers),
            static_cast<unsigned long long>(summary.cultivators),
            static_cast<unsigned long long>(summary.gatherers));
        ImGui::Text("Material workers: wood=%llu stone=%llu craft=%llu",
            static_cast<unsigned long long>(summary.wood_gatherers),
            static_cast<unsigned long long>(summary.stone_extractors),
            static_cast<unsigned long long>(summary.craft_workers));
        ImGui::Text("Specialized labour: alloc=%lld shortfall=%lld | cultivation %lld/%lld | gathering %lld/%lld",
            static_cast<long long>(summary.total_labour_allocated),
            static_cast<long long>(summary.labour_shortfall),
            static_cast<long long>(summary.cultivation_labour_allocated),
            static_cast<long long>(summary.cultivation_labour_available),
            static_cast<long long>(summary.gathering_labour_allocated),
            static_cast<long long>(summary.gathering_labour_available));
        ImGui::Text("Materials labour: demand=%lld alloc=%lld shortfall=%lld | wood %lld/%lld stone %lld/%lld craft %lld/%lld",
            static_cast<long long>(summary.material_labour_demand),
            static_cast<long long>(summary.material_labour_allocated),
            static_cast<long long>(summary.material_labour_shortfall),
            static_cast<long long>(summary.wood_labour_allocated),
            static_cast<long long>(summary.wood_labour_available),
            static_cast<long long>(summary.stone_labour_allocated),
            static_cast<long long>(summary.stone_labour_available),
            static_cast<long long>(summary.craft_labour_allocated),
            static_cast<long long>(summary.craft_labour_available));
        ImGui::Text("Environment materials: wood=%lld/%lld stone=%lld/%lld",
            static_cast<long long>(summary.wood_stock),
            static_cast<long long>(summary.wood_capacity),
            static_cast<long long>(summary.stone_remaining),
            static_cast<long long>(summary.stone_capacity));
        ImGui::Text("Inventory materials: wood=%lld stone=%lld timber=%lld tools=%lld shortfall=%lld",
            static_cast<long long>(summary.wood_inventory),
            static_cast<long long>(summary.stone_inventory),
            static_cast<long long>(summary.timber_inventory),
            static_cast<long long>(summary.basic_tool_inventory),
            static_cast<long long>(summary.material_production_shortfall));
        ImGui::Text("Tools durability: broken=%lld avg_coverage=%u/10000",
            static_cast<long long>(summary.basic_tool_broken),
            summary.average_tool_coverage_per_10k);
        ImGui::Text("Settlements: active=%llu extinct=%llu avg_pop=%llu largest=%llu high_pressure=%llu",
            static_cast<unsigned long long>(summary.active_settlements),
            static_cast<unsigned long long>(summary.extinct_settlements),
            static_cast<unsigned long long>(summary.average_settlement_population),
            static_cast<unsigned long long>(summary.largest_settlement_population),
            static_cast<unsigned long long>(summary.high_pressure_settlements));
        ImGui::Text("Age bands: child=%llu working_age=%llu elderly=%llu",
            static_cast<unsigned long long>(summary.child_population),
            static_cast<unsigned long long>(summary.working_age_population),
            static_cast<unsigned long long>(summary.elderly_population));

        ImGui::Separator();

        const auto households = controller.households();
        const auto settlements = controller.settlements();
        const auto settlement_summaries = controller.settlement_summaries();
        const auto people = controller.people();
        const auto events = controller.events();
        const auto memories = controller.agent_memories();

        std::unordered_map<MemoryId, const AgentMemory*> memory_by_id;
        memory_by_id.reserve(memories.size());
        for (const auto& memory : memories) {
            memory_by_id.emplace(memory.id, &memory);
        }

        std::unordered_map<EventId, const SimulationEvent*> event_by_id;
        event_by_id.reserve(events.size());
        for (const auto& event : events) {
            event_by_id.emplace(event.id, &event);
        }

        ImGui::BeginChild("settlements", ImVec2(0, 180), true);
        ImGui::Text("Settlements");
        if (ImGui::BeginTable("settlement_table", 12, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("Region");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Pop");
            ImGui::TableSetupColumn("Households");
            ImGui::TableSetupColumn("Labour");
            ImGui::TableSetupColumn("Capacity");
            ImGui::TableSetupColumn("Pressure");
            ImGui::TableSetupColumn("FoodStored");
            ImGui::TableSetupColumn("Workers");
            ImGui::TableSetupColumn("Shortfall");
            ImGui::TableSetupColumn("Materials");
            ImGui::TableHeadersRow();

            for (const auto& settlement : settlement_summaries) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const bool selected = ui.selected_settlement.has_value() && ui.selected_settlement.value() == settlement.id;
                if (ImGui::Selectable(std::to_string(settlement.id).c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    ui.selected_settlement = settlement.id;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(std::to_string(settlement.region_id).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(settlement.active ? "active" : "extinct");
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(std::to_string(settlement.alive_population).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(std::to_string(settlement.households).c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(std::to_string(settlement.labour_capacity).c_str());
                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted(std::to_string(settlement.accessible_food_capacity_per_day).c_str());
                ImGui::TableSetColumnIndex(7);
                ImGui::TextUnformatted(std::to_string(settlement.pressure_per_10k).c_str());
                ImGui::TableSetColumnIndex(8);
                ImGui::TextUnformatted(std::to_string(settlement.food_stored).c_str());
                ImGui::TableSetColumnIndex(9);
                ImGui::TextUnformatted(std::to_string(settlement.active_workers).c_str());
                ImGui::TableSetColumnIndex(10);
                ImGui::TextUnformatted(std::to_string(settlement.labour_shortfall).c_str());
                ImGui::TableSetColumnIndex(11);
                ImGui::Text("W:%lld S:%lld T:%lld B:%lld",
                    static_cast<long long>(settlement.wood_inventory),
                    static_cast<long long>(settlement.stone_inventory),
                    static_cast<long long>(settlement.timber_inventory),
                    static_cast<long long>(settlement.basic_tool_inventory));
            }

            ImGui::EndTable();
        }
        ImGui::EndChild();

        ImGui::BeginChild("households", ImVec2(0, 220), true);
        ImGui::Text("Households");
        if (ImGui::BeginTable("household_table", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("Alive");
            ImGui::TableSetupColumn("Total");
            ImGui::TableSetupColumn("Food");
            ImGui::TableSetupColumn("Shortage");
            ImGui::TableSetupColumn("Stress");
            ImGui::TableHeadersRow();

            for (const auto& household : households) {
                std::uint64_t alive_members = 0;
                for (const auto member_id : household.member_ids) {
                    const auto* person = find_person(people, member_id);
                    if (person != nullptr && person->alive) {
                        ++alive_members;
                    }
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const bool selected = ui.selected_household.has_value() && ui.selected_household.value() == household.id;
                if (ImGui::Selectable(std::to_string(household.id).c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    ui.selected_household = household.id;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(std::to_string(alive_members).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(std::to_string(household.member_ids.size()).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(std::to_string(household.resources.food_inventory.value).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(std::to_string(household.resources.consecutive_shortage_days).c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(std::to_string(household.resources.nutritional_stress_points).c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();

        ImGui::BeginChild("inspectors", ImVec2(0, 250), true);
        ImGui::Columns(3);

        ImGui::Text("Settlement Inspector");
        if (ui.selected_settlement.has_value()) {
            const auto* settlement = find_settlement(settlements, *ui.selected_settlement);
            const SettlementSummaryView* settlement_view = nullptr;
            for (const auto& candidate : settlement_summaries) {
                if (candidate.id == *ui.selected_settlement) {
                    settlement_view = &candidate;
                    break;
                }
            }
            if (settlement != nullptr) {
                ImGui::Text("ID: %llu", static_cast<unsigned long long>(settlement->id));
                ImGui::Text("Region: %llu", static_cast<unsigned long long>(settlement->region_id));
                ImGui::Text("Status: %s", settlement->active ? "active" : "extinct");
                ImGui::Text("Access share: %u/10000", settlement->access_share_points);
                if (settlement_view != nullptr) {
                    ImGui::Text("Population: %llu", static_cast<unsigned long long>(settlement_view->alive_population));
                    ImGui::Text("Workers: %llu eligible, %llu active, %llu unallocated",
                        static_cast<unsigned long long>(settlement_view->eligible_workers),
                        static_cast<unsigned long long>(settlement_view->active_workers),
                        static_cast<unsigned long long>(settlement_view->unallocated_workers));
                    ImGui::Text("Occupations: cultivators=%llu gatherers=%llu wood=%llu stone=%llu craft=%llu",
                        static_cast<unsigned long long>(settlement_view->cultivators),
                        static_cast<unsigned long long>(settlement_view->gatherers),
                        static_cast<unsigned long long>(settlement_view->wood_gatherers),
                        static_cast<unsigned long long>(settlement_view->stone_extractors),
                        static_cast<unsigned long long>(settlement_view->craft_workers));
                    ImGui::Text("Labour demand=%lld supplied=%lld shortfall=%lld",
                        static_cast<long long>(settlement_view->labour_demand),
                        static_cast<long long>(settlement_view->labour_allocated),
                        static_cast<long long>(settlement_view->labour_shortfall));
                    ImGui::Text("Cultivation labour %lld/%lld | Gathering labour %lld/%lld",
                        static_cast<long long>(settlement_view->cultivation_labour_allocated),
                        static_cast<long long>(settlement_view->cultivation_labour_available),
                        static_cast<long long>(settlement_view->gathering_labour_allocated),
                        static_cast<long long>(settlement_view->gathering_labour_available));
                    ImGui::Text("Material labour W %lld/%lld | S %lld/%lld | C %lld/%lld",
                        static_cast<long long>(settlement_view->wood_labour_allocated),
                        static_cast<long long>(settlement_view->wood_labour_available),
                        static_cast<long long>(settlement_view->stone_labour_allocated),
                        static_cast<long long>(settlement_view->stone_labour_available),
                        static_cast<long long>(settlement_view->craft_labour_allocated),
                        static_cast<long long>(settlement_view->craft_labour_available));
                    ImGui::Text("Inventory wood=%lld stone=%lld timber=%lld tools=%lld",
                        static_cast<long long>(settlement_view->wood_inventory),
                        static_cast<long long>(settlement_view->stone_inventory),
                        static_cast<long long>(settlement_view->timber_inventory),
                        static_cast<long long>(settlement_view->basic_tool_inventory));
                    ImGui::Text("Tools broken=%lld coverage=%u/10000",
                        static_cast<long long>(settlement_view->cumulative_basic_tool_broken),
                        settlement_view->tool_coverage_per_10k);
                    ImGui::Text("Material shortfall=%lld", static_cast<long long>(settlement_view->material_shortfall));
                }
                ImGui::Separator();
                ImGui::TextUnformatted("Households");
                for (const auto& household : households) {
                    if (household.settlement_id != settlement->id) {
                        continue;
                    }
                    if (ImGui::SmallButton(("Household " + std::to_string(household.id)).c_str())) {
                        ui.selected_household = household.id;
                    }
                }
            }
        }

        ImGui::NextColumn();
        ImGui::Text("Household Inspector");
        if (ui.selected_household.has_value()) {
            const auto* household = find_household(households, *ui.selected_household);
            if (household != nullptr) {
                ImGui::Text("ID: %llu", static_cast<unsigned long long>(household->id));
                ImGui::Text("Settlement: %llu", static_cast<unsigned long long>(household->settlement_id));
                ImGui::Text("Food inventory: %lld", static_cast<long long>(household->resources.food_inventory.value));
                ImGui::Text("Produced: %lld", static_cast<long long>(household->resources.food_produced_total));
                ImGui::Text("Consumed: %lld", static_cast<long long>(household->resources.food_consumed_total));
                ImGui::Text("Unmet: %lld", static_cast<long long>(household->resources.unmet_food_total));
                ImGui::Text("Stress: %lld", static_cast<long long>(household->resources.nutritional_stress_points));
                ImGui::Text("Shortage streak: %u", household->resources.consecutive_shortage_days);
                ImGui::Separator();
                ImGui::TextUnformatted("Members");
                for (const auto member_id : household->member_ids) {
                    if (ImGui::SmallButton(("Person " + std::to_string(member_id)).c_str())) {
                        ui.selected_person = member_id;
                    }
                }
            }
        }

        ImGui::NextColumn();
        ImGui::Text("Person Inspector");
        if (ui.selected_person.has_value()) {
            const auto* person = find_person(people, *ui.selected_person);
            if (person != nullptr) {
                ImGui::Text("ID: %llu", static_cast<unsigned long long>(person->id));
                ImGui::Text("Alive: %s", person->alive ? "yes" : "no");
                ImGui::Text("Age: %u", person_age_years(summary, *person));
                ImGui::Text("Birth year: %lld", static_cast<long long>(person->birth_year));
                ImGui::Text("Sex: %s", person->sex == BiologicalSex::Female ? "Female" : "Male");
                ImGui::Text("Household: %llu", static_cast<unsigned long long>(person->household_id));
                ImGui::Text("Nutrition: %d", person->nutrition_points);
                ImGui::Text("Health: %d", person->health_points);
                ImGui::Text("Undernourished streak: %u", person->consecutive_undernourished_days);
                const auto age = person_age_years(summary, *person);
                const auto labour_capacity = universe::person_labour_capacity_units(
                    person->alive,
                    age,
                    person->nutrition_points,
                    person->health_points,
                    universe::FoodModelParameters{},
                    100U);
                const bool occupation_eligible =
                    person->alive &&
                    age >= 18U &&
                    person->nutrition_points >= 160 &&
                    person->health_points >= 160 &&
                    labour_capacity > 0;
                ImGui::Text("Occupation: %s", occupation_label(person->occupation));
                ImGui::Text("Current labour capacity: %lld", static_cast<long long>(labour_capacity));
                ImGui::Text("Occupation eligible: %s", occupation_eligible ? "yes" : "no");

                if (person->mother_id.has_value()) {
                    if (ImGui::SmallButton(("Go mother " + std::to_string(*person->mother_id)).c_str())) {
                        ui.selected_person = *person->mother_id;
                    }
                }
                if (person->father_id.has_value()) {
                    if (ImGui::SmallButton(("Go father " + std::to_string(*person->father_id)).c_str())) {
                        ui.selected_person = *person->father_id;
                    }
                }
                if (ImGui::SmallButton(("Go household " + std::to_string(person->household_id)).c_str())) {
                    ui.selected_household = person->household_id;
                }

                const auto person_memories = controller.memories_for_person(person->id);
                ImGui::Separator();
                ImGui::Text("Memories: %llu", static_cast<unsigned long long>(person_memories.size()));

                if (ImGui::BeginTable("person_memory_table", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 120))) {
                    ImGui::TableSetupColumn("MemoryId");
                    ImGui::TableSetupColumn("AcquiredYear");
                    ImGui::TableSetupColumn("Event");
                    ImGui::TableSetupColumn("EventYear");
                    ImGui::TableSetupColumn("Source");
                    ImGui::TableSetupColumn("Confidence");
                    ImGui::TableHeadersRow();

                    for (const auto& memory : person_memories) {
                        const auto* source_event = find_event(events, memory.source_event_id);
                        const auto acquired_year = universe::DemographicCalendarAdapter::demographic_year(memory.acquired_at);
                        const auto event_year = source_event == nullptr
                            ? 0U
                            : universe::DemographicCalendarAdapter::demographic_year(source_event->timestamp);
                        const auto event_label = source_event == nullptr
                            ? "Unknown"
                            : event_type_label(source_event->type);

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(std::to_string(memory.id).c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%llu (t=%lld)",
                            static_cast<unsigned long long>(acquired_year),
                            static_cast<long long>(memory.acquired_at.ticks));
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%s #%llu",
                            event_label,
                            static_cast<unsigned long long>(memory.source_event_id));
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(std::to_string(event_year).c_str());
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(memory_source_label(memory.source));
                        ImGui::TableSetColumnIndex(5);
                        ImGui::Text("%u/10000", memory.confidence_per_10k);
                    }

                    ImGui::EndTable();
                }

                const auto person_knowledge = controller.knowledge_for_person(person->id);
                ImGui::Separator();
                ImGui::Text("Knowledge: %llu", static_cast<unsigned long long>(person_knowledge.size()));

                if (ImGui::BeginTable("person_knowledge_table", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 150))) {
                    ImGui::TableSetupColumn("KnowledgeId");
                    ImGui::TableSetupColumn("Kind");
                    ImGui::TableSetupColumn("Subject");
                    ImGui::TableSetupColumn("State");
                    ImGui::TableSetupColumn("Source");
                    ImGui::TableSetupColumn("Provenance");
                    ImGui::TableSetupColumn("Memory");
                    ImGui::TableSetupColumn("Event");
                    ImGui::TableSetupColumn("Acquired");
                    ImGui::TableSetupColumn("Updated");
                    ImGui::TableHeadersRow();

                    for (const auto& knowledge : person_knowledge) {
                        const auto fact_kind = knowledge_fact_kind(knowledge.fact);
                        const auto source_memory_it = memory_by_id.find(knowledge.source_memory_id);
                        const auto* source_memory = source_memory_it == memory_by_id.end() ? nullptr : source_memory_it->second;
                        const auto source_event_it =
                            source_memory == nullptr ? event_by_id.end() : event_by_id.find(source_memory->source_event_id);
                        const auto* source_event = source_event_it == event_by_id.end() ? nullptr : source_event_it->second;
                        const auto transmission = controller.transmission_for_memory(knowledge.source_memory_id);

                        std::string subject;
                        std::string state_label;
                        if (const auto* life = std::get_if<universe::PersonLifeStatusKnowledgeFact>(&knowledge.fact); life != nullptr) {
                            subject = "Person #" + std::to_string(life->person_id);
                            state_label = known_life_status_label(life->status);
                        } else if (const auto* household = std::get_if<universe::PersonHouseholdKnowledgeFact>(&knowledge.fact); household != nullptr) {
                            subject = "Person #" + std::to_string(household->person_id);
                            state_label = "Household #" + std::to_string(household->household_id);
                        } else if (const auto* settlement = std::get_if<universe::SettlementStatusKnowledgeFact>(&knowledge.fact); settlement != nullptr) {
                            subject = "Settlement #" + std::to_string(settlement->settlement_id);
                            state_label = "Region #" + std::to_string(settlement->region_id) + (settlement->active ? " Active" : " Inactive");
                        }

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(std::to_string(knowledge.id).c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(knowledge_fact_kind_label(fact_kind));
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(subject.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(state_label.c_str());
                        ImGui::TableSetColumnIndex(4);
                        ImGui::Text("%s %u/10000", knowledge_source_label(knowledge.source), knowledge.confidence_per_10k);
                        ImGui::TableSetColumnIndex(5);
                        if (!transmission.has_value()) {
                            ImGui::TextUnformatted("DirectObservation");
                        } else {
                            const auto method_label = transmission->method == CommunicationMethod::Teaching
                                ? "Taught"
                                : "Told";
                            ImGui::Text("%s by #%llu y%llu",
                                method_label,
                                static_cast<unsigned long long>(transmission->speaker_id),
                                static_cast<unsigned long long>(universe::DemographicCalendarAdapter::demographic_year(transmission->transmitted_at)));
                        }
                        ImGui::TableSetColumnIndex(6);
                        ImGui::Text("#%llu", static_cast<unsigned long long>(knowledge.source_memory_id));
                        ImGui::TableSetColumnIndex(7);
                        if (source_event == nullptr) {
                            ImGui::TextUnformatted("Unknown");
                        } else {
                            ImGui::Text("%s #%llu", event_type_label(source_event->type), static_cast<unsigned long long>(source_event->id));
                        }
                        ImGui::TableSetColumnIndex(8);
                        ImGui::Text("y%llu t=%lld",
                            static_cast<unsigned long long>(universe::DemographicCalendarAdapter::demographic_year(knowledge.acquired_at)),
                            static_cast<long long>(knowledge.acquired_at.ticks));
                        ImGui::TableSetColumnIndex(9);
                        ImGui::Text("y%llu t=%lld",
                            static_cast<unsigned long long>(universe::DemographicCalendarAdapter::demographic_year(knowledge.updated_at)),
                            static_cast<long long>(knowledge.updated_at.ticks));
                    }

                    ImGui::EndTable();
                }
            }
        }

        ImGui::Columns(1);
        ImGui::EndChild();

        ImGui::BeginChild("events", ImVec2(0, 190), true);
        ImGui::Text("Event Log");
        ImGui::Checkbox("Birth", &ui.show_birth_events);
        ImGui::SameLine();
        ImGui::Checkbox("Death", &ui.show_death_events);
        ImGui::SameLine();
        ImGui::Checkbox("SettlementFounded", &ui.show_settlement_founded_events);
        ImGui::SameLine();
        ImGui::Checkbox("SettlementExtinct", &ui.show_settlement_extinct_events);

        if (ImGui::BeginTable("event_table", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 140))) {
            ImGui::TableSetupColumn("EventId");
            ImGui::TableSetupColumn("Year");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Entity");
            ImGui::TableSetupColumn("Household");
            ImGui::TableSetupColumn("Context");
            ImGui::TableHeadersRow();

            for (auto it = events.rbegin(); it != events.rend(); ++it) {
                const auto& event = *it;
                if (event.type == universe::EventType::Birth && !ui.show_birth_events) {
                    continue;
                }
                if (event.type == universe::EventType::Death && !ui.show_death_events) {
                    continue;
                }
                if (event.type == universe::EventType::SettlementFounded && !ui.show_settlement_founded_events) {
                    continue;
                }
                if (event.type == universe::EventType::SettlementExtinct && !ui.show_settlement_extinct_events) {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(std::to_string(event.id).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(std::to_string(universe::DemographicCalendarAdapter::demographic_year(event.timestamp)).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(event_type_label(event.type));

                ImGui::TableSetColumnIndex(3);
                if (event.type == universe::EventType::Birth) {
                    const auto& payload = std::get<universe::BirthEventPayload>(event.payload);
                    if (ImGui::SmallButton(("Person " + std::to_string(payload.child_id) + "##evt" + std::to_string(event.id)).c_str())) {
                        ui.selected_person = payload.child_id;
                    }
                } else if (event.type == universe::EventType::Death) {
                    const auto& payload = std::get<universe::DeathEventPayload>(event.payload);
                    if (ImGui::SmallButton(("Person " + std::to_string(payload.person_id) + "##evt" + std::to_string(event.id)).c_str())) {
                        ui.selected_person = payload.person_id;
                    }
                } else if (event.type == universe::EventType::SettlementFounded) {
                    const auto& payload = std::get<universe::SettlementFoundedEventPayload>(event.payload);
                    if (ImGui::SmallButton(("S " + std::to_string(payload.settlement_id) + "##evt" + std::to_string(event.id)).c_str())) {
                        ui.selected_settlement = payload.settlement_id;
                    }
                } else if (event.type == universe::EventType::SettlementExtinct) {
                    const auto& payload = std::get<universe::SettlementExtinctEventPayload>(event.payload);
                    if (ImGui::SmallButton(("S " + std::to_string(payload.settlement_id) + "##evt" + std::to_string(event.id)).c_str())) {
                        ui.selected_settlement = payload.settlement_id;
                    }
                }

                ImGui::TableSetColumnIndex(4);
                if (event.type == universe::EventType::Birth) {
                    const auto& payload = std::get<universe::BirthEventPayload>(event.payload);
                    if (ImGui::SmallButton(("H " + std::to_string(payload.household_id) + "##evt" + std::to_string(event.id)).c_str())) {
                        ui.selected_household = payload.household_id;
                    }
                } else if (event.type == universe::EventType::Death) {
                    const auto& payload = std::get<universe::DeathEventPayload>(event.payload);
                    if (ImGui::SmallButton(("H " + std::to_string(payload.household_id) + "##evt" + std::to_string(event.id)).c_str())) {
                        ui.selected_household = payload.household_id;
                    }
                }

                ImGui::TableSetColumnIndex(5);
                if (event.type == universe::EventType::Birth) {
                    const auto& payload = std::get<universe::BirthEventPayload>(event.payload);
                    if (payload.father_id.has_value()) {
                        ImGui::TextUnformatted(("father=" + std::to_string(*payload.father_id)).c_str());
                    }
                } else if (event.type == universe::EventType::SettlementFounded) {
                    const auto& payload = std::get<universe::SettlementFoundedEventPayload>(event.payload);
                    std::string context = "region=" + std::to_string(payload.region_id);
                    if (payload.source_settlement_id.has_value()) {
                        context += " source=" + std::to_string(*payload.source_settlement_id);
                    }
                    ImGui::TextUnformatted(context.c_str());
                } else if (event.type == universe::EventType::SettlementExtinct) {
                    const auto& payload = std::get<universe::SettlementExtinctEventPayload>(event.payload);
                    ImGui::TextUnformatted(("region=" + std::to_string(payload.region_id)).c_str());
                }
            }

            ImGui::EndTable();
        }

        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColorFloat(renderer, 0.06f, 0.07f, 0.10f, 1.0f);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
