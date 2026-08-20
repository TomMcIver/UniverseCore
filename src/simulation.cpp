#include "universe/simulation.hpp"

#include "universe/snapshot.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace universe {

namespace {
constexpr std::uint32_t kAdultAgeYears = 18;

std::uint32_t bounded_u32(const std::uint64_t value) {
    return static_cast<std::uint32_t>(std::min(value, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
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

    if (!config_.food_system_enabled) {
        food_next_run_ = SimulationTime{std::numeric_limits<std::int64_t>::max()};
    }

    initialize_population();
    initialize_households();
    refresh_population_count();
    refresh_food_aggregates();
}

const WorldState& Simulation::state() const noexcept {
    return state_;
}

SimulationTime Simulation::time() const noexcept {
    return state_.time;
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
    for (auto& household : state_.households) {
        std::uint64_t adults = 0;
        std::uint64_t children = 0;
        for (const auto member_id : household.member_ids) {
            for (const auto& person : state_.people) {
                if (person.id != member_id || !person.alive) {
                    continue;
                }

                if (age_years(person) >= kAdultAgeYears) {
                    ++adults;
                } else {
                    ++children;
                }
                break;
            }
        }

        const auto adults_i64 = static_cast<std::int64_t>(adults);
        const auto children_i64 = static_cast<std::int64_t>(children);

        std::int64_t required_food = 0;
        std::int64_t adult_required = 0;
        std::int64_t child_required = 0;
        if (!checked_mul_i64(adults_i64, food_parameters_.adult_daily_food_units, adult_required) ||
            !checked_mul_i64(children_i64, food_parameters_.child_daily_food_units, child_required) ||
            !checked_add_i64(adult_required, child_required, required_food)) {
            throw std::overflow_error("Food requirement overflow");
        }

        std::int64_t produced_food = required_food;
        if (config_.food_system_enabled) {
            std::int64_t adult_produced = 0;
            std::int64_t child_produced = 0;
            std::int64_t baseline_production = 0;
            if (!checked_mul_i64(adults_i64, food_parameters_.adult_daily_production_units, adult_produced) ||
                !checked_mul_i64(children_i64, food_parameters_.child_daily_production_units, child_produced) ||
                !checked_add_i64(adult_produced, child_produced, baseline_production)) {
                throw std::overflow_error("Food production overflow");
            }

            const auto household_seed_local = derive_seed(root_seed(), SeedDomain::Household, household.id);
            const auto production_seed = derive_seed(household_seed_local, SeedDomain::FoodProduction, food_period);
            const auto daily_variation_percent = 90U + static_cast<std::uint32_t>(seed_random_bounded(production_seed, 21U));

            produced_food = apply_scaled_percent(baseline_production, household_base_productivity_percent(household.id));
            produced_food = apply_scaled_percent(produced_food, daily_variation_percent);
            produced_food = apply_scaled_percent(produced_food, config_.food_production_scale_percent);
        }

        std::int64_t inventory_after_production = 0;
        if (!checked_add_i64(household.resources.food_inventory.value, produced_food, inventory_after_production)) {
            throw std::overflow_error("Food inventory overflow on production");
        }
        household.resources.food_inventory.value = inventory_after_production;

        const auto consumed_food = std::min<std::int64_t>(household.resources.food_inventory.value, required_food);
        household.resources.food_inventory.value -= consumed_food;
        const auto unmet_food = required_food - consumed_food;

        if (household.resources.food_inventory.value < 0) {
            throw std::runtime_error("Food inventory must never be negative");
        }

        if (!checked_add_i64(household.resources.food_produced_total, produced_food, household.resources.food_produced_total) ||
            !checked_add_i64(household.resources.food_consumed_total, consumed_food, household.resources.food_consumed_total) ||
            !checked_add_i64(household.resources.unmet_food_total, unmet_food, household.resources.unmet_food_total)) {
            throw std::overflow_error("Food accounting totals overflow");
        }

        if (unmet_food > 0) {
            ++household.resources.consecutive_shortage_days;

            const auto delta = static_cast<std::int64_t>(shortage_stress_delta_per_day(required_food, unmet_food));
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
    }

    refresh_food_aggregates();
}

void Simulation::process_demography_period(const std::uint64_t demographic_year) {
    process_mortality(demographic_year);
    process_births(demographic_year);
    refresh_population_count();
    refresh_food_aggregates();
}

PersonId Simulation::next_person_id() noexcept {
    return next_person_id_++;
}

HouseholdId Simulation::next_household_id() noexcept {
    return next_household_id_++;
}

SimulationSeed Simulation::root_seed() const noexcept {
    return simulation_seed_;
}

SimulationSeed Simulation::person_seed(const PersonId person_id) const noexcept {
    return derive_seed(root_seed(), SeedDomain::Person, person_id);
}

bool Simulation::sample_sex(const SimulationSeed seed) const noexcept {
    const auto sex_seed = derive_seed(seed, SeedDomain::Sex, 0U);
    return seed_chance_per_10k(sex_seed, 5'000U);
}

std::uint32_t Simulation::mortality_rate_per_10k(const std::uint32_t age_years) const noexcept {
    if (age_years == 0U) {
        return 500;
    }
    if (age_years <= 4U) {
        return 50;
    }
    if (age_years <= 14U) {
        return 20;
    }
    if (age_years <= 49U) {
        return 30;
    }
    if (age_years <= 64U) {
        return 80;
    }
    if (age_years <= 74U) {
        return 250;
    }
    if (age_years <= 84U) {
        return 1'200;
    }
    if (age_years <= 94U) {
        return 4'000;
    }
    if (age_years <= 104U) {
        return 8'000;
    }
    return 10'000;
}

std::uint32_t Simulation::mortality_penalty_from_food_stress_per_10k(const Person& person) const noexcept {
    const auto stress_points = household_stress_points(person.household_id);
    const auto is_child = age_years(person) < kAdultAgeYears;
    return food_stress_mortality_penalty_per_10k(stress_points, is_child);
}

std::uint32_t Simulation::fertility_rate_per_10k(const std::uint32_t age_years) const noexcept {
    if (age_years < 18U || age_years > 44U) {
        return 0;
    }
    if (age_years <= 24U) {
        return 1'800;
    }
    if (age_years <= 34U) {
        return 2'200;
    }
    if (age_years <= 39U) {
        return 900;
    }
    return 200;
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

std::uint32_t Simulation::effective_fertility_rate_per_10k(const Person& person) const noexcept {
    const auto base_rate = fertility_rate_per_10k(age_years(person));
    if (base_rate == 0U) {
        return 0U;
    }

    const auto household_alive = alive_count_in_household(person.household_id);
    if (household_alive >= 18U) {
        return std::max<std::uint32_t>(base_rate / 20U, 1U);
    }

    const auto capacity_remaining = 18U - household_alive;
    const auto scaled = static_cast<std::uint64_t>(base_rate) * capacity_remaining;
    auto effective = std::max<std::uint32_t>(static_cast<std::uint32_t>(scaled / 18U), 1U);

    const auto stress_scale = food_stress_fertility_scale_percent(household_stress_points(person.household_id));
    effective = static_cast<std::uint32_t>((static_cast<std::uint64_t>(effective) * stress_scale) / 100U);
    return effective;
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
}

void Simulation::process_mortality(const std::uint64_t demographic_year) {
    for (auto& person : state_.people) {
        if (!person.alive) {
            continue;
        }

        const auto baseline = mortality_rate_per_10k(age_years(person));
        const auto food_penalty = mortality_penalty_from_food_stress_per_10k(person);
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
            emit_death_event(person.id, household_id);
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

        for (auto& household : state_.households) {
            if (household.id == birth.household_id) {
                household.member_ids.push_back(newborn_id);
                break;
            }
        }

        ++state_.total_births;
        emit_birth_event(newborn_id, birth.household_id, birth.mother_id, birth.father_id);
    }
}

void Simulation::append_event(SimulationEvent event) {
    event.id = next_event_id_++;
    state_.event_log.push_back(event);
}

void Simulation::emit_death_event(const PersonId person_id, const HouseholdId household_id) {
    append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::Death,
        .subject_person_id = person_id,
        .mother_id = std::nullopt,
        .father_id = std::nullopt,
        .household_id = household_id,
    });
}

void Simulation::emit_birth_event(
    const PersonId child_id,
    const HouseholdId household_id,
    const PersonId mother_id,
    const std::optional<PersonId> father_id) {
    append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::Birth,
        .subject_person_id = child_id,
        .mother_id = mother_id,
        .father_id = father_id,
        .household_id = household_id,
    });
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

    std::unordered_map<HouseholdId, bool> household_in_stress;
    household_in_stress.reserve(state_.households.size());

    for (const auto& household : state_.households) {
        state_.total_food_stored += household.resources.food_inventory.value;
        state_.total_food_produced += household.resources.food_produced_total;
        state_.total_food_consumed += household.resources.food_consumed_total;
        state_.total_unmet_food += household.resources.unmet_food_total;

        const bool shortage = household.resources.consecutive_shortage_days > 0U;
        if (shortage) {
            ++state_.households_in_shortage;
        }

        household_in_stress.emplace(household.id, household.resources.nutritional_stress_points > 0);
    }

    for (const auto& person : state_.people) {
        if (!person.alive) {
            continue;
        }

        const auto it = household_in_stress.find(person.household_id);
        if (it != household_in_stress.end() && it->second) {
            ++state_.population_under_food_stress;
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
