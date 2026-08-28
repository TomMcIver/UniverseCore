#include "universe/simulation.hpp"

#include "universe/snapshot.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace universe {

namespace {
constexpr std::uint32_t kAdultAgeYears = 18;
constexpr std::int32_t kOccupationMinimumNutritionPoints = 160;
constexpr std::int32_t kOccupationMinimumHealthPoints = 160;
constexpr std::int64_t kMinRegionAreaUnits = 10;
constexpr std::int64_t kMaxRegionAreaUnits = 10'000;
constexpr std::uint32_t kSettlementShareScale = 10'000;
constexpr std::uint32_t kInitialAllocatedSettlementShare = 8'500;
constexpr std::uint32_t kHighPressureThresholdPer10k = 8'500;
constexpr std::uint32_t kFoundingMinimumUnallocatedShare = 1'500;
constexpr std::uint32_t kFoundingDefaultShare = 900;
constexpr std::uint64_t kFoundingMinimumSourceHouseholds = 4;
constexpr std::uint64_t kFoundingMinimumNewHouseholds = 2;
constexpr std::uint64_t kFoundingMinimumRemainingHouseholds = 1;
constexpr std::uint64_t kFoundingMinimumEligibleWorkers = 4;
constexpr std::uint32_t kFoundingMinimumEconomicViabilityStreak = 2;
constexpr std::uint32_t kFoundingMinimumSourceShareAfterSplit = 1'500;
constexpr std::uint64_t kTargetHouseholdsPerSettlement = 8;
constexpr std::uint64_t kMaxInitialSettlementsPerRegion = 4;
constexpr std::int64_t kAssignmentRequiredCoveragePercent = 140;
constexpr std::int64_t kSettlementMaterialShareScale = 10'000;
constexpr std::int64_t kToolWearWorkUnitsPerBreak = 24;
constexpr std::uint32_t kToolProductivityBonusPercentAtFullCoverage = 45;
constexpr std::uint32_t kCommunicationChanceSameSettlementPer10k = 2'800;
constexpr std::uint32_t kTeachingChanceSameSettlementPer10k = 1'600;
constexpr std::uint32_t kCommunicationChanceSameHouseholdPer10k = 4'200;
constexpr std::uint32_t kTeachingChanceSameHouseholdPer10k = 6'400;
constexpr std::uint32_t kCommunicationConfidenceCapPer10k = 7'000;
constexpr std::uint32_t kTeachingConfidenceCapPer10k = 9'000;
constexpr std::uint32_t kRecordCreationChancePer10k = 1'200;
constexpr std::uint32_t kRecordReadChancePer10k = 1'900;
constexpr std::uint32_t kRecordWriteConfidenceCapPer10k = 9'500;
constexpr std::uint32_t kRecordReadConfidenceCapPer10k = 8'500;
constexpr std::uint32_t kKnowledgeDecayDirectPerYearPer10k = 120;
constexpr std::uint32_t kKnowledgeDecayTeachingPerYearPer10k = 260;
constexpr std::uint32_t kKnowledgeDecayRecordPerYearPer10k = 320;
constexpr std::uint32_t kKnowledgeDecayCommunicationPerYearPer10k = 420;
constexpr std::uint32_t kKnowledgeForgetThresholdPer10k = 1'200;
constexpr std::uint64_t kKnowledgeDecayGraceYears = 180;
constexpr std::uint32_t kDiscoveryAttemptPerSettlementPer10k = 1'800;
constexpr std::uint32_t kInstitutionFoundingChancePerSettlementPer10k = 1'000;
constexpr std::uint64_t kInstitutionMinimumSettlementPopulation = 24;
constexpr std::uint64_t kInstitutionMinimumMembers = 3;
constexpr std::uint64_t kInstitutionTargetMembers = 6;
constexpr std::uint32_t kInstitutionTeachingConfidenceCapPer10k = 9'200;
constexpr std::array<std::uint32_t, 3> kBasicDiscoveryKeys{1U, 2U, 3U};
constexpr std::int64_t kFoodCommonsTransferReserveDays = 14;
constexpr std::int64_t kWealthValueWood = 2;
constexpr std::int64_t kWealthValueStone = 2;
constexpr std::int64_t kWealthValueTimber = 4;
constexpr std::int64_t kWealthValueBasicTool = 8;
constexpr std::int64_t kDwellingTimberRequired = 20;
constexpr std::int64_t kDwellingStoneRequired = 8;
constexpr std::int64_t kDwellingLabourRequired = 560;
constexpr std::int64_t kFoodStoreTimberRequired = 26;
constexpr std::int64_t kFoodStoreStoneRequired = 12;
constexpr std::int64_t kFoodStoreLabourRequired = 680;
constexpr std::int64_t kWorkshopTimberRequired = 18;
constexpr std::int64_t kWorkshopStoneRequired = 20;
constexpr std::int64_t kWorkshopLabourRequired = 620;
constexpr std::int64_t kDwellingCapacityBonusPerBuilding = 18;
constexpr std::int64_t kFoodStoreCapacityBonusPerBuilding = 32;
constexpr std::int64_t kWorkshopCapacityBonusPerBuilding = 14;
constexpr std::int64_t kCommonsReserveTargetDays = 10;
constexpr std::int64_t kCommonsReplenishDays = 8;
constexpr std::int64_t kFoodCrisisBufferDays = 7;

struct LabourDemandProfile {
    std::int64_t cultivation_labour_demand{};
    std::int64_t gathering_labour_demand{};

    [[nodiscard]] std::int64_t total() const noexcept {
        return cultivation_labour_demand + gathering_labour_demand;
    }
};

struct MaterialTargetProfile {
    std::int64_t wood_target{};
    std::int64_t stone_target{};
    std::int64_t timber_target{};
    std::int64_t basic_tool_target{};
};

struct ConstructionDemandProfile {
    std::int64_t timber_remaining{};
    std::int64_t stone_remaining{};
    std::int64_t labour_remaining{};
    std::uint64_t active_projects{};
};

[[nodiscard]] ConstructionDemandProfile construction_demand_for_settlement(
    const SettlementId settlement_id,
    const std::vector<Building>& buildings) noexcept {
    ConstructionDemandProfile demand{};
    for (const auto& building : buildings) {
        if (building.settlement_id != settlement_id || building.state == ConstructionState::Complete) {
            continue;
        }

        ++demand.active_projects;
        demand.timber_remaining += std::max<std::int64_t>(0, building.timber_required - building.timber_committed);
        demand.stone_remaining += std::max<std::int64_t>(0, building.stone_required - building.stone_committed);
        demand.labour_remaining += std::max<std::int64_t>(0, building.labour_required - building.labour_completed);
    }
    return demand;
}

[[nodiscard]] MaterialTargetProfile material_targets_for_settlement(
    const std::uint64_t alive_population,
    const std::uint64_t eligible_workers) noexcept {
    const auto worker_i64 = static_cast<std::int64_t>(eligible_workers);
    const auto population_i64 = static_cast<std::int64_t>(alive_population);

    const auto basic_tool_target = std::max<std::int64_t>(4, (worker_i64 + 2) / 3);
    const auto timber_target = std::max<std::int64_t>(8, population_i64 / 2);
    const auto wood_target = (timber_target * 2) + basic_tool_target + std::max<std::int64_t>(2, population_i64 / 4);
    const auto stone_target = basic_tool_target + std::max<std::int64_t>(2, population_i64 / 5);

    return MaterialTargetProfile{
        .wood_target = wood_target,
        .stone_target = stone_target,
        .timber_target = timber_target,
        .basic_tool_target = basic_tool_target,
    };
}

[[nodiscard]] bool is_productive_occupation(const Occupation occupation) noexcept {
    return occupation == Occupation::SubsistenceCultivator ||
        occupation == Occupation::WildFoodGatherer ||
        occupation == Occupation::WoodGatherer ||
        occupation == Occupation::StoneExtractor ||
        occupation == Occupation::CraftWorker;
}

[[nodiscard]] std::uint32_t tool_coverage_per_10k(
    const std::int64_t tool_inventory,
    const std::uint64_t productive_workers) noexcept {
    if (productive_workers == 0U || tool_inventory <= 0) {
        return 0U;
    }

    const auto covered_workers = std::min<std::uint64_t>(
        productive_workers,
        static_cast<std::uint64_t>(tool_inventory));
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(10'000U, (covered_workers * 10'000U) / productive_workers));
}

