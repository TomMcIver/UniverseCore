#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "universe/seed.hpp"
#include "universe/snapshot.hpp"
#include "universe/time.hpp"

namespace universe {

using PersonId = std::uint64_t;
using HouseholdId = std::uint64_t;
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
};

struct Household {
    HouseholdId id{};
    std::vector<PersonId> member_ids{};
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
};

struct WorldState {
    SeedValue seed{};
    SimulationTime time{};
    std::uint64_t year{};
    std::uint64_t population{};
    std::uint64_t total_births{};
    std::uint64_t total_deaths{};
    std::vector<Person> people{};
    std::vector<Household> households{};
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
    [[nodiscard]] SimulationSeed root_seed() const noexcept;
    [[nodiscard]] SimulationSeed person_seed(PersonId person_id) const noexcept;
    [[nodiscard]] bool sample_sex(SimulationSeed seed) const noexcept;
    [[nodiscard]] std::uint32_t sample_initial_age_years(PersonId person_id) const noexcept;
    [[nodiscard]] std::uint32_t mortality_rate_per_10k(std::uint32_t age_years) const noexcept;
    [[nodiscard]] std::uint32_t fertility_rate_per_10k(std::uint32_t age_years) const noexcept;
    [[nodiscard]] std::int64_t current_year_signed() const noexcept;
    [[nodiscard]] std::uint32_t age_years(const Person& person) const noexcept;
    [[nodiscard]] bool has_any_alive_adult_male() const noexcept;
    [[nodiscard]] bool is_alive_adult_male_in_household(HouseholdId household_id) const noexcept;
    [[nodiscard]] std::uint32_t alive_count_in_household(HouseholdId household_id) const noexcept;
    [[nodiscard]] std::uint32_t effective_fertility_rate_per_10k(const Person& person) const noexcept;
    [[nodiscard]] std::optional<PersonId> sample_father_global(SimulationSeed selection_seed) noexcept;
    [[nodiscard]] std::optional<PersonId> sample_father_in_household(HouseholdId household_id, SimulationSeed selection_seed) noexcept;
    [[nodiscard]] std::uint64_t current_demographic_year() const noexcept;
    void run_scheduled_systems_until(SimulationTime target_time);
    void process_demography_period(std::uint64_t demographic_year);
    void remove_from_household(HouseholdId household_id, PersonId person_id);
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
    EventId next_event_id_{1};
    SimulationDuration demography_interval_{DemographicCalendarAdapter::year_duration()};
    SimulationTime demography_next_run_{DemographicCalendarAdapter::year_duration().ticks};

    friend class SnapshotCodec;
};

}  // namespace universe
