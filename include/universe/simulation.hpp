#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "universe/resources.hpp"
#include "universe/seed.hpp"
#include "universe/snapshot.hpp"
#include "universe/time.hpp"

namespace universe {

using PersonId = std::uint64_t;
using HouseholdId = std::uint64_t;
using RegionId = std::uint64_t;
using SettlementId = std::uint64_t;
using EventId = std::uint64_t;
using MemoryId = std::uint64_t;
using KnowledgeId = std::uint64_t;
using CommunicationId = std::uint64_t;
using RecordId = std::uint64_t;
using InstitutionId = std::uint64_t;
using BuildingId = std::uint64_t;
using CohortId = std::uint64_t;

enum class BiologicalSex : std::uint8_t {
    Female = 0,
    Male = 1,
};

enum class Occupation : std::uint8_t {
    None = 0,
    SubsistenceCultivator = 1,
    WildFoodGatherer = 2,
    WoodGatherer = 3,
    StoneExtractor = 4,
    CraftWorker = 5,
};

// Ticket #37: a person's activity for the current day. Work/Social/
// Teaching/RecordKeeping/Discovery/Rest drive real existing systems
// (Occupation, KnowledgeTransmission, institutions, KnowledgeRecord/
// discovery, health); Sleep/Travel/Eat/Idle are presentation-only this
// slice. Picked once per day, not per hour -- the simulation has no
// sub-day tick granularity (advance_days(1) is the only stepping unit
// anywhere), so an hour-of-day model would be unobservable in practice.
enum class ActivityKind : std::uint8_t {
    Sleep = 0,
    Travel = 1,
    Work = 2,
    Eat = 3,
    Social = 4,
    Teaching = 5,
    RecordKeeping = 6,
    Discovery = 7,
    Childcare = 8,
    Rest = 9,
    Idle = 10,
};

enum class SimulationDetailLevel : std::uint8_t {
    Detailed = 0,
    SettlementAggregate = 1,
    RegionalCohort = 2,
};

enum class CohortAgeBand : std::uint8_t {
    Child = 0,
    YoungAdult = 1,
    Adult = 2,
    Senior = 3,
};

enum class CohortOccupationGroup : std::uint8_t {
    Unspecified = 0,
    Food = 1,
    Materials = 2,
    Craft = 3,
};

enum class CohortHealthBand : std::uint8_t {
    Low = 0,
    Medium = 1,
    High = 2,
};

enum class CohortWealthBand : std::uint8_t {
    Low = 0,
    Medium = 1,
    High = 2,
};

struct RegionLodState {
    RegionId region_id{};
    SimulationDetailLevel detail_level{SimulationDetailLevel::Detailed};

    [[nodiscard]] bool operator==(const RegionLodState& other) const = default;
};

struct PopulationCohort {
    CohortId id{};
    RegionId region_id{};
    SettlementId settlement_id{};
    CohortAgeBand age_band{CohortAgeBand::Adult};
    BiologicalSex sex{BiologicalSex::Female};
    CohortOccupationGroup occupation_group{CohortOccupationGroup::Unspecified};
    CohortHealthBand health_band{CohortHealthBand::Medium};
    CohortWealthBand wealth_band{CohortWealthBand::Medium};
    std::uint32_t knowledge_profile_id{};
    std::uint64_t population{};

    [[nodiscard]] bool operator==(const PopulationCohort& other) const = default;
};

struct SettlementAggregateState {
    SettlementId settlement_id{};
    RegionId region_id{};
    std::uint64_t household_count{};
    std::uint64_t small_household_count{};
    std::uint64_t medium_household_count{};
    std::uint64_t large_household_count{};
    std::int64_t food_inventory{};
    std::int64_t food_produced_total{};
    std::int64_t food_consumed_total{};
    std::int64_t unmet_food_total{};
    std::int64_t labour_capacity{};
    std::int64_t labour_demand{};
    std::int64_t labour_allocated{};

    // Household labour-capacity distribution (M4B household aggregation contract).
    std::uint64_t labour_low_household_count{};
    std::uint64_t labour_medium_household_count{};
    std::uint64_t labour_high_household_count{};

    // Household wealth distribution.
    std::uint64_t wealth_low_household_count{};
    std::uint64_t wealth_medium_household_count{};
    std::uint64_t wealth_high_household_count{};

    // Household food-buffer distribution.
    std::uint64_t food_buffer_low_household_count{};
    std::uint64_t food_buffer_medium_household_count{};
    std::uint64_t food_buffer_high_household_count{};

    // Exact conserved settlement-level economic totals held by the aggregate.
    std::int64_t wealth_points_total{};
    std::int64_t productive_access_points{};

    // Rolling per-demographic-year food flow, used to drive aggregate
    // mortality/fertility stress. Reset at the start of each demographic year.
    std::int64_t required_food_year{};
    std::int64_t unmet_food_year{};

    [[nodiscard]] bool operator==(const SettlementAggregateState& other) const = default;
};

struct Person {
    PersonId id{};
    std::int64_t birth_year{};
    BiologicalSex sex{BiologicalSex::Female};
    bool alive{true};
    HouseholdId household_id{};
    std::optional<PersonId> mother_id{};
    std::optional<PersonId> father_id{};
    std::int32_t nutrition_points{700};
    std::int32_t health_points{850};
    std::uint32_t consecutive_undernourished_days{};
    Occupation occupation{Occupation::None};
    bool materialized_from_cohort{false};
    std::optional<CohortId> source_cohort_id{};

