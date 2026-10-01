// Regression tests for issue #60: a region merged into population groups
// (demotion) and split back out (promotion) must preserve the world's
// consequential state, and the result must continue deterministically.
#include "universe/simulation.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

constexpr std::uint64_t kSeed = 42U;
constexpr std::uint64_t kWarmupYears = 5U;
// A demote/promote cycle must not cost more than normal year-to-year noise.
constexpr std::uint64_t kMaxCycleLossPercent = 10U;

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

universe::SimulationConfig demo_config() {
    return universe::SimulationConfig{
        .seed = kSeed,
        .initial_population = 200,
        .enable_unrelated_random_probe = false,
        .initial_household_food_units = 80,
        .food_production_scale_percent = 100,
        .food_system_enabled = true,
    };
}

std::vector<universe::RegionId> region_ids(const universe::Simulation& sim) {
    std::vector<universe::RegionId> ids;
    for (const auto& region : sim.state().regions) {
        ids.push_back(region.id);
    }
    return ids;
}

void cycle_every_region(universe::Simulation& sim) {
    const auto ids = region_ids(sim);
    for (const auto id : ids) {
        sim.demote_region_to_settlement_aggregate(id);
    }
    for (const auto id : ids) {
        sim.promote_region_to_detailed(id);
    }
}

std::uint64_t workers_in_food_occupations(const universe::Simulation& sim) {
    std::uint64_t count = 0;
    for (const auto& person : sim.state().people) {
        if (person.alive &&
            (person.occupation == universe::Occupation::SubsistenceCultivator ||
             person.occupation == universe::Occupation::WildFoodGatherer)) {
            ++count;
        }
    }
    return count;
}

std::uint64_t living_population(const universe::Simulation& sim) {
    return sim.detailed_population() + sim.aggregate_population();
}

void test_promotion_restores_working_occupations() {
    universe::Simulation sim{demo_config()};
    sim.run_years(kWarmupYears);
    const auto workers_before = workers_in_food_occupations(sim);
    require(workers_before > 0U, "warmed-up world must have food workers to preserve");

    cycle_every_region(sim);

    require(
        workers_in_food_occupations(sim) == workers_before,
        "a demote/promote cycle must restore the food workers it folded into population groups");
}

void test_demote_promote_cycle_does_not_starve_the_population() {
    universe::Simulation control{demo_config()};
    control.run_years(kWarmupYears);
    control.run_years(1);
    const auto control_population = living_population(control);

    universe::Simulation cycled{demo_config()};
    cycled.run_years(kWarmupYears);
    const auto before_cycle = living_population(cycled);
    cycle_every_region(cycled);
    require(living_population(cycled) == before_cycle, "cycle itself must conserve population");
    cycled.run_years(1);

    const auto floor = control_population - (control_population * kMaxCycleLossPercent) / 100U;
    require(
        living_population(cycled) >= floor,
        "population one year after a demote/promote cycle must stay near the uncycled run");
}

void test_save_load_continuation_matches_after_cycle() {
    universe::Simulation sim{demo_config()};
    sim.run_years(kWarmupYears);
    cycle_every_region(sim);

    const auto saved = sim.save_snapshot();
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(saved, restored).ok, "post-cycle snapshot must load");

    sim.run_years(1);
    restored.run_years(1);
    require(
        sim.save_snapshot() == restored.save_snapshot(),
        "save/load continuation after a demote/promote cycle must match the uninterrupted run");
}

}  // namespace

int main() {
    test_promotion_restores_working_occupations();
    test_demote_promote_cycle_does_not_starve_the_population();
    test_save_load_continuation_matches_after_cycle();
    std::cout << "lod round-trip tests passed\n";
    return 0;
}
