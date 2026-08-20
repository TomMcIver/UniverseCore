#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"
#include "universe/seed.hpp"
#include "universe/ui/simulation_controller.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::uint32_t age_years(const universe::WorldState& state, const universe::Person& person) {
    const auto current_year = static_cast<std::int64_t>(state.year);
    if (current_year <= person.birth_year) {
        return 0;
    }

    return static_cast<std::uint32_t>(current_year - person.birth_year);
}

const universe::Person* find_person(const universe::WorldState& state, const universe::PersonId id) {
    for (const auto& person : state.people) {
        if (person.id == id) {
            return &person;
        }
    }
    return nullptr;
}

const universe::Household* find_household(const universe::WorldState& state, const universe::HouseholdId id) {
    for (const auto& household : state.households) {
        if (household.id == id) {
            return &household;
        }
    }
    return nullptr;
}

void verify_household_membership(const universe::WorldState& state) {
    std::unordered_set<universe::PersonId> all_member_ids;
    for (const auto& household : state.households) {
        for (const auto member_id : household.member_ids) {
            const auto* person = find_person(state, member_id);
            require(person != nullptr, "household member id must reference a known person");
            require(person->alive, "dead people must not remain in household membership");
            require(person->household_id == household.id, "person household id must match household membership");
            all_member_ids.insert(member_id);
        }
    }

    for (const auto& person : state.people) {
        if (!person.alive) {
            continue;
        }
        require(all_member_ids.contains(person.id), "every alive person must belong to a household");
    }
}

void verify_demographic_invariants(const universe::WorldState& state) {
    std::unordered_set<universe::PersonId> ids;
    std::uint64_t alive_count = 0;

    for (const auto& person : state.people) {
        require(person.id > 0, "person id must be non-zero");
        require(ids.insert(person.id).second, "person ids must be unique");

        if (person.alive) {
            const auto age = age_years(state, person);
            require(age <= 200U, "alive person age must remain within plausible bounds");
            ++alive_count;
        }

        if (!person.mother_id.has_value()) {
            continue;
        }

        const auto* mother = find_person(state, *person.mother_id);
        require(mother != nullptr, "mother id must reference an existing person");
        require(mother->sex == universe::BiologicalSex::Female, "mother must be female");
        require(mother->birth_year < person.birth_year, "mother must be older than child");
        require((person.birth_year - mother->birth_year) >= 18, "mother must be at least 18 at child birth");

        require(person.father_id.has_value(), "child with recorded mother must have recorded father");
        const auto* father = find_person(state, *person.father_id);
        require(father != nullptr, "father id must reference an existing person");
        require(father->sex == universe::BiologicalSex::Male, "father must be male");
        require(father->birth_year < person.birth_year, "father must be older than child");
        require((person.birth_year - father->birth_year) >= 18, "father must be at least 18 at child birth");
    }

    require(alive_count == state.population, "alive people count must equal state population");
    verify_household_membership(state);
}

void verify_resource_invariants(const universe::WorldState& state, const std::int64_t expected_initial_food_units) {
    std::int64_t total_food_stored = 0;
    std::int64_t total_food_produced = 0;
    std::int64_t total_food_consumed = 0;
    std::int64_t total_unmet_food = 0;
    std::uint64_t households_in_shortage = 0;
    std::uint64_t stressed_population = 0;

    for (const auto& household : state.households) {
        const auto& resources = household.resources;
        require(resources.food_inventory.value >= 0, "household food inventory must never be negative");
        require(resources.food_produced_total >= 0, "household food produced total must never be negative");
        require(resources.food_consumed_total >= 0, "household food consumed total must never be negative");
        require(resources.unmet_food_total >= 0, "household unmet food total must never be negative");
        require(resources.nutritional_stress_points >= 0, "household stress must never be negative");

        const auto inferred_initial =
            resources.food_inventory.value + resources.food_consumed_total - resources.food_produced_total;
        require(
            inferred_initial == expected_initial_food_units,
            "food accounting should preserve canonical inventory conservation per household");

        total_food_stored += resources.food_inventory.value;
        total_food_produced += resources.food_produced_total;
        total_food_consumed += resources.food_consumed_total;
        total_unmet_food += resources.unmet_food_total;

        if (resources.consecutive_shortage_days > 0U) {
            ++households_in_shortage;
        }
    }

    for (const auto& person : state.people) {
        if (!person.alive) {
            continue;
        }
        require(person.nutrition_points >= 0 && person.nutrition_points <= 1000, "person nutrition points must remain bounded");
        require(person.health_points >= 0 && person.health_points <= 1000, "person health points must remain bounded");
        if (person.nutrition_points < 550 || person.health_points < 500) {
            ++stressed_population;
        }
    }

    require(total_food_stored == state.total_food_stored, "stored food aggregate must match household totals");
    require(total_food_produced == state.total_food_produced, "produced food aggregate must match household totals");
    require(total_food_consumed == state.total_food_consumed, "consumed food aggregate must match household totals");
    require(total_unmet_food == state.total_unmet_food, "unmet food aggregate must match household totals");
    require(households_in_shortage == state.households_in_shortage, "shortage household aggregate must match");
    require(stressed_population == state.population_under_food_stress, "population under stress aggregate must match");
}

