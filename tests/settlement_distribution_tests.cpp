// Acceptance tests for the settlement size distribution (ADR 0002), run over many
// seeds. They are slow, so they live apart from the fast structure tests.
#include "settlement_test_support.hpp"

#include "universe/simulation.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>

namespace {

using namespace settlement_test;

// Measured for ADR 0002 (single-settlement start, 200 people, 20 years, seeds
// 1-64): 52 seeds end with 2-5 settlements and a largest of 70 or more. Ten
// seeds die out entirely and two fall to about 75 people. That is a property of
// the one-region start, not of fission: with fission switched off the same
// start loses 13 seeds. So the target is asserted for worlds that did not
// collapse, and the number that did not is held above a floor.
constexpr std::uint64_t kDistributionSeedCount = 64;
constexpr int kDistributionYears = 20;
constexpr std::uint64_t kCollapsedBelowPopulation = kStartingPopulation / 2U;
constexpr std::size_t kTargetMinSettlements = 2;
constexpr std::size_t kTargetMaxSettlements = 5;
constexpr std::uint64_t kTargetMinLargestSettlement = 70;
constexpr std::size_t kMinSurvivingSeeds = 40;

void test_single_settlement_start_grows_into_a_village_and_hamlets() {
    std::size_t surviving_seeds = 0;
    for (std::uint64_t seed = 1; seed <= kDistributionSeedCount; ++seed) {
        universe::Simulation sim{single_settlement_config(seed)};
        sim.run_years(kDistributionYears);

        const auto active = active_settlements(sim.state());
        std::uint64_t largest = 0;
        for (const auto* settlement : active) {
            largest = std::max(largest, settlement->population);
            require(
                settlement->days_below_viable_size <= kAbsorptionDays,
                "no settlement may stay below the viable size for more than five years");
        }
        require_every_household_in_one_valid_settlement(sim.state());
        require_population_accounted_for(sim);
        require_region_access_within_capacity(sim.state());

        if (sim.detailed_population() < kCollapsedBelowPopulation) {
            continue;
        }
        ++surviving_seeds;
        require(
            active.size() >= kTargetMinSettlements && active.size() <= kTargetMaxSettlements,
            "a surviving world must end with two to five settlements");
        require(largest >= kTargetMinLargestSettlement, "a surviving world must hold a village of at least seventy people");
    }
    require(surviving_seeds >= kMinSurvivingSeeds, "most seeds must survive the single-settlement start");
}

// The default layout starts with several settlements and must not splinter
// further: nothing in it is large enough to split within twenty years.
void test_default_layout_does_not_splinter_further() {
    constexpr std::uint64_t kSeedsChecked = 16;
    for (std::uint64_t seed = 1; seed <= kSeedsChecked; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        const auto starting = active_settlements(sim.state()).size();
        sim.run_years(kDistributionYears);
        require(
            active_settlements(sim.state()).size() <= starting,
            "the default layout must not end with more settlements than it started with");
    }
}

}  // namespace

int main() {
    test_single_settlement_start_grows_into_a_village_and_hamlets();
    test_default_layout_does_not_splinter_further();
    std::cout << "settlement distribution tests passed\n";
    return 0;
}