[[nodiscard]] std::uint32_t tool_bonus_percent(const std::uint32_t coverage_per_10k) noexcept {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(std::min<std::uint32_t>(coverage_per_10k, 10'000U)) *
         kToolProductivityBonusPercentAtFullCoverage) /
        10'000U);
}

[[nodiscard]] std::uint32_t tool_output_scale_percent(const std::uint32_t coverage_per_10k) noexcept {
    return 100U + tool_bonus_percent(coverage_per_10k);
}

[[nodiscard]] std::int64_t compute_daily_food_output_target(
    const std::int64_t daily_food_required,
    const std::int64_t stored_food,
    const std::int64_t output_opportunity) noexcept {
    if (daily_food_required <= 0 || output_opportunity <= 0) {
        return 0;
    }

    const auto buffer_days = stored_food / std::max<std::int64_t>(1, daily_food_required);
    std::int64_t target = 0;
    if (buffer_days >= 90) {
        target = (daily_food_required * 90) / 100;
    } else if (buffer_days >= 45) {
        target = daily_food_required;
    } else if (buffer_days >= 14) {
        target = daily_food_required + (daily_food_required / 5);
    } else {
        target = daily_food_required + ((daily_food_required * 45) / 100);
    }

    return std::min(output_opportunity, std::max<std::int64_t>(target, 0));
}

[[nodiscard]] LabourDemandProfile split_demand_by_opportunity(
    const std::int64_t target_output,
    const std::int64_t cultivation_opportunity,
    const std::int64_t gathering_opportunity) noexcept {
    LabourDemandProfile demand{};
    const auto cultivation = std::max<std::int64_t>(0, cultivation_opportunity);
    const auto gathering = std::max<std::int64_t>(0, gathering_opportunity);
    const auto total_opportunity = cultivation + gathering;
    if (target_output <= 0 || total_opportunity <= 0) {
        return demand;
    }

    demand.cultivation_labour_demand = (target_output * cultivation) / total_opportunity;
    demand.gathering_labour_demand = target_output - demand.cultivation_labour_demand;
    return demand;
}

std::uint32_t bounded_u32(const std::uint64_t value) {
    return static_cast<std::uint32_t>(std::min(value, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
}

[[nodiscard]] std::uint64_t ceil_div_u64(const std::uint64_t value, const std::uint64_t divisor) noexcept {
    if (divisor == 0U) {
        return 0U;
    }
    return (value / divisor) + ((value % divisor) == 0U ? 0U : 1U);
}

std::int32_t clamp_i32(const std::int32_t value, const std::int32_t lo, const std::int32_t hi) {
    return std::min(std::max(value, lo), hi);
}

std::int64_t clamp_i64(const std::int64_t value, const std::int64_t lo, const std::int64_t hi) {
    return std::min(std::max(value, lo), hi);
}

std::int64_t environment_class_productive_units_per_area(const EnvironmentClass environment_class) {
    switch (environment_class) {
        case EnvironmentClass::ProductiveArable:
            return 15;
        case EnvironmentClass::Woodland:
            return 9;
        case EnvironmentClass::Marginal:
            return 6;
        case EnvironmentClass::WaterWetland:
            return 5;
    }
    return 0;
}

std::int64_t environment_class_wild_capacity_units_per_area(const EnvironmentClass environment_class) {
    switch (environment_class) {
        case EnvironmentClass::ProductiveArable:
            return 190;
        case EnvironmentClass::Woodland:
            return 250;
        case EnvironmentClass::Marginal:
            return 140;
        case EnvironmentClass::WaterWetland:
            return 220;
    }
    return 0;
}

[[nodiscard]] bool event_payload_matches_type(const SimulationEvent& event) noexcept {
    switch (event.type) {
        case EventType::Birth:
            return std::holds_alternative<BirthEventPayload>(event.payload);
        case EventType::Death:
            return std::holds_alternative<DeathEventPayload>(event.payload);
        case EventType::SettlementFounded:
            return std::holds_alternative<SettlementFoundedEventPayload>(event.payload);
        case EventType::SettlementExtinct:
            return std::holds_alternative<SettlementExtinctEventPayload>(event.payload);
        case EventType::DiscoveryMade:
            return std::holds_alternative<DiscoveryMadeEventPayload>(event.payload);
        case EventType::InstitutionFounded:
            return std::holds_alternative<InstitutionFoundedEventPayload>(event.payload);
    }
    return false;
}
}

Simulation::Simulation(SimulationConfig config)
    : config_(config),
      state_{.seed = config.seed, .time = SimulationTime{0}, .year = 0, .population = config.initial_population},
      simulation_seed_{config.seed} {
    if (config_.initial_household_food_units < 0) {
        throw std::invalid_argument("Initial household food units must be non-negative");
    }
    if (config_.food_production_scale_percent > 10'000U) {
        throw std::invalid_argument("Food production scale percent must be <= 10000");
    }
    if (config_.initial_region_count == 0U) {
        throw std::invalid_argument("Initial region count must be positive");
    }
    if (config_.base_region_area_units < kMinRegionAreaUnits || config_.base_region_area_units > kMaxRegionAreaUnits) {
        throw std::invalid_argument("Base region area units out of supported range");
    }
    if (config_.productive_effort_scale_percent > 10'000U) {
        throw std::invalid_argument("Productive effort scale percent must be <= 10000");
    }
    if (config_.wood_resource_scale_percent > 10'000U) {
        throw std::invalid_argument("Wood resource scale percent must be <= 10000");
    }
    if (config_.stone_resource_scale_percent > 10'000U) {
        throw std::invalid_argument("Stone resource scale percent must be <= 10000");
    }
    if (config_.wood_regeneration_scale_percent > 10'000U) {
        throw std::invalid_argument("Wood regeneration scale percent must be <= 10000");
    }
    if (config_.material_crafting_scale_percent > 10'000U) {
        throw std::invalid_argument("Material crafting scale percent must be <= 10000");
    }

    if (!config_.food_system_enabled) {
        food_next_run_ = SimulationTime{std::numeric_limits<std::int64_t>::max()};
    }

    initialize_population();
    initialize_households();
    initialize_regions();
    assign_households_to_regions();
    initialize_settlements();
    process_household_access_allocation();
    process_occupation_reassignment(0);
    refresh_population_count();
    refresh_food_aggregates();
    refresh_environment_aggregates();
    refresh_labour_aggregates();
    refresh_material_aggregates();
    refresh_economic_aggregates();
}

const WorldState& Simulation::state() const noexcept {
    return state_;
}

SimulationTime Simulation::time() const noexcept {
    return state_.time;
}

std::span<const AgentMemory> Simulation::agent_memories() const noexcept {
    return state_.agent_memories;
}

std::vector<AgentMemory> Simulation::memories_for_person(const PersonId person_id) const {
    std::vector<AgentMemory> memories;
    for (const auto& memory : state_.agent_memories) {
        if (memory.owner_id == person_id) {
            memories.push_back(memory);
        }
    }
    return memories;
}

std::span<const KnowledgeTransmission> Simulation::knowledge_transmissions() const noexcept {
    return state_.knowledge_transmissions;
}

std::vector<KnowledgeTransmission> Simulation::transmissions_for_recipient(const PersonId person_id) const {
    std::vector<KnowledgeTransmission> transmissions;
    for (const auto& transmission : state_.knowledge_transmissions) {
        if (transmission.recipient_id == person_id) {
            transmissions.push_back(transmission);
        }
    }
    return transmissions;
}

std::optional<KnowledgeTransmission> Simulation::transmission_for_memory(const MemoryId recipient_memory_id) const {
    const auto index_it = communication_index_by_recipient_memory_.find(recipient_memory_id);
    if (index_it == communication_index_by_recipient_memory_.end()) {
        return std::nullopt;
    }
    if (index_it->second >= state_.knowledge_transmissions.size()) {
        return std::nullopt;
    }
    return state_.knowledge_transmissions[index_it->second];
}

std::span<const KnowledgeRecord> Simulation::knowledge_records() const noexcept {
    return state_.knowledge_records;
}

std::vector<KnowledgeRecord> Simulation::records_for_settlement(const SettlementId settlement_id) const {
    std::vector<KnowledgeRecord> records;
    for (const auto& record : state_.knowledge_records) {
        if (record.settlement_id == settlement_id) {
            records.push_back(record);
        }
    }
    return records;
}

std::span<const Institution> Simulation::institutions() const noexcept {
    return state_.institutions;
}

std::vector<Institution> Simulation::institutions_for_settlement(const SettlementId settlement_id) const {
    std::vector<Institution> institutions;
    for (const auto& institution : state_.institutions) {
        if (institution.settlement_id == settlement_id) {
            institutions.push_back(institution);
        }
    }
    return institutions;
}

std::optional<KnowledgeRecordAccess> Simulation::record_access_for_memory(const MemoryId reader_memory_id) const {
    const auto index_it = record_access_index_by_memory_.find(reader_memory_id);
    if (index_it == record_access_index_by_memory_.end()) {
        return std::nullopt;
    }
    if (index_it->second >= state_.knowledge_record_accesses.size()) {
        return std::nullopt;
    }
    return state_.knowledge_record_accesses[index_it->second];
}

bool Simulation::person_remembers_event(const PersonId person_id, const EventId event_id) const noexcept {
    for (const auto& memory : state_.agent_memories) {
        if (memory.owner_id == person_id && memory.source_event_id == event_id) {
            return true;
        }
    }
    return false;
}


struct KnowledgeFactDescriptor {
    KnowledgeFactKind kind{KnowledgeFactKind::PersonLifeStatus};
    std::uint64_t primary_subject_id{};
    std::uint64_t secondary_subject_id{};
};

[[nodiscard]] KnowledgeFactDescriptor describe_knowledge_fact(const KnowledgeFact& fact) {
    if (const auto* person_life = std::get_if<PersonLifeStatusKnowledgeFact>(&fact); person_life != nullptr) {
        return KnowledgeFactDescriptor{
            .kind = KnowledgeFactKind::PersonLifeStatus,
            .primary_subject_id = person_life->person_id,
            .secondary_subject_id = 0U,
        };
    }

    if (const auto* person_household = std::get_if<PersonHouseholdKnowledgeFact>(&fact); person_household != nullptr) {
        return KnowledgeFactDescriptor{
            .kind = KnowledgeFactKind::PersonHousehold,
            .primary_subject_id = person_household->person_id,
            .secondary_subject_id = 0U,
        };
    }

    if (const auto* settlement = std::get_if<SettlementStatusKnowledgeFact>(&fact); settlement != nullptr) {
        return KnowledgeFactDescriptor{
            .kind = KnowledgeFactKind::SettlementStatus,
            .primary_subject_id = settlement->settlement_id,
            .secondary_subject_id = 0U,
        };
    }

    if (const auto* discovery = std::get_if<BasicDiscoveryKnowledgeFact>(&fact); discovery != nullptr) {
        return KnowledgeFactDescriptor{
            .kind = KnowledgeFactKind::BasicDiscovery,
            .primary_subject_id = discovery->discovery_key,
            .secondary_subject_id = 0U,
        };
    }

    throw std::invalid_argument("Unknown knowledge fact variant");
}

std::uint64_t Simulation::memory_count_for_person(const PersonId person_id) const noexcept {
    std::uint64_t count = 0;
    for (const auto& memory : state_.agent_memories) {
        if (memory.owner_id == person_id) {
            ++count;
        }
    }
    return count;
}

std::span<const AgentKnowledge> Simulation::agent_knowledge() const noexcept {
    return state_.agent_knowledge;
}

std::vector<AgentKnowledge> Simulation::knowledge_for_person(const PersonId person_id) const {
    std::vector<AgentKnowledge> facts;
    const auto owner_it = knowledge_indices_by_owner_.find(person_id);
    if (owner_it == knowledge_indices_by_owner_.end()) {
        return facts;
    }

    facts.reserve(owner_it->second.size());
    for (const auto index : owner_it->second) {
        if (index >= state_.agent_knowledge.size()) {
            continue;
        }
        if (state_.agent_knowledge[index].confidence_per_10k == 0U) {
            continue;
        }
        facts.push_back(state_.agent_knowledge[index]);
    }
    return facts;
}

std::uint64_t Simulation::knowledge_count_for_person(const PersonId person_id) const noexcept {
    const auto owner_it = knowledge_indices_by_owner_.find(person_id);
    if (owner_it == knowledge_indices_by_owner_.end()) {
        return 0U;
    }
    std::uint64_t count = 0U;
    for (const auto index : owner_it->second) {
        if (index >= state_.agent_knowledge.size()) {
            continue;
        }
        if (state_.agent_knowledge[index].confidence_per_10k == 0U) {
            continue;
        }
        ++count;
    }
    return count;
}

std::optional<KnownLifeStatus> Simulation::known_life_status(
    const PersonId knower_id,
    const PersonId subject_id) const noexcept {
    const KnowledgeSemanticKey key{
        .owner_id = knower_id,
        .fact_kind = KnowledgeFactKind::PersonLifeStatus,
        .primary_subject_id = subject_id,
        .secondary_subject_id = 0U,
    };
    const auto key_it = knowledge_index_by_semantic_key_.find(key);
    if (key_it == knowledge_index_by_semantic_key_.end() || key_it->second >= state_.agent_knowledge.size()) {
        return std::nullopt;
    }
    if (state_.agent_knowledge[key_it->second].confidence_per_10k == 0U) {
        return std::nullopt;
    }

    const auto* fact = std::get_if<PersonLifeStatusKnowledgeFact>(&state_.agent_knowledge[key_it->second].fact);
    if (fact == nullptr) {
        return std::nullopt;
    }

    return fact->status;
}

std::optional<HouseholdId> Simulation::known_household_for_person(
    const PersonId knower_id,
    const PersonId subject_id) const noexcept {
    const KnowledgeSemanticKey key{
        .owner_id = knower_id,
        .fact_kind = KnowledgeFactKind::PersonHousehold,
        .primary_subject_id = subject_id,
        .secondary_subject_id = 0U,
    };
    const auto key_it = knowledge_index_by_semantic_key_.find(key);
    if (key_it == knowledge_index_by_semantic_key_.end() || key_it->second >= state_.agent_knowledge.size()) {
        return std::nullopt;
    }
    if (state_.agent_knowledge[key_it->second].confidence_per_10k == 0U) {
        return std::nullopt;
    }

    const auto* fact = std::get_if<PersonHouseholdKnowledgeFact>(&state_.agent_knowledge[key_it->second].fact);
    if (fact == nullptr) {
        return std::nullopt;
    }

    return fact->household_id;
}

std::optional<SettlementStatusKnowledgeFact> Simulation::known_settlement_status(
    const PersonId knower_id,
    const SettlementId settlement_id) const noexcept {
    const KnowledgeSemanticKey key{
        .owner_id = knower_id,
        .fact_kind = KnowledgeFactKind::SettlementStatus,
        .primary_subject_id = settlement_id,
        .secondary_subject_id = 0U,
    };
    const auto key_it = knowledge_index_by_semantic_key_.find(key);
    if (key_it == knowledge_index_by_semantic_key_.end() || key_it->second >= state_.agent_knowledge.size()) {
        return std::nullopt;
    }
    if (state_.agent_knowledge[key_it->second].confidence_per_10k == 0U) {
        return std::nullopt;
    }

    const auto* fact = std::get_if<SettlementStatusKnowledgeFact>(&state_.agent_knowledge[key_it->second].fact);
    if (fact == nullptr) {
        return std::nullopt;
    }

    return *fact;
}

bool Simulation::knows_basic_discovery(const PersonId knower_id, const std::uint32_t discovery_key) const noexcept {
    if (discovery_key == 0U) {
        return false;
    }

    const KnowledgeSemanticKey key{
        .owner_id = knower_id,
        .fact_kind = KnowledgeFactKind::BasicDiscovery,
        .primary_subject_id = discovery_key,
        .secondary_subject_id = 0U,
    };
    const auto key_it = knowledge_index_by_semantic_key_.find(key);
    if (key_it == knowledge_index_by_semantic_key_.end() || key_it->second >= state_.agent_knowledge.size()) {
        return false;
    }
    if (state_.agent_knowledge[key_it->second].confidence_per_10k == 0U) {
        return false;
    }

    const auto* fact = std::get_if<BasicDiscoveryKnowledgeFact>(&state_.agent_knowledge[key_it->second].fact);
    if (fact == nullptr) {
        return false;
    }
    return fact->discovery_key == discovery_key;
}

std::vector<std::byte> Simulation::save_snapshot() const {
    return SnapshotCodec::serialize(*this);
}

SnapshotLoadResult Simulation::load_snapshot(const std::span<const std::byte> data, Simulation& destination) {
    return SnapshotCodec::deserialize(data, destination);
}

std::uint64_t Simulation::current_demographic_year() const noexcept {
    return DemographicCalendarAdapter::demographic_year(state_.time);
}

std::uint64_t Simulation::current_food_period() const noexcept {
    if (state_.time.ticks <= 0) {
        return 0;
    }

    const auto day_ticks = days(1).ticks;
    return static_cast<std::uint64_t>(state_.time.ticks / day_ticks);
}

void Simulation::advance(const SimulationDuration duration) {
    if (duration.ticks < 0) {
        throw std::invalid_argument("Simulation duration must be non-negative");
    }

    SimulationTime target_time{};
    if (!checked_add_time(state_.time, duration, target_time)) {
        throw std::overflow_error("Simulation time overflow");
    }

    run_scheduled_systems_until(target_time);
    state_.time = target_time;
    state_.year = current_demographic_year();
}

void Simulation::run_scheduled_systems_until(const SimulationTime target_time) {
    while (true) {
        SimulationTime next_time = demography_next_run_;
        if (config_.food_system_enabled && food_next_run_ < next_time) {
            next_time = food_next_run_;
        }

        if (next_time > target_time) {
            break;
        }

        state_.time = next_time;
        state_.year = current_demographic_year();

        if (config_.food_system_enabled && food_next_run_ <= next_time) {
            process_food_period(current_food_period());

            SimulationTime next_food_run{};
            if (!checked_add_time(food_next_run_, food_interval_, next_food_run)) {
                throw std::overflow_error("Scheduled food system time overflow");
            }
            food_next_run_ = next_food_run;
        }

        if (demography_next_run_ <= next_time) {
            process_demography_period(state_.year);

            SimulationTime next_run{};
            if (!checked_add_time(demography_next_run_, demography_interval_, next_run)) {
                throw std::overflow_error("Scheduled demography time overflow");
            }
            demography_next_run_ = next_run;
        }
    }
}

void Simulation::process_food_period(const std::uint64_t food_period) {
    process_household_access_allocation();

    struct HouseholdDayState {
        Household* household{};
        std::vector<Person*> alive_members{};
        std::vector<std::int64_t> per_person_required{};
        std::int64_t required_food{};
        std::int64_t cultivation_labour_available{};
        std::int64_t gathering_labour_available{};
        std::int64_t cultivation_labour_allocated{};
        std::int64_t gathering_labour_allocated{};
        std::int64_t produced_food{};
    };

    std::vector<HouseholdDayState> household_states;
    household_states.reserve(state_.households.size());

    std::unordered_map<PersonId, Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<SettlementId, Settlement*> settlement_by_id;
    settlement_by_id.reserve(state_.settlements.size());
    for (auto& settlement : state_.settlements) {
        settlement_by_id.emplace(settlement.id, &settlement);
    }

    for (auto& settlement : state_.settlements) {
        settlement.last_tool_coverage_per_10k = 0U;
        settlement.last_transfer_food_volume = 0;
        settlement.last_exchange_volume = 0;
    }

    for (auto& household : state_.households) {
        HouseholdDayState household_state{};
        household_state.household = &household;
        household_state.alive_members.reserve(household.member_ids.size());
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end()) {
                continue;
            }

            auto* person = person_it->second;
            if (person->alive) {
                household_state.alive_members.push_back(person);
            }
        }

        std::sort(household_state.alive_members.begin(), household_state.alive_members.end(), [](const Person* lhs, const Person* rhs) {
            return lhs->id < rhs->id;
        });

        household_state.per_person_required.reserve(household_state.alive_members.size());
        for (const auto* person : household_state.alive_members) {
            const auto person_required = food_requirement_for_person(*person);
            if (!checked_add_i64(household_state.required_food, person_required, household_state.required_food)) {
                throw std::overflow_error("Food accounting overflow");
            }

            if (occupation_eligible_for_productive_work(*person)) {
                const auto person_capacity = productive_effort_for_person(*person);
                if (person->occupation == Occupation::SubsistenceCultivator) {
                    if (!checked_add_i64(
                            household_state.cultivation_labour_available,
                            person_capacity,
                            household_state.cultivation_labour_available)) {
                        throw std::overflow_error("Cultivation labour accounting overflow");
                    }
                } else if (person->occupation == Occupation::WildFoodGatherer) {
                    if (!checked_add_i64(
                            household_state.gathering_labour_available,
                            person_capacity,
                            household_state.gathering_labour_available)) {
                        throw std::overflow_error("Gathering labour accounting overflow");
                    }
                }
            }

            household_state.per_person_required.push_back(person_required);
        }

        household_states.push_back(std::move(household_state));
    }

    std::sort(household_states.begin(), household_states.end(), [](const HouseholdDayState& lhs, const HouseholdDayState& rhs) {
        return lhs.household->id < rhs.household->id;
    });

    std::unordered_map<SettlementId, std::vector<HouseholdDayState*>> household_states_by_settlement;
    household_states_by_settlement.reserve(state_.settlements.size());
    for (auto& household_state : household_states) {
        household_states_by_settlement[household_state.household->settlement_id].push_back(&household_state);
    }

    state_.eligible_workers = 0;
    state_.active_workers = 0;
    state_.unallocated_workers = 0;
    state_.cultivators = 0;
    state_.gatherers = 0;
    state_.wood_gatherers = 0;
    state_.stone_extractors = 0;
    state_.craft_workers = 0;
    state_.total_labour_capacity = 0;
    state_.total_labour_demand = 0;
    state_.total_labour_allocated = 0;
    state_.cultivation_labour_available = 0;
    state_.cultivation_labour_allocated = 0;
    state_.gathering_labour_available = 0;
    state_.gathering_labour_allocated = 0;
    state_.labour_shortfall = 0;
    state_.wood_labour_available = 0;
    state_.stone_labour_available = 0;
    state_.craft_labour_available = 0;
    state_.wood_labour_available = 0;
    state_.stone_labour_available = 0;
    state_.craft_labour_available = 0;

    for (const auto& person : state_.people) {
        if (!occupation_eligible_for_productive_work(person)) {
            continue;
        }

        const auto capacity = productive_effort_for_person(person);
        ++state_.eligible_workers;
        if (!checked_add_i64(state_.total_labour_capacity, capacity, state_.total_labour_capacity)) {
            throw std::overflow_error("Total labour capacity overflow");
        }

        if (person.occupation == Occupation::SubsistenceCultivator) {
            ++state_.active_workers;
            ++state_.cultivators;
            if (!checked_add_i64(state_.cultivation_labour_available, capacity, state_.cultivation_labour_available)) {
                throw std::overflow_error("Cultivation available labour overflow");
            }
        } else if (person.occupation == Occupation::WildFoodGatherer) {
            ++state_.active_workers;
            ++state_.gatherers;
            if (!checked_add_i64(state_.gathering_labour_available, capacity, state_.gathering_labour_available)) {
                throw std::overflow_error("Gathering available labour overflow");
            }
        } else if (person.occupation == Occupation::WoodGatherer) {
            ++state_.active_workers;
            ++state_.wood_gatherers;
            if (!checked_add_i64(state_.wood_labour_available, capacity, state_.wood_labour_available)) {
                throw std::overflow_error("Wood available labour overflow");
            }
        } else if (person.occupation == Occupation::StoneExtractor) {
            ++state_.active_workers;
            ++state_.stone_extractors;
            if (!checked_add_i64(state_.stone_labour_available, capacity, state_.stone_labour_available)) {
                throw std::overflow_error("Stone available labour overflow");
            }
        } else if (person.occupation == Occupation::CraftWorker) {
            ++state_.active_workers;
            ++state_.craft_workers;
            if (!checked_add_i64(state_.craft_labour_available, capacity, state_.craft_labour_available)) {
                throw std::overflow_error("Craft available labour overflow");
            }
        } else {
            ++state_.unallocated_workers;
        }
    }

    for (auto& region : state_.regions) {
        std::vector<SettlementId> active_settlement_ids;
        active_settlement_ids.reserve(state_.settlements.size());
        for (const auto& settlement : state_.settlements) {
            if (settlement.active && settlement.region_id == region.id && settlement.access_share_points > 0U) {
                active_settlement_ids.push_back(settlement.id);
            }
        }
        std::sort(active_settlement_ids.begin(), active_settlement_ids.end());

        auto productive_available = apply_scaled_percent(region.productive_capacity_per_day, config_.food_production_scale_percent);
        const auto productive_seed = derive_seed(region_seed(region.id), SeedDomain::RegionDailyVariation, food_period);
        const auto productive_variation_percent = 85U + static_cast<std::uint32_t>(seed_random_bounded(productive_seed, 31U));
        productive_available = apply_scaled_percent(productive_available, productive_variation_percent);

        auto wild_harvest_available = std::min(region.wild_food_stock, region.wild_food_harvest_limit_per_day);
        const auto wild_seed = derive_seed(region_seed(region.id), SeedDomain::RegionWildHarvest, food_period);
        const auto wild_variation_percent = 80U + static_cast<std::uint32_t>(seed_random_bounded(wild_seed, 41U));
        wild_harvest_available = apply_scaled_percent(wild_harvest_available, wild_variation_percent);
        wild_harvest_available = std::min(wild_harvest_available, region.wild_food_stock);

        std::int64_t distributed_wild = 0;
        auto productive_remaining = productive_available;
        auto wild_harvest_remaining = wild_harvest_available;

        for (const auto settlement_id : active_settlement_ids) {
            const auto settlement_it = settlement_by_id.find(settlement_id);
            auto* settlement = (settlement_it != settlement_by_id.end()) ? settlement_it->second : nullptr;
            if (settlement == nullptr || settlement->access_share_points == 0U) {
                continue;
            }

            const auto grouped_households_it = household_states_by_settlement.find(settlement_id);
            if (grouped_households_it == household_states_by_settlement.end()) {
                continue;
            }

            const auto& settlement_households = grouped_households_it->second;
            std::int64_t settlement_required_food = 0;
            std::int64_t settlement_stored_food = 0;
            std::int64_t settlement_cultivation_available = 0;
            std::int64_t settlement_gathering_available = 0;
            std::uint64_t settlement_productive_workers = 0;
            bool has_region_households = false;
            for (auto* household_state : settlement_households) {
                if (household_state->household->region_id != region.id) {
                    continue;
                }
                has_region_households = true;

                if (!checked_add_i64(settlement_required_food, household_state->required_food, settlement_required_food) ||
                    !checked_add_i64(settlement_stored_food, household_state->household->resources.food_inventory.value, settlement_stored_food) ||
                    !checked_add_i64(
                        settlement_cultivation_available,
                        household_state->cultivation_labour_available,
                        settlement_cultivation_available) ||
                    !checked_add_i64(
                        settlement_gathering_available,
                        household_state->gathering_labour_available,
                        settlement_gathering_available)) {
                    throw std::overflow_error("Settlement labour/demand aggregation overflow");
                }

                for (const auto* person : household_state->alive_members) {
                    if (!occupation_eligible_for_productive_work(*person)) {
                        continue;
                    }
                    if (is_productive_occupation(person->occupation)) {
                        ++settlement_productive_workers;
                    }
                }
            }

            if (!has_region_households) {
                continue;
            }

            auto& tool_inventory = settlement->materials.basic_tool.value;
            const auto tools_used = std::min<std::uint64_t>(
                settlement_productive_workers,
                tool_inventory > 0 ? static_cast<std::uint64_t>(tool_inventory) : 0U);

            if (tools_used > 0U) {
                if (!checked_add_i64(
                        settlement->tool_wear_work_units,
                        static_cast<std::int64_t>(tools_used),
                        settlement->tool_wear_work_units)) {
                    throw std::overflow_error("Tool wear accumulation overflow");
                }

                auto broken_tools = settlement->tool_wear_work_units / kToolWearWorkUnitsPerBreak;
                settlement->tool_wear_work_units %= kToolWearWorkUnitsPerBreak;
                broken_tools = std::min<std::int64_t>(broken_tools, std::max<std::int64_t>(0, tool_inventory));

                if (broken_tools > 0) {
                    if (!checked_sub_nonnegative_i64(tool_inventory, broken_tools, tool_inventory) ||
                        !checked_add_i64(
                            settlement->cumulative_basic_tool_broken,
                            broken_tools,
                            settlement->cumulative_basic_tool_broken)) {
                        throw std::overflow_error("Tool breakage accounting overflow");
                    }
                }
            }

            settlement->last_tool_coverage_per_10k =
                tool_coverage_per_10k(tool_inventory, settlement_productive_workers);
            const auto output_scale_percent = tool_output_scale_percent(settlement->last_tool_coverage_per_10k);

            auto settlement_productive_budget =
                (productive_available * static_cast<std::int64_t>(settlement->access_share_points)) /
                static_cast<std::int64_t>(kSettlementShareScale);
            auto settlement_wild_budget =
                (wild_harvest_available * static_cast<std::int64_t>(settlement->access_share_points)) /
                static_cast<std::int64_t>(kSettlementShareScale);

            settlement_productive_budget = apply_scaled_percent(settlement_productive_budget, output_scale_percent);
            settlement_wild_budget = apply_scaled_percent(settlement_wild_budget, output_scale_percent);
            settlement_productive_budget = std::min(
                std::max<std::int64_t>(0, settlement_productive_budget),
                std::max<std::int64_t>(0, productive_remaining));
            settlement_wild_budget = std::min(
                std::max<std::int64_t>(0, settlement_wild_budget),
                std::max<std::int64_t>(0, wild_harvest_remaining));

            const auto output_target = compute_daily_food_output_target(
                settlement_required_food,
                settlement_stored_food,
                settlement_productive_budget + settlement_wild_budget);
            const auto labour_demand = split_demand_by_opportunity(
                output_target,
                settlement_productive_budget,
                settlement_wild_budget);

            if (!checked_add_i64(state_.total_labour_demand, labour_demand.total(), state_.total_labour_demand)) {
                throw std::overflow_error("Total labour demand overflow");
            }

            const auto settlement_cultivation_allocated =
                std::min(settlement_cultivation_available, std::max<std::int64_t>(0, labour_demand.cultivation_labour_demand));
            const auto settlement_gathering_allocated =
                std::min(settlement_gathering_available, std::max<std::int64_t>(0, labour_demand.gathering_labour_demand));

            if (!checked_add_i64(
                    state_.cultivation_labour_allocated,
                    settlement_cultivation_allocated,
                    state_.cultivation_labour_allocated) ||
                !checked_add_i64(
                    state_.gathering_labour_allocated,
                    settlement_gathering_allocated,
                    state_.gathering_labour_allocated)) {
                throw std::overflow_error("Allocated labour aggregate overflow");
            }

            std::int64_t distributed_cultivation_allocation = 0;
            for (auto* household_state : settlement_households) {
                if (settlement_cultivation_allocated <= 0 || settlement_cultivation_available <= 0 ||
                    household_state->cultivation_labour_available <= 0) {
                    continue;
                }
                household_state->cultivation_labour_allocated =
                    (settlement_cultivation_allocated * household_state->cultivation_labour_available) /
                    settlement_cultivation_available;
                if (!checked_add_i64(
                        distributed_cultivation_allocation,
                        household_state->cultivation_labour_allocated,
                        distributed_cultivation_allocation)) {
                    throw std::overflow_error("Cultivation allocation distribution overflow");
                }
            }

            auto cultivation_remainder = settlement_cultivation_allocated - distributed_cultivation_allocation;
            for (auto* household_state : settlement_households) {
                if (cultivation_remainder <= 0) {
                    break;
                }
                if (household_state->cultivation_labour_available <= 0) {
                    continue;
                }
                ++household_state->cultivation_labour_allocated;
                --cultivation_remainder;
            }

            std::int64_t distributed_gathering_allocation = 0;
            for (auto* household_state : settlement_households) {
                if (settlement_gathering_allocated <= 0 || settlement_gathering_available <= 0 ||
                    household_state->gathering_labour_available <= 0) {
                    continue;
                }
                household_state->gathering_labour_allocated =
                    (settlement_gathering_allocated * household_state->gathering_labour_available) /
                    settlement_gathering_available;
                if (!checked_add_i64(
                        distributed_gathering_allocation,
                        household_state->gathering_labour_allocated,
                        distributed_gathering_allocation)) {
                    throw std::overflow_error("Gathering allocation distribution overflow");
                }
            }

            auto gathering_remainder = settlement_gathering_allocated - distributed_gathering_allocation;
            for (auto* household_state : settlement_households) {
                if (gathering_remainder <= 0) {
                    break;
                }
                if (household_state->gathering_labour_available <= 0) {
                    continue;
                }
                ++household_state->gathering_labour_allocated;
                --gathering_remainder;
            }

            std::int64_t settlement_cultivation_output = 0;
            std::int64_t settlement_gathering_output = 0;
            if (!checked_add_i64(
                    settlement_cultivation_output,
                    std::min(settlement_productive_budget, settlement_cultivation_allocated),
                    settlement_cultivation_output) ||
                !checked_add_i64(
                    settlement_gathering_output,
                    std::min(settlement_wild_budget, settlement_gathering_allocated),
                    settlement_gathering_output)) {
                throw std::overflow_error("Specialized output overflow");
            }

            std::int64_t distributed_productive = 0;
            for (auto* household_state : settlement_households) {
                if (settlement_cultivation_allocated <= 0 || household_state->cultivation_labour_allocated <= 0) {
                    continue;
                }
                const auto produced =
                    (settlement_cultivation_output * household_state->cultivation_labour_allocated) /
                    settlement_cultivation_allocated;
                household_state->produced_food += produced;
                if (!checked_add_i64(distributed_productive, produced, distributed_productive)) {
                    throw std::overflow_error("Settlement productive distribution overflow");
                }
            }

            auto productive_remainder = settlement_cultivation_output - distributed_productive;
            for (auto* household_state : settlement_households) {
                if (productive_remainder <= 0) {
                    break;
                }
                if (household_state->cultivation_labour_allocated <= 0) {
                    continue;
                }
                ++household_state->produced_food;
                --productive_remainder;
            }

            std::int64_t settlement_distributed_wild = 0;
            for (auto* household_state : settlement_households) {
                if (settlement_gathering_allocated <= 0 || household_state->gathering_labour_allocated <= 0) {
                    continue;
                }
                const auto wild_for_household =
                    (settlement_gathering_output * household_state->gathering_labour_allocated) /
                    settlement_gathering_allocated;
                household_state->produced_food += wild_for_household;
                if (!checked_add_i64(settlement_distributed_wild, wild_for_household, settlement_distributed_wild)) {
                    throw std::overflow_error("Settlement wild distribution overflow");
                }
            }

            auto wild_remainder = settlement_gathering_output - settlement_distributed_wild;
            for (auto* household_state : settlement_households) {
                if (wild_remainder <= 0) {
                    break;
                }
                if (household_state->gathering_labour_allocated <= 0) {
                    continue;
                }
                ++household_state->produced_food;
                --wild_remainder;
                ++settlement_distributed_wild;
            }

            if (!checked_add_i64(distributed_wild, settlement_distributed_wild, distributed_wild)) {
                throw std::overflow_error("Region wild distribution overflow");
            }

            if (!checked_sub_nonnegative_i64(
                    productive_remaining,
                    settlement_cultivation_output,
                    productive_remaining) ||
                !checked_sub_nonnegative_i64(
                    wild_harvest_remaining,
                    settlement_distributed_wild,
                    wild_harvest_remaining)) {
                throw std::overflow_error("Region capacity accounting underflow");
            }
        }

        region.wild_food_stock = std::max<std::int64_t>(0, region.wild_food_stock - distributed_wild);
        if (!checked_add_i64(region.cumulative_wild_food_harvested, distributed_wild, region.cumulative_wild_food_harvested)) {
            throw std::overflow_error("Region cumulative harvest overflow");
        }

        if (region.wild_food_stock < region.wild_food_capacity) {
            const auto deficit = region.wild_food_capacity - region.wild_food_stock;
            const auto regeneration = std::min(deficit, region.wild_food_regeneration_per_day);
            if (!checked_add_i64(region.wild_food_stock, regeneration, region.wild_food_stock) ||
                !checked_add_i64(region.cumulative_wild_food_regenerated, regeneration, region.cumulative_wild_food_regenerated)) {
                throw std::overflow_error("Region regeneration overflow");
            }
        }
    }

    if (!checked_add_i64(
            state_.cultivation_labour_allocated,
            state_.gathering_labour_allocated,
            state_.total_labour_allocated)) {
        throw std::overflow_error("Total allocated labour overflow");
    }
    state_.labour_shortfall = std::max<std::int64_t>(0, state_.total_labour_demand - state_.total_labour_allocated);

    for (auto& household_state : household_states) {
        auto& household = *household_state.household;
        const auto produced_food = household_state.produced_food;

        std::int64_t inventory_after_production = 0;
        if (!checked_add_i64(household.resources.food_inventory.value, produced_food, inventory_after_production)) {
            throw std::overflow_error("Food inventory overflow on production");
        }
        household.resources.food_inventory.value = inventory_after_production;

        if (!checked_add_i64(household.resources.food_produced_total, produced_food, household.resources.food_produced_total)) {
            throw std::overflow_error("Food produced total overflow");
        }
    }

    std::vector<SettlementId> grouped_settlement_ids;
    grouped_settlement_ids.reserve(household_states_by_settlement.size());
    for (const auto& [settlement_id, grouped_households] : household_states_by_settlement) {
        if (!grouped_households.empty()) {
            grouped_settlement_ids.push_back(settlement_id);
        }
    }
    std::sort(grouped_settlement_ids.begin(), grouped_settlement_ids.end());

    for (const auto settlement_id : grouped_settlement_ids) {
        const auto grouped_it = household_states_by_settlement.find(settlement_id);
        if (grouped_it == household_states_by_settlement.end()) {
            continue;
        }
        const auto& grouped_households = grouped_it->second;

        std::int64_t settlement_required = 0;
        std::int64_t settlement_inventory = 0;
        for (const auto* household_state : grouped_households) {
            if (!checked_add_i64(settlement_required, household_state->required_food, settlement_required) ||
                !checked_add_i64(
                    settlement_inventory,
                    household_state->household->resources.food_inventory.value,
                    settlement_inventory)) {
                throw std::overflow_error("Settlement pooling aggregation overflow");
            }
        }

        const auto settlement_consumed = std::min(settlement_inventory, settlement_required);
        const auto settlement_leftover = std::max<std::int64_t>(0, settlement_inventory - settlement_consumed);

        std::vector<std::int64_t> household_allocated(grouped_households.size(), 0);
        std::vector<std::int64_t> household_leftover(grouped_households.size(), 0);

        std::int64_t distributed_consumed = 0;
        for (std::size_t i = 0; i < grouped_households.size(); ++i) {
            const auto* household_state = grouped_households[i];
            std::int64_t allocated = 0;
            if (settlement_consumed > 0 && settlement_required > 0 && household_state->required_food > 0) {
                allocated = (settlement_consumed * household_state->required_food) / settlement_required;
            }
            household_allocated[i] = allocated;
            if (!checked_add_i64(distributed_consumed, allocated, distributed_consumed)) {
                throw std::overflow_error("Settlement pooled consumed distribution overflow");
            }
        }

        auto consumed_remainder = settlement_consumed - distributed_consumed;
        for (std::size_t i = 0; i < grouped_households.size(); ++i) {
            const auto* household_state = grouped_households[i];
            if (consumed_remainder <= 0) {
                break;
            }
            if (household_state->required_food <= 0) {
                continue;
            }
            ++household_allocated[i];
            --consumed_remainder;
        }

        std::int64_t distributed_leftover = 0;
        for (std::size_t i = 0; i < grouped_households.size(); ++i) {
            const auto* household_state = grouped_households[i];
            std::int64_t leftover = 0;
            if (settlement_leftover > 0 && settlement_inventory > 0 && household_state->household->resources.food_inventory.value > 0) {
                leftover =
                    (settlement_leftover * household_state->household->resources.food_inventory.value) /
                    settlement_inventory;
            }
            household_leftover[i] = leftover;
            if (!checked_add_i64(distributed_leftover, leftover, distributed_leftover)) {
                throw std::overflow_error("Settlement pooled leftover distribution overflow");
            }
        }

        auto leftover_remainder = settlement_leftover - distributed_leftover;
        for (std::size_t i = 0; i < grouped_households.size(); ++i) {
            const auto* household_state = grouped_households[i];
            if (leftover_remainder <= 0) {
                break;
            }
            if (household_state->household->resources.food_inventory.value <= 0) {
                continue;
            }
            ++household_leftover[i];
            --leftover_remainder;
        }

        for (std::size_t i = 0; i < grouped_households.size(); ++i) {
            auto* household_state = grouped_households[i];
            auto& household = *household_state->household;
            const auto consumed_food = household_allocated[i];
            const auto unmet_food = std::max<std::int64_t>(0, household_state->required_food - consumed_food);
            household.resources.food_inventory.value = std::max<std::int64_t>(0, household_leftover[i]);

            if (!checked_add_i64(household.resources.food_consumed_total, consumed_food, household.resources.food_consumed_total) ||
                !checked_add_i64(household.resources.unmet_food_total, unmet_food, household.resources.unmet_food_total)) {
                throw std::overflow_error("Food accounting totals overflow");
            }

            if (unmet_food > 0) {
                ++household.resources.consecutive_shortage_days;

                const auto delta = static_cast<std::int64_t>(shortage_stress_delta_per_day(household_state->required_food, unmet_food));
                std::int64_t new_stress = 0;
                if (!checked_add_i64(household.resources.nutritional_stress_points, delta, new_stress)) {
                    throw std::overflow_error("Food stress overflow");
                }
                household.resources.nutritional_stress_points = std::min<std::int64_t>(new_stress, food_parameters_.max_stress_points);
            } else {
                household.resources.consecutive_shortage_days = 0;
                household.resources.nutritional_stress_points = std::max<std::int64_t>(
                    0,
                    household.resources.nutritional_stress_points - food_parameters_.stress_recovery_per_day);
            }

            std::vector<std::int64_t> per_person_allocated(household_state->alive_members.size(), 0);
            if (consumed_food > 0 && household_state->required_food > 0 && !household_state->alive_members.empty()) {
                std::int64_t allocated_sum = 0;
                for (std::size_t member_index = 0; member_index < household_state->alive_members.size(); ++member_index) {
                    per_person_allocated[member_index] =
                        (consumed_food * household_state->per_person_required[member_index]) /
                        household_state->required_food;
                    if (!checked_add_i64(allocated_sum, per_person_allocated[member_index], allocated_sum)) {
                        throw std::overflow_error("Per-person food allocation overflow");
                    }
                }

                auto remainder = consumed_food - allocated_sum;
                for (std::size_t member_index = 0;
                     member_index < household_state->alive_members.size() && remainder > 0;
                     ++member_index) {
                    ++per_person_allocated[member_index];
                    --remainder;
                }
            }

            for (std::size_t member_index = 0; member_index < household_state->alive_members.size(); ++member_index) {
                apply_person_nutrition_and_health(
                    *household_state->alive_members[member_index],
                    household_state->per_person_required[member_index],
                    per_person_allocated[member_index]);
            }
        }
    }

    if (config_.materials_system_enabled) {
        process_materials_period(food_period);
        process_local_exchange(current_demographic_year());
    }

    process_household_stabilization_transfers();

    refresh_food_aggregates();
    refresh_environment_aggregates();
    refresh_material_aggregates();
    refresh_economic_aggregates();
}

