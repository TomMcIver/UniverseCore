// Acceptance tests for the settlement size distribution (ADR 0002), run over many
// seeds. They are slow, so they live apart from the fast structure tests.
#include "settlement_test_support.hpp"

#include "universe/simulation.hpp"
#include "universe/time.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>

namespace {

using namespace settlement_test;

constexpr std::uint64_t kDistributionSeedCount = 64;
constexpr int kDistributionYears = 20;
constexpr std::size_t kTargetMinSettlements = 2;
constexpr std::size_t kTargetMaxSettlements = 5;
constexpr std::uint64_t kTargetMinLargestSettlement = 70;
// The single-settlement start puts all 200 people on one region. Its land class
// is drawn from the seed, and only arable land can feed them (ADR 0002, measured
// distribution): on 31 of 31 arable seeds the target below is met, while marginal
// and wetland regions starve the start before any split can help. The strict
// target is therefore asserted on every arable seed, and this floor stops the
// filter from emptying the test.
constexpr std::size_t kMinArableSeeds = 25;

bool is_arable_start(const universe::Simulation& sim) {
    return sim.state().regions.front().environment_class == universe::EnvironmentClass::ProductiveArable;
}

// Runs the scenario a year at a time and checks, at every year boundary, that no
// settlement has been below the viable size for more than five years while a
// neighbour could have absorbed it. ADR 0002 absorbs only into a viable neighbour
// in range, so a lone survivor or an isolated hamlet legitimately stays.
void run_scenario_checking_every_year(universe::Simulation& sim) {
    const auto starting_coin = total_coin(sim.state());
    for (int year = 0; year < kDistributionYears; ++year) {
        sim.run_years(1);
        require(total_coin(sim.state()) == starting_coin, "total coin must never change as settlements split and merge");
        for (const auto* settlement : active_settlements(sim.state())) {
            require(
                settlement->days_below_viable_size <= universe::kAbsorptionBelowViableDays ||
                    !has_absorbing_neighbour(sim.state(), *settlement),
                "a settlement below the viable size for over five years must have no neighbour able to absorb it");
        }
    }
}

void test_single_settlement_start_grows_into_a_village_and_hamlets() {
    std::size_t arable_seeds = 0;
    for (std::uint64_t seed = 1; seed <= kDistributionSeedCount; ++seed) {
        universe::Simulation sim{single_settlement_config(seed)};
        const bool is_arable = is_arable_start(sim);
        run_scenario_checking_every_year(sim);

        require_every_household_in_one_valid_settlement(sim.state());
        require_population_accounted_for(sim);
        require_region_access_within_capacity(sim.state());

        if (!is_arable) {
            continue;
        }
        ++arable_seeds;
        const auto active = active_settlements(sim.state());
        std::uint64_t largest = 0;
        for (const auto* settlement : active) {
            largest = std::max(largest, settlement->population);
        }
        require(
            active.size() >= kTargetMinSettlements && active.size() <= kTargetMaxSettlements,
            "an arable start must end with two to five settlements");
        require(largest >= kTargetMinLargestSettlement, "an arable start must hold a village of at least seventy people");
    }
    require(arable_seeds >= kMinArableSeeds, "enough seeds must draw arable land for the target to mean something");
}

// The default layout starts with several settlements and must not splinter
// further: nothing in it is large enough to split within twenty years.
void test_default_layout_does_not_splinter_further() {
    for (std::uint64_t seed = 1; seed <= kDistributionSeedCount; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        const auto starting = active_settlements(sim.state()).size();
        run_scenario_checking_every_year(sim);
        require(
            active_settlements(sim.state()).size() <= starting,
            "the default layout must not end with more settlements than it started with");
    }
}

// Determinism, equivalent time-advance schedules and save/load continuation over
// the scenario. The first seeds cover arable, woodland and marginal starts.
void test_scenario_is_deterministic_schedule_independent_and_resumable() {
    constexpr std::uint64_t kSeedsChecked = 8;
    constexpr std::int64_t kChunkDays = 73;  // 100 chunks make twenty 365-day years
    constexpr std::int64_t kChunks = (kDistributionYears * universe::DemographicCalendarAdapter::kDaysPerYear) / kChunkDays;
    static_assert(kChunks * kChunkDays == kDistributionYears * universe::DemographicCalendarAdapter::kDaysPerYear);
    constexpr int kResumeAfterYears = 10;

    for (std::uint64_t seed = 1; seed <= kSeedsChecked; ++seed) {
        universe::Simulation first{single_settlement_config(seed)};
        universe::Simulation second{single_settlement_config(seed)};
        first.run_years(kDistributionYears);
        second.run_years(kDistributionYears);
        const auto reference = first.save_snapshot();
        require(reference == second.save_snapshot(), "the same seed must reproduce the same world");

        universe::Simulation chunked{single_settlement_config(seed)};
        for (std::int64_t chunk = 0; chunk < kChunks; ++chunk) {
            chunked.advance(universe::days(kChunkDays));
        }
        require(reference == chunked.save_snapshot(), "equivalent time-advance schedules must give the same world");

        universe::Simulation halfway{single_settlement_config(seed)};
        halfway.run_years(kResumeAfterYears);
        universe::Simulation resumed{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        require(universe::Simulation::load_snapshot(halfway.save_snapshot(), resumed).ok, "a mid-scenario snapshot must load");
        resumed.run_years(kDistributionYears - kResumeAfterYears);
        require(reference == resumed.save_snapshot(), "a reloaded scenario must continue to the same world");
    }
}

}  // namespace

int main() {
    test_single_settlement_start_grows_into_a_village_and_hamlets();
    test_default_layout_does_not_splinter_further();
    test_scenario_is_deterministic_schedule_independent_and_resumable();
    std::cout << "settlement distribution tests passed\n";
    return 0;
}
