// Behaviour tests for settlement structure (ADR 0002): the starting layout,
// pressure-driven fission and absorption of settlements that stay too small.
// They run at the headless Simulation seam: configure a world, advance time,
// read the resulting state.
#include "settlement_test_support.hpp"

#include "universe/simulation.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace {

using namespace settlement_test;

// Today's automatic layout, recorded before the option existed: the same seed
// must keep producing the same number of starting settlements.
void test_default_layout_is_unchanged() {
    struct Baseline {
        std::uint64_t seed;
        std::size_t active_settlements;
    };
    constexpr Baseline kBaselines[] = {{1U, 9U}, {7U, 10U}, {42U, 9U}};
    for (const auto& baseline : kBaselines) {
        universe::Simulation sim{default_layout_config(baseline.seed)};
        require(
            active_settlements(sim.state()).size() == baseline.active_settlements,
            "the default starting layout must not change when the option is unset");
        require_every_household_in_one_valid_settlement(sim.state());
    }
}

void test_single_settlement_start_holds_every_household() {
    for (const std::uint64_t seed : {1U, 7U, 42U}) {
        universe::Simulation sim{single_settlement_config(seed)};
        const auto active = active_settlements(sim.state());
        require(sim.state().regions.size() == 1U, "the single-settlement start uses exactly one region");
        require(active.size() == 1U, "the single-settlement start must begin with exactly one active settlement");
        require(!sim.state().households.empty(), "the single-settlement world must have households");
        require_every_household_in_one_valid_settlement(sim.state());
        require(
            active.front()->population == sim.detailed_population(),
            "the lone settlement must hold the whole starting population");
    }
}

void test_requested_settlement_count_is_honoured_and_capped() {
    auto two = single_settlement_config(42);
    two.initial_settlements_per_region = 2;
    universe::Simulation sim_two{two};
    require(active_settlements(sim_two.state()).size() == 2U, "a requested count of two must start two settlements");
    require_every_household_in_one_valid_settlement(sim_two.state());

    auto absurd = single_settlement_config(42);
    absurd.initial_settlements_per_region = 10'000;
    universe::Simulation sim_absurd{absurd};
    const auto settlements = active_settlements(sim_absurd.state());
    require(settlements.size() <= sim_absurd.state().households.size(), "a request above the household count is capped at it");
    require_every_household_in_one_valid_settlement(sim_absurd.state());
}

void test_single_settlement_world_continues_identically_after_reload() {
    universe::Simulation uninterrupted{single_settlement_config(42)};
    universe::Simulation source{single_settlement_config(42)};
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(source.save_snapshot(), restored).ok, "a single-settlement snapshot must load");

    uninterrupted.run_years(3);
    restored.run_years(3);
    require(
        uninterrupted.save_snapshot() == restored.save_snapshot(),
        "a reloaded single-settlement world must continue identically");
}

struct HouseholdShortage {
    std::uint64_t households{0};
    std::uint64_t short_households{0};
};

HouseholdShortage shortage_in(const universe::WorldState& state, const universe::Settlement& settlement) {
    HouseholdShortage shortage;
    for (const auto& household : state.households) {
        if (household.settlement_id != settlement.id || household.region_id != settlement.region_id || household.member_ids.empty()) {
            continue;
        }
        ++shortage.households;
        if (household.resources.consecutive_shortage_days > 0U) {
            ++shortage.short_households;
        }
    }
    return shortage;
}

