// Behaviour tests for settlement structure (ADR 0002): the starting layout,
// pressure-driven fission and absorption of settlements that stay too small.
// They run at the headless Simulation seam: configure a world, advance time,
// read the resulting state.
#include "universe/simulation.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <variant>
#include <vector>

namespace {

constexpr std::uint64_t kStartingPopulation = 200U;

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

universe::SimulationConfig default_layout_config(const std::uint64_t seed) {
    return universe::SimulationConfig{
        .seed = seed,
        .initial_population = kStartingPopulation,
        .enable_unrelated_random_probe = false,
        .initial_household_food_units = 80,
        .food_production_scale_percent = 100,
        .food_system_enabled = true,
    };
}

// One region holding as much land as the default four regions together, so a
// single-settlement start is not starved of land.
constexpr std::int64_t kSingleRegionLandUnits = 480;

universe::SimulationConfig single_settlement_config(const std::uint64_t seed) {
    auto config = default_layout_config(seed);
    config.initial_region_count = 1;
    config.initial_settlements_per_region = 1;
    config.base_region_area_units = kSingleRegionLandUnits;
    return config;
}

std::vector<const universe::Settlement*> active_settlements(const universe::WorldState& state) {
    std::vector<const universe::Settlement*> active;
    for (const auto& settlement : state.settlements) {
        if (settlement.active) {
            active.push_back(&settlement);
        }
    }
    return active;
}

// An active household (one with living members) belongs to exactly one active
// settlement in its region. Households emptied by extinction are not active.
void require_every_household_in_one_valid_settlement(const universe::WorldState& state) {
    for (const auto& household : state.households) {
        if (household.member_ids.empty()) {
            continue;
        }
        std::uint64_t matches = 0;
        for (const auto& settlement : state.settlements) {
            if (settlement.id == household.settlement_id && settlement.active && settlement.region_id == household.region_id) {
                ++matches;
            }
        }
        require(matches == 1U, "every household must belong to exactly one active settlement in its region");
    }
}

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
        }
    }
    require(splits_checked > 0U, "the scenario must exercise at least one split");
}

// ADR 0002: the absorbed side is below the viable size for this many years, the
// survivor sits within this distance of it.
constexpr std::uint32_t kAbsorptionDays = 5U * 365U;
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
    for (const std::uint64_t seed : {1U, 7U, 42U}) {
        universe::Simulation sim{default_layout_config(seed)};
        for (int year = 0; year < kYears; ++year) {
            const auto events_before = sim.state().event_log.size();
            const auto before = snapshot_before(sim.state());
            sim.run_years(1);

            const auto absorptions = absorptions_since(sim.state(), events_before);
            for (const auto& absorption : absorptions) {
                const auto& absorbed = settlement_by_id(sim.state(), absorption.absorbed_id);
                const auto& survivor = settlement_by_id(sim.state(), absorption.survivor_id);

                require(
                    before.at(absorption.absorbed_id).days_below_viable_size + kDaysPerYear >= kAbsorptionDays,
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
                require(
                    absorbed.materials.wood.value == 0 && absorbed.materials.stone.value == 0 &&
                        absorbed.materials.timber.value == 0 && absorbed.materials.basic_tool.value == 0,
                    "an absorbed settlement keeps no materials");
                for (const auto household_id : before.at(absorption.absorbed_id).household_ids) {
                    require(
                        household_by_id(sim.state(), household_id).settlement_id == survivor.id,
                        "every household of an absorbed settlement moves to the survivor");
                }
                ++absorptions_checked;
            }
            require_every_household_in_one_valid_settlement(sim.state());

            // A merge that strands stock in an inactive settlement makes the world
            // unloadable, so the state right after an absorption must round-trip.
            if (!absorptions.empty()) {
                universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
                const auto load_result = universe::Simulation::load_snapshot(sim.save_snapshot(), restored);
                if (!load_result.ok) {
                    std::cerr << "load failed: " << load_result.message << '\n';
                }
                require(load_result.ok, "a snapshot taken right after an absorption must load");

                // Hidden state (such as lookup indexes keyed by settlement) must
                // match what a reload rebuilds, or the two runs drift apart.
                constexpr std::uint64_t kContinuationYears = 3;
                auto continuing = sim;
                continuing.run_years(kContinuationYears);
                restored.run_years(kContinuationYears);
                require(
                    continuing.save_snapshot() == restored.save_snapshot(),
                    "a reload right after an absorption must continue identically");
            }
        }
    }
    require(absorptions_checked > 0U, "the scenario must exercise at least one absorption");
}

