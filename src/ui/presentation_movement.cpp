#include "universe/ui/presentation_movement.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace universe::ui {

namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kWorkshopDoorDistance = 3.6F;
constexpr float kResourceEdgeDistance = 40.0F;
// Presentation time of day: tick % 86400 == 0 is midday, matching the Godot
// client's day/night lighting (day_night.gd), so a fresh connection's
// schedule lines up with what's on screen.
constexpr std::int64_t kDaySeconds = 86400;
constexpr std::int64_t kNoonOffsetSeconds = 43200;
constexpr std::int64_t kOutDepartSeconds = (6 * 3600) + 1800;
constexpr std::int64_t kOutDepartSpreadSeconds = 9000;
constexpr std::int64_t kBackDepartSeconds = 16 * 3600;
constexpr std::int64_t kBackDepartSpreadSeconds = 10800;

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] PresentationPoint direction_from_degrees(const float degrees) noexcept {
    const auto radians = degrees * (kPi / 180.0F);
    return PresentationPoint{std::cos(radians), std::sin(radians)};
}

[[nodiscard]] std::int64_t day_index(const std::int64_t tick) noexcept {
    const auto presentation = tick + kNoonOffsetSeconds;
    return presentation >= 0 ? presentation / kDaySeconds : -1 - ((-presentation - 1) / kDaySeconds);
}

struct DaySchedule {
    std::int64_t out_depart{};
    std::int64_t back_depart{};
};

[[nodiscard]] DaySchedule day_schedule(const PersonId id, const std::int64_t tick) noexcept {
    const auto day = day_index(tick);
    const auto day_start = (day * kDaySeconds) - kNoonOffsetSeconds;
    const auto hash = mix64((id * 0x100000001B3ULL) ^ static_cast<std::uint64_t>(day));
    return DaySchedule{
        .out_depart = day_start + kOutDepartSeconds + static_cast<std::int64_t>(hash % kOutDepartSpreadSeconds),
        .back_depart = day_start + kBackDepartSeconds + static_cast<std::int64_t>((hash >> 20U) % kBackDepartSpreadSeconds),
    };
}

[[nodiscard]] ActivityTargetKind target_kind_for(const Person& person) noexcept {
    switch (person.current_activity) {
        case ActivityKind::Work:
        case ActivityKind::Discovery:
            switch (person.occupation) {
                case Occupation::SubsistenceCultivator:
                    return ActivityTargetKind::Field;
                case Occupation::WildFoodGatherer:
                case Occupation::WoodGatherer:
                case Occupation::StoneExtractor:
                    return ActivityTargetKind::Woodland;
                case Occupation::CraftWorker:
                    return ActivityTargetKind::Workshop;
                case Occupation::None:
                    return ActivityTargetKind::Home;
            }
            return ActivityTargetKind::Home;
        case ActivityKind::Social:
        case ActivityKind::Teaching:
        case ActivityKind::RecordKeeping:
        case ActivityKind::Travel:
            return ActivityTargetKind::Common;
        case ActivityKind::Sleep:
        case ActivityKind::Eat:
        case ActivityKind::Childcare:
        case ActivityKind::Rest:
        case ActivityKind::Idle:
            return ActivityTargetKind::Home;
    }
    return ActivityTargetKind::Home;
}

[[nodiscard]] PresentationPoint resource_edge_point(const SettlementId settlement_id, const PresentationPoint centre, const bool quarry) noexcept {
    const auto hash = mix64(static_cast<std::uint64_t>(settlement_id) ^ (quarry ? 0xA5A5ULL : 0x5A5AULL));
    const auto degrees = static_cast<float>(hash % 360U);
    const auto direction = direction_from_degrees(degrees);
    return PresentationPoint{centre.x + (direction.x * kResourceEdgeDistance), centre.z + (direction.z * kResourceEdgeDistance)};
}

struct SettlementFacts {
    PresentationPoint centre{};
    std::optional<PresentationPoint> workshop_door{};
};

}  // namespace

std::unordered_map<PersonId, AgentPresentation> build_agent_presentations(
    const SimulationController& controller,
    const std::int64_t tick) {
    std::unordered_map<HouseholdId, const Household*> household_by_id;
    for (const auto& household : controller.households()) {
        household_by_id.emplace(household.id, &household);
    }

    std::unordered_map<SettlementId, SettlementFacts> facts_by_settlement;
    for (const auto& settlement : controller.settlements()) {
        facts_by_settlement.emplace(
            settlement.id,
            SettlementFacts{.centre = PresentationPoint{
                static_cast<float>(settlement.position_x), static_cast<float>(settlement.position_z)}});
    }
    // The lowest-id complete Workshop in a settlement is a stable, real
    // building the client also draws at this same canonical position, so a
    // craft worker's target position here can never disagree with the
    // building the player sees.
    std::vector<const Building*> buildings_by_id;
    for (const auto& building : controller.buildings()) {
        buildings_by_id.push_back(&building);
    }
    std::sort(buildings_by_id.begin(), buildings_by_id.end(), [](const Building* a, const Building* b) { return a->id < b->id; });
    for (const auto* building : buildings_by_id) {
        if (building->type != BuildingType::Workshop || building->state != ConstructionState::Complete) {
            continue;
        }
        auto& facts = facts_by_settlement[building->settlement_id];
        if (facts.workshop_door.has_value()) {
            continue;
        }
        const auto direction = direction_from_degrees(static_cast<float>(building->facing_degrees));
        facts.workshop_door = PresentationPoint{
            static_cast<float>(building->position_x) + (direction.x * kWorkshopDoorDistance),
            static_cast<float>(building->position_z) + (direction.z * kWorkshopDoorDistance),
        };
    }

    std::unordered_map<PersonId, AgentPresentation> presentations;
    for (const auto& person : controller.people()) {
        if (!person.alive) {
            continue;
        }
        const auto household_it = household_by_id.find(person.household_id);
        if (household_it == household_by_id.end()) {
            continue;
        }
        const auto* household = household_it->second;
        const auto facts_it = facts_by_settlement.find(household->settlement_id);
        if (facts_it == facts_by_settlement.end()) {
            continue;
        }
        const auto& facts = facts_it->second;

        AgentPresentation presentation{};
        presentation.home = PresentationPoint{
            facts.centre.x + static_cast<float>(household->settlement_offset_x),
            facts.centre.z + static_cast<float>(household->settlement_offset_z),
        };
        presentation.target_kind = target_kind_for(person);
        switch (presentation.target_kind) {
            case ActivityTargetKind::Home:
                break;
            case ActivityTargetKind::Common:
                presentation.target = facts.centre;
                break;
            case ActivityTargetKind::Woodland:
                presentation.target = resource_edge_point(household->settlement_id, facts.centre, false);
                break;
            case ActivityTargetKind::Quarry:
                presentation.target = resource_edge_point(household->settlement_id, facts.centre, true);
                break;
            case ActivityTargetKind::Workshop:
                presentation.target = facts.workshop_door.has_value() ? facts.workshop_door : std::optional{facts.centre};
                break;
            case ActivityTargetKind::Field:
                // Left unset: the client resolves this household's actual
                // drawn field position (village_dressing.gd owns that
                // layout).
                break;
        }

        if (presentation.target_kind != ActivityTargetKind::Home) {
            const auto schedule = day_schedule(person.id, tick);
            presentation.depart_tick = schedule.out_depart;
            presentation.return_depart_tick = schedule.back_depart;
        }

        presentations.emplace(person.id, presentation);
    }
    return presentations;
}

}  // namespace universe::ui
