#include "universe/simulation.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

constexpr std::uint64_t kDemoPopulation = 200;
constexpr std::uint64_t kDemoYears = 20;
constexpr std::uint64_t kMinimumSurvivingPopulationPercent = 90;

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

universe::SimulationConfig food_secure_config(const std::uint64_t seed) {
    return universe::SimulationConfig{
        .seed = seed,
        .initial_population = kDemoPopulation,
    };
}

std::uint64_t completed_buildings(const universe::WorldState& state) {
    std::uint64_t count = 0;
    for (const auto& building : state.buildings) {
        if (building.state == universe::ConstructionState::Complete) {
            ++count;
        }
    }
    return count;
}

std::uint64_t alive_people(const universe::WorldState& state) {
    std::uint64_t count = 0;
    for (const auto& person : state.people) {
        if (person.alive) {
            ++count;
        }
    }
    return count;
}

void food_secure_world_completes_buildings_within_twenty_years() {
    universe::Simulation simulation{food_secure_config(1)};
    const auto initial_population = alive_people(simulation.state());

    simulation.run_years(kDemoYears);

    const auto& state = simulation.state();
    require(completed_buildings(state) > 0U,
            "a food-secure 200-person world must complete at least one building within 20 years");
    require(alive_people(state) * 100U >= initial_population * kMinimumSurvivingPopulationPercent,
            "construction must not collapse the population");
}

std::int64_t total_region_wood(const universe::WorldState& state) {
    std::int64_t total = 0;
    for (const auto& region : state.regions) {
        total += region.wood_stock;
    }
    return total;
}

void construction_materials_never_exceed_regional_stock() {
    universe::Simulation simulation{food_secure_config(3)};
    for (std::uint64_t year = 0; year < kDemoYears; ++year) {
        simulation.run_years(1);
        const auto& state = simulation.state();
        for (const auto& region : state.regions) {
            require(region.wood_stock >= 0, "region wood stock must never go negative");
            require(region.stone_remaining >= 0, "region stone must never go negative");
        }
        for (const auto& settlement : state.settlements) {
            require(settlement.materials.wood.value >= 0 && settlement.materials.timber.value >= 0 &&
                        settlement.materials.stone.value >= 0,
                    "settlement material inventories must never go negative");
        }
    }
    require(total_region_wood(simulation.state()) >= 0, "regional wood must be conserved");
}

void construction_is_deterministic_across_equivalent_schedules() {
    universe::Simulation one_run{food_secure_config(7)};
    universe::Simulation stepped_run{food_secure_config(7)};
    one_run.run_years(10);
    for (int year = 0; year < 10; ++year) {
        stepped_run.run_years(1);
    }

    const auto& lhs = one_run.state().buildings;
    const auto& rhs = stepped_run.state().buildings;
    require(lhs.size() == rhs.size(), "equivalent schedules must plan the same buildings");
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        require(lhs[index].id == rhs[index].id && lhs[index].state == rhs[index].state &&
                    lhs[index].labour_completed == rhs[index].labour_completed &&
                    lhs[index].timber_committed == rhs[index].timber_committed &&
                    lhs[index].stone_committed == rhs[index].stone_committed,
                "equivalent schedules must produce identical construction progress");
    }
}

}  // namespace

int main() {
    food_secure_world_completes_buildings_within_twenty_years();
    construction_materials_never_exceed_regional_stock();
    construction_is_deterministic_across_equivalent_schedules();
    std::cout << "construction tests passed\n";
    return 0;
}
