#include "universe/ui/simulation_controller.hpp"
#include "universe/ui/web_protocol.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

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

bool test_parse_entity_details_command() {
    std::string error;
    const auto command = universe::ui::parse_client_command(
        "{\"type\":\"request_entity_details\",\"entityId\":42}",
        error);
    return command.has_value() &&
        command->type == universe::ui::ClientCommandType::RequestEntityDetails &&
        command->entity_id == 42U;
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

bool test_determinism_with_observer_idle() {
    universe::Simulation baseline{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    universe::Simulation observed{universe::SimulationConfig{.seed = 42, .initial_population = 200}};
    universe::ui::SimulationController observer_controller{universe::SimulationConfig{.seed = 42, .initial_population = 200}};

    for (std::uint32_t day = 0; day < 720U; ++day) {
        baseline.advance(universe::days(1));
        observed.advance(universe::days(1));
        observer_controller.advance_days(1);
        static_cast<void>(universe::ui::build_world_snapshot(observer_controller));
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

}  // namespace

int main() {
    const bool ok =
        test_parse_known_command() &&
        test_reject_unknown_command() &&
        test_reject_bad_speed() &&
        test_parse_entity_details_command() &&
        test_snapshot_is_read_only() &&
        test_snapshot_includes_varying_region_environment_data() &&
        test_protocol_version_in_message() &&
        test_world_delta_buckets_by_id() &&
        test_world_delta_message_json_shape() &&
        test_dead_agent_removed_not_updated() &&
        test_protocol_version_two_on_delta_and_hello_roundtrips() &&
        test_determinism_with_observer_idle();

    if (!ok) {
        std::cerr << "ui protocol tests failed\n";
        return 1;
    }

    return 0;
}
