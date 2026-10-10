#include "universe/trade_pricing.hpp"

#include <algorithm>
#include <limits>

namespace universe {

std::int64_t base_price_millicoin(const ResourceType resource) noexcept {
    std::int64_t wealth_value = kWealthValueFood;
    switch (resource) {
        case ResourceType::Food:
            wealth_value = kWealthValueFood;
            break;
        case ResourceType::Wood:
            wealth_value = kWealthValueWood;
            break;
        case ResourceType::Stone:
            wealth_value = kWealthValueStone;
            break;
        case ResourceType::Timber:
            wealth_value = kWealthValueTimber;
            break;
        case ResourceType::BasicTool:
            wealth_value = kWealthValueBasicTool;
            break;
    }
    return wealth_value * kCoinPerStockValue * kMillicoinPerCoin;
}

std::int64_t daily_need_milli_from_outstanding(const std::int64_t outstanding_units) noexcept {
    if (outstanding_units <= 0) {
        return 0;
    }
    std::int64_t milli_units = 0;
    if (!checked_mul_i64(outstanding_units, kNeedMilliPerUnit, milli_units)) {
        return std::numeric_limits<std::int64_t>::max() / kPriceDemandHorizonDays;
    }
    return milli_units / kPriceDemandHorizonDays;
}

std::int64_t price_multiplier_permille(const std::int64_t stock, const std::int64_t daily_need_milli) noexcept {
    if (daily_need_milli <= 0) {
        return kPriceFloorPermille;
    }
    if (stock <= 0) {
        return kPriceCeilingPermille;
    }
    // permille = 1000 * target_cover_days / cover_days, and cover_days is
    // stock / (daily_need_milli / 1000), so permille = target * need_milli / stock.
    std::int64_t scaled_need = 0;
    if (!checked_mul_i64(kPriceTargetCoverDays, daily_need_milli, scaled_need)) {
        return kPriceCeilingPermille;
    }
    return std::clamp(scaled_need / stock, kPriceFloorPermille, kPriceCeilingPermille);
}

std::int64_t price_millicoin(const ResourceType resource, const std::int64_t stock, const std::int64_t daily_need_milli) noexcept {
    return (base_price_millicoin(resource) * price_multiplier_permille(stock, daily_need_milli)) / kPricePermilleScale;
}

}  // namespace universe
