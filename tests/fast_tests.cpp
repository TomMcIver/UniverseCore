#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"
#include "universe/seed.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

universe::Simulation load_from_snapshot_or_fail(const std::vector<std::byte>& bytes, const char* context) {
    universe::Simulation loaded{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto load_result = universe::Simulation::load_snapshot(bytes, loaded);
    if (!load_result.ok) {
        std::cerr << "Snapshot load failed (" << static_cast<unsigned>(load_result.code) << "): " << load_result.message << '\n';
    }
    require(load_result.ok, context);
    return loaded;
}

const universe::SimulationEvent* find_event_by_id(const universe::WorldState& state, const universe::EventId id) {
    for (const auto& event : state.event_log) {
        if (event.id == id) {
            return &event;
        }
    }
    return nullptr;
}

const universe::Person* find_person_by_id(const universe::WorldState& state, const universe::PersonId id) {
    for (const auto& person : state.people) {
        if (person.id == id) {
            return &person;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    {
        require(universe::seconds(1).ticks == 1, "seconds conversion must be exact");
        require(universe::minutes(1).ticks == 60, "minutes conversion must be exact");
        require(universe::hours(1).ticks == 3600, "hours conversion must be exact");
        require(universe::days(1).ticks == 86400, "days conversion must be exact");

        universe::SimulationTime sum{};
        require(
            universe::checked_add_time(universe::SimulationTime{10}, universe::SimulationDuration{15}, sum),
            "checked_add_time should succeed for in-range additions");
        require(sum.ticks == 25, "checked_add_time must compute exact sums");
    }

    {
        const universe::SimulationSeed root{42};
        const auto a = universe::derive_seed(root, universe::SeedDomain::Person, 7U);
        const auto b = universe::derive_seed(root, universe::SeedDomain::Person, 7U);
        require(a.value == b.value, "same parent/domain/id must derive same seed");

        const auto person_seed = universe::derive_seed(root, universe::SeedDomain::Person, 7U);
        const auto household_seed = universe::derive_seed(root, universe::SeedDomain::Household, 7U);
        require(person_seed.value != household_seed.value, "different domains should derive different seeds for same id");
    }

    const universe::SimulationConfig config{
        .seed = 42,
        .initial_population = 200,
        .enable_unrelated_random_probe = false,
        .initial_household_food_units = 80,
        .food_production_scale_percent = 100,
        .food_system_enabled = true,
    };

    {
        universe::Simulation day_by_day{config};
        for (std::uint64_t i = 0; i < 365U; ++i) {
            day_by_day.advance(universe::days(1));
        }

        universe::Simulation one_chunk{config};
        one_chunk.advance(universe::days(365));
        require(day_by_day.save_snapshot() == one_chunk.save_snapshot(), "365 daily advances must equal one 365-day advance");
    }

    {
        universe::Simulation initial{config};
        initial.run_years(10);
        const auto bytes = initial.save_snapshot();
        auto loaded = load_from_snapshot_or_fail(bytes, "10-year snapshot must load");
        require(initial.save_snapshot() == loaded.save_snapshot(), "10-year snapshot round-trip must preserve state");
        require(bytes == loaded.save_snapshot(), "snapshot bytes must be stable across round-trip");
    }

    {
        universe::Simulation a{config};
        universe::Simulation b{config};
        a.run_years(20);
        b.run_years(20);
        require(a.save_snapshot() == b.save_snapshot(), "same seed must produce identical 20-year state");

        auto different_seed = config;
        different_seed.seed = 43;
        universe::Simulation c{different_seed};
        c.run_years(20);
        require(a.save_snapshot() != c.save_snapshot(), "different seeds should produce different canonical snapshot bytes");
    }

    {
        universe::Simulation events{config};
        events.run_years(30);
        for (const auto& event : events.state().event_log) {
            if (event.type == universe::EventType::Birth) {
                require(std::holds_alternative<universe::BirthEventPayload>(event.payload), "birth payload must match event type");
            } else if (event.type == universe::EventType::Death) {
                require(std::holds_alternative<universe::DeathEventPayload>(event.payload), "death payload must match event type");
            } else if (event.type == universe::EventType::SettlementFounded) {
                require(std::holds_alternative<universe::SettlementFoundedEventPayload>(event.payload), "founded payload must match event type");
            } else if (event.type == universe::EventType::SettlementExtinct) {
                require(std::holds_alternative<universe::SettlementExtinctEventPayload>(event.payload), "extinct payload must match event type");
            }
        }
    }

    {
        universe::Simulation memories{config};
        memories.run_years(40);
        const auto& state = memories.state();

        std::unordered_set<universe::MemoryId> memory_ids;
        std::unordered_map<universe::PersonId, std::unordered_set<universe::EventId>> direct_pairs;
        for (const auto& memory : state.agent_memories) {
            require(memory.id > 0U, "memory id must be non-zero");
            require(memory.owner_id > 0U, "memory owner id must be non-zero");
            require(memory.source_event_id > 0U, "memory event id must be non-zero");
            require(memory.source == universe::MemorySource::DirectObservation, "M2 memory source must be direct observation");
            require(memory.confidence_per_10k <= 10'000U, "memory confidence must be bounded");
            require(memory.confidence_per_10k == 10'000U, "direct observations start at full confidence");
            require(memory_ids.insert(memory.id).second, "memory ids must be unique");

            require(
                direct_pairs[memory.owner_id].insert(memory.source_event_id).second,
                "direct observation memory must be unique per owner/event");

            const auto* owner = find_person_by_id(state, memory.owner_id);
            require(owner != nullptr, "memory owner must reference existing person");
            const auto* event = find_event_by_id(state, memory.source_event_id);
            require(event != nullptr, "memory source event must reference existing event");
            require(event->timestamp == memory.acquired_at, "direct memory must align with source event timestamp");
            require(memories.person_remembers_event(memory.owner_id, memory.source_event_id), "person_remembers_event must match stored memory");
        }

        for (const auto& person : state.people) {
            const auto queried_count = memories.memory_count_for_person(person.id);
            const auto queried_memories = memories.memories_for_person(person.id);
            require(queried_count == queried_memories.size(), "memory_count_for_person must match memories_for_person size");
        }
    }

    {
        universe::Simulation no_omniscience{config};
        no_omniscience.run_years(50);

        const auto& state = no_omniscience.state();
        for (const auto& event : state.event_log) {
            if (event.type != universe::EventType::SettlementExtinct) {
                continue;
            }

            std::uint64_t observers = 0;
            for (const auto& memory : state.agent_memories) {
                if (memory.source_event_id == event.id) {
                    ++observers;
                }
            }
            require(observers == 0U, "settlement extinction may have zero direct observers");
        }
    }

    std::cout << "All UniverseCore fast tests passed.\n";
    return 0;
}