    // Ticket #37: only detailed-resolution people carry schedule/activity
    // state; cohort demotion drops it (mirrors existing LOD philosophy).
    // current_activity is cached from the daily schedule and re-derived
    // alongside occupation reassignment -- it is not itself independently
    // serialized state beyond this cached value.
    ActivityKind current_activity{ActivityKind::Idle};

    // Set when a person is resolved to individual detail (initial world,
    // promotion from a population group, snapshot migration): their next
    // occupation reassignment is recorded silently, so a resolution
    // transition never appears as a job change in their life history.
    bool occupation_history_resync_pending{false};
};

struct Household {
    HouseholdId id{};
    RegionId region_id{};
    SettlementId settlement_id{};
    std::vector<PersonId> member_ids{};
    HouseholdResourceState resources{};
    SettlementMaterialInventory owned_materials{};
    std::uint32_t productive_access_share_points{};
    std::uint32_t commons_access_share_points{};
    std::int64_t cumulative_food_transferred_in{};
    std::int64_t cumulative_food_transferred_out{};
    std::int64_t cumulative_material_transferred_in{};
    std::int64_t cumulative_material_transferred_out{};
    std::int64_t derived_wealth_points{};
    // Coin held by the household (ADR 0003); see Settlement::coin.
    std::int64_t coin{};

    // Ticket #35/#38: a person's canonical position is always
    // Settlement.position + Household.settlement_offset -- derived once at
    // household founding via SeedDomain::HouseholdPosition, never
    // re-derived, constant regardless of any person's current_activity.
    std::int32_t settlement_offset_x{};
    std::int32_t settlement_offset_z{};
};

enum class SettlementTier : std::uint8_t {
    Village = 0,
    Town = 1,
    City = 2,
};

// The smallest population a settlement can hold and still be treated as
// sustainable on its own. Fission and absorption share it so neither rule can
// leave a settlement the other can never resolve (ADR 0002).
inline constexpr std::uint64_t kViableSettlementSize = 20U;

// Absorption (ADR 0002): a settlement below the viable size for this many
// consecutive days is absorbed by a neighbour in its region that is viable, has
// kept a viability streak of at least kSurvivorViabilityStreakDays and lies
// within kSettlementMergeDistanceUnits. With no such neighbour it stays.
inline constexpr std::uint32_t kAbsorptionBelowViableDays = 5U * 365U;
inline constexpr std::uint32_t kSurvivorViabilityStreakDays = 365U;
inline constexpr std::int64_t kSettlementMergeDistanceUnits = 2'000;

struct Settlement {
    SettlementId id{};
    RegionId region_id{};
    bool active{true};
    std::int64_t position_x{};
    std::int64_t position_z{};
    std::uint32_t access_share_points{};
    SettlementMaterialInventory materials{};
    std::int64_t tool_wear_work_units{};
    std::uint32_t last_tool_coverage_per_10k{};
    std::int64_t cumulative_wood_extracted{};
    std::int64_t cumulative_stone_extracted{};
    std::int64_t cumulative_timber_produced{};
    std::int64_t cumulative_basic_tool_produced{};
    std::int64_t cumulative_basic_tool_broken{};
    ResourceAmount commons_food{};
    // Coin held by the commons (ADR 0003). A plain conserved integer balance,
    // not a resource: it is created once at world creation and only moves.
    std::int64_t coin{};
    std::int64_t last_transfer_food_volume{};
    std::int64_t last_exchange_volume{};
    std::uint32_t economic_viability_streak{};
    std::uint32_t economic_pressure_streak{};
    std::int64_t dwelling_capacity_bonus{};
    std::int64_t food_store_capacity_bonus{};
    std::int64_t workshop_capacity_bonus{};
    std::int64_t last_median_household_wealth{};
    std::int64_t last_min_household_wealth{};
    std::int64_t last_max_household_wealth{};
    std::uint64_t last_inequality_numerator{};
    std::uint64_t last_inequality_denominator{};
    std::uint64_t population{};
    SettlementTier tier{SettlementTier::Village};
    SettlementTier pending_tier{SettlementTier::Village};
    std::uint32_t tier_transition_streak{};
    // Consecutive days this settlement's population has been below
    // kViableSettlementSize. Resets as soon as it is viable again. An inactive
    // settlement keeps its last value until the next daily update zeroes it,
    // because merges and extinctions happen between daily updates.
    std::uint32_t days_below_viable_size{};
    // Consecutive days on which at least a third of this settlement's
    // households went short of food. Resets on the first day below that share.
    std::uint32_t days_under_food_strain{};
};

enum class BuildingType : std::uint8_t {
    Dwelling = 0,
    FoodStore = 1,
    Workshop = 2,
};

enum class ConstructionState : std::uint8_t {
    Planned = 0,
    ActiveConstruction = 1,
    Complete = 2,
};

struct Building {
    BuildingId id{};
    BuildingType type{BuildingType::Dwelling};
    SettlementId settlement_id{};
    std::optional<HouseholdId> owner_household_id{};
    bool settlement_commons_owned{true};
    ConstructionState state{ConstructionState::Planned};
    std::int64_t timber_required{};
    std::int64_t stone_required{};
    std::int64_t labour_required{};
    std::int64_t timber_committed{};
    std::int64_t stone_committed{};
    std::int64_t labour_completed{};
    SimulationTime started_at{};
    std::optional<SimulationTime> completed_at{};

