#include "universe/trade_trip_endings.hpp"

#include <algorithm>
#include <limits>

namespace universe {

namespace {

const TripSite* find_site(const std::vector<TripSite>& sites, const std::uint64_t id) noexcept {
    const auto it = std::find_if(sites.begin(), sites.end(), [id](const TripSite& site) { return site.settlement_id == id; });
    return it == sites.end() ? nullptr : &*it;
}

bool is_active(const std::vector<TripSite>& sites, const std::uint64_t id) noexcept {
    const auto* site = find_site(sites, id);
    return site != nullptr && site->active;
}

// Squared distance, saturating so a huge coordinate cannot overflow.
std::int64_t axis_squared(const std::int64_t lhs, const std::int64_t rhs) noexcept {
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    std::int64_t gap = 0;
    if (!checked_add_i64(lhs, -rhs, gap) || gap == std::numeric_limits<std::int64_t>::min()) {
        return kMax;
    }
    std::int64_t squared = 0;
    return checked_mul_i64(gap, gap, squared) ? squared : kMax;
}

std::int64_t squared_distance(const TripSite& a, const TripSite& b) noexcept {
    std::int64_t total = 0;
    return checked_add_i64(axis_squared(a.x, b.x), axis_squared(a.z, b.z), total) ? total
                                                                                  : std::numeric_limits<std::int64_t>::max();
}

}  // namespace

std::optional<std::uint64_t> resolve_receiving_settlement(
    const std::uint64_t from_id,
    const std::vector<TripSite>& sites,
    const std::vector<AbsorptionLink>& absorptions) noexcept {
    // Follow the merges, bounded so a malformed cycle cannot loop forever.
    std::uint64_t current = from_id;
    for (std::size_t step = 0; step <= absorptions.size(); ++step) {
        if (is_active(sites, current)) {
            return current;
        }
        const auto link = std::find_if(absorptions.begin(), absorptions.end(), [current](const AbsorptionLink& entry) {
            return entry.absorbed_id == current;
        });
        if (link == absorptions.end()) {
            break;
        }
        current = link->survivor_id;
    }

    const auto* anchor = find_site(sites, from_id);
    const TripSite* best = nullptr;
    std::int64_t best_distance = 0;
    for (const auto& site : sites) {
        if (!site.active) {
            continue;
        }
        const std::int64_t distance = anchor != nullptr ? squared_distance(*anchor, site) : 0;
        if (best == nullptr || distance < best_distance ||
            (distance == best_distance && site.settlement_id < best->settlement_id)) {
            best = &site;
            best_distance = distance;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return best->settlement_id;
}

std::optional<TripDelivery> end_trip(
    const TripEndReason reason,
    const std::uint64_t origin_id,
    const TripHold& hold,
    const std::vector<TripSite>& sites,
    const std::vector<AbsorptionLink>& absorptions) noexcept {
    (void)reason;  // every early ending returns the hold unchanged; the reason is for the caller's events
    const auto receiver = resolve_receiving_settlement(origin_id, sites, absorptions);
    if (!receiver.has_value()) {
        return std::nullopt;
    }
    return TripDelivery{.receiver_id = *receiver, .hold = hold, .sale_made = false};
}

}  // namespace universe