void Simulation::process_materials_period(const std::uint64_t food_period) {
    state_.wood_labour_available = 0;
    state_.wood_labour_allocated = 0;
    state_.stone_labour_available = 0;
    state_.stone_labour_allocated = 0;
    state_.craft_labour_available = 0;
    state_.craft_labour_allocated = 0;
    state_.material_labour_demand = 0;
    state_.material_labour_allocated = 0;
    state_.material_labour_shortfall = 0;
    state_.material_production_shortfall = 0;

    struct SettlementMaterialDayState {
        Settlement* settlement{};
        std::uint64_t alive_population{};
        std::uint64_t eligible_workers{};
        std::int64_t wood_labour_available{};
        std::int64_t stone_labour_available{};
        std::int64_t craft_labour_available{};
        std::int64_t wood_quota{};
        std::int64_t stone_quota{};
    };

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<SettlementId, SettlementMaterialDayState> by_settlement;
    by_settlement.reserve(state_.settlements.size());
    for (auto& settlement : state_.settlements) {
        if (!settlement.active) {
            continue;
        }

        by_settlement.emplace(
            settlement.id,
            SettlementMaterialDayState{.settlement = &settlement});
    }

    for (const auto& household : state_.households) {
        auto settlement_it = by_settlement.find(household.settlement_id);
        if (settlement_it == by_settlement.end()) {
            continue;
        }

        auto& settlement_state = settlement_it->second;
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end() || !person_it->second->alive) {
                continue;
            }

            ++settlement_state.alive_population;
            const auto& person = *person_it->second;
            if (!occupation_eligible_for_productive_work(person)) {
                continue;
            }

            ++settlement_state.eligible_workers;
            const auto capacity = productive_effort_for_person(person);
            if (person.occupation == Occupation::WoodGatherer) {
                if (!checked_add_i64(
                        settlement_state.wood_labour_available,
                        capacity,
                        settlement_state.wood_labour_available)) {
                    throw std::overflow_error("Wood labour availability overflow");
                }
            } else if (person.occupation == Occupation::StoneExtractor) {
                if (!checked_add_i64(
                        settlement_state.stone_labour_available,
                        capacity,
                        settlement_state.stone_labour_available)) {
                    throw std::overflow_error("Stone labour availability overflow");
                }
            } else if (person.occupation == Occupation::CraftWorker) {
                if (!checked_add_i64(
                        settlement_state.craft_labour_available,
                        capacity,
                        settlement_state.craft_labour_available)) {
                    throw std::overflow_error("Craft labour availability overflow");
                }
            }
        }
    }

    std::vector<RegionId> region_ids;
    region_ids.reserve(state_.regions.size());
    for (const auto& region : state_.regions) {
        region_ids.push_back(region.id);
    }
    std::sort(region_ids.begin(), region_ids.end());

    for (const auto region_id : region_ids) {
        auto* region = find_region_mutable(region_id);
        if (region == nullptr) {
            continue;
        }

        std::vector<SettlementMaterialDayState*> active_settlements;
        active_settlements.reserve(by_settlement.size());
        std::int64_t total_share = 0;
        for (auto& [settlement_id, settlement_state] : by_settlement) {
            (void)settlement_id;
            if (settlement_state.settlement->region_id != region_id || !settlement_state.settlement->active ||
                settlement_state.settlement->access_share_points == 0U) {
                continue;
            }

            active_settlements.push_back(&settlement_state);
            total_share += settlement_state.settlement->access_share_points;
        }

        std::sort(active_settlements.begin(), active_settlements.end(), [](const SettlementMaterialDayState* lhs, const SettlementMaterialDayState* rhs) {
            return lhs->settlement->id < rhs->settlement->id;
        });

        if (active_settlements.empty() || total_share <= 0) {
            continue;
        }

        auto wood_available = std::max<std::int64_t>(0, region->wood_stock);
        auto stone_available = std::max<std::int64_t>(0, region->stone_remaining);

        const auto wood_daily_seed = derive_seed(region_seed(region_id), SeedDomain::RegionWoodDailyVariation, food_period);
        const auto stone_daily_seed = derive_seed(region_seed(region_id), SeedDomain::RegionStoneDailyVariation, food_period);
        wood_available = apply_scaled_percent(
            wood_available,
            85U + static_cast<std::uint32_t>(seed_random_bounded(wood_daily_seed, 31U)));
        stone_available = apply_scaled_percent(
            stone_available,
            85U + static_cast<std::uint32_t>(seed_random_bounded(stone_daily_seed, 31U)));
        wood_available = std::min(wood_available, region->wood_stock);
        stone_available = std::min(stone_available, region->stone_remaining);

        std::int64_t distributed_wood_quota = 0;
        std::int64_t distributed_stone_quota = 0;
        for (auto* settlement_state : active_settlements) {
            settlement_state->wood_quota =
                (wood_available * static_cast<std::int64_t>(settlement_state->settlement->access_share_points)) /
                static_cast<std::int64_t>(kSettlementMaterialShareScale);
            settlement_state->stone_quota =
                (stone_available * static_cast<std::int64_t>(settlement_state->settlement->access_share_points)) /
                static_cast<std::int64_t>(kSettlementMaterialShareScale);
            distributed_wood_quota += settlement_state->wood_quota;
            distributed_stone_quota += settlement_state->stone_quota;
        }

        auto wood_quota_remainder = wood_available - distributed_wood_quota;
        auto stone_quota_remainder = stone_available - distributed_stone_quota;
        for (auto* settlement_state : active_settlements) {
            if (wood_quota_remainder > 0) {
                ++settlement_state->wood_quota;
                --wood_quota_remainder;
            }
            if (stone_quota_remainder > 0) {
                ++settlement_state->stone_quota;
                --stone_quota_remainder;
            }
        }

        std::int64_t extracted_wood_region = 0;
        std::int64_t extracted_stone_region = 0;

        for (auto* settlement_state : active_settlements) {
            const auto targets = material_targets_for_settlement(
                settlement_state->alive_population,
                settlement_state->eligible_workers);
            const auto construction_demand =
                construction_demand_for_settlement(settlement_state->settlement->id, state_.buildings);

            auto& inventory = settlement_state->settlement->materials;
            std::int64_t timber_target_with_construction = 0;
            if (!checked_add_i64(targets.timber_target, construction_demand.timber_remaining, timber_target_with_construction)) {
                throw std::overflow_error("Timber target with construction overflow");
            }
            std::int64_t stone_target_with_construction = 0;
            if (!checked_add_i64(targets.stone_target, construction_demand.stone_remaining, stone_target_with_construction)) {
                throw std::overflow_error("Stone target with construction overflow");
            }
            const auto timber_deficit = std::max<std::int64_t>(0, timber_target_with_construction - inventory.timber.value);
            const auto tool_deficit = std::max<std::int64_t>(0, targets.basic_tool_target - inventory.basic_tool.value);
            const auto stone_deficit = std::max<std::int64_t>(0, stone_target_with_construction - inventory.stone.value);

            std::int64_t timber_wood_recipe_need = 0;
            std::int64_t tool_wood_recipe_need = 0;
            if (!checked_mul_i64(timber_deficit, material_parameters_.wood_per_timber, timber_wood_recipe_need) ||
                !checked_mul_i64(tool_deficit, material_parameters_.wood_per_basic_tool, tool_wood_recipe_need)) {
                throw std::overflow_error("Wood recipe need overflow");
            }
            std::int64_t wood_recipe_need = 0;
            if (!checked_add_i64(timber_wood_recipe_need, tool_wood_recipe_need, wood_recipe_need)) {
                throw std::overflow_error("Wood recipe need aggregation overflow");
            }

            std::int64_t stone_recipe_need = 0;
            if (!checked_mul_i64(tool_deficit, material_parameters_.stone_per_basic_tool, stone_recipe_need)) {
                throw std::overflow_error("Stone recipe need overflow");
            }

            const auto wood_reserve_need = std::max<std::int64_t>(0, targets.wood_target - inventory.wood.value);
            const auto stone_reserve_need = stone_deficit;
            std::int64_t wood_total_need = 0;
            if (!checked_add_i64(wood_recipe_need, wood_reserve_need, wood_total_need)) {
                throw std::overflow_error("Wood total need overflow");
            }
            std::int64_t stone_total_need = 0;
            if (!checked_add_i64(stone_recipe_need, stone_reserve_need, stone_total_need)) {
                throw std::overflow_error("Stone total need overflow");
            }

            const auto output_scale_percent =
                tool_output_scale_percent(settlement_state->settlement->last_tool_coverage_per_10k);
            const auto effective_wood_labour_available = apply_scaled_percent(
                std::max<std::int64_t>(0, settlement_state->wood_labour_available),
                output_scale_percent);
            const auto effective_stone_labour_available = apply_scaled_percent(
                std::max<std::int64_t>(0, settlement_state->stone_labour_available),
                output_scale_percent);

            const auto extracted_wood = safe_min3_i64(
                std::max<std::int64_t>(0, wood_total_need),
                std::max<std::int64_t>(0, settlement_state->wood_quota),
                effective_wood_labour_available);
            const auto extracted_stone = safe_min3_i64(
                std::max<std::int64_t>(0, stone_total_need),
                std::max<std::int64_t>(0, settlement_state->stone_quota),
                effective_stone_labour_available);

            if (!checked_sub_nonnegative_i64(region->wood_stock, extracted_wood, region->wood_stock) ||
                !checked_sub_nonnegative_i64(region->stone_remaining, extracted_stone, region->stone_remaining)) {
                throw std::overflow_error("Material extraction exceeded region stock");
            }

            if (!checked_add_i64(region->cumulative_wood_harvested, extracted_wood, region->cumulative_wood_harvested) ||
                !checked_add_i64(region->cumulative_stone_extracted, extracted_stone, region->cumulative_stone_extracted) ||
                !checked_add_i64(inventory.wood.value, extracted_wood, inventory.wood.value) ||
                !checked_add_i64(inventory.stone.value, extracted_stone, inventory.stone.value) ||
                !checked_add_i64(
                    settlement_state->settlement->cumulative_wood_extracted,
                    extracted_wood,
                    settlement_state->settlement->cumulative_wood_extracted) ||
                !checked_add_i64(
                    settlement_state->settlement->cumulative_stone_extracted,
                    extracted_stone,
                    settlement_state->settlement->cumulative_stone_extracted)) {
                throw std::overflow_error("Material extraction accounting overflow");
            }

            if (!checked_add_i64(extracted_wood_region, extracted_wood, extracted_wood_region) ||
                !checked_add_i64(extracted_stone_region, extracted_stone, extracted_stone_region) ||
                !checked_add_i64(state_.wood_labour_allocated, extracted_wood, state_.wood_labour_allocated) ||
                !checked_add_i64(state_.stone_labour_allocated, extracted_stone, state_.stone_labour_allocated)) {
                throw std::overflow_error("Material labour accounting overflow");
            }

            auto craft_labour_remaining = apply_scaled_percent(
                std::max<std::int64_t>(0, settlement_state->craft_labour_available),
                config_.material_crafting_scale_percent);
            craft_labour_remaining = apply_scaled_percent(craft_labour_remaining, output_scale_percent);
            if (!checked_add_i64(
                    craft_labour_remaining,
                    std::max<std::int64_t>(0, settlement_state->settlement->workshop_capacity_bonus),
                    craft_labour_remaining)) {
                throw std::overflow_error("Workshop bonus labour overflow");
            }

            const auto craft_demand_total = timber_deficit + tool_deficit;
            std::int64_t material_labour_increment = 0;
            if (!checked_add_i64(craft_demand_total, extracted_wood, material_labour_increment) ||
                !checked_add_i64(material_labour_increment, extracted_stone, material_labour_increment) ||
                !checked_add_i64(
                    state_.material_labour_demand,
                    material_labour_increment,
                    state_.material_labour_demand)) {
                throw std::overflow_error("Material labour demand overflow");
            }

            const auto tools_possible = safe_min3_i64(
                tool_deficit,
                craft_labour_remaining / std::max<std::int64_t>(1, material_parameters_.craft_labour_per_basic_tool),
                safe_min3_i64(
                    inventory.wood.value / std::max<std::int64_t>(1, material_parameters_.wood_per_basic_tool),
                    inventory.stone.value / std::max<std::int64_t>(1, material_parameters_.stone_per_basic_tool),
                    std::numeric_limits<std::int64_t>::max()));

            if (tools_possible > 0) {
                inventory.wood.value -= tools_possible * material_parameters_.wood_per_basic_tool;
                inventory.stone.value -= tools_possible * material_parameters_.stone_per_basic_tool;
                if (!checked_add_i64(inventory.basic_tool.value, tools_possible, inventory.basic_tool.value)) {
                    throw std::overflow_error("Basic tool inventory overflow");
                }
                if (!checked_add_i64(
                        settlement_state->settlement->cumulative_basic_tool_produced,
                        tools_possible,
                        settlement_state->settlement->cumulative_basic_tool_produced)) {
                    throw std::overflow_error("Basic tool cumulative production overflow");
                }
                craft_labour_remaining -= tools_possible * material_parameters_.craft_labour_per_basic_tool;
            }

            const auto timber_possible = safe_min3_i64(
                timber_deficit,
                craft_labour_remaining / std::max<std::int64_t>(1, material_parameters_.craft_labour_per_timber),
                inventory.wood.value / std::max<std::int64_t>(1, material_parameters_.wood_per_timber));

            if (timber_possible > 0) {
                inventory.wood.value -= timber_possible * material_parameters_.wood_per_timber;
                if (!checked_add_i64(inventory.timber.value, timber_possible, inventory.timber.value)) {
                    throw std::overflow_error("Timber inventory overflow");
                }
                if (!checked_add_i64(
                        settlement_state->settlement->cumulative_timber_produced,
                        timber_possible,
                        settlement_state->settlement->cumulative_timber_produced)) {
                    throw std::overflow_error("Timber cumulative production overflow");
                }
                craft_labour_remaining -= timber_possible * material_parameters_.craft_labour_per_timber;
            }

            const auto used_craft_labour =
                (tools_possible * material_parameters_.craft_labour_per_basic_tool) +
                (timber_possible * material_parameters_.craft_labour_per_timber);
            if (!checked_add_i64(
                    state_.craft_labour_allocated,
                    used_craft_labour,
                    state_.craft_labour_allocated)) {
                throw std::overflow_error("Craft labour allocation overflow");
            }

            const auto production_shortfall =
                std::max<std::int64_t>(0, tool_deficit - tools_possible) +
                std::max<std::int64_t>(0, timber_deficit - timber_possible);
            if (!checked_add_i64(
                    state_.material_production_shortfall,
                    production_shortfall,
                    state_.material_production_shortfall)) {
                throw std::overflow_error("Material production shortfall overflow");
            }
        }

        if (region->wood_stock < region->wood_capacity) {
            const auto deficit = region->wood_capacity - region->wood_stock;
            const auto regeneration = std::min(deficit, std::max<std::int64_t>(0, region->wood_regeneration_per_day));
            if (!checked_add_i64(region->wood_stock, regeneration, region->wood_stock) ||
                !checked_add_i64(region->cumulative_wood_regenerated, regeneration, region->cumulative_wood_regenerated)) {
                throw std::overflow_error("Wood regeneration overflow");
            }
        }

        (void)extracted_wood_region;
        (void)extracted_stone_region;
    }

    for (const auto& [settlement_id, settlement_state] : by_settlement) {
        (void)settlement_id;
        if (!checked_add_i64(
                state_.wood_labour_available,
                settlement_state.wood_labour_available,
                state_.wood_labour_available) ||
            !checked_add_i64(
                state_.stone_labour_available,
                settlement_state.stone_labour_available,
                state_.stone_labour_available) ||
            !checked_add_i64(
                state_.craft_labour_available,
                settlement_state.craft_labour_available,
                state_.craft_labour_available)) {
            throw std::overflow_error("Material labour availability aggregate overflow");
        }
    }

    state_.material_labour_allocated =
        state_.wood_labour_allocated + state_.stone_labour_allocated + state_.craft_labour_allocated;
    state_.material_labour_shortfall =
        std::max<std::int64_t>(0, state_.material_labour_demand - state_.material_labour_allocated);
}

void Simulation::process_demography_period(const std::uint64_t demographic_year) {
    process_mortality(demographic_year);
    process_births(demographic_year);
    process_settlement_restructuring(demographic_year);
    process_basic_discoveries(demographic_year);
    process_communication_teaching(demographic_year);
    process_records(demographic_year);
    process_institutions(demographic_year);
    process_knowledge_loss(demographic_year);
    process_household_access_allocation();
    process_local_exchange(demographic_year);
    process_construction(demographic_year);
    process_local_exchange(demographic_year);
    process_occupation_reassignment(demographic_year);
    process_household_access_allocation();
    refresh_population_count();
    refresh_food_aggregates();
    refresh_labour_aggregates();
    refresh_material_aggregates();
    refresh_economic_aggregates();
}

void Simulation::process_basic_discoveries(const std::uint64_t demographic_year) {
    std::unordered_set<std::uint32_t> discovered_keys;
    discovered_keys.reserve(kBasicDiscoveryKeys.size());
    for (const auto& event : state_.event_log) {
        if (event.type != EventType::DiscoveryMade) {
            continue;
        }
        const auto* payload = std::get_if<DiscoveryMadeEventPayload>(&event.payload);
        if (payload == nullptr || payload->discovery_key == 0U) {
            continue;
        }
        discovered_keys.insert(payload->discovery_key);
    }

    if (discovered_keys.size() >= kBasicDiscoveryKeys.size()) {
        return;
    }

    std::unordered_map<HouseholdId, SettlementId> settlement_by_household;
    settlement_by_household.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        settlement_by_household.emplace(household.id, household.settlement_id);
    }

    std::vector<SettlementId> settlement_ids;
    settlement_ids.reserve(state_.settlements.size());
    for (const auto& settlement : state_.settlements) {
        if (settlement.active) {
            settlement_ids.push_back(settlement.id);
        }
    }
    std::sort(settlement_ids.begin(), settlement_ids.end());

    for (const auto settlement_id : settlement_ids) {
        if (discovered_keys.size() >= kBasicDiscoveryKeys.size()) {
            break;
        }

        const auto attempt_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::DiscoveryAttempt,
            demographic_year);
        if (!seed_chance_per_10k(attempt_seed, kDiscoveryAttemptPerSettlementPer10k)) {
            continue;
        }

        std::vector<std::uint32_t> undiscovered_keys;
        undiscovered_keys.reserve(kBasicDiscoveryKeys.size());
        for (const auto key : kBasicDiscoveryKeys) {
            if (!discovered_keys.contains(key)) {
                undiscovered_keys.push_back(key);
            }
        }
        if (undiscovered_keys.empty()) {
            continue;
        }

        const auto key_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::DiscoveryKeySelection,
            demographic_year);
        const auto key_index = static_cast<std::size_t>(seed_random_bounded(key_seed, undiscovered_keys.size()));
        const auto discovery_key = undiscovered_keys[key_index];

        std::vector<PersonId> discoverer_candidates;
        for (const auto& person : state_.people) {
            if (!person.alive || age_years(person) < kAdultAgeYears) {
                continue;
            }
            const auto settlement_it = settlement_by_household.find(person.household_id);
            if (settlement_it == settlement_by_household.end() || settlement_it->second != settlement_id) {
                continue;
            }
            discoverer_candidates.push_back(person.id);
        }

        if (discoverer_candidates.empty()) {
            continue;
        }

        std::sort(discoverer_candidates.begin(), discoverer_candidates.end());
        const auto discoverer_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::DiscoveryPersonSelection,
            demographic_year ^ discovery_key);
        const auto discoverer_index = static_cast<std::size_t>(
            seed_random_bounded(discoverer_seed, discoverer_candidates.size()));
        const auto discoverer_id = discoverer_candidates[discoverer_index];

        const auto event_id = emit_discovery_made_event(discovery_key, discoverer_id, settlement_id);
        const std::array<PersonId, 1> observer_ids{discoverer_id};
        append_direct_observation_memories(event_id, observer_ids, state_.time);
        discovered_keys.insert(discovery_key);
    }
}

void Simulation::process_communication_teaching(const std::uint64_t demographic_year) {
    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<HouseholdId, SettlementId> settlement_by_household;
    settlement_by_household.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        settlement_by_household.emplace(household.id, household.settlement_id);
    }

    std::vector<PersonId> recipient_ids;
    recipient_ids.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        if (person.alive) {
            recipient_ids.push_back(person.id);
        }
    }
    std::sort(recipient_ids.begin(), recipient_ids.end());

    for (const auto recipient_id : recipient_ids) {
        const auto recipient_it = person_by_id.find(recipient_id);
        if (recipient_it == person_by_id.end()) {
            continue;
        }
        const auto* recipient = recipient_it->second;
        if (recipient == nullptr || !recipient->alive) {
            continue;
        }

        std::vector<PersonId> household_candidates;
        std::vector<PersonId> settlement_candidates;
        const auto recipient_settlement_it = settlement_by_household.find(recipient->household_id);
        if (recipient_settlement_it == settlement_by_household.end()) {
            continue;
        }
        const auto recipient_settlement_id = recipient_settlement_it->second;
        for (const auto& candidate : state_.people) {
            if (!candidate.alive || candidate.id == recipient_id) {
                continue;
            }
            if (knowledge_count_for_person(candidate.id) == 0U) {
                continue;
            }

            if (candidate.household_id == recipient->household_id) {
                household_candidates.push_back(candidate.id);
                continue;
            }

            if (candidate.household_id == 0U || candidate.household_id == recipient->household_id) {
                continue;
            }

            const auto candidate_settlement_it = settlement_by_household.find(candidate.household_id);
            if (candidate_settlement_it == settlement_by_household.end()) {
                continue;
            }

            if (recipient_settlement_id == candidate_settlement_it->second) {
                settlement_candidates.push_back(candidate.id);
            }
        }

        const auto& teacher_candidates = household_candidates.empty() ? settlement_candidates : household_candidates;
        if (teacher_candidates.empty()) {
            continue;
        }

        const auto selection_base_seed = derive_seed(
            person_seed(recipient_id),
            SeedDomain::CommunicationTeacherSelection,
            demographic_year);

        const auto teacher_index = static_cast<std::size_t>(
            seed_random_bounded(selection_base_seed, teacher_candidates.size()));
        const auto teacher_id = teacher_candidates[teacher_index];
        const bool same_household = !household_candidates.empty();

        const auto attempt_seed = derive_seed(
            selection_base_seed,
            SeedDomain::CommunicationAttempt,
            teacher_id);
        const auto teaching_chance = same_household
            ? kTeachingChanceSameHouseholdPer10k
            : kTeachingChanceSameSettlementPer10k;
        const auto communication_chance = same_household
            ? kCommunicationChanceSameHouseholdPer10k
            : kCommunicationChanceSameSettlementPer10k;

        const bool teaching = seed_chance_per_10k(attempt_seed, teaching_chance);
        const auto communication_seed = derive_seed(
            attempt_seed,
            SeedDomain::CommunicationAttempt,
            demographic_year + recipient_id + teacher_id);
        const bool communication = teaching || seed_chance_per_10k(communication_seed, communication_chance);
        if (!communication) {
            continue;
        }

        auto teacher_knowledge = knowledge_for_person(teacher_id);
        if (teacher_knowledge.empty()) {
            continue;
        }
        std::sort(teacher_knowledge.begin(), teacher_knowledge.end(), [](const AgentKnowledge& lhs, const AgentKnowledge& rhs) {
            return lhs.id < rhs.id;
        });

        std::vector<AgentKnowledge> transmissible_knowledge;
        transmissible_knowledge.reserve(teacher_knowledge.size());
        for (const auto& knowledge : teacher_knowledge) {
            if (knowledge.source_memory_id == 0U) {
                continue;
            }

            const TransmissionSemanticKey semantic_key{
                .recipient_id = recipient_id,
                .speaker_memory_id = knowledge.source_memory_id,
            };
            if (transmission_semantic_keys_.contains(semantic_key)) {
                continue;
            }

            transmissible_knowledge.push_back(knowledge);
        }

        if (transmissible_knowledge.empty()) {
            continue;
        }

        const auto knowledge_seed = derive_seed(
            person_seed(recipient_id),
            SeedDomain::CommunicationKnowledgeSelection,
            (demographic_year * 1'000'003ULL) ^ teacher_id);
        const auto knowledge_index = static_cast<std::size_t>(
            seed_random_bounded(knowledge_seed, transmissible_knowledge.size()));
        const auto& selected_knowledge = transmissible_knowledge[knowledge_index];

        EventId source_event_id = 0U;
        for (const auto& memory : state_.agent_memories) {
            if (memory.id == selected_knowledge.source_memory_id) {
                source_event_id = memory.source_event_id;
                break;
            }
        }

        if (source_event_id == 0U) {
            continue;
        }

        const auto confidence_cap = teaching ? kTeachingConfidenceCapPer10k : kCommunicationConfidenceCapPer10k;
        const auto confidence = std::min<std::uint32_t>(selected_knowledge.confidence_per_10k, confidence_cap);
        const auto teacher_it = person_by_id.find(teacher_id);
        if (teacher_it == person_by_id.end() || teacher_it->second == nullptr) {
            continue;
        }
        const auto speaker_settlement_it = settlement_by_household.find(teacher_it->second->household_id);
        if (speaker_settlement_it == settlement_by_household.end()) {
            continue;
        }
        const auto recipient_memory_id = append_memory(AgentMemory{
            .id = 0,
            .owner_id = recipient_id,
            .acquired_at = state_.time,
            .source = teaching ? MemorySource::Teaching : MemorySource::Communication,
            .source_event_id = source_event_id,
            .confidence_per_10k = confidence,
        });

        append_knowledge_transmission(KnowledgeTransmission{
            .id = 0,
            .speaker_id = teacher_id,
            .recipient_id = recipient_id,
            .speaker_household_id = teacher_it->second->household_id,
            .recipient_household_id = recipient->household_id,
            .speaker_settlement_id = speaker_settlement_it->second,
            .recipient_settlement_id = recipient_settlement_id,
            .transmitted_at = state_.time,
            .method = teaching ? CommunicationMethod::Teaching : CommunicationMethod::Communication,
            .speaker_memory_id = selected_knowledge.source_memory_id,
            .recipient_memory_id = recipient_memory_id,
            .confidence_per_10k = confidence,
        });
    }
}

