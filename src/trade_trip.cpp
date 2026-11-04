#include "universe/trade_trip.hpp"

#include "universe/trade_merchants.hpp"
#include "universe/trade_pricing.hpp"

#include <algorithm>
#include <limits>

namespace universe {

namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

// Integer square root, rounded up. Inputs here are squares of world distances.
[[nodiscard]] std::int64_t isqrt_ceil(const std::int64_t value) noexcept {
    if (value <= 0) {
        return 0;
    }
    std::int64_t low = 0;
    std::int64_t high = std::min<std::int64_t>(value, 3'037'000'499LL);  // floor(sqrt(INT64_MAX))
    while (low < high) {
        const auto mid = low + ((high - low) / 2);
        if (mid * mid >= value) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return low;
}

// ceil(units * price / 1000) or floor, saturating instead of overflowing.
[[nodiscard]] std::int64_t scaled_coin(const std::int64_t units, const std::int64_t price_millicoin, const bool round_up) noexcept {
    if (units <= 0 || price_millicoin <= 0) {
        return 0;
    }
    std::int64_t product = 0;
    if (!checked_mul_i64(units, price_millicoin, product)) {
        return kInt64Max / kMillicoinPerCoin;
    }
    const auto whole = product / kMillicoinPerCoin;
    return round_up && (product % kMillicoinPerCoin) != 0 ? whole + 1 : whole;
}

}  // namespace

TradeCapability baseline_trade_capability() noexcept {
    return TradeCapability{.speed_units_per_day = kWalkingUnitsPerDay, .cargo_multiplier_permille = kCargoMultiplierScale};
}

std::int64_t trade_distance_units(
    const std::int64_t from_x, const std::int64_t from_z, const std::int64_t to_x, const std::int64_t to_z) noexcept {
    const auto dx = to_x - from_x;
    const auto dz = to_z - from_z;
    return isqrt_ceil((dx * dx) + (dz * dz));
}

std::int64_t travel_ticks(const std::int64_t distance_units, const std::int64_t speed_units_per_day) noexcept {
    if (speed_units_per_day <= 0) {
        return kInt64Max;
    }
    if (distance_units <= 0) {
        return 1;
    }
    std::int64_t scaled = 0;
    if (!checked_mul_i64(distance_units, days(1).ticks, scaled)) {
        return kInt64Max;
    }
    const auto whole = scaled / speed_units_per_day;
    const auto ticks = (scaled % speed_units_per_day) != 0 ? whole + 1 : whole;
    return std::max<std::int64_t>(1, ticks);
}

bool trip_within_reach(const std::int64_t distance_units, const TradeCapability& capability) noexcept {
    return travel_ticks(distance_units, capability.speed_units_per_day) <= days(kTradeMaxOneWayDays).ticks;
}

std::int64_t cargo_capacity_units(const TradeCapability& capability) noexcept {
    if (capability.cargo_multiplier_permille <= 0) {
        return 0;
    }
    std::int64_t scaled = 0;
    if (!checked_mul_i64(kBaseCargoUnits, capability.cargo_multiplier_permille, scaled)) {
        return kInt64Max / kCargoMultiplierScale;
    }
    return scaled / kCargoMultiplierScale;
}

std::int64_t trade_outlay_coin(const std::int64_t units, const std::int64_t price_millicoin) noexcept {
    return scaled_coin(units, price_millicoin, true);
}

std::int64_t trade_revenue_coin(const std::int64_t units, const std::int64_t price_millicoin) noexcept {
    return scaled_coin(units, price_millicoin, false);
}

std::int64_t units_affordable(const std::int64_t coin, const std::int64_t price_millicoin) noexcept {
    if (coin <= 0 || price_millicoin <= 0) {
        return 0;
    }
    std::int64_t scaled = 0;
    if (!checked_mul_i64(coin, kMillicoinPerCoin, scaled)) {
        return kInt64Max;
    }
    return scaled / price_millicoin;
}

std::optional<TripOffer> evaluate_trip(
    const std::int64_t origin_price_millicoin,
    const std::int64_t remembered_destination_price_millicoin,
    const std::int64_t origin_stock,
    const std::int64_t origin_coin,
    const TradeCapability& capability) noexcept {
    if (origin_price_millicoin <= 0 || remembered_destination_price_millicoin <= 0 || origin_stock <= 0) {
        return std::nullopt;
    }
    const auto sendable = (origin_stock / 100) * kMaxCargoShareOfStockPercent +
        ((origin_stock % 100) * kMaxCargoShareOfStockPercent) / 100;
    const auto cargo = std::min({cargo_capacity_units(capability), sendable, units_affordable(origin_coin, origin_price_millicoin)});
    if (cargo <= 0) {
        return std::nullopt;
    }
    const auto outlay = trade_outlay_coin(cargo, origin_price_millicoin);
    const auto revenue = trade_revenue_coin(cargo, remembered_destination_price_millicoin);
    const auto profit = revenue - outlay;
    if (outlay > origin_coin || profit < kMinimumTripProfitCoin) {
        return std::nullopt;
    }
    return TripOffer{.cargo_units = cargo, .coin_outlay = outlay, .expected_revenue = revenue, .expected_profit = profit};
}

TradeProceeds split_trade_proceeds(const std::int64_t coin_carried, const std::int64_t coin_outlay) noexcept {
    const auto carried = std::max<std::int64_t>(0, coin_carried);
    const auto profit = carried - coin_outlay;
    std::int64_t share = 0;
    if (profit > 0) {
        std::int64_t scaled = 0;
        share = checked_mul_i64(profit, kMerchantProfitSharePermille, scaled) ? scaled / kProfitShareScale
                                                                           : (profit / kProfitShareScale) * kMerchantProfitSharePermille;
    }
    share = std::min(share, carried);
    return TradeProceeds{.household_share = share, .commons_return = carried - share};
}

}  // namespace universe