    // Canonical building placement (ADR 0001), fixed when the building is
    // planned, in the same world units as Settlement.position.
    std::int64_t position_x{};
    std::int64_t position_z{};
    std::uint16_t facing_degrees{};
};

enum class EnvironmentClass : std::uint8_t {
    ProductiveArable = 0,
    Woodland = 1,
    Marginal = 2,
    WaterWetland = 3,
};

struct RegionEnvironmentState {
    RegionId id{};
    std::int64_t land_area_units{};
    std::int64_t position_x{};
    std::int64_t position_z{};
    EnvironmentClass environment_class{EnvironmentClass::ProductiveArable};
    std::int64_t productive_capacity_per_day{};
    std::int64_t wild_food_capacity{};
    std::int64_t wild_food_stock{};
    std::int64_t wild_food_regeneration_per_day{};
    std::int64_t wild_food_harvest_limit_per_day{};
    std::int64_t cumulative_wild_food_harvested{};
    std::int64_t cumulative_wild_food_regenerated{};
    std::int64_t wood_capacity{};
    std::int64_t wood_stock{};
    std::int64_t wood_regeneration_per_day{};
    std::int64_t cumulative_wood_harvested{};
    std::int64_t cumulative_wood_regenerated{};
    std::int64_t stone_capacity{};
    std::int64_t stone_remaining{};
    std::int64_t cumulative_stone_extracted{};
};

enum class EventType : std::uint8_t {
    Birth = 0,
    Death = 1,
    SettlementFounded = 2,
    SettlementExtinct = 3,
    DiscoveryMade = 4,
    InstitutionFounded = 5,
    SettlementMerged = 6,
    OccupationChanged = 7,
    HouseholdRelocated = 8,
};

struct BirthEventPayload {
    PersonId child_id{};
    PersonId mother_id{};
    std::optional<PersonId> father_id{};
    HouseholdId household_id{};

    [[nodiscard]] bool operator==(const BirthEventPayload& other) const = default;
};

struct DeathEventPayload {
    PersonId person_id{};
    HouseholdId household_id{};

    [[nodiscard]] bool operator==(const DeathEventPayload& other) const = default;
};

struct SettlementFoundedEventPayload {
    SettlementId settlement_id{};
    RegionId region_id{};
    std::optional<SettlementId> source_settlement_id{};

    [[nodiscard]] bool operator==(const SettlementFoundedEventPayload& other) const = default;
};

struct SettlementExtinctEventPayload {
    SettlementId settlement_id{};
    RegionId region_id{};

    [[nodiscard]] bool operator==(const SettlementExtinctEventPayload& other) const = default;
};

struct SettlementMergedEventPayload {
    SettlementId survivor_settlement_id{};
    SettlementId absorbed_settlement_id{};
    RegionId region_id{};

    [[nodiscard]] bool operator==(const SettlementMergedEventPayload& other) const = default;
};

struct DiscoveryMadeEventPayload {
    std::uint32_t discovery_key{};
    PersonId discoverer_id{};
    SettlementId settlement_id{};

    [[nodiscard]] bool operator==(const DiscoveryMadeEventPayload& other) const = default;
};

enum class InstitutionKind : std::uint8_t {
    KnowledgeKeeper = 0,
};

struct InstitutionFoundedEventPayload {
    InstitutionId institution_id{};
    InstitutionKind kind{InstitutionKind::KnowledgeKeeper};
    SettlementId settlement_id{};
    PersonId founder_id{};

    [[nodiscard]] bool operator==(const InstitutionFoundedEventPayload& other) const = default;
};

struct OccupationChangedEventPayload {
    PersonId person_id{};
    Occupation from{Occupation::None};
    Occupation to{Occupation::None};

    [[nodiscard]] bool operator==(const OccupationChangedEventPayload& other) const = default;
};

// member_ids records the living members who moved, so each member's life
// history stays exact after later household membership changes.
struct HouseholdRelocatedEventPayload {
    HouseholdId household_id{};
    SettlementId from_settlement_id{};
    SettlementId to_settlement_id{};
    std::vector<PersonId> member_ids{};

    [[nodiscard]] bool operator==(const HouseholdRelocatedEventPayload& other) const = default;
};

using SimulationEventPayload = std::variant<
    BirthEventPayload,
    DeathEventPayload,
    SettlementFoundedEventPayload,
    SettlementExtinctEventPayload,
    DiscoveryMadeEventPayload,
    InstitutionFoundedEventPayload,
    SettlementMergedEventPayload,
    OccupationChangedEventPayload,
    HouseholdRelocatedEventPayload>;

struct SimulationEvent {
    EventId id{};
    SimulationTime timestamp{};
    EventType type{EventType::Birth};
    SimulationEventPayload payload{};
};

enum class MemorySource : std::uint8_t {
    DirectObservation = 0,
    Communication = 1,
    Teaching = 2,
    Record = 3,
};

struct AgentMemory {
    MemoryId id{};
    PersonId owner_id{};
    SimulationTime acquired_at{};
    MemorySource source{MemorySource::DirectObservation};
    EventId source_event_id{};
    std::uint32_t confidence_per_10k{10'000};

    [[nodiscard]] bool operator==(const AgentMemory& other) const = default;
};

enum class CommunicationMethod : std::uint8_t {
    Communication = 0,
    Teaching = 1,
};

struct KnowledgeTransmission {
    CommunicationId id{};
    PersonId speaker_id{};
    PersonId recipient_id{};
    HouseholdId speaker_household_id{};
    HouseholdId recipient_household_id{};
    SettlementId speaker_settlement_id{};
    SettlementId recipient_settlement_id{};
    SimulationTime transmitted_at{};
    CommunicationMethod method{CommunicationMethod::Communication};
    MemoryId speaker_memory_id{};
    MemoryId recipient_memory_id{};
    std::uint32_t confidence_per_10k{10'000};

    [[nodiscard]] bool operator==(const KnowledgeTransmission& other) const = default;
};

struct KnowledgeRecord {
    RecordId id{};
    SettlementId settlement_id{};
    PersonId author_id{};
    HouseholdId author_household_id{};
    SimulationTime recorded_at{};
    MemoryId source_memory_id{};
    EventId source_event_id{};
    std::uint32_t confidence_per_10k{10'000};

    [[nodiscard]] bool operator==(const KnowledgeRecord& other) const = default;
};

struct KnowledgeRecordAccess {
    RecordId record_id{};
    PersonId reader_id{};
    HouseholdId reader_household_id{};
    SettlementId reader_settlement_id{};
    MemoryId reader_memory_id{};
    SimulationTime read_at{};
    std::uint32_t confidence_per_10k{10'000};

    [[nodiscard]] bool operator==(const KnowledgeRecordAccess& other) const = default;
};

struct Institution {
    InstitutionId id{};
    InstitutionKind kind{InstitutionKind::KnowledgeKeeper};
    SettlementId settlement_id{};
    PersonId founder_id{};
    SimulationTime founded_at{};
    bool active{true};
    std::vector<PersonId> member_ids{};
    std::vector<RecordId> preserved_record_ids{};
    std::vector<std::uint32_t> preserved_discovery_keys{};