void Simulation::process_records(const std::uint64_t demographic_year) {
    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<HouseholdId, SettlementId> settlement_by_household;
    settlement_by_household.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        settlement_by_household.emplace(household.id, household.settlement_id);
    }

    std::vector<SettlementId> settlement_ids;
    settlement_ids.reserve(state_.settlements.size());
    for (const auto& settlement : state_.settlements) {
        if (settlement.active) {
            settlement_ids.push_back(settlement.id);
        }
    }
    std::sort(settlement_ids.begin(), settlement_ids.end());

    for (const auto settlement_id : settlement_ids) {
        const auto attempt_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::RecordAttempt,
            demographic_year);
        if (!seed_chance_per_10k(attempt_seed, kRecordCreationChancePer10k)) {
            continue;
        }

        std::vector<PersonId> author_candidates;
        for (const auto& person : state_.people) {
            if (!person.alive || age_years(person) < kAdultAgeYears) {
                continue;
            }
            const auto household_it = settlement_by_household.find(person.household_id);
            if (household_it == settlement_by_household.end() || household_it->second != settlement_id) {
                continue;
            }
            if (knowledge_count_for_person(person.id) == 0U) {
                continue;
            }
            author_candidates.push_back(person.id);
        }

        if (author_candidates.empty()) {
            continue;
        }

        std::sort(author_candidates.begin(), author_candidates.end());
        const auto author_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::RecordAuthorSelection,
            demographic_year);
        const auto author_index = static_cast<std::size_t>(
            seed_random_bounded(author_seed, author_candidates.size()));
        const auto author_id = author_candidates[author_index];

        auto author_knowledge = knowledge_for_person(author_id);
        if (author_knowledge.empty()) {
            continue;
        }
        std::sort(author_knowledge.begin(), author_knowledge.end(), [](const AgentKnowledge& lhs, const AgentKnowledge& rhs) {
            return lhs.id < rhs.id;
        });

        std::vector<AgentKnowledge> recordable_knowledge;
        recordable_knowledge.reserve(author_knowledge.size());
        for (const auto& knowledge : author_knowledge) {
            if (knowledge.source_memory_id == 0U) {
                continue;
            }
            recordable_knowledge.push_back(knowledge);
        }

        if (recordable_knowledge.empty()) {
            continue;
        }

        const auto knowledge_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::RecordKnowledgeSelection,
            (demographic_year * 1'000'003ULL) ^ author_id);
        const auto knowledge_index = static_cast<std::size_t>(
            seed_random_bounded(knowledge_seed, recordable_knowledge.size()));
        const auto& selected_knowledge = recordable_knowledge[knowledge_index];

        EventId source_event_id = 0U;
        for (const auto& memory : state_.agent_memories) {
            if (memory.id == selected_knowledge.source_memory_id) {
                source_event_id = memory.source_event_id;
                break;
            }
        }
        if (source_event_id == 0U) {
            continue;
        }

        const RecordSemanticKey source_event_key{
            .settlement_id = settlement_id,
            .source_event_id = source_event_id,
        };
        if (record_semantic_keys_.contains(source_event_key)) {
            continue;
        }

        const auto author_it = person_by_id.find(author_id);
        if (author_it == person_by_id.end() || author_it->second == nullptr) {
            continue;
        }
        const auto author_household_id = author_it->second->household_id;
        const auto author_settlement_it = settlement_by_household.find(author_household_id);
        if (author_settlement_it == settlement_by_household.end() || author_settlement_it->second != settlement_id) {
            continue;
        }

        append_knowledge_record(KnowledgeRecord{
            .id = 0,
            .settlement_id = settlement_id,
            .author_id = author_id,
            .author_household_id = author_household_id,
            .recorded_at = state_.time,
            .source_memory_id = selected_knowledge.source_memory_id,
            .source_event_id = source_event_id,
            .confidence_per_10k = std::min<std::uint32_t>(selected_knowledge.confidence_per_10k, kRecordWriteConfidenceCapPer10k),
        });
    }

    std::vector<PersonId> reader_ids;
    reader_ids.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        if (person.alive) {
            reader_ids.push_back(person.id);
        }
    }
    std::sort(reader_ids.begin(), reader_ids.end());

    for (const auto reader_id : reader_ids) {
        const auto reader_it = person_by_id.find(reader_id);
        if (reader_it == person_by_id.end() || reader_it->second == nullptr) {
            continue;
        }

        const auto reader = reader_it->second;
        const auto reader_settlement_it = settlement_by_household.find(reader->household_id);
        if (reader_settlement_it == settlement_by_household.end()) {
            continue;
        }
        const auto reader_settlement_id = reader_settlement_it->second;

        std::vector<KnowledgeRecord> readable_records;
        for (const auto& record : state_.knowledge_records) {
            if (record.settlement_id != reader_settlement_id) {
                continue;
            }
            if (person_remembers_event(reader_id, record.source_event_id)) {
                continue;
            }

            const RecordAccessSemanticKey semantic_key{
                .record_id = record.id,
                .reader_id = reader_id,
            };
            if (record_access_semantic_keys_.contains(semantic_key)) {
                continue;
            }

            readable_records.push_back(record);
        }

        if (readable_records.empty()) {
            continue;
        }

        const auto attempt_seed = derive_seed(
            person_seed(reader_id),
            SeedDomain::RecordReadAttempt,
            demographic_year);
        if (!seed_chance_per_10k(attempt_seed, kRecordReadChancePer10k)) {
            continue;
        }

        std::sort(readable_records.begin(), readable_records.end(), [](const KnowledgeRecord& lhs, const KnowledgeRecord& rhs) {
            return lhs.id < rhs.id;
        });

        const auto selection_seed = derive_seed(
            person_seed(reader_id),
            SeedDomain::RecordReadSelection,
            (demographic_year * 1'000'003ULL) ^ reader_settlement_id);
        const auto record_index = static_cast<std::size_t>(
            seed_random_bounded(selection_seed, readable_records.size()));
        const auto& selected_record = readable_records[record_index];

        const auto read_confidence = std::min<std::uint32_t>(
            selected_record.confidence_per_10k,
            kRecordReadConfidenceCapPer10k);
        const auto reader_memory_id = append_memory(AgentMemory{
            .id = 0,
            .owner_id = reader_id,
            .acquired_at = state_.time,
            .source = MemorySource::Record,
            .source_event_id = selected_record.source_event_id,
            .confidence_per_10k = read_confidence,
        });

        append_knowledge_record_access(KnowledgeRecordAccess{
            .record_id = selected_record.id,
            .reader_id = reader_id,
            .reader_household_id = reader->household_id,
            .reader_settlement_id = reader_settlement_id,
            .reader_memory_id = reader_memory_id,
            .read_at = state_.time,
            .confidence_per_10k = read_confidence,
        });
    }
}

void Simulation::process_institutions(const std::uint64_t demographic_year) {
    std::unordered_map<HouseholdId, SettlementId> settlement_by_household;
    settlement_by_household.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        settlement_by_household.emplace(household.id, household.settlement_id);
    }

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<HouseholdId, const Household*> household_by_id;
    household_by_id.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        household_by_id.emplace(household.id, &household);
    }

    std::unordered_map<RecordId, const KnowledgeRecord*> record_by_id;
    record_by_id.reserve(state_.knowledge_records.size());
    for (const auto& record : state_.knowledge_records) {
        record_by_id.emplace(record.id, &record);
    }

    std::unordered_map<EventId, const SimulationEvent*> event_by_id;
    event_by_id.reserve(state_.event_log.size());
    for (const auto& event : state_.event_log) {
        event_by_id.emplace(event.id, &event);
    }

    std::unordered_map<MemoryId, const AgentMemory*> memory_by_id;
    memory_by_id.reserve(state_.agent_memories.size());
    for (const auto& memory : state_.agent_memories) {
        memory_by_id.emplace(memory.id, &memory);
    }

    std::vector<SettlementId> active_settlement_ids;
    active_settlement_ids.reserve(state_.settlements.size());
    for (const auto& settlement : state_.settlements) {
        if (settlement.active) {
            active_settlement_ids.push_back(settlement.id);
        }
    }
    std::sort(active_settlement_ids.begin(), active_settlement_ids.end());

    for (const auto settlement_id : active_settlement_ids) {
        bool has_active_institution = false;
        for (const auto& institution : state_.institutions) {
            if (institution.settlement_id == settlement_id && institution.active) {
                has_active_institution = true;
                break;
            }
        }
        if (has_active_institution) {
            continue;
        }

        std::vector<PersonId> local_alive_people;
        std::vector<PersonId> founder_candidates;
        std::unordered_set<std::uint32_t> local_discovery_keys;
        bool has_local_record = false;

        for (const auto& record : state_.knowledge_records) {
            if (record.settlement_id == settlement_id) {
                has_local_record = true;
                break;
            }
        }

        for (const auto& person : state_.people) {
            if (!person.alive || person.household_id == 0U) {
                continue;
            }

            const auto household_it = settlement_by_household.find(person.household_id);
            if (household_it == settlement_by_household.end() || household_it->second != settlement_id) {
                continue;
            }

            local_alive_people.push_back(person.id);
            if (age_years(person) < kAdultAgeYears) {
                continue;
            }

            auto person_knowledge = knowledge_for_person(person.id);
            if (person_knowledge.empty()) {
                continue;
            }

            bool knows_local_discovery = false;
            for (const auto& knowledge : person_knowledge) {
                if (knowledge.confidence_per_10k == 0U) {
                    continue;
                }
                const auto* discovery_fact = std::get_if<BasicDiscoveryKnowledgeFact>(&knowledge.fact);
                if (discovery_fact == nullptr || discovery_fact->discovery_key == 0U) {
                    continue;
                }
                local_discovery_keys.insert(discovery_fact->discovery_key);
                knows_local_discovery = true;
            }

            if (knows_local_discovery) {
                founder_candidates.push_back(person.id);
            }
        }

        if (local_alive_people.size() < kInstitutionMinimumSettlementPopulation ||
            founder_candidates.empty() ||
            local_discovery_keys.empty() ||
            !has_local_record) {
            continue;
        }

        const auto founding_attempt_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::InstitutionFoundingAttempt,
            demographic_year);
        if (!seed_chance_per_10k(founding_attempt_seed, kInstitutionFoundingChancePerSettlementPer10k)) {
            continue;
        }

        std::sort(founder_candidates.begin(), founder_candidates.end());
        const auto founder_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::InstitutionFounderSelection,
            demographic_year);
        const auto founder_index = static_cast<std::size_t>(seed_random_bounded(founder_seed, founder_candidates.size()));
        const auto founder_id = founder_candidates[founder_index];

        std::vector<PersonId> member_candidates;
        for (const auto& person : state_.people) {
            if (!person.alive || person.household_id == 0U) {
                continue;
            }
            const auto household_it = settlement_by_household.find(person.household_id);
            if (household_it == settlement_by_household.end() || household_it->second != settlement_id) {
                continue;
            }
            if (knowledge_count_for_person(person.id) == 0U) {
                continue;
            }
            member_candidates.push_back(person.id);
        }

        std::sort(member_candidates.begin(), member_candidates.end());
        std::vector<PersonId> initial_members;
        if (!member_candidates.empty()) {
            struct ScoredMember {
                std::uint64_t score{};
                PersonId person_id{};
            };
            std::vector<ScoredMember> scored_members;
            scored_members.reserve(member_candidates.size());
            for (const auto person_id : member_candidates) {
                const auto member_seed = derive_seed(
                    settlement_seed(settlement_id),
                    SeedDomain::InstitutionMemberSelection,
                    demographic_year ^ person_id);
                scored_members.push_back(ScoredMember{.score = member_seed.value, .person_id = person_id});
            }
            std::sort(scored_members.begin(), scored_members.end(), [](const ScoredMember& lhs, const ScoredMember& rhs) {
                if (lhs.score != rhs.score) {
                    return lhs.score < rhs.score;
                }
                return lhs.person_id < rhs.person_id;
            });

            for (const auto& scored_member : scored_members) {
                initial_members.push_back(scored_member.person_id);
                if (initial_members.size() >= kInstitutionTargetMembers) {
                    break;
                }
            }
        }

        if (std::find(initial_members.begin(), initial_members.end(), founder_id) == initial_members.end()) {
            initial_members.push_back(founder_id);
        }
        std::sort(initial_members.begin(), initial_members.end());
        initial_members.erase(std::unique(initial_members.begin(), initial_members.end()), initial_members.end());
        if (initial_members.size() < kInstitutionMinimumMembers) {
            continue;
        }

        std::vector<RecordId> preserved_record_ids;
        for (const auto& record : state_.knowledge_records) {
            if (record.settlement_id == settlement_id) {
                preserved_record_ids.push_back(record.id);
            }
        }
        std::sort(preserved_record_ids.begin(), preserved_record_ids.end());
        preserved_record_ids.erase(std::unique(preserved_record_ids.begin(), preserved_record_ids.end()), preserved_record_ids.end());

        std::vector<std::uint32_t> preserved_discovery_keys;
        preserved_discovery_keys.reserve(local_discovery_keys.size());
        for (const auto key : local_discovery_keys) {
            if (key > 0U) {
                preserved_discovery_keys.push_back(key);
            }
        }
        std::sort(preserved_discovery_keys.begin(), preserved_discovery_keys.end());
        preserved_discovery_keys.erase(
            std::unique(preserved_discovery_keys.begin(), preserved_discovery_keys.end()),
            preserved_discovery_keys.end());

        const auto institution_id = next_institution_id();
        state_.institutions.push_back(Institution{
            .id = institution_id,
            .kind = InstitutionKind::KnowledgeKeeper,
            .settlement_id = settlement_id,
            .founder_id = founder_id,
            .founded_at = state_.time,
            .active = true,
            .member_ids = initial_members,
            .preserved_record_ids = preserved_record_ids,
            .preserved_discovery_keys = preserved_discovery_keys,
        });

        const auto event_id = emit_institution_founded_event(
            institution_id,
            InstitutionKind::KnowledgeKeeper,
            settlement_id,
            founder_id);
        append_direct_observation_memories(event_id, initial_members, state_.time);
    }

    std::sort(state_.institutions.begin(), state_.institutions.end(), [](const Institution& lhs, const Institution& rhs) {
        return lhs.id < rhs.id;
    });

    for (auto& institution : state_.institutions) {
        const auto* settlement = find_settlement(institution.settlement_id);
        if (settlement == nullptr || !settlement->active) {
            institution.active = false;
            institution.member_ids.clear();
            continue;
        }

        std::vector<PersonId> valid_members;
        valid_members.reserve(institution.member_ids.size());
        for (const auto member_id : institution.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end() || person_it->second == nullptr || !person_it->second->alive) {
                continue;
            }
            const auto household_it = household_by_id.find(person_it->second->household_id);
            if (household_it == household_by_id.end() || household_it->second == nullptr) {
                continue;
            }
            if (household_it->second->settlement_id != institution.settlement_id) {
                continue;
            }
            valid_members.push_back(member_id);
        }
        std::sort(valid_members.begin(), valid_members.end());
        valid_members.erase(std::unique(valid_members.begin(), valid_members.end()), valid_members.end());
        institution.member_ids = std::move(valid_members);

        if (institution.member_ids.size() < kInstitutionTargetMembers) {
            std::unordered_set<PersonId> existing_members;
            existing_members.reserve(institution.member_ids.size());
            for (const auto member_id : institution.member_ids) {
                existing_members.insert(member_id);
            }

            struct ScoredCandidate {
                std::uint64_t score{};
                PersonId person_id{};
            };
            std::vector<ScoredCandidate> add_candidates;
            for (const auto& person : state_.people) {
                if (!person.alive || person.household_id == 0U) {
                    continue;
                }
                const auto household_it = settlement_by_household.find(person.household_id);
                if (household_it == settlement_by_household.end() || household_it->second != institution.settlement_id) {
                    continue;
                }
                if (existing_members.contains(person.id) || knowledge_count_for_person(person.id) == 0U) {
                    continue;
                }

                const auto member_seed = derive_seed(
                    settlement_seed(institution.settlement_id),
                    SeedDomain::InstitutionMemberSelection,
                    (demographic_year * 1'000'003ULL) ^ institution.id ^ person.id);
                add_candidates.push_back(ScoredCandidate{.score = member_seed.value, .person_id = person.id});
            }

            std::sort(add_candidates.begin(), add_candidates.end(), [](const ScoredCandidate& lhs, const ScoredCandidate& rhs) {
                if (lhs.score != rhs.score) {
                    return lhs.score < rhs.score;
                }
                return lhs.person_id < rhs.person_id;
            });

            for (const auto& candidate : add_candidates) {
                if (institution.member_ids.size() >= kInstitutionTargetMembers) {
                    break;
                }
                institution.member_ids.push_back(candidate.person_id);
            }

            std::sort(institution.member_ids.begin(), institution.member_ids.end());
            institution.member_ids.erase(std::unique(institution.member_ids.begin(), institution.member_ids.end()), institution.member_ids.end());
        }

        institution.active = institution.member_ids.size() >= kInstitutionMinimumMembers;
        if (!institution.active) {
            continue;
        }

        for (const auto& record : state_.knowledge_records) {
            if (record.settlement_id != institution.settlement_id) {
                continue;
            }
            institution.preserved_record_ids.push_back(record.id);
        }
        std::sort(institution.preserved_record_ids.begin(), institution.preserved_record_ids.end());
        institution.preserved_record_ids.erase(
            std::unique(institution.preserved_record_ids.begin(), institution.preserved_record_ids.end()),
            institution.preserved_record_ids.end());

        std::unordered_set<std::uint32_t> discovered_keys;

        for (const auto member_id : institution.member_ids) {
            auto member_knowledge = knowledge_for_person(member_id);
            for (const auto& knowledge : member_knowledge) {
                if (knowledge.confidence_per_10k == 0U) {
                    continue;
                }
                const auto* discovery_fact = std::get_if<BasicDiscoveryKnowledgeFact>(&knowledge.fact);
                if (discovery_fact != nullptr && discovery_fact->discovery_key > 0U) {
                    discovered_keys.insert(discovery_fact->discovery_key);
                }
            }
        }

        for (const auto record_id : institution.preserved_record_ids) {
            const auto record_it = record_by_id.find(record_id);
            if (record_it == record_by_id.end() || record_it->second == nullptr) {
                continue;
            }
            const auto event_it = event_by_id.find(record_it->second->source_event_id);
            if (event_it == event_by_id.end() || event_it->second == nullptr) {
                continue;
            }
            if (event_it->second->type != EventType::DiscoveryMade) {
                continue;
            }

            const auto* discovery_payload = std::get_if<DiscoveryMadeEventPayload>(&event_it->second->payload);
            if (discovery_payload != nullptr && discovery_payload->discovery_key > 0U) {
                discovered_keys.insert(discovery_payload->discovery_key);
            }
        }

        institution.preserved_discovery_keys.clear();
        institution.preserved_discovery_keys.reserve(discovered_keys.size());
        for (const auto key : discovered_keys) {
            institution.preserved_discovery_keys.push_back(key);
        }
        std::sort(institution.preserved_discovery_keys.begin(), institution.preserved_discovery_keys.end());

        std::unordered_set<EventId> preserved_record_source_events;
        for (const auto record_id : institution.preserved_record_ids) {
            const auto record_it = record_by_id.find(record_id);
            if (record_it != record_by_id.end() && record_it->second != nullptr) {
                preserved_record_source_events.insert(record_it->second->source_event_id);
            }
        }

        struct InstitutionSourceCandidate {
            PersonId speaker_id{};
            MemoryId speaker_memory_id{};
            EventId source_event_id{};
            std::uint32_t confidence_per_10k{};
            std::optional<std::uint32_t> discovery_key{};
        };

        std::vector<InstitutionSourceCandidate> source_candidates;
        for (const auto member_id : institution.member_ids) {
            auto member_knowledge = knowledge_for_person(member_id);
            std::sort(member_knowledge.begin(), member_knowledge.end(), [](const AgentKnowledge& lhs, const AgentKnowledge& rhs) {
                return lhs.id < rhs.id;
            });

            for (const auto& knowledge : member_knowledge) {
                if (knowledge.confidence_per_10k == 0U || knowledge.source_memory_id == 0U) {
                    continue;
                }

                const auto source_memory_it = memory_by_id.find(knowledge.source_memory_id);
                if (source_memory_it == memory_by_id.end() || source_memory_it->second == nullptr) {
                    continue;
                }
                const auto* source_memory = source_memory_it->second;
                if (source_memory->owner_id != member_id || source_memory->source_event_id == 0U) {
                    continue;
                }

                bool include = false;
                std::optional<std::uint32_t> discovery_key = std::nullopt;
                if (const auto* discovery_fact = std::get_if<BasicDiscoveryKnowledgeFact>(&knowledge.fact); discovery_fact != nullptr) {
                    if (std::binary_search(
                            institution.preserved_discovery_keys.begin(),
                            institution.preserved_discovery_keys.end(),
                            discovery_fact->discovery_key)) {
                        include = true;
                        discovery_key = discovery_fact->discovery_key;
                    }
                }

                if (!include && preserved_record_source_events.contains(source_memory->source_event_id)) {
                    include = true;
                }

                if (!include) {
                    continue;
                }

                source_candidates.push_back(InstitutionSourceCandidate{
                    .speaker_id = member_id,
                    .speaker_memory_id = knowledge.source_memory_id,
                    .source_event_id = source_memory->source_event_id,
                    .confidence_per_10k = knowledge.confidence_per_10k,
                    .discovery_key = discovery_key,
                });
            }
        }

        if (source_candidates.empty()) {
            continue;
        }

        std::sort(source_candidates.begin(), source_candidates.end(), [](const InstitutionSourceCandidate& lhs, const InstitutionSourceCandidate& rhs) {
            if (lhs.speaker_memory_id != rhs.speaker_memory_id) {
                return lhs.speaker_memory_id < rhs.speaker_memory_id;
            }
            return lhs.speaker_id < rhs.speaker_id;
        });
        source_candidates.erase(
            std::unique(source_candidates.begin(), source_candidates.end(), [](const InstitutionSourceCandidate& lhs, const InstitutionSourceCandidate& rhs) {
                return lhs.speaker_memory_id == rhs.speaker_memory_id && lhs.speaker_id == rhs.speaker_id;
            }),
            source_candidates.end());

        std::unordered_set<PersonId> member_ids;
        member_ids.reserve(institution.member_ids.size());
        for (const auto member_id : institution.member_ids) {
            member_ids.insert(member_id);
        }

        std::vector<PersonId> recipient_candidates;
        for (const auto& person : state_.people) {
            if (!person.alive || person.household_id == 0U || member_ids.contains(person.id)) {
                continue;
            }

            const auto household_it = settlement_by_household.find(person.household_id);
            if (household_it == settlement_by_household.end() || household_it->second != institution.settlement_id) {
                continue;
            }
            recipient_candidates.push_back(person.id);
        }

        if (recipient_candidates.empty()) {
            continue;
        }
        std::sort(recipient_candidates.begin(), recipient_candidates.end());

        const auto source_seed = derive_seed(
            settlement_seed(institution.settlement_id),
            SeedDomain::InstitutionTeachingSourceSelection,
            demographic_year ^ institution.id);
        const auto source_index = static_cast<std::size_t>(seed_random_bounded(source_seed, source_candidates.size()));
        const auto& selected_source = source_candidates[source_index];

        std::vector<PersonId> eligible_recipients;
        eligible_recipients.reserve(recipient_candidates.size());
        for (const auto recipient_id : recipient_candidates) {
            if (selected_source.discovery_key.has_value() &&
                knows_basic_discovery(recipient_id, *selected_source.discovery_key)) {
                continue;
            }

            if (transmission_semantic_keys_.contains(TransmissionSemanticKey{
                    .recipient_id = recipient_id,
                    .speaker_memory_id = selected_source.speaker_memory_id,
                })) {
                continue;
            }
            eligible_recipients.push_back(recipient_id);
        }

        if (eligible_recipients.empty()) {
            continue;
        }

        const auto recipient_seed = derive_seed(
            settlement_seed(institution.settlement_id),
            SeedDomain::InstitutionTeachingRecipientSelection,
            (demographic_year * 1'000'003ULL) ^ institution.id ^ selected_source.speaker_memory_id);
        const auto recipient_index = static_cast<std::size_t>(seed_random_bounded(recipient_seed, eligible_recipients.size()));
        const auto recipient_id = eligible_recipients[recipient_index];

        const auto speaker_it = person_by_id.find(selected_source.speaker_id);
        const auto recipient_it = person_by_id.find(recipient_id);
        if (speaker_it == person_by_id.end() || recipient_it == person_by_id.end() ||
            speaker_it->second == nullptr || recipient_it->second == nullptr) {
            continue;
        }

        const auto speaker_household_it = household_by_id.find(speaker_it->second->household_id);
        const auto recipient_household_it = household_by_id.find(recipient_it->second->household_id);
        if (speaker_household_it == household_by_id.end() || recipient_household_it == household_by_id.end() ||
            speaker_household_it->second == nullptr || recipient_household_it->second == nullptr) {
            continue;
        }

        const auto confidence = std::min<std::uint32_t>(
            selected_source.confidence_per_10k,
            kInstitutionTeachingConfidenceCapPer10k);
        const auto recipient_memory_id = append_memory(AgentMemory{
            .id = 0,
            .owner_id = recipient_id,
            .acquired_at = state_.time,
            .source = MemorySource::Teaching,
            .source_event_id = selected_source.source_event_id,
            .confidence_per_10k = confidence,
        });

        append_knowledge_transmission(KnowledgeTransmission{
            .id = 0,
            .speaker_id = selected_source.speaker_id,
            .recipient_id = recipient_id,
            .speaker_household_id = speaker_household_it->second->id,
            .recipient_household_id = recipient_household_it->second->id,
            .speaker_settlement_id = institution.settlement_id,
            .recipient_settlement_id = institution.settlement_id,
            .transmitted_at = state_.time,
            .method = CommunicationMethod::Teaching,
            .speaker_memory_id = selected_source.speaker_memory_id,
            .recipient_memory_id = recipient_memory_id,
            .confidence_per_10k = confidence,
        });
    }
}