void test_absorption_history_is_deterministic() {
    universe::Simulation first{default_layout_config(42)};
    universe::Simulation second{default_layout_config(42)};
    first.run_years(8);
    second.run_years(8);
    require(first.save_snapshot() == second.save_snapshot(), "the same seed must produce the same settlement history");
}

// Measured for ADR 0002 (single-settlement start, 200 people, 20 years, seeds
// 1-64): 52 seeds end with 2-5 settlements and a largest of 70 or more. Ten
// seeds die out entirely and two fall to about 75 people. That is a property of
// the one-region start, not of fission: with fission switched off the same
// start loses 13 seeds. So the target is asserted for worlds that did not
// collapse, and the number that did not is held above a floor.
constexpr std::uint64_t kDistributionSeedCount = 64;
constexpr int kDistributionYears = 20;
constexpr std::uint64_t kCollapsedBelowPopulation = kStartingPopulation / 2U;
constexpr std::size_t kTargetMinSettlements = 2;
constexpr std::size_t kTargetMaxSettlements = 5;
constexpr std::uint64_t kTargetMinLargestSettlement = 70;
constexpr std::size_t kMinSurvivingSeeds = 40;

void test_single_settlement_start_grows_into_a_village_and_hamlets() {
    std::size_t surviving_seeds = 0;
    for (std::uint64_t seed = 1; seed <= kDistributionSeedCount; ++seed) {
        universe::Simulation sim{single_settlement_config(seed)};
        sim.run_years(kDistributionYears);

        const auto active = active_settlements(sim.state());
        std::uint64_t largest = 0;
        for (const auto* settlement : active) {
            largest = std::max(largest, settlement->population);
            require(
                settlement->days_below_viable_size <= kAbsorptionDays,
                "no settlement may stay below the viable size for more than five years");
        }
        require_every_household_in_one_valid_settlement(sim.state());

        if (sim.detailed_population() < kCollapsedBelowPopulation) {
            continue;
        }
        ++surviving_seeds;
        require(
            active.size() >= kTargetMinSettlements && active.size() <= kTargetMaxSettlements,
            "a surviving world must end with two to five settlements");
        require(largest >= kTargetMinLargestSettlement, "a surviving world must hold a village of at least seventy people");
    }
    require(surviving_seeds >= kMinSurvivingSeeds, "most seeds must survive the single-settlement start");
}

// The default layout starts with several settlements and must not splinter
// further: nothing in it is large enough to split within twenty years.
void test_default_layout_does_not_splinter_further() {
    constexpr std::uint64_t kSeedsChecked = 16;
    for (std::uint64_t seed = 1; seed <= kSeedsChecked; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        const auto starting = active_settlements(sim.state()).size();
        sim.run_years(kDistributionYears);
        require(
            active_settlements(sim.state()).size() <= starting,
            "the default layout must not end with more settlements than it started with");
    }
}

}  // namespace

int main() {
    test_single_settlement_start_grows_into_a_village_and_hamlets();
    test_default_layout_does_not_splinter_further();
    test_nothing_is_absorbed_before_five_years_below_viable_size();
    test_settlements_below_viable_size_for_five_years_are_absorbed_into_a_viable_neighbour();
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
