// Trade calibration harness (ADR 0003, ticket #76): runs one seed and records the
// merchant, trade-pays, trip, coin and price measurements. Used by the sweep
// executable and by a small invariant test. Reads state through the public
// Simulation API and steps one day at a time, which is equivalent to any other
// advancement schedule.
#pragma once

#include "universe/demo_scenario.hpp"
#include "universe/simulation.hpp"
#include "universe/trade_merchants.hpp"
#include "universe/trade_pricing.hpp"
#include "universe/trade_trip.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <variant>
#include <vector>

namespace universe {

enum class CalibrationLayout : std::uint8_t { SingleSettlement, DefaultLayout };

struct TradeSeedMetrics {
    std::uint64_t seed{};
    bool trade_enabled{true};
    // Invariants, sampled every day.
    bool coin_conserved{true};  // settlements + households + coin carried on trips never changes
    bool goods_accounted{true}; // every trip's cargo, unsold goods and stock are non-negative and consistent
    bool trips_consistent{true}; // one trip per merchant, merchant alive and still a merchant, no future times
    bool prices_in_range{true};
    // Outcome.
    std::uint64_t final_population{};
    std::uint64_t final_settlements{};
    std::uint64_t final_merchants{};
    std::uint64_t stress_person_days{};  // sum over days of people under food stress
    std::uint64_t shortage_household_days{};
    // Merchants and the decision to trade (sampled at year ends).
    std::uint64_t peak_merchants{};
    std::uint64_t merchant_years{};
    std::uint64_t settlement_years{};
    std::uint64_t trade_pays_settlement_years{};
    std::int64_t max_margin_permille{};
    std::int64_t min_price_millicoin{std::numeric_limits<std::int64_t>::max()};
    std::int64_t max_price_millicoin{};
    std::int64_t max_price_spread_permille{};   // widest same-resource gap, over the lower price
    std::int64_t pair_gap_sum_permille{};       // all active pairs and resources, summed over year ends
    std::uint64_t pair_gap_count{};
    std::int64_t total_exchange_volume{};
    // Trips (daily, from the event log and the trip table).
    std::uint64_t trips_departed{};
    std::uint64_t trips_completed{};
    std::uint64_t trips_in_flight_at_end{};
    std::uint64_t trips_lost_money{};           // completed with a realised loss
    std::int64_t expected_profit_departed{};    // sum of profit expected at departure
    std::int64_t realised_profit_completed{};   // sum of realised profit at completion
    std::int64_t cargo_units_completed{};
    std::array<std::uint64_t, 5> trips_by_resource{};
    std::uint64_t merchants_who_travelled{};
    std::uint64_t max_trips_one_merchant{};
    std::uint64_t settlements_that_sent{};
    std::uint64_t max_trips_one_settlement{};
    std::int64_t max_profit_one_settlement{};
    std::int64_t min_profit_one_settlement{};
    std::int64_t max_coin_in_transit{};
    std::int64_t max_units_in_transit{};
    std::int64_t household_share_coin{};  // coin held by households at the end (their cut of profit)
    // Per completed trip. Revenue is realised profit plus the outlay noted when the
    // trip left.
    std::uint64_t trips_profitable{};
    std::uint64_t trips_sold_nothing{};
    std::int64_t revenue_completed{};
    std::int64_t outlay_completed{};
    // Where the coin ended up.
    std::uint64_t active_settlements_without_coin_at_end{};
    std::int64_t richest_settlement_coin_share_permille{};
};

inline SimulationConfig calibration_config(const CalibrationLayout layout, const std::uint64_t seed, const bool trade_enabled = true) {
    SimulationConfig config{
        .seed = seed,
        .initial_population = 200U,
        .enable_unrelated_random_probe = false,
        .initial_household_food_units = 80,
        .food_production_scale_percent = 100,
        .food_system_enabled = true,
    };
    config.trade_enabled = trade_enabled;
    if (layout == CalibrationLayout::SingleSettlement) {
        config.initial_region_count = 1;
        config.initial_settlements_per_region = 1;
        config.base_region_area_units = kSingleSettlementRegionLandUnits;
    }
    return config;
}

// All coin in the world: held by settlements and households or carried by a
// merchant between them.
inline std::int64_t calibration_total_coin(const WorldState& state) {
    std::int64_t total = 0;
    for (const auto& settlement : state.settlements) {
        total += settlement.coin;
    }
    for (const auto& household : state.households) {
        total += household.coin;
    }
    for (const auto& trip : state.merchant_trips) {
        total += trip.coin_carried;
    }
    return total;
}

inline std::int64_t calibration_household_coin(const WorldState& state) {
    std::int64_t total = 0;
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
                const auto gap = margin_permille(low, high);
                metrics.max_price_spread_permille = std::max(metrics.max_price_spread_permille, gap);
                if (j > i) {
                    metrics.pair_gap_sum_permille += gap;
                    ++metrics.pair_gap_count;
                }
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

// Daily checks on the trip table and the stock it draws on.
inline void sample_trade_day(
    const Simulation& sim, const std::int64_t starting_coin, TradeSeedMetrics& metrics) {
    const auto& state = sim.state();
    if (calibration_total_coin(state) != starting_coin) {
        metrics.coin_conserved = false;
    }
    metrics.stress_person_days += state.population_under_food_stress;
    metrics.shortage_household_days += state.households_in_shortage;

    std::int64_t coin_in_transit = 0;
    std::int64_t units_in_transit = 0;
    std::set<PersonId> seen;
    for (const auto& trip : state.merchant_trips) {
        coin_in_transit += trip.coin_carried;
        units_in_transit += trip.phase == TripPhase::Outbound ? trip.cargo_units : trip.unsold_units;
        if (trip.cargo_units <= 0 || trip.unsold_units < 0 || trip.unsold_units > trip.cargo_units || trip.coin_carried < 0 ||
            trip.coin_outlay < 0 || (trip.phase == TripPhase::Outbound && (trip.coin_carried != 0 || trip.unsold_units != 0))) {
            metrics.goods_accounted = false;
        }
        const bool unique = seen.insert(trip.merchant_id).second;
        bool merchant_ok = false;
        for (const auto& person : state.people) {
            if (person.id == trip.merchant_id) {
                merchant_ok = person.alive && person.occupation == Occupation::Merchant;
                break;
            }
        }
        if (!unique || !merchant_ok || trip.departed_at.ticks > state.time.ticks) {
            metrics.trips_consistent = false;
        }
    }
    for (const auto& settlement : state.settlements) {
        if (settlement.coin < 0 || settlement.commons_food.value < 0 || settlement.materials.wood.value < 0 ||
            settlement.materials.stone.value < 0 || settlement.materials.timber.value < 0 ||
            settlement.materials.basic_tool.value < 0) {
            metrics.goods_accounted = false;
        }
    }
    metrics.max_coin_in_transit = std::max(metrics.max_coin_in_transit, coin_in_transit);
    metrics.max_units_in_transit = std::max(metrics.max_units_in_transit, units_in_transit);
}

// Folds the trade events appended since `from` into `metrics`, keeping per-merchant
// and per-settlement tallies in the maps.
struct TripTallies {
    std::map<PersonId, std::uint64_t> trips_by_merchant;
    std::map<SettlementId, std::uint64_t> trips_by_settlement;
    std::map<SettlementId, std::int64_t> profit_by_settlement;
    std::map<PersonId, std::int64_t> outlay_by_merchant;
};

// Records the outlay of every trip in the table (a short trip can start and finish
// between two daily samples, so the outlay is noted while it is still outbound).
inline void note_outbound_trips(const WorldState& state, TripTallies& tallies) {
    for (const auto& trip : state.merchant_trips) {
        tallies.outlay_by_merchant[trip.merchant_id] = trip.coin_outlay;
    }
}

inline void fold_trade_events(const WorldState& state, std::size_t& from, TripTallies& tallies, TradeSeedMetrics& metrics) {
    for (; from < state.event_log.size(); ++from) {
        const auto& event = state.event_log[from];
        if (event.type != EventType::TradeDeparted && event.type != EventType::TradeCompleted) {
            continue;
        }
        const auto& payload = std::get<TradeEventPayload>(event.payload);
        if (event.type == EventType::TradeDeparted) {
            ++metrics.trips_departed;
            metrics.expected_profit_departed += payload.coin_profit;
            ++metrics.trips_by_resource[static_cast<std::size_t>(payload.resource)];
            ++tallies.trips_by_merchant[payload.merchant_id];
            ++tallies.trips_by_settlement[payload.origin_settlement_id];
        } else {
            ++metrics.trips_completed;
            metrics.realised_profit_completed += payload.coin_profit;
            metrics.cargo_units_completed += payload.cargo_units;
            metrics.trips_lost_money += payload.coin_profit < 0 ? 1U : 0U;
            tallies.profit_by_settlement[payload.origin_settlement_id] += payload.coin_profit;
            const auto outlay = tallies.outlay_by_merchant[payload.merchant_id];
            metrics.outlay_completed += outlay;
            metrics.revenue_completed += outlay + payload.coin_profit;
            metrics.trips_sold_nothing += (outlay + payload.coin_profit) == 0 ? 1U : 0U;
            metrics.trips_profitable += payload.coin_profit > 0 ? 1U : 0U;
        }
    }
}

inline void finish_trade_tallies(const TripTallies& tallies, TradeSeedMetrics& metrics) {
    metrics.merchants_who_travelled = tallies.trips_by_merchant.size();
    for (const auto& [id, count] : tallies.trips_by_merchant) {
        metrics.max_trips_one_merchant = std::max(metrics.max_trips_one_merchant, count);
    }
    metrics.settlements_that_sent = tallies.trips_by_settlement.size();
    for (const auto& [id, count] : tallies.trips_by_settlement) {
        metrics.max_trips_one_settlement = std::max(metrics.max_trips_one_settlement, count);
    }
    for (const auto& [id, profit] : tallies.profit_by_settlement) {
        metrics.max_profit_one_settlement = std::max(metrics.max_profit_one_settlement, profit);
        metrics.min_profit_one_settlement = std::min(metrics.min_profit_one_settlement, profit);
    }
}

inline TradeSeedMetrics run_trade_seed(
    const CalibrationLayout layout, const std::uint64_t seed, const int years, const bool trade_enabled = true) {
    Simulation sim{calibration_config(layout, seed, trade_enabled)};
    const auto starting_coin = calibration_total_coin(sim.state());
    TradeSeedMetrics metrics{};
    metrics.seed = seed;
    metrics.trade_enabled = trade_enabled;
    TripTallies tallies;
    std::size_t event_cursor = 0;
    for (int year = 0; year < years; ++year) {
        for (std::int64_t day = 0; day < DemographicCalendarAdapter::kDaysPerYear; ++day) {
            sim.advance(days(1));
            sample_trade_day(sim, starting_coin, metrics);
            fold_trade_events(sim.state(), event_cursor, tallies, metrics);
            note_outbound_trips(sim.state(), tallies);
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
    metrics.trips_in_flight_at_end = state.merchant_trips.size();
    std::int64_t settlement_coin = 0;
    std::int64_t richest = 0;
    for (const auto& settlement : state.settlements) {
        settlement_coin += settlement.coin;
        richest = std::max(richest, settlement.coin);
        metrics.active_settlements_without_coin_at_end += (settlement.active && settlement.coin == 0) ? 1U : 0U;
    }
    metrics.richest_settlement_coin_share_permille = settlement_coin == 0 ? 0 : (richest * 1000) / settlement_coin;
    metrics.household_share_coin = calibration_household_coin(state);
    metrics.total_exchange_volume = state.total_exchange_volume;
    finish_trade_tallies(tallies, metrics);
    if (metrics.min_price_millicoin == std::numeric_limits<std::int64_t>::max()) {
        metrics.min_price_millicoin = 0;
    }
    return metrics;
}

inline bool trade_invariants_hold(const TradeSeedMetrics& m) {
    return m.coin_conserved && m.goods_accounted && m.trips_consistent && m.prices_in_range &&
        m.trips_completed + m.trips_in_flight_at_end <= m.trips_departed;
}

}  // namespace universe
