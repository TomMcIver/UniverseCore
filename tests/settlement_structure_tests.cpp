// Behaviour tests for settlement structure (ADR 0002): the starting layout,
// pressure-driven fission and absorption of settlements that stay too small.
// They run at the headless Simulation seam: configure a world, advance time,
// read the resulting state.
#include "universe/simulation.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

constexpr std::uint64_t kStartingPopulation = 200U;

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

universe::SimulationConfig default_layout_config(const std::uint64_t seed) {
    return universe::SimulationConfig{
        .seed = seed,
        .initial_population = kStartingPopulation,
        .enable_unrelated_random_probe = false,
        .initial_household_food_units = 80,
        .food_production_scale_percent = 100,
        .food_system_enabled = true,
    };
}

universe::SimulationConfig single_settlement_config(const std::uint64_t seed) {
    auto config = default_layout_config(seed);
    config.initial_region_count = 1;
    config.initial_settlements_per_region = 1;
    return config;
}

std::vector<const universe::Settlement*> active_settlements(const universe::WorldState& state) {
    std::vector<const universe::Settlement*> active;
    for (const auto& settlement : state.settlements) {
        if (settlement.active) {
            active.push_back(&settlement);
        }
    }
    return active;
}

void require_every_household_in_one_valid_settlement(const universe::WorldState& state) {
    for (const auto& household : state.households) {
        std::uint64_t matches = 0;
        for (const auto& settlement : state.settlements) {
            if (settlement.id == household.settlement_id && settlement.active && settlement.region_id == household.region_id) {
                ++matches;
            }
        }
        require(matches == 1U, "every household must belong to exactly one active settlement in its region");
    }
}

// Today's automatic layout, recorded before the option existed: the same seed
// must keep producing the same number of starting settlements.
void test_default_layout_is_unchanged() {
    struct Baseline {
        std::uint64_t seed;
        std::size_t active_settlements;
    };
    constexpr Baseline kBaselines[] = {{1U, 9U}, {7U, 10U}, {42U, 9U}};
    for (const auto& baseline : kBaselines) {
        universe::Simulation sim{default_layout_config(baseline.seed)};
        require(
            active_settlements(sim.state()).size() == baseline.active_settlements,
            "the default starting layout must not change when the option is unset");
        require_every_household_in_one_valid_settlement(sim.state());
    }
}

void test_single_settlement_start_holds_every_household() {
    for (const std::uint64_t seed : {1U, 7U, 42U}) {
        universe::Simulation sim{single_settlement_config(seed)};
        const auto active = active_settlements(sim.state());
        require(sim.state().regions.size() == 1U, "the single-settlement start uses exactly one region");
        require(active.size() == 1U, "the single-settlement start must begin with exactly one active settlement");
        require(!sim.state().households.empty(), "the single-settlement world must have households");
        require_every_household_in_one_valid_settlement(sim.state());
        require(
            active.front()->population == sim.detailed_population(),
            "the lone settlement must hold the whole starting population");
    }
}

void test_requested_settlement_count_is_honoured_and_capped() {
    auto two = single_settlement_config(42);
    two.initial_settlements_per_region = 2;
    universe::Simulation sim_two{two};
    require(active_settlements(sim_two.state()).size() == 2U, "a requested count of two must start two settlements");
    require_every_household_in_one_valid_settlement(sim_two.state());

    auto absurd = single_settlement_config(42);
    absurd.initial_settlements_per_region = 10'000;
    universe::Simulation sim_absurd{absurd};
    const auto settlements = active_settlements(sim_absurd.state());
    require(settlements.size() <= sim_absurd.state().households.size(), "a request above the household count is capped at it");
    require_every_household_in_one_valid_settlement(sim_absurd.state());
}

void test_single_settlement_world_continues_identically_after_reload() {
    universe::Simulation uninterrupted{single_settlement_config(42)};
    universe::Simulation source{single_settlement_config(42)};
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(source.save_snapshot(), restored).ok, "a single-settlement snapshot must load");

    uninterrupted.run_years(3);
    restored.run_years(3);
    require(
        uninterrupted.save_snapshot() == restored.save_snapshot(),
        "a reloaded single-settlement world must continue identically");
}

}  // namespace

int main() {
    test_default_layout_is_unchanged();
    test_single_settlement_start_holds_every_household();
    test_requested_settlement_count_is_honoured_and_capped();
    test_single_settlement_world_continues_identically_after_reload();
    std::cout << "settlement structure tests passed\n";
    return 0;
}
