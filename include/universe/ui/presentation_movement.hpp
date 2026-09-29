#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

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

struct AgentPresentation {
    PresentationPoint home{};
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

[[nodiscard]] std::unordered_map<PersonId, AgentPresentation> build_agent_presentations(
    const SimulationController& controller,
    std::int64_t tick);

}  // namespace universe::ui