void Simulation::process_knowledge_loss(const std::uint64_t demographic_year) {
    std::unordered_map<MemoryId, const AgentMemory*> memory_by_id;
    memory_by_id.reserve(state_.agent_memories.size());
    for (const auto& memory : state_.agent_memories) {
        memory_by_id.emplace(memory.id, &memory);
    }

    for (auto& knowledge : state_.agent_knowledge) {
        if (knowledge.updated_at >= state_.time) {
            continue;
        }

        const auto stale_duration_ticks = state_.time.ticks - knowledge.updated_at.ticks;
        const auto stale_years = static_cast<std::uint64_t>(
            stale_duration_ticks / DemographicCalendarAdapter::year_duration().ticks);
        if (stale_years < kKnowledgeDecayGraceYears) {
            continue;
        }

        const auto source_memory_it = memory_by_id.find(knowledge.source_memory_id);
        if (source_memory_it == memory_by_id.end() || source_memory_it->second == nullptr) {
            continue;
        }

        const auto* source_memory = source_memory_it->second;
        std::uint32_t decay = kKnowledgeDecayDirectPerYearPer10k;
        if (source_memory->source == MemorySource::Communication) {
            decay = kKnowledgeDecayCommunicationPerYearPer10k;
        } else if (source_memory->source == MemorySource::Teaching) {
            decay = kKnowledgeDecayTeachingPerYearPer10k;
        } else if (source_memory->source == MemorySource::Record) {
            decay = kKnowledgeDecayRecordPerYearPer10k;
        }

        if (knowledge.confidence_per_10k <= decay) {
            knowledge.confidence_per_10k = 0U;
        } else {
            knowledge.confidence_per_10k -= decay;
        }

        if (knowledge.confidence_per_10k > kKnowledgeForgetThresholdPer10k) {
            continue;
        }

        const auto forget_seed = derive_seed(
            person_seed(knowledge.owner_id),
            SeedDomain::KnowledgeForgetAttempt,
            (knowledge.id * 1'000'003ULL) ^ demographic_year);
        const auto roll_seed = derive_seed(
            forget_seed,
            SeedDomain::KnowledgeForgetRoll,
            knowledge.source_memory_id);

        const auto deficit = kKnowledgeForgetThresholdPer10k - knowledge.confidence_per_10k;
        const auto forget_chance = std::min<std::uint32_t>(
            10'000U,
            2'000U + (deficit * 6U));
        if (!seed_chance_per_10k(roll_seed, forget_chance)) {
            continue;
        }

        knowledge.confidence_per_10k = 0U;
    }
}

PersonId Simulation::next_person_id() noexcept {
    return next_person_id_++;
}

HouseholdId Simulation::next_household_id() noexcept {
    return next_household_id_++;
}

RegionId Simulation::next_region_id() noexcept {
    return next_region_id_++;
}

SettlementId Simulation::next_settlement_id() noexcept {
    return next_settlement_id_++;
}

MemoryId Simulation::next_memory_id() noexcept {
    return next_memory_id_++;
}

KnowledgeId Simulation::next_knowledge_id() noexcept {
    return next_knowledge_id_++;
}

CommunicationId Simulation::next_communication_id() noexcept {
    return next_communication_id_++;
}

RecordId Simulation::next_record_id() noexcept {
    return next_record_id_++;
}

InstitutionId Simulation::next_institution_id() noexcept {
    return next_institution_id_++;
}

BuildingId Simulation::next_building_id() noexcept {
    return next_building_id_++;
}

SimulationSeed Simulation::root_seed() const noexcept {
    return simulation_seed_;
}

SimulationSeed Simulation::person_seed(const PersonId person_id) const noexcept {
    return derive_seed(root_seed(), SeedDomain::Person, person_id);
}

SimulationSeed Simulation::household_seed(const HouseholdId household_id) const noexcept {
    return derive_seed(root_seed(), SeedDomain::Household, household_id);
}

SimulationSeed Simulation::region_seed(const RegionId region_id) const noexcept {
    return derive_seed(root_seed(), SeedDomain::Region, region_id);
}

SimulationSeed Simulation::settlement_seed(const SettlementId settlement_id) const noexcept {
    return derive_seed(root_seed(), SeedDomain::Settlement, settlement_id);
}

bool Simulation::sample_sex(const SimulationSeed seed) const noexcept {
    const auto sex_seed = derive_seed(seed, SeedDomain::Sex, 0U);
    return seed_chance_per_10k(sex_seed, 5'000U);
}

std::uint32_t Simulation::mortality_rate_per_10k(const std::uint32_t age_years) const noexcept {
    if (age_years == 0U) {
        return 380;
    }
    if (age_years <= 4U) {
        return 35;
    }
    if (age_years <= 14U) {
        return 15;
    }
    if (age_years <= 49U) {
        return 20;
    }
    if (age_years <= 64U) {
        return 60;
    }
    if (age_years <= 74U) {
        return 180;
    }
    if (age_years <= 84U) {
        return 900;
    }
    if (age_years <= 94U) {
        return 2'500;
    }
    if (age_years <= 104U) {
        return 5'000;
    }
    return 10'000;
}

std::uint32_t Simulation::mortality_penalty_from_person_health_per_10k(const Person& person) const noexcept {
    const auto is_child = age_years(person) < kAdultAgeYears;
    return nutrition_health_mortality_penalty_per_10k(person.nutrition_points, person.health_points, is_child);
}

std::uint32_t Simulation::fertility_rate_per_10k(const std::uint32_t age_years) const noexcept {
    if (age_years < 18U || age_years > 49U) {
        return 0;
    }
    if (age_years <= 24U) {
        return 2'200;
    }
    if (age_years <= 34U) {
        return 2'800;
    }
    if (age_years <= 39U) {
        return 1'300;
    }
    if (age_years <= 44U) {
        return 500;
    }
    return 180;
}

std::uint32_t Simulation::sample_initial_age_years(const PersonId person_id) const noexcept {
    const auto base_seed = derive_seed(person_seed(person_id), SeedDomain::InitialPopulation, 0U);
    const auto roll = static_cast<std::uint32_t>(seed_random_bounded(base_seed, 100U));
    if (roll < 35U) {
        const auto age_seed = derive_seed(base_seed, SeedDomain::InitialPopulation, 1U);
        return static_cast<std::uint32_t>(seed_random_bounded(age_seed, 15U));
    }
    if (roll < 85U) {
        const auto age_seed = derive_seed(base_seed, SeedDomain::InitialPopulation, 2U);
        return 15U + static_cast<std::uint32_t>(seed_random_bounded(age_seed, 35U));
    }
    const auto age_seed = derive_seed(base_seed, SeedDomain::InitialPopulation, 3U);
    return 50U + static_cast<std::uint32_t>(seed_random_bounded(age_seed, 41U));
}

std::int64_t Simulation::current_year_signed() const noexcept {
    const auto year = current_demographic_year();
    const auto max_i64_u64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (year > max_i64_u64) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(year);
}

std::uint32_t Simulation::age_years(const Person& person) const noexcept {
    const auto current_year = current_year_signed();
    if (current_year <= person.birth_year) {
        return 0;
    }

    const auto diff = current_year - person.birth_year;
    return bounded_u32(static_cast<std::uint64_t>(diff));
}

bool Simulation::has_any_alive_adult_male() const noexcept {
    for (const auto& person : state_.people) {
        if (person.alive && person.sex == BiologicalSex::Male && age_years(person) >= kAdultAgeYears) {
            return true;
        }
    }
    return false;
}

bool Simulation::is_alive_adult_male_in_household(const HouseholdId household_id) const noexcept {
    for (const auto& person : state_.people) {
        if (!person.alive || person.household_id != household_id || person.sex != BiologicalSex::Male) {
            continue;
        }
        if (age_years(person) >= kAdultAgeYears) {
            return true;
        }
    }
    return false;
}

std::uint32_t Simulation::alive_count_in_household(const HouseholdId household_id) const noexcept {
    std::uint32_t count = 0;
    for (const auto& person : state_.people) {
        if (person.alive && person.household_id == household_id) {
            ++count;
        }
    }
    return count;
}

std::int64_t Simulation::food_requirement_for_person(const Person& person) const noexcept {
    return age_years(person) >= kAdultAgeYears
        ? food_parameters_.adult_daily_food_units
        : food_parameters_.child_daily_food_units;
}

std::int64_t Simulation::productive_effort_for_person(const Person& person) const noexcept {
    return person_labour_capacity_units(
        person.alive,
        age_years(person),
        person.nutrition_points,
        person.health_points,
        food_parameters_,
        config_.productive_effort_scale_percent);
}

bool Simulation::occupation_eligible_for_productive_work(const Person& person) const noexcept {
    if (!person.alive) {
        return false;
    }
    if (age_years(person) < kAdultAgeYears) {
        return false;
    }
    if (person.nutrition_points < kOccupationMinimumNutritionPoints || person.health_points < kOccupationMinimumHealthPoints) {
        return false;
    }
    return productive_effort_for_person(person) > 0;
}

std::uint32_t Simulation::effective_fertility_rate_per_10k(const Person& person) const noexcept {
    const auto base_rate = fertility_rate_per_10k(age_years(person));
    if (base_rate == 0U) {
        return 0U;
    }

    const auto household_alive = alive_count_in_household(person.household_id);
    if (household_alive >= 24U) {
        return std::max<std::uint32_t>(base_rate / 20U, 1U);
    }

    const auto capacity_remaining = 24U - household_alive;
    const auto scaled = static_cast<std::uint64_t>(base_rate) * capacity_remaining;
    auto effective = std::max<std::uint32_t>(static_cast<std::uint32_t>(scaled / 24U), 1U);

    const auto person_scale = nutrition_health_fertility_scale_percent(person.nutrition_points, person.health_points);
    effective = static_cast<std::uint32_t>((static_cast<std::uint64_t>(effective) * person_scale) / 100U);
    return effective;
}

void Simulation::apply_person_nutrition_and_health(
    Person& person,
    const std::int64_t required_food,
    const std::int64_t allocated_food) {
    const auto required = std::max<std::int64_t>(required_food, 0);
    const auto allocated = std::max<std::int64_t>(allocated_food, 0);

    const auto deficit = std::max<std::int64_t>(required - allocated, 0);
    const auto age = age_years(person);

    std::int32_t nutrition_delta = 0;
    if (deficit <= 0) {
        nutrition_delta = 8;
        person.consecutive_undernourished_days = 0;
    } else {
        const auto ratio_points = (required > 0) ? static_cast<std::int32_t>((deficit * 100) / required) : 100;
        nutrition_delta = -(5 + ratio_points / 12);
        ++person.consecutive_undernourished_days;
    }

    person.nutrition_points = clamp_i32(
        person.nutrition_points + nutrition_delta,
        food_parameters_.min_person_nutrition_points,
        food_parameters_.max_person_nutrition_points);

    std::int32_t health_delta = 0;
    if (person.nutrition_points >= 750) {
        health_delta = 3;
    } else if (person.nutrition_points >= 550) {
        health_delta = 0;
    } else if (person.nutrition_points >= 350) {
        health_delta = -2;
    } else {
        health_delta = -5;
    }

    if (age < 5U && deficit > 0) {
        health_delta -= 2;
    }

    if (person.consecutive_undernourished_days >= 20U && deficit > 0) {
        health_delta -= 2;
    }

    person.health_points = clamp_i32(
        person.health_points + health_delta,
        food_parameters_.min_person_health_points,
        food_parameters_.max_person_health_points);
}

std::int64_t Simulation::household_stress_points(const HouseholdId household_id) const noexcept {
    for (const auto& household : state_.households) {
        if (household.id == household_id) {
            return household.resources.nutritional_stress_points;
        }
    }
    return 0;
}

std::optional<PersonId> Simulation::sample_father_in_household(
    const HouseholdId household_id,
    const SimulationSeed selection_seed) noexcept {
    std::vector<PersonId> candidates;
    for (const auto& person : state_.people) {
        if (!person.alive || person.household_id != household_id || person.sex != BiologicalSex::Male) {
            continue;
        }
        if (age_years(person) >= kAdultAgeYears) {
            candidates.push_back(person.id);
        }
    }

    if (candidates.empty()) {
        return std::nullopt;
    }

    std::sort(candidates.begin(), candidates.end());
    const auto scoped_seed = derive_seed(selection_seed, SeedDomain::ParentSelection, household_id);
    const auto index = static_cast<std::size_t>(seed_random_bounded(scoped_seed, candidates.size()));
    return candidates[index];
}

std::optional<PersonId> Simulation::sample_father_global(const SimulationSeed selection_seed) noexcept {
    std::vector<PersonId> candidates;
    for (const auto& person : state_.people) {
        if (!person.alive || person.sex != BiologicalSex::Male) {
            continue;
        }
        if (age_years(person) >= kAdultAgeYears) {
            candidates.push_back(person.id);
        }
    }

    if (candidates.empty()) {
        return std::nullopt;
    }

    std::sort(candidates.begin(), candidates.end());
    const auto scoped_seed = derive_seed(selection_seed, SeedDomain::ParentSelection, 0U);
    const auto index = static_cast<std::size_t>(seed_random_bounded(scoped_seed, candidates.size()));
    return candidates[index];
}

void Simulation::remove_from_household(const HouseholdId household_id, const PersonId person_id) {
    for (auto& household : state_.households) {
        if (household.id != household_id) {
            continue;
        }

        auto& members = household.member_ids;
        members.erase(std::remove(members.begin(), members.end(), person_id), members.end());
        return;
    }
}

void Simulation::initialize_population() {
    state_.people.clear();
    state_.people.reserve(static_cast<std::size_t>(config_.initial_population));

    for (std::uint64_t i = 0; i < config_.initial_population; ++i) {
        (void)i;
        const auto id = next_person_id();
        const auto age = sample_initial_age_years(id);
        const auto sex = sample_sex(person_seed(id)) ? BiologicalSex::Female : BiologicalSex::Male;

        state_.people.push_back(Person{
            .id = id,
            .birth_year = -static_cast<std::int64_t>(age),
            .sex = sex,
            .alive = true,
            .household_id = 0,
            .mother_id = std::nullopt,
            .father_id = std::nullopt,
        });
    }
}

void Simulation::initialize_households() {
    state_.households.clear();
    if (state_.people.empty()) {
        return;
    }

    std::vector<PersonId> adult_ids;
    std::vector<PersonId> child_ids;
    for (const auto& person : state_.people) {
        if (age_years(person) >= kAdultAgeYears) {
            adult_ids.push_back(person.id);
        } else {
            child_ids.push_back(person.id);
        }
    }

    std::uint64_t household_count = 1;
    if (!adult_ids.empty()) {
        household_count = (static_cast<std::uint64_t>(adult_ids.size()) + 1U) / 2U;
        household_count = std::max<std::uint64_t>(household_count, 1U);
    }

    state_.households.reserve(static_cast<std::size_t>(household_count));
    for (std::uint64_t i = 0; i < household_count; ++i) {
        state_.households.push_back(Household{
            .id = next_household_id(),
            .region_id = 0,
            .settlement_id = 0,
            .member_ids = {},
            .resources = HouseholdResourceState{.food_inventory = ResourceAmount{config_.initial_household_food_units}},
        });
    }

    const auto assign_person_to_household = [this](const PersonId person_id, const HouseholdId household_id) {
        for (auto& person : state_.people) {
            if (person.id == person_id) {
                person.household_id = household_id;
                break;
            }
        }

        for (auto& household : state_.households) {
            if (household.id == household_id) {
                household.member_ids.push_back(person_id);
                break;
            }
        }
    };

    if (!adult_ids.empty()) {
        for (std::size_t i = 0; i < adult_ids.size(); ++i) {
            const auto household_index = std::min(i / 2U, state_.households.size() - 1U);
            assign_person_to_household(adult_ids[i], state_.households[household_index].id);
        }
    }

    for (const auto child_id : child_ids) {
        const auto assignment_seed = derive_seed(person_seed(child_id), SeedDomain::HouseholdAssignment, 0U);
        const auto household_index = static_cast<std::size_t>(seed_random_bounded(assignment_seed, state_.households.size()));
        assign_person_to_household(child_id, state_.households[household_index].id);
    }

    std::sort(state_.households.begin(), state_.households.end(), [](const Household& lhs, const Household& rhs) {
        return lhs.id < rhs.id;
    });
    for (auto& household : state_.households) {
        std::sort(household.member_ids.begin(), household.member_ids.end());
    }
}

RegionEnvironmentState Simulation::generate_region_state(const RegionId region_id) const {
    const auto seed = region_seed(region_id);

    const auto area_variation_seed = derive_seed(seed, SeedDomain::RegionProductiveCapacity, 0U);
    const auto area_variation = static_cast<std::int64_t>(seed_random_bounded(area_variation_seed, 41U)) - 20;
    auto land_area_units = config_.base_region_area_units + ((config_.base_region_area_units * area_variation) / 100);
    land_area_units = clamp_i64(land_area_units, kMinRegionAreaUnits, kMaxRegionAreaUnits);

    const auto environment_seed = derive_seed(seed, SeedDomain::RegionEnvironmentClass, 0U);
    const auto class_roll = static_cast<std::uint32_t>(seed_random_bounded(environment_seed, 100U));
    EnvironmentClass environment_class = EnvironmentClass::Marginal;
    if (class_roll < 45U) {
        environment_class = EnvironmentClass::ProductiveArable;
    } else if (class_roll < 70U) {
        environment_class = EnvironmentClass::Woodland;
    } else if (class_roll >= 90U) {
        environment_class = EnvironmentClass::WaterWetland;
    }

    std::int64_t productive_capacity_per_day = 0;
    if (!checked_mul_i64(land_area_units, environment_class_productive_units_per_area(environment_class), productive_capacity_per_day)) {
        throw std::overflow_error("Region productive capacity overflow");
    }

    std::int64_t wild_food_capacity = 0;
    if (!checked_mul_i64(land_area_units, environment_class_wild_capacity_units_per_area(environment_class), wild_food_capacity)) {
        throw std::overflow_error("Region wild food capacity overflow");
    }

    const auto productive_jitter_seed = derive_seed(seed, SeedDomain::RegionProductiveCapacity, 1U);
    const auto productive_jitter_percent = 90U + static_cast<std::uint32_t>(seed_random_bounded(productive_jitter_seed, 21U));
    productive_capacity_per_day = apply_scaled_percent(productive_capacity_per_day, productive_jitter_percent);

    const auto wild_jitter_seed = derive_seed(seed, SeedDomain::RegionWildCapacity, 1U);
    const auto wild_jitter_percent = 90U + static_cast<std::uint32_t>(seed_random_bounded(wild_jitter_seed, 21U));
    wild_food_capacity = apply_scaled_percent(wild_food_capacity, wild_jitter_percent);

    const auto wild_food_regeneration_per_day = std::max<std::int64_t>(1, wild_food_capacity / 120);
    const auto wild_food_harvest_limit_per_day = std::max<std::int64_t>(1, wild_food_capacity / 20);

    std::int64_t wood_capacity = 0;
    if (!checked_mul_i64(land_area_units, 180, wood_capacity)) {
        throw std::overflow_error("Region wood capacity overflow");
    }
    const auto wood_seed = derive_seed(seed, SeedDomain::RegionWoodCapacity, 1U);
    const auto wood_jitter_percent = 85U + static_cast<std::uint32_t>(seed_random_bounded(wood_seed, 31U));
    wood_capacity = apply_scaled_percent(wood_capacity, wood_jitter_percent);
    wood_capacity = apply_scaled_percent(wood_capacity, config_.wood_resource_scale_percent);

    std::int64_t stone_capacity = 0;
    if (!checked_mul_i64(land_area_units, 90, stone_capacity)) {
        throw std::overflow_error("Region stone capacity overflow");
    }
    const auto stone_seed = derive_seed(seed, SeedDomain::RegionStoneCapacity, 1U);
    const auto stone_jitter_percent = 85U + static_cast<std::uint32_t>(seed_random_bounded(stone_seed, 31U));
    stone_capacity = apply_scaled_percent(stone_capacity, stone_jitter_percent);
    stone_capacity = apply_scaled_percent(stone_capacity, config_.stone_resource_scale_percent);

    auto wood_regeneration_per_day = std::max<std::int64_t>(1, wood_capacity / 90);
    wood_regeneration_per_day = apply_scaled_percent(wood_regeneration_per_day, config_.wood_regeneration_scale_percent);
    wood_regeneration_per_day = std::max<std::int64_t>(wood_regeneration_per_day, 0);

    return RegionEnvironmentState{
        .id = region_id,
        .land_area_units = land_area_units,
        .environment_class = environment_class,
        .productive_capacity_per_day = productive_capacity_per_day,
        .wild_food_capacity = wild_food_capacity,
        .wild_food_stock = wild_food_capacity,
        .wild_food_regeneration_per_day = wild_food_regeneration_per_day,
        .wild_food_harvest_limit_per_day = wild_food_harvest_limit_per_day,
        .cumulative_wild_food_harvested = 0,
        .cumulative_wild_food_regenerated = 0,
        .wood_capacity = wood_capacity,
        .wood_stock = wood_capacity,
        .wood_regeneration_per_day = wood_regeneration_per_day,
        .cumulative_wood_harvested = 0,
        .cumulative_wood_regenerated = 0,
        .stone_capacity = stone_capacity,
        .stone_remaining = stone_capacity,
        .cumulative_stone_extracted = 0,
    };
}

void Simulation::initialize_regions() {
    state_.regions.clear();
    state_.regions.reserve(static_cast<std::size_t>(config_.initial_region_count));
    for (std::uint64_t i = 0; i < config_.initial_region_count; ++i) {
        (void)i;
        const auto region_id = next_region_id();
        state_.regions.push_back(generate_region_state(region_id));
    }
    std::sort(state_.regions.begin(), state_.regions.end(), [](const RegionEnvironmentState& lhs, const RegionEnvironmentState& rhs) {
        return lhs.id < rhs.id;
    });
}

void Simulation::assign_households_to_regions() {
    if (state_.regions.empty()) {
        return;
    }

    std::vector<RegionId> region_ids;
    region_ids.reserve(state_.regions.size());
    for (const auto& region : state_.regions) {
        region_ids.push_back(region.id);
    }
    std::sort(region_ids.begin(), region_ids.end());

    for (auto& household : state_.households) {
        const auto assignment_seed = derive_seed(household_seed(household.id), SeedDomain::RegionAssignment, 0U);
        const auto region_index = static_cast<std::size_t>(seed_random_bounded(assignment_seed, region_ids.size()));
        household.region_id = region_ids[region_index];
        household.settlement_id = 0;
    }
}

void Simulation::initialize_settlements() {
    state_.settlements.clear();

    std::vector<RegionId> region_ids;
    region_ids.reserve(state_.regions.size());
    for (const auto& region : state_.regions) {
        region_ids.push_back(region.id);
    }
    std::sort(region_ids.begin(), region_ids.end());

    for (const auto region_id : region_ids) {
        std::vector<Household*> region_households;
        region_households.reserve(state_.households.size());
        for (auto& household : state_.households) {
            if (household.region_id == region_id) {
                household.settlement_id = 0;
                region_households.push_back(&household);
            }
        }

        std::sort(region_households.begin(), region_households.end(), [](const Household* lhs, const Household* rhs) {
            return lhs->id < rhs->id;
        });

        if (region_households.empty()) {
            continue;
        }

        auto settlement_count = (static_cast<std::uint64_t>(region_households.size()) + (kTargetHouseholdsPerSettlement - 1U)) /
            kTargetHouseholdsPerSettlement;
        settlement_count = std::max<std::uint64_t>(1U, std::min<std::uint64_t>(settlement_count, kMaxInitialSettlementsPerRegion));

        const auto share_seed = derive_seed(region_seed(region_id), SeedDomain::SettlementAssignment, 1U);
        const auto share_jitter = static_cast<std::int32_t>(seed_random_bounded(share_seed, 1'001U)) - 500;
        const auto allocated_share_i32 = clamp_i32(
            static_cast<std::int32_t>(kInitialAllocatedSettlementShare) + share_jitter,
            static_cast<std::int32_t>(settlement_count),
            static_cast<std::int32_t>(kSettlementShareScale));
        const auto allocated_share = static_cast<std::uint32_t>(allocated_share_i32);

        std::vector<SettlementId> settlement_ids;
        settlement_ids.reserve(static_cast<std::size_t>(settlement_count));

        const auto base_share = allocated_share / static_cast<std::uint32_t>(settlement_count);
        auto share_remainder = allocated_share % static_cast<std::uint32_t>(settlement_count);
        for (std::uint64_t i = 0; i < settlement_count; ++i) {
            const auto settlement_id = next_settlement_id();
            std::uint32_t share = base_share;
            if (share_remainder > 0U) {
                ++share;
                --share_remainder;
            }

            state_.settlements.push_back(Settlement{
                .id = settlement_id,
                .region_id = region_id,
                .active = true,
                .access_share_points = share,
            });
            settlement_ids.push_back(settlement_id);
        }

        for (auto* household : region_households) {
            const auto assignment_seed = derive_seed(household_seed(household->id), SeedDomain::SettlementAssignment, 0U);
            const auto settlement_index = static_cast<std::size_t>(seed_random_bounded(assignment_seed, settlement_ids.size()));
            household->settlement_id = settlement_ids[settlement_index];
        }

        std::unordered_map<SettlementId, std::uint64_t> settlement_household_counts;
        settlement_household_counts.reserve(settlement_ids.size());
        for (const auto settlement_id : settlement_ids) {
            settlement_household_counts.emplace(settlement_id, 0U);
        }
        for (const auto* household : region_households) {
            ++settlement_household_counts[household->settlement_id];
        }

        for (const auto settlement_id : settlement_ids) {
            auto count_it = settlement_household_counts.find(settlement_id);
            if (count_it == settlement_household_counts.end() || count_it->second > 0U) {
                continue;
            }

            Household* donor_household = nullptr;
            SettlementId donor_settlement_id = 0;
            std::uint64_t donor_count = 0;
            for (const auto donor_id : settlement_ids) {
                const auto donor_it = settlement_household_counts.find(donor_id);
                if (donor_it == settlement_household_counts.end() || donor_it->second <= 1U) {
                    continue;
                }
                if (donor_it->second > donor_count || (donor_it->second == donor_count && donor_id < donor_settlement_id)) {
                    donor_count = donor_it->second;
                    donor_settlement_id = donor_id;
                }
            }

            if (donor_count <= 1U) {
                continue;
            }

            for (auto* household : region_households) {
                if (household->settlement_id == donor_settlement_id) {
                    donor_household = household;
                    break;
                }
            }

            if (donor_household == nullptr) {
                continue;
            }

            donor_household->settlement_id = settlement_id;
            --settlement_household_counts[donor_settlement_id];
            ++settlement_household_counts[settlement_id];
        }
    }

    std::sort(state_.settlements.begin(), state_.settlements.end(), [](const Settlement& lhs, const Settlement& rhs) {
        return lhs.id < rhs.id;
    });
}

RegionEnvironmentState* Simulation::find_region_mutable(const RegionId region_id) noexcept {
    for (auto& region : state_.regions) {
        if (region.id == region_id) {
            return &region;
        }
    }
    return nullptr;
}

const RegionEnvironmentState* Simulation::find_region(const RegionId region_id) const noexcept {
    for (const auto& region : state_.regions) {
        if (region.id == region_id) {
            return &region;
        }
    }
    return nullptr;
}

Settlement* Simulation::find_settlement_mutable(const SettlementId settlement_id) noexcept {
    for (auto& settlement : state_.settlements) {
        if (settlement.id == settlement_id) {
            return &settlement;
        }
    }
    return nullptr;
}

const Settlement* Simulation::find_settlement(const SettlementId settlement_id) const noexcept {
    for (const auto& settlement : state_.settlements) {
        if (settlement.id == settlement_id) {
            return &settlement;
        }
    }
    return nullptr;
}

std::int64_t Simulation::settlement_accessible_food_capacity_per_day(const Settlement& settlement) const noexcept {
    if (!settlement.active || settlement.access_share_points == 0U) {
        return 0;
    }

    const auto* region = find_region(settlement.region_id);
    if (region == nullptr) {
        return 0;
    }

    const auto productive = (region->productive_capacity_per_day * static_cast<std::int64_t>(settlement.access_share_points)) /
        static_cast<std::int64_t>(kSettlementShareScale);
    const auto wild = (region->wild_food_harvest_limit_per_day * static_cast<std::int64_t>(settlement.access_share_points)) /
        static_cast<std::int64_t>(kSettlementShareScale);
    std::int64_t capacity = 0;
    if (!checked_add_i64(productive, wild, capacity)) {
        capacity = std::numeric_limits<std::int64_t>::max();
    }
    if (!checked_add_i64(capacity, settlement.dwelling_capacity_bonus, capacity)) {
        capacity = std::numeric_limits<std::int64_t>::max();
    }
    if (!checked_add_i64(capacity, settlement.food_store_capacity_bonus, capacity)) {
        capacity = std::numeric_limits<std::int64_t>::max();
    }
    return std::max<std::int64_t>(0, capacity);
}

std::uint32_t Simulation::settlement_pressure_per_10k(const Settlement& settlement) const noexcept {
    if (!settlement.active) {
        return 0U;
    }

    std::int64_t total_required = 0;
    for (const auto& household : state_.households) {
        if (household.settlement_id != settlement.id) {
            continue;
        }
        for (const auto member_id : household.member_ids) {
            for (const auto& person : state_.people) {
                if (person.id == member_id && person.alive) {
                    total_required += food_requirement_for_person(person);
                    break;
                }
            }
        }
    }

    if (total_required <= 0) {
        return 0U;
    }

    const auto capacity = settlement_accessible_food_capacity_per_day(settlement);
    if (capacity <= 0) {
        return 100'000U;
    }

    const auto ratio = (total_required * 10'000) / capacity;
    if (ratio < 0) {
        return 0U;
    }
    return static_cast<std::uint32_t>(std::min<std::int64_t>(ratio, 100'000));
}

void Simulation::process_settlement_extinction() {
    for (auto& settlement : state_.settlements) {
        if (!settlement.active) {
            continue;
        }

        std::uint64_t alive_members = 0;
        for (const auto& household : state_.households) {
            if (household.settlement_id != settlement.id) {
                continue;
            }
            for (const auto member_id : household.member_ids) {
                for (const auto& person : state_.people) {
                    if (person.id == member_id && person.alive) {
                        ++alive_members;
                        break;
                    }
                }
            }
        }

        if (alive_members == 0U) {
            settlement.active = false;
            settlement.access_share_points = 0U;
            emit_settlement_extinct_event(settlement.id, settlement.region_id);
        }
    }
}

void Simulation::process_settlement_founding(const std::uint64_t demographic_year) {
    std::unordered_set<PersonId> alive_person_ids;
    alive_person_ids.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        if (person.alive) {
            alive_person_ids.insert(person.id);
        }
    }

    std::vector<RegionId> region_ids;
    region_ids.reserve(state_.regions.size());
    for (const auto& region : state_.regions) {
        region_ids.push_back(region.id);
    }
    std::sort(region_ids.begin(), region_ids.end());

    for (const auto region_id : region_ids) {
        std::vector<Settlement*> active_settlements;
        active_settlements.reserve(state_.settlements.size());
        std::uint32_t allocated_share = 0;
        for (auto& settlement : state_.settlements) {
            if (!settlement.active || settlement.region_id != region_id) {
                continue;
            }
            active_settlements.push_back(&settlement);
            allocated_share = std::min<std::uint32_t>(kSettlementShareScale, allocated_share + settlement.access_share_points);
        }

        if (active_settlements.empty()) {
            continue;
        }

        const auto unallocated_share = (allocated_share >= kSettlementShareScale)
            ? 0U
            : (kSettlementShareScale - allocated_share);

        std::sort(active_settlements.begin(), active_settlements.end(), [](const Settlement* lhs, const Settlement* rhs) {
            return lhs->id < rhs->id;
        });

        Settlement* source = nullptr;
        std::uint32_t best_pressure = 0;

        for (auto* candidate : active_settlements) {
            std::uint64_t alive_households = 0;
            std::uint64_t eligible_workers = 0;
            std::int64_t required_food = 0;
            std::int64_t stored_food = std::max<std::int64_t>(0, candidate->commons_food.value);
            for (const auto& household : state_.households) {
                if (household.region_id != region_id || household.settlement_id != candidate->id) {
                    continue;
                }

                if (!checked_add_i64(stored_food, household.resources.food_inventory.value, stored_food)) {
                    throw std::overflow_error("Settlement founding stored food aggregation overflow");
                }

                bool has_alive = false;
                for (const auto member_id : household.member_ids) {
                    for (const auto& person : state_.people) {
                        if (person.id == member_id && person.alive) {
                            has_alive = true;
                            if (!checked_add_i64(required_food, food_requirement_for_person(person), required_food)) {
                                throw std::overflow_error("Settlement founding required food aggregation overflow");
                            }
                            if (occupation_eligible_for_productive_work(person)) {
                                ++eligible_workers;
                            }
                        }
                    }
                }

                if (has_alive) {
                    ++alive_households;
                }
            }

            if (alive_households < kFoundingMinimumSourceHouseholds) {
                continue;
            }

            if (required_food <= 0 || eligible_workers < kFoundingMinimumEligibleWorkers) {
                continue;
            }

            if (candidate->economic_viability_streak < kFoundingMinimumEconomicViabilityStreak) {
                continue;
            }

            const auto pressure = settlement_pressure_per_10k(*candidate);

            std::uint32_t candidate_assigned_share = 0U;
            if (unallocated_share >= kFoundingMinimumUnallocatedShare) {
                candidate_assigned_share = std::min<std::uint32_t>(unallocated_share, kFoundingDefaultShare);
            } else if (candidate->access_share_points > kFoundingMinimumSourceShareAfterSplit) {
                const auto transferable = candidate->access_share_points - kFoundingMinimumSourceShareAfterSplit;
                candidate_assigned_share = std::min<std::uint32_t>(transferable, kFoundingDefaultShare);
            }

            if (candidate_assigned_share == 0U) {
                continue;
            }

            if (source == nullptr || pressure > best_pressure || (pressure == best_pressure && candidate->id < source->id)) {
                source = candidate;
                best_pressure = pressure;
            }
        }

        if (source == nullptr) {
            continue;
        }

        std::vector<Household*> source_alive_households;
        source_alive_households.reserve(state_.households.size());
        for (auto& household : state_.households) {
            if (household.region_id != region_id || household.settlement_id != source->id) {
                continue;
            }

            bool has_alive = false;
            for (const auto member_id : household.member_ids) {
                for (const auto& person : state_.people) {
                    if (person.id == member_id && person.alive) {
                        has_alive = true;
                        break;
                    }
                }
                if (has_alive) {
                    break;
                }
            }

            if (has_alive) {
                source_alive_households.push_back(&household);
            }
        }

        if (source_alive_households.size() < kFoundingMinimumSourceHouseholds) {
            continue;
        }

        auto move_count = std::max<std::uint64_t>(kFoundingMinimumNewHouseholds, source_alive_households.size() / 4U);
        const auto max_movable = source_alive_households.size() - kFoundingMinimumRemainingHouseholds;
        if (max_movable < kFoundingMinimumNewHouseholds) {
            continue;
        }
        move_count = std::min<std::uint64_t>(move_count, max_movable);

        const auto new_settlement_id = next_settlement_id();
        const auto source_settlement_id = source->id;
        std::uint32_t assigned_share = 0U;
        if (unallocated_share >= kFoundingMinimumUnallocatedShare) {
            assigned_share = std::min<std::uint32_t>(unallocated_share, kFoundingDefaultShare);
        } else if (source->access_share_points > kFoundingMinimumSourceShareAfterSplit) {
            const auto transferable = source->access_share_points - kFoundingMinimumSourceShareAfterSplit;
            assigned_share = std::min<std::uint32_t>(transferable, kFoundingDefaultShare);
            source->access_share_points -= assigned_share;
        }

        if (assigned_share == 0U) {
            continue;
        }

        state_.settlements.push_back(Settlement{
            .id = new_settlement_id,
            .region_id = region_id,
            .active = true,
            .access_share_points = assigned_share,
        });

        std::sort(source_alive_households.begin(), source_alive_households.end(), [&](const Household* lhs, const Household* rhs) {
            const auto lhs_seed = derive_seed(household_seed(lhs->id), SeedDomain::SettlementHouseholdSelection, demographic_year ^ new_settlement_id);
            const auto rhs_seed = derive_seed(household_seed(rhs->id), SeedDomain::SettlementHouseholdSelection, demographic_year ^ new_settlement_id);
            if (lhs_seed.value != rhs_seed.value) {
                return lhs_seed.value < rhs_seed.value;
            }
            return lhs->id < rhs->id;
        });

        for (std::uint64_t i = 0; i < move_count; ++i) {
            source_alive_households[i]->settlement_id = new_settlement_id;
        }

        std::vector<PersonId> moved_observers;
        for (std::uint64_t i = 0; i < move_count; ++i) {
            for (const auto member_id : source_alive_households[i]->member_ids) {
                if (alive_person_ids.contains(member_id)) {
                    moved_observers.push_back(member_id);
                }
            }
        }
        std::sort(moved_observers.begin(), moved_observers.end());
        moved_observers.erase(std::unique(moved_observers.begin(), moved_observers.end()), moved_observers.end());

        const auto event_id = emit_settlement_founded_event(new_settlement_id, region_id, source_settlement_id);
        append_direct_observation_memories(event_id, moved_observers, state_.time);
    }

    std::sort(state_.settlements.begin(), state_.settlements.end(), [](const Settlement& lhs, const Settlement& rhs) {
        return lhs.id < rhs.id;
    });
}

void Simulation::process_settlement_restructuring(const std::uint64_t demographic_year) {
    process_settlement_extinction();
    process_settlement_founding(demographic_year);
    process_settlement_extinction();
}

void Simulation::clear_occupations_for_extinct_or_invalid_households() {
    std::unordered_map<HouseholdId, SettlementId> household_settlement;
    household_settlement.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        household_settlement.emplace(household.id, household.settlement_id);
    }

    std::unordered_map<SettlementId, bool> settlement_active;
    settlement_active.reserve(state_.settlements.size());
    for (const auto& settlement : state_.settlements) {
        settlement_active.emplace(settlement.id, settlement.active);
    }

    for (auto& person : state_.people) {
        if (!occupation_eligible_for_productive_work(person)) {
            person.occupation = Occupation::None;
            continue;
        }

        const auto household_it = household_settlement.find(person.household_id);
        if (household_it == household_settlement.end()) {
            person.occupation = Occupation::None;
            continue;
        }

        const auto settlement_it = settlement_active.find(household_it->second);
        if (settlement_it == settlement_active.end() || !settlement_it->second) {
            person.occupation = Occupation::None;
        }
    }
}

void Simulation::process_occupation_reassignment(const std::uint64_t demographic_year) {
    (void)demographic_year;
    clear_occupations_for_extinct_or_invalid_households();

    std::unordered_map<PersonId, Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::vector<SettlementId> settlement_ids;
    settlement_ids.reserve(state_.settlements.size());
    for (const auto& settlement : state_.settlements) {
        if (settlement.active) {
            settlement_ids.push_back(settlement.id);
        }
    }
    std::sort(settlement_ids.begin(), settlement_ids.end());

    for (const auto settlement_id : settlement_ids) {
        const auto* settlement = find_settlement(settlement_id);
        if (settlement == nullptr || !settlement->active) {
            continue;
        }

        const auto* region = find_region(settlement->region_id);
        if (region == nullptr) {
            continue;
        }

        std::int64_t settlement_required_food = 0;
        std::int64_t settlement_stored_food = std::max<std::int64_t>(0, settlement->commons_food.value);
        std::uint64_t settlement_alive_population = 0;

        struct Candidate {
            Person* person{};
            std::int64_t labour_capacity{};
            bool assigned{false};
        };

        std::vector<Candidate> candidates;
        candidates.reserve(128);

        for (const auto& household : state_.households) {
            if (household.settlement_id != settlement_id) {
                continue;
            }

            if (!checked_add_i64(
                    settlement_stored_food,
                    household.resources.food_inventory.value,
                    settlement_stored_food)) {
                throw std::overflow_error("Settlement stored food aggregation overflow");
            }

            for (const auto member_id : household.member_ids) {
                const auto person_it = person_by_id.find(member_id);
                if (person_it == person_by_id.end()) {
                    continue;
                }
                Person* person = person_it->second;
                if (!person->alive) {
                    continue;
                }

                ++settlement_alive_population;

                const auto required = food_requirement_for_person(*person);
                if (!checked_add_i64(settlement_required_food, required, settlement_required_food)) {
                    throw std::overflow_error("Settlement required food aggregation overflow");
                }

                if (!occupation_eligible_for_productive_work(*person)) {
                    person->occupation = Occupation::None;
                    continue;
                }

                candidates.push_back(Candidate{
                    .person = person,
                    .labour_capacity = productive_effort_for_person(*person),
                    .assigned = false,
                });
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
            return lhs.person->id < rhs.person->id;
        });

        const auto construction_demand = construction_demand_for_settlement(settlement_id, state_.buildings);

        const auto cultivation_opportunity =
            (region->productive_capacity_per_day * static_cast<std::int64_t>(settlement->access_share_points)) /
            static_cast<std::int64_t>(kSettlementShareScale);
        const auto gathering_opportunity =
            (std::min(region->wild_food_stock, region->wild_food_harvest_limit_per_day) *
             static_cast<std::int64_t>(settlement->access_share_points)) /
            static_cast<std::int64_t>(kSettlementShareScale);

        std::int64_t output_opportunity = 0;
        if (!checked_add_i64(cultivation_opportunity, gathering_opportunity, output_opportunity)) {
            throw std::overflow_error("Food output opportunity overflow");
        }

        const auto base_food_target = compute_daily_food_output_target(
            settlement_required_food,
            settlement_stored_food,
            output_opportunity);

        auto assignment_target = base_food_target;
        std::int64_t commons_target = 0;
        if (!checked_mul_i64(settlement_required_food, kCommonsReserveTargetDays, commons_target)) {
            throw std::overflow_error("Commons reserve target overflow");
        }
        commons_target = std::max<std::int64_t>(0, commons_target);
        const auto commons_deficit = std::max<std::int64_t>(0, commons_target - std::max<std::int64_t>(0, settlement->commons_food.value));
        if (commons_deficit > 0) {
            const auto replenish_target = ceil_div_u64(
                static_cast<std::uint64_t>(commons_deficit),
                static_cast<std::uint64_t>(std::max<std::int64_t>(1, kCommonsReplenishDays)));

            std::int64_t replenished_target = 0;
            if (!checked_add_i64(base_food_target, static_cast<std::int64_t>(replenish_target), replenished_target)) {
                throw std::overflow_error("Replenished assignment target overflow");
            }

            assignment_target = std::max(
                assignment_target,
                std::min<std::int64_t>(
                    output_opportunity,
                    replenished_target));
        }

        const auto pressure = settlement_pressure_per_10k(*settlement);
        const auto food_buffer_days = settlement_required_food > 0
            ? settlement_stored_food / std::max<std::int64_t>(1, settlement_required_food)
            : 0;
        const bool food_crisis =
            settlement_required_food > 0 &&
            (pressure >= kHighPressureThresholdPer10k || food_buffer_days < kFoodCrisisBufferDays);

        if (settlement_required_food > 0) {
            std::int64_t required_coverage_target = 0;
            if (!checked_mul_i64(settlement_required_food, kAssignmentRequiredCoveragePercent, required_coverage_target)) {
                throw std::overflow_error("Required food coverage target overflow");
            }
            required_coverage_target /= 100;
            assignment_target = std::max(assignment_target, required_coverage_target);
        }
        assignment_target = std::min(assignment_target, output_opportunity);
        const auto demand = split_demand_by_opportunity(assignment_target, cultivation_opportunity, gathering_opportunity);

        std::int64_t cultivation_remaining = std::max<std::int64_t>(0, demand.cultivation_labour_demand);
        std::int64_t gathering_remaining = std::max<std::int64_t>(0, demand.gathering_labour_demand);

        for (auto& candidate : candidates) {
            if (candidate.person->occupation != Occupation::SubsistenceCultivator || cultivation_remaining <= 0) {
                continue;
            }
            candidate.assigned = true;
            candidate.person->occupation = Occupation::SubsistenceCultivator;
            cultivation_remaining = std::max<std::int64_t>(0, cultivation_remaining - candidate.labour_capacity);
        }

        for (auto& candidate : candidates) {
            if (candidate.assigned || candidate.person->occupation != Occupation::WildFoodGatherer || gathering_remaining <= 0) {
                continue;
            }
            candidate.assigned = true;
            candidate.person->occupation = Occupation::WildFoodGatherer;
            gathering_remaining = std::max<std::int64_t>(0, gathering_remaining - candidate.labour_capacity);
        }

        for (auto& candidate : candidates) {
            if (candidate.assigned) {
                continue;
            }

            if (cultivation_remaining <= 0 && gathering_remaining <= 0) {
                break;
            }

            if (cultivation_remaining >= gathering_remaining && cultivation_remaining > 0) {
                candidate.person->occupation = Occupation::SubsistenceCultivator;
                candidate.assigned = true;
                cultivation_remaining = std::max<std::int64_t>(0, cultivation_remaining - candidate.labour_capacity);
            } else if (gathering_remaining > 0) {
                candidate.person->occupation = Occupation::WildFoodGatherer;
                candidate.assigned = true;
                gathering_remaining = std::max<std::int64_t>(0, gathering_remaining - candidate.labour_capacity);
            }
        }

        auto* settlement_mutable = find_settlement_mutable(settlement_id);
        if (settlement_mutable != nullptr) {
            std::uint64_t eligible_workers = 0;
            for (const auto& candidate : candidates) {
                if (candidate.labour_capacity > 0) {
                    ++eligible_workers;
                }
            }

            const auto targets = material_targets_for_settlement(settlement_alive_population, eligible_workers);
            const auto tool_deficit = std::max<std::int64_t>(0, targets.basic_tool_target - settlement_mutable->materials.basic_tool.value);
            std::int64_t timber_target_with_construction = 0;
            if (!checked_add_i64(targets.timber_target, construction_demand.timber_remaining, timber_target_with_construction)) {
                throw std::overflow_error("Settlement timber target overflow");
            }
            std::int64_t stone_target_with_construction = 0;
            if (!checked_add_i64(targets.stone_target, construction_demand.stone_remaining, stone_target_with_construction)) {
                throw std::overflow_error("Settlement stone target overflow");
            }
            const auto timber_deficit = std::max<std::int64_t>(0, timber_target_with_construction - settlement_mutable->materials.timber.value);
            const auto stone_deficit = std::max<std::int64_t>(0, stone_target_with_construction - settlement_mutable->materials.stone.value);

            const auto wood_reserve_need = std::max<std::int64_t>(0, targets.wood_target - settlement_mutable->materials.wood.value);
            std::int64_t timber_wood_need = 0;
            std::int64_t tool_wood_need = 0;
            if (!checked_mul_i64(timber_deficit, material_parameters_.wood_per_timber, timber_wood_need) ||
                !checked_mul_i64(tool_deficit, material_parameters_.wood_per_basic_tool, tool_wood_need)) {
                throw std::overflow_error("Settlement wood material need overflow");
            }
            std::int64_t wood_total_need = 0;
            if (!checked_add_i64(wood_reserve_need, timber_wood_need, wood_total_need) ||
                !checked_add_i64(wood_total_need, tool_wood_need, wood_total_need)) {
                throw std::overflow_error("Settlement total wood need overflow");
            }

            std::int64_t tool_stone_need = 0;
            if (!checked_mul_i64(tool_deficit, material_parameters_.stone_per_basic_tool, tool_stone_need)) {
                throw std::overflow_error("Settlement stone material need overflow");
            }
            std::int64_t stone_total_need = 0;
            if (!checked_add_i64(stone_deficit, tool_stone_need, stone_total_need)) {
                throw std::overflow_error("Settlement total stone need overflow");
            }

            std::int64_t craft_need = 0;
            if (!checked_add_i64(timber_deficit, tool_deficit, craft_need)) {
                throw std::overflow_error("Settlement craft need overflow");
            }
            const bool prioritize_craft_for_tools = tool_deficit > 0;

            if (food_crisis) {
                wood_total_need /= 4;
                stone_total_need /= 4;
                craft_need /= 4;
            }

            auto assign_for_occupation = [&](const Occupation occupation, std::int64_t& demand_value) {
                if (demand_value <= 0) {
                    return false;
                }

                for (auto& candidate : candidates) {
                    if (candidate.assigned || candidate.person->occupation != occupation) {
                        continue;
                    }
                    candidate.assigned = true;
                    candidate.person->occupation = occupation;
                    demand_value = std::max<std::int64_t>(0, demand_value - candidate.labour_capacity);
                    return true;
                }

                for (auto& candidate : candidates) {
                    if (candidate.assigned) {
                        continue;
                    }
                    candidate.assigned = true;
                    candidate.person->occupation = occupation;
                    demand_value = std::max<std::int64_t>(0, demand_value - candidate.labour_capacity);
                    return true;
                }

                return false;
            };

            while (true) {
                const auto has_demand = wood_total_need > 0 || stone_total_need > 0 || craft_need > 0;
                if (!has_demand) {
                    break;
                }

                bool any_unassigned = false;
                for (const auto& candidate : candidates) {
                    if (!candidate.assigned) {
                        any_unassigned = true;
                        break;
                    }
                }
                if (!any_unassigned) {
                    break;
                }

                bool assigned = false;
                if (prioritize_craft_for_tools && craft_need > 0 && wood_total_need <= 0 && stone_total_need <= 0) {
                    assigned = assign_for_occupation(Occupation::CraftWorker, craft_need);
                } else if (wood_total_need >= stone_total_need && wood_total_need >= craft_need && wood_total_need > 0) {
                    assigned = assign_for_occupation(Occupation::WoodGatherer, wood_total_need);
                } else if (stone_total_need >= craft_need && stone_total_need > 0) {
                    assigned = assign_for_occupation(Occupation::StoneExtractor, stone_total_need);
                } else if (craft_need > 0) {
                    assigned = assign_for_occupation(Occupation::CraftWorker, craft_need);
                } else {
                    break;
                }

                if (!assigned) {
                    break;
                }
            }
        }

        for (auto& candidate : candidates) {
            if (!candidate.assigned) {
                candidate.person->occupation = Occupation::None;
            }
        }
    }

    refresh_labour_aggregates();
}

void Simulation::refresh_labour_aggregates() {
    state_.eligible_workers = 0;
    state_.active_workers = 0;
    state_.unallocated_workers = 0;
    state_.cultivators = 0;
    state_.gatherers = 0;
    state_.wood_gatherers = 0;
    state_.stone_extractors = 0;
    state_.craft_workers = 0;
    state_.total_labour_capacity = 0;
    state_.total_labour_demand = 0;
    state_.total_labour_allocated = 0;
    state_.cultivation_labour_available = 0;
    state_.cultivation_labour_allocated = 0;
    state_.gathering_labour_available = 0;
    state_.gathering_labour_allocated = 0;
    state_.labour_shortfall = 0;
    state_.wood_labour_available = 0;
    state_.wood_labour_allocated = 0;
    state_.stone_labour_available = 0;
    state_.stone_labour_allocated = 0;
    state_.craft_labour_available = 0;
    state_.craft_labour_allocated = 0;
    state_.material_labour_allocated = 0;

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
        if (!occupation_eligible_for_productive_work(person)) {
            continue;
        }

        const auto capacity = productive_effort_for_person(person);
        ++state_.eligible_workers;
        state_.total_labour_capacity += capacity;

        if (person.occupation == Occupation::SubsistenceCultivator) {
            ++state_.active_workers;
            ++state_.cultivators;
            state_.cultivation_labour_available += capacity;
        } else if (person.occupation == Occupation::WildFoodGatherer) {
            ++state_.active_workers;
            ++state_.gatherers;
            state_.gathering_labour_available += capacity;
        } else if (person.occupation == Occupation::WoodGatherer) {
            ++state_.active_workers;
            ++state_.wood_gatherers;
            state_.wood_labour_available += capacity;
        } else if (person.occupation == Occupation::StoneExtractor) {
            ++state_.active_workers;
            ++state_.stone_extractors;
            state_.stone_labour_available += capacity;
        } else if (person.occupation == Occupation::CraftWorker) {
            ++state_.active_workers;
            ++state_.craft_workers;
            state_.craft_labour_available += capacity;
        } else {
            ++state_.unallocated_workers;
        }
    }

    for (const auto& settlement : state_.settlements) {
        if (!settlement.active) {
            continue;
        }

        const auto* region = find_region(settlement.region_id);
        if (region == nullptr) {
            continue;
        }

        std::int64_t required_food = 0;
        std::int64_t stored_food = 0;
        std::int64_t cultivation_available = 0;
        std::int64_t gathering_available = 0;

        for (const auto& household : state_.households) {
            if (household.settlement_id != settlement.id) {
                continue;
            }

            stored_food += household.resources.food_inventory.value;
            for (const auto member_id : household.member_ids) {
                const auto person_it = person_by_id.find(member_id);
                if (person_it == person_by_id.end() || !person_it->second->alive) {
                    continue;
                }

                const auto* person = person_it->second;
                required_food += food_requirement_for_person(*person);
                if (!occupation_eligible_for_productive_work(*person)) {
                    continue;
                }

                const auto capacity = productive_effort_for_person(*person);
                if (person->occupation == Occupation::SubsistenceCultivator) {
                    cultivation_available += capacity;
                } else if (person->occupation == Occupation::WildFoodGatherer) {
                    gathering_available += capacity;
                }
            }
        }

        const auto cultivation_opportunity =
            (region->productive_capacity_per_day * static_cast<std::int64_t>(settlement.access_share_points)) /
            static_cast<std::int64_t>(kSettlementShareScale);
        const auto gathering_opportunity =
            (std::min(region->wild_food_stock, region->wild_food_harvest_limit_per_day) *
             static_cast<std::int64_t>(settlement.access_share_points)) /
            static_cast<std::int64_t>(kSettlementShareScale);

        const auto output_target = compute_daily_food_output_target(
            required_food,
            stored_food,
            cultivation_opportunity + gathering_opportunity);
        const auto demand = split_demand_by_opportunity(output_target, cultivation_opportunity, gathering_opportunity);

        state_.total_labour_demand += demand.total();

        const auto cultivation_allocated = std::min(cultivation_available, std::max<std::int64_t>(0, demand.cultivation_labour_demand));
        const auto gathering_allocated = std::min(gathering_available, std::max<std::int64_t>(0, demand.gathering_labour_demand));
        state_.cultivation_labour_allocated += cultivation_allocated;
        state_.gathering_labour_allocated += gathering_allocated;
    }

    state_.total_labour_allocated = state_.cultivation_labour_allocated + state_.gathering_labour_allocated;
    state_.labour_shortfall = std::max<std::int64_t>(0, state_.total_labour_demand - state_.total_labour_allocated);
    state_.material_labour_allocated =
        state_.wood_labour_allocated + state_.stone_labour_allocated + state_.craft_labour_allocated;
    state_.material_labour_shortfall =
        std::max<std::int64_t>(0, state_.material_labour_demand - state_.material_labour_allocated);
}

void Simulation::process_mortality(const std::uint64_t demographic_year) {
    std::unordered_map<PersonId, Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<HouseholdId, Household*> household_by_id;
    household_by_id.reserve(state_.households.size());
    for (auto& household : state_.households) {
        household_by_id.emplace(household.id, &household);
    }

    for (auto& person : state_.people) {
        if (!person.alive) {
            continue;
        }

        const auto baseline = mortality_rate_per_10k(age_years(person));
        const auto food_penalty = mortality_penalty_from_person_health_per_10k(person);
        const auto mortality_rate = std::min<std::uint32_t>(10'000U, baseline + food_penalty);
        const auto mortality_seed = derive_seed(person_seed(person.id), SeedDomain::Mortality, demographic_year);
        if (config_.enable_unrelated_random_probe) {
            const auto probe_seed = derive_seed(person_seed(person.id), SeedDomain::UnrelatedProbe, demographic_year);
            (void)seed_random_u64(probe_seed);
        }

        if (seed_chance_per_10k(mortality_seed, mortality_rate)) {
            const auto household_id = person.household_id;
            person.alive = false;
            remove_from_household(household_id, person.id);
            ++state_.total_deaths;

            std::vector<PersonId> observers;
            const auto household_it = household_by_id.find(household_id);
            if (household_it != household_by_id.end()) {
                for (const auto member_id : household_it->second->member_ids) {
                    const auto person_it = person_by_id.find(member_id);
                    if (person_it != person_by_id.end() && person_it->second->alive) {
                        observers.push_back(member_id);
                    }
                }
            }

            const auto event_id = emit_death_event(person.id, household_id);
            append_direct_observation_memories(event_id, observers, state_.time);
        }
    }
}

void Simulation::process_births(const std::uint64_t demographic_year) {
    struct PendingBirth {
        PersonId mother_id{};
        std::optional<PersonId> father_id{};
        HouseholdId household_id{};
    };

    std::vector<PendingBirth> pending_births;
    pending_births.reserve(64);

    std::unordered_map<HouseholdId, Household*> household_by_id;
    household_by_id.reserve(state_.households.size());
    for (auto& household : state_.households) {
        household_by_id.emplace(household.id, &household);
    }

    std::unordered_set<PersonId> alive_person_ids;
    alive_person_ids.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        if (person.alive) {
            alive_person_ids.insert(person.id);
        }
    }

    for (const auto& person : state_.people) {
        if (!person.alive || person.sex != BiologicalSex::Female) {
            continue;
        }

        const auto fertility_rate = effective_fertility_rate_per_10k(person);
        if (fertility_rate == 0U) {
            continue;
        }

        if (!has_any_alive_adult_male()) {
            continue;
        }

        const auto fertility_seed = derive_seed(person_seed(person.id), SeedDomain::Fertility, demographic_year);
        if (config_.enable_unrelated_random_probe) {
            const auto probe_seed = derive_seed(person_seed(person.id), SeedDomain::UnrelatedProbe, demographic_year ^ 0xABCDEFU);
            (void)seed_random_u64(probe_seed);
        }

        if (seed_chance_per_10k(fertility_seed, fertility_rate)) {
            auto father_id = sample_father_in_household(person.household_id, fertility_seed);
            if (!father_id.has_value()) {
                father_id = sample_father_global(fertility_seed);
            }

            pending_births.push_back(PendingBirth{
                .mother_id = person.id,
                .father_id = father_id,
                .household_id = person.household_id,
            });
        }
    }

    for (const auto& birth : pending_births) {
        const auto newborn_id = next_person_id();
        const auto newborn_seed = derive_seed(person_seed(newborn_id), SeedDomain::Birth, demographic_year);
        const auto sex = sample_sex(newborn_seed) ? BiologicalSex::Female : BiologicalSex::Male;

        state_.people.push_back(Person{
            .id = newborn_id,
            .birth_year = current_year_signed(),
            .sex = sex,
            .alive = true,
            .household_id = birth.household_id,
            .mother_id = birth.mother_id,
            .father_id = birth.father_id,
        });

        auto household_it = household_by_id.find(birth.household_id);
        if (household_it != household_by_id.end()) {
            household_it->second->member_ids.push_back(newborn_id);
        }
        alive_person_ids.insert(newborn_id);

        ++state_.total_births;

        std::vector<PersonId> observers;
        const auto observer_household_it = household_by_id.find(birth.household_id);
        if (observer_household_it != household_by_id.end()) {
            for (const auto member_id : observer_household_it->second->member_ids) {
                if (member_id == newborn_id) {
                    continue;
                }
                if (alive_person_ids.contains(member_id)) {
                    observers.push_back(member_id);
                }
            }
        }
        if (alive_person_ids.contains(birth.mother_id) &&
            !std::binary_search(observers.begin(), observers.end(), birth.mother_id)) {
            const auto insert_it = std::lower_bound(observers.begin(), observers.end(), birth.mother_id);
            observers.insert(insert_it, birth.mother_id);
        }
        observers.erase(std::unique(observers.begin(), observers.end()), observers.end());

        const auto event_id = emit_birth_event(newborn_id, birth.household_id, birth.mother_id, birth.father_id);
        append_direct_observation_memories(event_id, observers, state_.time);
    }
}

EventId Simulation::append_event(SimulationEvent event) {
    if (!event_payload_matches_type(event)) {
        throw std::invalid_argument("Event payload does not match event type");
    }
    event.id = next_event_id_++;
    state_.event_log.push_back(event);
    return event.id;
}

EventId Simulation::emit_death_event(const PersonId person_id, const HouseholdId household_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::Death,
        .payload = DeathEventPayload{
            .person_id = person_id,
            .household_id = household_id,
        },
    });
    return event_id;
}

EventId Simulation::emit_birth_event(
    const PersonId child_id,
    const HouseholdId household_id,
    const PersonId mother_id,
    const std::optional<PersonId> father_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::Birth,
        .payload = BirthEventPayload{
            .child_id = child_id,
            .mother_id = mother_id,
            .father_id = father_id,
            .household_id = household_id,
        },
    });
    return event_id;
}