// A day is strained for a settlement when at least a third of its households
// went short of food that day; the counter holds the current unbroken run.
void test_food_strain_days_follow_household_shortage() {
    constexpr int kDaysChecked = 120;
    universe::Simulation sim{default_layout_config(42)};
    std::vector<std::uint32_t> previous(sim.state().settlements.size(), 0U);
    bool saw_strained_day = false;
    bool saw_unstrained_day = false;
    for (int day = 0; day < kDaysChecked; ++day) {
        sim.advance(universe::days(1));
        for (std::size_t i = 0; i < sim.state().settlements.size(); ++i) {
            const auto& settlement = sim.state().settlements[i];
            if (!settlement.active) {
                continue;
            }
            const auto shortage = shortage_in(sim.state(), settlement);
            const bool strained = shortage.households > 0U && (shortage.short_households * 3U) >= shortage.households;
            const auto expected = strained ? previous[i] + 1U : 0U;
            require(settlement.days_under_food_strain == expected, "strain days must count unbroken days with a third of households short");
            previous[i] = settlement.days_under_food_strain;
            saw_strained_day = saw_strained_day || strained;
            saw_unstrained_day = saw_unstrained_day || !strained;
        }
    }
    require(saw_strained_day && saw_unstrained_day, "the world must exercise both a strained and an unstrained day");
}

// ADR 0002: a split leaves the daughter at least this large, and the source at
// least this large.
constexpr std::uint64_t kMinimumDaughterPopulation = 20U;
constexpr std::uint64_t kMinimumSourcePopulationAfterSplit = 40U;
// Below this a settlement splits only under sustained food strain.
constexpr std::uint64_t kPopulationSplitThreshold = 120U;

struct Founding {
    universe::SettlementId daughter_id{};
    universe::SettlementId source_id{};
};

std::vector<Founding> foundings_since(const universe::WorldState& state, const std::size_t first_event_index) {
    std::vector<Founding> foundings;
    for (std::size_t i = first_event_index; i < state.event_log.size(); ++i) {
        const auto& event = state.event_log[i];
        if (event.type != universe::EventType::SettlementFounded) {
            continue;
        }
        const auto& payload = std::get<universe::SettlementFoundedEventPayload>(event.payload);
        if (payload.source_settlement_id.has_value()) {
            foundings.push_back(Founding{.daughter_id = payload.settlement_id, .source_id = *payload.source_settlement_id});
        }
    }
    return foundings;
}

const universe::Settlement& settlement_by_id(const universe::WorldState& state, const universe::SettlementId id) {
    for (const auto& settlement : state.settlements) {
        if (settlement.id == id) {
            return settlement;
        }
    }
    std::cerr << "FAILED: settlement " << id << " must exist\n";
    std::exit(1);
}

void test_overgrown_settlement_splits() {
    universe::Simulation sim{single_settlement_config(42)};
    require(sim.state().settlements.front().population >= kPopulationSplitThreshold, "the starting settlement must be over the split threshold");
    sim.run_years(1);
    require(active_settlements(sim.state()).size() >= 2U, "a settlement over the population threshold must split");
}

void test_healthy_settlement_below_the_threshold_does_not_split() {
    // 100 people with ample land never reach the threshold or sustained strain,
    // however much spare access share the settlement has.
    auto config = single_settlement_config(42);
    config.initial_population = 100;
    universe::Simulation sim{config};
    sim.run_years(2);

    require(
        settlement_by_id(sim.state(), sim.state().settlements.front().id).population < kPopulationSplitThreshold,
        "the world must stay under the split threshold for this check to apply");
    require(active_settlements(sim.state()).size() == 1U, "a healthy settlement under the threshold must not split");
    require(foundings_since(sim.state(), 0U).empty(), "no settlement may be founded by a healthy settlement under the threshold");
}

void test_no_split_leaves_a_side_below_its_minimum() {
    constexpr std::uint64_t kSeedCount = 16;
    constexpr int kYears = 6;
    std::uint64_t splits_checked = 0;
    for (std::uint64_t seed = 1; seed <= kSeedCount; ++seed) {
        universe::Simulation sim{single_settlement_config(seed)};
        for (int year = 0; year < kYears; ++year) {
            const auto events_before = sim.state().event_log.size();
            sim.run_years(1);
            for (const auto& founding : foundings_since(sim.state(), events_before)) {
                const auto& daughter = settlement_by_id(sim.state(), founding.daughter_id);
                const auto& source = settlement_by_id(sim.state(), founding.source_id);
                require(daughter.population >= kMinimumDaughterPopulation, "a new settlement must start with at least the minimum daughter population");
                require(source.population >= kMinimumSourcePopulationAfterSplit, "a source settlement must keep at least the minimum population");
                ++splits_checked;
            }
            // A split loses and duplicates nobody, and creates no land.
            require_population_accounted_for(sim);
            require_region_access_within_capacity(sim.state());
        }
    }
    require(splits_checked > 0U, "the scenario must exercise at least one split");
}