    [[nodiscard]] bool operator==(const Institution& other) const = default;
};

enum class KnownLifeStatus : std::uint8_t {
    Alive = 0,
    Dead = 1,
};

enum class KnowledgeFactKind : std::uint8_t {
    PersonLifeStatus = 0,
    PersonHousehold = 1,
    SettlementStatus = 2,
    BasicDiscovery = 3,
};

struct PersonLifeStatusKnowledgeFact {
    PersonId person_id{};
    KnownLifeStatus status{KnownLifeStatus::Alive};

    [[nodiscard]] bool operator==(const PersonLifeStatusKnowledgeFact& other) const = default;
};

struct PersonHouseholdKnowledgeFact {
    PersonId person_id{};
    HouseholdId household_id{};

    [[nodiscard]] bool operator==(const PersonHouseholdKnowledgeFact& other) const = default;
};

struct SettlementStatusKnowledgeFact {
    SettlementId settlement_id{};
    RegionId region_id{};
    bool active{true};

    [[nodiscard]] bool operator==(const SettlementStatusKnowledgeFact& other) const = default;
};

struct BasicDiscoveryKnowledgeFact {
    std::uint32_t discovery_key{};

    [[nodiscard]] bool operator==(const BasicDiscoveryKnowledgeFact& other) const = default;
};

using KnowledgeFact = std::variant<
    PersonLifeStatusKnowledgeFact,
    PersonHouseholdKnowledgeFact,
    SettlementStatusKnowledgeFact,
    BasicDiscoveryKnowledgeFact>;

enum class KnowledgeSource : std::uint8_t {
    MemoryDerived = 0,
};

struct AgentKnowledge {
    KnowledgeId id{};
    PersonId owner_id{};
    SimulationTime acquired_at{};
    SimulationTime updated_at{};
    KnowledgeSource source{KnowledgeSource::MemoryDerived};
    MemoryId source_memory_id{};
    KnowledgeFact fact{};
    std::uint32_t confidence_per_10k{10'000};

    [[nodiscard]] bool operator==(const AgentKnowledge& other) const = default;
};

struct SimulationConfig {
    SeedValue seed{1};
    std::uint64_t initial_population{200};
    bool enable_unrelated_random_probe{false};
    std::int64_t initial_household_food_units{80};
    std::uint32_t food_production_scale_percent{100};
    bool food_system_enabled{true};
    std::uint64_t initial_region_count{4};
    // Starting settlements per region. 0 keeps the automatic layout (about one
    // per 8 households, at most 4); a positive value is used as given, capped at
    // the region's household count. It only shapes world creation, which the
    // snapshot records through the settlements themselves, so it is not saved.
    std::uint64_t initial_settlements_per_region{0};
    std::int64_t base_region_area_units{120};
    std::uint32_t productive_effort_scale_percent{100};
    bool materials_system_enabled{true};
    std::uint32_t wood_resource_scale_percent{100};
    std::uint32_t stone_resource_scale_percent{100};
    std::uint32_t wood_regeneration_scale_percent{100};
    std::uint32_t material_crafting_scale_percent{100};
};

struct WorldState {
    SeedValue seed{};
    SimulationTime time{};
    std::uint64_t year{};
    std::uint64_t population{};
    std::uint64_t total_births{};
    std::uint64_t total_deaths{};
    // Portion of the canonical totals produced by aggregate cohort demography.
    // Anonymous aggregate transitions deliberately emit no per-person events,
    // so the event journal reconciles against totals minus these counters.
    std::uint64_t aggregate_births_total{};
    std::uint64_t aggregate_deaths_total{};
    std::int64_t total_food_stored{};
    std::int64_t total_food_produced{};
    std::int64_t total_food_consumed{};
    std::int64_t total_unmet_food{};
    std::uint64_t households_in_shortage{};
    std::uint64_t population_under_food_stress{};
    std::int64_t total_productive_capacity_per_day{};
    std::int64_t total_wild_food_capacity{};
    std::int64_t total_wild_food_stock{};
    std::int64_t total_wild_food_harvested{};
    std::int64_t total_wild_food_regenerated{};
    std::uint64_t eligible_workers{};
    std::uint64_t active_workers{};
    std::uint64_t unallocated_workers{};
    std::uint64_t cultivators{};
    std::uint64_t gatherers{};
    std::uint64_t wood_gatherers{};
    std::uint64_t stone_extractors{};
    std::uint64_t craft_workers{};
    std::int64_t total_labour_capacity{};
    std::int64_t total_labour_demand{};
    std::int64_t total_labour_allocated{};
    std::int64_t cultivation_labour_available{};
    std::int64_t cultivation_labour_allocated{};
    std::int64_t gathering_labour_available{};
    std::int64_t gathering_labour_allocated{};
    std::int64_t labour_shortfall{};
    std::int64_t total_wood_capacity{};
    std::int64_t total_wood_stock{};
    std::int64_t total_stone_capacity{};
    std::int64_t total_stone_remaining{};
    std::int64_t total_wood_harvested{};
    std::int64_t total_wood_regenerated{};
    std::int64_t total_stone_extracted{};
    std::int64_t total_wood_inventory{};
    std::int64_t total_stone_inventory{};
    std::int64_t total_timber_inventory{};
    std::int64_t total_basic_tool_inventory{};
    std::int64_t total_timber_produced{};
    std::int64_t total_basic_tool_produced{};
    std::int64_t total_basic_tool_broken{};
    std::uint32_t average_tool_coverage_per_10k{};
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
    std::int64_t total_household_wealth{};
    std::int64_t total_settlement_commons_food{};
    std::uint64_t active_buildings{};
    std::uint64_t active_projects{};
    std::int64_t total_exchange_volume{};
    std::int64_t total_transfer_volume{};
    SimulationDetailLevel detail_level{SimulationDetailLevel::Detailed};
    std::vector<RegionLodState> region_lod_states{};
    std::vector<PopulationCohort> population_cohorts{};
    std::vector<SettlementAggregateState> settlement_aggregates{};
    std::vector<Person> people{};
    std::vector<Household> households{};
    std::vector<RegionEnvironmentState> regions{};
    std::vector<Settlement> settlements{};
    std::vector<Building> buildings{};
    std::vector<SimulationEvent> event_log{};
    std::vector<AgentMemory> agent_memories{};
    std::vector<KnowledgeTransmission> knowledge_transmissions{};
    std::vector<KnowledgeRecord> knowledge_records{};
    std::vector<KnowledgeRecordAccess> knowledge_record_accesses{};
    std::vector<AgentKnowledge> agent_knowledge{};
    std::vector<Institution> institutions{};
};

class Simulation {
public:
    explicit Simulation(SimulationConfig config);

