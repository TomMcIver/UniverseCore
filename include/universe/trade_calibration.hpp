// Trade calibration harness (ADR 0003, ticket #76): runs one seed and records the
// merchant, trade-pays, coin and price measurements. Used by the sweep executable
// and by a small invariant test. Reads state through the public Simulation API.
#pragma once

#include "universe/demo_scenario.hpp"
#include "universe/simulation.hpp"
#include "universe/trade_merchants.hpp"
#include "universe/trade_pricing.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace universe {

enum class CalibrationLayout : std::uint8_t { SingleSettlement, DefaultLayout };

struct TradeSeedMetrics {
    std::uint64_t seed{};
    bool coin_conserved{true};
    bool prices_in_range{true};
    std::uint64_t final_population{};
    std::uint64_t final_settlements{};
    std::uint64_t final_merchants{};
    std::uint64_t peak_merchants{};
    std::uint64_t merchant_years{};               // sum over year ends of living merchants
    std::uint64_t settlement_years{};             // sum over year ends of active settlements
    std::uint64_t trade_pays_settlement_years{};  // active settlements whose best margin clears the threshold
    std::int64_t max_margin_permille{};           // best in-range margin seen at any year end
    std::int64_t min_price_millicoin{std::numeric_limits<std::int64_t>::max()};
    std::int64_t max_price_millicoin{};
    std::int64_t max_price_spread_permille{};  // widest same-resource gap between two settlements, over the lower price
    std::int64_t total_exchange_volume{};
};

inline SimulationConfig calibration_config(const CalibrationLayout layout, const std::uint64_t seed) {
    SimulationConfig config{
        .seed = seed,
        .initial_population = 200U,
        .enable_unrelated_random_probe = false,
        .initial_household_food_units = 80,
        .food_production_scale_percent = 100,
        .food_system_enabled = true,
    };
    if (layout == CalibrationLayout::SingleSettlement) {
        config.initial_region_count = 1;
        config.initial_settlements_per_region = 1;
        config.base_region_area_units = kSingleSettlementRegionLandUnits;
    }
    return config;
}

inline std::int64_t calibration_total_coin(const WorldState& state) {
    std::int64_t total = 0;
    for (const auto& settlement : state.settlements) {
        total += settlement.coin;
    }
    for (const auto& household : state.households) {
        total += household.coin;
    }
    return total;
}

// Samples one year-end into `metrics`.
inline void sample_trade_year_end(const Simulation& sim, TradeSeedMetrics& metrics) {
    const auto& state = sim.state();
    std::uint64_t merchants = 0;
    for (const auto& person : state.people) {
        if (person.alive && person.occupation == Occupation::Merchant) {
            ++merchants;
        }
    }
    metrics.peak_merchants = std::max(metrics.peak_merchants, merchants);
    metrics.merchant_years += merchants;

    std::vector<const Settlement*> active;
    std::vector<SettlementPrices> prices;
    for (const auto& settlement : state.settlements) {
        const auto price = settlement.active ? sim.settlement_prices(settlement.id) : std::nullopt;
        if (price.has_value()) {
            active.push_back(&settlement);
            prices.push_back(*price);
        }
    }
    metrics.settlement_years += active.size();
    for (std::size_t i = 0; i < active.size(); ++i) {
        const auto& from = *active[i];
        const std::array<bool, 5> holds{
            from.commons_food.value > 0,
            from.materials.wood.value > 0,
            from.materials.stone.value > 0,
            from.materials.timber.value > 0,
            from.materials.basic_tool.value > 0,
        };
        std::optional<PriceMargin> best;
        for (std::size_t j = 0; j < active.size(); ++j) {
            if (i == j) {
                continue;
            }
            const auto& to = *active[j];
            for (std::size_t r = 0; r < 5; ++r) {
                const auto low = std::min(prices[i].millicoin[r], prices[j].millicoin[r]);
                const auto high = std::max(prices[i].millicoin[r], prices[j].millicoin[r]);
                metrics.max_price_spread_permille = std::max(metrics.max_price_spread_permille, margin_permille(low, high));
            }
            if (!within_days_of_walking(from.position_x, from.position_z, to.position_x, to.position_z, kDaysWalkKnownNeighbours)) {
                continue;
            }
            const auto margin = best_margin(prices[i], prices[j], holds);
            if (margin.has_value() && (!best.has_value() || margin->margin_permille > best->margin_permille)) {
                best = margin;
            }
        }
        if (best.has_value()) {
            metrics.max_margin_permille = std::max(metrics.max_margin_permille, best->margin_permille);
        }
        if (margin_clears_threshold(best)) {
            ++metrics.trade_pays_settlement_years;
        }
        for (std::size_t r = 0; r < 5; ++r) {
            const auto price = prices[i].millicoin[r];
            metrics.min_price_millicoin = std::min(metrics.min_price_millicoin, price);
            metrics.max_price_millicoin = std::max(metrics.max_price_millicoin, price);
            const auto base = base_price_millicoin(static_cast<ResourceType>(r));
            if (price < (base * kPriceFloorPermille) / kPricePermilleScale ||
                price > (base * kPriceCeilingPermille) / kPricePermilleScale) {
                metrics.prices_in_range = false;
            }
        }
    }
}

inline TradeSeedMetrics run_trade_seed(const CalibrationLayout layout, const std::uint64_t seed, const int years) {
    Simulation sim{calibration_config(layout, seed)};
    const auto starting_coin = calibration_total_coin(sim.state());
    TradeSeedMetrics metrics{};
    metrics.seed = seed;
    for (int year = 0; year < years; ++year) {
        sim.run_years(1);
        if (calibration_total_coin(sim.state()) != starting_coin) {
            metrics.coin_conserved = false;
        }
        sample_trade_year_end(sim, metrics);
    }
    const auto& state = sim.state();
    for (const auto& person : state.people) {
        if (person.alive) {
            ++metrics.final_population;
            if (person.occupation == Occupation::Merchant) {
                ++metrics.final_merchants;
            }
        }
    }
    for (const auto& settlement : state.settlements) {
        metrics.final_settlements += settlement.active ? 1U : 0U;
    }
    metrics.total_exchange_volume = state.total_exchange_volume;
    if (metrics.min_price_millicoin == std::numeric_limits<std::int64_t>::max()) {
        metrics.min_price_millicoin = 0;
    }
    return metrics;
}

}  // namespace universe
