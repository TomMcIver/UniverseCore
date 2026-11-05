#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "universe/ui/simulation_controller.hpp"

namespace universe::ui {

// Presentation state only (ADR 0001): where a villager appears and how they
// walk is a pure function of canonical state plus a tick. Never serialized
// and never read back by the simulation.
//
// Geometry ownership is split deliberately: home/common/woodland/quarry/
// workshop positions are cheap to derive here and nothing else draws them, so
// the server computes and sends them directly. A cultivator's field is
// already laid out, per household, by the Godot client's village dressing
// (apps/godot-client/scripts/world/village_dressing.gd) from canonical
// buildings/households -- duplicating that layout here would drift from what
// is actually drawn, so for Field the server names only *whose* field
// (the household id, already on every agent) and lets the client look up the
// real position it already computed.

struct PresentationPoint {
    float x{};
    float z{};

    [[nodiscard]] bool operator==(const PresentationPoint& other) const = default;
};

enum class ActivityTargetKind : std::uint8_t {
    Home = 0,
    Common = 1,
    Field = 2,
    Woodland = 3,
    Quarry = 4,
    Workshop = 5,
};

// A merchant's trade trip as the client draws it (#78). The trip itself is
// canonical (ADR 0003); this is only the two settlement centres and the trip
// clock, so the client can walk the merchant out to `destination` from
// `depart_tick`, and back to `origin` from `arrive_tick`, until `return_tick`.
// The walk pace and route are client presentation, like any villager's.
struct TripPresentation {
    SettlementId origin_id{};
    SettlementId destination_id{};
    PresentationPoint origin{};
    PresentationPoint destination{};
    std::int64_t depart_tick{};
    std::int64_t arrive_tick{};
    std::int64_t return_tick{};

    [[nodiscard]] bool operator==(const TripPresentation& other) const = default;
};

// The subset of canonical trip state presentation needs. Deliberately a
// plain struct so the adapter from the canonical trip type (#72) is one small
// mapping; ticks are simulation seconds like every other tick here.
struct TradeTripFacts {
    PersonId merchant{};
    SettlementId origin{};
    SettlementId destination{};
    std::int64_t depart_tick{};
    std::int64_t arrive_tick{};
    std::int64_t return_tick{};
};

struct AgentPresentation {
    PresentationPoint home{};
    // Set only while `tick` lies in [depart_tick, return_tick) of this
    // person's trip. While set, the person is away from their settlement and
    // does no settlement work.
    std::optional<TripPresentation> trip{};
    ActivityTargetKind target_kind{ActivityTargetKind::Home};
    // Set for Common/Woodland/Quarry/Workshop, whose position the server
    // knows outright. Unset for Field (client-resolved) and for Home (equal
    // to `home`).
    std::optional<PresentationPoint> target{};
    // The daily schedule: when this person leaves home for `target_kind`,
    // and when they leave `target_kind` to come home. Pure per-person/day
    // policy, independent of how far away the target turns out to be -- the
    // client, which knows the real distance once it resolves Field/Workshop
    // positions, derives its own arrival ticks and walk animation from these
    // departure ticks plus a fixed walking speed.
    std::int64_t depart_tick{};
    std::int64_t return_depart_tick{};

    [[nodiscard]] bool operator==(const AgentPresentation& other) const = default;
};

// Maps canonical trips (WorldState::merchant_trips, ADR 0003) to the facts
// presentation needs. Pure and deterministic; never serialized. Outbound runs
// depart..arrive toward the destination, Returning arrive..return toward home.
// Ticks are simulation seconds.
[[nodiscard]] std::vector<TradeTripFacts> trade_trip_facts(std::span<const MerchantTrip> trips);
[[nodiscard]] std::vector<TradeTripFacts> trade_trip_facts(const SimulationController& controller);

[[nodiscard]] std::unordered_map<PersonId, AgentPresentation> build_agent_presentations(
    const SimulationController& controller,
    std::int64_t tick,
    std::span<const TradeTripFacts> trips = {});

}  // namespace universe::ui
