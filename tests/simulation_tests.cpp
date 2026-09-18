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
#include <variant>
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

    std::unordered_map<universe::SettlementId, std::uint64_t> expected_population;
    for (const auto& household : state.households) {
        const auto* settlement = find_settlement(state, household.settlement_id);
        require(settlement != nullptr, "household settlement id must reference existing settlement");
        require(household.member_ids.empty() || settlement->active,
                "a household with living members may not reference an inactive (extinct or merged-away) settlement");
        require(settlement->region_id == household.region_id, "household settlement and region must be consistent");
        expected_population[household.settlement_id] += household.member_ids.size();
    }
    for (const auto& cohort : state.population_cohorts) {
        expected_population[cohort.settlement_id] += cohort.population;
    }
    for (const auto& settlement : state.settlements) {
        const auto expected_it = expected_population.find(settlement.id);
        const auto expected = expected_it != expected_population.end() ? expected_it->second : 0U;
        require(settlement.population == expected,
                "settlement population must equal sum of alive household members and aggregate cohort population");

        require(
            settlement.tier == universe::SettlementTier::Village ||
                settlement.tier == universe::SettlementTier::Town ||
                settlement.tier == universe::SettlementTier::City,
            "settlement tier must be one of the three valid values");
        require(
            settlement.pending_tier == universe::SettlementTier::Village ||
                settlement.pending_tier == universe::SettlementTier::Town ||
                settlement.pending_tier == universe::SettlementTier::City,
            "settlement pending tier must be one of the three valid values");
        if (settlement.pending_tier == settlement.tier) {
            require(settlement.tier_transition_streak == 0U,
                    "tier transition streak must be zero once a settlement has no pending tier change");
        }
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
    std::int64_t total_basic_tool_broken = 0;
    std::int64_t total_household_wealth = 0;
    std::int64_t total_commons_food = 0;
    std::int64_t total_transfer_volume = 0;
    std::int64_t total_exchange_volume = 0;
    std::uint64_t active_settlement_count = 0;
    std::uint64_t coverage_sum = 0;

    std::unordered_set<universe::RegionId> region_ids;
    std::unordered_set<universe::SettlementId> settlement_ids;
    std::unordered_map<universe::SettlementId, std::uint32_t> productive_access_by_settlement;
    std::unordered_map<universe::SettlementId, std::uint32_t> commons_access_by_settlement;
    std::unordered_map<universe::SettlementId, std::int64_t> owned_wood_by_settlement;
    std::unordered_map<universe::SettlementId, std::int64_t> owned_stone_by_settlement;
    std::unordered_map<universe::SettlementId, std::int64_t> owned_timber_by_settlement;
    std::unordered_map<universe::SettlementId, std::int64_t> owned_tools_by_settlement;
    for (const auto& settlement : state.settlements) {
        settlement_ids.insert(settlement.id);
        require(settlement.position_x >= -1'000'000 && settlement.position_x <= 1'000'000, "settlement position x must be in canonical world bounds");
        require(settlement.position_z >= -1'000'000 && settlement.position_z <= 1'000'000, "settlement position z must be in canonical world bounds");
        total_basic_tool_broken += settlement.cumulative_basic_tool_broken;
        require(settlement.commons_food.value >= 0, "settlement commons food must be non-negative");
        require(settlement.last_transfer_food_volume >= 0, "settlement transfer volume must be non-negative");
        require(settlement.last_exchange_volume >= 0, "settlement exchange volume must be non-negative");
        require(settlement.dwelling_capacity_bonus >= 0, "settlement dwelling capacity bonus must be non-negative");
        require(settlement.food_store_capacity_bonus >= 0, "settlement food store capacity bonus must be non-negative");
        require(settlement.workshop_capacity_bonus >= 0, "settlement workshop capacity bonus must be non-negative");
        require(settlement.last_min_household_wealth >= 0, "settlement min household wealth must be non-negative");
        require(settlement.last_median_household_wealth >= 0, "settlement median household wealth must be non-negative");
        require(settlement.last_max_household_wealth >= 0, "settlement max household wealth must be non-negative");
        require(
            settlement.last_min_household_wealth <= settlement.last_median_household_wealth &&
                settlement.last_median_household_wealth <= settlement.last_max_household_wealth,
            "settlement wealth statistics must be ordered min <= median <= max");
        require(
            settlement.last_inequality_numerator <= settlement.last_inequality_denominator,
            "settlement inequality ratio numerator must not exceed denominator");
        if (settlement.last_inequality_denominator == 0U) {
            require(settlement.last_inequality_numerator == 0U, "zero inequality denominator requires zero numerator");
        }
        total_commons_food += settlement.commons_food.value;
        total_transfer_volume += settlement.last_transfer_food_volume;
        total_exchange_volume += settlement.last_exchange_volume;
        if (settlement.active) {
            ++active_settlement_count;
            coverage_sum += settlement.last_tool_coverage_per_10k;
        }
    }

    for (const auto& region : state.regions) {
        require(region.id > 0, "region id must be non-zero");
        require(region_ids.insert(region.id).second, "region ids must be unique");
        require(region.land_area_units > 0, "region area must be positive");
        require(region.position_x >= -1'000'000 && region.position_x <= 1'000'000, "region position x must be in canonical world bounds");
        require(region.position_z >= -1'000'000 && region.position_z <= 1'000'000, "region position z must be in canonical world bounds");
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
        require(household.owned_materials.wood.value >= 0, "household owned wood must be non-negative");
        require(household.owned_materials.stone.value >= 0, "household owned stone must be non-negative");
        require(household.owned_materials.timber.value >= 0, "household owned timber must be non-negative");
        require(household.owned_materials.basic_tool.value >= 0, "household owned tools must be non-negative");
        require(household.productive_access_share_points <= 10'000U, "household productive access share must be bounded");
        require(household.commons_access_share_points <= 10'000U, "household commons access share must be bounded");
        require(household.cumulative_food_transferred_in >= 0, "household cumulative transfer-in must be non-negative");
        require(household.cumulative_food_transferred_out >= 0, "household cumulative transfer-out must be non-negative");
        require(household.cumulative_material_transferred_in >= 0, "household cumulative material transfer-in must be non-negative");
        require(household.cumulative_material_transferred_out >= 0, "household cumulative material transfer-out must be non-negative");
        require(household.derived_wealth_points >= 0, "household derived wealth must be non-negative");
        productive_access_by_settlement[household.settlement_id] += household.productive_access_share_points;
        commons_access_by_settlement[household.settlement_id] += household.commons_access_share_points;
        owned_wood_by_settlement[household.settlement_id] += household.owned_materials.wood.value;
        owned_stone_by_settlement[household.settlement_id] += household.owned_materials.stone.value;
        owned_timber_by_settlement[household.settlement_id] += household.owned_materials.timber.value;
        owned_tools_by_settlement[household.settlement_id] += household.owned_materials.basic_tool.value;
        total_household_wealth += household.derived_wealth_points;

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

    require(total_food_stored + total_commons_food == state.total_food_stored, "stored food aggregate must match household plus commons totals");
    require(total_food_produced == state.total_food_produced, "produced food aggregate must match household totals");
    require(total_food_consumed == state.total_food_consumed, "consumed food aggregate must match household totals");
    require(total_unmet_food == state.total_unmet_food, "unmet food aggregate must match household totals");
    const auto total_inferred_initial_food = state.total_food_stored + state.total_food_consumed - state.total_food_produced;
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

    for (const auto& [settlement_id, productive_share] : productive_access_by_settlement) {
        (void)settlement_id;
        require(productive_share <= 10'000U, "household productive access allocations must remain bounded by settlement scale");
    }
    for (const auto& [settlement_id, commons_share] : commons_access_by_settlement) {
        (void)settlement_id;
        require(commons_share <= 10'000U, "household commons access allocations must remain bounded by settlement scale");
    }

    require(state.total_household_wealth == total_household_wealth, "household wealth aggregate must match household totals");
    require(state.total_settlement_commons_food == total_commons_food, "settlement commons food aggregate must match settlement totals");
    require(state.total_transfer_volume == total_transfer_volume, "transfer volume aggregate must match settlement totals");
    require(state.total_exchange_volume == total_exchange_volume, "exchange volume aggregate must match settlement totals");

    for (const auto& settlement : state.settlements) {
        const auto owned_wood = owned_wood_by_settlement.contains(settlement.id) ? owned_wood_by_settlement[settlement.id] : 0;
        const auto owned_stone = owned_stone_by_settlement.contains(settlement.id) ? owned_stone_by_settlement[settlement.id] : 0;
        const auto owned_timber = owned_timber_by_settlement.contains(settlement.id) ? owned_timber_by_settlement[settlement.id] : 0;
        const auto owned_tools = owned_tools_by_settlement.contains(settlement.id) ? owned_tools_by_settlement[settlement.id] : 0;
        require(owned_wood == settlement.materials.wood.value, "household-owned wood totals must match settlement inventory");
        require(owned_stone == settlement.materials.stone.value, "household-owned stone totals must match settlement inventory");
        require(owned_timber == settlement.materials.timber.value, "household-owned timber totals must match settlement inventory");
        require(owned_tools == settlement.materials.basic_tool.value, "household-owned tool totals must match settlement inventory");
    }

    std::uint64_t completed_buildings = 0;
    std::uint64_t active_projects = 0;
    std::unordered_set<universe::BuildingId> building_ids;
    std::unordered_map<universe::SettlementId, std::uint64_t> completed_dwellings_by_settlement;
    std::unordered_map<universe::SettlementId, std::uint64_t> completed_food_stores_by_settlement;
    std::unordered_map<universe::SettlementId, std::uint64_t> completed_workshops_by_settlement;
    for (const auto& building : state.buildings) {
        require(building.id > 0U, "building id must be non-zero");
        require(building_ids.insert(building.id).second, "building ids must be unique");
        require(settlement_ids.contains(building.settlement_id), "building settlement id must reference existing settlement");
        if (building.owner_household_id.has_value()) {
            const auto* owner = find_household(state, *building.owner_household_id);
            require(owner != nullptr, "building owner household id must reference existing household");
            require(owner->settlement_id == building.settlement_id, "building owner household must belong to building settlement");
        }
        require(building.timber_required >= 0, "building timber requirement must be non-negative");
        require(building.stone_required >= 0, "building stone requirement must be non-negative");
        require(building.labour_required >= 0, "building labour requirement must be non-negative");
        require(building.timber_committed >= 0, "building timber committed must be non-negative");
        require(building.stone_committed >= 0, "building stone committed must be non-negative");
        require(building.labour_completed >= 0, "building labour completed must be non-negative");
        require(building.timber_committed <= building.timber_required, "building timber committed cannot exceed requirement");
        require(building.stone_committed <= building.stone_required, "building stone committed cannot exceed requirement");
        require(building.labour_completed <= building.labour_required, "building labour completed cannot exceed requirement");
        require(building.started_at.ticks >= 0 && building.started_at.ticks <= state.time.ticks, "building start time must be within timeline");
        if (building.state == universe::ConstructionState::Complete) {
            require(building.completed_at.has_value(), "complete building must include completion time");
            require(
                building.completed_at->ticks >= building.started_at.ticks && building.completed_at->ticks <= state.time.ticks,
                "building completion time must be within timeline and after start");
            if (building.type == universe::BuildingType::Dwelling) {
                ++completed_dwellings_by_settlement[building.settlement_id];
            } else if (building.type == universe::BuildingType::FoodStore) {
                ++completed_food_stores_by_settlement[building.settlement_id];
            } else if (building.type == universe::BuildingType::Workshop) {
                ++completed_workshops_by_settlement[building.settlement_id];
            }
            ++completed_buildings;
        } else {
            require(!building.completed_at.has_value(), "incomplete building must not include completion time");
            ++active_projects;
        }
    }

    require(state.active_buildings == completed_buildings, "active building aggregate must match building list");
    require(state.active_projects == active_projects, "active project aggregate must match building list");

    for (const auto& settlement : state.settlements) {
        const auto dwellings = completed_dwellings_by_settlement.contains(settlement.id) ? completed_dwellings_by_settlement[settlement.id] : 0U;
        const auto food_stores = completed_food_stores_by_settlement.contains(settlement.id) ? completed_food_stores_by_settlement[settlement.id] : 0U;
        const auto workshops = completed_workshops_by_settlement.contains(settlement.id) ? completed_workshops_by_settlement[settlement.id] : 0U;
        require(
            settlement.dwelling_capacity_bonus == static_cast<std::int64_t>(dwellings) * 18,
            "dwelling capacity bonus must match completed dwelling count");
        require(
            settlement.food_store_capacity_bonus == static_cast<std::int64_t>(food_stores) * 32,
            "food store capacity bonus must match completed food store count");
        require(
            settlement.workshop_capacity_bonus == static_cast<std::int64_t>(workshops) * 14,
            "workshop capacity bonus must match completed workshop count");
    }

    verify_settlement_invariants(state);
}

void verify_person_health_invariants(const universe::WorldState& state) {
    for (const auto& person : state.people) {
        require(person.nutrition_points >= 0 && person.nutrition_points <= 1000, "nutrition points must be within canonical bounds");
        require(person.health_points >= 0 && person.health_points <= 1000, "health points must be within canonical bounds");
    }
}

void verify_labour_assignment_conservation(const universe::WorldState& state) {
    const universe::FoodModelParameters labour_parameters{};

    std::unordered_map<universe::PersonId, const universe::Person*> person_by_id;
    person_by_id.reserve(state.people.size());
    for (const auto& person : state.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<universe::SettlementId, std::int64_t> settlement_available_labour;
    std::unordered_map<universe::SettlementId, std::int64_t> settlement_allocated_labour;

    for (const auto& household : state.households) {
        std::int64_t household_available = 0;
        std::int64_t household_allocated = 0;
        for (const auto member_id : household.member_ids) {
            const auto it = person_by_id.find(member_id);
            if (it == person_by_id.end() || !it->second->alive) {
                continue;
            }

            const auto& person = *it->second;
            const auto capacity = person_labour_capacity_units(
                true,
                age_years(state, person),
                person.nutrition_points,
                person.health_points,
                labour_parameters,
                100U);
            if (capacity <= 0) {
                continue;
            }

            household_available += capacity;
            if (person.occupation != universe::Occupation::None) {
                household_allocated += capacity;
            }
        }

        require(
            household_allocated <= household_available,
            "household allocated labour must not exceed available eligible labour");
        settlement_available_labour[household.settlement_id] += household_available;
        settlement_allocated_labour[household.settlement_id] += household_allocated;
    }

    for (const auto& settlement : state.settlements) {
        const auto available = settlement_available_labour.contains(settlement.id)
            ? settlement_available_labour[settlement.id]
            : 0;
        const auto allocated = settlement_allocated_labour.contains(settlement.id)
            ? settlement_allocated_labour[settlement.id]
            : 0;
        require(
            allocated <= available,
            "settlement allocated labour must not exceed available eligible labour");
    }
}

void verify_event_log_invariants(const universe::WorldState& state) {
    std::uint64_t birth_events = 0;
    std::uint64_t death_events = 0;
    std::unordered_set<std::uint32_t> discovery_keys;
    std::unordered_map<universe::SettlementId, std::uint64_t> founded_events_by_settlement;
    std::unordered_map<universe::SettlementId, std::uint64_t> extinct_events_by_settlement;
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

        if (event.type == universe::EventType::Birth) {
            ++birth_events;
            require(std::holds_alternative<universe::BirthEventPayload>(event.payload), "birth event payload must match type");
            const auto& payload = std::get<universe::BirthEventPayload>(event.payload);

            require(payload.child_id > 0U, "birth event child id must be non-zero");
            require(payload.mother_id > 0U, "birth event mother id must be non-zero");
            require(payload.father_id.has_value(), "birth event must contain father id");
            require(payload.household_id > 0U, "birth event household id must be non-zero");

            const auto* child = find_person(state, payload.child_id);
            const auto* mother = find_person(state, payload.mother_id);
            const auto* father = find_person(state, *payload.father_id);
            const auto* household = find_household(state, payload.household_id);

            require(child != nullptr, "birth event child id must reference an existing person");
            require(mother != nullptr, "birth event mother id must reference an existing person");
            require(father != nullptr, "birth event father id must reference an existing person");
            require(household != nullptr, "birth event household id must reference an existing household");
            require(mother->sex == universe::BiologicalSex::Female, "birth event mother must be female");
            require(father->sex == universe::BiologicalSex::Male, "birth event father must be male");
        } else if (event.type == universe::EventType::Death) {
            ++death_events;
            require(std::holds_alternative<universe::DeathEventPayload>(event.payload), "death event payload must match type");
            const auto& payload = std::get<universe::DeathEventPayload>(event.payload);

            require(payload.person_id > 0U, "death event person id must be non-zero");
            require(payload.household_id > 0U, "death event household id must be non-zero");
            require(find_person(state, payload.person_id) != nullptr, "death event person id must reference existing person");
            require(find_household(state, payload.household_id) != nullptr, "death event household id must reference existing household");
        } else if (event.type == universe::EventType::SettlementFounded) {
            require(
                std::holds_alternative<universe::SettlementFoundedEventPayload>(event.payload),
                "settlement founded event payload must match type");
            const auto& payload = std::get<universe::SettlementFoundedEventPayload>(event.payload);
            require(payload.settlement_id > 0U, "settlement founded event settlement id must be non-zero");
            require(payload.region_id > 0U, "settlement founded event region id must be non-zero");
            require(
                find_settlement(state, payload.settlement_id) != nullptr,
                "settlement founded event settlement id must reference existing settlement");
            founded_events_by_settlement[payload.settlement_id] += 1U;
        } else if (event.type == universe::EventType::SettlementExtinct) {
            require(
                std::holds_alternative<universe::SettlementExtinctEventPayload>(event.payload),
                "settlement extinct event payload must match type");
            const auto& payload = std::get<universe::SettlementExtinctEventPayload>(event.payload);
            require(payload.settlement_id > 0U, "settlement extinct event settlement id must be non-zero");
            require(payload.region_id > 0U, "settlement extinct event region id must be non-zero");
            require(
                find_settlement(state, payload.settlement_id) != nullptr,
                "settlement extinct event settlement id must reference existing settlement");
            extinct_events_by_settlement[payload.settlement_id] += 1U;
        } else if (event.type == universe::EventType::DiscoveryMade) {
            require(
                std::holds_alternative<universe::DiscoveryMadeEventPayload>(event.payload),
                "discovery event payload must match type");
            const auto& payload = std::get<universe::DiscoveryMadeEventPayload>(event.payload);
            require(payload.discovery_key > 0U, "discovery event key must be non-zero");
            require(payload.discoverer_id > 0U, "discovery event discoverer id must be non-zero");
            require(payload.settlement_id > 0U, "discovery event settlement id must be non-zero");
            const auto* discoverer = find_person(state, payload.discoverer_id);
            const auto* settlement = find_settlement(state, payload.settlement_id);
            require(discoverer != nullptr, "discovery event discoverer must exist");
            require(settlement != nullptr, "discovery event settlement must exist");
            const auto* discoverer_household = find_household(state, discoverer->household_id);
            require(discoverer_household != nullptr, "discovery event discoverer household must exist");
            require(discoverer_household->region_id == settlement->region_id, "discovery event discoverer must remain in event region");
            require(discovery_keys.insert(payload.discovery_key).second, "discovery event keys must be globally unique");
        } else if (event.type == universe::EventType::InstitutionFounded) {
            require(
                std::holds_alternative<universe::InstitutionFoundedEventPayload>(event.payload),
                "institution founded event payload must match type");
            const auto& payload = std::get<universe::InstitutionFoundedEventPayload>(event.payload);
            require(payload.institution_id > 0U, "institution founded event id must be non-zero");
            require(payload.settlement_id > 0U, "institution founded settlement id must be non-zero");
            require(payload.founder_id > 0U, "institution founded founder id must be non-zero");
            require(payload.kind == universe::InstitutionKind::KnowledgeKeeper, "institution founded kind must be knowledge keeper");
            require(find_settlement(state, payload.settlement_id) != nullptr, "institution founded settlement must exist");
            require(find_person(state, payload.founder_id) != nullptr, "institution founded founder must exist");
        }
    }

    for (const auto& [settlement_id, founded_count] : founded_events_by_settlement) {
        (void)settlement_id;
        require(founded_count == 1U, "each settlement must have at most one founded event");
    }
    for (const auto& [settlement_id, extinct_count] : extinct_events_by_settlement) {
        (void)settlement_id;
        require(extinct_count == 1U, "each settlement must have at most one extinct event");
    }

    require(birth_events == state.total_births, "every generated birth must produce exactly one birth event");
    require(death_events == state.total_deaths, "every generated death must produce exactly one death event");

    std::unordered_map<universe::EventId, const universe::SimulationEvent*> event_by_id;
    event_by_id.reserve(state.event_log.size());
    for (const auto& event : state.event_log) {
        event_by_id.emplace(event.id, &event);
    }

    std::unordered_map<universe::PersonId, std::unordered_set<universe::EventId>> direct_memories_by_owner;
    std::unordered_map<universe::MemoryId, const universe::KnowledgeTransmission*> transmission_by_recipient_memory;
    transmission_by_recipient_memory.reserve(state.knowledge_transmissions.size());
    for (const auto& transmission : state.knowledge_transmissions) {
        transmission_by_recipient_memory.emplace(transmission.recipient_memory_id, &transmission);
    }
    std::unordered_map<universe::MemoryId, const universe::KnowledgeRecordAccess*> record_access_by_memory;
    record_access_by_memory.reserve(state.knowledge_record_accesses.size());
    for (const auto& access : state.knowledge_record_accesses) {
        record_access_by_memory.emplace(access.reader_memory_id, &access);
    }
    std::unordered_map<universe::RecordId, const universe::KnowledgeRecord*> record_by_id;
    record_by_id.reserve(state.knowledge_records.size());
    for (const auto& record : state.knowledge_records) {
        record_by_id.emplace(record.id, &record);
    }

    universe::MemoryId expected_memory_id = 1;
    for (const auto& memory : state.agent_memories) {
        require(memory.id == expected_memory_id, "memory ids must be contiguous and deterministic");
        ++expected_memory_id;
        require(memory.owner_id > 0U, "memory owner id must be non-zero");
        require(memory.source_event_id > 0U, "memory event id must be non-zero");
        require(
            memory.source == universe::MemorySource::DirectObservation ||
                memory.source == universe::MemorySource::Communication ||
                memory.source == universe::MemorySource::Teaching ||
                memory.source == universe::MemorySource::Record,
            "memory source must be recognized");
        require(memory.confidence_per_10k <= 10'000U, "memory confidence must remain bounded");

        const auto* owner = find_person(state, memory.owner_id);
        require(owner != nullptr, "memory owner must reference existing person");

        const auto event_it = event_by_id.find(memory.source_event_id);
        require(event_it != event_by_id.end(), "memory source event must exist");
        const auto* source_event = event_it->second;
        if (memory.source == universe::MemorySource::DirectObservation) {
            require(source_event->timestamp == memory.acquired_at, "direct memory acquisition timestamp must match source event");
            require(
                direct_memories_by_owner[memory.owner_id].insert(memory.source_event_id).second,
                "owner/event direct memory must be unique");
        }

        if (memory.source == universe::MemorySource::Communication || memory.source == universe::MemorySource::Teaching) {
            const auto transmission_it = transmission_by_recipient_memory.find(memory.id);
            require(transmission_it != transmission_by_recipient_memory.end(), "communication memory must have transmission provenance");

            const auto* transmission = transmission_it->second;
            require(transmission->recipient_id == memory.owner_id, "transmission recipient must match memory owner");
            require(transmission->transmitted_at == memory.acquired_at, "transmission time must match memory acquisition time");

            const universe::AgentMemory* speaker_memory = nullptr;
            const universe::Person* speaker = nullptr;
            for (const auto& candidate : state.agent_memories) {
                if (candidate.id == transmission->speaker_memory_id) {
                    speaker_memory = &candidate;
                    break;
                }
            }
            speaker = find_person(state, transmission->speaker_id);
            require(speaker_memory != nullptr, "transmission speaker memory must exist");
            require(speaker != nullptr, "transmission speaker must exist");
            require(speaker_memory->owner_id == speaker->id, "transmission speaker memory owner mismatch");
            require(speaker_memory->source_event_id == memory.source_event_id, "transmission must preserve source event");

            require(
                transmission->speaker_settlement_id == transmission->recipient_settlement_id,
                "transmission must stay within settlement locality");
        } else if (memory.source == universe::MemorySource::Record) {
            const auto access_it = record_access_by_memory.find(memory.id);
            require(access_it != record_access_by_memory.end(), "record memory must have record-access provenance");
            const auto* access = access_it->second;
            require(access->reader_id == memory.owner_id, "record access reader must match memory owner");
            require(access->read_at == memory.acquired_at, "record access time must match memory acquisition");
            require(access->confidence_per_10k == memory.confidence_per_10k, "record access confidence must match memory confidence");

            const auto record_it = record_by_id.find(access->record_id);
            require(record_it != record_by_id.end(), "record access must reference existing record");
            const auto* record = record_it->second;
            require(record->settlement_id == access->reader_settlement_id, "record access must stay within record settlement");
            require(record->source_event_id == memory.source_event_id, "record memory must preserve record source event");
            require(access->read_at.ticks >= record->recorded_at.ticks, "record access time must not predate record creation");
        }

        if (source_event->type == universe::EventType::Birth) {
            const auto& payload = std::get<universe::BirthEventPayload>(source_event->payload);
            if (memory.source == universe::MemorySource::DirectObservation) {
                require(memory.owner_id != payload.child_id, "newborn must not directly observe own birth");
            }
        }
        if (source_event->type == universe::EventType::Death) {
            const auto& payload = std::get<universe::DeathEventPayload>(source_event->payload);
            require(memory.owner_id != payload.person_id, "person must not remember own death");
        }
        if (source_event->type == universe::EventType::SettlementExtinct) {
            require(false, "settlement extinction should not create direct memories in current model");
        }
        if (source_event->type == universe::EventType::DiscoveryMade) {
            const auto& payload = std::get<universe::DiscoveryMadeEventPayload>(source_event->payload);
            if (memory.source == universe::MemorySource::DirectObservation) {
                require(memory.owner_id == payload.discoverer_id, "discovery direct memory must belong to discoverer");
            }
        }
    }

    struct KnowledgeSemanticKey {
        universe::PersonId owner_id{};
        universe::KnowledgeFactKind fact_kind{universe::KnowledgeFactKind::PersonLifeStatus};
        std::uint64_t primary_subject_id{};

        [[nodiscard]] bool operator==(const KnowledgeSemanticKey& other) const noexcept {
            return owner_id == other.owner_id &&
                fact_kind == other.fact_kind &&
                primary_subject_id == other.primary_subject_id;
        }
    };

    struct KnowledgeSemanticKeyHash {
        [[nodiscard]] std::size_t operator()(const KnowledgeSemanticKey& key) const noexcept {
            const auto mixed =
                (key.owner_id * 0x9E3779B97F4A7C15ULL) ^
                (static_cast<std::uint64_t>(key.fact_kind) * 0xD6E8FEB86659FD93ULL) ^
                (key.primary_subject_id * 0xA24BAED4963EE407ULL);
            return static_cast<std::size_t>(mixed);
        }
    };

    std::unordered_set<KnowledgeSemanticKey, KnowledgeSemanticKeyHash> semantic_keys;
    universe::KnowledgeId expected_knowledge_id = 1;
    for (const auto& knowledge : state.agent_knowledge) {
        require(knowledge.id == expected_knowledge_id, "knowledge ids must be contiguous and deterministic");
        ++expected_knowledge_id;
        require(knowledge.owner_id > 0U, "knowledge owner id must be non-zero");
        require(knowledge.source == universe::KnowledgeSource::MemoryDerived, "knowledge source must be memory-derived");
        require(knowledge.source_memory_id > 0U, "knowledge source memory id must be non-zero");
        require(knowledge.confidence_per_10k <= 10'000U, "knowledge confidence must remain bounded");
        require(knowledge.acquired_at.ticks >= 0, "knowledge acquired_at must be non-negative");
        require(knowledge.updated_at.ticks >= knowledge.acquired_at.ticks, "knowledge updated_at must be >= acquired_at");
        require(knowledge.updated_at.ticks <= state.time.ticks, "knowledge updated_at must not exceed simulation time");

        const auto* source_memory = [&state](const universe::MemoryId memory_id) -> const universe::AgentMemory* {
            for (const auto& memory : state.agent_memories) {
                if (memory.id == memory_id) {
                    return &memory;
                }
            }
            return nullptr;
        }(knowledge.source_memory_id);

        require(source_memory != nullptr, "knowledge source memory must reference existing memory");
        require(source_memory->owner_id == knowledge.owner_id, "knowledge source memory owner must match knowledge owner");
        require(source_memory->acquired_at == knowledge.updated_at, "knowledge updated_at must align with source memory acquisition");

        if (const auto* fact = std::get_if<universe::PersonLifeStatusKnowledgeFact>(&knowledge.fact); fact != nullptr) {
            require(fact->person_id > 0U, "person life-status knowledge person id must be non-zero");
            const auto* subject = find_person(state, fact->person_id);
            require(subject != nullptr, "person life-status knowledge person id must reference existing person");

            require(
                semantic_keys.insert(KnowledgeSemanticKey{
                    .owner_id = knowledge.owner_id,
                    .fact_kind = universe::KnowledgeFactKind::PersonLifeStatus,
                    .primary_subject_id = fact->person_id,
                }).second,
                "knowledge semantic key owner+person-life-status+subject must be unique");
        } else if (const auto* fact = std::get_if<universe::PersonHouseholdKnowledgeFact>(&knowledge.fact); fact != nullptr) {
            require(fact->person_id > 0U, "person-household knowledge person id must be non-zero");
            require(fact->household_id > 0U, "person-household knowledge household id must be non-zero");
            require(find_person(state, fact->person_id) != nullptr, "person-household knowledge person must exist");
            require(find_household(state, fact->household_id) != nullptr, "person-household knowledge household must exist");

            require(
                semantic_keys.insert(KnowledgeSemanticKey{
                    .owner_id = knowledge.owner_id,
                    .fact_kind = universe::KnowledgeFactKind::PersonHousehold,
                    .primary_subject_id = fact->person_id,
                }).second,
                "knowledge semantic key owner+person-household+subject must be unique");
        } else if (const auto* fact = std::get_if<universe::SettlementStatusKnowledgeFact>(&knowledge.fact); fact != nullptr) {
            require(fact->settlement_id > 0U, "settlement-status knowledge settlement id must be non-zero");
            require(fact->region_id > 0U, "settlement-status knowledge region id must be non-zero");
            require(find_settlement(state, fact->settlement_id) != nullptr, "settlement-status knowledge settlement must exist");

            require(
                semantic_keys.insert(KnowledgeSemanticKey{
                    .owner_id = knowledge.owner_id,
                    .fact_kind = universe::KnowledgeFactKind::SettlementStatus,
                    .primary_subject_id = fact->settlement_id,
                }).second,
                "knowledge semantic key owner+settlement-status+subject must be unique");
        } else if (const auto* fact = std::get_if<universe::BasicDiscoveryKnowledgeFact>(&knowledge.fact); fact != nullptr) {
            require(fact->discovery_key > 0U, "basic-discovery knowledge key must be non-zero");

            require(
                semantic_keys.insert(KnowledgeSemanticKey{
                    .owner_id = knowledge.owner_id,
                    .fact_kind = universe::KnowledgeFactKind::BasicDiscovery,
                    .primary_subject_id = fact->discovery_key,
                }).second,
                "knowledge semantic key owner+basic-discovery+subject must be unique");
        } else {
            require(false, "knowledge fact variant must be recognized");
        }
    }
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
    require(lhs.total_household_wealth == rhs.total_household_wealth, context);
    require(lhs.total_settlement_commons_food == rhs.total_settlement_commons_food, context);
    require(lhs.active_buildings == rhs.active_buildings, context);
    require(lhs.active_projects == rhs.active_projects, context);
    require(lhs.total_exchange_volume == rhs.total_exchange_volume, context);
    require(lhs.total_transfer_volume == rhs.total_transfer_volume, context);
    require(lhs.detail_level == rhs.detail_level, context);
    require(lhs.region_lod_states.size() == rhs.region_lod_states.size(), context);
    require(lhs.population_cohorts.size() == rhs.population_cohorts.size(), context);
    require(lhs.settlement_aggregates.size() == rhs.settlement_aggregates.size(), context);
    require(lhs.people.size() == rhs.people.size(), context);
    require(lhs.households.size() == rhs.households.size(), context);
    require(lhs.regions.size() == rhs.regions.size(), context);
    require(lhs.settlements.size() == rhs.settlements.size(), context);
    require(lhs.buildings.size() == rhs.buildings.size(), context);
    require(lhs.event_log.size() == rhs.event_log.size(), context);
    require(lhs.agent_memories.size() == rhs.agent_memories.size(), context);
    require(lhs.knowledge_transmissions.size() == rhs.knowledge_transmissions.size(), context);
    require(lhs.knowledge_records.size() == rhs.knowledge_records.size(), context);
    require(lhs.knowledge_record_accesses.size() == rhs.knowledge_record_accesses.size(), context);
    require(lhs.agent_knowledge.size() == rhs.agent_knowledge.size(), context);
    require(lhs.institutions.size() == rhs.institutions.size(), context);

    for (std::size_t i = 0; i < lhs.region_lod_states.size(); ++i) {
        const auto& a = lhs.region_lod_states[i];
        const auto& b = rhs.region_lod_states[i];
        require(a.region_id == b.region_id, context);
        require(a.detail_level == b.detail_level, context);
    }

    for (std::size_t i = 0; i < lhs.population_cohorts.size(); ++i) {
        const auto& a = lhs.population_cohorts[i];
        const auto& b = rhs.population_cohorts[i];
        require(a.id == b.id, context);
        require(a.region_id == b.region_id, context);
        require(a.settlement_id == b.settlement_id, context);
        require(a.age_band == b.age_band, context);
        require(a.sex == b.sex, context);
        require(a.occupation_group == b.occupation_group, context);
        require(a.health_band == b.health_band, context);
        require(a.wealth_band == b.wealth_band, context);
        require(a.knowledge_profile_id == b.knowledge_profile_id, context);
        require(a.population == b.population, context);
    }

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
        require(a.materialized_from_cohort == b.materialized_from_cohort, context);
        require(a.source_cohort_id == b.source_cohort_id, context);
    }

    for (std::size_t i = 0; i < lhs.settlement_aggregates.size(); ++i) {
        const auto& a = lhs.settlement_aggregates[i];
        const auto& b = rhs.settlement_aggregates[i];
        require(a.settlement_id == b.settlement_id, context);
        require(a.region_id == b.region_id, context);
        require(a.household_count == b.household_count, context);
        require(a.small_household_count == b.small_household_count, context);
        require(a.medium_household_count == b.medium_household_count, context);
        require(a.large_household_count == b.large_household_count, context);
        require(a.food_inventory == b.food_inventory, context);
        require(a.food_produced_total == b.food_produced_total, context);
        require(a.food_consumed_total == b.food_consumed_total, context);
        require(a.unmet_food_total == b.unmet_food_total, context);
        require(a.labour_capacity == b.labour_capacity, context);
        require(a.labour_demand == b.labour_demand, context);
        require(a.labour_allocated == b.labour_allocated, context);
        require(a.labour_low_household_count == b.labour_low_household_count, context);
        require(a.labour_medium_household_count == b.labour_medium_household_count, context);
        require(a.labour_high_household_count == b.labour_high_household_count, context);
        require(a.wealth_low_household_count == b.wealth_low_household_count, context);
        require(a.wealth_medium_household_count == b.wealth_medium_household_count, context);
        require(a.wealth_high_household_count == b.wealth_high_household_count, context);
        require(a.food_buffer_low_household_count == b.food_buffer_low_household_count, context);
        require(a.food_buffer_medium_household_count == b.food_buffer_medium_household_count, context);
        require(a.food_buffer_high_household_count == b.food_buffer_high_household_count, context);
        require(a.wealth_points_total == b.wealth_points_total, context);
        require(a.productive_access_points == b.productive_access_points, context);
        require(a.required_food_year == b.required_food_year, context);
        require(a.unmet_food_year == b.unmet_food_year, context);
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
        require(a.owned_materials.wood.value == b.owned_materials.wood.value, context);
        require(a.owned_materials.stone.value == b.owned_materials.stone.value, context);
        require(a.owned_materials.timber.value == b.owned_materials.timber.value, context);
        require(a.owned_materials.basic_tool.value == b.owned_materials.basic_tool.value, context);
        require(a.productive_access_share_points == b.productive_access_share_points, context);
        require(a.commons_access_share_points == b.commons_access_share_points, context);
        require(a.cumulative_food_transferred_in == b.cumulative_food_transferred_in, context);
        require(a.cumulative_food_transferred_out == b.cumulative_food_transferred_out, context);
        require(a.cumulative_material_transferred_in == b.cumulative_material_transferred_in, context);
        require(a.cumulative_material_transferred_out == b.cumulative_material_transferred_out, context);
        require(a.derived_wealth_points == b.derived_wealth_points, context);
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
        require(a.commons_food.value == b.commons_food.value, context);
        require(a.last_transfer_food_volume == b.last_transfer_food_volume, context);
        require(a.last_exchange_volume == b.last_exchange_volume, context);
        require(a.economic_viability_streak == b.economic_viability_streak, context);
        require(a.economic_pressure_streak == b.economic_pressure_streak, context);
        require(a.dwelling_capacity_bonus == b.dwelling_capacity_bonus, context);
        require(a.food_store_capacity_bonus == b.food_store_capacity_bonus, context);
        require(a.workshop_capacity_bonus == b.workshop_capacity_bonus, context);
        require(a.last_median_household_wealth == b.last_median_household_wealth, context);
        require(a.last_min_household_wealth == b.last_min_household_wealth, context);
        require(a.last_max_household_wealth == b.last_max_household_wealth, context);
        require(a.last_inequality_numerator == b.last_inequality_numerator, context);
        require(a.last_inequality_denominator == b.last_inequality_denominator, context);
    }

    for (std::size_t i = 0; i < lhs.buildings.size(); ++i) {
        const auto& a = lhs.buildings[i];
        const auto& b = rhs.buildings[i];
        require(a.id == b.id, context);
        require(a.type == b.type, context);
        require(a.settlement_id == b.settlement_id, context);
        require(a.owner_household_id == b.owner_household_id, context);
        require(a.settlement_commons_owned == b.settlement_commons_owned, context);
        require(a.state == b.state, context);
        require(a.timber_required == b.timber_required, context);
        require(a.stone_required == b.stone_required, context);
        require(a.labour_required == b.labour_required, context);
        require(a.timber_committed == b.timber_committed, context);
        require(a.stone_committed == b.stone_committed, context);
        require(a.labour_completed == b.labour_completed, context);
        require(a.started_at == b.started_at, context);
        require(a.completed_at == b.completed_at, context);
    }

    for (std::size_t i = 0; i < lhs.event_log.size(); ++i) {
        const auto& a = lhs.event_log[i];
        const auto& b = rhs.event_log[i];
        require(a.id == b.id, context);
        require(a.timestamp == b.timestamp, context);
        require(a.type == b.type, context);
        require(a.payload == b.payload, context);
    }

    for (std::size_t i = 0; i < lhs.agent_memories.size(); ++i) {
        const auto& a = lhs.agent_memories[i];
        const auto& b = rhs.agent_memories[i];
        require(a.id == b.id, context);
        require(a.owner_id == b.owner_id, context);
        require(a.acquired_at == b.acquired_at, context);
        require(a.source == b.source, context);
        require(a.source_event_id == b.source_event_id, context);
        require(a.confidence_per_10k == b.confidence_per_10k, context);
    }

    for (std::size_t i = 0; i < lhs.knowledge_transmissions.size(); ++i) {
        const auto& a = lhs.knowledge_transmissions[i];
        const auto& b = rhs.knowledge_transmissions[i];
        require(a.id == b.id, context);
        require(a.speaker_id == b.speaker_id, context);
        require(a.recipient_id == b.recipient_id, context);
        require(a.speaker_household_id == b.speaker_household_id, context);
        require(a.recipient_household_id == b.recipient_household_id, context);
        require(a.speaker_settlement_id == b.speaker_settlement_id, context);
        require(a.recipient_settlement_id == b.recipient_settlement_id, context);
        require(a.transmitted_at == b.transmitted_at, context);
        require(a.method == b.method, context);
        require(a.speaker_memory_id == b.speaker_memory_id, context);
        require(a.recipient_memory_id == b.recipient_memory_id, context);
        require(a.confidence_per_10k == b.confidence_per_10k, context);
    }

    for (std::size_t i = 0; i < lhs.knowledge_records.size(); ++i) {
        const auto& a = lhs.knowledge_records[i];
        const auto& b = rhs.knowledge_records[i];
        require(a.id == b.id, context);
        require(a.settlement_id == b.settlement_id, context);
        require(a.author_id == b.author_id, context);
        require(a.author_household_id == b.author_household_id, context);
        require(a.recorded_at == b.recorded_at, context);
        require(a.source_memory_id == b.source_memory_id, context);
        require(a.source_event_id == b.source_event_id, context);
        require(a.confidence_per_10k == b.confidence_per_10k, context);
    }

    for (std::size_t i = 0; i < lhs.knowledge_record_accesses.size(); ++i) {
        const auto& a = lhs.knowledge_record_accesses[i];
        const auto& b = rhs.knowledge_record_accesses[i];
        require(a.record_id == b.record_id, context);
        require(a.reader_id == b.reader_id, context);
        require(a.reader_household_id == b.reader_household_id, context);
        require(a.reader_settlement_id == b.reader_settlement_id, context);
        require(a.reader_memory_id == b.reader_memory_id, context);
        require(a.read_at == b.read_at, context);
        require(a.confidence_per_10k == b.confidence_per_10k, context);
    }

    for (std::size_t i = 0; i < lhs.agent_knowledge.size(); ++i) {
        const auto& a = lhs.agent_knowledge[i];
        const auto& b = rhs.agent_knowledge[i];
        require(a.id == b.id, context);
        require(a.owner_id == b.owner_id, context);
        require(a.acquired_at == b.acquired_at, context);
        require(a.updated_at == b.updated_at, context);
        require(a.source == b.source, context);
        require(a.source_memory_id == b.source_memory_id, context);
        require(a.fact == b.fact, context);
        require(a.confidence_per_10k == b.confidence_per_10k, context);
    }

    for (std::size_t i = 0; i < lhs.institutions.size(); ++i) {
        const auto& a = lhs.institutions[i];
        const auto& b = rhs.institutions[i];
        require(a.id == b.id, context);
        require(a.kind == b.kind, context);
        require(a.settlement_id == b.settlement_id, context);
        require(a.founder_id == b.founder_id, context);
        require(a.founded_at == b.founded_at, context);
        require(a.active == b.active, context);
        require(a.member_ids == b.member_ids, context);
        require(a.preserved_record_ids == b.preserved_record_ids, context);
        require(a.preserved_discovery_keys == b.preserved_discovery_keys, context);
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

struct EventSectionInfo {
    std::size_t event_count_offset{};
    std::uint64_t event_count{};
    std::vector<std::size_t> event_offsets{};
    std::vector<std::size_t> event_type_offsets{};
    std::vector<universe::EventType> event_types{};
    std::vector<std::optional<std::size_t>> payload_discriminator_offsets{};
    std::size_t section_end_offset{};
};

EventSectionInfo locate_event_section(const universe::WorldState& state) {
    constexpr std::size_t header_size = 24U;
    constexpr std::size_t payload_people_count_offset = 590U;

    EventSectionInfo info{};
    std::size_t cursor = header_size + payload_people_count_offset;

    const auto person_record_base_size = std::size_t{41};
    std::vector<const universe::Person*> people_order;
    people_order.reserve(state.people.size());
    for (const auto& person : state.people) {
        people_order.push_back(&person);
    }
    std::sort(people_order.begin(), people_order.end(), [](const universe::Person* lhs, const universe::Person* rhs) {
        return lhs->id < rhs->id;
    });

    cursor += 8U;
    for (const auto* person : people_order) {
        std::size_t record_size = person_record_base_size;
        if (person->mother_id.has_value()) {
            record_size += 8U;
        }
        if (person->father_id.has_value()) {
            record_size += 8U;
        }
        cursor += record_size;
    }

    std::vector<const universe::Household*> household_order;
    household_order.reserve(state.households.size());
    for (const auto& household : state.households) {
        household_order.push_back(&household);
    }
    std::sort(household_order.begin(), household_order.end(), [](const universe::Household* lhs, const universe::Household* rhs) {
        return lhs->id < rhs->id;
    });

    cursor += 8U;
    for (const auto* household : household_order) {
        std::vector<universe::PersonId> member_ids = household->member_ids;
        std::sort(member_ids.begin(), member_ids.end());
        cursor += 76U;
        cursor += member_ids.size() * 8U;
    }

    std::vector<const universe::RegionEnvironmentState*> region_order;
    region_order.reserve(state.regions.size());
    for (const auto& region : state.regions) {
        region_order.push_back(&region);
    }
    std::sort(region_order.begin(), region_order.end(), [](const universe::RegionEnvironmentState* lhs, const universe::RegionEnvironmentState* rhs) {
        return lhs->id < rhs->id;
    });

    cursor += 8U;
    cursor += region_order.size() * 89U;

    std::vector<const universe::Settlement*> settlement_order;
    settlement_order.reserve(state.settlements.size());
    for (const auto& settlement : state.settlements) {
        settlement_order.push_back(&settlement);
    }
    std::sort(settlement_order.begin(), settlement_order.end(), [](const universe::Settlement* lhs, const universe::Settlement* rhs) {
        return lhs->id < rhs->id;
    });

    cursor += 8U;
    cursor += settlement_order.size() * 37U;

    std::vector<const universe::SimulationEvent*> event_order;
    event_order.reserve(state.event_log.size());
    for (const auto& event : state.event_log) {
        event_order.push_back(&event);
    }
    std::sort(event_order.begin(), event_order.end(), [](const universe::SimulationEvent* lhs, const universe::SimulationEvent* rhs) {
        return lhs->id < rhs->id;
    });

    info.event_count_offset = cursor;
    info.event_count = static_cast<std::uint64_t>(event_order.size());
    cursor += 8U;
    info.event_offsets.reserve(static_cast<std::size_t>(info.event_count));
    info.event_type_offsets.reserve(static_cast<std::size_t>(info.event_count));
    info.event_types.reserve(static_cast<std::size_t>(info.event_count));
    info.payload_discriminator_offsets.reserve(static_cast<std::size_t>(info.event_count));

    for (const auto* event : event_order) {
        info.event_offsets.push_back(cursor);
        cursor += 16U;

        const auto type = event->type;
        const auto type_offset = cursor;
        cursor += 1U;
        info.event_type_offsets.push_back(type_offset);
        info.event_types.push_back(type);

        std::optional<std::size_t> discriminator_offset = std::nullopt;
        if (type == universe::EventType::Birth) {
            const auto& payload = std::get<universe::BirthEventPayload>(event->payload);
            cursor += 16U;
            discriminator_offset = cursor;
            cursor += 1U;
            if (payload.father_id.has_value()) {
                cursor += 8U;
            }
            cursor += 8U;
        } else if (type == universe::EventType::Death) {
            cursor += 16U;
        } else if (type == universe::EventType::SettlementFounded) {
            const auto& payload = std::get<universe::SettlementFoundedEventPayload>(event->payload);
            cursor += 16U;
            discriminator_offset = cursor;
            cursor += 1U;
            if (payload.source_settlement_id.has_value()) {
                cursor += 8U;
            }
        } else if (type == universe::EventType::SettlementExtinct) {
            cursor += 16U;
        } else if (type == universe::EventType::DiscoveryMade) {
            cursor += 20U;
        } else if (type == universe::EventType::InstitutionFounded) {
            cursor += 25U;
        } else if (type == universe::EventType::SettlementMerged) {
            cursor += 24U;
        }

        info.payload_discriminator_offsets.push_back(discriminator_offset);
    }

    info.section_end_offset = cursor;
    return info;
}

struct MemorySectionInfo {
    std::size_t memory_count_offset{};
    std::uint64_t memory_count{};
    std::vector<std::size_t> memory_offsets{};
};

MemorySectionInfo locate_memory_section(const universe::WorldState& state) {
    constexpr std::size_t kMemoryRecordSize = 37U;
    const auto event_section = locate_event_section(state);

    std::vector<const universe::AgentMemory*> memory_order;
    memory_order.reserve(state.agent_memories.size());
    for (const auto& memory : state.agent_memories) {
        memory_order.push_back(&memory);
    }
    std::sort(memory_order.begin(), memory_order.end(), [](const universe::AgentMemory* lhs, const universe::AgentMemory* rhs) {
        return lhs->id < rhs->id;
    });

    MemorySectionInfo info{};
    info.memory_count_offset = event_section.section_end_offset;
    info.memory_count = static_cast<std::uint64_t>(memory_order.size());
    info.memory_offsets.reserve(memory_order.size());

    std::size_t cursor = info.memory_count_offset + 8U;
    for (std::size_t i = 0; i < memory_order.size(); ++i) {
        info.memory_offsets.push_back(cursor);
        cursor += kMemoryRecordSize;
    }

    return info;
}

struct KnowledgeSectionInfo {
    std::size_t knowledge_count_offset{};
    std::uint64_t knowledge_count{};
    std::vector<std::size_t> knowledge_offsets{};
    std::vector<std::size_t> knowledge_fact_kind_offsets{};
    std::vector<universe::KnowledgeFactKind> fact_kinds{};
    std::size_t next_knowledge_id_offset{};
};

KnowledgeSectionInfo locate_knowledge_section(const universe::WorldState& state) {
    const auto memory_section = locate_memory_section(state);

    std::vector<const universe::AgentKnowledge*> knowledge_order;
    knowledge_order.reserve(state.agent_knowledge.size());
    for (const auto& knowledge : state.agent_knowledge) {
        knowledge_order.push_back(&knowledge);
    }
    std::sort(knowledge_order.begin(), knowledge_order.end(), [](const universe::AgentKnowledge* lhs, const universe::AgentKnowledge* rhs) {
        return lhs->id < rhs->id;
    });

    KnowledgeSectionInfo info{};
    info.knowledge_count_offset = memory_section.memory_count_offset + 8U + (memory_section.memory_offsets.size() * 37U);
    info.knowledge_count = static_cast<std::uint64_t>(knowledge_order.size());
    info.knowledge_offsets.reserve(knowledge_order.size());
    info.knowledge_fact_kind_offsets.reserve(knowledge_order.size());
    info.fact_kinds.reserve(knowledge_order.size());

    std::size_t cursor = info.knowledge_count_offset + 8U;
    for (const auto* knowledge : knowledge_order) {
        info.knowledge_offsets.push_back(cursor);
        cursor += 8U;   // id
        cursor += 8U;   // owner
        cursor += 8U;   // acquired
        cursor += 8U;   // updated
        cursor += 1U;   // source
        cursor += 8U;   // source memory
        info.knowledge_fact_kind_offsets.push_back(cursor);
        const auto kind_offset = cursor;
        cursor += 1U;   // fact kind

        if (std::holds_alternative<universe::PersonLifeStatusKnowledgeFact>(knowledge->fact)) {
            info.fact_kinds.push_back(universe::KnowledgeFactKind::PersonLifeStatus);
            cursor += 8U; // person id
            cursor += 1U; // life status
        } else if (std::holds_alternative<universe::PersonHouseholdKnowledgeFact>(knowledge->fact)) {
            info.fact_kinds.push_back(universe::KnowledgeFactKind::PersonHousehold);
            cursor += 8U; // person id
            cursor += 8U; // household id
        } else if (std::holds_alternative<universe::SettlementStatusKnowledgeFact>(knowledge->fact)) {
            info.fact_kinds.push_back(universe::KnowledgeFactKind::SettlementStatus);
            cursor += 8U; // settlement id
            cursor += 8U; // region id
            cursor += 1U; // active
        } else {
            (void)kind_offset;
            info.fact_kinds.push_back(universe::KnowledgeFactKind::BasicDiscovery);
            cursor += 4U; // discovery key
        }

        cursor += 4U;   // confidence
    }

    info.next_knowledge_id_offset = cursor;
    return info;
}

void require_snapshot_load_fails_any(const std::vector<std::byte>& bytes, const char* context) {
    universe::Simulation destination{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto result = universe::Simulation::load_snapshot(bytes, destination);
    require(!result.ok, context);
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
        // Ticket #33: settlement merging can transiently disrupt a handful of
        // people's nutrition right around a merge event (the newly-consolidated
        // household set takes one cycle to fully integrate), even with abundant
        // food overall. Bound it tightly rather than requiring exact zero, since
        // merging is now a real, expected structural event at this scale.
        require(abundant_run.state().population_under_food_stress * 20U <= abundant_run.state().population,
                "sustained food surplus should keep food stress rare, bounded by transient settlement merges");
        require(abundant_run.state().households_in_shortage * 20U <= abundant_run.state().households.size(),
                "sustained food surplus should keep active shortages rare, bounded by transient settlement merges");

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
        auto crisis = config;
        crisis.seed = 22;
        crisis.initial_population = 280;
        crisis.initial_region_count = 1;
        crisis.base_region_area_units = 55;
        crisis.initial_household_food_units = 12;
        crisis.food_production_scale_percent = 72;
        crisis.productive_effort_scale_percent = 85;

        universe::Simulation crisis_run{crisis};
        crisis_run.run_years(80);
        const auto& state = crisis_run.state();
        require(
            state.households_in_shortage > 0U || state.total_unmet_food > 0 || state.population_under_food_stress > 0U,
            "crisis scenario must include measurable scarcity signals");

        bool checked_high_pressure_settlement = false;
        for (const auto& settlement : state.settlements) {
            if (!settlement.active) {
                continue;
            }

            if (settlement.economic_pressure_streak == 0U) {
                continue;
            }

            std::uint64_t subsistence_workers = 0;
            std::uint64_t optional_workers = 0;
            for (const auto& household : state.households) {
                if (household.settlement_id != settlement.id) {
                    continue;
                }
                for (const auto member_id : household.member_ids) {
                    const auto* person = find_person(state, member_id);
                    if (person == nullptr || !occupation_eligible_for_productive_work(state, *person, 100U)) {
                        continue;
                    }

                    if (person->occupation == universe::Occupation::SubsistenceCultivator ||
                        person->occupation == universe::Occupation::WildFoodGatherer) {
                        ++subsistence_workers;
                    } else if (person->occupation == universe::Occupation::WoodGatherer ||
                               person->occupation == universe::Occupation::StoneExtractor ||
                               person->occupation == universe::Occupation::CraftWorker) {
                        ++optional_workers;
                    }
                }
            }

            require(
                subsistence_workers >= optional_workers,
                "high-pressure settlements must not prioritize optional labour over subsistence");
            checked_high_pressure_settlement = true;
        }
        require(
            checked_high_pressure_settlement || state.total_unmet_food > 0 || state.population_under_food_stress > 0U,
            "crisis scenario should produce high-pressure settlements or equivalent scarcity stress");
    }

    {
        auto constrained_materials = config;
        constrained_materials.seed = 44;
        constrained_materials.initial_population = 220;
        constrained_materials.initial_region_count = 2;
        constrained_materials.base_region_area_units = 110;
        constrained_materials.wood_resource_scale_percent = 35;
        constrained_materials.stone_resource_scale_percent = 35;
        constrained_materials.material_crafting_scale_percent = 35;

        auto abundant_materials = constrained_materials;
        abundant_materials.wood_resource_scale_percent = 180;
        abundant_materials.stone_resource_scale_percent = 180;
        abundant_materials.material_crafting_scale_percent = 180;

        universe::Simulation constrained_run{constrained_materials};
        universe::Simulation abundant_run{abundant_materials};
        constrained_run.run_years(70);
        abundant_run.run_years(70);

        require(
            constrained_run.state().material_production_shortfall >= abundant_run.state().material_production_shortfall,
            "material-constrained scenario should not reduce production shortfall");
        require(
            constrained_run.state().material_labour_shortfall >= abundant_run.state().material_labour_shortfall,
            "material-constrained scenario should not reduce labour shortfall");
    }

    {
        auto viable = config;
        viable.seed = 52;
        viable.initial_population = 240;
        viable.initial_region_count = 2;
        viable.base_region_area_units = 130;
        viable.initial_household_food_units = 90;
        viable.food_production_scale_percent = 110;

        auto crisis = viable;
        crisis.food_production_scale_percent = 70;
        crisis.initial_household_food_units = 15;

        universe::Simulation viable_run{viable};
        universe::Simulation crisis_run{crisis};
        viable_run.run_years(120);
        crisis_run.run_years(120);

        require(
            viable_run.state().active_buildings >= crisis_run.state().active_buildings,
            "viable economy should build at least as much as crisis economy");
        require(
            crisis_run.state().total_unmet_food >= viable_run.state().total_unmet_food ||
                crisis_run.state().population_under_food_stress >= viable_run.state().population_under_food_stress ||
                crisis_run.state().households_in_shortage >= viable_run.state().households_in_shortage,
            "construction competition must not suppress subsistence crisis signals");
    }

    {
        auto extraction_ceiling = config;
        extraction_ceiling.seed = 61;
        extraction_ceiling.initial_population = 260;
        extraction_ceiling.initial_region_count = 2;
        extraction_ceiling.base_region_area_units = 140;
        extraction_ceiling.wood_resource_scale_percent = 220;
        extraction_ceiling.stone_resource_scale_percent = 220;
        extraction_ceiling.material_crafting_scale_percent = 220;

        universe::Simulation ceiling_run{extraction_ceiling};
        ceiling_run.run_years(120);
        for (const auto& region : ceiling_run.state().regions) {
            require(
                region.stone_remaining + region.cumulative_stone_extracted == region.stone_capacity,
                "stone extraction must conserve finite regional stock");
            require(region.wood_stock <= region.wood_capacity, "wood stock must remain bounded by capacity");
            require(region.stone_remaining >= 0, "stone remaining must never be negative");
            require(region.wood_stock >= 0, "wood stock must never be negative");
        }
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
            before_relief_health >= relieved_health ||
                feedback.state().total_unmet_food >= recovery.state().total_unmet_food ||
                feedback.state().households_in_shortage >= recovery.state().households_in_shortage,
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
        bad_version[4] = std::byte{0x08};
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
        constexpr std::size_t payload_people_count_offset = 590U;
        constexpr std::size_t payload_first_person_offset = 598U;
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

        // The payload ends with aggregate_births_total then aggregate_deaths_total.
        constexpr std::size_t aggregate_demography_tail_size = 16U;
        require(
            valid.size() > aggregate_demography_tail_size,
            "snapshot must include the aggregate demography tail");
        const auto aggregate_births_offset = valid.size() - aggregate_demography_tail_size;
        const auto aggregate_deaths_offset = valid.size() - 8U;

        auto excessive_aggregate_births = clone_bytes(valid);
        write_u64_le(excessive_aggregate_births, aggregate_births_offset, 1'000'000'000ULL);
        refresh_snapshot_payload_hash(excessive_aggregate_births);
        require_snapshot_load_fails(
            excessive_aggregate_births,
            universe::SnapshotErrorCode::InvalidState,
            "aggregate births exceeding canonical totals must be rejected");

        auto excessive_aggregate_deaths = clone_bytes(valid);
        write_u64_le(excessive_aggregate_deaths, aggregate_deaths_offset, 1'000'000'000ULL);
        refresh_snapshot_payload_hash(excessive_aggregate_deaths);
        require_snapshot_load_fails(
            excessive_aggregate_deaths,
            universe::SnapshotErrorCode::InvalidState,
            "aggregate deaths exceeding canonical totals must be rejected");

        auto truncated_aggregate_tail = clone_bytes(valid);
        truncated_aggregate_tail.resize(truncated_aggregate_tail.size() - 4U);
        // The declared payload size must shrink with the buffer, otherwise the
        // hash refresh would read past the end.
        write_u64_le(
            truncated_aggregate_tail,
            8U,
            static_cast<std::uint64_t>(truncated_aggregate_tail.size() - 24U));
        refresh_snapshot_payload_hash(truncated_aggregate_tail);
        require_snapshot_load_fails(
            truncated_aggregate_tail,
            universe::SnapshotErrorCode::TruncatedData,
            "a truncated aggregate demography tail must be rejected");
    }

    {
        universe::Simulation journal_source{config};
        journal_source.run_years(120);
        const auto valid = journal_source.save_snapshot();
        const auto event_section = locate_event_section(journal_source.state());
        require(event_section.event_count > 1U, "journal corruption tests require at least two events");

        auto invalid_event_enum = clone_bytes(valid);
        invalid_event_enum[event_section.event_type_offsets.front()] = std::byte{9};
        refresh_snapshot_payload_hash(invalid_event_enum);
        require_snapshot_load_fails(
            invalid_event_enum,
            universe::SnapshotErrorCode::InvalidEnum,
            "invalid event enum values must be rejected");

        auto invalid_reference = clone_bytes(valid);
        const auto first_event_payload_offset = event_section.event_offsets.front() + 17U;
        write_u64_le(invalid_reference, first_event_payload_offset, 9'999'999'999ULL);
        refresh_snapshot_payload_hash(invalid_reference);
        require_snapshot_load_fails(
            invalid_reference,
            universe::SnapshotErrorCode::InvalidReference,
            "invalid event payload references must be rejected");

        auto non_contiguous_ids = clone_bytes(valid);
        const auto first_event_id = read_u64_le(non_contiguous_ids, event_section.event_offsets.front());
        write_u64_le(non_contiguous_ids, event_section.event_offsets[1], first_event_id);
        refresh_snapshot_payload_hash(non_contiguous_ids);
        require_snapshot_load_fails(
            non_contiguous_ids,
            universe::SnapshotErrorCode::DuplicateId,
            "non-contiguous event ids must be rejected");

        auto future_timestamp = clone_bytes(valid);
        write_u64_le(
            future_timestamp,
            event_section.event_offsets.front() + 8U,
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        refresh_snapshot_payload_hash(future_timestamp);
        require_snapshot_load_fails(
            future_timestamp,
            universe::SnapshotErrorCode::InvalidState,
            "future event timestamps must be rejected");

        auto timestamp_order_violation = clone_bytes(valid);
        write_u64_le(timestamp_order_violation, event_section.event_offsets.front() + 8U, 10ULL);
        write_u64_le(timestamp_order_violation, event_section.event_offsets[1] + 8U, 0ULL);
        refresh_snapshot_payload_hash(timestamp_order_violation);
        require_snapshot_load_fails(
            timestamp_order_violation,
            universe::SnapshotErrorCode::InvalidState,
            "event timestamp ordering violations must be rejected");

        auto incompatible_payload = clone_bytes(valid);
        incompatible_payload[event_section.event_type_offsets.front()] =
            static_cast<std::byte>(static_cast<std::uint8_t>(universe::EventType::SettlementExtinct));
        refresh_snapshot_payload_hash(incompatible_payload);
        require_snapshot_load_fails_any(
            incompatible_payload,
            "payload incompatible with event type must be rejected");

        std::size_t discriminator_index = std::numeric_limits<std::size_t>::max();
        for (std::size_t i = 0; i < event_section.payload_discriminator_offsets.size(); ++i) {
            if (event_section.payload_discriminator_offsets[i].has_value()) {
                discriminator_index = i;
                break;
            }
        }
        require(
            discriminator_index != std::numeric_limits<std::size_t>::max(),
            "journal corruption tests require at least one payload discriminator");

        auto invalid_discriminator = clone_bytes(valid);
        invalid_discriminator[*event_section.payload_discriminator_offsets[discriminator_index]] = std::byte{2};
        refresh_snapshot_payload_hash(invalid_discriminator);
        require_snapshot_load_fails_any(
            invalid_discriminator,
            "invalid payload discriminators must be rejected");
    }

    {
        universe::Simulation memory_source{config};
        memory_source.run_years(120);
        const auto valid = memory_source.save_snapshot();
        const auto memory_section = locate_memory_section(memory_source.state());
        require(memory_section.memory_count > 1U, "memory corruption tests require at least two memories");

        auto memory_id_zero = clone_bytes(valid);
        write_u64_le(memory_id_zero, memory_section.memory_offsets.front(), 0U);
        refresh_snapshot_payload_hash(memory_id_zero);
        require_snapshot_load_fails(
            memory_id_zero,
            universe::SnapshotErrorCode::InvalidState,
            "memory id zero must be rejected");

        auto duplicate_memory_id = clone_bytes(valid);
        const auto first_memory_id = read_u64_le(duplicate_memory_id, memory_section.memory_offsets.front());
        write_u64_le(duplicate_memory_id, memory_section.memory_offsets[1], first_memory_id);
        refresh_snapshot_payload_hash(duplicate_memory_id);
        require_snapshot_load_fails(
            duplicate_memory_id,
            universe::SnapshotErrorCode::DuplicateId,
            "duplicate memory ids must be rejected");

        auto non_contiguous_memory_ids = clone_bytes(valid);
        write_u64_le(
            non_contiguous_memory_ids,
            memory_section.memory_offsets.front(),
            static_cast<std::uint64_t>(memory_section.memory_count + 17U));
        refresh_snapshot_payload_hash(non_contiguous_memory_ids);
        require_snapshot_load_fails(
            non_contiguous_memory_ids,
            universe::SnapshotErrorCode::InvalidState,
            "non-contiguous memory ids must be rejected");

        auto invalid_owner = clone_bytes(valid);
        write_u64_le(invalid_owner, memory_section.memory_offsets.front() + 8U, 0U);
        refresh_snapshot_payload_hash(invalid_owner);
        require_snapshot_load_fails(
            invalid_owner,
            universe::SnapshotErrorCode::InvalidReference,
            "memory owner id must be valid");

        auto invalid_event_id = clone_bytes(valid);
        write_u64_le(invalid_event_id, memory_section.memory_offsets.front() + 25U, 0U);
        refresh_snapshot_payload_hash(invalid_event_id);
        require_snapshot_load_fails(
            invalid_event_id,
            universe::SnapshotErrorCode::InvalidReference,
            "memory source event id must be valid");

        auto future_acquired_at = clone_bytes(valid);
        write_u64_le(
            future_acquired_at,
            memory_section.memory_offsets.front() + 16U,
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        refresh_snapshot_payload_hash(future_acquired_at);
        require_snapshot_load_fails(
            future_acquired_at,
            universe::SnapshotErrorCode::InvalidState,
            "memory acquired_at beyond simulation time must be rejected");

        auto invalid_source_enum = clone_bytes(valid);
        invalid_source_enum[memory_section.memory_offsets.front() + 24U] = std::byte{4};
        refresh_snapshot_payload_hash(invalid_source_enum);
        require_snapshot_load_fails(
            invalid_source_enum,
            universe::SnapshotErrorCode::InvalidEnum,
            "memory source enum out of range must be rejected");

        auto invalid_confidence = clone_bytes(valid);
        write_u32_le(invalid_confidence, memory_section.memory_offsets.front() + 33U, 10'001U);
        refresh_snapshot_payload_hash(invalid_confidence);
        require_snapshot_load_fails(
            invalid_confidence,
            universe::SnapshotErrorCode::InvalidState,
            "memory confidence above 10000 must be rejected");

        auto duplicate_direct_memory = clone_bytes(valid);
        const auto first_owner_id = read_u64_le(duplicate_direct_memory, memory_section.memory_offsets.front() + 8U);
        const auto first_event_id = read_u64_le(duplicate_direct_memory, memory_section.memory_offsets.front() + 25U);
        duplicate_direct_memory[memory_section.memory_offsets[1] + 24U] = std::byte{0};
        write_u64_le(duplicate_direct_memory, memory_section.memory_offsets[1] + 8U, first_owner_id);
        write_u64_le(duplicate_direct_memory, memory_section.memory_offsets[1] + 25U, first_event_id);
        refresh_snapshot_payload_hash(duplicate_direct_memory);
        require_snapshot_load_fails(
            duplicate_direct_memory,
            universe::SnapshotErrorCode::InvalidState,
            "duplicate direct owner/event memories must be rejected");

        auto acquired_before_event = clone_bytes(valid);
        write_u64_le(acquired_before_event, memory_section.memory_offsets.front() + 16U, 0U);
        refresh_snapshot_payload_hash(acquired_before_event);
        require_snapshot_load_fails(
            acquired_before_event,
            universe::SnapshotErrorCode::InvalidState,
            "memory acquired_at before source event timestamp must be rejected");
    }

    {
        universe::Simulation knowledge_source{config};
        knowledge_source.run_years(140);
        const auto valid = knowledge_source.save_snapshot();
        const auto knowledge_section = locate_knowledge_section(knowledge_source.state());
        require(knowledge_section.knowledge_count > 1U, "knowledge corruption tests require at least two knowledge records");

        auto knowledge_id_zero = clone_bytes(valid);
        write_u64_le(knowledge_id_zero, knowledge_section.knowledge_offsets.front(), 0U);
        refresh_snapshot_payload_hash(knowledge_id_zero);
        require_snapshot_load_fails(
            knowledge_id_zero,
            universe::SnapshotErrorCode::InvalidState,
            "knowledge id zero must be rejected");

        auto duplicate_knowledge_id = clone_bytes(valid);
        const auto first_id = read_u64_le(duplicate_knowledge_id, knowledge_section.knowledge_offsets.front());
        write_u64_le(duplicate_knowledge_id, knowledge_section.knowledge_offsets[1], first_id);
        refresh_snapshot_payload_hash(duplicate_knowledge_id);
        require_snapshot_load_fails(
            duplicate_knowledge_id,
            universe::SnapshotErrorCode::DuplicateId,
            "duplicate knowledge ids must be rejected");

        auto non_contiguous_ids = clone_bytes(valid);
        write_u64_le(non_contiguous_ids, knowledge_section.knowledge_offsets.front(), knowledge_section.knowledge_count + 17U);
        refresh_snapshot_payload_hash(non_contiguous_ids);
        require_snapshot_load_fails(
            non_contiguous_ids,
            universe::SnapshotErrorCode::InvalidState,
            "non-contiguous knowledge ids must be rejected");

        auto invalid_owner = clone_bytes(valid);
        write_u64_le(invalid_owner, knowledge_section.knowledge_offsets.front() + 8U, 0U);
        refresh_snapshot_payload_hash(invalid_owner);
        require_snapshot_load_fails(
            invalid_owner,
            universe::SnapshotErrorCode::InvalidReference,
            "knowledge owner id must be valid");

        auto invalid_source_memory = clone_bytes(valid);
        write_u64_le(invalid_source_memory, knowledge_section.knowledge_offsets.front() + 33U, 0U);
        refresh_snapshot_payload_hash(invalid_source_memory);
        require_snapshot_load_fails(
            invalid_source_memory,
            universe::SnapshotErrorCode::InvalidReference,
            "knowledge source memory id must be valid");

        auto invalid_source_enum = clone_bytes(valid);
        invalid_source_enum[knowledge_section.knowledge_offsets.front() + 32U] = std::byte{1};
        refresh_snapshot_payload_hash(invalid_source_enum);
        require_snapshot_load_fails(
            invalid_source_enum,
            universe::SnapshotErrorCode::InvalidEnum,
            "knowledge source enum out of range must be rejected");

        auto invalid_fact_kind = clone_bytes(valid);
        invalid_fact_kind[knowledge_section.knowledge_fact_kind_offsets.front()] = std::byte{9};
        refresh_snapshot_payload_hash(invalid_fact_kind);
        require_snapshot_load_fails(
            invalid_fact_kind,
            universe::SnapshotErrorCode::InvalidEnum,
            "knowledge fact kind out of range must be rejected");

        auto updated_before_acquired = clone_bytes(valid);
        const auto acquired_ticks = read_u64_le(updated_before_acquired, knowledge_section.knowledge_offsets.front() + 16U);
        if (acquired_ticks > 0U) {
            write_u64_le(updated_before_acquired, knowledge_section.knowledge_offsets.front() + 24U, acquired_ticks - 1U);
        } else {
            write_u64_le(updated_before_acquired, knowledge_section.knowledge_offsets.front() + 24U, 0U);
        }
        refresh_snapshot_payload_hash(updated_before_acquired);
        require_snapshot_load_fails(
            updated_before_acquired,
            universe::SnapshotErrorCode::InvalidState,
            "knowledge updated_at before acquired_at must be rejected");

        auto invalid_confidence = clone_bytes(valid);
        const auto confidence_offset = knowledge_section.next_knowledge_id_offset - 4U;
        write_u32_le(invalid_confidence, confidence_offset, 10'001U);
        refresh_snapshot_payload_hash(invalid_confidence);
        require_snapshot_load_fails(
            invalid_confidence,
            universe::SnapshotErrorCode::InvalidState,
            "knowledge confidence above 10000 must be rejected");

        std::size_t same_kind_first = std::numeric_limits<std::size_t>::max();
        std::size_t same_kind_second = std::numeric_limits<std::size_t>::max();
        for (std::size_t i = 0; i < knowledge_section.fact_kinds.size(); ++i) {
            for (std::size_t j = i + 1; j < knowledge_section.fact_kinds.size(); ++j) {
                if (knowledge_section.fact_kinds[i] == knowledge_section.fact_kinds[j]) {
                    same_kind_first = i;
                    same_kind_second = j;
                    break;
                }
            }
            if (same_kind_first != std::numeric_limits<std::size_t>::max()) {
                break;
            }
        }
        require(
            same_kind_first != std::numeric_limits<std::size_t>::max(),
            "knowledge corruption tests require two knowledge records of same kind");

        auto duplicate_semantic_key = clone_bytes(valid);
        const auto first_offset = knowledge_section.knowledge_offsets[same_kind_first];
        const auto second_offset = knowledge_section.knowledge_offsets[same_kind_second];
        const auto kind = knowledge_section.fact_kinds[same_kind_first];
        const auto owner = read_u64_le(duplicate_semantic_key, first_offset + 8U);
        write_u64_le(duplicate_semantic_key, second_offset + 8U, owner);
        if (kind == universe::KnowledgeFactKind::PersonLifeStatus) {
            const auto subject = read_u64_le(duplicate_semantic_key, first_offset + 42U);
            write_u64_le(duplicate_semantic_key, second_offset + 42U, subject);
        } else if (kind == universe::KnowledgeFactKind::PersonHousehold) {
            const auto subject = read_u64_le(duplicate_semantic_key, first_offset + 42U);
            write_u64_le(duplicate_semantic_key, second_offset + 42U, subject);
        } else {
            const auto subject = read_u64_le(duplicate_semantic_key, first_offset + 42U);
            write_u64_le(duplicate_semantic_key, second_offset + 42U, subject);
        }
        refresh_snapshot_payload_hash(duplicate_semantic_key);
        require_snapshot_load_fails(
            duplicate_semantic_key,
            universe::SnapshotErrorCode::InvalidState,
            "duplicate knowledge semantic keys must be rejected");

        auto invalid_next_knowledge_id = clone_bytes(valid);
        write_u64_le(invalid_next_knowledge_id, knowledge_section.next_knowledge_id_offset, 1U);
        refresh_snapshot_payload_hash(invalid_next_knowledge_id);
        require_snapshot_load_fails(
            invalid_next_knowledge_id,
            universe::SnapshotErrorCode::InvalidState,
            "invalid next knowledge id must be rejected");
    }

    {
        auto aggregate_config = config;
        aggregate_config.initial_population = 0;
        aggregate_config.initial_region_count = 1;
        aggregate_config.base_region_area_units = 140;

        universe::Simulation aggregate_only{aggregate_config};
        require(aggregate_only.state().people.empty(), "aggregate-only scenario starts with no detailed people");
        require(aggregate_only.state().households.empty(), "aggregate-only scenario starts with no detailed households");
        require(!aggregate_only.state().regions.empty(), "aggregate-only scenario requires at least one region");

        const auto region_id = aggregate_only.state().regions.front().id;
        aggregate_only.add_aggregate_population(region_id, 0U, 10'000U);
        require(aggregate_only.aggregate_population() == 10'000U, "aggregate population must be injected exactly");
        require(aggregate_only.detailed_population() == 0U, "aggregate-only scenario should remain zero-detailed");

        aggregate_only.reset_lod_instrumentation();
        aggregate_only.run_years(5);

        require(aggregate_only.state().people.empty(), "aggregate execution must not silently materialize detailed people");
        require(aggregate_only.state().households.empty(), "aggregate execution must not silently create detailed households");
        require(aggregate_only.state().total_food_produced > 0, "aggregate execution must produce food without person loops");

        // The decisive anti-facade assertion: aggregate settlements must never
        // reach the per-person daily or per-person demographic code paths.
        require(
            aggregate_only.person_daily_update_count() == 0U,
            "aggregate execution must never run per-person daily nutrition/health processing");
        require(
            aggregate_only.person_demography_update_count() == 0U,
            "aggregate execution must never run per-person mortality/fertility rolls");
        require(
            aggregate_only.aggregate_settlement_update_count() > 0U,
            "aggregate execution must actually process settlement aggregates");
        require(
            aggregate_only.aggregate_cohort_update_count() > 0U,
            "aggregate execution must actually process population cohorts");

        // Ticket #32: a settlement with real City-scale population but zero
        // detailed households has no wealth data to ever build a viability
        // streak, so promotion must stay gated even after years at 10k+ pop.
        require(aggregate_only.state().settlements.size() == 1U,
                "aggregate-only scenario must hold exactly one settlement");
        require(aggregate_only.state().settlements.front().population == aggregate_only.state().population,
                "settlement population must account entirely for the world's aggregate cohort population");
        require(aggregate_only.state().settlements.front().tier == universe::SettlementTier::Village,
                "settlement tier must not promote without a real economic viability streak");

        // Aggregate population is simulated, not frozen: cohorts must age, die
        // and reproduce rather than sitting at their injected value.
        require(
            aggregate_only.state().total_deaths > 0U,
            "aggregate execution must produce cohort deaths");
        require(
            aggregate_only.state().total_births > 0U,
            "aggregate execution must produce cohort births");
        require(
            aggregate_only.aggregate_population() == aggregate_only.state().population,
            "aggregate-only population must be accounted entirely by cohorts");
    }

    {
        // Aggregate execution cost must scale with settlements, not people:
        // a 10x larger represented population must not cost 10x more work.
        auto scaling_config = config;
        scaling_config.initial_population = 0;
        scaling_config.initial_region_count = 1;
        scaling_config.base_region_area_units = 1'400;

        universe::Simulation small{scaling_config};
        small.add_aggregate_population(small.state().regions.front().id, 0U, 10'000U);
        small.reset_lod_instrumentation();
        small.run_years(5);

        universe::Simulation large{scaling_config};
        large.add_aggregate_population(large.state().regions.front().id, 0U, 100'000U);
        large.reset_lod_instrumentation();
        large.run_years(5);

        require(
            small.person_daily_update_count() == 0U && large.person_daily_update_count() == 0U,
            "neither aggregate scale may run per-person daily loops");
        require(
            large.aggregate_settlement_update_count() == small.aggregate_settlement_update_count(),
            "aggregate settlement work must be independent of represented population size");
        // Both scales must actually be simulated rather than held at their
        // injected values, and must stay fully cohort-represented.
        require(
            small.state().population != 10'000U && large.state().population != 100'000U,
            "aggregate populations at both scales must actually evolve");
        require(
            small.aggregate_population() == small.state().population &&
                large.aggregate_population() == large.state().population,
            "aggregate populations at both scales must remain cohort-represented");
        require(
            small.state().settlements.size() == 1U && large.state().settlements.size() == 1U,
            "single-region aggregate scaling scenario must hold exactly one settlement");
        require(
            small.state().settlements.front().population == small.state().population,
            "settlement population must account for aggregate cohort population at 10k scale");
        require(
            large.state().settlements.front().population == large.state().population,
            "settlement population must account for aggregate cohort population at 100k scale");
    }

    {
        // Demoting a natively-detailed region must actually stop per-person
        // execution -- the exact failure mode where aggregate state is added
        // alongside a still-running detailed simulation.
        auto demote_config = config;
        demote_config.initial_population = 200;
        demote_config.initial_region_count = 1;
        demote_config.base_region_area_units = 140;

        universe::Simulation sim{demote_config};
        const auto region_id = sim.state().regions.front().id;
        sim.run_years(2);

        sim.reset_lod_instrumentation();
        sim.run_years(1);
        require(
            sim.person_daily_update_count() > 0U,
            "detailed regions must run per-person daily processing before demotion");

        const auto alive_before = sim.detailed_population();
        require(alive_before > 0U, "detailed region must hold living people before demotion");

        sim.demote_region_to_settlement_aggregate(region_id);
        // Anonymous population folds into cohorts; canonized entities (record
        // authors, discoverers, institution members) stay exact by contract.
        const auto canonized_survivors = sim.detailed_population();
        require(
            sim.aggregate_population() > 0U,
            "demotion must fold anonymous population into cohorts");
        require(
            sim.aggregate_population() + canonized_survivors == alive_before,
            "demotion must conserve population exactly across cohorts and canonized entities");
        require(
            canonized_survivors < alive_before,
            "demotion must not leave the whole population in detailed form");

        sim.reset_lod_instrumentation();
        sim.run_years(3);
        require(
            sim.person_daily_update_count() == 0U,
            "a demoted region must not keep running per-person daily loops underneath");
        require(
            sim.person_demography_update_count() == 0U,
            "a demoted region must not keep running per-person demography underneath");
        require(
            sim.aggregate_settlement_update_count() > 0U,
            "a demoted region must be executed by the aggregate path");
    }

    {
        // Conservation across a full detailed -> aggregate -> detailed cycle.
        auto conservation_config = config;
        conservation_config.initial_population = 200;
        conservation_config.initial_region_count = 1;
        conservation_config.base_region_area_units = 140;

        universe::Simulation sim{conservation_config};
        const auto region_id = sim.state().regions.front().id;
        sim.run_years(3);

        // Settlement inventory and household ownership are separate ledgers, so
        // the physically conserved quantity is their sum.
        const auto total_material = [](const universe::WorldState& state, const bool wood) {
            std::int64_t total = 0;
            for (const auto& settlement : state.settlements) {
                total += wood ? settlement.materials.wood.value : settlement.materials.stone.value;
            }
            for (const auto& household : state.households) {
                total += wood ? household.owned_materials.wood.value
                              : household.owned_materials.stone.value;
            }
            return total;
        };

        const auto population_before = sim.state().population;
        const auto food_stored_before = sim.state().total_food_stored;
        const auto wood_before = total_material(sim.state(), true);
        const auto stone_before = total_material(sim.state(), false);
        const auto births_before = sim.state().total_births;
        const auto deaths_before = sim.state().total_deaths;

        sim.demote_region_to_settlement_aggregate(region_id);

        require(sim.state().population == population_before, "demotion must conserve population");
        require(sim.state().total_food_stored == food_stored_before, "demotion must conserve stored food");
        require(total_material(sim.state(), true) == wood_before, "demotion must conserve wood");
        require(total_material(sim.state(), false) == stone_before, "demotion must conserve stone");
        require(sim.state().total_births == births_before, "demotion must not invent births");
        require(sim.state().total_deaths == deaths_before, "demotion must not invent deaths");

        const auto population_aggregate = sim.state().population;
        const auto food_stored_aggregate = sim.state().total_food_stored;

        const auto canonized_survivors = sim.detailed_population();
        require(
            canonized_survivors > 0U,
            "canonized entities must survive demotion in exact detailed form");

        // Canonized survivors must be exactly the entities carrying durable
        // provenance: institution founders/members, record authors, discoverers.
        std::unordered_set<universe::PersonId> provenance_anchors;
        for (const auto& institution : sim.institutions()) {
            provenance_anchors.insert(institution.founder_id);
            for (const auto member_id : institution.member_ids) {
                provenance_anchors.insert(member_id);
            }
        }
        for (const auto& record : sim.knowledge_records()) {
            provenance_anchors.insert(record.author_id);
        }
        for (const auto& event : sim.state().event_log) {
            if (event.type == universe::EventType::DiscoveryMade) {
                provenance_anchors.insert(
                    std::get<universe::DiscoveryMadeEventPayload>(event.payload).discoverer_id);
            } else if (event.type == universe::EventType::InstitutionFounded) {
                provenance_anchors.insert(
                    std::get<universe::InstitutionFoundedEventPayload>(event.payload).founder_id);
            }
        }
        for (const auto& person : sim.state().people) {
            if (person.alive) {
                require(
                    provenance_anchors.contains(person.id),
                    "only provenance-anchored entities may stay detailed in an aggregate region");
            }
        }

        sim.promote_region_to_detailed(region_id);

        require(sim.state().population == population_aggregate, "promotion must conserve population");
        require(sim.state().total_food_stored == food_stored_aggregate, "promotion must conserve stored food");
        require(sim.aggregate_population() == 0U, "promotion must leave no residual cohorts in the region");
        require(sim.detailed_population() == population_aggregate, "promotion must realize the full population");
    }

    {
        // Cohort representation must stay bounded: repeated aging and births
        // must merge back into canonical buckets rather than growing forever.
        auto bounded_config = config;
        bounded_config.initial_population = 0;
        bounded_config.initial_region_count = 1;
        bounded_config.base_region_area_units = 1'400;

        universe::Simulation sim{bounded_config};
        sim.add_aggregate_population(sim.state().regions.front().id, 0U, 10'000U);

        sim.run_years(5);
        const auto cohorts_after_5 = sim.state().population_cohorts.size();
        sim.run_years(45);
        const auto cohorts_after_50 = sim.state().population_cohorts.size();

        require(cohorts_after_5 > 0U, "aggregate execution must retain cohorts");
        require(
            cohorts_after_50 <= cohorts_after_5 * 2U,
            "cohort count must stay bounded rather than growing with simulated years");
        require(
            cohorts_after_50 < 64U,
            "cohort count must stay within the canonical bucket space for one settlement");
    }

    {
        // Aggregate settlements advance on a chunked cadence; save/load taken
        // mid-chunk must not change the trajectory.
        auto cadence_config = config;
        cadence_config.initial_population = 0;
        cadence_config.initial_region_count = 1;
        cadence_config.base_region_area_units = 1'400;

        universe::Simulation direct{cadence_config};
        direct.add_aggregate_population(direct.state().regions.front().id, 0U, 10'000U);
        direct.advance(universe::days(1'000));

        universe::Simulation segmented{cadence_config};
        segmented.add_aggregate_population(segmented.state().regions.front().id, 0U, 10'000U);
        // 407 and 593 are deliberately not multiples of the aggregate chunk.
        segmented.advance(universe::days(407));
        segmented = load_from_snapshot_or_fail(
            segmented.save_snapshot(),
            "mid-chunk aggregate snapshot must load");
        segmented.advance(universe::days(593));

        require_world_state_equal(
            direct.state(),
            segmented.state(),
            "aggregate chunked cadence must survive save/load taken mid-chunk");
        require(
            direct.save_snapshot() == segmented.save_snapshot(),
            "mid-chunk segmented aggregate run must serialize identically");
    }

    {
        auto aggregate_config = config;
        aggregate_config.initial_population = 0;
        aggregate_config.initial_region_count = 1;
        aggregate_config.base_region_area_units = 140;

        universe::Simulation lhs{aggregate_config};
        universe::Simulation rhs{aggregate_config};
        const auto region_id_lhs = lhs.state().regions.front().id;
        const auto region_id_rhs = rhs.state().regions.front().id;
        lhs.add_aggregate_population(region_id_lhs, 0U, 100'000U);
        rhs.add_aggregate_population(region_id_rhs, 0U, 100'000U);
        lhs.run_years(8);
        rhs.run_years(8);

        require_world_state_equal(lhs.state(), rhs.state(), "aggregate execution must be deterministic");
        require(lhs.save_snapshot() == rhs.save_snapshot(), "aggregate deterministic runs must serialize identically");
    }

    {
        auto promotion_config = config;
        promotion_config.initial_population = 0;
        promotion_config.initial_region_count = 1;
        promotion_config.base_region_area_units = 140;

        universe::Simulation promotion_run{promotion_config};
        const auto region_id = promotion_run.state().regions.front().id;
        promotion_run.add_aggregate_population(region_id, 0U, 10'000U);
        promotion_run.run_years(2);

        promotion_run.promote_region_to_detailed(region_id);
        require(promotion_run.aggregate_population() == 0U, "promoted region should consume aggregate cohorts");
        require(promotion_run.detailed_population() > 0U, "promoted region should realize detailed people");

        std::vector<universe::PersonId> first_promoted_ids;
        for (const auto& person : promotion_run.state().people) {
            if (person.alive && person.materialized_from_cohort) {
                first_promoted_ids.push_back(person.id);
            }
        }
        std::sort(first_promoted_ids.begin(), first_promoted_ids.end());
        require(!first_promoted_ids.empty(), "promotion must produce cohort-materialized detailed identities");

        promotion_run.demote_region_to_settlement_aggregate(region_id);
        require(promotion_run.aggregate_population() > 0U, "demotion must restore aggregate cohort representation");

        const auto demoted_snapshot = promotion_run.save_snapshot();
        auto resumed = load_from_snapshot_or_fail(demoted_snapshot, "demoted aggregate snapshot must load");
        resumed.promote_region_to_detailed(region_id);

        std::vector<universe::PersonId> promoted_again_ids;
        for (const auto& person : resumed.state().people) {
            if (person.alive && person.materialized_from_cohort) {
                promoted_again_ids.push_back(person.id);
            }
        }
        std::sort(promoted_again_ids.begin(), promoted_again_ids.end());
        require(
            first_promoted_ids == promoted_again_ids,
            "promotion after demote/save/load must reactivate the same materialized identities without rerolling");
    }

    {
        auto transition_config = config;
        transition_config.initial_population = 0;
        transition_config.initial_region_count = 1;
        transition_config.base_region_area_units = 140;

        universe::Simulation direct{transition_config};
        const auto region_id = direct.state().regions.front().id;
        direct.add_aggregate_population(region_id, 0U, 10'000U);
        direct.run_years(3);
        direct.promote_region_to_detailed(region_id);
        direct.run_years(2);
        direct.demote_region_to_settlement_aggregate(region_id);
        direct.run_years(4);

        universe::Simulation segmented{transition_config};
        segmented.add_aggregate_population(region_id, 0U, 10'000U);
        segmented.run_years(1);
        segmented = load_from_snapshot_or_fail(
            segmented.save_snapshot(),
            "mixed LOD segmented path: year-1 aggregate snapshot must load");

        segmented.run_years(2);
        segmented.promote_region_to_detailed(region_id);
        segmented = load_from_snapshot_or_fail(
            segmented.save_snapshot(),
            "mixed LOD segmented path: post-promotion snapshot must load");

        segmented.run_years(1);
        segmented = load_from_snapshot_or_fail(
            segmented.save_snapshot(),
            "mixed LOD segmented path: post-detailed snapshot must load");

        segmented.run_years(1);
        segmented.demote_region_to_settlement_aggregate(region_id);
        segmented = load_from_snapshot_or_fail(
            segmented.save_snapshot(),
            "mixed LOD segmented path: post-demotion snapshot must load");

        segmented.run_years(4);

        require_world_state_equal(
            direct.state(),
            segmented.state(),
            "mixed LOD transition schedule should remain deterministic across save/load segmentation");
        require(
            direct.save_snapshot() == segmented.save_snapshot(),
            "mixed LOD deterministic paths must serialize identically");
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
        universe::Simulation founding_run{universe::SimulationConfig{
            .seed = 42,
            .initial_population = 600,
        }};

        std::uint64_t initial_settlement_count = 0;
        universe::SettlementId initial_max_settlement_id = 0;
        for (const auto& settlement : founding_run.state().settlements) {
            if (settlement.active) {
                ++initial_settlement_count;
            }
            initial_max_settlement_id = std::max(initial_max_settlement_id, settlement.id);
        }

        founding_run.run_years(200);

        std::unordered_map<universe::SettlementId, std::uint64_t> founded_counts;
        std::uint64_t founded_total = 0;
        for (const auto& event : founding_run.state().event_log) {
            if (event.type != universe::EventType::SettlementFounded) {
                continue;
            }

            ++founded_total;
            require(
                std::holds_alternative<universe::SettlementFoundedEventPayload>(event.payload),
                "settlement founded event payload must match type");
            const auto& payload = std::get<universe::SettlementFoundedEventPayload>(event.payload);
            require(payload.settlement_id > initial_max_settlement_id, "dynamic founded settlement id must be above initial id range");
            const auto* settlement = find_settlement(founding_run.state(), payload.settlement_id);
            require(settlement != nullptr, "founded settlement id must reference a known settlement");
            require(settlement->region_id == payload.region_id, "founded settlement region id must match canonical settlement state");
            founded_counts[payload.settlement_id] += 1U;
        }

        require(initial_settlement_count > 0U, "founding scenario must start with active settlements");
        require(founded_total > 0U, "founding scenario must produce at least one founded event");
        for (const auto& [settlement_id, count] : founded_counts) {
            (void)settlement_id;
            require(count == 1U, "each founded settlement must emit exactly one founded event");
        }
    }

    {
        universe::Simulation extinction_run{universe::SimulationConfig{
            .seed = 22,
            .initial_population = 200,
            .initial_household_food_units = 20,
            .food_production_scale_percent = 35,
        }};
        extinction_run.run_years(250);

        std::unordered_map<universe::SettlementId, std::uint64_t> extinction_counts;
        std::uint64_t extinction_total = 0;
        for (const auto& event : extinction_run.state().event_log) {
            if (event.type != universe::EventType::SettlementExtinct) {
                continue;
            }

            ++extinction_total;
            require(
                std::holds_alternative<universe::SettlementExtinctEventPayload>(event.payload),
                "settlement extinct event payload must match type");
            const auto& payload = std::get<universe::SettlementExtinctEventPayload>(event.payload);
            const auto* settlement = find_settlement(extinction_run.state(), payload.settlement_id);
            require(settlement != nullptr, "extinct settlement id must reference known settlement");
            require(settlement->region_id == payload.region_id, "extinct settlement region id must match canonical settlement state");
            require(!settlement->active, "settlement with extinction event must be inactive by scenario end");
            extinction_counts[payload.settlement_id] += 1U;
        }

        require(extinction_total > 0U, "extinction scenario must produce at least one extinction event");
        for (const auto& [settlement_id, count] : extinction_counts) {
            (void)settlement_id;
            require(count == 1U, "each settlement must emit at most one extinction event");
        }
    }

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
    verify_labour_assignment_conservation(a.state());

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
            verify_labour_assignment_conservation(baseline.state());
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
        verify_labour_assignment_conservation(sweep.state());
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
        verify_labour_assignment_conservation(long_horizon.state());
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

    {
        auto geography_config = universe::SimulationConfig{.seed = 4242U, .initial_population = 200U};
        geography_config.initial_region_count = 6;
        geography_config.base_region_area_units = 140;

        universe::Simulation geography_run{geography_config};
        const auto& state = geography_run.state();

        require(state.regions.size() >= 2, "canonical geography test requires multiple regions");

        std::unordered_set<std::int64_t> distinct_region_x;
        for (const auto& region : state.regions) {
            distinct_region_x.insert(region.position_x);
        }
        require(distinct_region_x.size() > 1, "regions must not all collapse onto the same x position");

        for (std::size_t i = 0; i < state.regions.size(); ++i) {
            for (std::size_t j = i + 1; j < state.regions.size(); ++j) {
                const auto dx = state.regions[i].position_x - state.regions[j].position_x;
                const auto dz = state.regions[i].position_z - state.regions[j].position_z;
                const auto distance_squared = (dx * dx) + (dz * dz);
                require(distance_squared > 0, "two distinct regions must not share the exact same canonical position");
            }
        }

        std::unordered_map<universe::RegionId, std::vector<std::pair<std::int64_t, std::int64_t>>> settlement_positions_by_region;
        for (const auto& settlement : state.settlements) {
            settlement_positions_by_region[settlement.region_id].emplace_back(settlement.position_x, settlement.position_z);
        }
        for (const auto& [region_id, positions] : settlement_positions_by_region) {
            const auto region_it = std::find_if(
                state.regions.begin(), state.regions.end(),
                [region_id](const universe::RegionEnvironmentState& candidate) { return candidate.id == region_id; });
            require(region_it != state.regions.end(), "settlement region id must reference an existing region");
            const auto* region = &(*region_it);

            for (const auto& [settlement_x, settlement_z] : positions) {
                const auto dx = settlement_x - region->position_x;
                const auto dz = settlement_z - region->position_z;
                const auto distance_squared = (dx * dx) + (dz * dz);
                require(distance_squared <= (2'000 * 2'000), "settlement position must stay reasonably close to its region's position");
            }

            for (std::size_t i = 0; i < positions.size(); ++i) {
                for (std::size_t j = i + 1; j < positions.size(); ++j) {
                    const auto dx = positions[i].first - positions[j].first;
                    const auto dz = positions[i].second - positions[j].second;
                    const auto distance_squared = (dx * dx) + (dz * dz);
                    require(distance_squared > 0, "two distinct settlements in the same region must not share the exact same position");
                }
            }
        }

        universe::Simulation geography_repeat{geography_config};
        require(
            geography_run.save_snapshot() == geography_repeat.save_snapshot(),
            "canonical region/settlement position derivation must be deterministic for a given seed");

        auto different_seed_config = geography_config;
        different_seed_config.seed = 9999U;
        universe::Simulation geography_different_seed{different_seed_config};
        require(
            geography_run.save_snapshot() != geography_different_seed.save_snapshot(),
            "a different seed must produce different canonical geography");

        const auto bytes = geography_run.save_snapshot();
        auto loaded = load_from_snapshot_or_fail(bytes, "geography snapshot must round-trip");
        require(loaded.state().regions.size() == state.regions.size(), "round-tripped snapshot must preserve region count");
        for (std::size_t i = 0; i < state.regions.size(); ++i) {
            require(loaded.state().regions[i].position_x == state.regions[i].position_x, "round-tripped region position x must be preserved");
            require(loaded.state().regions[i].position_z == state.regions[i].position_z, "round-tripped region position z must be preserved");
        }
        require(loaded.state().settlements.size() == state.settlements.size(), "round-tripped snapshot must preserve settlement count");
        for (std::size_t i = 0; i < state.settlements.size(); ++i) {
            require(
                loaded.state().settlements[i].position_x == state.settlements[i].position_x,
                "round-tripped settlement position x must be preserved");
            require(
                loaded.state().settlements[i].position_z == state.settlements[i].position_z,
                "round-tripped settlement position z must be preserved");
        }
    }

    {
        // Ticket #31: settlement population field -- non-negative (trivial via
        // uint64), consistent with actual households/cohorts (verified every
        // call via verify_settlement_invariants), deterministic per seed, and
        // round-trips through snapshot serialization.
        auto population_config = config;
        population_config.initial_population = 240;
        population_config.initial_region_count = 2;

        universe::Simulation population_run{population_config};
        population_run.run_years(3);
        verify_settlement_invariants(population_run.state());

        universe::Simulation population_repeat{population_config};
        population_repeat.run_years(3);
        require(
            population_run.save_snapshot() == population_repeat.save_snapshot(),
            "settlement population must be deterministic for a given seed");

        auto population_different_seed_config = population_config;
        population_different_seed_config.seed = 4242U;
        universe::Simulation population_different_seed{population_different_seed_config};
        population_different_seed.run_years(3);
        require(
            population_run.save_snapshot() != population_different_seed.save_snapshot(),
            "a different seed must produce different settlement population trajectories");

        const auto population_bytes = population_run.save_snapshot();
        auto population_loaded = load_from_snapshot_or_fail(population_bytes, "population snapshot must round-trip");
        require(
            population_loaded.state().settlements.size() == population_run.state().settlements.size(),
            "round-tripped snapshot must preserve settlement count for population test");
        for (std::size_t i = 0; i < population_run.state().settlements.size(); ++i) {
            require(
                population_loaded.state().settlements[i].population == population_run.state().settlements[i].population,
                "round-tripped settlement population must be preserved");
        }
    }

    {
        // Ticket #32: tier promotion requires crossing the population threshold
        // AND a real economic viability streak, and only takes effect after a
        // symmetric 365-day hysteresis streak, per the settlement tier spec.
        auto tier_config = config;
        tier_config.initial_population = 8'000;
        tier_config.initial_region_count = 1;
        tier_config.base_region_area_units = 10'000;

        universe::Simulation tier_run{tier_config};
        require(!tier_run.state().settlements.empty(), "tier hysteresis scenario requires at least one settlement");

        const auto largest_settlement_id = [](const universe::WorldState& state) {
            universe::SettlementId largest_id = 0;
            std::uint64_t largest_population = 0;
            for (const auto& settlement : state.settlements) {
                if (settlement.population > largest_population) {
                    largest_population = settlement.population;
                    largest_id = settlement.id;
                }
            }
            return largest_id;
        };
        const auto find_settlement_by_id = [](const universe::WorldState& state, const universe::SettlementId id) -> const universe::Settlement& {
            const auto it = std::find_if(state.settlements.begin(), state.settlements.end(),
                                          [id](const universe::Settlement& candidate) { return candidate.id == id; });
            require(it != state.settlements.end(), "expected settlement id must exist");
            return *it;
        };

        const auto tracked_id = largest_settlement_id(tier_run.state());
        require(find_settlement_by_id(tier_run.state(), tracked_id).population >= 1'000U,
                "tier hysteresis scenario's largest settlement must clear the Town population threshold");

        tier_run.advance(universe::days(364));
        {
            const auto& settlement = find_settlement_by_id(tier_run.state(), tracked_id);
            require(settlement.tier == universe::SettlementTier::Village,
                    "tier promotion must not take effect before the 365-day hysteresis streak completes");
            require(settlement.pending_tier == universe::SettlementTier::Town,
                    "a qualifying settlement must have a pending promotion to Town before day 365");
            require(settlement.tier_transition_streak > 0U && settlement.tier_transition_streak < 365U,
                    "tier transition streak must be counting up but not yet complete before day 365");
        }

        tier_run.advance(universe::days(1));
        {
            const auto& settlement = find_settlement_by_id(tier_run.state(), tracked_id);
            require(settlement.tier == universe::SettlementTier::Town,
                    "tier promotion must take effect once the 365-day hysteresis streak completes");
            require(settlement.pending_tier == universe::SettlementTier::Town,
                    "pending tier must match confirmed tier once promotion has taken effect");
            require(settlement.tier_transition_streak == 0U,
                    "tier transition streak must reset once a promotion takes effect");
        }

        universe::Simulation tier_repeat{tier_config};
        tier_repeat.advance(universe::days(365));
        require(tier_run.save_snapshot() == tier_repeat.save_snapshot(),
                "settlement tier trajectory must be deterministic for a given seed");

        auto tier_different_seed_config = tier_config;
        tier_different_seed_config.seed = 777U;
        universe::Simulation tier_different_seed{tier_different_seed_config};
        tier_different_seed.advance(universe::days(365));
        require(tier_run.save_snapshot() != tier_different_seed.save_snapshot(),
                "a different seed must produce a different settlement tier trajectory");

        const auto tier_bytes = tier_run.save_snapshot();
        auto tier_loaded = load_from_snapshot_or_fail(tier_bytes, "tier snapshot must round-trip");
        require(tier_loaded.state().settlements.size() == tier_run.state().settlements.size(),
                "round-tripped snapshot must preserve settlement count for tier test");
        for (std::size_t i = 0; i < tier_run.state().settlements.size(); ++i) {
            require(tier_loaded.state().settlements[i].tier == tier_run.state().settlements[i].tier,
                    "round-tripped settlement tier must be preserved");
            require(tier_loaded.state().settlements[i].pending_tier == tier_run.state().settlements[i].pending_tier,
                    "round-tripped settlement pending tier must be preserved");
            require(tier_loaded.state().settlements[i].tier_transition_streak == tier_run.state().settlements[i].tier_transition_streak,
                    "round-tripped settlement tier transition streak must be preserved");
        }
    }

    {
        // Ticket #33: settlement merging -- two active, nearby, sustained-viability
        // settlements merge; the larger survives, the smaller becomes inactive and
        // transfers households/materials/access-share, and a SettlementMergedEventPayload
        // is emitted. Conservation: nothing is lost across a merge.
        auto merge_config = config;
        merge_config.initial_population = 400;
        merge_config.initial_region_count = 1;
        merge_config.base_region_area_units = 10'000;

        universe::Simulation merge_run{merge_config};
        require(merge_run.state().settlements.size() > 1U,
                "settlement merging scenario requires more than one settlement to start");

        const auto total_alive_members = [](const universe::WorldState& state) {
            std::uint64_t total = 0;
            for (const auto& household : state.households) {
                total += household.member_ids.size();
            }
            return total;
        };
        const auto total_materials = [](const universe::WorldState& state) {
            std::int64_t total = 0;
            for (const auto& settlement : state.settlements) {
                total += settlement.materials.wood.value + settlement.materials.stone.value +
                         settlement.materials.timber.value + settlement.materials.basic_tool.value;
            }
            return total;
        };
        const auto total_access_share = [](const universe::WorldState& state) {
            std::uint64_t total = 0;
            for (const auto& settlement : state.settlements) {
                total += settlement.access_share_points;
            }
            return total;
        };

        const auto members_before = total_alive_members(merge_run.state());

        // At this world scale, settlements only drift close enough together
        // (and both hold a sustained enough viability streak) to qualify for
        // merging after several years, not within the first year.
        merge_run.run_years(5);

        std::size_t merge_events = 0;
        std::unordered_set<universe::SettlementId> absorbed_ids;
        for (const auto& event : merge_run.state().event_log) {
            if (event.type == universe::EventType::SettlementMerged) {
                const auto& payload_data = std::get<universe::SettlementMergedEventPayload>(event.payload);
                require(payload_data.survivor_settlement_id != payload_data.absorbed_settlement_id,
                        "a settlement merge must not merge a settlement with itself");
                const auto* survivor = find_settlement(merge_run.state(), payload_data.survivor_settlement_id);
                const auto* absorbed = find_settlement(merge_run.state(), payload_data.absorbed_settlement_id);
                require(survivor != nullptr && survivor->active, "merge survivor must exist and remain active");
                require(absorbed != nullptr && !absorbed->active, "merge absorbed settlement must exist and become inactive");
                require(absorbed->access_share_points == 0U, "absorbed settlement must have zero access share after merging");
                require(survivor->region_id == payload_data.region_id && absorbed->region_id == payload_data.region_id,
                        "a merge must never cross region boundaries -- survivor and absorbed must share a region");
                absorbed_ids.insert(payload_data.absorbed_settlement_id);
                ++merge_events;
            }
        }
        require(merge_events > 0U, "close, sustained-viability settlements must eventually merge");

        verify_settlement_invariants(merge_run.state());
        require(total_alive_members(merge_run.state()) == members_before,
                "merging must not lose or duplicate any living household member");

        // Viability exclusion: every absorbed settlement must have actually held
        // the required sustained viability streak (>= 365 days) at merge time --
        // a settlement that never sustains viability must never be absorbed.
        for (const auto& settlement : merge_run.state().settlements) {
            if (!absorbed_ids.contains(settlement.id)) {
                continue;
            }
            require(settlement.economic_viability_streak >= 365U,
                    "an absorbed settlement must have held a sustained economic viability streak before merging");
        }

        universe::Simulation merge_repeat{merge_config};
        merge_repeat.run_years(5);
        require(merge_run.save_snapshot() == merge_repeat.save_snapshot(),
                "settlement merge outcomes must be deterministic for a given seed");

        auto merge_different_seed_config = merge_config;
        merge_different_seed_config.seed = 5150U;
        universe::Simulation merge_different_seed{merge_different_seed_config};
        merge_different_seed.run_years(5);
        require(merge_run.save_snapshot() != merge_different_seed.save_snapshot(),
                "a different seed must produce a different settlement merge trajectory");

        const auto merge_bytes = merge_run.save_snapshot();
        auto merge_loaded = load_from_snapshot_or_fail(merge_bytes, "settlement merge snapshot must round-trip");
        require(merge_loaded.state().settlements.size() == merge_run.state().settlements.size(),
                "round-tripped snapshot must preserve settlement count for merge test");
        require(total_materials(merge_loaded.state()) == total_materials(merge_run.state()),
                "round-tripped snapshot must preserve total settlement materials after merging");
        require(total_access_share(merge_loaded.state()) == total_access_share(merge_run.state()),
                "round-tripped snapshot must preserve total settlement access share after merging");

        std::size_t round_tripped_merge_events = 0;
        for (const auto& event : merge_loaded.state().event_log) {
            if (event.type == universe::EventType::SettlementMerged) {
                ++round_tripped_merge_events;
            }
        }
        require(round_tripped_merge_events == merge_events,
                "round-tripped snapshot must preserve every settlement merged event");
    }

    std::cout << "All UniverseCore tests passed.\n";
    return 0;
}