    [[nodiscard]] const WorldState& state() const noexcept;
    [[nodiscard]] SimulationTime time() const noexcept;
    [[nodiscard]] std::span<const AgentMemory> agent_memories() const noexcept;
    [[nodiscard]] std::vector<AgentMemory> memories_for_person(PersonId person_id) const;
    [[nodiscard]] std::span<const KnowledgeTransmission> knowledge_transmissions() const noexcept;
    [[nodiscard]] std::vector<KnowledgeTransmission> transmissions_for_recipient(PersonId person_id) const;
    [[nodiscard]] std::optional<KnowledgeTransmission> transmission_for_memory(MemoryId recipient_memory_id) const;
    [[nodiscard]] std::span<const KnowledgeRecord> knowledge_records() const noexcept;
    [[nodiscard]] std::vector<KnowledgeRecord> records_for_settlement(SettlementId settlement_id) const;
    [[nodiscard]] std::span<const Institution> institutions() const noexcept;
    [[nodiscard]] std::vector<Institution> institutions_for_settlement(SettlementId settlement_id) const;
    [[nodiscard]] std::optional<KnowledgeRecordAccess> record_access_for_memory(MemoryId reader_memory_id) const;
    [[nodiscard]] bool person_remembers_event(PersonId person_id, EventId event_id) const noexcept;
    [[nodiscard]] std::uint64_t memory_count_for_person(PersonId person_id) const noexcept;
    [[nodiscard]] std::span<const AgentKnowledge> agent_knowledge() const noexcept;
    [[nodiscard]] std::vector<AgentKnowledge> knowledge_for_person(PersonId person_id) const;
    [[nodiscard]] std::uint64_t knowledge_count_for_person(PersonId person_id) const noexcept;
    [[nodiscard]] std::optional<KnownLifeStatus> known_life_status(PersonId knower_id, PersonId subject_id) const noexcept;
    [[nodiscard]] std::optional<HouseholdId> known_household_for_person(PersonId knower_id, PersonId subject_id) const noexcept;
    [[nodiscard]] std::optional<SettlementStatusKnowledgeFact> known_settlement_status(PersonId knower_id, SettlementId settlement_id) const noexcept;
    [[nodiscard]] bool knows_basic_discovery(PersonId knower_id, std::uint32_t discovery_key) const noexcept;
    [[nodiscard]] std::vector<std::byte> save_snapshot() const;
    [[nodiscard]] static SnapshotLoadResult load_snapshot(std::span<const std::byte> data, Simulation& destination);
    void set_region_detail_level(RegionId region_id, SimulationDetailLevel level);
    [[nodiscard]] SimulationDetailLevel region_detail_level(RegionId region_id) const noexcept;
    void add_aggregate_population(RegionId region_id, SettlementId settlement_id, std::uint64_t population);
    void promote_region_to_detailed(RegionId region_id);
    void demote_region_to_settlement_aggregate(RegionId region_id);
    // Re-derives every building placement in building-id order. Placement is a
    // pure function of settlement geography and lower-id buildings, so this
    // reproduces planning-time placement exactly; used to migrate v24 saves.
    void recompute_building_placements();
    [[nodiscard]] std::uint64_t aggregate_population() const noexcept;
    [[nodiscard]] std::uint64_t detailed_population() const noexcept;