// ADR 0002: the absorbed side is below the viable size for this many years, the
// survivor sits within this distance of it.
constexpr std::int64_t kAbsorptionDistanceLimitUnits = 2'000;
constexpr std::uint32_t kDaysPerYear = 365U;

struct Absorption {
    universe::SettlementId survivor_id{};
    universe::SettlementId absorbed_id{};
};

std::vector<Absorption> absorptions_since(const universe::WorldState& state, const std::size_t first_event_index) {
    std::vector<Absorption> absorptions;
    for (std::size_t i = first_event_index; i < state.event_log.size(); ++i) {
        const auto& event = state.event_log[i];
        if (event.type != universe::EventType::SettlementMerged) {
            continue;
        }
        const auto& payload = std::get<universe::SettlementMergedEventPayload>(event.payload);
        absorptions.push_back(Absorption{.survivor_id = payload.survivor_settlement_id, .absorbed_id = payload.absorbed_settlement_id});
    }
    return absorptions;
}

struct SettlementBefore {
    std::uint32_t days_below_viable_size{0};
    std::vector<universe::HouseholdId> household_ids{};
};

std::unordered_map<universe::SettlementId, SettlementBefore> snapshot_before(const universe::WorldState& state) {
    std::unordered_map<universe::SettlementId, SettlementBefore> before;
    for (const auto& settlement : state.settlements) {
        before[settlement.id].days_below_viable_size = settlement.days_below_viable_size;
    }
    for (const auto& household : state.households) {
        before[household.settlement_id].household_ids.push_back(household.id);
    }
    return before;
}

std::unordered_map<universe::PersonId, universe::HouseholdId> household_of_living_people(const universe::WorldState& state) {
    std::unordered_map<universe::PersonId, universe::HouseholdId> households;
    for (const auto& person : state.people) {
        if (person.alive) {
            households[person.id] = person.household_id;
        }
    }
    return households;
}

// A reload taken right now must succeed and then continue exactly as the live
// world does. A failed load means something was stranded; a drifting
// continuation means hidden state (such as lookup indexes keyed by settlement)
// differs from what a reload rebuilds.
void require_reload_continues_identically(const universe::Simulation& sim) {
    constexpr std::uint64_t kContinuationYears = 3;
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto load_result = universe::Simulation::load_snapshot(sim.save_snapshot(), restored);
    if (!load_result.ok) {
        std::cerr << "load failed: " << load_result.message << '\n';
    }
    require(load_result.ok, "a snapshot taken right after an absorption must load");

    auto continuing = sim;
    continuing.run_years(kContinuationYears);
    restored.run_years(kContinuationYears);
    require(
        continuing.save_snapshot() == restored.save_snapshot(),
        "a reload right after an absorption must continue identically");
}

const universe::Household& household_by_id(const universe::WorldState& state, const universe::HouseholdId id) {
    for (const auto& household : state.households) {
        if (household.id == id) {
            return household;
        }
    }
    std::cerr << "FAILED: household " << id << " must exist\n";
    std::exit(1);
}

std::int64_t squared_distance_between(const universe::Settlement& a, const universe::Settlement& b) {
    const auto dx = a.position_x - b.position_x;
    const auto dz = a.position_z - b.position_z;
    return (dx * dx) + (dz * dz);
}