EventId Simulation::emit_settlement_founded_event(
    const SettlementId settlement_id,
    const RegionId region_id,
    const std::optional<SettlementId> source_settlement_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::SettlementFounded,
        .payload = SettlementFoundedEventPayload{
            .settlement_id = settlement_id,
            .region_id = region_id,
            .source_settlement_id = source_settlement_id,
        },
    });
    return event_id;
}

EventId Simulation::emit_settlement_extinct_event(const SettlementId settlement_id, const RegionId region_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::SettlementExtinct,
        .payload = SettlementExtinctEventPayload{
            .settlement_id = settlement_id,
            .region_id = region_id,
        },
    });
    return event_id;
}

EventId Simulation::emit_discovery_made_event(
    const std::uint32_t discovery_key,
    const PersonId discoverer_id,
    const SettlementId settlement_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::DiscoveryMade,
        .payload = DiscoveryMadeEventPayload{
            .discovery_key = discovery_key,
            .discoverer_id = discoverer_id,
            .settlement_id = settlement_id,
        },
    });
    return event_id;
}

EventId Simulation::emit_institution_founded_event(
    const InstitutionId institution_id,
    const InstitutionKind kind,
    const SettlementId settlement_id,
    const PersonId founder_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::InstitutionFounded,
        .payload = InstitutionFoundedEventPayload{
            .institution_id = institution_id,
            .kind = kind,
            .settlement_id = settlement_id,
            .founder_id = founder_id,
        },
    });
    return event_id;
}