    // Non-canonical LOD execution instrumentation. These counters are never
    // serialized and never affect simulation outcomes; they exist so tests can
    // prove that aggregate settlements do not fall back to per-person loops.
    [[nodiscard]] std::uint64_t person_daily_update_count() const noexcept { return person_daily_update_count_; }
    [[nodiscard]] std::uint64_t person_demography_update_count() const noexcept { return person_demography_update_count_; }
    [[nodiscard]] std::uint64_t aggregate_settlement_update_count() const noexcept { return aggregate_settlement_update_count_; }
    [[nodiscard]] std::uint64_t aggregate_cohort_update_count() const noexcept { return aggregate_cohort_update_count_; }
    void reset_lod_instrumentation() noexcept {
        person_daily_update_count_ = 0;
        person_demography_update_count_ = 0;
        aggregate_settlement_update_count_ = 0;
        aggregate_cohort_update_count_ = 0;
    }
    void advance(SimulationDuration duration);
    void step_year();
    void run_years(std::uint64_t years);

private:
    [[nodiscard]] PersonId next_person_id() noexcept;
    [[nodiscard]] HouseholdId next_household_id() noexcept;
    [[nodiscard]] RegionId next_region_id() noexcept;
    [[nodiscard]] SettlementId next_settlement_id() noexcept;
    [[nodiscard]] MemoryId next_memory_id() noexcept;
    [[nodiscard]] KnowledgeId next_knowledge_id() noexcept;
    [[nodiscard]] CommunicationId next_communication_id() noexcept;
    [[nodiscard]] RecordId next_record_id() noexcept;
    [[nodiscard]] InstitutionId next_institution_id() noexcept;
    [[nodiscard]] BuildingId next_building_id() noexcept;
    [[nodiscard]] SimulationSeed root_seed() const noexcept;
    [[nodiscard]] SimulationSeed person_seed(PersonId person_id) const noexcept;
    [[nodiscard]] SimulationSeed household_seed(HouseholdId household_id) const noexcept;
    void assign_household_position_offset(Household& household) const noexcept;
    [[nodiscard]] SimulationSeed region_seed(RegionId region_id) const noexcept;
    [[nodiscard]] SimulationSeed settlement_seed(SettlementId settlement_id) const noexcept;
    [[nodiscard]] bool sample_sex(SimulationSeed seed) const noexcept;
    [[nodiscard]] std::uint32_t sample_initial_age_years(PersonId person_id) const noexcept;
    [[nodiscard]] std::uint32_t mortality_rate_per_10k(std::uint32_t age_years) const noexcept;
    [[nodiscard]] std::uint32_t mortality_penalty_from_person_health_per_10k(const Person& person) const noexcept;
    [[nodiscard]] std::uint32_t fertility_rate_per_10k(std::uint32_t age_years) const noexcept;
    [[nodiscard]] std::int64_t current_year_signed() const noexcept;
    [[nodiscard]] std::uint32_t age_years(const Person& person) const noexcept;
    [[nodiscard]] bool has_any_alive_adult_male() const noexcept;
    [[nodiscard]] bool is_alive_adult_male_in_household(HouseholdId household_id) const noexcept;
    [[nodiscard]] std::uint32_t alive_count_in_household(HouseholdId household_id) const noexcept;
    [[nodiscard]] std::int64_t food_requirement_for_person(const Person& person) const noexcept;
    [[nodiscard]] std::int64_t productive_effort_for_person(const Person& person) const noexcept;
    [[nodiscard]] bool occupation_eligible_for_productive_work(const Person& person) const noexcept;
    [[nodiscard]] std::uint32_t effective_fertility_rate_per_10k(const Person& person) const noexcept;
    [[nodiscard]] std::int64_t household_stress_points(HouseholdId household_id) const noexcept;
    [[nodiscard]] std::optional<PersonId> sample_father_global(SimulationSeed selection_seed) noexcept;
    [[nodiscard]] std::optional<PersonId> sample_father_in_household(HouseholdId household_id, SimulationSeed selection_seed) noexcept;
    [[nodiscard]] std::uint64_t current_demographic_year() const noexcept;
    [[nodiscard]] std::uint64_t current_food_period() const noexcept;
    void run_scheduled_systems_until(SimulationTime target_time);
    void process_schedule_period(std::uint64_t food_period);
    void process_demography_period(std::uint64_t demographic_year);
    void process_basic_discoveries(std::uint64_t demographic_year);
    void process_communication_teaching(std::uint64_t demographic_year);
    void process_records(std::uint64_t demographic_year);
    void process_institutions(std::uint64_t demographic_year);
    void process_knowledge_loss(std::uint64_t demographic_year);
    void process_food_period(std::uint64_t food_period);
    void process_materials_period(std::uint64_t food_period);
    void process_occupation_reassignment(std::uint64_t demographic_year);
    void process_household_access_allocation();
    void process_household_stabilization_transfers();
    void process_local_exchange(std::uint64_t demographic_year);
    void process_construction(std::uint64_t demographic_year);
    [[nodiscard]] std::int64_t source_construction_materials(
        Settlement& settlement,
        std::int64_t timber_shortfall,
        std::int64_t stone_shortfall,
        std::int64_t labour_budget);
    void clear_occupations_for_extinct_or_invalid_households();
    void refresh_labour_aggregates();
    void apply_person_nutrition_and_health(Person& person, std::int64_t required_food, std::int64_t allocated_food);
    void remove_from_household(HouseholdId household_id, PersonId person_id);
    void refresh_food_aggregates() noexcept;
    void refresh_environment_aggregates() noexcept;
    void refresh_material_aggregates() noexcept;
    void initialize_regions();
    void assign_region_positions();
    void assign_households_to_regions();
    void initialize_settlements();
    void assign_settlement_positions(RegionId region_id, const std::vector<SettlementId>& settlement_ids);
    void assign_position_for_new_settlement(SettlementId settlement_id, RegionId region_id);
    void initialize_region_lod_states();
    void initialize_settlement_aggregates();
    void endow_initial_coin();
    [[nodiscard]] bool is_region_detailed(RegionId region_id) const noexcept;
    [[nodiscard]] bool is_settlement_detailed(SettlementId settlement_id) const noexcept;
    [[nodiscard]] bool is_household_detailed(HouseholdId household_id) const noexcept;
    [[nodiscard]] bool is_person_detailed(const Person& person) const noexcept;
    void merge_population_cohorts();
    [[nodiscard]] std::unordered_set<PersonId> collect_canonized_person_ids() const;
    void process_aggregate_cohort_aging(std::uint64_t demographic_year);
    void process_aggregate_cohort_mortality(std::uint64_t demographic_year);
    void process_aggregate_cohort_fertility(std::uint64_t demographic_year);
    void process_aggregate_food_period(std::uint64_t food_period);
    void process_aggregate_demography_period(std::uint64_t demographic_year);
    void rebuild_settlement_aggregate_distribution(SettlementId settlement_id);
    void rebuild_global_detail_level();
    void process_settlement_restructuring(std::uint64_t demographic_year);
    void process_settlement_extinction();
    void process_settlement_founding(std::uint64_t demographic_year);
    // The households a fission would move, movers first. See the definition.
    struct FissionPlan {
        std::vector<Household*> households{};
        std::uint64_t move_count{};
    };
    [[nodiscard]] std::optional<FissionPlan> plan_fission_households(
        const Settlement& source,
        std::uint64_t demographic_year,
        const std::unordered_set<PersonId>& alive_person_ids);
    void process_settlement_merging();
    void merge_settlements(SettlementId survivor_id, SettlementId absorbed_id);
    [[nodiscard]] std::uint32_t settlement_pressure_per_10k(const Settlement& settlement) const noexcept;
    [[nodiscard]] std::int64_t settlement_accessible_food_capacity_per_day(const Settlement& settlement) const noexcept;
    [[nodiscard]] Settlement* find_settlement_mutable(SettlementId settlement_id) noexcept;
    [[nodiscard]] const Settlement* find_settlement(SettlementId settlement_id) const noexcept;
    [[nodiscard]] RegionEnvironmentState generate_region_state(RegionId region_id) const;
    [[nodiscard]] RegionEnvironmentState* find_region_mutable(RegionId region_id) noexcept;
    [[nodiscard]] const RegionEnvironmentState* find_region(RegionId region_id) const noexcept;
    void initialize_population();
    void initialize_households();
    void process_mortality(std::uint64_t demographic_year);
    void process_births(std::uint64_t demographic_year);
    EventId append_event(SimulationEvent event);
    EventId emit_death_event(PersonId person_id, HouseholdId household_id);
    EventId emit_birth_event(PersonId child_id, HouseholdId household_id, PersonId mother_id, std::optional<PersonId> father_id);
    EventId emit_settlement_founded_event(SettlementId settlement_id, RegionId region_id, std::optional<SettlementId> source_settlement_id);
    EventId emit_settlement_extinct_event(SettlementId settlement_id, RegionId region_id);
    EventId emit_settlement_merged_event(SettlementId survivor_settlement_id, SettlementId absorbed_settlement_id, RegionId region_id);
    EventId emit_occupation_changed_event(PersonId person_id, Occupation from, Occupation to);
    void record_occupation_changes(const std::unordered_map<PersonId, Occupation>& occupations_before);
    EventId emit_household_relocated_event(const Household& household, SettlementId from_settlement_id, SettlementId to_settlement_id);
    void place_building(Building& building, std::span<const Building> earlier_buildings) const;
    EventId emit_discovery_made_event(std::uint32_t discovery_key, PersonId discoverer_id, SettlementId settlement_id);
    EventId emit_institution_founded_event(InstitutionId institution_id, InstitutionKind kind, SettlementId settlement_id, PersonId founder_id);
    [[nodiscard]] MemoryId append_memory(AgentMemory memory);
    void append_knowledge_transmission(KnowledgeTransmission transmission);
    void append_knowledge_record(KnowledgeRecord record);
    void append_knowledge_record_access(KnowledgeRecordAccess access);
    void append_direct_observation_memories(EventId source_event_id, std::span<const PersonId> observer_ids, SimulationTime acquired_at);
    void derive_knowledge_from_memory(const AgentMemory& memory);
    void upsert_knowledge(
        PersonId owner_id,
        SimulationTime acquired_at,
        MemoryId source_memory_id,
        std::uint32_t confidence_per_10k,
        KnowledgeFact fact,
        KnowledgeFactKind fact_kind,
        std::uint64_t primary_subject_id,
        std::uint64_t secondary_subject_id);
    void rebuild_knowledge_indexes();
    void rebuild_communication_indexes();
    void rebuild_record_indexes();
    void refresh_economic_aggregates() noexcept;
    void advance_settlement_daily_counters();
    void refresh_population_count() noexcept;