void verify_person_health_invariants(const universe::WorldState& state) {
    for (const auto& person : state.people) {
        require(person.nutrition_points >= 0 && person.nutrition_points <= 1000, "nutrition points must be within canonical bounds");
        require(person.health_points >= 0 && person.health_points <= 1000, "health points must be within canonical bounds");
    }
}

void verify_event_log_invariants(const universe::WorldState& state) {
    std::uint64_t birth_events = 0;
    std::uint64_t death_events = 0;
    universe::EventId expected_event_id = 1;
    universe::SimulationTime last_timestamp{0};
    bool saw_event = false;

    for (const auto& event : state.event_log) {
        require(event.id == expected_event_id, "event ids must be contiguous and deterministic");
        ++expected_event_id;

        require(event.timestamp.ticks <= state.time.ticks, "event timestamp must not exceed simulation time");
        const auto event_year = universe::DemographicCalendarAdapter::demographic_year(event.timestamp);
        require(event_year <= state.year, "event demographic year must not exceed simulation year");
        if (saw_event) {
            require(last_timestamp <= event.timestamp, "event timestamps must be monotonic non-decreasing");
        }
        last_timestamp = event.timestamp;
        saw_event = true;

        const auto* subject = find_person(state, event.subject_person_id);
        require(subject != nullptr, "event subject id must reference an existing person");

        if (event.household_id.has_value()) {
            bool household_exists = false;
            for (const auto& household : state.households) {
                if (household.id == *event.household_id) {
                    household_exists = true;
                    break;
                }
            }
            require(household_exists, "event household id must reference an existing household");
        }

        if (event.type == universe::EventType::Birth) {
            ++birth_events;
            require(event.mother_id.has_value(), "birth event must contain mother id");
            require(event.father_id.has_value(), "birth event must contain father id");

            const auto* mother = find_person(state, *event.mother_id);
            const auto* father = find_person(state, *event.father_id);
            require(mother != nullptr, "birth event mother id must reference an existing person");
            require(father != nullptr, "birth event father id must reference an existing person");
            require(mother->sex == universe::BiologicalSex::Female, "birth event mother must be female");
            require(father->sex == universe::BiologicalSex::Male, "birth event father must be male");
        } else {
            ++death_events;
            require(!event.mother_id.has_value(), "death event must not contain mother id");
            require(!event.father_id.has_value(), "death event must not contain father id");
        }
    }

    require(birth_events == state.total_births, "every generated birth must produce exactly one birth event");
    require(death_events == state.total_deaths, "every generated death must produce exactly one death event");
}

void require_world_state_equal(const universe::WorldState& lhs, const universe::WorldState& rhs, const char* context) {
    require(lhs.time == rhs.time, context);
    require(lhs.year == rhs.year, context);
    require(lhs.population == rhs.population, context);
    require(lhs.total_births == rhs.total_births, context);
    require(lhs.total_deaths == rhs.total_deaths, context);
    require(lhs.total_food_stored == rhs.total_food_stored, context);
    require(lhs.total_food_produced == rhs.total_food_produced, context);
    require(lhs.total_food_consumed == rhs.total_food_consumed, context);
    require(lhs.total_unmet_food == rhs.total_unmet_food, context);
    require(lhs.households_in_shortage == rhs.households_in_shortage, context);
    require(lhs.population_under_food_stress == rhs.population_under_food_stress, context);
    require(lhs.people.size() == rhs.people.size(), context);
    require(lhs.households.size() == rhs.households.size(), context);
    require(lhs.event_log.size() == rhs.event_log.size(), context);

    for (std::size_t i = 0; i < lhs.people.size(); ++i) {
        const auto& a = lhs.people[i];
        const auto& b = rhs.people[i];
        require(a.id == b.id, context);
        require(a.birth_year == b.birth_year, context);
        require(a.sex == b.sex, context);
        require(a.alive == b.alive, context);
        require(a.household_id == b.household_id, context);
        require(a.mother_id == b.mother_id, context);
        require(a.father_id == b.father_id, context);
        require(a.nutrition_points == b.nutrition_points, context);
        require(a.health_points == b.health_points, context);
        require(a.consecutive_undernourished_days == b.consecutive_undernourished_days, context);
    }

    for (std::size_t i = 0; i < lhs.households.size(); ++i) {
        const auto& a = lhs.households[i];
        const auto& b = rhs.households[i];
        require(a.id == b.id, context);

        auto a_members = a.member_ids;
        auto b_members = b.member_ids;
        std::sort(a_members.begin(), a_members.end());
        std::sort(b_members.begin(), b_members.end());
        require(a_members == b_members, context);

        require(a.resources.food_inventory.value == b.resources.food_inventory.value, context);
        require(a.resources.food_produced_total == b.resources.food_produced_total, context);
        require(a.resources.food_consumed_total == b.resources.food_consumed_total, context);
        require(a.resources.unmet_food_total == b.resources.unmet_food_total, context);
        require(a.resources.nutritional_stress_points == b.resources.nutritional_stress_points, context);
        require(a.resources.consecutive_shortage_days == b.resources.consecutive_shortage_days, context);
    }

    for (std::size_t i = 0; i < lhs.event_log.size(); ++i) {
        const auto& a = lhs.event_log[i];
        const auto& b = rhs.event_log[i];
        require(a.id == b.id, context);
        require(a.timestamp == b.timestamp, context);
        require(a.type == b.type, context);
        require(a.subject_person_id == b.subject_person_id, context);
        require(a.mother_id == b.mother_id, context);
        require(a.father_id == b.father_id, context);
        require(a.household_id == b.household_id, context);
    }
}

