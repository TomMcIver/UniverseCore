#include "universe/trade_merchants.hpp"

#include <limits>

namespace universe {

std::int64_t margin_permille(const std::int64_t origin_price, const std::int64_t destination_price) noexcept {
    if (origin_price <= 0) {
        return 0;
    }
    std::int64_t gap = 0;
    if (!checked_add_i64(destination_price, -origin_price, gap)) {
        return 0;
    }
    std::int64_t scaled = 0;
    if (!checked_mul_i64(gap, kMerchantMarginScale, scaled)) {
        // Saturate: a gap too large to scale is an enormous margin either way.
        return gap > 0 ? std::numeric_limits<std::int64_t>::max() : std::numeric_limits<std::int64_t>::min();
    }
    return scaled / origin_price;
}

std::optional<PriceMargin> best_margin(
    const SettlementPrices& origin,
    const SettlementPrices& destination,
    const std::array<bool, 5>& origin_holds) noexcept {
    std::optional<PriceMargin> best;
    for (std::size_t index = 0; index < origin_holds.size(); ++index) {
        if (!origin_holds[index]) {
            continue;
        }
        const auto resource = static_cast<ResourceType>(index);
        const auto margin = margin_permille(origin.of(resource), destination.of(resource));
        if (!best.has_value() || margin > best->margin_permille) {
            best = PriceMargin{.resource = resource, .margin_permille = margin};
        }
    }
    return best;
}

bool margin_clears_threshold(const std::optional<PriceMargin>& margin) noexcept {
    return margin.has_value() && margin->margin_permille >= kMerchantProfitThresholdPermille;
}

MerchantAction decide_merchant_action(
    const bool trade_pays,
    const std::uint64_t merchants,
    const std::uint64_t eligible_adults,
    const std::uint64_t spare_candidates,
    const bool under_food_strain) noexcept {
    // A settlement that cannot feed its people recalls its merchants to work.
    if (!trade_pays || under_food_strain) {
        return merchants > 0U ? MerchantAction::Retire : MerchantAction::None;
    }
    if (merchants >= kMaxMerchantsPerSettlement || eligible_adults < kMerchantMinimumEligibleAdults || spare_candidates == 0U) {
        return MerchantAction::None;
    }
    return MerchantAction::Appoint;
}

bool within_days_of_walking(
    const std::int64_t from_x,
    const std::int64_t from_z,
    const std::int64_t to_x,
    const std::int64_t to_z,
    const std::int64_t days) noexcept {
    const auto reach = days * kWalkingUnitsPerDay;
    const auto dx = to_x - from_x;
    const auto dz = to_z - from_z;
    return (dx * dx) + (dz * dz) <= reach * reach;
}

}  // namespace universe
