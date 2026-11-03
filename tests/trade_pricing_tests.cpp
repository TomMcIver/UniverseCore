// Settlement prices from days of cover (ADR 0003, ticket #70). The arithmetic is
// tested on its own; the settlement prices are tested at the headless Simulation
// seam, through the public API only.
#include "settlement_test_support.hpp"

#include "universe/simulation.hpp"
#include "universe/trade_pricing.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using namespace settlement_test;
using universe::ResourceType;

constexpr std::array<ResourceType, 5> kAllResources{
    ResourceType::Food, ResourceType::Wood, ResourceType::Stone, ResourceType::Timber, ResourceType::BasicTool};

// Ten units a day, in the milli-units the arithmetic works in.
constexpr std::int64_t kTenPerDay = 10 * 1'000;
// Thirty days of cover at ten a day.
constexpr std::int64_t kTargetStock = 300;

void test_base_prices_follow_the_household_wealth_values() {
    // Food 1, wood 2, stone 2, timber 4, tool 8 wealth points, ten coin per point.
    require(universe::base_price_millicoin(ResourceType::Food) == 10'000, "food base price");
    require(universe::base_price_millicoin(ResourceType::Wood) == 20'000, "wood base price");
    require(universe::base_price_millicoin(ResourceType::Stone) == 20'000, "stone base price");
    require(universe::base_price_millicoin(ResourceType::Timber) == 40'000, "timber base price");
    require(universe::base_price_millicoin(ResourceType::BasicTool) == 80'000, "tool base price");
}

void test_price_is_the_base_at_the_target_cover() {
    for (const auto resource : kAllResources) {
        require(
            universe::price_millicoin(resource, kTargetStock, kTenPerDay) == universe::base_price_millicoin(resource),
            "a settlement holding the target days of cover pays the base price");
    }
}

void test_price_rises_as_cover_falls_and_stays_bounded() {
    for (const auto resource : kAllResources) {
        const auto base = universe::base_price_millicoin(resource);
        std::int64_t previous = std::numeric_limits<std::int64_t>::max();
        for (std::int64_t stock = 0; stock <= 5'000; stock += 7) {
            const auto price = universe::price_millicoin(resource, stock, kTenPerDay);
            require(price <= previous, "a larger stock must never cost more");
            require(price >= base / 4 && price <= base * 4, "a price must stay between 0.25x and 4x the base");
            previous = price;
        }
        std::int64_t last = 0;
        for (std::int64_t need = 0; need <= 200'000; need += 1'009) {
            const auto price = universe::price_millicoin(resource, kTargetStock, need);
            require(price >= last, "a larger need must never cost less");
            last = price;
        }
    }
}

void test_extremes_are_the_floor_and_the_ceiling() {
    const auto base = universe::base_price_millicoin(ResourceType::Wood);
    require(universe::price_millicoin(ResourceType::Wood, 0, kTenPerDay) == base * 4, "no stock but a need prices at the ceiling");
    require(universe::price_millicoin(ResourceType::Wood, 1'000'000, kTenPerDay) == base / 4, "abundant stock prices at the floor");
}

void test_no_need_has_a_defined_price() {
    const auto base = universe::base_price_millicoin(ResourceType::Stone);
    require(universe::price_millicoin(ResourceType::Stone, 0, 0) == base / 4, "no need and no stock is the floor, not a divide by zero");
    require(universe::price_millicoin(ResourceType::Stone, 500, 0) == base / 4, "no need with stock is the floor");
    require(universe::price_millicoin(ResourceType::Stone, 500, -5) == base / 4, "a negative need is treated as none");
    require(universe::price_millicoin(ResourceType::Stone, -5, kTenPerDay) == base * 4, "a negative stock is treated as none");
}

void test_equal_inputs_equal_price_and_different_stock_differs() {
    require(
        universe::price_millicoin(ResourceType::Timber, 120, kTenPerDay) == universe::price_millicoin(ResourceType::Timber, 120, kTenPerDay),
        "equal stock and need give equal prices");
    require(
        universe::price_millicoin(ResourceType::Timber, 120, kTenPerDay) != universe::price_millicoin(ResourceType::Timber, 240, kTenPerDay),
        "different stock of the same resource gives a different price");
}

void test_huge_need_does_not_overflow() {
    const auto base = universe::base_price_millicoin(ResourceType::Food);
    require(
        universe::price_millicoin(ResourceType::Food, 5, std::numeric_limits<std::int64_t>::max()) == base * 4,
        "an enormous need prices at the ceiling instead of overflowing");
}

void test_outstanding_demand_is_spread_over_the_horizon() {
    require(universe::daily_need_milli_from_outstanding(300) == 10'000, "300 units over thirty days is ten a day");
    require(universe::daily_need_milli_from_outstanding(0) == 0, "no outstanding demand is no need");
    require(universe::daily_need_milli_from_outstanding(-4) == 0, "negative outstanding demand is no need");
    const auto huge = universe::daily_need_milli_from_outstanding(std::numeric_limits<std::int64_t>::max());
    require(huge > 0, "an enormous outstanding demand saturates instead of overflowing");
    require(
        universe::price_multiplier_permille(1, huge) == universe::kPriceCeilingPermille,
        "a saturated need prices at the ceiling");
}

// The multiplier is exactly the floor and the ceiling at the stocks that land on
// them, and moves off them one unit of stock either side.
void test_multiplier_boundaries_are_exact() {
    require(universe::price_multiplier_permille(1'200, kTenPerDay) == universe::kPriceFloorPermille, "stock 1200 is exactly 0.25x");
    require(universe::price_multiplier_permille(1'199, kTenPerDay) == 250, "just under the floor stock is still 0.25x");
    require(universe::price_multiplier_permille(1'201, kTenPerDay) == universe::kPriceFloorPermille, "past the floor stock is clamped to 0.25x");
    require(universe::price_multiplier_permille(1'000, kTenPerDay) == 300, "stock 1000 is 0.3x");
    require(universe::price_multiplier_permille(75, kTenPerDay) == universe::kPriceCeilingPermille, "stock 75 is exactly 4x");
    require(universe::price_multiplier_permille(74, kTenPerDay) == universe::kPriceCeilingPermille, "below the ceiling stock is clamped to 4x");
    require(universe::price_multiplier_permille(76, kTenPerDay) == 3'947, "stock 76 is just under 4x");
}

std::int64_t price_of(const universe::SettlementPrices& prices, const ResourceType resource) {
    return prices.of(resource);
}

void require_prices_in_bounds(const universe::Simulation& sim) {
    for (const auto& settlement : sim.state().settlements) {
        const auto prices = sim.settlement_prices(settlement.id);
        if (!settlement.active) {
            require(!prices.has_value(), "an inactive settlement has no prices");
            continue;
        }
        require(prices.has_value(), "an active settlement has prices");
        for (const auto resource : kAllResources) {
            const auto base = universe::base_price_millicoin(resource);
            const auto price = price_of(*prices, resource);
            require(price >= base / 4 && price <= base * 4, "a settlement price must stay between 0.25x and 4x the base");
        }
    }
    require(!sim.settlement_prices(universe::SettlementId{999'999'999U}).has_value(), "an unknown settlement has no prices");
}

void test_settlement_prices_stay_in_bounds_as_the_world_runs() {
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        require_prices_in_bounds(sim);
        for (int year = 0; year < 3; ++year) {
            sim.run_years(1);
            require_prices_in_bounds(sim);
        }
    }
}

void test_settlements_with_different_stock_have_different_prices() {
    bool found_difference = false;
    std::size_t pairs_compared = 0;
    for (std::uint64_t seed = 1; seed <= 8 && !found_difference; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        sim.run_years(3);
        const auto active = active_settlements(sim.state());
        for (std::size_t i = 0; i + 1 < active.size() && !found_difference; ++i) {
            const auto first = sim.settlement_prices(active[i]->id);
            const auto second = sim.settlement_prices(active[i + 1]->id);
            require(first.has_value() && second.has_value(), "active settlements have prices");
            ++pairs_compared;
            for (const auto resource : kAllResources) {
                found_difference = found_difference || price_of(*first, resource) != price_of(*second, resource);
            }
        }
    }
    require(pairs_compared > 0U, "the comparison must see at least two settlements");
    require(found_difference, "settlements with different stock and need must not all share one price");
}

bool is_productive(const universe::Occupation occupation) {
    return occupation == universe::Occupation::SubsistenceCultivator || occupation == universe::Occupation::WildFoodGatherer ||
        occupation == universe::Occupation::WoodGatherer || occupation == universe::Occupation::StoneExtractor ||
        occupation == universe::Occupation::CraftWorker;
}

// An oracle worked out by hand from public state: a settlement's tool price is
// its commons tool stock against one break per twenty-four worker-days of its
// productive people. Households hold a share of the same stock (owned_materials
// is the commons stock shared out), so adding them in would double count it and
// push the price down.
void test_tool_price_matches_a_hand_computed_oracle() {
    constexpr std::int64_t kWorkUnitsPerBreak = 24;
    std::size_t settlements_checked = 0;
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        sim.run_years(3);
        for (const auto* settlement : active_settlements(sim.state())) {
            std::int64_t productive_workers = 0;
            for (const auto& household : sim.state().households) {
                if (household.settlement_id != settlement->id) {
                    continue;
                }
                for (const auto member_id : household.member_ids) {
                    for (const auto& person : sim.state().people) {
                        if (person.id == member_id && person.alive && is_productive(person.occupation)) {
                            ++productive_workers;
                        }
                    }
                }
            }
            const auto expected = universe::price_millicoin(
                ResourceType::BasicTool,
                std::max<std::int64_t>(0, settlement->materials.basic_tool.value),
                productive_workers * universe::kNeedMilliPerUnit / kWorkUnitsPerBreak);
            require(
                sim.settlement_prices(settlement->id)->of(ResourceType::BasicTool) == expected,
                "the tool price must follow the commons tool stock against worker wear");
            ++settlements_checked;
        }
    }
    require(settlements_checked > 0U, "the oracle must check at least one settlement");
}

void test_prices_are_identical_for_the_same_seed_and_schedule() {
    universe::Simulation first{default_layout_config(3)};
    universe::Simulation second{default_layout_config(3)};
    first.run_years(2);
    second.run_years(2);
    for (const auto& settlement : first.state().settlements) {
        const auto a = first.settlement_prices(settlement.id);
        const auto b = second.settlement_prices(settlement.id);
        require(a.has_value() == b.has_value(), "the same seed must price the same settlements");
        for (const auto resource : kAllResources) {
            require(!a.has_value() || price_of(*a, resource) == price_of(*b, resource), "the same seed must give the same prices");
        }
    }
}

void test_equivalent_time_advance_schedules_give_the_same_prices() {
    universe::Simulation day_by_day{default_layout_config(4)};
    universe::Simulation one_chunk{default_layout_config(4)};
    for (int day = 0; day < 365; ++day) {
        day_by_day.advance(universe::days(1));
    }
    one_chunk.advance(universe::days(365));
    for (const auto& settlement : day_by_day.state().settlements) {
        const auto a = day_by_day.settlement_prices(settlement.id);
        const auto b = one_chunk.settlement_prices(settlement.id);
        require(a.has_value() == b.has_value(), "equivalent schedules must price the same settlements");
        for (const auto resource : kAllResources) {
            require(!a.has_value() || price_of(*a, resource) == price_of(*b, resource), "equivalent schedules must give the same prices");
        }
    }
}

void test_prices_survive_save_and_load() {
    universe::Simulation source{default_layout_config(5)};
    source.run_years(2);
    const auto saved = source.save_snapshot();
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(saved, restored).ok, "the snapshot must load");
    for (const auto& settlement : source.state().settlements) {
        const auto a = source.settlement_prices(settlement.id);
        const auto b = restored.settlement_prices(settlement.id);
        require(a.has_value() == b.has_value(), "a reload must price the same settlements");
        for (const auto resource : kAllResources) {
            require(!a.has_value() || price_of(*a, resource) == price_of(*b, resource), "prices must survive save and load unchanged");
        }
    }
}

void test_prices_are_defined_when_a_region_is_demoted() {
    universe::Simulation sim{default_layout_config(2)};
    sim.run_years(1);
    for (const auto& region : std::vector<universe::RegionEnvironmentState>(sim.state().regions)) {
        sim.demote_region_to_settlement_aggregate(region.id);
    }
    require_prices_in_bounds(sim);
}

}  // namespace

int main() {
    test_base_prices_follow_the_household_wealth_values();
    test_price_is_the_base_at_the_target_cover();
    test_price_rises_as_cover_falls_and_stays_bounded();
    test_extremes_are_the_floor_and_the_ceiling();
    test_no_need_has_a_defined_price();
    test_equal_inputs_equal_price_and_different_stock_differs();
    test_huge_need_does_not_overflow();
    test_outstanding_demand_is_spread_over_the_horizon();
    test_multiplier_boundaries_are_exact();
    test_settlement_prices_stay_in_bounds_as_the_world_runs();
    test_settlements_with_different_stock_have_different_prices();
    test_tool_price_matches_a_hand_computed_oracle();
    test_prices_are_identical_for_the_same_seed_and_schedule();
    test_equivalent_time_advance_schedules_give_the_same_prices();
    test_prices_survive_save_and_load();
    test_prices_are_defined_when_a_region_is_demoted();
    std::cout << "trade pricing tests passed\n";
    return 0;
}
