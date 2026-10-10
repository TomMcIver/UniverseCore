// Merchants appear when trade pays (ADR 0003, ticket #71). The decision rules are
// tested on their own; the rest runs at the headless Simulation seam.
#include "settlement_test_support.hpp"
#include "snapshot_test_support.hpp"

#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"
#include "universe/trade_merchants.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <tuple>
#include <variant>
#include <vector>

namespace {

using namespace settlement_test;
using universe::MerchantAction;
using universe::ResourceType;

constexpr std::array<bool, 5> kHoldsAll{true, true, true, true, true};
constexpr std::array<bool, 5> kHoldsNothing{false, false, false, false, false};

universe::SettlementPrices flat_prices(const std::int64_t price) {
    universe::SettlementPrices prices{};
    prices.millicoin.fill(price);
    return prices;
}

void test_margin_is_the_gap_over_the_origin_price() {
    require(universe::margin_permille(10'000, 10'000) == 0, "equal prices have no margin");
    require(universe::margin_permille(10'000, 15'000) == 500, "a fifty percent gap is 500 permille");
    require(universe::margin_permille(10'000, 40'000) == 3'000, "a fourfold price is 3000 permille");
    require(universe::margin_permille(10'000, 5'000) == -500, "selling below cost is a negative margin");
    require(universe::margin_permille(0, 5'000) == 0, "a zero origin price has no margin instead of dividing by zero");
}

void test_equal_prices_never_clear_the_threshold_and_a_large_gap_does() {
    const auto equal = universe::best_margin(flat_prices(10'000), flat_prices(10'000), kHoldsAll);
    require(equal.has_value() && !universe::margin_clears_threshold(equal), "equal prices between settlements do not pay");

    auto destination = flat_prices(10'000);
    destination.millicoin[static_cast<std::size_t>(ResourceType::Wood)] = 40'000;
    const auto gap = universe::best_margin(flat_prices(10'000), destination, kHoldsAll);
    require(gap.has_value() && universe::margin_clears_threshold(gap), "a large gap pays");
    require(gap->resource == ResourceType::Wood, "the margin names the resource that pays");
}

void test_threshold_boundary() {
    auto just_under = flat_prices(10'000);
    just_under.millicoin[0] = 14'999;
    auto exactly = flat_prices(10'000);
    exactly.millicoin[0] = 15'000;
    require(!universe::margin_clears_threshold(universe::best_margin(flat_prices(10'000), just_under, kHoldsAll)), "499 permille does not clear");
    require(universe::margin_clears_threshold(universe::best_margin(flat_prices(10'000), exactly, kHoldsAll)), "500 permille clears");
}

void test_only_goods_the_origin_holds_count_and_ties_take_the_lowest_resource() {
    auto destination = flat_prices(10'000);
    destination.millicoin[static_cast<std::size_t>(ResourceType::Stone)] = 40'000;
    require(!universe::best_margin(flat_prices(10'000), destination, kHoldsNothing).has_value(), "nothing held, nothing to sell");

    std::array<bool, 5> holds_no_stone = kHoldsAll;
    holds_no_stone[static_cast<std::size_t>(ResourceType::Stone)] = false;
    require(
        !universe::margin_clears_threshold(universe::best_margin(flat_prices(10'000), destination, holds_no_stone)),
        "a gap on a good the origin does not hold does not pay");

    auto tied = flat_prices(10'000);
    tied.millicoin[static_cast<std::size_t>(ResourceType::Timber)] = 30'000;
    tied.millicoin[static_cast<std::size_t>(ResourceType::Wood)] = 30'000;
    require(universe::best_margin(flat_prices(10'000), tied, kHoldsAll)->resource == ResourceType::Wood, "ties go to the lowest resource");
}

void test_decide_merchant_action_rules() {
    const auto bound = universe::kMaxMerchantsPerSettlement;
    const auto adults = universe::kMerchantMinimumEligibleAdults;
    require(universe::decide_merchant_action(true, 0, adults, 5, false) == MerchantAction::Appoint, "trade that pays trains a merchant");
    require(universe::decide_merchant_action(true, bound, adults, 5, false) == MerchantAction::None, "the bound is never exceeded");
    require(universe::decide_merchant_action(true, bound - 1U, adults, 5, false) == MerchantAction::Appoint, "one below the bound still trains");
    require(universe::decide_merchant_action(true, 0, adults - 1U, 5, false) == MerchantAction::None, "a settlement without adults to spare trains none");
    require(universe::decide_merchant_action(true, 0, adults, 0, false) == MerchantAction::None, "no one available, no merchant");
    require(universe::decide_merchant_action(false, 0, adults, 5, false) == MerchantAction::None, "trade that does not pay trains none");
    require(universe::decide_merchant_action(false, 2, adults, 5, false) == MerchantAction::Retire, "merchants retire when the margins close");
    require(universe::decide_merchant_action(true, 0, adults, 5, true) == MerchantAction::None, "a settlement under food strain trains no merchant");
    require(universe::decide_merchant_action(true, 2, adults, 5, true) == MerchantAction::Retire, "a settlement under food strain recalls its merchants");
    require(universe::decide_merchant_action(true, 0, adults, 5, true) == MerchantAction::None, "strain with no merchants has nothing to recall");
}

void test_within_days_of_walking() {
    require(universe::within_days_of_walking(0, 0, 1'000, 0, 1), "exactly a day's walk is within a day");
    require(!universe::within_days_of_walking(0, 0, 1'001, 0, 1), "one unit further is not");
    require(universe::within_days_of_walking(0, 0, 600, 800, 1), "straight-line distance, not axis distance");
    require(universe::within_days_of_walking(0, 0, 2'999, 0, 3), "three days reach three times as far");
}

std::vector<const universe::Person*> merchants_in(const universe::WorldState& state) {
    std::vector<const universe::Person*> merchants;
    for (const auto& person : state.people) {
        if (person.alive && person.occupation == universe::Occupation::Merchant) {
            merchants.push_back(&person);
        }
    }
    return merchants;
}

const universe::Household* household_of(const universe::WorldState& state, const universe::PersonId person_id) {
    for (const auto& household : state.households) {
        if (std::find(household.member_ids.begin(), household.member_ids.end(), person_id) != household.member_ids.end()) {
            return &household;
        }
    }
    return nullptr;
}

void require_merchant_invariants(const universe::Simulation& sim) {
    const auto& state = sim.state();
    const auto merchants = merchants_in(state);
    std::map<universe::SettlementId, std::uint64_t> per_settlement;
    for (const auto* merchant : merchants) {
        const auto* household = household_of(state, merchant->id);
        require(household != nullptr, "a merchant belongs to a household");
        ++per_settlement[household->settlement_id];
    }
    for (const auto& [settlement_id, count] : per_settlement) {
        require(count <= universe::kMaxMerchantsPerSettlement, "a settlement never has more merchants than the bound");
    }
    if (active_settlements(state).size() < 2U) {
        require(merchants.empty(), "a world with fewer than two active settlements has no merchants");
    }
    // Merchants are employed but outside the labour pools, so the worker counts still add up.
    require(
        state.eligible_workers == state.active_workers + state.unallocated_workers,
        "eligible workers must equal active plus unallocated, merchants included");

    // Memory belongs to living merchants, is sorted, and starts with the home settlement.
    for (std::size_t i = 0; i < state.merchant_price_memory.size(); ++i) {
        const auto& row = state.merchant_price_memory[i];
        require(
            std::any_of(merchants.begin(), merchants.end(), [&row](const universe::Person* merchant) { return merchant->id == row.merchant_id; }),
            "price memory belongs to a living merchant");
        if (i > 0U) {
            const auto& previous = state.merchant_price_memory[i - 1U];
            require(
                std::tie(previous.merchant_id, previous.settlement_id, previous.resource) <
                    std::tie(row.merchant_id, row.settlement_id, row.resource),
                "price memory is strictly ordered");
        }
        const auto base = universe::base_price_millicoin(row.resource);
        require(row.price_millicoin >= base / 4 && row.price_millicoin <= base * 4, "remembered prices are in the price range");
        require(row.seen_at.ticks <= state.time.ticks, "a price cannot be seen in the future");
    }
    // A merchant knows at least the five prices of the settlement it was trained in
    // (its household may have moved since, so the rows are not tied to where it lives now).
    for (const auto* merchant : merchants) {
        const auto known_rows = std::count_if(
            state.merchant_price_memory.begin(), state.merchant_price_memory.end(), [&](const universe::MerchantPriceMemory& row) {
                return row.merchant_id == merchant->id;
            });
        require(known_rows >= 5 && known_rows % 5 == 0, "a merchant remembers all five prices of each settlement it knows");
    }
}

void test_a_world_that_cannot_trade_has_no_merchants() {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        auto config = default_layout_config(seed);
        config.initial_population = 30U;
        config.initial_region_count = 1;
        config.initial_settlements_per_region = 1;
        universe::Simulation sim{config};
        for (int year = 0; year < 12; ++year) {
            sim.run_years(1);
            require_merchant_invariants(sim);
            if (active_settlements(sim.state()).size() == 1U) {
                require(merchants_in(sim.state()).empty(), "a single-settlement world never creates a merchant");
            }
        }
    }
}

// Runs a default-layout world for ten years and returns the merchant person ids
// seen at each year end.
std::vector<std::vector<universe::PersonId>> merchant_history(const std::uint64_t seed) {
    std::vector<std::vector<universe::PersonId>> history;
    universe::Simulation sim{default_layout_config(seed)};
    for (int year = 0; year < 10; ++year) {
        sim.run_years(1);
        require_merchant_invariants(sim);
        std::vector<universe::PersonId> ids;
        for (const auto* merchant : merchants_in(sim.state())) {
            ids.push_back(merchant->id);
        }
        history.push_back(ids);
    }
    return history;
}

void test_merchants_appear_where_trade_pays_and_are_deterministic() {
    std::uint64_t worlds_with_merchants = 0;
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        const auto first = merchant_history(seed);
        const auto second = merchant_history(seed);
        require(first == second, "the same seed must choose the same merchants every year");
        const bool any = std::any_of(first.begin(), first.end(), [](const auto& ids) { return !ids.empty(); });
        worlds_with_merchants += any ? 1U : 0U;
    }
    require(worlds_with_merchants > 0U, "trade must pay somewhere: at least one world trains a merchant");
}

void test_every_merchant_has_an_occupation_changed_event() {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        sim.run_years(10);
        for (const auto* merchant : merchants_in(sim.state())) {
            bool recorded = false;
            for (const auto& event : sim.state().event_log) {
                if (event.type != universe::EventType::OccupationChanged) {
                    continue;
                }
                const auto& payload = std::get<universe::OccupationChangedEventPayload>(event.payload);
                recorded = recorded || (payload.person_id == merchant->id && payload.to == universe::Occupation::Merchant);
            }
            require(recorded, "becoming a merchant is recorded as an occupation change");
        }
    }
}

void test_equivalent_time_advance_schedules_give_the_same_merchants() {
    universe::Simulation day_by_day{default_layout_config(2)};
    universe::Simulation one_chunk{default_layout_config(2)};
    for (int day = 0; day < 365 * 4; ++day) {
        day_by_day.advance(universe::days(1));
    }
    one_chunk.advance(universe::days(365 * 4));
    require(day_by_day.save_snapshot() == one_chunk.save_snapshot(), "equivalent schedules must give identical worlds, merchants included");
}

// The first seed whose world has a merchant after a few years; the tests below
// need one, and trade must pay somewhere for there to be any.
std::optional<std::uint64_t> seed_with_a_merchant() {
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        sim.run_years(6);
        if (!merchants_in(sim.state()).empty()) {
            return seed;
        }
    }
    return std::nullopt;
}

void test_merchant_memory_survives_save_and_load_and_the_world_continues_identically() {
    const auto seed = seed_with_a_merchant();
    require(seed.has_value(), "a world with a merchant is needed");
    universe::Simulation source{default_layout_config(*seed)};
    source.run_years(6);
    require(!source.state().merchant_price_memory.empty(), "a merchant remembers prices");
    const auto saved = source.save_snapshot();

    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(saved, restored).ok, "a snapshot with merchants must load");
    require(restored.save_snapshot() == saved, "merchant memory must round-trip byte for byte");
    require(restored.state().merchant_price_memory == source.state().merchant_price_memory, "remembered prices must survive reload");

    source.run_years(4);
    restored.run_years(4);
    require(source.save_snapshot() == restored.save_snapshot(), "a reloaded world must continue identically");
}

// Demoting a region folds its people into population groups, merchants included,
// so their remembered prices must go with them or the world could not be saved
// and loaded again.
void test_demoting_regions_drops_the_memory_of_the_merchants_it_folds() {
    const auto seed = seed_with_a_merchant();
    require(seed.has_value(), "a world with a merchant is needed");
    universe::Simulation sim{default_layout_config(*seed)};
    sim.run_years(6);
    require(!sim.state().merchant_price_memory.empty(), "a merchant remembers prices before the demotion");

    std::vector<universe::RegionId> region_ids;
    for (const auto& region : sim.state().regions) {
        region_ids.push_back(region.id);
    }
    for (const auto region_id : region_ids) {
        sim.demote_region_to_settlement_aggregate(region_id);
    }
    require(sim.state().merchant_price_memory.empty(), "folded merchants keep no remembered prices");
    require(merchants_in(sim.state()).empty(), "no living merchant remains once everyone is folded");

    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(sim.save_snapshot(), restored).ok, "a world demoted with merchants must save and load");
}

void test_a_v27_snapshot_loads_with_no_remembered_prices() {
    // Merchants are trained at a yearly boundary, so a world that has not reached one has none.
    universe::Simulation source{default_layout_config(3)};
    require(merchants_in(source.state()).empty(), "no merchant exists before the first yearly boundary has trained one");
    const auto v27 = snapshot_test::downgrade_to_v27(source.save_snapshot(), source.state());
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(v27, restored).ok, "a v27 snapshot must still load");
    require(restored.state().merchant_price_memory.empty(), "a snapshot from before merchants remembers no prices");
}

// Merchants exist only from schema v28, so an older file that names one is invalid.
void test_an_older_snapshot_cannot_carry_a_merchant() {
    const auto seed = seed_with_a_merchant();
    require(seed.has_value(), "a world with a merchant is needed");
    universe::Simulation source{default_layout_config(*seed)};
    source.run_years(6);
    const auto v27 = snapshot_test::downgrade_to_v27(source.save_snapshot(), source.state());
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto result = universe::Simulation::load_snapshot(v27, restored);
    require(!result.ok && result.code == universe::SnapshotErrorCode::InvalidEnum, "a v27 file naming the merchant occupation must be rejected");
}

void refresh_hash(std::vector<std::byte>& bytes) {
    const auto payload_size = bytes.size() - snapshot_test::kHeaderSize;
    snapshot_test::write_u64_at(bytes, 8U, payload_size);
    snapshot_test::write_u64_at(
        bytes, 16U, universe::snapshot_hash64(std::span<const std::byte>{bytes.data() + snapshot_test::kHeaderSize, payload_size}));
}

void test_snapshot_rejects_corrupt_merchant_memory() {
    const auto seed = seed_with_a_merchant();
    require(seed.has_value(), "a world with a merchant is needed");
    universe::Simulation source{default_layout_config(*seed)};
    source.run_years(6);
    const auto valid = source.save_snapshot();
    const auto rows = source.state().merchant_price_memory.size();
    require(rows >= 2U, "the corruption cases need two remembered rows");
    const auto section_start = valid.size() - snapshot_test::merchant_section_size(source.state());
    const auto row_offset = [&](const std::size_t index) {
        return section_start + snapshot_test::kCountSize + (index * snapshot_test::kMerchantMemoryRowSize);
    };

    const auto expect_failure = [&](const std::vector<std::byte>& bytes, const universe::SnapshotErrorCode code, const char* message) {
        universe::Simulation destination{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        const auto result = universe::Simulation::load_snapshot(bytes, destination);
        require(!result.ok && result.code == code, message);
    };

    auto not_a_merchant = valid;
    snapshot_test::write_u64_at(not_a_merchant, row_offset(0), 0xFFFFFFULL);
    refresh_hash(not_a_merchant);
    expect_failure(not_a_merchant, universe::SnapshotErrorCode::InvalidReference, "memory that belongs to no living merchant must be rejected");

    auto price_out_of_range = valid;
    snapshot_test::write_u64_at(price_out_of_range, row_offset(0) + 17U, 1ULL);
    refresh_hash(price_out_of_range);
    expect_failure(price_out_of_range, universe::SnapshotErrorCode::InvalidState, "a price outside the range must be rejected");

    auto future_row = valid;
    snapshot_test::write_u64_at(future_row, row_offset(0) + 25U, 0x7FFFFFFFFFFFULL);
    refresh_hash(future_row);
    expect_failure(future_row, universe::SnapshotErrorCode::InvalidState, "a price seen in the future must be rejected");

    // Drop the last remembered row: its settlement group is then four prices, not five.
    auto incomplete_group = valid;
    incomplete_group.resize(incomplete_group.size() - snapshot_test::kMerchantMemoryRowSize);
    snapshot_test::write_u64_at(incomplete_group, section_start, static_cast<std::uint64_t>(rows - 1U));
    refresh_hash(incomplete_group);
    expect_failure(incomplete_group, universe::SnapshotErrorCode::InvalidStructure, "a settlement group with fewer than five prices must be rejected");

    auto too_many = valid;
    snapshot_test::write_u64_at(too_many, section_start, static_cast<std::uint64_t>(rows + 1U));
    refresh_hash(too_many);
    expect_failure(too_many, universe::SnapshotErrorCode::TruncatedData, "a row count beyond the payload must be rejected");
}

}  // namespace

int main() {
    test_margin_is_the_gap_over_the_origin_price();
    test_equal_prices_never_clear_the_threshold_and_a_large_gap_does();
    test_threshold_boundary();
    test_only_goods_the_origin_holds_count_and_ties_take_the_lowest_resource();
    test_decide_merchant_action_rules();
    test_within_days_of_walking();
    test_a_world_that_cannot_trade_has_no_merchants();
    test_merchants_appear_where_trade_pays_and_are_deterministic();
    test_every_merchant_has_an_occupation_changed_event();
    test_equivalent_time_advance_schedules_give_the_same_merchants();
    test_merchant_memory_survives_save_and_load_and_the_world_continues_identically();
    test_demoting_regions_drops_the_memory_of_the_merchants_it_folds();
    test_a_v27_snapshot_loads_with_no_remembered_prices();
    test_an_older_snapshot_cannot_carry_a_merchant();
    test_snapshot_rejects_corrupt_merchant_memory();
    std::cout << "merchant tests passed\n";
    return 0;
}