    struct KnowledgeSemanticKey {
        PersonId owner_id{};
        KnowledgeFactKind fact_kind{KnowledgeFactKind::PersonLifeStatus};
        std::uint64_t primary_subject_id{};
        std::uint64_t secondary_subject_id{};

        [[nodiscard]] bool operator==(const KnowledgeSemanticKey& other) const noexcept {
            return owner_id == other.owner_id &&
                fact_kind == other.fact_kind &&
                primary_subject_id == other.primary_subject_id &&
                secondary_subject_id == other.secondary_subject_id;
        }
    };

    struct KnowledgeSemanticKeyHash {
        [[nodiscard]] std::size_t operator()(const KnowledgeSemanticKey& key) const noexcept {
            const auto kind_u64 = static_cast<std::uint64_t>(key.fact_kind);
            const auto mixed =
                (key.owner_id * 0x9E3779B97F4A7C15ULL) ^
                (kind_u64 * 0xD6E8FEB86659FD93ULL) ^
                (key.primary_subject_id * 0xA24BAED4963EE407ULL) ^
                (key.secondary_subject_id * 0x94D049BB133111EBULL);
            return static_cast<std::size_t>(mixed);
        }
    };

    SimulationConfig config_;
    WorldState state_;
    SimulationSeed simulation_seed_{};
    PersonId next_person_id_{1};
    HouseholdId next_household_id_{1};
    RegionId next_region_id_{1};
    SettlementId next_settlement_id_{1};
    EventId next_event_id_{1};
    MemoryId next_memory_id_{1};
    KnowledgeId next_knowledge_id_{1};
    CommunicationId next_communication_id_{1};
    RecordId next_record_id_{1};
    InstitutionId next_institution_id_{1};
    BuildingId next_building_id_{1};
    CohortId next_cohort_id_{1};