// A settlement must have stayed below the viable size for the full period, so
// nothing is absorbed in the first four years however small it starts.
void test_nothing_is_absorbed_before_five_years_below_viable_size() {
    for (const std::uint64_t seed : {1U, 7U, 42U}) {
        universe::Simulation sim{default_layout_config(seed)};
        bool started_with_a_small_settlement = false;
        for (const auto* settlement : active_settlements(sim.state())) {
            started_with_a_small_settlement = started_with_a_small_settlement || settlement->population < universe::kViableSettlementSize;
        }
        require(started_with_a_small_settlement, "the default layout must start with a settlement below the viable size");
        sim.run_years(4);
        require(absorptions_since(sim.state(), 0U).empty(), "no settlement may be absorbed before five years below the viable size");
    }
}

void test_settlements_below_viable_size_for_five_years_are_absorbed_into_a_viable_neighbour() {
    constexpr int kYears = 8;
    std::uint64_t absorptions_checked = 0;
    std::uint64_t people_followed = 0;
    for (const std::uint64_t seed : {1U, 7U, 42U}) {
        universe::Simulation sim{default_layout_config(seed)};
        for (int year = 0; year < kYears; ++year) {
            const auto events_before = sim.state().event_log.size();
            const auto before = snapshot_before(sim.state());
            const auto household_before = household_of_living_people(sim.state());
            const auto coin_before = total_coin(sim.state());
            sim.run_years(1);

            require(total_coin(sim.state()) == coin_before, "absorbing a settlement must not change total coin");
            const auto absorptions = absorptions_since(sim.state(), events_before);
            for (const auto& absorption : absorptions) {
                const auto& absorbed = settlement_by_id(sim.state(), absorption.absorbed_id);
                const auto& survivor = settlement_by_id(sim.state(), absorption.survivor_id);

                require(
                    before.at(absorption.absorbed_id).days_below_viable_size + kDaysPerYear >= universe::kAbsorptionBelowViableDays,
                    "an absorbed settlement must have stayed below the viable size for five years");
                require(!absorbed.active && survivor.active, "the absorbed settlement is inactive and the survivor stays active");
                require(survivor.population >= universe::kViableSettlementSize, "the survivor must be viable");
                require(survivor.region_id == absorbed.region_id, "absorption stays within one region");
                require(
                    squared_distance_between(survivor, absorbed) <= kAbsorptionDistanceLimitUnits * kAbsorptionDistanceLimitUnits,
                    "the survivor must lie within the distance limit");

                // Nothing is left behind in the absorbed settlement.
                require(absorbed.access_share_points == 0U, "an absorbed settlement keeps no access share");
                require(absorbed.commons_food.value == 0, "an absorbed settlement keeps no commons food");
                require(absorbed.coin == 0, "an absorbed settlement keeps no coin");
                require(
                    absorbed.materials.wood.value == 0 && absorbed.materials.stone.value == 0 &&
                        absorbed.materials.timber.value == 0 && absorbed.materials.basic_tool.value == 0,
                    "an absorbed settlement keeps no materials");
                for (const auto household_id : before.at(absorption.absorbed_id).household_ids) {
                    require(
                        household_by_id(sim.state(), household_id).settlement_id == survivor.id,
                        "every household of an absorbed settlement moves to the survivor");
                }

                // The people move with their households and keep who they are: anyone
                // alive before and after is still in the household they were in.
                std::unordered_set<universe::HouseholdId> absorbed_households(
                    before.at(absorption.absorbed_id).household_ids.begin(),
                    before.at(absorption.absorbed_id).household_ids.end());
                for (const auto& person : sim.state().people) {
                    const auto earlier = household_before.find(person.id);
                    if (!person.alive || earlier == household_before.end() || !absorbed_households.contains(earlier->second)) {
                        continue;
                    }
                    require(person.household_id == earlier->second, "a person must keep their household across an absorption");
                    ++people_followed;
                }
                ++absorptions_checked;
            }
            require_every_household_in_one_valid_settlement(sim.state());
            require_population_accounted_for(sim);
            require_region_access_within_capacity(sim.state());

            // A merge that strands stock in an inactive settlement makes the world
            // unloadable, and stale hidden state makes a reloaded run drift, so the
            // state right after an absorption must reload and continue identically.
            if (!absorptions.empty()) {
                require_reload_continues_identically(sim);
            }
        }
    }
    require(absorptions_checked > 0U, "the scenario must exercise at least one absorption");
    require(people_followed > 0U, "an absorption must move at least one living person with their household");
}

