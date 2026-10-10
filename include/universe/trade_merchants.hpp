// When trade pays and who becomes a merchant (ADR 0003, ticket #71).
//
// The decision is made from prices alone: a settlement compares its own prices
// with those of its neighbours within a day's walk and, if the best margin on a
// good it holds clears a threshold, trains one of its adults as a merchant. The
// arithmetic here is pure and integer-only; the Simulation supplies the prices,
// the stock and the people.
#pragma once

#include "universe/resources.hpp"
#include "universe/time.hpp"
#include "universe/trade_pricing.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace universe {

// Positions are world units (the world is +-4000 across). Walking pace is the
// one baseline every trade distance is built on; a later capability function
// will raise it (ADR 0003). First values, calibrated by #76.
inline constexpr std::int64_t kWalkingUnitsPerDay = 1'000;
inline constexpr std::int64_t kDaysWalkKnownNeighbours = 1;

// A margin is (destination price - origin price) / origin price, in parts per
// thousand. A settlement trains a merchant when the best margin it knows of
// reaches this, and a merchant retires when none does.
inline constexpr std::int64_t kMerchantProfitThresholdPermille = 500;
inline constexpr std::int64_t kMerchantMarginScale = 1'000;

// A settlement needs this many eligible adults before it can spare one, and
// never has more merchants than this bound.
inline constexpr std::uint64_t kMerchantMinimumEligibleAdults = 12;
inline constexpr std::uint64_t kMaxMerchantsPerSettlement = 2;

// What a merchant last saw of one settlement's price for one resource.
struct MerchantPriceMemory {
    std::uint64_t merchant_id{};
    std::uint64_t settlement_id{};
    ResourceType resource{ResourceType::Food};
    std::int64_t price_millicoin{};
    SimulationTime seen_at{};

    [[nodiscard]] bool operator==(const MerchantPriceMemory& other) const = default;
};

struct PriceMargin {
    ResourceType resource{ResourceType::Food};
    std::int64_t margin_permille{};

    [[nodiscard]] bool operator==(const PriceMargin& other) const = default;
};

// The margin from buying at `origin` and selling at `destination`, in parts per
// thousand of the origin price. Zero when the origin price is not positive.
[[nodiscard]] std::int64_t margin_permille(std::int64_t origin_price, std::int64_t destination_price) noexcept;

// The best margin on a resource the origin actually holds (`origin_holds`),
// highest first and, on a tie, the lowest resource. Empty when it holds nothing.
[[nodiscard]] std::optional<PriceMargin> best_margin(
    const SettlementPrices& origin,
    const SettlementPrices& destination,
    const std::array<bool, 5>& origin_holds) noexcept;

[[nodiscard]] bool margin_clears_threshold(const std::optional<PriceMargin>& margin) noexcept;

enum class MerchantAction : std::uint8_t {
    None = 0,
    Appoint = 1,  // train one more merchant
    Retire = 2,   // every merchant returns to ordinary work
};

// The yearly decision for one settlement. Merchants retire as soon as no known
// margin clears the threshold; one more is trained when trade pays, the bound is
// not reached, the settlement can spare an adult, and someone is available. A
// settlement under food strain needs every worker it has: it trains none and
// recalls the merchants it has.
[[nodiscard]] MerchantAction decide_merchant_action(
    bool trade_pays,
    std::uint64_t merchants,
    std::uint64_t eligible_adults,
    std::uint64_t spare_candidates,
    bool under_food_strain) noexcept;

// Whether two positions are within `days` of walking at the baseline pace.
[[nodiscard]] bool within_days_of_walking(
    std::int64_t from_x, std::int64_t from_z, std::int64_t to_x, std::int64_t to_z, std::int64_t days) noexcept;

}  // namespace universe
