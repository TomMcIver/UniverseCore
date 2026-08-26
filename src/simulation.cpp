#include "universe/simulation.hpp"

#include "universe/snapshot.hpp"

#include <algorithm>
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
constexpr std::uint64_t kFoundingMinimumSourceHouseholds = 5;
constexpr std::uint64_t kFoundingMinimumNewHouseholds = 2;
constexpr std::uint64_t kFoundingMinimumRemainingHouseholds = 1;
constexpr std::uint64_t kTargetHouseholdsPerSettlement = 8;
constexpr std::uint64_t kMaxInitialSettlementsPerRegion = 6;
constexpr std::int64_t kAssignmentRequiredCoveragePercent = 115;
constexpr std::int64_t kSettlementMaterialShareScale = 10'000;
constexpr std::int64_t kToolWearWorkUnitsPerBreak = 24;
constexpr std::uint32_t kToolProductivityBonusPercentAtFullCoverage = 45;

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
    process_occupation_reassignment(0);
    refresh_population_count();
    refresh_food_aggregates();
    refresh_environment_aggregates();
    refresh_labour_aggregates();
    refresh_material_aggregates();
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
        facts.push_back(state_.agent_knowledge[index]);
    }
    return facts;
}

std::uint64_t Simulation::knowledge_count_for_person(const PersonId person_id) const noexcept {
    const auto owner_it = knowledge_indices_by_owner_.find(person_id);
    if (owner_it == knowledge_indices_by_owner_.end()) {
        return 0U;
    }
    return static_cast<std::uint64_t>(owner_it->second.size());
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

    const auto* fact = std::get_if<SettlementStatusKnowledgeFact>(&state_.agent_knowledge[key_it->second].fact);
    if (fact == nullptr) {
        return std::nullopt;
    }

    return *fact;
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
    }

    refresh_food_aggregates();
    refresh_environment_aggregates();
    refresh_material_aggregates();
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

            auto& inventory = settlement_state->settlement->materials;
            const auto timber_deficit = std::max<std::int64_t>(0, targets.timber_target - inventory.timber.value);
            const auto tool_deficit = std::max<std::int64_t>(0, targets.basic_tool_target - inventory.basic_tool.value);

            const auto wood_recipe_need =
                (timber_deficit * material_parameters_.wood_per_timber) +
                (tool_deficit * material_parameters_.wood_per_basic_tool);
            const auto stone_recipe_need = tool_deficit * material_parameters_.stone_per_basic_tool;

            const auto wood_reserve_need = std::max<std::int64_t>(0, targets.wood_target - inventory.wood.value);
            const auto stone_reserve_need = std::max<std::int64_t>(0, targets.stone_target - inventory.stone.value);
            const auto wood_total_need = wood_recipe_need + wood_reserve_need;
            const auto stone_total_need = stone_recipe_need + stone_reserve_need;

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
    process_occupation_reassignment(demographic_year);
    refresh_population_count();
    refresh_food_aggregates();
    refresh_labour_aggregates();
    refresh_material_aggregates();
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

        const auto allocated_share = std::min<std::uint32_t>(
            kInitialAllocatedSettlementShare,
            static_cast<std::uint32_t>(kSettlementShareScale));

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
    return std::max<std::int64_t>(0, productive + wild);
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
        if (unallocated_share < kFoundingMinimumUnallocatedShare) {
            continue;
        }

        std::sort(active_settlements.begin(), active_settlements.end(), [](const Settlement* lhs, const Settlement* rhs) {
            return lhs->id < rhs->id;
        });

        Settlement* source = nullptr;
        std::uint32_t best_pressure = 0;

        for (auto* candidate : active_settlements) {
            std::uint64_t alive_households = 0;
            for (const auto& household : state_.households) {
                if (household.region_id != region_id || household.settlement_id != candidate->id) {
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
                    ++alive_households;
                }
            }

            if (alive_households < kFoundingMinimumSourceHouseholds) {
                continue;
            }

            const auto pressure = settlement_pressure_per_10k(*candidate);
            if (pressure < kHighPressureThresholdPer10k) {
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
        const auto assigned_share = std::min<std::uint32_t>(unallocated_share, kFoundingDefaultShare);
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
        std::int64_t settlement_stored_food = 0;
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

        const auto cultivation_opportunity =
            (region->productive_capacity_per_day * static_cast<std::int64_t>(settlement->access_share_points)) /
            static_cast<std::int64_t>(kSettlementShareScale);
        const auto gathering_opportunity =
            (std::min(region->wild_food_stock, region->wild_food_harvest_limit_per_day) *
             static_cast<std::int64_t>(settlement->access_share_points)) /
            static_cast<std::int64_t>(kSettlementShareScale);

        const auto output_target = compute_daily_food_output_target(
            settlement_required_food,
            settlement_stored_food,
            cultivation_opportunity + gathering_opportunity);
        auto assignment_target = output_target;
        if (settlement_required_food > 0) {
            const auto required_coverage_target = (settlement_required_food * kAssignmentRequiredCoveragePercent) / 100;
            assignment_target = std::max(assignment_target, required_coverage_target);
        }
        assignment_target = std::min(assignment_target, cultivation_opportunity + gathering_opportunity);
        const auto demand = split_demand_by_opportunity(assignment_target, cultivation_opportunity, gathering_opportunity);

        std::int64_t cultivation_remaining = std::max<std::int64_t>(0, demand.cultivation_labour_demand);
        std::int64_t gathering_remaining = std::max<std::int64_t>(0, demand.gathering_labour_demand);

        for (auto& candidate : candidates) {
            if (candidate.person->occupation != Occupation::SubsistenceCultivator || cultivation_remaining <= 0) {
                continue;
            }
            candidate.assigned = true;
            cultivation_remaining = std::max<std::int64_t>(0, cultivation_remaining - candidate.labour_capacity);
        }

        for (auto& candidate : candidates) {
            if (candidate.assigned || candidate.person->occupation != Occupation::WildFoodGatherer || gathering_remaining <= 0) {
                continue;
            }
            candidate.assigned = true;
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
            const auto timber_deficit = std::max<std::int64_t>(0, targets.timber_target - settlement_mutable->materials.timber.value);
            const auto tool_deficit = std::max<std::int64_t>(0, targets.basic_tool_target - settlement_mutable->materials.basic_tool.value);
            auto wood_total_need =
                std::max<std::int64_t>(0, targets.wood_target - settlement_mutable->materials.wood.value) +
                (timber_deficit * material_parameters_.wood_per_timber) +
                (tool_deficit * material_parameters_.wood_per_basic_tool);
            auto stone_total_need =
                std::max<std::int64_t>(0, targets.stone_target - settlement_mutable->materials.stone.value) +
                (tool_deficit * material_parameters_.stone_per_basic_tool);
            auto craft_need = timber_deficit + tool_deficit;

            for (auto& candidate : candidates) {
                if (candidate.assigned) {
                    continue;
                }

                if (wood_total_need > 0 && wood_total_need >= stone_total_need && wood_total_need >= craft_need) {
                    candidate.person->occupation = Occupation::WoodGatherer;
                    candidate.assigned = true;
                    wood_total_need = std::max<std::int64_t>(0, wood_total_need - candidate.labour_capacity);
                } else if (stone_total_need > 0 && stone_total_need >= craft_need) {
                    candidate.person->occupation = Occupation::StoneExtractor;
                    candidate.assigned = true;
                    stone_total_need = std::max<std::int64_t>(0, stone_total_need - candidate.labour_capacity);
                } else if (craft_need > 0) {
                    candidate.person->occupation = Occupation::CraftWorker;
                    candidate.assigned = true;
                    craft_need = std::max<std::int64_t>(0, craft_need - candidate.labour_capacity);
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

void Simulation::append_memory(AgentMemory memory) {
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

    memory.id = next_memory_id();
    state_.agent_memories.push_back(memory);
    derive_knowledge_from_memory(memory);
}

void Simulation::append_direct_observation_memories(
    const EventId source_event_id,
    std::span<const PersonId> observer_ids,
    const SimulationTime acquired_at) {
    for (const auto observer_id : observer_ids) {
        if (observer_id == 0U) {
            continue;
        }

        append_memory(AgentMemory{
            .id = 0,
            .owner_id = observer_id,
            .acquired_at = acquired_at,
            .source = MemorySource::DirectObservation,
            .source_event_id = source_event_id,
            .confidence_per_10k = 10'000U,
        });
    }
}

void Simulation::derive_knowledge_from_memory(const AgentMemory& memory) {
    if (memory.source != MemorySource::DirectObservation) {
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
