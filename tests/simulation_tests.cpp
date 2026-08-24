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

std::int64_t person_labour_capacity(
    const universe::WorldState& state,
    const universe::Person& person,
    const std::uint32_t labour_scale_percent) {
    const universe::FoodModelParameters parameters{};
    return universe::person_labour_capacity_units(
        person.alive,
        age_years(state, person),
        person.nutrition_points,
        person.health_points,
        parameters,
        labour_scale_percent);
}

bool occupation_eligible_for_productive_work(
    const universe::WorldState& state,
    const universe::Person& person,
    const std::uint32_t labour_scale_percent) {
    if (!person.alive) {
        return false;
    }
    if (age_years(state, person) < 18U) {
        return false;
    }
    if (person.nutrition_points < 160 || person.health_points < 160) {
        return false;
    }
    return person_labour_capacity(state, person, labour_scale_percent) > 0;
}

std::int64_t occupation_allocated_labour(const universe::WorldState& state, const universe::Occupation occupation) {
    std::int64_t total = 0;
    for (const auto& person : state.people) {
        if (person.occupation != occupation) {
            continue;
        }
        if (!occupation_eligible_for_productive_work(state, person, 100U)) {
            continue;
        }
        total += person_labour_capacity(state, person, 100U);
    }
    return total;
}

std::int64_t total_labour_capacity_by_people(
    const universe::WorldState& state,
    const std::uint32_t labour_scale_percent,
    const bool reverse_order) {
    std::vector<const universe::Person*> ordered;
    ordered.reserve(state.people.size());
    for (const auto& person : state.people) {
        ordered.push_back(&person);
    }
    std::sort(ordered.begin(), ordered.end(), [](const universe::Person* lhs, const universe::Person* rhs) {
        return lhs->id < rhs->id;
    });
    if (reverse_order) {
        std::reverse(ordered.begin(), ordered.end());
    }

    std::int64_t total = 0;
    for (const auto* person : ordered) {
        total += person_labour_capacity(state, *person, labour_scale_percent);
    }
    return total;
}

std::int64_t total_labour_capacity_by_households(
    const universe::WorldState& state,
    const std::uint32_t labour_scale_percent,
    const bool reverse_order) {
    std::vector<const universe::Household*> ordered;
    ordered.reserve(state.households.size());
    for (const auto& household : state.households) {
        ordered.push_back(&household);
    }
    std::sort(ordered.begin(), ordered.end(), [](const universe::Household* lhs, const universe::Household* rhs) {
        return lhs->id < rhs->id;
    });
    if (reverse_order) {
        std::reverse(ordered.begin(), ordered.end());
    }

    std::int64_t total = 0;
    for (const auto* household : ordered) {
        for (const auto member_id : household->member_ids) {
            const universe::Person* person = nullptr;
            for (const auto& candidate : state.people) {
                if (candidate.id == member_id) {
                    person = &candidate;
                    break;
                }
            }
            if (person == nullptr) {
                continue;
            }
            total += person_labour_capacity(state, *person, labour_scale_percent);
        }
    }
    return total;
}