std::vector<std::byte> clone_bytes(const std::vector<std::byte>& bytes) {
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

void corrupt_byte(std::vector<std::byte>& bytes, const std::size_t index, const std::uint8_t mask) {
    bytes[index] ^= static_cast<std::byte>(mask);
}

std::uint64_t read_u64_le(const std::vector<std::byte>& bytes, const std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8U; ++i) {
        const auto current = static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + i]));
        value |= (current << (8U * i));
    }
    return value;
}

void write_u64_le(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint64_t value) {
    for (std::size_t i = 0; i < 8U; ++i) {
        const auto shifted = static_cast<std::uint64_t>(value >> (8U * i));
        bytes[offset + i] = static_cast<std::byte>(shifted & 0xFFU);
    }
}

void refresh_snapshot_payload_hash(std::vector<std::byte>& bytes) {
    constexpr std::size_t header_size = 24U;
    constexpr std::size_t payload_size_offset = 8U;
    constexpr std::size_t payload_hash_offset = 16U;

    const auto payload_size = static_cast<std::size_t>(read_u64_le(bytes, payload_size_offset));
    const std::span<const std::byte> payload{bytes.data() + header_size, payload_size};
    const auto hash = universe::snapshot_hash64(payload);
    write_u64_le(bytes, payload_hash_offset, hash);
}

