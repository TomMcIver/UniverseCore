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
#include <vector>

namespace {

using universe::BiologicalSex;
using universe::Household;
using universe::HouseholdId;
using universe::Person;
using universe::PersonId;
using universe::SimulationEvent;
using universe::ui::SimulationController;
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

    bool show_birth_events{true};
    bool show_death_events{true};

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

[[nodiscard]] const Person* find_person(const std::span<const Person> people, const PersonId id) {
    for (const auto& person : people) {
        if (person.id == id) {
            return &person;
        }
    }
    return nullptr;
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

        ImGui::Separator();

        const auto households = controller.households();
        const auto people = controller.people();
        const auto events = controller.events();

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
        ImGui::Columns(2);

        ImGui::Text("Household Inspector");
        if (ui.selected_household.has_value()) {
            const auto* household = find_household(households, *ui.selected_household);
            if (household != nullptr) {
                ImGui::Text("ID: %llu", static_cast<unsigned long long>(household->id));
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
            }
        }

        ImGui::Columns(1);
        ImGui::EndChild();

        ImGui::BeginChild("events", ImVec2(0, 190), true);
        ImGui::Text("Event Log");
        ImGui::Checkbox("Birth", &ui.show_birth_events);
        ImGui::SameLine();
        ImGui::Checkbox("Death", &ui.show_death_events);

        if (ImGui::BeginTable("event_table", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 140))) {
            ImGui::TableSetupColumn("EventId");
            ImGui::TableSetupColumn("Year");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Subject");
            ImGui::TableSetupColumn("Household");
            ImGui::TableHeadersRow();

            for (auto it = events.rbegin(); it != events.rend(); ++it) {
                const auto& event = *it;
                const bool is_birth = event.type == universe::EventType::Birth;
                if ((is_birth && !ui.show_birth_events) || (!is_birth && !ui.show_death_events)) {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(std::to_string(event.id).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(std::to_string(universe::DemographicCalendarAdapter::demographic_year(event.timestamp)).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(is_birth ? "Birth" : "Death");
                ImGui::TableSetColumnIndex(3);
                if (ImGui::SmallButton(("Person " + std::to_string(event.subject_person_id) + "##evt" + std::to_string(event.id)).c_str())) {
                    ui.selected_person = event.subject_person_id;
                }
                ImGui::TableSetColumnIndex(4);
                if (event.household_id.has_value()) {
                    if (ImGui::SmallButton(("H " + std::to_string(*event.household_id) + "##evt" + std::to_string(event.id)).c_str())) {
                        ui.selected_household = *event.household_id;
                    }
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
