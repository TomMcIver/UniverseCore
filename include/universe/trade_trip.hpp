// A merchant's trade trip (ADR 0003, ticket #72).
//
// The pure, integer-only arithmetic of one trip: how far a merchant can go, how
// much it can carry, whether the trip pays and how the proceeds are split. The
// Simulation supplies prices, stock and people and applies the result.
//
// One capability function (`baseline_trade_capability`) is the single hook for
// travel speed and cargo load. Reach and load both read it, so better transport
// raises both without touching trip logic.
#pragma once

#include "universe/resources.hpp"
#include "universe/trade_merchants.hpp"
#include "universe/time.hpp"
#include "universe/trade_tuning.hpp"

#include <cstdint>
#include <optional>

namespace universe {

// Travel and load. First values, calibrated by #76.
inline constexpr std::int64_t kTradeMaxOneWayDays = 3;
inline constexpr std::int64_t kBaseCargoUnits = UNIVERSE_TRADE_BASE_CARGO_UNITS;
inline constexpr std::int64_t kCargoMultiplierScale = 1'000;
// A commons never sends more than this share of one stock in a single trip, so
// a cheap-looking margin cannot empty a settlement.
inline constexpr std::int64_t kMaxCargoShareOfStockPercent = UNIVERSE_TRADE_STOCK_CAP_PERCENT;
// The merchant's household keeps this share of a realised profit; the commons
// keeps the rest (parts per thousand).
inline constexpr std::int64_t kMerchantProfitSharePermille = UNIVERSE_TRADE_PROFIT_SHARE_PERMILLE;
inline constexpr std::int64_t kProfitShareScale = 1'000;
// A trip is scheduled only if it is expected to earn at least this much coin.
inline constexpr std::int64_t kMinimumTripProfitCoin = 1;

// What a settlement's transport allows. Walking baseline: kWalkingUnitsPerDay
// and a multiplier of one (1000 permille).
struct TradeCapability {
    std::int64_t speed_units_per_day{};
    std::int64_t cargo_multiplier_permille{kCargoMultiplierScale};

    [[nodiscard]] bool operator==(const TradeCapability& other) const = default;
};

[[nodiscard]] TradeCapability baseline_trade_capability() noexcept;

enum class TripPhase : std::uint8_t {
    Outbound = 0,   // carrying the cargo to the destination
    Returning = 1,  // carrying the coin (and any unsold goods) home
};

// Canonical state of one merchant's trip. One per merchant at most; sorted by
// merchant id in WorldState::merchant_trips. A merchant with a trip is away.
struct MerchantTrip {
    std::uint64_t merchant_id{};
    std::uint64_t origin_settlement_id{};
    std::uint64_t destination_settlement_id{};
    ResourceType resource{ResourceType::Food};
    // Units taken from the origin commons at departure.
    std::int64_t cargo_units{};
    // The cargo valued at the origin price: the commons pays itself, so no coin
    // moves at departure, but the commons must be able to fund it. Profit is
    // measured against it.
    std::int64_t coin_outlay{};
    // Coin the destination paid, carried home. Zero while Outbound.
    std::int64_t coin_carried{};
    // Cargo the destination could not pay for, carried home. Zero while Outbound.
    std::int64_t unsold_units{};
    SimulationTime departed_at{};
    SimulationTime arrives_at{};
    SimulationTime returns_at{};
    TripPhase phase{TripPhase::Outbound};

    [[nodiscard]] bool operator==(const MerchantTrip& other) const = default;
};

// Straight-line distance between two canonical positions, rounded up to a whole
// world unit.
[[nodiscard]] std::int64_t trade_distance_units(
    std::int64_t from_x, std::int64_t from_z, std::int64_t to_x, std::int64_t to_z) noexcept;

// Simulated seconds to cover `distance_units` at `speed_units_per_day`, rounded
// up and never below one. A non-positive speed never arrives (max value).
[[nodiscard]] std::int64_t travel_ticks(std::int64_t distance_units, std::int64_t speed_units_per_day) noexcept;

// One-way travel of at most kTradeMaxOneWayDays. Doubling the speed doubles the
// distance this allows.
[[nodiscard]] bool trip_within_reach(std::int64_t distance_units, const TradeCapability& capability) noexcept;

// Units one trip can carry: the base load times the capability multiplier.
[[nodiscard]] std::int64_t cargo_capacity_units(const TradeCapability& capability) noexcept;

// Coin for `units` at a milli-coin price. The buyer's outlay rounds up and the
// seller's revenue rounds down, so rounding never creates coin.
[[nodiscard]] std::int64_t trade_outlay_coin(std::int64_t units, std::int64_t price_millicoin) noexcept;
[[nodiscard]] std::int64_t trade_revenue_coin(std::int64_t units, std::int64_t price_millicoin) noexcept;

// The most units `coin` can pay for at a milli-coin price.
[[nodiscard]] std::int64_t units_affordable(std::int64_t coin, std::int64_t price_millicoin) noexcept;

// A trip worth making, from the origin's actual price and the destination price
// the merchant remembers.
struct TripOffer {
    std::int64_t cargo_units{};
    std::int64_t coin_outlay{};
    std::int64_t expected_revenue{};
    std::int64_t expected_profit{};

    [[nodiscard]] bool operator==(const TripOffer& other) const = default;
};

// The cargo is the least of the carrying capacity, the share of stock the
// commons may send and what its coin can pay for. It is also capped at what the
// coin the merchant remembers the destination holding can buy (#86): a destination
// known to be out of coin yields no trip, and an unknown balance (kCoinUnknown)
// caps nothing. Empty when nothing can be carried or the expected profit is below
// kMinimumTripProfitCoin: an unprofitable trip is never offered.
[[nodiscard]] std::optional<TripOffer> evaluate_trip(
    std::int64_t origin_price_millicoin,
    std::int64_t remembered_destination_price_millicoin,
    std::int64_t origin_stock,
    std::int64_t origin_coin,
    const TradeCapability& capability,
    std::int64_t remembered_destination_coin = kCoinUnknown) noexcept;

// Where a returning merchant's coin goes. The two parts always sum to the coin
// carried: nothing is created or lost to rounding.
struct TradeProceeds {
    std::int64_t household_share{};
    std::int64_t commons_return{};

    [[nodiscard]] bool operator==(const TradeProceeds& other) const = default;
};

[[nodiscard]] TradeProceeds split_trade_proceeds(std::int64_t coin_carried, std::int64_t coin_outlay) noexcept;

}  // namespace universe
