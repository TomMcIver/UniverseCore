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
constexpr std::int64_t kMinRegionAreaUnits = 10;
constexpr std::int64_t kMaxRegionAreaUnits = 10'000;
constexpr std::uint32_t kSettlementShareScale = 10'000;
constexpr std::uint32_t kInitialAllocatedSettlementShare = 8'500;
constexpr std::uint32_t kHighPressureThresholdPer10k = 5'000;
constexpr std::uint32_t kFoundingMinimumUnallocatedShare = 800;
constexpr std::uint32_t kFoundingDefaultShare = 1'200;
constexpr std::uint64_t kFoundingMinimumSourceHouseholds = 3;
constexpr std::uint64_t kFoundingMinimumNewHouseholds = 2;
constexpr std::uint64_t kFoundingMinimumRemainingHouseholds = 1;
constexpr std::uint64_t kTargetHouseholdsPerSettlement = 8;
constexpr std::uint64_t kMaxInitialSettlementsPerRegion = 6;

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

    if (!config_.food_system_enabled) {
        food_next_run_ = SimulationTime{std::numeric_limits<std::int64_t>::max()};
    }

    initialize_population();
    initialize_households();
    initialize_regions();
    assign_households_to_regions();
    initialize_settlements();
    refresh_population_count();
    refresh_food_aggregates();
    refresh_environment_aggregates();
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
    struct HouseholdDayState {
        Household* household{};
        std::vector<Person*> alive_members{};
        std::vector<std::int64_t> per_person_required{};
        std::int64_t required_food{};
        std::int64_t productive_effort{};
        std::int64_t produced_food{};
    };

    std::vector<HouseholdDayState> household_states;
    household_states.reserve(state_.households.size());

    for (auto& household : state_.households) {
        HouseholdDayState household_state{};
        household_state.household = &household;
        household_state.alive_members.reserve(household.member_ids.size());
        for (const auto member_id : household.member_ids) {
            for (auto& person : state_.people) {
                if (person.id == member_id && person.alive) {
                    household_state.alive_members.push_back(&person);
                    break;
                }
            }
        }

        std::sort(household_state.alive_members.begin(), household_state.alive_members.end(), [](const Person* lhs, const Person* rhs) {
            return lhs->id < rhs->id;
        });

        household_state.per_person_required.reserve(household_state.alive_members.size());
        for (const auto* person : household_state.alive_members) {
            const auto person_required = food_requirement_for_person(*person);
            const auto person_effort = productive_effort_for_person(*person);
            if (!checked_add_i64(household_state.required_food, person_required, household_state.required_food) ||
                !checked_add_i64(household_state.productive_effort, person_effort, household_state.productive_effort)) {
                throw std::overflow_error("Food accounting overflow");
            }
            household_state.per_person_required.push_back(person_required);
        }

        household_states.push_back(std::move(household_state));
    }

    std::sort(household_states.begin(), household_states.end(), [](const HouseholdDayState& lhs, const HouseholdDayState& rhs) {
        return lhs.household->id < rhs.household->id;
    });

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

        for (const auto settlement_id : active_settlement_ids) {
            const auto* settlement = find_settlement(settlement_id);
            if (settlement == nullptr || settlement->access_share_points == 0U) {
                continue;
            }

            std::vector<HouseholdDayState*> settlement_households;
            std::int64_t settlement_effort = 0;
            for (auto& household_state : household_states) {
                if (household_state.household->region_id != region.id || household_state.household->settlement_id != settlement_id) {
                    continue;
                }
                settlement_households.push_back(&household_state);
                if (!checked_add_i64(settlement_effort, household_state.productive_effort, settlement_effort)) {
                    throw std::overflow_error("Settlement effort overflow");
                }
            }

            if (settlement_households.empty() || settlement_effort <= 0) {
                continue;
            }

            const auto settlement_productive_budget =
                (productive_available * static_cast<std::int64_t>(settlement->access_share_points)) /
                static_cast<std::int64_t>(kSettlementShareScale);
            const auto settlement_wild_budget =
                (wild_harvest_available * static_cast<std::int64_t>(settlement->access_share_points)) /
                static_cast<std::int64_t>(kSettlementShareScale);

            std::int64_t distributed_productive = 0;
            for (auto* household_state : settlement_households) {
                if (household_state->productive_effort <= 0) {
                    continue;
                }
                const auto produced = (settlement_productive_budget * household_state->productive_effort) / settlement_effort;
                household_state->produced_food += produced;
                if (!checked_add_i64(distributed_productive, produced, distributed_productive)) {
                    throw std::overflow_error("Settlement productive distribution overflow");
                }
            }

            auto productive_remainder = settlement_productive_budget - distributed_productive;
            for (auto* household_state : settlement_households) {
                if (productive_remainder <= 0) {
                    break;
                }
                if (household_state->productive_effort <= 0) {
                    continue;
                }
                ++household_state->produced_food;
                --productive_remainder;
            }

            std::int64_t settlement_distributed_wild = 0;
            for (auto* household_state : settlement_households) {
                if (household_state->productive_effort <= 0) {
                    continue;
                }
                const auto wild_for_household = (settlement_wild_budget * household_state->productive_effort) / settlement_effort;
                household_state->produced_food += wild_for_household;
                if (!checked_add_i64(settlement_distributed_wild, wild_for_household, settlement_distributed_wild)) {
                    throw std::overflow_error("Settlement wild distribution overflow");
                }
            }

            auto wild_remainder = settlement_wild_budget - settlement_distributed_wild;
            for (auto* household_state : settlement_households) {
                if (wild_remainder <= 0) {
                    break;
                }
                if (household_state->productive_effort <= 0) {
                    continue;
                }
                ++household_state->produced_food;
                --wild_remainder;
                ++settlement_distributed_wild;
            }

            if (!checked_add_i64(distributed_wild, settlement_distributed_wild, distributed_wild)) {
                throw std::overflow_error("Region wild distribution overflow");
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

    for (auto& household_state : household_states) {
        auto& household = *household_state.household;
        const auto produced_food = household_state.produced_food;

        std::int64_t inventory_after_production = 0;
        if (!checked_add_i64(household.resources.food_inventory.value, produced_food, inventory_after_production)) {
            throw std::overflow_error("Food inventory overflow on production");
        }
        household.resources.food_inventory.value = inventory_after_production;

        const auto consumed_food = std::min<std::int64_t>(household.resources.food_inventory.value, household_state.required_food);
        household.resources.food_inventory.value -= consumed_food;
        const auto unmet_food = household_state.required_food - consumed_food;

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

            const auto delta = static_cast<std::int64_t>(shortage_stress_delta_per_day(household_state.required_food, unmet_food));
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

        std::vector<std::int64_t> per_person_allocated(household_state.alive_members.size(), 0);
        if (consumed_food > 0 && household_state.required_food > 0 && !household_state.alive_members.empty()) {
            std::int64_t allocated_sum = 0;
            for (std::size_t i = 0; i < household_state.alive_members.size(); ++i) {
                per_person_allocated[i] = (consumed_food * household_state.per_person_required[i]) / household_state.required_food;
                if (!checked_add_i64(allocated_sum, per_person_allocated[i], allocated_sum)) {
                    throw std::overflow_error("Per-person food allocation overflow");
                }
            }

            auto remainder = consumed_food - allocated_sum;
            for (std::size_t i = 0; i < household_state.alive_members.size() && remainder > 0; ++i) {
                ++per_person_allocated[i];
                --remainder;
            }
        }

        for (std::size_t i = 0; i < household_state.alive_members.size(); ++i) {
            apply_person_nutrition_and_health(
                *household_state.alive_members[i],
                household_state.per_person_required[i],
                per_person_allocated[i]);
        }
    }

    refresh_food_aggregates();
    refresh_environment_aggregates();
}

void Simulation::process_demography_period(const std::uint64_t demographic_year) {
    process_mortality(demographic_year);
    process_births(demographic_year);
    process_settlement_restructuring(demographic_year);
    refresh_population_count();
    refresh_food_aggregates();
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

std::uint32_t Simulation::mortality_penalty_from_person_health_per_10k(const Person& person) const noexcept {
    const auto is_child = age_years(person) < kAdultAgeYears;
    return nutrition_health_mortality_penalty_per_10k(person.nutrition_points, person.health_points, is_child);
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
        nutrition_delta = -(10 + ratio_points / 8);
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
        health_delta = -3;
    } else {
        health_delta = -8;
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
        }
    }
}

void Simulation::process_settlement_founding(const std::uint64_t demographic_year) {
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

void Simulation::process_mortality(const std::uint64_t demographic_year) {
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

    for (const auto& region : state_.regions) {
        state_.total_productive_capacity_per_day += region.productive_capacity_per_day;
        state_.total_wild_food_capacity += region.wild_food_capacity;
        state_.total_wild_food_stock += region.wild_food_stock;
        state_.total_wild_food_harvested += region.cumulative_wild_food_harvested;
        state_.total_wild_food_regenerated += region.cumulative_wild_food_regenerated;
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
