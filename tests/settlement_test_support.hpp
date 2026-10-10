// Helpers shared by the settlement structure tests (ADR 0002). All checks run at
// the headless Simulation seam: they read state through the public API only.
#pragma once

#include "universe/demo_scenario.hpp"
#include "universe/simulation.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace settlement_test {

inline constexpr std::uint64_t kStartingPopulation = 200U;
// One region holding as much land as the default four regions together, so a
// single-settlement start is not starved of land.
inline constexpr std::int64_t kSingleRegionLandUnits = universe::kSingleSettlementRegionLandUnits;
// Access share is a fraction of a region's land, in points out of this many.
inline constexpr std::uint32_t kAccessShareScale = 10'000;

inline void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

inline universe::SimulationConfig default_layout_config(const std::uint64_t seed) {
    return universe::SimulationConfig{
        .seed = seed,
        .initial_population = kStartingPopulation,
        .enable_unrelated_random_probe = false,
        .initial_household_food_units = 80,
        .food_production_scale_percent = 100,
        .food_system_enabled = true,
    };
}

inline universe::SimulationConfig single_settlement_config(const std::uint64_t seed) {
    auto config = default_layout_config(seed);
    config.initial_region_count = 1;
    config.initial_settlements_per_region = 1;
    config.base_region_area_units = kSingleRegionLandUnits;
    return config;
}

// Every unit of coin in the world: settlement commons plus households, active or
// not. Coin is created once at world creation and only moves (ADR 0003).
inline std::int64_t total_coin(const universe::WorldState& state) {
    std::int64_t total = 0;
    for (const auto& settlement : state.settlements) {
        total += settlement.coin;
    }
    for (const auto& household : state.households) {
        total += household.coin;
    }
    return total;
}

inline std::vector<const universe::Settlement*> active_settlements(const universe::WorldState& state) {
    std::vector<const universe::Settlement*> active;
    for (const auto& settlement : state.settlements) {
        if (settlement.active) {
            active.push_back(&settlement);
        }
    }
    return active;
}

// Whether another settlement could absorb this one under ADR 0002: same region,
// viable, holding its viability streak, and within the merge distance.
inline bool has_absorbing_neighbour(const universe::WorldState& state, const universe::Settlement& settlement) {
    for (const auto& candidate : state.settlements) {
        if (!candidate.active || candidate.id == settlement.id || candidate.region_id != settlement.region_id) {
            continue;
        }
        if (candidate.population < universe::kViableSettlementSize ||
            candidate.economic_viability_streak < universe::kSurvivorViabilityStreakDays) {
            continue;
        }
        const auto dx = candidate.position_x - settlement.position_x;
        const auto dz = candidate.position_z - settlement.position_z;
        if ((dx * dx) + (dz * dz) <= universe::kSettlementMergeDistanceUnits * universe::kSettlementMergeDistanceUnits) {
            return true;
        }
    }
    return false;
}

// An active household (one with living members) belongs to exactly one active
// settlement in its region. Households emptied by extinction are not active.
inline void require_every_household_in_one_valid_settlement(const universe::WorldState& state) {
    for (const auto& household : state.households) {
        if (household.member_ids.empty()) {
            continue;
        }
        std::uint64_t matches = 0;
        for (const auto& settlement : state.settlements) {
            if (settlement.id == household.settlement_id && settlement.active && settlement.region_id == household.region_id) {
                ++matches;
            }
        }
        require(matches == 1U, "every household must belong to exactly one active settlement in its region");
    }
}

// Nobody is lost or counted twice by a split or absorption: the people held by
// settlements add up to the people alive.
inline void require_population_accounted_for(const universe::Simulation& sim) {
    std::uint64_t held_by_settlements = 0;
    for (const auto& settlement : sim.state().settlements) {
        held_by_settlements += settlement.population;
    }
    require(
        held_by_settlements == sim.detailed_population() + sim.aggregate_population(),
        "the people held by settlements must add up to the people alive");
}

// A region's land is finite: what its active settlements hold never exceeds it,
// however many settlements split or merge.
inline void require_region_access_within_capacity(const universe::WorldState& state) {
    for (const auto& region : state.regions) {
        std::uint64_t held = 0;
        for (const auto& settlement : state.settlements) {
            if (settlement.active && settlement.region_id == region.id) {
                held += settlement.access_share_points;
            }
        }
        require(held <= kAccessShareScale, "a region's settlements must never hold more than all of its land");
    }
}

}  // namespace settlement_test
