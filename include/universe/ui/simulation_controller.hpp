#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"

namespace universe::ui {

struct SimulationSummary {
    SeedValue seed{};
    SimulationTime time{};
    std::uint64_t year{};
    std::uint64_t population{};
    std::uint64_t total_people{};
    std::uint64_t alive_people{};
    std::uint64_t households{};
    std::uint64_t births{};
    std::uint64_t deaths{};
    std::uint64_t active_settlements{};
    std::uint64_t extinct_settlements{};
    std::uint64_t average_settlement_population{};
    std::uint64_t largest_settlement_population{};
    std::uint64_t high_pressure_settlements{};

    std::int64_t food_stored{};
    std::int64_t food_produced{};
    std::int64_t food_consumed{};
    std::int64_t unmet_food{};
    std::uint64_t households_in_shortage{};

    std::int64_t total_labour_capacity{};
    std::int64_t total_labour_demand{};
    std::int64_t total_labour_allocated{};
    std::int64_t labour_shortfall{};
    std::uint64_t zero_capable_worker_households{};
    std::uint64_t labour_constrained_households{};
    std::uint64_t eligible_workers{};
    std::uint64_t active_workers{};
    std::uint64_t unallocated_workers{};
    std::uint64_t cultivators{};
    std::uint64_t gatherers{};
    std::uint64_t wood_gatherers{};
    std::uint64_t stone_extractors{};
    std::uint64_t craft_workers{};
    std::int64_t cultivation_labour_available{};
    std::int64_t cultivation_labour_allocated{};
    std::int64_t gathering_labour_available{};
    std::int64_t gathering_labour_allocated{};
    std::int64_t wood_labour_available{};
    std::int64_t wood_labour_allocated{};
    std::int64_t stone_labour_available{};
    std::int64_t stone_labour_allocated{};
    std::int64_t craft_labour_available{};
    std::int64_t craft_labour_allocated{};
    std::int64_t material_labour_demand{};
    std::int64_t material_labour_allocated{};
    std::int64_t material_labour_shortfall{};
    std::int64_t material_production_shortfall{};
    std::int64_t wood_capacity{};
    std::int64_t wood_stock{};
    std::int64_t stone_capacity{};
    std::int64_t stone_remaining{};
    std::int64_t wood_inventory{};
    std::int64_t stone_inventory{};
    std::int64_t timber_inventory{};
    std::int64_t basic_tool_inventory{};
    std::int64_t basic_tool_broken{};
    std::uint32_t average_tool_coverage_per_10k{};
    std::uint64_t child_population{};
    std::uint64_t working_age_population{};
    std::uint64_t elderly_population{};

    std::uint64_t healthy_population{};
    std::uint64_t mildly_malnourished{};
    std::uint64_t severely_malnourished{};
    std::uint64_t critical_health{};
    std::int32_t average_health{};
};

struct SnapshotOperationResult {
    bool ok{false};
    std::string path{};
    std::uint64_t hash{};
    SnapshotErrorCode error_code{SnapshotErrorCode::None};
    std::string message{};
};

struct SettlementSummaryView {
    SettlementId id{};
    RegionId region_id{};
    bool active{false};
    std::uint64_t alive_population{};
    std::uint64_t households{};
    std::int64_t labour_capacity{};
    std::int64_t labour_demand{};
    std::int64_t labour_allocated{};
    std::int64_t labour_shortfall{};
    std::uint64_t eligible_workers{};
    std::uint64_t active_workers{};
    std::uint64_t unallocated_workers{};
    std::uint64_t cultivators{};
    std::uint64_t gatherers{};
    std::uint64_t wood_gatherers{};
    std::uint64_t stone_extractors{};
    std::uint64_t craft_workers{};
    std::int64_t cultivation_labour_available{};
    std::int64_t cultivation_labour_allocated{};
    std::int64_t gathering_labour_available{};
    std::int64_t gathering_labour_allocated{};
    std::int64_t wood_labour_available{};
    std::int64_t wood_labour_allocated{};
    std::int64_t stone_labour_available{};
    std::int64_t stone_labour_allocated{};
    std::int64_t craft_labour_available{};
    std::int64_t craft_labour_allocated{};
    std::int64_t wood_inventory{};
    std::int64_t stone_inventory{};
    std::int64_t timber_inventory{};
    std::int64_t basic_tool_inventory{};
    std::int64_t cumulative_basic_tool_broken{};
    std::uint32_t tool_coverage_per_10k{};
    std::int64_t material_shortfall{};
    std::int64_t accessible_food_capacity_per_day{};
    std::uint32_t pressure_per_10k{};
    std::int64_t food_stored{};
    std::int64_t food_unmet_total{};
};

class SimulationController {
public:
    explicit SimulationController(SimulationConfig config = {});

    void reset(SimulationConfig config);
    [[nodiscard]] const Simulation& simulation() const noexcept;

    [[nodiscard]] SimulationSummary summary() const noexcept;

    [[nodiscard]] std::span<const Person> people() const noexcept;
    [[nodiscard]] std::span<const Household> households() const noexcept;
    [[nodiscard]] std::span<const Settlement> settlements() const noexcept;
    [[nodiscard]] std::span<const RegionEnvironmentState> regions() const noexcept;
    [[nodiscard]] std::span<const SimulationEvent> events() const noexcept;
    [[nodiscard]] std::span<const AgentMemory> agent_memories() const noexcept;
    [[nodiscard]] std::vector<AgentMemory> memories_for_person(PersonId person_id) const;
    [[nodiscard]] bool person_remembers_event(PersonId person_id, EventId event_id) const noexcept;
    [[nodiscard]] std::uint64_t memory_count_for_person(PersonId person_id) const noexcept;
    [[nodiscard]] std::span<const AgentKnowledge> agent_knowledge() const noexcept;
    [[nodiscard]] std::vector<AgentKnowledge> knowledge_for_person(PersonId person_id) const;
    [[nodiscard]] std::uint64_t knowledge_count_for_person(PersonId person_id) const noexcept;
    [[nodiscard]] std::optional<KnownLifeStatus> known_life_status(PersonId knower_id, PersonId subject_id) const noexcept;
    [[nodiscard]] std::optional<HouseholdId> known_household_for_person(PersonId knower_id, PersonId subject_id) const noexcept;
    [[nodiscard]] std::optional<SettlementStatusKnowledgeFact> known_settlement_status(PersonId knower_id, SettlementId settlement_id) const noexcept;
    [[nodiscard]] std::vector<SettlementSummaryView> settlement_summaries() const;

    void advance_days(std::uint64_t days);
    void advance_years(std::uint64_t years);

    [[nodiscard]] SnapshotOperationResult save_snapshot_file(const std::string& path) const;
    [[nodiscard]] SnapshotOperationResult load_snapshot_file(const std::string& path);

private:
    Simulation simulation_;
};

}  // namespace universe::ui
