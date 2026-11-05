// A small slice of the trade calibration sweep (ADR 0003, #76): invariants on a
// few seeds from both layouts, determinism, and the no-trade switch. The full
// 64-seed sweep is the universe_trade_calibration executable, which is too slow
// for a test.
#include "settlement_test_support.hpp"

#include "universe/trade_calibration.hpp"

#include <iostream>

namespace {

using settlement_test::require;
constexpr std::uint64_t kSeeds = 3;
constexpr int kYears = 20;

void test_invariants_hold_on_a_few_seeds_of_both_layouts() {
    for (const auto layout : {universe::CalibrationLayout::SingleSettlement, universe::CalibrationLayout::DefaultLayout}) {
        for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
            const auto m = universe::run_trade_seed(layout, seed, kYears);
            require(m.coin_conserved, "total coin (held and carried) must never change on any day");
            require(m.goods_accounted, "trip cargo, unsold goods and stocks must stay consistent and non-negative");
            require(m.trips_consistent, "each merchant has at most one trip and is alive and a merchant while away");
            require(m.prices_in_range, "every price must stay within the 0.25x-4x range");
            require(universe::trade_invariants_hold(m), "the combined invariant must hold");
            require(
                m.final_merchants <= m.final_settlements * universe::kMaxMerchantsPerSettlement,
                "the merchant bound must hold");
            const auto again = universe::run_trade_seed(layout, seed, kYears);
            require(
                m.final_population == again.final_population && m.merchant_years == again.merchant_years &&
                    m.trips_departed == again.trips_departed && m.realised_profit_completed == again.realised_profit_completed,
                "the same seed must measure the same");
        }
    }
}

void test_trade_off_means_no_merchants_and_no_trips() {
    for (const auto layout : {universe::CalibrationLayout::SingleSettlement, universe::CalibrationLayout::DefaultLayout}) {
        const auto m = universe::run_trade_seed(layout, 1, kYears, false);
        require(m.peak_merchants == 0U && m.trips_departed == 0U, "disabling trade must train no merchant and send no trip");
        require(universe::trade_invariants_hold(m), "the invariants hold without trade too");
    }
}

void test_default_layout_runs_trips() {
    const auto m = universe::run_trade_seed(universe::CalibrationLayout::DefaultLayout, 1, kYears);
    require(m.trips_departed > 0U, "the default layout sends trips");
    require(m.trips_completed + m.trips_in_flight_at_end <= m.trips_departed, "every trip finishes, is in flight or is abandoned");
}

}  // namespace

int main() {
    test_invariants_hold_on_a_few_seeds_of_both_layouts();
    test_trade_off_means_no_merchants_and_no_trips();
    test_default_layout_runs_trips();
    std::cout << "trade calibration tests passed\n";
    return 0;
}