void require_absorbed_ledger_empty(const universe::WorldState& state, const universe::SettlementId absorbed_id) {
    for (const auto& ledger : state.settlement_aggregates) {
        if (ledger.settlement_id != absorbed_id) {
            continue;
        }
        require(
            ledger.household_count == 0U && ledger.food_inventory == 0 && ledger.labour_capacity == 0 &&
                ledger.wealth_points_total == 0 && ledger.productive_access_points == 0 &&
                ledger.required_food_year == 0 && ledger.unmet_food_year == 0,
            "an absorbed settlement must keep no aggregate ledger");
    }
}

// A settlement in a demoted region holds its people as population groups and its
// stock in an aggregate ledger. Absorbing it must carry that ledger over too,
// leaving nothing stranded, and the world must still reload and continue.
void test_absorption_in_a_demoted_region_moves_the_aggregate_ledger() {
    constexpr int kYears = 8;
    std::uint64_t absorptions_checked = 0;
    for (const std::uint64_t seed : {1U, 7U, 42U}) {
        universe::Simulation sim{default_layout_config(seed)};
        std::vector<universe::RegionId> region_ids;
        for (const auto& region : sim.state().regions) {
            region_ids.push_back(region.id);
        }
        for (const auto region_id : region_ids) {
            sim.demote_region_to_settlement_aggregate(region_id);
        }

        for (int year = 0; year < kYears; ++year) {
            const auto events_before = sim.state().event_log.size();
            const auto coin_before = total_coin(sim.state());
            sim.run_years(1);
            require(total_coin(sim.state()) == coin_before, "absorbing a demoted settlement must not change total coin");
            const auto absorptions = absorptions_since(sim.state(), events_before);
            for (const auto& absorption : absorptions) {
                require(settlement_by_id(sim.state(), absorption.absorbed_id).coin == 0, "an absorbed settlement keeps no coin");
                require_absorbed_ledger_empty(sim.state(), absorption.absorbed_id);
                ++absorptions_checked;
            }
            require_population_accounted_for(sim);
            require_region_access_within_capacity(sim.state());
            if (!absorptions.empty()) {
                require_reload_continues_identically(sim);
            }
        }
    }
    require(absorptions_checked > 0U, "the demoted scenario must exercise at least one absorption");
}

void test_absorption_history_is_deterministic() {
    universe::Simulation first{default_layout_config(42)};
    universe::Simulation second{default_layout_config(42)};
    first.run_years(8);
    second.run_years(8);
    require(first.save_snapshot() == second.save_snapshot(), "the same seed must produce the same settlement history");
}

}  // namespace

int main() {
    test_nothing_is_absorbed_before_five_years_below_viable_size();
    test_settlements_below_viable_size_for_five_years_are_absorbed_into_a_viable_neighbour();
    test_absorption_in_a_demoted_region_moves_the_aggregate_ledger();
    test_absorption_history_is_deterministic();
    test_overgrown_settlement_splits();
    test_healthy_settlement_below_the_threshold_does_not_split();
    test_no_split_leaves_a_side_below_its_minimum();
    test_food_strain_days_follow_household_shortage();
    test_default_layout_is_unchanged();
    test_single_settlement_start_holds_every_household();
    test_requested_settlement_count_is_honoured_and_capped();
    test_single_settlement_world_continues_identically_after_reload();
    std::cout << "settlement structure tests passed\n";
    return 0;
}
