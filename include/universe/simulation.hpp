#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
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

enum class BiologicalSex : std::uint8_t {
    Female = 0,
    Male = 1,
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
};

enum class EventType : std::uint8_t {
    Birth = 0,
    Death = 1,
};

struct SimulationEvent {
    EventId id{};
    SimulationTime timestamp{};
    EventType type{EventType::Birth};
    PersonId subject_person_id{};
    std::optional<PersonId> mother_id{};
    std::optional<PersonId> father_id{};
    std::optional<HouseholdId> household_id{};
};

struct SimulationConfig {
    SeedValue seed{1};
    std::uint64_t initial_population{200};
    bool enable_unrelated_random_probe{false};
    std::int64_t initial_household_food_units{80};
    std::uint32_t food_production_scale_percent{100};
    bool food_system_enabled{true};
    std::uint64_t initial_region_count{4};
    std::int64_t base_region_area_units{100};
    std::uint32_t productive_effort_scale_percent{100};
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
    std::vector<Person> people{};
    std::vector<Household> households{};
    std::vector<RegionEnvironmentState> regions{};
    std::vector<Settlement> settlements{};
    std::vector<SimulationEvent> event_log{};
};

class Simulation {
public:
    explicit Simulation(SimulationConfig config);

    [[nodiscard]] const WorldState& state() const noexcept;
    [[nodiscard]] SimulationTime time() const noexcept;
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
    [[nodiscard]] std::uint32_t effective_fertility_rate_per_10k(const Person& person) const noexcept;
    [[nodiscard]] std::int64_t household_stress_points(HouseholdId household_id) const noexcept;
    [[nodiscard]] std::optional<PersonId> sample_father_global(SimulationSeed selection_seed) noexcept;
    [[nodiscard]] std::optional<PersonId> sample_father_in_household(HouseholdId household_id, SimulationSeed selection_seed) noexcept;
    [[nodiscard]] std::uint64_t current_demographic_year() const noexcept;
    [[nodiscard]] std::uint64_t current_food_period() const noexcept;
    void run_scheduled_systems_until(SimulationTime target_time);
    void process_demography_period(std::uint64_t demographic_year);
    void process_food_period(std::uint64_t food_period);
    void apply_person_nutrition_and_health(Person& person, std::int64_t required_food, std::int64_t allocated_food);
    void remove_from_household(HouseholdId household_id, PersonId person_id);
    void refresh_food_aggregates() noexcept;
    void refresh_environment_aggregates() noexcept;
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
    void append_event(SimulationEvent event);
    void emit_death_event(PersonId person_id, HouseholdId household_id);
    void emit_birth_event(PersonId child_id, HouseholdId household_id, PersonId mother_id, std::optional<PersonId> father_id);
    void refresh_population_count() noexcept;

    SimulationConfig config_;
    WorldState state_;
    SimulationSeed simulation_seed_{};
    PersonId next_person_id_{1};
    HouseholdId next_household_id_{1};
    RegionId next_region_id_{1};
    SettlementId next_settlement_id_{1};
    EventId next_event_id_{1};
    SimulationDuration demography_interval_{DemographicCalendarAdapter::year_duration()};
    SimulationTime demography_next_run_{DemographicCalendarAdapter::year_duration().ticks};
    SimulationDuration food_interval_{days(1)};
    SimulationTime food_next_run_{days(1).ticks};
    FoodModelParameters food_parameters_{};

    friend class SnapshotCodec;
};

}  // namespace universe