MemoryId Simulation::append_memory(AgentMemory memory) {
    if (memory.owner_id == 0U || memory.source_event_id == 0U) {
        throw std::invalid_argument("Agent memory owner and source event ids must be non-zero");
    }
    if (memory.acquired_at.ticks < 0) {
        throw std::invalid_argument("Agent memory acquisition time must be non-negative");
    }
    if (memory.acquired_at > state_.time) {
        throw std::invalid_argument("Agent memory acquisition time must not exceed simulation time");
    }
    if (memory.confidence_per_10k > 10'000U) {
        throw std::invalid_argument("Agent memory confidence must be <= 10000");
    }
    if (memory.source != MemorySource::DirectObservation &&
        memory.source != MemorySource::Communication &&
        memory.source != MemorySource::Teaching &&
        memory.source != MemorySource::Record) {
        throw std::invalid_argument("Agent memory source is invalid");
    }

    memory.id = next_memory_id();
    state_.agent_memories.push_back(memory);
    derive_knowledge_from_memory(memory);
    return memory.id;
}

void Simulation::append_knowledge_transmission(KnowledgeTransmission transmission) {
    if (transmission.speaker_id == 0U ||
        transmission.recipient_id == 0U ||
        transmission.speaker_household_id == 0U ||
        transmission.recipient_household_id == 0U ||
        transmission.speaker_settlement_id == 0U ||
        transmission.recipient_settlement_id == 0U ||
        transmission.speaker_memory_id == 0U ||
        transmission.recipient_memory_id == 0U) {
        throw std::invalid_argument("Knowledge transmission references must be non-zero");
    }
    if (transmission.speaker_id == transmission.recipient_id) {
        throw std::invalid_argument("Knowledge transmission speaker and recipient must differ");
    }
    if (transmission.speaker_settlement_id != transmission.recipient_settlement_id) {
        throw std::invalid_argument("Knowledge transmission must remain within one settlement");
    }
    if (transmission.transmitted_at.ticks < 0 || transmission.transmitted_at > state_.time) {
        throw std::invalid_argument("Knowledge transmission time is invalid");
    }
    if (transmission.confidence_per_10k > 10'000U) {
        throw std::invalid_argument("Knowledge transmission confidence must be <= 10000");
    }

    const AgentMemory* speaker_memory = nullptr;
    const AgentMemory* recipient_memory = nullptr;
    for (const auto& memory : state_.agent_memories) {
        if (memory.id == transmission.speaker_memory_id) {
            speaker_memory = &memory;
        }
        if (memory.id == transmission.recipient_memory_id) {
            recipient_memory = &memory;
        }
        if (speaker_memory != nullptr && recipient_memory != nullptr) {
            break;
        }
    }

    if (speaker_memory == nullptr || recipient_memory == nullptr) {
        throw std::invalid_argument("Knowledge transmission must reference existing memories");
    }
    if (speaker_memory->owner_id != transmission.speaker_id) {
        throw std::invalid_argument("Knowledge transmission speaker mismatch");
    }
    if (recipient_memory->owner_id != transmission.recipient_id) {
        throw std::invalid_argument("Knowledge transmission recipient mismatch");
    }
    if (recipient_memory->source != MemorySource::Communication &&
        recipient_memory->source != MemorySource::Teaching) {
        throw std::invalid_argument("Knowledge transmission recipient memory must be communication-derived");
    }
    if (recipient_memory->source == MemorySource::Communication && transmission.method != CommunicationMethod::Communication) {
        throw std::invalid_argument("Knowledge transmission method/source mismatch for communication memory");
    }
    if (recipient_memory->source == MemorySource::Teaching && transmission.method != CommunicationMethod::Teaching) {
        throw std::invalid_argument("Knowledge transmission method/source mismatch for teaching memory");
    }
    if (speaker_memory->source_event_id != recipient_memory->source_event_id) {
        throw std::invalid_argument("Knowledge transmission memory event mismatch");
    }

    const Household* speaker_household = nullptr;
    const Household* recipient_household = nullptr;
    for (const auto& household : state_.households) {
        if (household.id == transmission.speaker_household_id) {
            speaker_household = &household;
        }
        if (household.id == transmission.recipient_household_id) {
            recipient_household = &household;
        }
        if (speaker_household != nullptr && recipient_household != nullptr) {
            break;
        }
    }

    if (speaker_household == nullptr || recipient_household == nullptr) {
        throw std::invalid_argument("Knowledge transmission households must exist");
    }
    if (speaker_household->settlement_id != transmission.speaker_settlement_id ||
        recipient_household->settlement_id != transmission.recipient_settlement_id) {
        throw std::invalid_argument("Knowledge transmission household/settlement mismatch");
    }

    const TransmissionSemanticKey semantic_key{
        .recipient_id = transmission.recipient_id,
        .speaker_memory_id = transmission.speaker_memory_id,
    };
    if (transmission_semantic_keys_.contains(semantic_key)) {
        throw std::invalid_argument("Duplicate knowledge transmission semantic key");
    }

    transmission.id = next_communication_id();
    state_.knowledge_transmissions.push_back(transmission);
    const auto index = state_.knowledge_transmissions.size() - 1U;
    communication_index_by_recipient_memory_[transmission.recipient_memory_id] = index;
    transmission_semantic_keys_.insert(semantic_key);
}

void Simulation::append_knowledge_record(KnowledgeRecord record) {
    if (record.settlement_id == 0U ||
        record.author_id == 0U ||
        record.author_household_id == 0U ||
        record.source_memory_id == 0U ||
        record.source_event_id == 0U) {
        throw std::invalid_argument("Knowledge record references must be non-zero");
    }
    if (record.recorded_at.ticks < 0 || record.recorded_at > state_.time) {
        throw std::invalid_argument("Knowledge record time is invalid");
    }
    if (record.confidence_per_10k > 10'000U) {
        throw std::invalid_argument("Knowledge record confidence must be <= 10000");
    }

    const Person* author = nullptr;
    for (const auto& person : state_.people) {
        if (person.id == record.author_id) {
            author = &person;
            break;
        }
    }
    if (author == nullptr || !author->alive) {
        throw std::invalid_argument("Knowledge record author must exist and be alive");
    }
    if (author->household_id != record.author_household_id) {
        throw std::invalid_argument("Knowledge record author household mismatch");
    }

    const Household* author_household = nullptr;
    for (const auto& household : state_.households) {
        if (household.id == record.author_household_id) {
            author_household = &household;
            break;
        }
    }
    if (author_household == nullptr) {
        throw std::invalid_argument("Knowledge record author household must exist");
    }
    if (author_household->settlement_id != record.settlement_id) {
        throw std::invalid_argument("Knowledge record settlement mismatch with author household");
    }

    const AgentMemory* source_memory = nullptr;
    for (const auto& memory : state_.agent_memories) {
        if (memory.id == record.source_memory_id) {
            source_memory = &memory;
            break;
        }
    }
    if (source_memory == nullptr) {
        throw std::invalid_argument("Knowledge record source memory must exist");
    }
    if (source_memory->owner_id != record.author_id) {
        throw std::invalid_argument("Knowledge record author must own source memory");
    }
    if (source_memory->source_event_id != record.source_event_id) {
        throw std::invalid_argument("Knowledge record source event mismatch");
    }
    if (source_memory->acquired_at > record.recorded_at) {
        throw std::invalid_argument("Knowledge record must not precede source memory");
    }

    const RecordSemanticKey semantic_key{
        .settlement_id = record.settlement_id,
        .source_event_id = record.source_event_id,
    };
    if (record_semantic_keys_.contains(semantic_key)) {
        throw std::invalid_argument("Duplicate knowledge record semantic key");
    }

    record.id = next_record_id();
    state_.knowledge_records.push_back(record);
    record_semantic_keys_.insert(semantic_key);
}

void Simulation::append_knowledge_record_access(KnowledgeRecordAccess access) {
    if (access.record_id == 0U ||
        access.reader_id == 0U ||
        access.reader_household_id == 0U ||
        access.reader_settlement_id == 0U ||
        access.reader_memory_id == 0U) {
        throw std::invalid_argument("Knowledge record access references must be non-zero");
    }
    if (access.read_at.ticks < 0 || access.read_at > state_.time) {
        throw std::invalid_argument("Knowledge record access time is invalid");
    }
    if (access.confidence_per_10k > 10'000U) {
        throw std::invalid_argument("Knowledge record access confidence must be <= 10000");
    }

    const KnowledgeRecord* record = nullptr;
    for (const auto& candidate : state_.knowledge_records) {
        if (candidate.id == access.record_id) {
            record = &candidate;
            break;
        }
    }
    if (record == nullptr) {
        throw std::invalid_argument("Knowledge record access must reference existing record");
    }
    if (record->settlement_id != access.reader_settlement_id) {
        throw std::invalid_argument("Knowledge record access must remain within record settlement");
    }
    if (access.read_at < record->recorded_at) {
        throw std::invalid_argument("Knowledge record access must not predate record creation");
    }

    const Person* reader = nullptr;
    for (const auto& person : state_.people) {
        if (person.id == access.reader_id) {
            reader = &person;
            break;
        }
    }
    if (reader == nullptr || !reader->alive) {
        throw std::invalid_argument("Knowledge record reader must exist and be alive");
    }
    if (reader->household_id != access.reader_household_id) {
        throw std::invalid_argument("Knowledge record access reader household mismatch");
    }

    const Household* reader_household = nullptr;
    for (const auto& household : state_.households) {
        if (household.id == access.reader_household_id) {
            reader_household = &household;
            break;
        }
    }
    if (reader_household == nullptr) {
        throw std::invalid_argument("Knowledge record access reader household must exist");
    }
    if (reader_household->settlement_id != access.reader_settlement_id) {
        throw std::invalid_argument("Knowledge record access settlement mismatch with reader household");
    }

    const AgentMemory* reader_memory = nullptr;
    for (const auto& memory : state_.agent_memories) {
        if (memory.id == access.reader_memory_id) {
            reader_memory = &memory;
            break;
        }
    }
    if (reader_memory == nullptr) {
        throw std::invalid_argument("Knowledge record access reader memory must exist");
    }
    if (reader_memory->owner_id != access.reader_id) {
        throw std::invalid_argument("Knowledge record access reader memory owner mismatch");
    }
    if (reader_memory->source != MemorySource::Record) {
        throw std::invalid_argument("Knowledge record access memory must be record-derived");
    }
    if (reader_memory->source_event_id != record->source_event_id) {
        throw std::invalid_argument("Knowledge record access event mismatch");
    }
    if (reader_memory->acquired_at != access.read_at) {
        throw std::invalid_argument("Knowledge record access time must match reader memory time");
    }
    if (reader_memory->confidence_per_10k != access.confidence_per_10k) {
        throw std::invalid_argument("Knowledge record access confidence must match reader memory");
    }

    const RecordAccessSemanticKey semantic_key{
        .record_id = access.record_id,
        .reader_id = access.reader_id,
    };
    if (record_access_semantic_keys_.contains(semantic_key)) {
        throw std::invalid_argument("Duplicate knowledge record access semantic key");
    }
    if (record_access_index_by_memory_.contains(access.reader_memory_id)) {
        throw std::invalid_argument("Knowledge record access memory id must be unique");
    }

    state_.knowledge_record_accesses.push_back(access);
    std::sort(state_.knowledge_record_accesses.begin(), state_.knowledge_record_accesses.end(), [](const KnowledgeRecordAccess& lhs, const KnowledgeRecordAccess& rhs) {
        if (lhs.record_id != rhs.record_id) {
            return lhs.record_id < rhs.record_id;
        }
        return lhs.reader_id < rhs.reader_id;
    });
    rebuild_record_indexes();
}

void Simulation::append_direct_observation_memories(
    const EventId source_event_id,
    std::span<const PersonId> observer_ids,
    const SimulationTime acquired_at) {
    for (const auto observer_id : observer_ids) {
        if (observer_id == 0U) {
            continue;
        }

        const auto memory_id = append_memory(AgentMemory{
            .id = 0,
            .owner_id = observer_id,
            .acquired_at = acquired_at,
            .source = MemorySource::DirectObservation,
            .source_event_id = source_event_id,
            .confidence_per_10k = 10'000U,
        });
        (void)memory_id;
    }
}

void Simulation::derive_knowledge_from_memory(const AgentMemory& memory) {
    if (memory.source != MemorySource::DirectObservation &&
        memory.source != MemorySource::Communication &&
        memory.source != MemorySource::Teaching &&
        memory.source != MemorySource::Record) {
        return;
    }

    if (memory.source_event_id == 0U || memory.source_event_id > state_.event_log.size()) {
        throw std::invalid_argument("Memory source event id is out of range for knowledge derivation");
    }

    const auto& source_event = state_.event_log[static_cast<std::size_t>(memory.source_event_id - 1U)];
    if (source_event.id != memory.source_event_id) {
        throw std::invalid_argument("Memory source event id does not match canonical event order");
    }

    if (source_event.type == EventType::Birth) {
        const auto& payload = std::get<BirthEventPayload>(source_event.payload);
        upsert_knowledge(
            memory.owner_id,
            memory.acquired_at,
            memory.id,
            memory.confidence_per_10k,
            KnowledgeFact{PersonLifeStatusKnowledgeFact{
                .person_id = payload.child_id,
                .status = KnownLifeStatus::Alive,
            }},
            KnowledgeFactKind::PersonLifeStatus,
            payload.child_id,
            0U);

        upsert_knowledge(
            memory.owner_id,
            memory.acquired_at,
            memory.id,
            memory.confidence_per_10k,
            KnowledgeFact{PersonHouseholdKnowledgeFact{
                .person_id = payload.child_id,
                .household_id = payload.household_id,
            }},
            KnowledgeFactKind::PersonHousehold,
            payload.child_id,
            0U);
        return;
    }

    if (source_event.type == EventType::Death) {
        const auto& payload = std::get<DeathEventPayload>(source_event.payload);
        upsert_knowledge(
            memory.owner_id,
            memory.acquired_at,
            memory.id,
            memory.confidence_per_10k,
            KnowledgeFact{PersonLifeStatusKnowledgeFact{
                .person_id = payload.person_id,
                .status = KnownLifeStatus::Dead,
            }},
            KnowledgeFactKind::PersonLifeStatus,
            payload.person_id,
            0U);
        return;
    }

    if (source_event.type == EventType::SettlementFounded) {
        const auto& payload = std::get<SettlementFoundedEventPayload>(source_event.payload);
        upsert_knowledge(
            memory.owner_id,
            memory.acquired_at,
            memory.id,
            memory.confidence_per_10k,
            KnowledgeFact{SettlementStatusKnowledgeFact{
                .settlement_id = payload.settlement_id,
                .region_id = payload.region_id,
                .active = true,
            }},
            KnowledgeFactKind::SettlementStatus,
            payload.settlement_id,
            0U);
        return;
    }

    if (source_event.type == EventType::SettlementExtinct) {
        const auto& payload = std::get<SettlementExtinctEventPayload>(source_event.payload);
        upsert_knowledge(
            memory.owner_id,
            memory.acquired_at,
            memory.id,
            memory.confidence_per_10k,
            KnowledgeFact{SettlementStatusKnowledgeFact{
                .settlement_id = payload.settlement_id,
                .region_id = payload.region_id,
                .active = false,
            }},
            KnowledgeFactKind::SettlementStatus,
            payload.settlement_id,
            0U);
        return;
    }

    if (source_event.type == EventType::DiscoveryMade) {
        const auto& payload = std::get<DiscoveryMadeEventPayload>(source_event.payload);
        upsert_knowledge(
            memory.owner_id,
            memory.acquired_at,
            memory.id,
            memory.confidence_per_10k,
            KnowledgeFact{BasicDiscoveryKnowledgeFact{
                .discovery_key = payload.discovery_key,
            }},
            KnowledgeFactKind::BasicDiscovery,
            payload.discovery_key,
            0U);
        return;
    }
}

void Simulation::upsert_knowledge(
    const PersonId owner_id,
    const SimulationTime acquired_at,
    const MemoryId source_memory_id,
    const std::uint32_t confidence_per_10k,
    KnowledgeFact fact,
    const KnowledgeFactKind fact_kind,
    const std::uint64_t primary_subject_id,
    const std::uint64_t secondary_subject_id) {
    if (owner_id == 0U || source_memory_id == 0U || primary_subject_id == 0U) {
        throw std::invalid_argument("Knowledge upsert references must be non-zero");
    }
    if (acquired_at.ticks < 0 || acquired_at > state_.time) {
        throw std::invalid_argument("Knowledge upsert acquisition time is invalid");
    }
    if (confidence_per_10k > 10'000U) {
        throw std::invalid_argument("Knowledge confidence must be <= 10000");
    }

    const KnowledgeSemanticKey semantic_key{
        .owner_id = owner_id,
        .fact_kind = fact_kind,
        .primary_subject_id = primary_subject_id,
        .secondary_subject_id = secondary_subject_id,
    };

    const auto existing_it = knowledge_index_by_semantic_key_.find(semantic_key);
    if (existing_it == knowledge_index_by_semantic_key_.end()) {
        AgentKnowledge knowledge{};
        knowledge.id = next_knowledge_id();
        knowledge.owner_id = owner_id;
        knowledge.acquired_at = acquired_at;
        knowledge.updated_at = acquired_at;
        knowledge.source = KnowledgeSource::MemoryDerived;
        knowledge.source_memory_id = source_memory_id;
        knowledge.fact = std::move(fact);
        knowledge.confidence_per_10k = confidence_per_10k;

        state_.agent_knowledge.push_back(std::move(knowledge));
        const auto index = state_.agent_knowledge.size() - 1U;
        knowledge_indices_by_owner_[owner_id].push_back(index);
        knowledge_index_by_semantic_key_.emplace(semantic_key, index);
        return;
    }

    if (existing_it->second >= state_.agent_knowledge.size()) {
        throw std::invalid_argument("Knowledge semantic index is out of range");
    }

    auto& knowledge = state_.agent_knowledge[existing_it->second];
    if (knowledge.owner_id != owner_id) {
        throw std::invalid_argument("Knowledge owner mismatch during upsert");
    }

    if (fact_kind == KnowledgeFactKind::PersonLifeStatus) {
        const auto* existing_life = std::get_if<PersonLifeStatusKnowledgeFact>(&knowledge.fact);
        const auto* incoming_life = std::get_if<PersonLifeStatusKnowledgeFact>(&fact);
        if (existing_life != nullptr && incoming_life != nullptr &&
            existing_life->status == KnownLifeStatus::Dead &&
            incoming_life->status == KnownLifeStatus::Alive) {
            return;
        }
    }

    if (acquired_at < knowledge.acquired_at) {
        knowledge.acquired_at = acquired_at;
    }

    if (acquired_at > knowledge.updated_at ||
        (acquired_at == knowledge.updated_at && source_memory_id >= knowledge.source_memory_id)) {
        knowledge.updated_at = acquired_at;
        knowledge.source = KnowledgeSource::MemoryDerived;
        knowledge.source_memory_id = source_memory_id;
        knowledge.fact = std::move(fact);
        knowledge.confidence_per_10k = confidence_per_10k;
    }
}

void Simulation::rebuild_knowledge_indexes() {
    knowledge_indices_by_owner_.clear();
    knowledge_index_by_semantic_key_.clear();
    knowledge_indices_by_owner_.reserve(state_.agent_knowledge.size());
    knowledge_index_by_semantic_key_.reserve(state_.agent_knowledge.size());

    for (std::size_t index = 0; index < state_.agent_knowledge.size(); ++index) {
        const auto& knowledge = state_.agent_knowledge[index];
        const auto descriptor = describe_knowledge_fact(knowledge.fact);

        const KnowledgeSemanticKey semantic_key{
            .owner_id = knowledge.owner_id,
            .fact_kind = descriptor.kind,
            .primary_subject_id = descriptor.primary_subject_id,
            .secondary_subject_id = descriptor.secondary_subject_id,
        };

        const auto inserted = knowledge_index_by_semantic_key_.emplace(semantic_key, index);
        if (!inserted.second) {
            throw std::invalid_argument("Duplicate knowledge semantic key found while rebuilding indexes");
        }
        knowledge_indices_by_owner_[knowledge.owner_id].push_back(index);
    }
}

void Simulation::rebuild_communication_indexes() {
    communication_index_by_recipient_memory_.clear();
    transmission_semantic_keys_.clear();
    communication_index_by_recipient_memory_.reserve(state_.knowledge_transmissions.size());
    transmission_semantic_keys_.reserve(state_.knowledge_transmissions.size());

    for (std::size_t index = 0; index < state_.knowledge_transmissions.size(); ++index) {
        const auto& transmission = state_.knowledge_transmissions[index];
        communication_index_by_recipient_memory_[transmission.recipient_memory_id] = index;
        transmission_semantic_keys_.insert(TransmissionSemanticKey{
            .recipient_id = transmission.recipient_id,
            .speaker_memory_id = transmission.speaker_memory_id,
        });
    }
}

void Simulation::rebuild_record_indexes() {
    record_semantic_keys_.clear();
    record_access_semantic_keys_.clear();
    record_access_index_by_memory_.clear();
    record_semantic_keys_.reserve(state_.knowledge_records.size());
    record_access_semantic_keys_.reserve(state_.knowledge_record_accesses.size());
    record_access_index_by_memory_.reserve(state_.knowledge_record_accesses.size());

    for (const auto& record : state_.knowledge_records) {
        record_semantic_keys_.insert(RecordSemanticKey{
            .settlement_id = record.settlement_id,
            .source_event_id = record.source_event_id,
        });
    }

    for (std::size_t index = 0; index < state_.knowledge_record_accesses.size(); ++index) {
        const auto& access = state_.knowledge_record_accesses[index];
        record_access_semantic_keys_.insert(RecordAccessSemanticKey{
            .record_id = access.record_id,
            .reader_id = access.reader_id,
        });
        record_access_index_by_memory_[access.reader_memory_id] = index;
    }
}

void Simulation::refresh_population_count() noexcept {
    std::uint64_t alive_count = 0;
    for (const auto& person : state_.people) {
        if (person.alive) {
            ++alive_count;
        }
    }
    state_.population = alive_count;
}

void Simulation::refresh_food_aggregates() noexcept {
    state_.total_food_stored = 0;
    state_.total_food_produced = 0;
    state_.total_food_consumed = 0;
    state_.total_unmet_food = 0;
    state_.households_in_shortage = 0;
    state_.population_under_food_stress = 0;

    for (const auto& household : state_.households) {
        state_.total_food_stored += household.resources.food_inventory.value;
        state_.total_food_produced += household.resources.food_produced_total;
        state_.total_food_consumed += household.resources.food_consumed_total;
        state_.total_unmet_food += household.resources.unmet_food_total;

        const bool shortage = household.resources.consecutive_shortage_days > 0U;
        if (shortage) {
            ++state_.households_in_shortage;
        }

    }

    for (const auto& settlement : state_.settlements) {
        state_.total_food_stored += std::max<std::int64_t>(0, settlement.commons_food.value);
    }

    for (const auto& person : state_.people) {
        if (!person.alive) {
            continue;
        }

        if (person.nutrition_points < 550 || person.health_points < 500) {
            ++state_.population_under_food_stress;
        }
    }
}