    // Non-canonical instrumentation; excluded from snapshots and equality.
    std::uint64_t person_daily_update_count_{};
    std::uint64_t person_demography_update_count_{};
    std::uint64_t aggregate_settlement_update_count_{};
    std::uint64_t aggregate_cohort_update_count_{};
    SimulationDuration demography_interval_{DemographicCalendarAdapter::year_duration()};
    SimulationTime demography_next_run_{DemographicCalendarAdapter::year_duration().ticks};
    SimulationDuration food_interval_{days(1)};
    SimulationTime food_next_run_{days(1).ticks};
    FoodModelParameters food_parameters_{};
    MaterialModelParameters material_parameters_{};
    std::unordered_map<PersonId, std::vector<std::size_t>> knowledge_indices_by_owner_{};
    std::unordered_map<KnowledgeSemanticKey, std::size_t, KnowledgeSemanticKeyHash> knowledge_index_by_semantic_key_{};

    struct TransmissionSemanticKey {
        PersonId recipient_id{};
        MemoryId speaker_memory_id{};

        [[nodiscard]] bool operator==(const TransmissionSemanticKey& other) const noexcept {
            return recipient_id == other.recipient_id && speaker_memory_id == other.speaker_memory_id;
        }
    };

    struct TransmissionSemanticKeyHash {
        [[nodiscard]] std::size_t operator()(const TransmissionSemanticKey& key) const noexcept {
            const auto mixed =
                (key.recipient_id * 0x9E3779B97F4A7C15ULL) ^
                (key.speaker_memory_id * 0xD6E8FEB86659FD93ULL);
            return static_cast<std::size_t>(mixed);
        }
    };

    std::unordered_map<MemoryId, std::size_t> communication_index_by_recipient_memory_{};
    std::unordered_set<TransmissionSemanticKey, TransmissionSemanticKeyHash> transmission_semantic_keys_{};

    struct RecordSemanticKey {
        SettlementId settlement_id{};
        EventId source_event_id{};

        [[nodiscard]] bool operator==(const RecordSemanticKey& other) const noexcept {
            return settlement_id == other.settlement_id && source_event_id == other.source_event_id;
        }
    };

    struct RecordSemanticKeyHash {
        [[nodiscard]] std::size_t operator()(const RecordSemanticKey& key) const noexcept {
            const auto mixed =
                (key.settlement_id * 0x9E3779B97F4A7C15ULL) ^
                (key.source_event_id * 0xD6E8FEB86659FD93ULL);
            return static_cast<std::size_t>(mixed);
        }
    };

    struct RecordAccessSemanticKey {
        RecordId record_id{};
        PersonId reader_id{};

        [[nodiscard]] bool operator==(const RecordAccessSemanticKey& other) const noexcept {
            return record_id == other.record_id && reader_id == other.reader_id;
        }
    };

    struct RecordAccessSemanticKeyHash {
        [[nodiscard]] std::size_t operator()(const RecordAccessSemanticKey& key) const noexcept {
            const auto mixed =
                (key.record_id * 0xA24BAED4963EE407ULL) ^
                (key.reader_id * 0x94D049BB133111EBULL);
            return static_cast<std::size_t>(mixed);
        }
    };

    std::unordered_set<RecordSemanticKey, RecordSemanticKeyHash> record_semantic_keys_{};
    std::unordered_set<RecordAccessSemanticKey, RecordAccessSemanticKeyHash> record_access_semantic_keys_{};
    std::unordered_map<MemoryId, std::size_t> record_access_index_by_memory_{};

    friend class SnapshotCodec;
};

}  // namespace universe
