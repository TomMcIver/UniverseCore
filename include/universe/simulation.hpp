#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
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
};

struct Household {
    HouseholdId id{};
    RegionId region_id{};
    SettlementId settlement_id{};
    std::vector<PersonId> member_ids{};
    HouseholdResourceState resources{};
};

struct Settlement {
    SettlementId id{};
    RegionId region_id{};
    bool active{true};
    std::uint32_t access_share_points{};
    SettlementMaterialInventory materials{};
    std::int64_t tool_wear_work_units{};
    std::uint32_t last_tool_coverage_per_10k{};
    std::int64_t cumulative_wood_extracted{};
    std::int64_t cumulative_stone_extracted{};
    std::int64_t cumulative_timber_produced{};
    std::int64_t cumulative_basic_tool_produced{};
    std::int64_t cumulative_basic_tool_broken{};
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

using SimulationEventPayload = std::variant<
    BirthEventPayload,
    DeathEventPayload,
    SettlementFoundedEventPayload,
    SettlementExtinctEventPayload>;

struct SimulationEvent {
    EventId id{};
    SimulationTime timestamp{};
    EventType type{EventType::Birth};
    SimulationEventPayload payload{};
};

enum class MemorySource : std::uint8_t {
    DirectObservation = 0,
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

struct SimulationConfig {
    SeedValue seed{1};
    std::uint64_t initial_population{200};
    bool enable_unrelated_random_probe{false};
    std::int64_t initial_household_food_units{80};
    std::uint32_t food_production_scale_percent{100};
    bool food_system_enabled{true};
    std::uint64_t initial_region_count{4};
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
    std::vector<Person> people{};
    std::vector<Household> households{};
    std::vector<RegionEnvironmentState> regions{};
    std::vector<Settlement> settlements{};
    std::vector<SimulationEvent> event_log{};
    std::vector<AgentMemory> agent_memories{};
};

class Simulation {
public:
    explicit Simulation(SimulationConfig config);

    [[nodiscard]] const WorldState& state() const noexcept;
    [[nodiscard]] SimulationTime time() const noexcept;
    [[nodiscard]] std::span<const AgentMemory> agent_memories() const noexcept;
    [[nodiscard]] std::vector<AgentMemory> memories_for_person(PersonId person_id) const;
    [[nodiscard]] bool person_remembers_event(PersonId person_id, EventId event_id) const noexcept;
    [[nodiscard]] std::uint64_t memory_count_for_person(PersonId person_id) const noexcept;
    [[nodiscard]] std::vector<std::byte> save_snapshot() const;
    [[nodiscard]] static SnapshotLoadResult load_snapshot(std::span<const std::byte> data, Simulation& destination);
    void advance(SimulationDuration duration);
    void step_year();
    void run_years(std::uint64_t years);

private:
    [[nodiscard]] PersonId next_person_id() noexcept;
    [[nodiscard]] HouseholdId next_household_id() noexcept;
    [[nodiscard]] RegionId next_region_id() noexcept;
    [[nodiscard]] SettlementId next_settlement_id() noexcept;
    [[nodiscard]] MemoryId next_memory_id() noexcept;
    [[nodiscard]] SimulationSeed root_seed() const noexcept;
    [[nodiscard]] SimulationSeed person_seed(PersonId person_id) const noexcept;
    [[nodiscard]] SimulationSeed household_seed(HouseholdId household_id) const noexcept;
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
    void process_demography_period(std::uint64_t demographic_year);
    void process_food_period(std::uint64_t food_period);
    void process_materials_period(std::uint64_t food_period);
    void process_occupation_reassignment(std::uint64_t demographic_year);
    void clear_occupations_for_extinct_or_invalid_households();
    void refresh_labour_aggregates();
    void apply_person_nutrition_and_health(Person& person, std::int64_t required_food, std::int64_t allocated_food);
    void remove_from_household(HouseholdId household_id, PersonId person_id);
    void refresh_food_aggregates() noexcept;
    void refresh_environment_aggregates() noexcept;
    void refresh_material_aggregates() noexcept;
    void initialize_regions();
    void assign_households_to_regions();
    void initialize_settlements();
    void process_settlement_restructuring(std::uint64_t demographic_year);
    void process_settlement_extinction();
    void process_settlement_founding(std::uint64_t demographic_year);
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
    void append_memory(AgentMemory memory);
    void append_direct_observation_memories(EventId source_event_id, std::span<const PersonId> observer_ids, SimulationTime acquired_at);
    void refresh_population_count() noexcept;

    SimulationConfig config_;
    WorldState state_;
    SimulationSeed simulation_seed_{};
    PersonId next_person_id_{1};
    HouseholdId next_household_id_{1};
    RegionId next_region_id_{1};
    SettlementId next_settlement_id_{1};
    EventId next_event_id_{1};
    MemoryId next_memory_id_{1};
    SimulationDuration demography_interval_{DemographicCalendarAdapter::year_duration()};
    SimulationTime demography_next_run_{DemographicCalendarAdapter::year_duration().ticks};
    SimulationDuration food_interval_{days(1)};
    SimulationTime food_next_run_{days(1).ticks};
    FoodModelParameters food_parameters_{};
    MaterialModelParameters material_parameters_{};

    friend class SnapshotCodec;
};

}  // namespace universe