void require_snapshot_load_fails(
    const std::vector<std::byte>& bytes,
    const universe::SnapshotErrorCode expected_code,
    const char* context) {
    universe::Simulation destination{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto result = universe::Simulation::load_snapshot(bytes, destination);
    require(!result.ok, context);
    require(result.code == expected_code, context);
}

universe::Simulation load_from_snapshot_or_fail(const std::vector<std::byte>& bytes, const char* context) {
    universe::Simulation loaded{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto load_result = universe::Simulation::load_snapshot(bytes, loaded);
    require(load_result.ok, context);
    return loaded;
}

}  // namespace

int main() {
    {
        require(universe::seconds(1).ticks == 1, "seconds conversion must be exact");
        require(universe::minutes(1).ticks == 60, "minutes conversion must be exact");
        require(universe::hours(1).ticks == 3600, "hours conversion must be exact");
        require(universe::days(1).ticks == 86400, "days conversion must be exact");
        require(
            universe::DemographicCalendarAdapter::year_duration().ticks == 365 * 24 * 60 * 60,
            "demographic year duration must be 365 days");

        const universe::SimulationTime t0{100};
        const universe::SimulationTime t1{101};
        require(t1 > t0, "time ordering must be strict");
        require(t0 < t1, "time ordering must be strict");
        require(t0 <= t1, "time ordering must support <=");
        require(t1 >= t0, "time ordering must support >=");
        require((t1 - t0).ticks == 1, "time subtraction must produce duration");

        universe::SimulationTime sum{};
        require(
            universe::checked_add_time(universe::SimulationTime{10}, universe::SimulationDuration{15}, sum),
            "checked_add_time should succeed for in-range additions");
        require(sum.ticks == 25, "checked_add_time must compute exact sums");

        const auto max_i64 = std::numeric_limits<std::int64_t>::max();
        require(
            !universe::checked_add_time(universe::SimulationTime{max_i64}, universe::SimulationDuration{1}, sum),
            "checked_add_time must detect positive overflow");
    }

    {
        const universe::SimulationSeed root{42};
        const auto a = universe::derive_seed(root, universe::SeedDomain::Person, 7U);
        const auto b = universe::derive_seed(root, universe::SeedDomain::Person, 7U);
        require(a.value == b.value, "same parent/domain/id must derive same seed");

        const auto person_seed = universe::derive_seed(root, universe::SeedDomain::Person, 7U);
        const auto household_seed = universe::derive_seed(root, universe::SeedDomain::Household, 7U);
        require(person_seed.value != household_seed.value, "different domains should derive different seeds for same id");

        const auto id_7 = universe::derive_seed(root, universe::SeedDomain::Person, 7U);
        const auto id_8 = universe::derive_seed(root, universe::SeedDomain::Person, 8U);
        require(id_7.value != id_8.value, "different ids should derive different seeds");

        std::vector<std::uint64_t> ids{4U, 2U, 9U, 1U};
        std::unordered_map<std::uint64_t, universe::SeedValue> forward;
        std::unordered_map<std::uint64_t, universe::SeedValue> reverse;
        for (const auto id : ids) {
            forward[id] = universe::derive_seed(root, universe::SeedDomain::Person, id).value;
        }
        std::reverse(ids.begin(), ids.end());
        for (const auto id : ids) {
            reverse[id] = universe::derive_seed(root, universe::SeedDomain::Person, id).value;
        }
        require(forward == reverse, "seed derivation must not depend on invocation order");

        const auto assign_household = [&root](const std::vector<std::uint64_t>& child_ids, const std::uint64_t household_count) {
            std::unordered_map<std::uint64_t, std::uint64_t> mapping;
            for (const auto child_id : child_ids) {
                const auto person_seed_local = universe::derive_seed(root, universe::SeedDomain::Person, child_id);
                const auto assignment_seed = universe::derive_seed(person_seed_local, universe::SeedDomain::HouseholdAssignment, 0U);
                mapping[child_id] = universe::seed_random_bounded(assignment_seed, household_count);
            }
            return mapping;
        };

        const std::vector<std::uint64_t> child_ids{101U, 102U, 103U, 104U, 105U, 106U};
        auto reverse_ids = child_ids;
        std::reverse(reverse_ids.begin(), reverse_ids.end());
        require(
            assign_household(child_ids, 6U) == assign_household(reverse_ids, 6U),
            "household assignment must remain canonical across traversal orders");

        const auto person_x = universe::derive_seed(root, universe::SeedDomain::Person, 222U);
        const auto person_y = universe::derive_seed(root, universe::SeedDomain::Person, 333U);

        const auto mortality_x_before = universe::seed_chance_per_10k(
            universe::derive_seed(person_x, universe::SeedDomain::Mortality, 37U),
            120U);
        (void)universe::seed_random_u64(universe::derive_seed(person_y, universe::SeedDomain::UnrelatedProbe, 37U));
        const auto mortality_x_after = universe::seed_chance_per_10k(
            universe::derive_seed(person_x, universe::SeedDomain::Mortality, 37U),
            120U);
        require(mortality_x_before == mortality_x_after, "person mortality decision must be independent of unrelated processing");

        const auto fertility_x_before = universe::seed_chance_per_10k(
            universe::derive_seed(person_x, universe::SeedDomain::Fertility, 37U),
            2200U);
        (void)universe::seed_random_u64(universe::derive_seed(person_y, universe::SeedDomain::UnrelatedProbe, 91U));
        const auto fertility_x_after = universe::seed_chance_per_10k(
            universe::derive_seed(person_x, universe::SeedDomain::Fertility, 37U),
            2200U);
        require(fertility_x_before == fertility_x_after, "person fertility decision must be independent of unrelated processing");

        const auto household_a = universe::derive_seed(root, universe::SeedDomain::Household, 77U);
        const auto household_b = universe::derive_seed(root, universe::SeedDomain::Household, 88U);
        const auto food_a_before = universe::seed_random_bounded(
            universe::derive_seed(household_a, universe::SeedDomain::FoodProduction, 123U),
            10'000U);
        (void)universe::seed_random_u64(universe::derive_seed(household_b, universe::SeedDomain::FoodProduction, 555U));
        const auto food_a_after = universe::seed_random_bounded(
            universe::derive_seed(household_a, universe::SeedDomain::FoodProduction, 123U),
            10'000U);
        require(food_a_before == food_a_after, "household food production decisions must be independent of unrelated household processing");
    }

    const universe::SimulationConfig config{
        .seed = 42,
        .initial_population = 200,
        .enable_unrelated_random_probe = false,
        .initial_household_food_units = 80,
        .food_production_scale_percent = 100,
        .food_system_enabled = true,
    };

    {
        universe::ui::SimulationController yearly_steps{config};
        yearly_steps.advance_years(1);
        yearly_steps.advance_years(1);
        yearly_steps.advance_years(1);

        universe::ui::SimulationController one_chunk{config};
        one_chunk.advance_years(3);

        require_world_state_equal(
            yearly_steps.simulation().state(),
            one_chunk.simulation().state(),
            "ui controller: +1 year +1 year +1 year must equal +3 years");
    }

    {
        universe::ui::SimulationController continuous{config};
        continuous.advance_years(500);

        universe::ui::SimulationController segmented{config};
        segmented.advance_years(250);
        const auto save_result = segmented.save_snapshot_file("ui_controller_resume_test.ucore");
        require(save_result.ok, "ui controller save must succeed");

        universe::ui::SimulationController resumed{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        const auto load_result = resumed.load_snapshot_file("ui_controller_resume_test.ucore");
        require(load_result.ok, "ui controller load must succeed");
        resumed.advance_years(250);

        require_world_state_equal(
            continuous.simulation().state(),
            resumed.simulation().state(),
            "ui controller save/load continuation must preserve deterministic outcomes");
    }

    {
        universe::Simulation fresh{config};
        const auto bytes_a = fresh.save_snapshot();
        const auto bytes_b = fresh.save_snapshot();
        require(bytes_a == bytes_b, "serializing identical state twice must be byte-identical");

        auto loaded = load_from_snapshot_or_fail(bytes_a, "fresh snapshot must load");
        require_world_state_equal(fresh.state(), loaded.state(), "fresh snapshot round-trip must preserve full world state");
        const auto bytes_after_load = loaded.save_snapshot();
        require(bytes_a == bytes_after_load, "fresh snapshot must be stable across load/save round-trip");
        verify_resource_invariants(fresh.state(), config.initial_household_food_units);
    }

    {
        universe::Simulation food_a{config};
        universe::Simulation food_b{config};
        food_a.run_years(25);
        food_b.run_years(25);
        require_world_state_equal(food_a.state(), food_b.state(), "same seed must produce identical household food state");

        auto diff = config;
        diff.seed = 43;
        universe::Simulation food_c{diff};
        food_c.run_years(25);
        const bool diverged =
            food_a.state().total_food_produced != food_c.state().total_food_produced ||
            food_a.state().total_food_consumed != food_c.state().total_food_consumed ||
            food_a.state().total_food_stored != food_c.state().total_food_stored ||
            food_a.state().total_unmet_food != food_c.state().total_unmet_food;
        require(diverged, "different seeds should diverge in food outcomes");
    }

    {
        universe::Simulation day_by_day{config};
        for (std::uint64_t i = 0; i < 365U; ++i) {
            day_by_day.advance(universe::days(1));
        }

        universe::Simulation one_chunk{config};
        one_chunk.advance(universe::days(365));
        require_world_state_equal(day_by_day.state(), one_chunk.state(), "365 daily advances must equal one 365-day advance");
    }

    {
        auto abundant = config;
        abundant.food_production_scale_percent = 170;
        universe::Simulation abundant_run{abundant};
        abundant_run.run_years(30);
        verify_resource_invariants(abundant_run.state(), abundant.initial_household_food_units);
        verify_person_health_invariants(abundant_run.state());
        require(abundant_run.state().population_under_food_stress == 0, "sustained food surplus should avoid food stress");
        require(abundant_run.state().households_in_shortage == 0, "sustained food surplus should avoid active shortages");

        auto constrained = config;
        constrained.food_production_scale_percent = 90;
        universe::Simulation constrained_run{constrained};
        constrained_run.run_years(30);
        verify_resource_invariants(constrained_run.state(), constrained.initial_household_food_units);
        verify_person_health_invariants(constrained_run.state());

        auto famine = config;
        famine.food_production_scale_percent = 40;
        universe::Simulation famine_run{famine};
        std::uint64_t peak_stressed_population = 0;
        std::int64_t peak_unmet_food = 0;
        for (std::uint64_t year = 0; year < 30U; ++year) {
            famine_run.run_years(1);
            peak_stressed_population = std::max(peak_stressed_population, famine_run.state().population_under_food_stress);
            peak_unmet_food = std::max(peak_unmet_food, famine_run.state().total_unmet_food);
        }
        verify_resource_invariants(famine_run.state(), famine.initial_household_food_units);
        verify_person_health_invariants(famine_run.state());
        require(peak_stressed_population > 0, "severe shortage should produce food stress");
        require(peak_unmet_food > 0, "severe shortage should produce unmet food demand");
        require(
            famine_run.state().total_deaths > abundant_run.state().total_deaths,
            "severe shortage should increase mortality relative to adequate-food baseline");

        std::int64_t max_total_stress = 0;
        std::int64_t final_total_stress = 0;
        universe::Simulation fluctuating{constrained};
        for (std::uint64_t year = 0; year < 80U; ++year) {
            fluctuating.run_years(1);
            std::int64_t total_stress = 0;
            for (const auto& household : fluctuating.state().households) {
                total_stress += household.resources.nutritional_stress_points;
            }
            max_total_stress = std::max(max_total_stress, total_stress);
            final_total_stress = total_stress;
        }
        require(max_total_stress > 0, "constrained food conditions should induce temporary stress");
        require(final_total_stress < max_total_stress, "food stress should be able to recover after temporary shortages");
    }

    {
        universe::Simulation boundary_a{config};
        boundary_a.advance(universe::days(364));
        require(boundary_a.state().year == 0, "partial year advance must not trigger yearly scheduler");

        boundary_a.advance(universe::days(1));
        require(boundary_a.state().year == 1, "crossing yearly boundary should trigger one yearly tick");

        universe::Simulation boundary_b{config};
        boundary_b.advance(universe::days(365));
        require_world_state_equal(
            boundary_a.state(),
            boundary_b.state(),
            "364+1 day chunking must match 365 day single chunk state");
    }

    {
        universe::Simulation three_steps{config};
        three_steps.step_year();
        three_steps.step_year();
        three_steps.step_year();

        universe::Simulation single_chunk{config};
        single_chunk.run_years(3);
        require_world_state_equal(
            three_steps.state(),
            single_chunk.state(),
            "three yearly steps must match a single three-year advance");
    }

    {
        universe::Simulation chunked_days{config};
        chunked_days.advance(universe::days(17));
        chunked_days.advance(universe::days(400));
        chunked_days.advance(universe::days(300));
        chunked_days.advance(universe::days(378));

        universe::Simulation contiguous_days{config};
        contiguous_days.advance(universe::days(1095));
        require_world_state_equal(
            chunked_days.state(),
            contiguous_days.state(),
            "equivalent day chunking must produce identical yearly outcomes");
    }

    {
        universe::Simulation chunked{config};
        for (std::uint64_t i = 0; i < 50; ++i) {
            chunked.advance(universe::days(73));
            chunked.advance(universe::days(292));
        }

        universe::Simulation contiguous{config};
        contiguous.run_years(50);
        require_world_state_equal(
            chunked.state(),
            contiguous.state(),
            "equivalent yearly boundaries must preserve temporal-seed determinism");

        const auto snapshot_chunked = chunked.save_snapshot();
        const auto snapshot_contiguous = contiguous.save_snapshot();
        require(
            snapshot_chunked == snapshot_contiguous,
            "equivalent advancement chunking should produce byte-identical snapshots");
    }

    {
        universe::Simulation invalid_duration{config};
        bool threw_invalid = false;
        try {
            invalid_duration.advance(universe::SimulationDuration{-1});
        } catch (const std::invalid_argument&) {
            threw_invalid = true;
        }
        require(threw_invalid, "negative duration advance must throw invalid_argument");

        universe::Simulation overflow_years{config};
        bool threw_years_overflow = false;
        try {
            overflow_years.run_years(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        } catch (const std::overflow_error&) {
            threw_years_overflow = true;
        }
        require(threw_years_overflow, "run_years must reject overflowing durations");
    }

    {
        universe::Simulation initial{config};
        initial.run_years(100);
        const auto bytes = initial.save_snapshot();
        auto loaded = load_from_snapshot_or_fail(bytes, "100-year snapshot must load");
        require_world_state_equal(initial.state(), loaded.state(), "100-year snapshot round-trip must preserve state");
    }

    {
        universe::Simulation continuous{config};
        continuous.run_years(500);
        const auto continuous_bytes = continuous.save_snapshot();

        universe::Simulation with_boundary{config};
        with_boundary.run_years(250);
        const auto mid_bytes = with_boundary.save_snapshot();
        auto resumed = load_from_snapshot_or_fail(mid_bytes, "midpoint snapshot must load");
        resumed.run_years(250);
        const auto resumed_bytes = resumed.save_snapshot();

        require_world_state_equal(
            continuous.state(),
            resumed.state(),
            "500 years continuous must equal 250+save/load+250");
        require(
            continuous_bytes == resumed_bytes,
            "500 years continuous and 250+save/load+250 must serialize identically");
    }

    {
        universe::Simulation continuous{config};
        continuous.run_years(500);

        universe::Simulation segmented{config};
        segmented.run_years(100);
        auto snap1 = segmented.save_snapshot();
        segmented = load_from_snapshot_or_fail(snap1, "snapshot boundary 1 must load");

        segmented.run_years(100);
        auto snap2 = segmented.save_snapshot();
        segmented = load_from_snapshot_or_fail(snap2, "snapshot boundary 2 must load");

        segmented.run_years(100);
        auto snap3 = segmented.save_snapshot();
        segmented = load_from_snapshot_or_fail(snap3, "snapshot boundary 3 must load");

        segmented.run_years(200);

        require_world_state_equal(
            continuous.state(),
            segmented.state(),
            "continuous 500-year run must equal repeated save/load segmented run");
        require(
            continuous.save_snapshot() == segmented.save_snapshot(),
            "continuous and repeated save/load segmented runs must serialize identically");
    }

    {
        universe::Simulation before_boundary{config};
        before_boundary.advance(universe::days(364));
        const auto pre_bytes = before_boundary.save_snapshot();
        auto pre_loaded = load_from_snapshot_or_fail(pre_bytes, "pre-boundary snapshot must load");
        pre_loaded.advance(universe::days(1));

        universe::Simulation direct_cross{config};
        direct_cross.advance(universe::days(365));
        require_world_state_equal(
            pre_loaded.state(),
            direct_cross.state(),
            "saving before boundary then crossing must equal uninterrupted crossing");

        universe::Simulation after_boundary{config};
        after_boundary.advance(universe::days(365));
        const auto post_bytes = after_boundary.save_snapshot();
        auto post_loaded = load_from_snapshot_or_fail(post_bytes, "post-boundary snapshot must load");
        require_world_state_equal(
            after_boundary.state(),
            post_loaded.state(),
            "saving after boundary and reloading must not duplicate processing");
    }

    {
        universe::Simulation empty_household_regression{universe::SimulationConfig{
            .seed = 77,
            .initial_population = 1,
            .enable_unrelated_random_probe = false,
            .initial_household_food_units = 0,
            .food_production_scale_percent = 0,
            .food_system_enabled = true,
        }};

        empty_household_regression.run_years(250);
        const auto& before = empty_household_regression.state();

        bool found_empty_household = false;
        std::unordered_map<universe::HouseholdId, std::int64_t> produced_before;
        for (const auto& household : before.households) {
            if (!household.member_ids.empty()) {
                continue;
            }
            found_empty_household = true;
            produced_before.emplace(household.id, household.resources.food_produced_total);
        }
        require(found_empty_household, "regression setup must produce at least one empty household");

        empty_household_regression.advance(universe::days(30));
        const auto& after = empty_household_regression.state();
        for (const auto& household : after.households) {
            const auto it = produced_before.find(household.id);
            if (it == produced_before.end()) {
                continue;
            }
            require(
                household.resources.food_produced_total == it->second,
                "empty household must not produce food");
        }
    }

    {
        universe::Simulation source{config};
        const auto valid = source.save_snapshot();

        auto bad_magic = clone_bytes(valid);
        bad_magic[0] = std::byte{0};
        require_snapshot_load_fails(bad_magic, universe::SnapshotErrorCode::InvalidMagic, "bad magic must be rejected");

        auto bad_version = clone_bytes(valid);
        bad_version[4] = std::byte{0x04};
        bad_version[5] = std::byte{0x00};
        require_snapshot_load_fails(
            bad_version,
            universe::SnapshotErrorCode::UnsupportedVersion,
            "unsupported schema version must be rejected");

        auto truncated = clone_bytes(valid);
        truncated.pop_back();
        require_snapshot_load_fails(
            truncated,
            universe::SnapshotErrorCode::TruncatedData,
            "truncated snapshot must be rejected");

        auto checksum_corrupt = clone_bytes(valid);
        corrupt_byte(checksum_corrupt, 40, 0x01);
        require_snapshot_load_fails(
            checksum_corrupt,
            universe::SnapshotErrorCode::InvalidChecksum,
            "payload corruption must be rejected via checksum");

        constexpr std::size_t header_size = 24U;
        constexpr std::size_t payload_people_count_offset = 182U;
        constexpr std::size_t payload_first_person_offset = 190U;
        constexpr std::size_t payload_person_record_size = 40U;
        constexpr std::size_t person_sex_offset_in_record = 16U;
        constexpr std::size_t person_household_offset_in_record = 18U;

        auto invalid_enum = clone_bytes(valid);
        invalid_enum[header_size + payload_first_person_offset + person_sex_offset_in_record] = std::byte{2};
        refresh_snapshot_payload_hash(invalid_enum);
        require_snapshot_load_fails(
            invalid_enum,
            universe::SnapshotErrorCode::InvalidEnum,
            "invalid enum values must be rejected");

        auto duplicate_person_id = clone_bytes(valid);
        const auto first_person_id = read_u64_le(duplicate_person_id, header_size + payload_first_person_offset);
        write_u64_le(
            duplicate_person_id,
            header_size + payload_first_person_offset + payload_person_record_size,
            first_person_id);
        refresh_snapshot_payload_hash(duplicate_person_id);
        require_snapshot_load_fails(
            duplicate_person_id,
            universe::SnapshotErrorCode::DuplicateId,
            "duplicate stable person ids must be rejected");

        auto invalid_reference = clone_bytes(valid);
        write_u64_le(
            invalid_reference,
            header_size + payload_first_person_offset + person_household_offset_in_record,
            9'999'999U);
        refresh_snapshot_payload_hash(invalid_reference);
        require_snapshot_load_fails(
            invalid_reference,
            universe::SnapshotErrorCode::InvalidReference,
            "invalid person->household references must be rejected");

        auto malformed_lengths = clone_bytes(valid);
        write_u64_le(malformed_lengths, header_size + payload_people_count_offset, static_cast<std::uint64_t>(1'000'000U));
        refresh_snapshot_payload_hash(malformed_lengths);
        require_snapshot_load_fails(
            malformed_lengths,
            universe::SnapshotErrorCode::InvalidStructure,
            "malformed collection lengths must be rejected");
    }

    universe::Simulation a{config};
    universe::Simulation b{config};
    a.run_years(100);
    b.run_years(100);

    require(a.state().year == 100, "simulation should advance exactly 100 years");
    require_world_state_equal(a.state(), b.state(), "same seed must reproduce full world state and event log");

    auto different_config = config;
    different_config.seed = 43;
    universe::Simulation c{different_config};
    c.run_years(100);

    const bool differs =
        a.state().population != c.state().population ||
        a.state().total_births != c.state().total_births ||
        a.state().total_deaths != c.state().total_deaths ||
        a.state().event_log.size() != c.state().event_log.size() ||
        a.state().people.size() != c.state().people.size();
    require(differs, "different seeds should normally produce different histories");

    auto probe_config = config;
    probe_config.enable_unrelated_random_probe = true;
    universe::Simulation with_probe{probe_config};
    with_probe.run_years(100);
    require_world_state_equal(a.state(), with_probe.state(), "unrelated scoped random probes must not alter canonical results");

    {
        auto different_seed = config;
        different_seed.seed = 43;
        universe::Simulation lhs{config};
        universe::Simulation rhs{different_seed};
        lhs.run_years(100);
        rhs.run_years(100);
        require(lhs.save_snapshot() != rhs.save_snapshot(), "different seeds should produce different canonical snapshot bytes");
    }

    verify_demographic_invariants(a.state());
    verify_event_log_invariants(a.state());
    verify_resource_invariants(a.state(), config.initial_household_food_units);
    verify_person_health_invariants(a.state());

    std::uint64_t people_with_recorded_parents = 0;
    for (const auto& person : a.state().people) {
        if (!person.mother_id.has_value()) {
            continue;
        }

        ++people_with_recorded_parents;
        const auto* mother = find_person(a.state(), *person.mother_id);
        require(mother != nullptr, "mother id must reference an existing person");
        require(mother->sex == universe::BiologicalSex::Female, "mother must be female in biological model");
        require(mother->birth_year < person.birth_year, "mother must be older than child");
        require(mother->household_id == person.household_id, "newborn should initially share mother's household");

        require(person.father_id.has_value(), "newborn should have a sampled father id when a household father exists");
        const auto* father = find_person(a.state(), *person.father_id);
        require(father != nullptr, "father id must reference an existing person");
        require(father->sex == universe::BiologicalSex::Male, "father must be male in biological model");
        require(father->birth_year < person.birth_year, "father must be older than child");
    }
    require(people_with_recorded_parents > 0, "simulation should produce births with parent relationships");

    universe::Simulation lifecycle{config};
    lifecycle.run_years(30);
    std::uint64_t born_and_now_adult = 0;
    for (const auto& person : lifecycle.state().people) {
        if (person.birth_year >= 0 && person.alive && age_years(lifecycle.state(), person) >= 18U) {
            ++born_and_now_adult;
        }
    }
    require(born_and_now_adult > 0, "children born during simulation should age into adults over decades");

    std::uint64_t extinct_runs = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        universe::Simulation sweep{universe::SimulationConfig{.seed = seed, .initial_population = 200}};
        sweep.run_years(100);
        verify_demographic_invariants(sweep.state());
        verify_event_log_invariants(sweep.state());
        verify_resource_invariants(sweep.state(), 80);
        verify_person_health_invariants(sweep.state());
        if (sweep.state().population == 0) {
            ++extinct_runs;
        }
    }
    require(extinct_runs < 20, "population should not always go extinct across seed sweep");

    const std::vector<std::uint64_t> long_run_seeds{
        1U, 2U, 3U, 4U, 5U,
        9U, 12U, 51U, 54U, 66U,
        110U, 118U,
    };
    std::uint64_t long_run_extinct = 0;
    std::uint64_t long_run_survived = 0;
    for (const auto seed : long_run_seeds) {
        universe::Simulation long_horizon{universe::SimulationConfig{.seed = seed, .initial_population = 20}};
        long_horizon.run_years(500);
        verify_demographic_invariants(long_horizon.state());
        verify_event_log_invariants(long_horizon.state());
        verify_resource_invariants(long_horizon.state(), 80);
        verify_person_health_invariants(long_horizon.state());
        require(long_horizon.state().population < 1000, "small population long run should avoid uncontrolled explosion");

        if (long_horizon.state().population == 0) {
            ++long_run_extinct;
        } else {
            ++long_run_survived;
        }
    }
    require(long_run_survived > 0, "long-horizon small populations should not always go extinct");
    require(long_run_extinct > 0, "long-horizon small populations should not always survive either");

    std::cout << "All UniverseCore tests passed.\n";
    return 0;
}