std::int64_t total_labour_capacity_by_settlements(
    const universe::WorldState& state,
    const std::uint32_t labour_scale_percent,
    const bool reverse_order) {
    std::vector<const universe::Settlement*> ordered;
    ordered.reserve(state.settlements.size());
    for (const auto& settlement : state.settlements) {
        ordered.push_back(&settlement);
    }
    std::sort(ordered.begin(), ordered.end(), [](const universe::Settlement* lhs, const universe::Settlement* rhs) {
        return lhs->id < rhs->id;
    });
    if (reverse_order) {
        std::reverse(ordered.begin(), ordered.end());
    }

    std::int64_t total = 0;
    for (const auto* settlement : ordered) {
        for (const auto& household : state.households) {
            if (household.settlement_id != settlement->id) {
                continue;
            }
            for (const auto member_id : household.member_ids) {
                const universe::Person* person = nullptr;
                for (const auto& candidate : state.people) {
                    if (candidate.id == member_id) {
                        person = &candidate;
                        break;
                    }
                }
                if (person == nullptr) {
                    continue;
                }
                total += person_labour_capacity(state, *person, labour_scale_percent);
            }
        }
    }
    return total;
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

const universe::Settlement* find_settlement(const universe::WorldState& state, const universe::SettlementId id) {
    for (const auto& settlement : state.settlements) {
        if (settlement.id == id) {
            return &settlement;
        }
    }
    return nullptr;
}

void verify_settlement_invariants(const universe::WorldState& state) {
    std::unordered_set<universe::SettlementId> settlement_ids;
    std::unordered_map<universe::RegionId, std::uint32_t> allocated_share_by_region;

    for (const auto& settlement : state.settlements) {
        require(settlement.id > 0, "settlement id must be non-zero");
        require(settlement_ids.insert(settlement.id).second, "settlement ids must be unique");
        require(settlement.region_id > 0, "settlement region id must be non-zero");
        require(settlement.access_share_points <= 10'000U, "settlement share points must be bounded");
        require(settlement.tool_wear_work_units >= 0, "settlement tool wear work units must be non-negative");
        require(settlement.last_tool_coverage_per_10k <= 10'000U, "settlement tool coverage must be bounded");
        require(settlement.cumulative_basic_tool_broken >= 0, "settlement broken tools must be non-negative");
        if (!settlement.active) {
            require(settlement.access_share_points == 0U, "extinct settlement must not retain active share points");
        } else {
            allocated_share_by_region[settlement.region_id] += settlement.access_share_points;
        }
    }

    for (const auto& [region_id, share] : allocated_share_by_region) {
        (void)region_id;
        require(share <= 10'000U, "active settlement allocation must not exceed regional capacity");
    }

    for (const auto& household : state.households) {
        const auto* settlement = find_settlement(state, household.settlement_id);
        require(settlement != nullptr, "household settlement id must reference existing settlement");
        require(settlement->region_id == household.region_id, "household settlement and region must be consistent");
    }
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

        if ((!person.alive || age_years(state, person) < 18U) && person.occupation != universe::Occupation::None) {
            require(person.occupation == universe::Occupation::None, "ineligible person must not have productive occupation");
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
    std::int64_t total_productive_capacity_per_day = 0;
    std::int64_t total_wild_food_capacity = 0;
    std::int64_t total_wild_food_stock = 0;
    std::int64_t total_wild_food_harvested = 0;
    std::int64_t total_wild_food_regenerated = 0;
    std::int64_t total_inferred_initial_food = 0;
    std::int64_t total_basic_tool_broken = 0;
    std::uint64_t active_settlement_count = 0;
    std::uint64_t coverage_sum = 0;

    std::unordered_set<universe::RegionId> region_ids;
    std::unordered_set<universe::SettlementId> settlement_ids;
    for (const auto& settlement : state.settlements) {
        settlement_ids.insert(settlement.id);
        total_basic_tool_broken += settlement.cumulative_basic_tool_broken;
        if (settlement.active) {
            ++active_settlement_count;
            coverage_sum += settlement.last_tool_coverage_per_10k;
        }
    }

    for (const auto& region : state.regions) {
        require(region.id > 0, "region id must be non-zero");
        require(region_ids.insert(region.id).second, "region ids must be unique");
        require(region.land_area_units > 0, "region area must be positive");
        require(region.productive_capacity_per_day >= 0, "region productive capacity must be non-negative");
        require(region.wild_food_capacity >= 0, "region wild capacity must be non-negative");
        require(region.wild_food_stock >= 0, "region wild stock must be non-negative");
        require(region.wild_food_regeneration_per_day >= 0, "region wild regeneration must be non-negative");
        require(region.wild_food_harvest_limit_per_day >= 0, "region wild harvest limit must be non-negative");
        require(region.cumulative_wild_food_harvested >= 0, "region cumulative wild harvest must be non-negative");
        require(region.cumulative_wild_food_regenerated >= 0, "region cumulative wild regeneration must be non-negative");
        require(
            region.cumulative_wild_food_harvested <= region.wild_food_capacity + region.cumulative_wild_food_regenerated,
            "region cumulative wild harvest must not exceed initial stock plus regeneration");
        require(region.wild_food_stock <= region.wild_food_capacity, "region wild stock must not exceed capacity");

        total_productive_capacity_per_day += region.productive_capacity_per_day;
        total_wild_food_capacity += region.wild_food_capacity;
        total_wild_food_stock += region.wild_food_stock;
        total_wild_food_harvested += region.cumulative_wild_food_harvested;
        total_wild_food_regenerated += region.cumulative_wild_food_regenerated;
    }

    require(!state.regions.empty(), "world must contain at least one region");

    for (const auto& household : state.households) {
        const auto& resources = household.resources;
        require(region_ids.contains(household.region_id), "household region id must reference an existing region");
        require(settlement_ids.contains(household.settlement_id), "household settlement id must reference an existing settlement");
        require(resources.food_inventory.value >= 0, "household food inventory must never be negative");
        require(resources.food_produced_total >= 0, "household food produced total must never be negative");
        require(resources.food_consumed_total >= 0, "household food consumed total must never be negative");
        require(resources.unmet_food_total >= 0, "household unmet food total must never be negative");
        require(resources.nutritional_stress_points >= 0, "household stress must never be negative");

        const auto inferred_initial =
            resources.food_inventory.value + resources.food_consumed_total - resources.food_produced_total;
        total_inferred_initial_food += inferred_initial;

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
    require(
        total_inferred_initial_food == expected_initial_food_units * static_cast<std::int64_t>(state.households.size()),
        "food accounting should preserve canonical world-level inventory conservation");
    require(households_in_shortage == state.households_in_shortage, "shortage household aggregate must match");
    require(stressed_population == state.population_under_food_stress, "population under stress aggregate must match");
    require(
        total_productive_capacity_per_day == state.total_productive_capacity_per_day,
        "productive capacity aggregate must match region totals");
    require(total_wild_food_capacity == state.total_wild_food_capacity, "wild capacity aggregate must match region totals");
    require(total_wild_food_stock == state.total_wild_food_stock, "wild stock aggregate must match region totals");
    require(total_wild_food_harvested == state.total_wild_food_harvested, "wild harvest aggregate must match region totals");
    require(
        total_wild_food_regenerated == state.total_wild_food_regenerated,
        "wild regeneration aggregate must match region totals");

    require(state.total_labour_capacity >= 0, "total labour capacity must be non-negative");
    require(state.total_labour_demand >= 0, "total labour demand must be non-negative");
    require(state.total_labour_allocated >= 0, "total labour allocated must be non-negative");
    require(state.cultivation_labour_available >= 0, "cultivation labour availability must be non-negative");
    require(state.gathering_labour_available >= 0, "gathering labour availability must be non-negative");
    require(state.cultivation_labour_allocated >= 0, "cultivation labour allocation must be non-negative");
    require(state.gathering_labour_allocated >= 0, "gathering labour allocation must be non-negative");
    require(state.labour_shortfall >= 0, "labour shortfall must be non-negative");
    require(
        state.total_labour_allocated == state.cultivation_labour_allocated + state.gathering_labour_allocated,
        "total allocated labour must equal specialized allocations");
    require(
        state.total_labour_allocated <= state.total_labour_capacity,
        "allocated labour must not exceed available labour capacity");
    require(
        state.cultivation_labour_allocated <= state.cultivation_labour_available,
        "cultivation allocated labour must not exceed available cultivation labour");
    require(
        state.gathering_labour_allocated <= state.gathering_labour_available,
        "gathering allocated labour must not exceed available gathering labour");
    require(
        state.active_workers == state.cultivators + state.gatherers + state.wood_gatherers + state.stone_extractors + state.craft_workers,
        "active workers must equal productive occupation totals");
    require(
        state.eligible_workers == state.active_workers + state.unallocated_workers,
        "eligible workers must equal active plus unallocated workers");
    require(
        state.labour_shortfall == std::max<std::int64_t>(0, state.total_labour_demand - state.total_labour_allocated),
        "labour shortfall must match demand minus allocated labour");

    require(state.total_wood_capacity >= 0, "total wood capacity must be non-negative");
    require(state.total_wood_stock >= 0, "total wood stock must be non-negative");
    require(state.total_stone_capacity >= 0, "total stone capacity must be non-negative");
    require(state.total_stone_remaining >= 0, "total stone remaining must be non-negative");
    require(state.total_wood_stock <= state.total_wood_capacity, "total wood stock must not exceed capacity");
    require(state.total_stone_remaining <= state.total_stone_capacity, "total stone remaining must not exceed capacity");
    require(state.total_wood_inventory >= 0, "total wood inventory must be non-negative");
    require(state.total_stone_inventory >= 0, "total stone inventory must be non-negative");
    require(state.total_timber_inventory >= 0, "total timber inventory must be non-negative");
    require(state.total_basic_tool_inventory >= 0, "total basic tool inventory must be non-negative");
    require(state.total_basic_tool_broken >= 0, "total basic tool broken must be non-negative");
    require(state.average_tool_coverage_per_10k <= 10'000U, "average tool coverage must be bounded");
    require(state.total_basic_tool_broken == total_basic_tool_broken, "total broken tools must match settlement totals");
    const auto expected_average_coverage = active_settlement_count > 0U
        ? static_cast<std::uint32_t>(coverage_sum / active_settlement_count)
        : 0U;
    require(
        state.average_tool_coverage_per_10k == expected_average_coverage,
        "average tool coverage must match settlement-level values");
    require(state.material_labour_demand >= 0, "material labour demand must be non-negative");
    require(state.material_labour_allocated >= 0, "material labour allocated must be non-negative");
    require(state.material_labour_shortfall >= 0, "material labour shortfall must be non-negative");
    require(
        state.material_labour_allocated ==
            state.wood_labour_allocated + state.stone_labour_allocated + state.craft_labour_allocated,
        "material allocated labour must equal specialized allocations");
    require(
        state.material_labour_shortfall == std::max<std::int64_t>(0, state.material_labour_demand - state.material_labour_allocated),
        "material labour shortfall must match demand minus allocated labour");

    verify_settlement_invariants(state);
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
    require(lhs.total_productive_capacity_per_day == rhs.total_productive_capacity_per_day, context);
    require(lhs.total_wild_food_capacity == rhs.total_wild_food_capacity, context);
    require(lhs.total_wild_food_stock == rhs.total_wild_food_stock, context);
    require(lhs.total_wild_food_harvested == rhs.total_wild_food_harvested, context);
    require(lhs.total_wild_food_regenerated == rhs.total_wild_food_regenerated, context);
    require(lhs.eligible_workers == rhs.eligible_workers, context);
    require(lhs.active_workers == rhs.active_workers, context);
    require(lhs.unallocated_workers == rhs.unallocated_workers, context);
    require(lhs.cultivators == rhs.cultivators, context);
    require(lhs.gatherers == rhs.gatherers, context);
    require(lhs.wood_gatherers == rhs.wood_gatherers, context);
    require(lhs.stone_extractors == rhs.stone_extractors, context);
    require(lhs.craft_workers == rhs.craft_workers, context);
    require(lhs.total_labour_capacity == rhs.total_labour_capacity, context);
    require(lhs.total_labour_demand == rhs.total_labour_demand, context);
    require(lhs.total_labour_allocated == rhs.total_labour_allocated, context);
    require(lhs.cultivation_labour_available == rhs.cultivation_labour_available, context);
    require(lhs.cultivation_labour_allocated == rhs.cultivation_labour_allocated, context);
    require(lhs.gathering_labour_available == rhs.gathering_labour_available, context);
    require(lhs.gathering_labour_allocated == rhs.gathering_labour_allocated, context);
    require(lhs.labour_shortfall == rhs.labour_shortfall, context);
    require(lhs.total_wood_capacity == rhs.total_wood_capacity, context);
    require(lhs.total_wood_stock == rhs.total_wood_stock, context);
    require(lhs.total_stone_capacity == rhs.total_stone_capacity, context);
    require(lhs.total_stone_remaining == rhs.total_stone_remaining, context);
    require(lhs.total_wood_harvested == rhs.total_wood_harvested, context);
    require(lhs.total_wood_regenerated == rhs.total_wood_regenerated, context);
    require(lhs.total_stone_extracted == rhs.total_stone_extracted, context);
    require(lhs.total_wood_inventory == rhs.total_wood_inventory, context);
    require(lhs.total_stone_inventory == rhs.total_stone_inventory, context);
    require(lhs.total_timber_inventory == rhs.total_timber_inventory, context);
    require(lhs.total_basic_tool_inventory == rhs.total_basic_tool_inventory, context);
    require(lhs.total_timber_produced == rhs.total_timber_produced, context);
    require(lhs.total_basic_tool_produced == rhs.total_basic_tool_produced, context);
    require(lhs.total_basic_tool_broken == rhs.total_basic_tool_broken, context);
    require(lhs.average_tool_coverage_per_10k == rhs.average_tool_coverage_per_10k, context);
    require(lhs.wood_labour_available == rhs.wood_labour_available, context);
    require(lhs.wood_labour_allocated == rhs.wood_labour_allocated, context);
    require(lhs.stone_labour_available == rhs.stone_labour_available, context);
    require(lhs.stone_labour_allocated == rhs.stone_labour_allocated, context);
    require(lhs.craft_labour_available == rhs.craft_labour_available, context);
    require(lhs.craft_labour_allocated == rhs.craft_labour_allocated, context);
    require(lhs.material_labour_demand == rhs.material_labour_demand, context);
    require(lhs.material_labour_allocated == rhs.material_labour_allocated, context);
    require(lhs.material_labour_shortfall == rhs.material_labour_shortfall, context);
    require(lhs.material_production_shortfall == rhs.material_production_shortfall, context);
    require(lhs.people.size() == rhs.people.size(), context);
    require(lhs.households.size() == rhs.households.size(), context);
    require(lhs.regions.size() == rhs.regions.size(), context);
    require(lhs.settlements.size() == rhs.settlements.size(), context);
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
        require(a.occupation == b.occupation, context);
    }

    for (std::size_t i = 0; i < lhs.households.size(); ++i) {
        const auto& a = lhs.households[i];
        const auto& b = rhs.households[i];
        require(a.id == b.id, context);
        require(a.region_id == b.region_id, context);
        require(a.settlement_id == b.settlement_id, context);

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

    for (std::size_t i = 0; i < lhs.regions.size(); ++i) {
        const auto& a = lhs.regions[i];
        const auto& b = rhs.regions[i];
        require(a.id == b.id, context);
        require(a.land_area_units == b.land_area_units, context);
        require(a.environment_class == b.environment_class, context);
        require(a.productive_capacity_per_day == b.productive_capacity_per_day, context);
        require(a.wild_food_capacity == b.wild_food_capacity, context);
        require(a.wild_food_stock == b.wild_food_stock, context);
        require(a.wild_food_regeneration_per_day == b.wild_food_regeneration_per_day, context);
        require(a.wild_food_harvest_limit_per_day == b.wild_food_harvest_limit_per_day, context);
        require(a.cumulative_wild_food_harvested == b.cumulative_wild_food_harvested, context);
        require(a.cumulative_wild_food_regenerated == b.cumulative_wild_food_regenerated, context);
        require(a.wood_capacity == b.wood_capacity, context);
        require(a.wood_stock == b.wood_stock, context);
        require(a.wood_regeneration_per_day == b.wood_regeneration_per_day, context);
        require(a.cumulative_wood_harvested == b.cumulative_wood_harvested, context);
        require(a.cumulative_wood_regenerated == b.cumulative_wood_regenerated, context);
        require(a.stone_capacity == b.stone_capacity, context);
        require(a.stone_remaining == b.stone_remaining, context);
        require(a.cumulative_stone_extracted == b.cumulative_stone_extracted, context);
    }

    for (std::size_t i = 0; i < lhs.settlements.size(); ++i) {
        const auto& a = lhs.settlements[i];
        const auto& b = rhs.settlements[i];
        require(a.id == b.id, context);
        require(a.region_id == b.region_id, context);
        require(a.active == b.active, context);
        require(a.access_share_points == b.access_share_points, context);
        require(a.materials.wood.value == b.materials.wood.value, context);
        require(a.materials.stone.value == b.materials.stone.value, context);
        require(a.materials.timber.value == b.materials.timber.value, context);
        require(a.materials.basic_tool.value == b.materials.basic_tool.value, context);
        require(a.tool_wear_work_units == b.tool_wear_work_units, context);
        require(a.last_tool_coverage_per_10k == b.last_tool_coverage_per_10k, context);
        require(a.cumulative_wood_extracted == b.cumulative_wood_extracted, context);
        require(a.cumulative_stone_extracted == b.cumulative_stone_extracted, context);
        require(a.cumulative_timber_produced == b.cumulative_timber_produced, context);
        require(a.cumulative_basic_tool_produced == b.cumulative_basic_tool_produced, context);
        require(a.cumulative_basic_tool_broken == b.cumulative_basic_tool_broken, context);
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

void write_u32_le(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t i = 0; i < 4U; ++i) {
        const auto shifted = static_cast<std::uint32_t>(value >> (8U * i));
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
    if (!load_result.ok) {
        std::cerr << "Snapshot load failed (" << static_cast<unsigned>(load_result.code) << "): " << load_result.message << '\n';
    }
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

    {
        const universe::FoodModelParameters labour_params{};

        const auto dead_capacity = universe::person_labour_capacity_units(
            false,
            30U,
            900,
            900,
            labour_params,
            100U);
        require(dead_capacity == 0, "dead person = zero labour");

        const auto adult_capacity = universe::person_labour_capacity_units(
            true,
            30U,
            900,
            900,
            labour_params,
            100U);
        const auto child_capacity = universe::person_labour_capacity_units(
            true,
            10U,
            900,
            900,
            labour_params,
            100U);
        const auto elderly_capacity = universe::person_labour_capacity_units(
            true,
            75U,
            900,
            900,
            labour_params,
            100U);
        require(adult_capacity > child_capacity, "healthy working-age adult > child labour capacity");
        require(adult_capacity > elderly_capacity, "healthy working-age adult > elderly labour capacity");

        const auto healthy_capacity = universe::person_labour_capacity_units(
            true,
            30U,
            900,
            900,
            labour_params,
            100U);
        const auto malnourished_capacity = universe::person_labour_capacity_units(
            true,
            30U,
            180,
            220,
            labour_params,
            100U);
        const auto recovered_capacity = universe::person_labour_capacity_units(
            true,
            30U,
            900,
            900,
            labour_params,
            100U);
        require(malnourished_capacity < healthy_capacity, "malnutrition reduces capacity");
        require(recovered_capacity >= healthy_capacity, "recovery restores capacity");
    }

    {
        universe::Simulation occupation_baseline{universe::SimulationConfig{
            .seed = 31415,
            .initial_population = 220,
            .initial_household_food_units = 80,
            .food_production_scale_percent = 100,
            .productive_effort_scale_percent = 100,
        }};

        occupation_baseline.advance(universe::days(1));
        const auto& state = occupation_baseline.state();

        bool saw_healthy_working_age = false;
        for (const auto& person : state.people) {
            const auto eligible = occupation_eligible_for_productive_work(state, person, 100U);
            if (!person.alive) {
                require(person_labour_capacity(state, person, 100U) == 0, "dead people provide zero labour");
            }

            if (age_years(state, person) < 18U) {
                require(
                    person.occupation == universe::Occupation::None,
                    "ineligible children must not receive productive occupations");
            }

            if (eligible) {
                if (person.nutrition_points >= 700 && person.health_points >= 700 && age_years(state, person) >= 18U && age_years(state, person) <= 55U) {
                    saw_healthy_working_age = true;
                }

                require(
                    person.occupation == universe::Occupation::None ||
                        person.occupation == universe::Occupation::SubsistenceCultivator ||
                        person.occupation == universe::Occupation::WildFoodGatherer,
                    "eligible person occupation must be valid");
            } else {
                require(
                    person.occupation == universe::Occupation::None,
                    "ineligible or severely impaired person should remain nonproductive");
            }
        }

        require(saw_healthy_working_age, "simulation should include healthy working-age adults eligible for productive occupations");
        require(
            state.total_labour_allocated <= state.total_labour_capacity,
            "total allocated labour must never exceed available labour capacity");
        require(
            state.cultivation_labour_allocated <= state.cultivation_labour_available,
            "cultivation allocated labour must not exceed cultivation availability");
        require(
            state.gathering_labour_allocated <= state.gathering_labour_available,
            "gathering allocated labour must not exceed gathering availability");
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
        auto environment_stress = config;
        environment_stress.initial_region_count = 3;
        environment_stress.base_region_area_units = 120;
        environment_stress.food_production_scale_percent = 70;
        environment_stress.productive_effort_scale_percent = 65;

        universe::Simulation stressed_run{environment_stress};
        std::int64_t peak_wild_harvested = 0;
        std::int64_t peak_wild_regenerated = 0;
        for (std::uint64_t year = 0; year < 40U; ++year) {
            stressed_run.run_years(1);
            peak_wild_harvested = std::max(peak_wild_harvested, stressed_run.state().total_wild_food_harvested);
            peak_wild_regenerated = std::max(peak_wild_regenerated, stressed_run.state().total_wild_food_regenerated);
        }

        verify_resource_invariants(stressed_run.state(), environment_stress.initial_household_food_units);
        require(stressed_run.state().total_productive_capacity_per_day > 0, "regions must provide productive capacity");
        require(stressed_run.state().total_wild_food_capacity > 0, "regions must provide wild capacity");
        require(peak_wild_harvested > 0, "environment stress should trigger wild harvest usage");
        require(peak_wild_regenerated > 0, "wild depletion should trigger regeneration over time");
    }

    {
        auto low_pressure = config;
        low_pressure.initial_region_count = 4;
        low_pressure.base_region_area_units = 140;
        low_pressure.food_production_scale_percent = 170;
        low_pressure.productive_effort_scale_percent = 170;

        auto high_pressure = low_pressure;
        high_pressure.food_production_scale_percent = 60;
        high_pressure.productive_effort_scale_percent = 60;

        universe::Simulation low_pressure_run{low_pressure};
        universe::Simulation high_pressure_run{high_pressure};
        low_pressure_run.run_years(20);
        high_pressure_run.run_years(20);

        verify_resource_invariants(low_pressure_run.state(), low_pressure.initial_household_food_units);
        verify_resource_invariants(high_pressure_run.state(), high_pressure.initial_household_food_units);

        require(
            high_pressure_run.state().total_unmet_food >= low_pressure_run.state().total_unmet_food,
            "higher carrying-capacity pressure should not reduce unmet food demand");
        require(
            high_pressure_run.state().population_under_food_stress >= low_pressure_run.state().population_under_food_stress,
            "higher carrying-capacity pressure should not reduce food stress");
    }

    {
        auto settlement_config = config;
        settlement_config.initial_population = 320;
        settlement_config.initial_region_count = 2;

        universe::Simulation lhs{settlement_config};
        universe::Simulation rhs{settlement_config};
        require_world_state_equal(lhs.state(), rhs.state(), "same seed must produce identical initial settlements");

        auto different_seed = settlement_config;
        different_seed.seed = 99;
        universe::Simulation diverged{different_seed};

        bool settlement_diverged = lhs.state().settlements.size() != diverged.state().settlements.size();
        if (!settlement_diverged) {
            for (std::size_t i = 0; i < lhs.state().settlements.size(); ++i) {
                const auto& a = lhs.state().settlements[i];
                const auto& b = diverged.state().settlements[i];
                if (a.region_id != b.region_id || a.access_share_points != b.access_share_points) {
                    settlement_diverged = true;
                    break;
                }
            }
        }
        require(settlement_diverged, "different seeds should diverge in settlement generation");
    }

    {
        auto multi_settlement = config;
        multi_settlement.initial_population = 360;
        multi_settlement.initial_region_count = 1;
        multi_settlement.base_region_area_units = 90;

        universe::Simulation simulation{multi_settlement};
        verify_settlement_invariants(simulation.state());

        std::unordered_map<universe::RegionId, std::uint64_t> settlement_counts;
        for (const auto& settlement : simulation.state().settlements) {
            ++settlement_counts[settlement.region_id];
        }

        bool has_multiple = false;
        for (const auto& [region_id, count] : settlement_counts) {
            (void)region_id;
            if (count > 1U) {
                has_multiple = true;
            }
        }
        require(has_multiple, "a region should support multiple settlements");
    }

    {
        auto founder = config;
        founder.initial_population = 300;
        founder.initial_region_count = 1;
        founder.base_region_area_units = 60;
        founder.initial_household_food_units = 10;
        founder.food_production_scale_percent = 90;

        universe::Simulation a{founder};
        universe::Simulation b{founder};
        const auto initial_settlement_count = a.state().settlements.size();
        a.run_years(120);
        b.run_years(120);
        require_world_state_equal(a.state(), b.state(), "settlement founding/splitting must be deterministic");
        require(
            a.state().settlements.size() >= initial_settlement_count,
            "settlement restructuring must not remove canonical settlement identities");
    }

    {
        auto resource_rich = config;
        resource_rich.initial_population = 220;
        resource_rich.initial_region_count = 1;
        resource_rich.base_region_area_units = 180;

        auto constrained = resource_rich;
        constrained.base_region_area_units = 45;

        universe::ui::SimulationController rich_controller{resource_rich};
        universe::ui::SimulationController constrained_controller{constrained};
        rich_controller.advance_years(15);
        constrained_controller.advance_years(15);

        const auto rich_settlements = rich_controller.settlement_summaries();
        const auto constrained_settlements = constrained_controller.settlement_summaries();

        std::int64_t rich_capacity = 0;
        std::int64_t constrained_capacity = 0;
        std::uint32_t rich_pressure = 0;
        std::uint32_t constrained_pressure = 0;
        for (const auto& settlement : rich_settlements) {
            if (!settlement.active) {
                continue;
            }
            rich_capacity += settlement.accessible_food_capacity_per_day;
            rich_pressure = std::max(rich_pressure, settlement.pressure_per_10k);
        }
        for (const auto& settlement : constrained_settlements) {
            if (!settlement.active) {
                continue;
            }
            constrained_capacity += settlement.accessible_food_capacity_per_day;
            constrained_pressure = std::max(constrained_pressure, settlement.pressure_per_10k);
        }

        require(rich_capacity > constrained_capacity, "resource-rich settlement profile should have higher local capacity");
        require(constrained_pressure > 0 || rich_pressure > 0, "settlement pressure should be measurable in at least one profile");
    }

    {
        universe::Simulation labour_order{config};
        labour_order.run_years(5);
        const auto& state = labour_order.state();

        const auto people_forward = total_labour_capacity_by_people(state, config.productive_effort_scale_percent, false);
        const auto people_reverse = total_labour_capacity_by_people(state, config.productive_effort_scale_percent, true);
        require(people_forward == people_reverse, "person traversal order independence");

        const auto households_forward = total_labour_capacity_by_households(state, config.productive_effort_scale_percent, false);
        const auto households_reverse = total_labour_capacity_by_households(state, config.productive_effort_scale_percent, true);
        require(households_forward == households_reverse, "household traversal order independence");

        const auto settlements_forward = total_labour_capacity_by_settlements(state, config.productive_effort_scale_percent, false);
        const auto settlements_reverse = total_labour_capacity_by_settlements(state, config.productive_effort_scale_percent, true);
        require(settlements_forward == settlements_reverse, "settlement traversal order independence");
        require(people_forward == households_forward, "household aggregation must equal person labour totals");
        require(people_forward == settlements_forward, "settlement aggregation must equal person labour totals");
    }

    {
        auto zero_worker_config = config;
        zero_worker_config.initial_population = 1;
        zero_worker_config.initial_household_food_units = 0;
        zero_worker_config.food_production_scale_percent = 200;
        zero_worker_config.productive_effort_scale_percent = 0;

        universe::Simulation zero_worker_run{zero_worker_config};
        std::unordered_map<universe::HouseholdId, std::int64_t> produced_before;
        for (const auto& household : zero_worker_run.state().households) {
            produced_before.emplace(household.id, household.resources.food_produced_total);
        }

        zero_worker_run.advance(universe::days(30));
        for (const auto& household : zero_worker_run.state().households) {
            const auto before_it = produced_before.find(household.id);
            require(before_it != produced_before.end(), "zero-capable-worker household setup mismatch");
            require(
                household.resources.food_produced_total == before_it->second,
                "zero-capable-worker household = zero productive effort");
        }
    }

    {
        auto labour_poor = config;
        labour_poor.initial_household_food_units = 300;
        labour_poor.food_production_scale_percent = 100;
        labour_poor.productive_effort_scale_percent = 35;

        auto labour_rich = labour_poor;
        labour_rich.initial_household_food_units = 20;
        labour_rich.productive_effort_scale_percent = 170;

        universe::Simulation labour_poor_run{labour_poor};
        universe::Simulation labour_rich_run{labour_rich};
        labour_poor_run.run_years(2);
        labour_rich_run.run_years(2);

        require(
            labour_rich_run.state().total_food_produced > 0,
            "labour-rich but resource-poor household profile should still produce food");
        const bool profile_diverged =
            labour_poor_run.state().total_food_stored != labour_rich_run.state().total_food_stored ||
            labour_poor_run.state().total_unmet_food != labour_rich_run.state().total_unmet_food ||
            labour_poor_run.state().total_food_produced != labour_rich_run.state().total_food_produced;
        require(profile_diverged, "distinct labour/resource profiles should diverge in outcomes");
    }

    {
        auto pressure = config;
        pressure.initial_household_food_units = 0;
        pressure.food_production_scale_percent = 55;
        pressure.productive_effort_scale_percent = 75;

        universe::Simulation feedback{pressure};
        feedback.run_years(10);
        const auto before_relief_health = feedback.state().population_under_food_stress;
        const auto before_relief_capacity = total_labour_capacity_by_people(
            feedback.state(),
            pressure.productive_effort_scale_percent,
            false);

        auto relieved = pressure;
        relieved.food_production_scale_percent = 180;
        universe::Simulation recovery{relieved};
        recovery.run_years(10);
        const auto relieved_health = recovery.state().population_under_food_stress;
        const auto relieved_capacity = total_labour_capacity_by_people(
            recovery.state(),
            relieved.productive_effort_scale_percent,
            false);

        require(
            before_relief_health >= relieved_health,
            "food shortage -> health loss -> labour reduction feedback must be observable");
        require(
            relieved_capacity >= before_relief_capacity,
            "recovery after pressure is removed should restore labour capacity");

        const auto unexplained_production =
            feedback.state().total_food_stored + feedback.state().total_food_consumed - feedback.state().total_food_produced;
        const auto expected_initial = feedback.state().households.empty()
            ? 0
            : static_cast<std::int64_t>(feedback.state().households.size()) * pressure.initial_household_food_units;
        require(
            unexplained_production == expected_initial,
            "no unexplained resource production");
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
        require(
            total_labour_capacity_by_people(chunked_days.state(), config.productive_effort_scale_percent, false) ==
                total_labour_capacity_by_people(contiguous_days.state(), config.productive_effort_scale_percent, false),
            "equivalent time chunking must preserve derived labour capacity");
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
            total_labour_capacity_by_people(continuous.state(), config.productive_effort_scale_percent, false) ==
                total_labour_capacity_by_people(resumed.state(), config.productive_effort_scale_percent, false),
            "save/load continuation must preserve derived labour capacity");
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
        auto durability_config = config;
        durability_config.initial_population = 280;
        durability_config.initial_region_count = 2;
        durability_config.base_region_area_units = 160;
        durability_config.food_production_scale_percent = 130;
        durability_config.wood_resource_scale_percent = 220;
        durability_config.stone_resource_scale_percent = 220;
        durability_config.material_crafting_scale_percent = 220;

        universe::Simulation durability_run_a{durability_config};
        universe::Simulation durability_run_b{durability_config};
        durability_run_a.run_years(30);
        durability_run_b.run_years(30);

        require_world_state_equal(
            durability_run_a.state(),
            durability_run_b.state(),
            "tool durability loop must remain deterministic");

        require(
            durability_run_a.state().total_basic_tool_produced > 0,
            "durability scenario should produce tools");
        require(
            durability_run_a.state().total_basic_tool_broken > 0,
            "durability scenario should consume and break tools over time");
        require(
            durability_run_a.state().total_basic_tool_produced >= durability_run_a.state().total_basic_tool_broken,
            "broken tools should never exceed total produced tools");
        require(
            durability_run_a.state().average_tool_coverage_per_10k <= 10'000U,
            "tool coverage must remain bounded");
    }

    {
        auto low_craft = config;
        low_craft.initial_population = 260;
        low_craft.initial_region_count = 2;
        low_craft.base_region_area_units = 150;
        low_craft.wood_resource_scale_percent = 200;
        low_craft.stone_resource_scale_percent = 200;
        low_craft.material_crafting_scale_percent = 0;

        auto high_craft = low_craft;
        high_craft.material_crafting_scale_percent = 260;

        universe::Simulation low_run{low_craft};
        universe::Simulation high_run{high_craft};
        low_run.run_years(25);
        high_run.run_years(25);

        require(
            high_run.state().total_basic_tool_produced >= low_run.state().total_basic_tool_produced,
            "higher craft scale should not reduce cumulative tool production");
        require(
            high_run.state().total_basic_tool_broken >= low_run.state().total_basic_tool_broken,
            "higher craft scale should not reduce cumulative tool wear/breakage");
        require(
            high_run.state().average_tool_coverage_per_10k >= low_run.state().average_tool_coverage_per_10k,
            "higher craft scale should not reduce average tool coverage");
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
        bad_version[4] = std::byte{0x09};
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
        constexpr std::size_t payload_people_count_offset = 582U;
        constexpr std::size_t payload_first_person_offset = 590U;
        constexpr std::size_t payload_person_record_size = 41U;
        constexpr std::size_t person_sex_offset_in_record = 16U;
        constexpr std::size_t person_household_offset_in_record = 18U;
        constexpr std::size_t person_occupation_offset_in_record = 40U;

        auto invalid_enum = clone_bytes(valid);
        invalid_enum[header_size + payload_first_person_offset + person_sex_offset_in_record] = std::byte{2};
        refresh_snapshot_payload_hash(invalid_enum);
        require_snapshot_load_fails(
            invalid_enum,
            universe::SnapshotErrorCode::InvalidEnum,
            "invalid enum values must be rejected");

        auto invalid_occupation_enum = clone_bytes(valid);
        invalid_occupation_enum[header_size + payload_first_person_offset + person_occupation_offset_in_record] = std::byte{9};
        refresh_snapshot_payload_hash(invalid_occupation_enum);
        require_snapshot_load_fails(
            invalid_occupation_enum,
            universe::SnapshotErrorCode::InvalidEnum,
            "invalid occupation enum values must be rejected");

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
    lifecycle.run_years(60);
    std::uint64_t born_and_now_adult = 0;
    for (const auto& person : lifecycle.state().people) {
        if (person.birth_year >= 0 && age_years(lifecycle.state(), person) >= 18U) {
            ++born_and_now_adult;
        }
    }
    require(born_and_now_adult > 0, "children born during simulation should age into adults over decades");

    {
        const std::array<std::uint64_t, 5> baseline_seeds{11U, 22U, 33U, 44U, 55U};
        std::uint64_t survivors_500 = 0;
        std::uint64_t extinctions_500 = 0;
        for (const auto seed : baseline_seeds) {
            universe::Simulation baseline{universe::SimulationConfig{.seed = seed, .initial_population = 200}};
            baseline.run_years(500);
            verify_demographic_invariants(baseline.state());
            verify_event_log_invariants(baseline.state());
            verify_resource_invariants(baseline.state(), 80);
            verify_person_health_invariants(baseline.state());
            if (baseline.state().population > 0) {
                ++survivors_500;
            } else {
                ++extinctions_500;
            }
        }

        require(survivors_500 >= 3U, "baseline 500-year seeds should usually remain viable");
        require(
            survivors_500 + extinctions_500 == baseline_seeds.size(),
            "baseline 500-year seed accounting must remain consistent");
    }

    {
        universe::Simulation severe_famine{universe::SimulationConfig{
            .seed = 22,
            .initial_population = 200,
            .initial_household_food_units = 20,
            .food_production_scale_percent = 35,
        }};
        severe_famine.run_years(250);
        require(
            severe_famine.state().population <= 20,
            "severe famine regime should produce major collapse or extinction");
    }

    {
        universe::Simulation recovery{universe::SimulationConfig{.seed = 22, .initial_population = 200}};
        std::uint64_t min_population_first_40 = std::numeric_limits<std::uint64_t>::max();
        for (std::uint64_t year = 0; year < 40; ++year) {
            recovery.step_year();
            min_population_first_40 = std::min(min_population_first_40, recovery.state().population);
        }
        for (std::uint64_t year = 40; year < 120; ++year) {
            recovery.step_year();
        }

        require(recovery.state().population > min_population_first_40, "baseline recovery scenario should rebound after early stress");
    }

    {
        universe::Simulation carrying{universe::SimulationConfig{.seed = 22, .initial_population = 200}};
        std::uint64_t peak_population = carrying.state().population;
        for (std::uint64_t year = 0; year < 500; ++year) {
            carrying.step_year();
            peak_population = std::max(peak_population, carrying.state().population);
        }

        require(peak_population > 200U, "carrying-capacity regime should allow growth above initial population");
        require(peak_population < 2'000U, "carrying-capacity regime should avoid uncontrolled runaway growth");
    }

    {
        universe::Simulation labour_constrained{universe::SimulationConfig{
            .seed = 22,
            .initial_population = 200,
            .food_production_scale_percent = 160,
            .productive_effort_scale_percent = 55,
        }};
        labour_constrained.run_years(250);
        require(
            labour_constrained.state().population == 0,
            "labour-constrained scenario should be able to collapse even with abundant resources");
    }

    {
        universe::Simulation baseline{universe::SimulationConfig{.seed = 22, .initial_population = 200}};
        baseline.run_years(250);

        universe::Simulation resource_constrained{universe::SimulationConfig{
            .seed = 22,
            .initial_population = 200,
            .food_production_scale_percent = 55,
        }};
        resource_constrained.run_years(250);

        require(
            resource_constrained.state().population < baseline.state().population,
            "resource-constrained scenario should underperform baseline population viability");
    }

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
    require(extinct_runs <= 20, "seed sweep extinction counter must remain bounded");

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
    require(
        long_run_survived + long_run_extinct == long_run_seeds.size(),
        "long-horizon seed accounting must remain consistent");

    std::cout << "All UniverseCore tests passed.\n";
    return 0;
}
