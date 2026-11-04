// How a trade trip ends when the world changes under it (ADR 0003, ticket #74).
//
// ASSUMPTION: ticket #72 (the trip mechanics and `MerchantTrip` state) had not
// landed when this was written. This module is therefore pure: it takes plain
// facts (which settlements are active, who absorbed whom, where settlements
// sit) and says where the cargo and coin go. It names no trip type, so #72 can
// call it from its arrival and return steps without reconciling structs. All
// arithmetic is integer; the answer depends on nothing but its inputs, with
// explicit tie-breaks.
#pragma once

#include "universe/resources.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace universe {

// A settlement and its canonical position (world units).
struct TripSite {
    std::uint64_t settlement_id{};
    std::int64_t x{};
    std::int64_t z{};
    bool active{};

    [[nodiscard]] bool operator==(const TripSite& other) const = default;
};

// `absorbed_id` was folded into `survivor_id` (a settlement merge).
struct AbsorptionLink {
    std::uint64_t absorbed_id{};
    std::uint64_t survivor_id{};

    [[nodiscard]] bool operator==(const AbsorptionLink& other) const = default;
};

// What the merchant is carrying when the trip ends: the goods and the coin.
struct TripHold {
    ResourceType resource{ResourceType::Food};
    std::int64_t cargo_amount{};
    std::int64_t coin{};

    [[nodiscard]] bool operator==(const TripHold& other) const = default;
};

enum class TripEndReason : std::uint8_t {
    MerchantDied = 0,           // died en route
    DestinationInactive = 1,    // destination gone on arrival: no sale, go home
    OriginInactiveOnReturn = 2  // origin gone when the merchant gets back
};

struct TripDelivery {
    std::uint64_t receiver_id{};
    TripHold hold{};
    bool sale_made{};  // false: nothing was sold, so the hold is the unsold cargo and the coin outlay

    [[nodiscard]] bool operator==(const TripDelivery& other) const = default;
};

// The active settlement that inherits a lost settlement's place: follow the
// absorption chain (cycle-safe) to an active survivor, otherwise the nearest
// active settlement by squared straight-line distance from `from_id`'s own
// position, lowest id on a tie. `from_id` itself is returned when it is active.
// Empty when no settlement is active.
[[nodiscard]] std::optional<std::uint64_t> resolve_receiving_settlement(
    std::uint64_t from_id,
    const std::vector<TripSite>& sites,
    const std::vector<AbsorptionLink>& absorptions) noexcept;

// Where the hold goes when a trip ends early. A dead merchant, or a destination
// that is gone, returns the whole hold to the origin (or its heir if the origin
// is gone too); the hold is never altered, so goods and coin are conserved.
// Empty only when no settlement is active at all.
[[nodiscard]] std::optional<TripDelivery> end_trip(
    TripEndReason reason,
    std::uint64_t origin_id,
    const TripHold& hold,
    const std::vector<TripSite>& sites,
    const std::vector<AbsorptionLink>& absorptions) noexcept;

}  // namespace universe