void Simulation::refresh_environment_aggregates() noexcept {
    state_.total_productive_capacity_per_day = 0;
    state_.total_wild_food_capacity = 0;
    state_.total_wild_food_stock = 0;
    state_.total_wild_food_harvested = 0;
    state_.total_wild_food_regenerated = 0;
    state_.total_wood_capacity = 0;
    state_.total_wood_stock = 0;
    state_.total_stone_capacity = 0;
    state_.total_stone_remaining = 0;
    state_.total_wood_harvested = 0;
    state_.total_wood_regenerated = 0;
    state_.total_stone_extracted = 0;

    for (const auto& region : state_.regions) {
        state_.total_productive_capacity_per_day += region.productive_capacity_per_day;
        state_.total_wild_food_capacity += region.wild_food_capacity;
        state_.total_wild_food_stock += region.wild_food_stock;
        state_.total_wild_food_harvested += region.cumulative_wild_food_harvested;
        state_.total_wild_food_regenerated += region.cumulative_wild_food_regenerated;
        state_.total_wood_capacity += region.wood_capacity;
        state_.total_wood_stock += region.wood_stock;
        state_.total_stone_capacity += region.stone_capacity;
        state_.total_stone_remaining += region.stone_remaining;
        state_.total_wood_harvested += region.cumulative_wood_harvested;
        state_.total_wood_regenerated += region.cumulative_wood_regenerated;
        state_.total_stone_extracted += region.cumulative_stone_extracted;
    }
}

void Simulation::refresh_material_aggregates() noexcept {
    state_.total_wood_inventory = 0;
    state_.total_stone_inventory = 0;
    state_.total_timber_inventory = 0;
    state_.total_basic_tool_inventory = 0;
    state_.total_timber_produced = 0;
    state_.total_basic_tool_produced = 0;
    state_.total_basic_tool_broken = 0;
    state_.average_tool_coverage_per_10k = 0;
    std::uint64_t active_settlement_count = 0;
    std::uint64_t coverage_sum = 0;

    for (const auto& settlement : state_.settlements) {
        state_.total_wood_inventory += settlement.materials.wood.value;
        state_.total_stone_inventory += settlement.materials.stone.value;
        state_.total_timber_inventory += settlement.materials.timber.value;
        state_.total_basic_tool_inventory += settlement.materials.basic_tool.value;
        state_.total_timber_produced += settlement.cumulative_timber_produced;
        state_.total_basic_tool_produced += settlement.cumulative_basic_tool_produced;
        state_.total_basic_tool_broken += settlement.cumulative_basic_tool_broken;
        if (settlement.active) {
            ++active_settlement_count;
            coverage_sum += settlement.last_tool_coverage_per_10k;
        }
    }

    if (active_settlement_count > 0U) {
        state_.average_tool_coverage_per_10k = bounded_u32(coverage_sum / active_settlement_count);
    }
}

void Simulation::process_household_access_allocation() {
    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<SettlementId, std::vector<Household*>> households_by_settlement;
    households_by_settlement.reserve(state_.settlements.size());
    for (auto& household : state_.households) {
        household.productive_access_share_points = 0U;
        household.commons_access_share_points = 0U;

        bool has_alive_member = false;
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it != person_by_id.end() && person_it->second->alive) {
                has_alive_member = true;
                break;
            }
        }

        if (has_alive_member) {
            households_by_settlement[household.settlement_id].push_back(&household);
        }
    }

    for (auto& [settlement_id, households] : households_by_settlement) {
        std::sort(households.begin(), households.end(), [](const Household* lhs, const Household* rhs) {
            return lhs->id < rhs->id;
        });

        std::vector<std::int64_t> capacities(households.size(), 0);
        std::int64_t total_capacity = 0;
        for (std::size_t i = 0; i < households.size(); ++i) {
            auto* household = households[i];
            std::int64_t capacity = 0;
            for (const auto member_id : household->member_ids) {
                const auto person_it = person_by_id.find(member_id);
                if (person_it == person_by_id.end()) {
                    continue;
                }

                const auto* person = person_it->second;
                if (!occupation_eligible_for_productive_work(*person)) {
                    continue;
                }
                capacity += productive_effort_for_person(*person);
            }
            capacities[i] = std::max<std::int64_t>(0, capacity);
            total_capacity += capacities[i];
        }

        if (households.empty()) {
            continue;
        }

        if (total_capacity <= 0) {
            const auto base = kSettlementShareScale / static_cast<std::uint32_t>(households.size());
            auto remainder = static_cast<std::uint32_t>(kSettlementShareScale - (base * static_cast<std::uint32_t>(households.size())));
            for (auto* household : households) {
                household->productive_access_share_points = base;
                if (remainder > 0U) {
                    ++household->productive_access_share_points;
                    --remainder;
                }
                household->commons_access_share_points = household->productive_access_share_points;
            }
            continue;
        }

        std::uint32_t allocated = 0;
        for (std::size_t i = 0; i < households.size(); ++i) {
            auto* household = households[i];
            const auto share = static_cast<std::uint32_t>(
                std::min<std::int64_t>(
                    kSettlementShareScale,
                    (capacities[i] * static_cast<std::int64_t>(kSettlementShareScale)) / total_capacity));
            household->productive_access_share_points = share;
            household->commons_access_share_points = share;
            allocated = std::min<std::uint32_t>(kSettlementShareScale, allocated + share);
        }

        auto remainder = static_cast<std::int64_t>(kSettlementShareScale) - static_cast<std::int64_t>(allocated);
        for (auto* household : households) {
            if (remainder <= 0) {
                break;
            }
            ++household->productive_access_share_points;
            ++household->commons_access_share_points;
            --remainder;
        }
        (void)settlement_id;
    }
}

void Simulation::process_household_stabilization_transfers() {
    std::unordered_map<SettlementId, Settlement*> settlement_by_id;
    settlement_by_id.reserve(state_.settlements.size());
    for (auto& settlement : state_.settlements) {
        settlement_by_id.emplace(settlement.id, &settlement);
    }

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    for (auto& household : state_.households) {
        auto settlement_it = settlement_by_id.find(household.settlement_id);
        if (settlement_it == settlement_by_id.end() || !settlement_it->second->active) {
            continue;
        }

        std::int64_t required_food = 0;
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end() || !person_it->second->alive) {
                continue;
            }
            required_food += food_requirement_for_person(*person_it->second);
        }

        const auto reserve = required_food * kFoodCommonsTransferReserveDays;
        auto surplus = household.resources.food_inventory.value - reserve;
        if (surplus <= 0) {
            continue;
        }

        auto* settlement = settlement_it->second;
        household.resources.food_inventory.value -= surplus;
        settlement->commons_food.value += surplus;
        household.cumulative_food_transferred_out += surplus;
        settlement->last_transfer_food_volume += surplus;
    }

    for (auto& household : state_.households) {
        auto settlement_it = settlement_by_id.find(household.settlement_id);
        if (settlement_it == settlement_by_id.end() || !settlement_it->second->active) {
            continue;
        }

        std::int64_t required_food = 0;
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end() || !person_it->second->alive) {
                continue;
            }
            required_food += food_requirement_for_person(*person_it->second);
        }
        if (required_food <= 0) {
            continue;
        }

        const auto reserve = required_food * kFoodCommonsTransferReserveDays;
        if (household.resources.food_inventory.value >= reserve) {
            continue;
        }

        auto* settlement = settlement_it->second;
        const auto need = reserve - household.resources.food_inventory.value;
        const auto transfer = std::min<std::int64_t>(need, std::max<std::int64_t>(0, settlement->commons_food.value));
        if (transfer <= 0) {
            continue;
        }

        settlement->commons_food.value -= transfer;
        household.resources.food_inventory.value += transfer;
        household.cumulative_food_transferred_in += transfer;
        settlement->last_transfer_food_volume += transfer;
    }
}

void Simulation::process_local_exchange(const std::uint64_t demographic_year) {
    (void)demographic_year;

    std::unordered_map<SettlementId, std::vector<Household*>> households_by_settlement;
    households_by_settlement.reserve(state_.settlements.size());
    for (auto& household : state_.households) {
        households_by_settlement[household.settlement_id].push_back(&household);
    }

    for (auto& settlement : state_.settlements) {
        auto it = households_by_settlement.find(settlement.id);
        if (it == households_by_settlement.end()) {
            continue;
        }

        auto& households = it->second;
        std::sort(households.begin(), households.end(), [](const Household* lhs, const Household* rhs) {
            return lhs->id < rhs->id;
        });

        if (households.empty()) {
            continue;
        }

        std::vector<std::uint32_t> shares;
        shares.reserve(households.size());
        std::uint32_t total_share = 0;
        for (const auto* household : households) {
            shares.push_back(household->productive_access_share_points);
            total_share += household->productive_access_share_points;
        }

        if (total_share == 0U) {
            shares.assign(households.size(), 1U);
            total_share = static_cast<std::uint32_t>(households.size());
        }

        auto allocate_and_apply = [&](const std::int64_t total_units, auto&& getter) {
            const auto capped_total = std::max<std::int64_t>(0, total_units);
            std::vector<std::int64_t> allocations(households.size(), 0);

            std::int64_t allocated = 0;
            for (std::size_t i = 0; i < households.size(); ++i) {
                const auto share_i64 = static_cast<std::int64_t>(shares[i]);
                const auto divisor = static_cast<std::int64_t>(total_share);
                const auto quotient = capped_total / divisor;
                const auto remainder = capped_total % divisor;

                if (quotient > 0 && share_i64 > 0 && quotient > (std::numeric_limits<std::int64_t>::max() / share_i64)) {
                    throw std::overflow_error("Exchange ownership allocation overflow");
                }

                const auto base = quotient * share_i64;
                const auto rem_component = (remainder * share_i64) / divisor;
                if (base > (std::numeric_limits<std::int64_t>::max() - rem_component)) {
                    throw std::overflow_error("Exchange ownership allocation overflow");
                }

                const auto value = base + rem_component;
                allocations[i] = std::max<std::int64_t>(0, value);
                if (!checked_add_i64(allocated, allocations[i], allocated)) {
                    throw std::overflow_error("Exchange allocation accumulation overflow");
                }
            }

            auto remainder = capped_total - allocated;
            for (std::size_t i = 0; i < households.size() && remainder > 0; ++i) {
                ++allocations[i];
                --remainder;
            }

            std::int64_t movement = 0;
            for (std::size_t i = 0; i < households.size(); ++i) {
                auto* household = households[i];
                const auto old_value = getter(*household);
                const auto new_value = allocations[i];

                if (new_value > old_value) {
                    household->cumulative_material_transferred_in += (new_value - old_value);
                    movement += (new_value - old_value);
                } else if (old_value > new_value) {
                    household->cumulative_material_transferred_out += (old_value - new_value);
                    movement += (old_value - new_value);
                }

                getter(*household) = new_value;
            }

            settlement.last_exchange_volume += movement / 2;
        };

        allocate_and_apply(settlement.materials.wood.value, [](Household& household) -> std::int64_t& {
            return household.owned_materials.wood.value;
        });
        allocate_and_apply(settlement.materials.stone.value, [](Household& household) -> std::int64_t& {
            return household.owned_materials.stone.value;
        });
        allocate_and_apply(settlement.materials.timber.value, [](Household& household) -> std::int64_t& {
            return household.owned_materials.timber.value;
        });
        allocate_and_apply(settlement.materials.basic_tool.value, [](Household& household) -> std::int64_t& {
            return household.owned_materials.basic_tool.value;
        });
    }
}

void Simulation::process_construction(const std::uint64_t demographic_year) {
    (void)demographic_year;

    std::unordered_map<SettlementId, std::uint64_t> alive_population_by_settlement;
    std::unordered_map<SettlementId, std::uint64_t> eligible_workers_by_settlement;
    alive_population_by_settlement.reserve(state_.settlements.size());
    eligible_workers_by_settlement.reserve(state_.settlements.size());

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    for (const auto& household : state_.households) {
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end() || !person_it->second->alive) {
                continue;
            }

            ++alive_population_by_settlement[household.settlement_id];
            if (occupation_eligible_for_productive_work(*person_it->second)) {
                ++eligible_workers_by_settlement[household.settlement_id];
            }
        }
    }

    auto find_building_mutable = [&](const BuildingId building_id) -> Building* {
        for (auto& building : state_.buildings) {
            if (building.id == building_id) {
                return &building;
            }
        }
        return nullptr;
    };

    std::unordered_map<SettlementId, std::vector<BuildingId>> buildings_by_settlement;
    buildings_by_settlement.reserve(state_.settlements.size());
    for (const auto& building : state_.buildings) {
        buildings_by_settlement[building.settlement_id].push_back(building.id);
    }

    for (auto& settlement : state_.settlements) {
        if (!settlement.active) {
            continue;
        }

        auto& settlement_buildings = buildings_by_settlement[settlement.id];
        std::sort(settlement_buildings.begin(), settlement_buildings.end());

        std::uint64_t complete_dwellings = 0;
        std::uint64_t complete_food_stores = 0;
        std::uint64_t complete_workshops = 0;
        std::uint64_t active_dwellings = 0;
        std::uint64_t active_food_stores = 0;
        std::uint64_t active_workshops = 0;

        for (const auto building_id : settlement_buildings) {
            const auto* building = find_building_mutable(building_id);
            if (building == nullptr) {
                continue;
            }
            const bool is_complete = building->state == ConstructionState::Complete;
            if (building->type == BuildingType::Dwelling) {
                is_complete ? ++complete_dwellings : ++active_dwellings;
            } else if (building->type == BuildingType::FoodStore) {
                is_complete ? ++complete_food_stores : ++active_food_stores;
            } else if (building->type == BuildingType::Workshop) {
                is_complete ? ++complete_workshops : ++active_workshops;
            }
        }

        const auto alive_population = alive_population_by_settlement.contains(settlement.id)
            ? alive_population_by_settlement[settlement.id]
            : 0U;
        const auto eligible_workers = eligible_workers_by_settlement.contains(settlement.id)
            ? eligible_workers_by_settlement[settlement.id]
            : 0U;

        std::int64_t required_food = 0;
        std::int64_t stored_food = std::max<std::int64_t>(0, settlement.commons_food.value);
        std::int64_t unallocated_labour_capacity = 0;
        for (const auto& household : state_.households) {
            if (household.settlement_id != settlement.id) {
                continue;
            }

            if (!checked_add_i64(stored_food, household.resources.food_inventory.value, stored_food)) {
                throw std::overflow_error("Construction stored food aggregation overflow");
            }

            for (const auto member_id : household.member_ids) {
                const auto person_it = person_by_id.find(member_id);
                if (person_it == person_by_id.end() || !person_it->second->alive) {
                    continue;
                }

                const auto& person = *person_it->second;
                if (!checked_add_i64(required_food, food_requirement_for_person(person), required_food)) {
                    throw std::overflow_error("Construction required food aggregation overflow");
                }
                if (!occupation_eligible_for_productive_work(person) || person.occupation != Occupation::None) {
                    continue;
                }

                if (!checked_add_i64(unallocated_labour_capacity, productive_effort_for_person(person), unallocated_labour_capacity)) {
                    throw std::overflow_error("Construction unallocated labour overflow");
                }
            }
        }

        const auto pressure = settlement_pressure_per_10k(settlement);
        const auto food_buffer_days = required_food > 0
            ? stored_food / std::max<std::int64_t>(1, required_food)
            : 0;
        const bool subsistence_constrained =
            required_food > 0 &&
            (pressure >= kHighPressureThresholdPer10k || food_buffer_days < kFoodCrisisBufferDays);

        const auto target_dwellings = std::max<std::uint64_t>(1U, ceil_div_u64(alive_population, 30U));
        const auto target_food_stores = std::max<std::uint64_t>(1U, ceil_div_u64(alive_population, 60U));
        const auto target_workshops = std::max<std::uint64_t>(1U, ceil_div_u64(eligible_workers, 45U));

        auto queue_project = [&](const BuildingType type, const std::int64_t timber_required, const std::int64_t stone_required, const std::int64_t labour_required) {
            Building project{};
            project.id = next_building_id();
            project.type = type;
            project.settlement_id = settlement.id;
            project.settlement_commons_owned = true;
            project.state = ConstructionState::Planned;
            project.timber_required = timber_required;
            project.stone_required = stone_required;
            project.labour_required = labour_required;
            project.started_at = state_.time;
            state_.buildings.push_back(project);
            settlement_buildings.push_back(project.id);
        };

        if (complete_dwellings + active_dwellings < target_dwellings) {
            queue_project(BuildingType::Dwelling, kDwellingTimberRequired, kDwellingStoneRequired, kDwellingLabourRequired);
        }
        if (complete_food_stores + active_food_stores < target_food_stores) {
            queue_project(BuildingType::FoodStore, kFoodStoreTimberRequired, kFoodStoreStoneRequired, kFoodStoreLabourRequired);
        }
        if (complete_workshops + active_workshops < target_workshops) {
            queue_project(BuildingType::Workshop, kWorkshopTimberRequired, kWorkshopStoneRequired, kWorkshopLabourRequired);
        }

        std::sort(settlement_buildings.begin(), settlement_buildings.end());

        auto labour_budget = subsistence_constrained ? 0 : std::max<std::int64_t>(0, unallocated_labour_capacity);
        for (const auto building_id : settlement_buildings) {
            auto* building = find_building_mutable(building_id);
            if (building == nullptr) {
                continue;
            }
            if (building->state == ConstructionState::Complete) {
                continue;
            }

            if (building->state == ConstructionState::Planned) {
                building->state = ConstructionState::ActiveConstruction;
            }

            const auto timber_need = std::max<std::int64_t>(0, building->timber_required - building->timber_committed);
            const auto timber_commit = std::min<std::int64_t>(timber_need, std::max<std::int64_t>(0, settlement.materials.timber.value));
            settlement.materials.timber.value -= timber_commit;
            building->timber_committed += timber_commit;

            const auto stone_need = std::max<std::int64_t>(0, building->stone_required - building->stone_committed);
            const auto stone_commit = std::min<std::int64_t>(stone_need, std::max<std::int64_t>(0, settlement.materials.stone.value));
            settlement.materials.stone.value -= stone_commit;
            building->stone_committed += stone_commit;

            const auto labour_need = std::max<std::int64_t>(0, building->labour_required - building->labour_completed);
            const auto labour_commit = std::min<std::int64_t>(labour_need, std::max<std::int64_t>(0, labour_budget));
            labour_budget -= labour_commit;
            building->labour_completed += labour_commit;

            if (building->timber_committed >= building->timber_required &&
                building->stone_committed >= building->stone_required &&
                building->labour_completed >= building->labour_required) {
                building->state = ConstructionState::Complete;
                building->completed_at = state_.time;
            }
        }
    }

    std::unordered_map<SettlementId, std::uint64_t> completed_dwellings_by_settlement;
    std::unordered_map<SettlementId, std::uint64_t> completed_food_stores_by_settlement;
    std::unordered_map<SettlementId, std::uint64_t> completed_workshops_by_settlement;
    completed_dwellings_by_settlement.reserve(state_.settlements.size());
    completed_food_stores_by_settlement.reserve(state_.settlements.size());
    completed_workshops_by_settlement.reserve(state_.settlements.size());

    for (const auto& building : state_.buildings) {
        if (building.state != ConstructionState::Complete) {
            continue;
        }

        if (building.type == BuildingType::Dwelling) {
            ++completed_dwellings_by_settlement[building.settlement_id];
        } else if (building.type == BuildingType::FoodStore) {
            ++completed_food_stores_by_settlement[building.settlement_id];
        } else if (building.type == BuildingType::Workshop) {
            ++completed_workshops_by_settlement[building.settlement_id];
        }
    }

    for (auto& settlement : state_.settlements) {
        const auto completed_dwellings = completed_dwellings_by_settlement.contains(settlement.id)
            ? completed_dwellings_by_settlement[settlement.id]
            : 0U;
        const auto completed_food_stores = completed_food_stores_by_settlement.contains(settlement.id)
            ? completed_food_stores_by_settlement[settlement.id]
            : 0U;
        const auto completed_workshops = completed_workshops_by_settlement.contains(settlement.id)
            ? completed_workshops_by_settlement[settlement.id]
            : 0U;

        settlement.dwelling_capacity_bonus = static_cast<std::int64_t>(completed_dwellings) * kDwellingCapacityBonusPerBuilding;
        settlement.food_store_capacity_bonus = static_cast<std::int64_t>(completed_food_stores) * kFoodStoreCapacityBonusPerBuilding;
        settlement.workshop_capacity_bonus = static_cast<std::int64_t>(completed_workshops) * kWorkshopCapacityBonusPerBuilding;
    }
}

void Simulation::refresh_economic_aggregates() noexcept {
    const auto saturating_increment_u32 = [](const std::uint32_t value) noexcept {
        return value < std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(value + 1U)
                                                                 : std::numeric_limits<std::uint32_t>::max();
    };
    const auto add_u64_checked = [](const std::uint64_t left, const std::uint64_t right, std::uint64_t& out) noexcept {
        if (left > std::numeric_limits<std::uint64_t>::max() - right) {
            out = std::numeric_limits<std::uint64_t>::max();
            return false;
        }
        out = left + right;
        return true;
    };
    const auto mul_u64_checked = [](const std::uint64_t left, const std::uint64_t right, std::uint64_t& out) noexcept {
        if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
            out = std::numeric_limits<std::uint64_t>::max();
            return false;
        }
        out = left * right;
        return true;
    };

    state_.total_household_wealth = 0;
    state_.total_settlement_commons_food = 0;
    state_.active_buildings = 0;
    state_.active_projects = 0;
    state_.total_exchange_volume = 0;
    state_.total_transfer_volume = 0;

    std::unordered_map<SettlementId, std::vector<std::int64_t>> wealth_by_settlement;
    wealth_by_settlement.reserve(state_.settlements.size());

    for (auto& household : state_.households) {
        const auto wealth_food = std::max<std::int64_t>(0, household.resources.food_inventory.value);
        const auto wealth_wood = std::max<std::int64_t>(0, household.owned_materials.wood.value) * kWealthValueWood;
        const auto wealth_stone = std::max<std::int64_t>(0, household.owned_materials.stone.value) * kWealthValueStone;
        const auto wealth_timber = std::max<std::int64_t>(0, household.owned_materials.timber.value) * kWealthValueTimber;
        const auto wealth_tools = std::max<std::int64_t>(0, household.owned_materials.basic_tool.value) * kWealthValueBasicTool;
        household.derived_wealth_points = wealth_food + wealth_wood + wealth_stone + wealth_timber + wealth_tools;
        state_.total_household_wealth += household.derived_wealth_points;
        wealth_by_settlement[household.settlement_id].push_back(household.derived_wealth_points);
    }

    for (auto& settlement : state_.settlements) {
        state_.total_settlement_commons_food += std::max<std::int64_t>(0, settlement.commons_food.value);
        state_.total_exchange_volume += std::max<std::int64_t>(0, settlement.last_exchange_volume);
        state_.total_transfer_volume += std::max<std::int64_t>(0, settlement.last_transfer_food_volume);

        const auto pressure = settlement_pressure_per_10k(settlement);
        if (pressure >= kHighPressureThresholdPer10k) {
            settlement.economic_pressure_streak = saturating_increment_u32(settlement.economic_pressure_streak);
        } else {
            settlement.economic_pressure_streak = 0U;
        }

        const auto wealth_it = wealth_by_settlement.find(settlement.id);
        if (wealth_it == wealth_by_settlement.end() || wealth_it->second.empty()) {
            settlement.last_median_household_wealth = 0;
            settlement.last_min_household_wealth = 0;
            settlement.last_max_household_wealth = 0;
            settlement.last_inequality_numerator = 0;
            settlement.last_inequality_denominator = 0;
            settlement.economic_viability_streak = 0U;
            continue;
        }

        auto wealths = wealth_it->second;
        std::sort(wealths.begin(), wealths.end());
        settlement.last_min_household_wealth = wealths.front();
        settlement.last_max_household_wealth = wealths.back();
        settlement.last_median_household_wealth = wealths[wealths.size() / 2U];

        std::uint64_t total_wealth = 0;
        for (const auto wealth : wealths) {
            total_wealth += static_cast<std::uint64_t>(std::max<std::int64_t>(0, wealth));
        }

        if (total_wealth == 0U) {
            settlement.last_inequality_numerator = 0;
            settlement.last_inequality_denominator = 0;
        } else {
            std::uint64_t pairwise_gap = 0;
            std::uint64_t prefix_sum = 0;
            for (std::size_t i = 0; i < wealths.size(); ++i) {
                const auto wealth_u64 = static_cast<std::uint64_t>(std::max<std::int64_t>(0, wealths[i]));
                std::uint64_t weighted = 0;
                mul_u64_checked(wealth_u64, static_cast<std::uint64_t>(i), weighted);

                if (weighted >= prefix_sum) {
                    std::uint64_t updated_pairwise_gap = 0;
                    add_u64_checked(pairwise_gap, weighted - prefix_sum, updated_pairwise_gap);
                    pairwise_gap = updated_pairwise_gap;
                }

                std::uint64_t updated_prefix_sum = 0;
                add_u64_checked(prefix_sum, wealth_u64, updated_prefix_sum);
                prefix_sum = updated_prefix_sum;
            }

            std::uint64_t numerator = 0;
            mul_u64_checked(pairwise_gap, 2U, numerator);

            std::uint64_t denominator = 0;
            mul_u64_checked(static_cast<std::uint64_t>(wealths.size()), total_wealth, denominator);
            settlement.last_inequality_numerator = std::min(numerator, denominator);
            settlement.last_inequality_denominator = denominator;
        }

        if (pressure <= 5'500U && settlement.last_median_household_wealth > 0) {
            settlement.economic_viability_streak = saturating_increment_u32(settlement.economic_viability_streak);
        } else {
            settlement.economic_viability_streak = 0U;
        }
    }

    for (const auto& building : state_.buildings) {
        if (building.state == ConstructionState::Complete) {
            ++state_.active_buildings;
        } else {
            ++state_.active_projects;
        }
    }
}

void Simulation::step_year() {
    advance(DemographicCalendarAdapter::year_duration());
}

void Simulation::run_years(const std::uint64_t years) {
    const auto years_i64_limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (years > years_i64_limit) {
        throw std::overflow_error("Requested simulation years exceed supported range");
    }

    const auto yearly = DemographicCalendarAdapter::year_duration().ticks;
    const auto years_i64 = static_cast<std::int64_t>(years);
    if (years_i64 > 0 && yearly > (std::numeric_limits<std::int64_t>::max() / years_i64)) {
        throw std::overflow_error("Requested simulation duration overflow");
    }

    advance(SimulationDuration{years_i64 * yearly});
}

}  // namespace universe
