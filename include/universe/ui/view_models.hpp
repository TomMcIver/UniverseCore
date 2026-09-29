#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "universe/ui/simulation_controller.hpp"

namespace universe::ui {

// Typed, display-ready projections of canonical state, one per purpose.
// Presentation state only: building one never mutates the simulation.
enum class ViewModelKind : std::uint8_t {
    PersonSummary = 0,
    BuildingSummary = 1,
    SettlementEconomy = 2,
    PersonLifeHistory = 3,
    DemoFocus = 4,
};

[[nodiscard]] std::optional<ViewModelKind> parse_view_model_kind(std::string_view label) noexcept;
[[nodiscard]] std::string_view view_model_kind_label(ViewModelKind kind) noexcept;
// Bumped when a kind's data shape changes, so clients can reject shapes they don't understand.
[[nodiscard]] std::uint32_t view_model_version(ViewModelKind kind) noexcept;

struct PersonSummaryView {
    PersonId id{};
    bool alive{false};
    std::uint32_t age_years{};
    BiologicalSex sex{BiologicalSex::Female};
    Occupation occupation{Occupation::None};
    ActivityKind current_activity{ActivityKind::Idle};
    HouseholdId household_id{};
    SettlementId settlement_id{};
    RegionId region_id{};
    std::int32_t nutrition_points{};
    std::int32_t health_points{};
    std::uint64_t memory_count{};
    std::uint64_t knowledge_count{};
    std::optional<PersonId> mother_id{};
    std::optional<PersonId> father_id{};
};

struct BuildingSummaryView {
    BuildingId id{};
    BuildingType type{BuildingType::Dwelling};
    ConstructionState state{ConstructionState::Planned};
    SettlementId settlement_id{};
    std::optional<HouseholdId> owner_household_id{};
    bool settlement_commons_owned{true};
    std::int64_t timber_required{};
    std::int64_t timber_committed{};
    std::int64_t stone_required{};
    std::int64_t stone_committed{};
    std::int64_t labour_required{};
    std::int64_t labour_completed{};
    SimulationTime started_at{};
    std::optional<SimulationTime> completed_at{};
};

// Food totals are cumulative since world start, summed over the settlement's households.
struct SettlementEconomyView {
    SettlementId id{};
    RegionId region_id{};
    bool active{false};
    SettlementTier tier{SettlementTier::Village};
    std::uint64_t alive_population{};
    std::uint64_t households{};
    std::int64_t food_produced_total{};
    std::int64_t food_consumed_total{};
    std::int64_t food_unmet_total{};
    std::int64_t household_food_stored{};
    std::int64_t commons_food_stored{};
    std::int64_t accessible_food_capacity_per_day{};
    std::uint64_t cultivators{};
    std::uint64_t gatherers{};
    std::uint64_t wood_gatherers{};
    std::uint64_t stone_extractors{};
    std::uint64_t craft_workers{};
    std::uint64_t unallocated_workers{};
    std::int64_t min_household_wealth{};
    std::int64_t median_household_wealth{};
    std::int64_t max_household_wealth{};
    std::uint32_t inequality_per_10k{};
    std::uint64_t buildings_complete{};
    std::uint64_t buildings_under_construction{};
    std::uint64_t buildings_planned{};
};

[[nodiscard]] std::optional<PersonSummaryView> build_person_summary(const SimulationController& controller, PersonId person_id);
[[nodiscard]] std::optional<BuildingSummaryView> build_building_summary(const SimulationController& controller, BuildingId building_id);
[[nodiscard]] std::optional<SettlementEconomyView> build_settlement_economy(const SimulationController& controller, SettlementId settlement_id);

// Returns a view_model message, or an error message with code view_model_not_found.
[[nodiscard]] std::string make_view_model_message(
    std::uint32_t protocol_version,
    const SimulationController& controller,
    ViewModelKind kind,
    std::uint64_t entity_id);

}  // namespace universe::ui
