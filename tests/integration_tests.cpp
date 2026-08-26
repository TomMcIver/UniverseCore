#include "universe/simulation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

const universe::Person* find_person(const universe::WorldState& state, const universe::PersonId id) {
    for (const auto& person : state.people) {
        if (person.id == id) {
            return &person;
        }
    }
    return nullptr;
}

const universe::SimulationEvent* find_event(const universe::WorldState& state, const universe::EventId id) {
    for (const auto& event : state.event_log) {
        if (event.id == id) {
            return &event;
        }
    }
    return nullptr;
}

void verify_resource_invariants(const universe::WorldState& state, const std::int64_t expected_initial_food_units) {
    std::int64_t total_food_stored = 0;
    std::int64_t total_food_produced = 0;
    std::int64_t total_food_consumed = 0;
    std::int64_t total_unmet_food = 0;
    std::uint64_t households_in_shortage = 0;

    for (const auto& household : state.households) {
        require(household.resources.food_inventory.value >= 0, "household food inventory must never be negative");
        require(household.resources.food_produced_total >= 0, "household food produced total must never be negative");
        require(household.resources.food_consumed_total >= 0, "household food consumed total must never be negative");
        require(household.resources.unmet_food_total >= 0, "household unmet food total must never be negative");

        total_food_stored += household.resources.food_inventory.value;
        total_food_produced += household.resources.food_produced_total;
        total_food_consumed += household.resources.food_consumed_total;
        total_unmet_food += household.resources.unmet_food_total;
        if (household.resources.consecutive_shortage_days > 0U) {
            ++households_in_shortage;
        }
    }

    const auto expected_initial = state.households.empty()
        ? 0
        : static_cast<std::int64_t>(state.households.size()) * expected_initial_food_units;

    require(total_food_stored == state.total_food_stored, "world total_food_stored must match household aggregation");
    require(total_food_produced == state.total_food_produced, "world total_food_produced must match household aggregation");
    require(total_food_consumed == state.total_food_consumed, "world total_food_consumed must match household aggregation");
    require(total_unmet_food == state.total_unmet_food, "world total_unmet_food must match household aggregation");
    require(households_in_shortage == state.households_in_shortage, "households_in_shortage must match household shortage flags");

    const auto inferred_initial_food = state.total_food_stored + state.total_food_consumed - state.total_food_produced;
    require(inferred_initial_food == expected_initial, "food accounting conservation must hold");
}

