// A small slice of the trade calibration sweep (ADR 0003, #76): invariants on a
// few seeds from both layouts, and determinism. The full 64-seed sweep is the
// universe_trade_calibration executable, which is too slow for a test.
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
            require(m.coin_conserved, "total coin must never change");
            require(m.prices_in_range, "every price must stay within the 0.25x-4x range");
            require(
                m.final_merchants <= m.final_settlements * universe::kMaxMerchantsPerSettlement,
                "the merchant bound must hold");
            const auto again = universe::run_trade_seed(layout, seed, kYears);
            require(
                m.final_population == again.final_population && m.merchant_years == again.merchant_years &&
                    m.trade_pays_settlement_years == again.trade_pays_settlement_years,
                "the same seed must measure the same");
        }
    }
}

}  // namespace

int main() {
    test_invariants_hold_on_a_few_seeds_of_both_layouts();
    std::cout << "trade calibration tests passed\n";
    return 0;
}
