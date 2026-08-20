#include "universe/simulation.hpp"
#include "universe/seed.hpp"

#include <algorithm>
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
    }

    const universe::SimulationConfig config{
        .seed = 42,
        .initial_population = 200,
        .enable_unrelated_random_probe = false,
    };

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

    verify_demographic_invariants(a.state());
    verify_event_log_invariants(a.state());

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
