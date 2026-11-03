// Settlement prices from days of cover (ADR 0003, ticket #70).
//
// A price is derived from a settlement's own stock and its own daily need for a
// resource, in integer arithmetic only. It depends on canonical state alone, never
// on wall-clock time or turn order, so it is never stored: it is recomputed from
// state and therefore survives save and load unchanged.
#pragma once

#include "universe/resources.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace universe {

// Household wealth already values stock in these relative points (food is one).
inline constexpr std::int64_t kWealthValueFood = 1;
inline constexpr std::int64_t kWealthValueWood = 2;
inline constexpr std::int64_t kWealthValueStone = 2;
inline constexpr std::int64_t kWealthValueTimber = 4;
inline constexpr std::int64_t kWealthValueBasicTool = 8;
// Coin endowment and base prices: coin per point of stock value (ADR 0003).
inline constexpr std::int64_t kCoinPerStockValue = 10;

// Prices are whole milli-coin per unit, so a quarter of the cheapest base price
// is still an integer. A trade rounds to whole coin where it settles.
inline constexpr std::int64_t kMillicoinPerCoin = 1'000;

// Price as a multiple of the base, in parts per thousand: 1000 is the base.
inline constexpr std::int64_t kPriceFloorPermille = 250;
inline constexpr std::int64_t kPriceCeilingPermille = 4'000;
inline constexpr std::int64_t kPricePermilleScale = 1'000;

// The cover at which a resource costs its base price, and the horizon over which
// outstanding demand (construction, crafting) is spread to get a daily need.
// Both are first values to be calibrated (#76).
inline constexpr std::int64_t kPriceTargetCoverDays = 30;
inline constexpr std::int64_t kPriceDemandHorizonDays = 30;

// Daily need is carried in thousandths of a unit per day.
inline constexpr std::int64_t kNeedMilliPerUnit = 1'000;

[[nodiscard]] std::int64_t base_price_millicoin(ResourceType resource) noexcept;

// Outstanding demand (a quantity) as a daily need over the demand horizon.
[[nodiscard]] std::int64_t daily_need_milli_from_outstanding(std::int64_t outstanding_units) noexcept;

// The price as a multiple of the base, in parts per thousand, for a settlement
// holding `stock` units against a need of `daily_need_milli`. It is 1000 at the
// target cover, rises as cover falls and falls as it rises, and stays between
// the floor and the ceiling. No need is the floor; no stock against a need is
// the ceiling. Never divides by zero.
[[nodiscard]] std::int64_t price_multiplier_permille(std::int64_t stock, std::int64_t daily_need_milli) noexcept;

[[nodiscard]] std::int64_t price_millicoin(ResourceType resource, std::int64_t stock, std::int64_t daily_need_milli) noexcept;

// One settlement's price for each of the five resources, in milli-coin per unit.
struct SettlementPrices {
    std::array<std::int64_t, 5> millicoin{};

    [[nodiscard]] std::int64_t of(const ResourceType resource) const noexcept {
        return millicoin[static_cast<std::size_t>(resource)];
    }
    [[nodiscard]] bool operator==(const SettlementPrices& other) const = default;
};

}  // namespace universe
