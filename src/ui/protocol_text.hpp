#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "universe/ui/simulation_controller.hpp"

namespace universe::ui::detail {

[[nodiscard]] inline std::uint32_t age_years_from_person(const SimulationSummary& summary, const Person& person) noexcept {
    const auto year_i64 = static_cast<std::int64_t>(summary.year);
    if (year_i64 <= person.birth_year) {
        return 0U;
    }
    return static_cast<std::uint32_t>(year_i64 - person.birth_year);
}

[[nodiscard]] inline std::uint64_t non_negative_ticks(const SimulationTime time) noexcept {
    return static_cast<std::uint64_t>(time.ticks < 0 ? 0 : time.ticks);
}

[[nodiscard]] inline std::string biological_sex_label(const BiologicalSex sex) {
    return sex == BiologicalSex::Female ? "Female" : "Male";
}

[[nodiscard]] inline std::string occupation_label(const Occupation occupation) {
    switch (occupation) {
        case Occupation::None:
            return "None";
        case Occupation::SubsistenceCultivator:
            return "SubsistenceCultivator";
        case Occupation::WildFoodGatherer:
            return "WildFoodGatherer";
        case Occupation::WoodGatherer:
            return "WoodGatherer";
        case Occupation::StoneExtractor:
            return "StoneExtractor";
        case Occupation::CraftWorker:
            return "CraftWorker";
    }
    return "Unknown";
}

[[nodiscard]] inline std::string building_type_label(const BuildingType type) {
    switch (type) {
        case BuildingType::Dwelling:
            return "Dwelling";
        case BuildingType::FoodStore:
            return "FoodStore";
        case BuildingType::Workshop:
            return "Workshop";
    }
    return "Unknown";
}

[[nodiscard]] inline std::string building_state_label(const ConstructionState state) {
    switch (state) {
        case ConstructionState::Planned:
            return "Planned";
        case ConstructionState::ActiveConstruction:
            return "ActiveConstruction";
        case ConstructionState::Complete:
            return "Complete";
    }
    return "Unknown";
}

[[nodiscard]] inline std::string activity_kind_label(const ActivityKind activity) {
    switch (activity) {
        case ActivityKind::Sleep:
            return "Sleep";
        case ActivityKind::Travel:
            return "Travel";
        case ActivityKind::Work:
            return "Work";
        case ActivityKind::Eat:
            return "Eat";
        case ActivityKind::Social:
            return "Social";
        case ActivityKind::Teaching:
            return "Teaching";
        case ActivityKind::RecordKeeping:
            return "RecordKeeping";
        case ActivityKind::Discovery:
            return "Discovery";
        case ActivityKind::Childcare:
            return "Childcare";
        case ActivityKind::Rest:
            return "Rest";
        case ActivityKind::Idle:
            return "Idle";
    }
    return "Unknown";
}

[[nodiscard]] inline std::string settlement_tier_label(const SettlementTier tier) {
    switch (tier) {
        case SettlementTier::Village:
            return "Village";
        case SettlementTier::Town:
            return "Town";
        case SettlementTier::City:
            return "City";
    }
    return "Unknown";
}

[[nodiscard]] inline std::string json_escape(const std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const auto ch : text) {
        switch (ch) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
}

template <typename Entity, typename Id>
[[nodiscard]] const Entity* find_by_id(const std::span<const Entity> entities, const Id id) noexcept {
    for (const auto& entity : entities) {
        if (entity.id == id) {
            return &entity;
        }
    }
    return nullptr;
}

}  // namespace universe::ui::detail