void verify_memory_invariants(const universe::WorldState& state, const universe::Simulation& simulation) {
    std::unordered_set<universe::MemoryId> ids;
    std::unordered_map<universe::PersonId, std::unordered_set<universe::EventId>> direct_by_owner;

    for (const auto& memory : state.agent_memories) {
        require(memory.id > 0U, "memory ids must be non-zero");
        require(memory.owner_id > 0U, "memory owner ids must be non-zero");
        require(memory.source_event_id > 0U, "memory source event ids must be non-zero");
        require(memory.source == universe::MemorySource::DirectObservation, "M2 memory sources must be direct observation");
        require(memory.confidence_per_10k <= 10'000U, "memory confidence must be bounded");
        require(memory.confidence_per_10k == 10'000U, "direct observation confidence starts at full confidence");
        require(ids.insert(memory.id).second, "memory ids must be unique");
        require(
            direct_by_owner[memory.owner_id].insert(memory.source_event_id).second,
            "owner+event direct memory must be unique");

        const auto* owner = find_person(state, memory.owner_id);
        require(owner != nullptr, "memory owner must reference existing person");

        const auto* source_event = find_event(state, memory.source_event_id);
        require(source_event != nullptr, "memory source event must reference existing event");
        require(source_event->timestamp == memory.acquired_at, "direct memory acquired_at must match source event timestamp");

        require(
            simulation.person_remembers_event(memory.owner_id, memory.source_event_id),
            "query path person_remembers_event must match canonical memory state");
    }
}

void verify_event_payload_types(const universe::WorldState& state) {
    for (const auto& event : state.event_log) {
        if (event.type == universe::EventType::Birth) {
            require(std::holds_alternative<universe::BirthEventPayload>(event.payload), "birth payload must match event type");
            const auto& payload = std::get<universe::BirthEventPayload>(event.payload);
            require(find_person(state, payload.child_id) != nullptr, "birth child id must exist");
            require(find_person(state, payload.mother_id) != nullptr, "birth mother id must exist");
        } else if (event.type == universe::EventType::Death) {
            require(std::holds_alternative<universe::DeathEventPayload>(event.payload), "death payload must match event type");
        } else if (event.type == universe::EventType::SettlementFounded) {
            require(std::holds_alternative<universe::SettlementFoundedEventPayload>(event.payload), "founded payload must match event type");
        } else if (event.type == universe::EventType::SettlementExtinct) {
            require(std::holds_alternative<universe::SettlementExtinctEventPayload>(event.payload), "extinct payload must match event type");
        }
    }
}

}  // namespace

int main() {
    const universe::SimulationConfig config{
        .seed = 42,
        .initial_population = 200,
        .enable_unrelated_random_probe = false,
        .initial_household_food_units = 80,
        .food_production_scale_percent = 100,
        .food_system_enabled = true,
    };

    {
        universe::Simulation continuous{config};
        continuous.run_years(500);
        const auto continuous_bytes = continuous.save_snapshot();

        universe::Simulation with_boundary{config};
        with_boundary.run_years(250);
        const auto mid_bytes = with_boundary.save_snapshot();

        universe::Simulation resumed{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        const auto load_result = universe::Simulation::load_snapshot(mid_bytes, resumed);
        require(load_result.ok, "midpoint snapshot must load");
        resumed.run_years(250);

        require(
            continuous_bytes == resumed.save_snapshot(),
            "500 years continuous and 250+save/load+250 must serialize identically");
    }

    {
        universe::Simulation deterministic_a{config};
        universe::Simulation deterministic_b{config};
        deterministic_a.run_years(120);
        deterministic_b.run_years(120);
        require(
            deterministic_a.save_snapshot() == deterministic_b.save_snapshot(),
            "same seed 120-year integration run must be deterministic");

        verify_resource_invariants(deterministic_a.state(), config.initial_household_food_units);
        verify_event_payload_types(deterministic_a.state());
        verify_memory_invariants(deterministic_a.state(), deterministic_a);

        std::uint64_t checked_births = 0;
        std::uint64_t checked_deaths = 0;
        std::uint64_t extinction_events = 0;
        for (const auto& event : deterministic_a.state().event_log) {
            if (event.type == universe::EventType::Birth) {
                const auto& payload = std::get<universe::BirthEventPayload>(event.payload);
                require(
                    deterministic_a.person_remembers_event(payload.mother_id, event.id),
                    "mother must remember birth through direct observation");
                require(
                    !deterministic_a.person_remembers_event(payload.child_id, event.id),
                    "newborn must not remember own birth");
                ++checked_births;
            } else if (event.type == universe::EventType::Death) {
                const auto& payload = std::get<universe::DeathEventPayload>(event.payload);
                require(
                    !deterministic_a.person_remembers_event(payload.person_id, event.id),
                    "person must not gain a memory of their own death");
                ++checked_deaths;
            } else if (event.type == universe::EventType::SettlementExtinct) {
                std::uint64_t observer_count = 0;
                for (const auto& memory : deterministic_a.state().agent_memories) {
                    if (memory.source_event_id == event.id) {
                        ++observer_count;
                    }
                }
                require(observer_count == 0U, "settlement extinction event may have zero direct observers");
                ++extinction_events;
            }
        }
        require(checked_births > 0U, "integration run should include birth events");
        require(checked_deaths > 0U, "integration run should include death events");
        require(extinction_events > 0U, "integration run should include settlement extinction events");
    }

    {
        auto founder = config;
        founder.initial_population = 300;
        founder.initial_region_count = 1;
        founder.base_region_area_units = 60;
        founder.initial_household_food_units = 10;
        founder.food_production_scale_percent = 90;

        universe::Simulation founding_run{founder};
        founding_run.run_years(120);

        std::uint64_t founded_events = 0;
        for (const auto& event : founding_run.state().event_log) {
            if (event.type != universe::EventType::SettlementFounded) {
                continue;
            }

            ++founded_events;
            std::uint64_t observer_count = 0;
            for (const auto& memory : founding_run.state().agent_memories) {
                if (memory.source_event_id == event.id) {
                    ++observer_count;
                }
            }

            require(observer_count > 0U, "settlement founded event should have direct observers from moved residents");
            require(
                observer_count < founding_run.state().people.size(),
                "settlement founded event should not automatically be remembered by everyone");
        }

        require(founded_events > 0U, "founding scenario should produce settlement founded events");
    }

    {
        universe::Simulation continuous{config};
        continuous.run_years(120);

        universe::Simulation segmented{config};
        segmented.run_years(40);
        auto boundary_1 = segmented.save_snapshot();
        segmented = universe::Simulation{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        auto load_result = universe::Simulation::load_snapshot(boundary_1, segmented);
        require(load_result.ok, "first repeated snapshot boundary must load");

        segmented.run_years(40);
        auto boundary_2 = segmented.save_snapshot();
        segmented = universe::Simulation{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        load_result = universe::Simulation::load_snapshot(boundary_2, segmented);
        require(load_result.ok, "second repeated snapshot boundary must load");

        segmented.run_years(40);

        require(
            continuous.save_snapshot() == segmented.save_snapshot(),
            "repeated save/load continuation must preserve canonical memory/event state without duplication");
    }

    {
        auto durability_config = config;
        durability_config.initial_population = 280;
        durability_config.initial_region_count = 2;
        durability_config.base_region_area_units = 160;
        durability_config.food_production_scale_percent = 130;
        durability_config.wood_resource_scale_percent = 220;
        durability_config.stone_resource_scale_percent = 220;
        durability_config.material_crafting_scale_percent = 220;

        universe::Simulation durability_run{durability_config};
        durability_run.run_years(30);

        require(
            durability_run.state().total_basic_tool_produced > 0,
            "durability scenario should produce tools");
        require(
            durability_run.state().total_basic_tool_broken > 0,
            "durability scenario should consume and break tools over time");
        require(
            durability_run.state().total_basic_tool_produced >= durability_run.state().total_basic_tool_broken,
            "broken tools should never exceed total produced tools");
        require(
            durability_run.state().average_tool_coverage_per_10k <= 10'000U,
            "tool coverage must remain bounded");
    }

    {
        const std::array<std::uint64_t, 5> baseline_seeds{11U, 22U, 33U, 44U, 55U};
        std::uint64_t survivors_500 = 0;
        std::uint64_t extinctions_500 = 0;
        for (const auto seed : baseline_seeds) {
            universe::Simulation baseline{universe::SimulationConfig{.seed = seed, .initial_population = 200}};
            baseline.run_years(500);
            verify_resource_invariants(baseline.state(), 80);
            if (baseline.state().population > 0) {
                ++survivors_500;
            } else {
                ++extinctions_500;
            }
        }

        require(survivors_500 >= 3U, "baseline 500-year seeds should usually remain viable");
        require(
            survivors_500 + extinctions_500 == baseline_seeds.size(),
            "baseline 500-year seed accounting must remain consistent");
    }

    std::cout << "All UniverseCore integration tests passed.\n";
    return 0;
}
