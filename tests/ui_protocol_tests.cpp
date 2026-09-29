#include "universe/ui/life_history.hpp"
#include "universe/ui/simulation_controller.hpp"
#include "universe/ui/view_models.hpp"
#include "universe/ui/web_protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace {

bool test_parse_known_command() {
    std::string error;
    const auto command = universe::ui::parse_client_command("{\"type\":\"pause\"}", error);
    return command.has_value() && command->type == universe::ui::ClientCommandType::Pause;
}

bool test_reject_unknown_command() {
    std::string error;
    const auto command = universe::ui::parse_client_command("{\"type\":\"nope\"}", error);
    return !command.has_value() && !error.empty();
}

bool test_reject_bad_speed() {
    std::string error;
    const auto command = universe::ui::parse_client_command("{\"type\":\"set_speed\",\"speed\":0}", error);
    return !command.has_value() && error == "Invalid speed";
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool test_parse_request_view_model_command() {
    std::string error;
    const auto command = universe::ui::parse_client_command(
        "{\"type\":\"request_view_model\",\"kind\":\"settlement_economy\",\"entityId\":42}",
        error);
    return command.has_value() &&
        command->type == universe::ui::ClientCommandType::RequestViewModel &&
        command->view_model_kind == universe::ui::ViewModelKind::SettlementEconomy &&
        command->entity_id == 42U;
}

bool test_reject_unknown_view_model_kind() {
    std::string error;
    const auto command = universe::ui::parse_client_command(
        "{\"type\":\"request_view_model\",\"kind\":\"raw_snapshot\",\"entityId\":42}",
        error);
    return !command.has_value() && error == "Unknown view model kind";
}

bool test_reject_view_model_without_entity() {
    std::string error;
    const auto command = universe::ui::parse_client_command(
        "{\"type\":\"request_view_model\",\"kind\":\"person_summary\"}",
        error);
    return !command.has_value() && error == "Invalid entityId";
}

bool test_legacy_entity_details_command_removed() {
    std::string error;
    const auto command = universe::ui::parse_client_command(
        "{\"type\":\"request_entity_details\",\"entityId\":42}",
        error);
    return !command.has_value();
}

bool test_view_model_kind_labels_round_trip() {
    for (const auto kind : {
             universe::ui::ViewModelKind::PersonSummary,
             universe::ui::ViewModelKind::BuildingSummary,
             universe::ui::ViewModelKind::SettlementEconomy,
             universe::ui::ViewModelKind::PersonLifeHistory,
             universe::ui::ViewModelKind::DemoFocus}) {
        const auto parsed = universe::ui::parse_view_model_kind(universe::ui::view_model_kind_label(kind));
        if (!parsed.has_value() || *parsed != kind || universe::ui::view_model_version(kind) == 0U) {
            return false;
        }
    }
    return true;
}

bool test_person_summary_view_model() {
    universe::ui::SimulationController controller{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    controller.advance_days(3);
    const auto& person = controller.people().front();
    const auto view = universe::ui::build_person_summary(controller, person.id);
    if (!view.has_value() || view->id != person.id || view->household_id != person.household_id ||
        view->current_activity != person.current_activity) {
        return false;
    }

    const auto message = universe::ui::make_view_model_message(
        2U, controller, universe::ui::ViewModelKind::PersonSummary, person.id);
    return contains(message, "\"type\":\"view_model\"") &&
        contains(message, "\"kind\":\"person_summary\"") &&
        contains(message, "\"entityId\":" + std::to_string(person.id)) &&
        contains(message, "\"version\":1") &&
        contains(message, "\"data\":{") &&
        contains(message, "\"occupation\":\"") &&
        contains(message, "\"currentActivity\":\"");
}

bool test_building_summary_view_model() {
    universe::ui::SimulationController controller{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    for (std::uint32_t year = 0; year < 10U && controller.buildings().empty(); ++year) {
        controller.advance_years(1);
    }
    if (controller.buildings().empty()) {
        std::cerr << "no buildings after 10 years at seed 42\n";
        return false;
    }

    const auto& building = controller.buildings().front();
    const auto view = universe::ui::build_building_summary(controller, building.id);
    if (!view.has_value() || view->id != building.id || view->settlement_id != building.settlement_id ||
        view->labour_required != building.labour_required) {
        return false;
    }

    const auto message = universe::ui::make_view_model_message(
        2U, controller, universe::ui::ViewModelKind::BuildingSummary, building.id);
    return contains(message, "\"kind\":\"building_summary\"") &&
        contains(message, "\"buildingType\":\"") &&
        contains(message, "\"constructionState\":\"");
}

bool test_settlement_economy_view_model_matches_canonical_state() {
    universe::ui::SimulationController controller{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    controller.advance_years(2);

    const auto summaries = controller.settlement_summaries();
    const auto summary_it = std::find_if(summaries.begin(), summaries.end(), [](const auto& summary) {
        return summary.active && summary.alive_population > 0U;
    });
    if (summary_it == summaries.end()) {
        return false;
    }

    std::int64_t produced = 0;
    std::int64_t consumed = 0;
    for (const auto& household : controller.households()) {
        if (household.settlement_id == summary_it->id) {
            produced += household.resources.food_produced_total;
            consumed += household.resources.food_consumed_total;
        }
    }

    const auto* settlement = [&]() -> const universe::Settlement* {
        for (const auto& candidate : controller.settlements()) {
            if (candidate.id == summary_it->id) {
                return &candidate;
            }
        }
        return nullptr;
    }();

    const auto view = universe::ui::build_settlement_economy(controller, summary_it->id);
    if (!view.has_value() || settlement == nullptr) {
        return false;
    }
    if (view->food_produced_total != produced || view->food_consumed_total != consumed ||
        view->alive_population != summary_it->alive_population || view->tier != settlement->tier ||
        view->cultivators != summary_it->cultivators || view->craft_workers != summary_it->craft_workers ||
        view->unallocated_workers != summary_it->unallocated_workers) {
        return false;
    }
    if (view->min_household_wealth > view->median_household_wealth ||
        view->median_household_wealth > view->max_household_wealth) {
        return false;
    }

    const auto message = universe::ui::make_view_model_message(
        2U, controller, universe::ui::ViewModelKind::SettlementEconomy, summary_it->id);
    return contains(message, "\"kind\":\"settlement_economy\"") &&
        contains(message, "\"tier\":\"") &&
        contains(message, "\"food\":{") &&
        contains(message, "\"jobs\":{") &&
        contains(message, "\"wealth\":{");
}

bool test_view_model_for_missing_entity_is_error() {
    universe::ui::SimulationController controller{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    const auto message = universe::ui::make_view_model_message(
        2U, controller, universe::ui::ViewModelKind::PersonSummary, 999'999'999U);
    return contains(message, "\"type\":\"error\"") && contains(message, "\"code\":\"view_model_not_found\"");
}

bool test_snapshot_is_read_only() {
    universe::ui::SimulationController controller{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    const auto before = controller.summary();
    const auto snapshot_a = universe::ui::build_world_snapshot(controller);
    const auto snapshot_b = universe::ui::build_world_snapshot(controller);
    const auto after = controller.summary();

    if (before.year != after.year || before.population != after.population || before.births != after.births || before.deaths != after.deaths) {
        return false;
    }

    if (snapshot_a.tick != snapshot_b.tick || snapshot_a.population != snapshot_b.population) {
        return false;
    }

    if (snapshot_a.agents.empty() || snapshot_b.agents.empty()) {
        return false;
    }

    return snapshot_a.agents.front().id == snapshot_b.agents.front().id;
}

bool test_snapshot_includes_varying_region_environment_data() {
    // Ticket #18: the Godot client needs real region environment data
    // (not just a bare regionId) to drive terrain that actually differs
    // between regions. This asserts the data both exists and genuinely
    // varies across regions in a real 200-person world, and that it
    // serializes onto the wire with the expected keys.
    universe::ui::SimulationController controller{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    const auto snapshot = universe::ui::build_world_snapshot(controller);

    if (snapshot.regions.empty()) {
        return false;
    }

    bool found_varying_field = false;
    for (std::size_t i = 1; i < snapshot.regions.size(); ++i) {
        if (snapshot.regions[i].environment_class != snapshot.regions[0].environment_class ||
            snapshot.regions[i].wild_food_capacity != snapshot.regions[0].wild_food_capacity) {
            found_varying_field = true;
            break;
        }
    }

    const auto message = universe::ui::make_snapshot_message(1U, snapshot, true);
    const bool has_regions_key = message.find("\"regions\":[") != std::string::npos;
    const bool has_environment_class_key = message.find("\"environmentClass\":\"") != std::string::npos;
    const bool has_wild_food_key = message.find("\"wildFoodCapacity\":") != std::string::npos;

    return found_varying_field && has_regions_key && has_environment_class_key && has_wild_food_key;
}

bool test_protocol_version_in_message() {
    const auto hello = universe::ui::make_hello_message(1U);
    return hello.find("\"protocolVersion\":1") != std::string::npos;
}

bool test_world_delta_buckets_by_id() {
    // Ticket #7: world_delta must be a real diff by stable ID, not the full
    // world restated. Given a known before/after pair, every id must land in
    // exactly one bucket (added/updated/removed) per entity kind.
    universe::ui::WorldSnapshotView before{};
    before.agents = {
        universe::ui::AgentRenderView{.id = 1, .alive = true},
        universe::ui::AgentRenderView{.id = 2, .alive = true},
    };
    before.settlements = {
        universe::ui::SettlementRenderView{.id = 10, .active = true},
    };
    before.buildings = {
        universe::ui::BuildingRenderView{.id = 100},
    };

    universe::ui::WorldSnapshotView after{};
    after.tick = 5;
    after.year = 1;
    after.population = 42;
    after.agents = {
        universe::ui::AgentRenderView{.id = 2, .alive = true, .age_years = 30},
        universe::ui::AgentRenderView{.id = 3, .alive = true},
    };
    after.settlements = {
        universe::ui::SettlementRenderView{.id = 10, .active = true},
        universe::ui::SettlementRenderView{.id = 20, .active = true},
    };

    const auto delta = universe::ui::build_world_delta(after, before);

    if (delta.added_agents.size() != 1U || delta.added_agents.front().id != 3U) {
        return false;
    }
    if (delta.updated_agents.size() != 1U || delta.updated_agents.front().id != 2U) {
        return false;
    }
    if (delta.removed_agent_ids.size() != 1U || delta.removed_agent_ids.front() != 1U) {
        return false;
    }

    if (delta.added_settlements.size() != 1U || delta.added_settlements.front().id != 20U) {
        return false;
    }
    if (delta.updated_settlements.size() != 1U || delta.updated_settlements.front().id != 10U) {
        return false;
    }
    if (!delta.removed_settlement_ids.empty()) {
        return false;
    }

    if (!delta.added_buildings.empty() || !delta.updated_buildings.empty()) {
        return false;
    }
    if (delta.removed_building_ids.size() != 1U || delta.removed_building_ids.front() != 100U) {
        return false;
    }

    return true;
}

bool test_world_delta_message_json_shape() {
    universe::ui::WorldSnapshotView before{};
    before.agents = {universe::ui::AgentRenderView{.id = 1, .alive = true}};

    universe::ui::WorldSnapshotView after{};
    after.tick = 7;

    const auto delta = universe::ui::build_world_delta(after, before);
    const auto message = universe::ui::make_delta_message(2U, delta);

    const bool has_type = message.find("\"type\":\"world_delta\"") != std::string::npos;
    const bool has_version = message.find("\"protocolVersion\":2") != std::string::npos;
    const bool has_removed = message.find("\"removedAgentIds\":[1]") != std::string::npos;
    const bool has_added_key = message.find("\"addedAgents\":[]") != std::string::npos;
    const bool has_updated_key = message.find("\"updatedAgents\":[]") != std::string::npos;

    return has_type && has_version && has_removed && has_added_key && has_updated_key;
}

bool test_dead_agent_removed_not_updated() {
    // A person who dies between two snapshots must appear in removed_agent_ids
    // and never in updated_agents.
    universe::ui::SimulationController controller{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    const auto before = universe::ui::build_world_snapshot(controller);

    universe::PersonId dead_person_id = 0;
    universe::ui::WorldSnapshotView after{};
    for (int day = 0; day < 36500 && dead_person_id == 0; ++day) {
        controller.advance_days(1);
        after = universe::ui::build_world_snapshot(controller);
        for (const auto& current : after.agents) {
            if (!current.alive) {
                dead_person_id = current.id;
                break;
            }
        }
    }
    if (dead_person_id == 0) {
        return false;
    }

    const auto delta = universe::ui::build_world_delta(after, before);
    bool found_in_removed = false;
    for (const auto id : delta.removed_agent_ids) {
        if (id == dead_person_id) {
            found_in_removed = true;
        }
    }
    bool found_in_updated = false;
    for (const auto& agent : delta.updated_agents) {
        if (agent.id == dead_person_id) {
            found_in_updated = true;
        }
    }

    return found_in_removed && !found_in_updated;
}

bool test_protocol_version_two_on_delta_and_hello_roundtrips() {
    const auto hello = universe::ui::make_hello_message(2U);
    if (hello.find("\"protocolVersion\":2") == std::string::npos || hello.find("\"type\":\"hello\"") == std::string::npos) {
        return false;
    }

    const universe::ui::WorldSnapshotView before{};
    const universe::ui::WorldSnapshotView after{};
    const auto delta = universe::ui::build_world_delta(after, before);
    const auto message = universe::ui::make_delta_message(2U, delta);
    return message.find("\"protocolVersion\":2") != std::string::npos;
}

bool test_snapshot_forwards_real_canonical_positions() {
    // Ticket #38: settlement and person positions on the wire must be the
    // real canonical Settlement.position / (Settlement.position +
    // Household.settlement_offset), not presentation-invented placement.
    universe::ui::SimulationController controller{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    const auto snapshot = universe::ui::build_world_snapshot(controller);
    const auto& simulation = controller.simulation();

    if (snapshot.settlements.empty() || snapshot.agents.empty()) {
        return false;
    }

    std::unordered_map<universe::SettlementId, const universe::Settlement*> settlement_by_id;
    for (const auto& settlement : simulation.state().settlements) {
        settlement_by_id.emplace(settlement.id, &settlement);
    }
    for (const auto& rendered : snapshot.settlements) {
        const auto it = settlement_by_id.find(rendered.id);
        if (it == settlement_by_id.end()) {
            return false;
        }
        if (rendered.x != static_cast<float>(it->second->position_x) ||
            rendered.z != static_cast<float>(it->second->position_z)) {
            return false;
        }
    }

    std::unordered_map<universe::HouseholdId, const universe::Household*> household_by_id;
    for (const auto& household : simulation.state().households) {
        household_by_id.emplace(household.id, &household);
    }
    for (const auto& agent : snapshot.agents) {
        const auto household_it = household_by_id.find(agent.household_id);
        if (household_it == household_by_id.end()) {
            return false;
        }
        const auto settlement_it = settlement_by_id.find(household_it->second->settlement_id);
        if (settlement_it == settlement_by_id.end()) {
            return false;
        }
        const auto expected_x = static_cast<float>(settlement_it->second->position_x) +
            static_cast<float>(household_it->second->settlement_offset_x);
        const auto expected_z = static_cast<float>(settlement_it->second->position_z) +
            static_cast<float>(household_it->second->settlement_offset_z);
        if (agent.x != expected_x || agent.z != expected_z) {
            return false;
        }
    }

    return true;
}

bool test_snapshot_message_includes_current_activity() {
    // Ticket #37: current_activity must be forwarded on the wire so the
    // client can label/react to it.
    universe::ui::SimulationController controller{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    const auto snapshot = universe::ui::build_world_snapshot(controller);
    if (snapshot.agents.empty()) {
        return false;
    }
    const auto message = universe::ui::make_snapshot_message(2U, snapshot, true);
    return message.find("\"currentActivity\":\"") != std::string::npos;
}

bool test_determinism_with_observer_idle() {
    universe::Simulation baseline{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    universe::Simulation observed{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    universe::ui::SimulationController observer_controller{universe::SimulationConfig{.seed = 42, .initial_population = 200}};

    for (std::uint32_t day = 0; day < 720U; ++day) {
        baseline.advance(universe::days(1));
        observed.advance(universe::days(1));
        observer_controller.advance_days(1);
        static_cast<void>(universe::ui::build_world_snapshot(observer_controller));
        if ((day % 90U) == 0U && !observer_controller.people().empty()) {
            const auto& person = observer_controller.people().front();
            static_cast<void>(universe::ui::make_view_model_message(
                2U, observer_controller, universe::ui::ViewModelKind::PersonSummary, person.id));
            static_cast<void>(universe::ui::make_view_model_message(
                2U, observer_controller, universe::ui::ViewModelKind::SettlementEconomy, 1U));
        }
    }

    const auto baseline_snapshot = baseline.save_snapshot();
    const auto observed_snapshot = observed.save_snapshot();
    const auto observer_snapshot = observer_controller.simulation().save_snapshot();

    const auto baseline_hash = universe::snapshot_hash64(baseline_snapshot);
    const auto observed_hash = universe::snapshot_hash64(observed_snapshot);
    const auto observer_hash = universe::snapshot_hash64(observer_snapshot);

    std::cout << "UI_DETERMINISM_A_BASELINE_HASH=" << baseline_hash << '\n';
    std::cout << "UI_DETERMINISM_B_CONNECTED_IDLE_HASH=" << observer_hash << '\n';

    return baseline_hash == observed_hash && baseline_hash == observer_hash;
}

// A world a few years in, with births, job changes and knowledge flow to inspect.
const universe::ui::SimulationController& aged_controller() {
    static const universe::ui::SimulationController controller = [] {
        universe::ui::SimulationController built{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
        built.advance_years(4);
        return built;
    }();
    return controller;
}

template <typename Payload>
const universe::SimulationEvent* first_event_with(const universe::ui::SimulationController& controller) {
    for (const auto& event : controller.events()) {
        if (std::holds_alternative<Payload>(event.payload)) {
            return &event;
        }
    }
    return nullptr;
}

bool has_entry(const universe::ui::PersonLifeHistoryView& view, const universe::ui::LifeEntryKind kind, const std::uint64_t source_id) {
    return std::any_of(view.entries.begin(), view.entries.end(), [&](const universe::ui::LifeHistoryEntry& entry) {
        return entry.kind == kind && entry.source_id == source_id && !entry.text.empty();
    });
}

bool test_life_history_records_birth_for_child_and_parents() {
    const auto& controller = aged_controller();
    const auto* event = first_event_with<universe::BirthEventPayload>(controller);
    if (event == nullptr) {
        std::cerr << "no birth in 4 years at seed 42\n";
        return false;
    }
    const auto& birth = std::get<universe::BirthEventPayload>(event->payload);
    const auto child = universe::ui::build_person_life_history(controller, birth.child_id);
    const auto mother = universe::ui::build_person_life_history(controller, birth.mother_id);
    return child.has_value() && mother.has_value() &&
        has_entry(*child, universe::ui::LifeEntryKind::Born, event->id) &&
        has_entry(*mother, universe::ui::LifeEntryKind::HadChild, event->id);
}

bool test_life_history_records_occupation_change() {
    const auto& controller = aged_controller();
    const auto* event = first_event_with<universe::OccupationChangedEventPayload>(controller);
    if (event == nullptr) {
        std::cerr << "no occupation change in 4 years at seed 42\n";
        return false;
    }
    const auto& change = std::get<universe::OccupationChangedEventPayload>(event->payload);
    const auto view = universe::ui::build_person_life_history(controller, change.person_id);
    return view.has_value() && has_entry(*view, universe::ui::LifeEntryKind::OccupationChanged, event->id);
}

// No fabricated history: every event-backed entry must name this person in the log.
bool test_life_history_entries_are_backed_by_records() {
    const auto& controller = aged_controller();
    std::size_t checked = 0;
    for (const auto& person : controller.people()) {
        if (checked == 40U) {
            break;
        }
        const auto view = universe::ui::build_person_life_history(controller, person.id);
        if (!view.has_value()) {
            return false;
        }
        for (std::size_t i = 1; i < view->entries.size(); ++i) {
            if (view->entries[i].at.ticks < view->entries[i - 1].at.ticks) {
                std::cerr << "life history out of order for person " << person.id << '\n';
                return false;
            }
        }
        for (const auto& entry : view->entries) {
            const auto kind = entry.kind;
            if (kind == universe::ui::LifeEntryKind::LearnedFrom || kind == universe::ui::LifeEntryKind::Taught) {
                const auto transmissions = controller.knowledge_transmissions();
                const bool found = std::any_of(transmissions.begin(), transmissions.end(), [&](const universe::KnowledgeTransmission& t) {
                    return t.id == entry.source_id && (t.speaker_id == person.id || t.recipient_id == person.id);
                });
                if (!found) {
                    std::cerr << "unbacked transmission entry for person " << person.id << '\n';
                    return false;
                }
                continue;
            }
            if (kind == universe::ui::LifeEntryKind::AuthoredRecord || kind == universe::ui::LifeEntryKind::ReadRecord) {
                const auto records = controller.knowledge_records();
                if (std::none_of(records.begin(), records.end(), [&](const universe::KnowledgeRecord& r) { return r.id == entry.source_id; })) {
                    std::cerr << "unbacked record entry for person " << person.id << '\n';
                    return false;
                }
                continue;
            }
            const auto events = controller.events();
            if (std::none_of(events.begin(), events.end(), [&](const universe::SimulationEvent& e) { return e.id == entry.source_id; })) {
                std::cerr << "unbacked event entry for person " << person.id << '\n';
                return false;
            }
        }
        ++checked;
    }
    return checked > 0U;
}

// Merging into population groups and splitting back out must never read as a death.
bool test_living_people_have_no_death_entry() {
    const auto& controller = aged_controller();
    for (const auto& person : controller.people()) {
        if (!person.alive) {
            continue;
        }
        const auto view = universe::ui::build_person_life_history(controller, person.id);
        if (!view.has_value()) {
            return false;
        }
        for (const auto& entry : view->entries) {
            if (entry.kind == universe::ui::LifeEntryKind::Died) {
                std::cerr << "living person " << person.id << " has a death entry\n";
                return false;
            }
        }
    }
    return true;
}

bool test_life_history_cap_keeps_newest_entries() {
    const auto& controller = aged_controller();
    for (const auto& person : controller.people()) {
        const auto full = universe::ui::build_person_life_history(controller, person.id);
        if (!full.has_value() || full->entries.size() < 3U) {
            continue;
        }
        const auto capped = universe::ui::build_person_life_history(controller, person.id, 2U);
        return capped.has_value() && capped->entries.size() == 2U &&
            capped->omitted_entries == full->entries.size() - 2U &&
            capped->entries.back().source_id == full->entries.back().source_id;
    }
    std::cerr << "no person with 3+ life entries\n";
    return false;
}

bool test_life_history_view_model_message_shape() {
    const auto& controller = aged_controller();
    const auto* event = first_event_with<universe::BirthEventPayload>(controller);
    if (event == nullptr) {
        return false;
    }
    const auto child_id = std::get<universe::BirthEventPayload>(event->payload).child_id;
    const auto message = universe::ui::make_view_model_message(2U, controller, universe::ui::ViewModelKind::PersonLifeHistory, child_id);
    return contains(message, "\"kind\":\"person_life_history\"") &&
        contains(message, "\"entries\":[") &&
        contains(message, "\"kind\":\"Born\"") &&
        contains(message, "\"text\":\"") &&
        contains(message, "\"omittedEntries\":");
}

std::uint64_t brute_force_life_event_count(const universe::ui::SimulationController& controller, const universe::PersonId id) {
    const auto view = universe::ui::build_person_life_history(controller, id, std::numeric_limits<std::size_t>::max());
    if (!view.has_value()) {
        return 0U;
    }
    return static_cast<std::uint64_t>(std::count_if(view->entries.begin(), view->entries.end(), [](const universe::ui::LifeHistoryEntry& entry) {
        return entry.kind != universe::ui::LifeEntryKind::LearnedFrom && entry.kind != universe::ui::LifeEntryKind::Taught &&
            entry.kind != universe::ui::LifeEntryKind::AuthoredRecord && entry.kind != universe::ui::LifeEntryKind::ReadRecord;
    }));
}

bool test_demo_focus_picks_living_person_with_most_life_events() {
    const auto& controller = aged_controller();
    const auto settlement_id = controller.settlements().front().id;
    const auto focus = universe::ui::build_demo_focus(controller, settlement_id);
    if (!focus.has_value() || !focus->person_id.has_value()) {
        std::cerr << "no demo focus for settlement " << settlement_id << '\n';
        return false;
    }

    std::optional<universe::PersonId> expected{};
    std::uint64_t best = 0;
    for (const auto& person : controller.people()) {
        if (!person.alive) {
            continue;
        }
        const auto households = controller.households();
        const auto household = std::find_if(households.begin(), households.end(), [&](const universe::Household& candidate) {
            return candidate.id == person.household_id;
        });
        if (household == households.end() || household->settlement_id != settlement_id) {
            continue;
        }
        const auto count = brute_force_life_event_count(controller, person.id);
        if (!expected.has_value() || count > best || (count == best && person.id < *expected)) {
            expected = person.id;
            best = count;
        }
    }
    return expected.has_value() && *focus->person_id == *expected && focus->life_event_count == best;
}

bool test_world_event_message_has_readable_summary() {
    const auto& controller = aged_controller();
    const auto* event = first_event_with<universe::BirthEventPayload>(controller);
    if (event == nullptr) {
        return false;
    }
    const auto message = universe::ui::make_world_event_message(2U, controller, *event);
    const auto child_id = std::get<universe::BirthEventPayload>(event->payload).child_id;
    return contains(message, "\"summary\":\"Villager " + std::to_string(child_id) + " was born to Villager ");
}

bool test_parse_run_until_year_command() {
    std::string error;
    const auto command = universe::ui::parse_client_command("{\"type\":\"run_until_year\",\"year\":20}", error);
    std::string rejected_error;
    const auto rejected = universe::ui::parse_client_command("{\"type\":\"run_until_year\",\"year\":0}", rejected_error);
    return command.has_value() && command->type == universe::ui::ClientCommandType::RunUntilYear &&
        command->target_year == 20U && !rejected.has_value();
}

bool test_advance_toward_year_stops_exactly_on_year_boundary() {
    universe::ui::SimulationController chunked{universe::SimulationConfig{.seed = 7, .initial_population = 200}};
    std::uint64_t calls = 0;
    while (chunked.advance_toward_year(2U, 60U) > 0U) {
        ++calls;
    }
    universe::ui::SimulationController whole{universe::SimulationConfig{.seed = 7, .initial_population = 200}};
    whole.advance_years(2);
    return calls > 1U && chunked.summary().year == 2U &&
        chunked.summary().time.ticks == whole.summary().time.ticks &&
        universe::snapshot_hash64(chunked.simulation().save_snapshot()) ==
            universe::snapshot_hash64(whole.simulation().save_snapshot()) &&
        chunked.advance_toward_year(2U, 60U) == 0U;
}

}  // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"parse_known_command", test_parse_known_command},
        {"reject_unknown_command", test_reject_unknown_command},
        {"reject_bad_speed", test_reject_bad_speed},
        {"parse_request_view_model_command", test_parse_request_view_model_command},
        {"reject_unknown_view_model_kind", test_reject_unknown_view_model_kind},
        {"reject_view_model_without_entity", test_reject_view_model_without_entity},
        {"legacy_entity_details_command_removed", test_legacy_entity_details_command_removed},
        {"view_model_kind_labels_round_trip", test_view_model_kind_labels_round_trip},
        {"person_summary_view_model", test_person_summary_view_model},
        {"building_summary_view_model", test_building_summary_view_model},
        {"settlement_economy_view_model_matches_canonical_state", test_settlement_economy_view_model_matches_canonical_state},
        {"view_model_for_missing_entity_is_error", test_view_model_for_missing_entity_is_error},
        {"snapshot_is_read_only", test_snapshot_is_read_only},
        {"snapshot_includes_varying_region_environment_data", test_snapshot_includes_varying_region_environment_data},
        {"protocol_version_in_message", test_protocol_version_in_message},
        {"world_delta_buckets_by_id", test_world_delta_buckets_by_id},
        {"world_delta_message_json_shape", test_world_delta_message_json_shape},
        {"dead_agent_removed_not_updated", test_dead_agent_removed_not_updated},
        {"protocol_version_two_on_delta_and_hello_roundtrips", test_protocol_version_two_on_delta_and_hello_roundtrips},
        {"snapshot_forwards_real_canonical_positions", test_snapshot_forwards_real_canonical_positions},
        {"snapshot_message_includes_current_activity", test_snapshot_message_includes_current_activity},
        {"determinism_with_observer_idle", test_determinism_with_observer_idle},
        {"life_history_records_birth_for_child_and_parents", test_life_history_records_birth_for_child_and_parents},
        {"life_history_records_occupation_change", test_life_history_records_occupation_change},
        {"life_history_entries_are_backed_by_records", test_life_history_entries_are_backed_by_records},
        {"living_people_have_no_death_entry", test_living_people_have_no_death_entry},
        {"life_history_cap_keeps_newest_entries", test_life_history_cap_keeps_newest_entries},
        {"life_history_view_model_message_shape", test_life_history_view_model_message_shape},
        {"demo_focus_picks_living_person_with_most_life_events", test_demo_focus_picks_living_person_with_most_life_events},
        {"world_event_message_has_readable_summary", test_world_event_message_has_readable_summary},
        {"parse_run_until_year_command", test_parse_run_until_year_command},
        {"advance_toward_year_stops_exactly_on_year_boundary", test_advance_toward_year_stops_exactly_on_year_boundary},
    };

    bool ok = true;
    for (const auto& test : tests) {
        if (!test.fn()) {
            std::cerr << "FAILED: " << test.name << '\n';
            ok = false;
        }
    }

    if (!ok) {
        std::cerr << "ui protocol tests failed\n";
        return 1;
    }

    return 0;
}
