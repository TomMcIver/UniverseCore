// Coin endowment and conservation (ADR 0003, ticket #69). Tested at the headless
// Simulation seam: world creation, running time, snapshots and region level of
// detail. Coin is created once at world creation and only ever moves.
#include "settlement_test_support.hpp"
#include "snapshot_test_support.hpp"

#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"

#include <cstdint>
#include <iostream>
#include <set>
#include <span>
#include <unordered_map>
#include <vector>

namespace {

using namespace settlement_test;

constexpr std::uint64_t kSeedsChecked = 8;
constexpr int kYearsChecked = 4;
constexpr std::int64_t kCommonsPercent = 25;

void test_world_creation_endows_coin_deterministically() {
    std::set<std::int64_t> totals;
    for (std::uint64_t seed = 1; seed <= kSeedsChecked; ++seed) {
        universe::Simulation first{default_layout_config(seed)};
        universe::Simulation second{default_layout_config(seed)};
        const auto total = total_coin(first.state());
        require(total > 0, "a new world must start with coin");
        require(total == total_coin(second.state()), "the same seed must endow the same coin");
        totals.insert(total);

        // Each commons holds a fixed share of what its own households were given.
        std::unordered_map<universe::SettlementId, std::int64_t> household_coin;
        for (const auto& household : first.state().households) {
            require(household.coin >= 0, "household coin must never be negative");
            household_coin[household.settlement_id] += household.coin;
        }
        for (const auto& settlement : first.state().settlements) {
            require(settlement.coin == household_coin[settlement.id] * kCommonsPercent / 100,
                    "a commons starts with its share of its households' coin");
        }
    }
    require(totals.size() > 1U, "different seeds must not all endow the same total");
}

void test_total_coin_is_unchanged_as_the_world_runs() {
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        for (const bool single_start : {false, true}) {
            universe::Simulation sim{single_start ? single_settlement_config(seed) : default_layout_config(seed)};
            const auto starting = total_coin(sim.state());
            for (int year = 0; year < kYearsChecked; ++year) {
                sim.run_years(1);
                require(total_coin(sim.state()) == starting, "total coin must never change as the world runs");
            }
        }
    }
}

void test_coin_survives_a_snapshot_round_trip() {
    universe::Simulation source{default_layout_config(5)};
    source.run_years(2);
    const auto saved = source.save_snapshot();

    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(saved, restored).ok, "a snapshot with coin must load");
    require(restored.save_snapshot() == saved, "coin must round-trip byte for byte");
    require(total_coin(restored.state()) == total_coin(source.state()), "a reloaded world must hold the same coin");
    for (std::size_t i = 0; i < source.state().settlements.size(); ++i) {
        require(restored.state().settlements[i].coin == source.state().settlements[i].coin, "settlement coin must survive reload");
    }
    for (std::size_t i = 0; i < source.state().households.size(); ++i) {
        require(restored.state().households[i].coin == source.state().households[i].coin, "household coin must survive reload");
    }

    source.run_years(2);
    restored.run_years(2);
    require(source.save_snapshot() == restored.save_snapshot(), "a reloaded world must continue identically");
}

void test_older_snapshots_load_with_no_coin() {
    universe::Simulation source{default_layout_config(5)};
    source.run_years(1);
    require(total_coin(source.state()) > 0, "the source world has coin to lose");
    const auto v26 = snapshot_test::downgrade_to_v26(source.save_snapshot(), source.state());

    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(v26, restored).ok, "a v26 snapshot must still load");
    require(total_coin(restored.state()) == 0, "a snapshot from before coin must load with no coin");
    require(restored.state().population == source.state().population, "migration must change nothing but coin");
}

void test_snapshot_rejects_negative_coin() {
    universe::Simulation source{default_layout_config(5)};
    auto bytes = source.save_snapshot();
    // First settlement record: id at +8 after the section count, coin 8 bytes later.
    const auto section_start = bytes.size() - snapshot_test::coin_section_size(source.state());
    snapshot_test::write_u64_at(bytes, section_start + snapshot_test::kCountSize + 8U, ~std::uint64_t{0});
    const auto payload_size = bytes.size() - snapshot_test::kHeaderSize;
    snapshot_test::write_u64_at(bytes, 16U, universe::snapshot_hash64(std::span<const std::byte>{bytes.data() + snapshot_test::kHeaderSize, payload_size}));

    universe::Simulation destination{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(!universe::Simulation::load_snapshot(bytes, destination).ok, "negative coin must be rejected");
}

void test_demote_and_promote_keep_total_coin() {
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        sim.run_years(2);
        const auto before = total_coin(sim.state());
        std::vector<universe::RegionId> region_ids;
        for (const auto& region : sim.state().regions) {
            region_ids.push_back(region.id);
        }
        for (const auto id : region_ids) {
            sim.demote_region_to_settlement_aggregate(id);
        }
        require(total_coin(sim.state()) == before, "demoting regions must not change total coin");
        for (const auto id : region_ids) {
            sim.promote_region_to_detailed(id);
        }
        require(total_coin(sim.state()) == before, "promoting regions must not change total coin");
        sim.run_years(1);
        require(total_coin(sim.state()) == before, "total coin must stay unchanged after the cycle");
    }
}

}  // namespace

int main() {
    test_world_creation_endows_coin_deterministically();
    test_total_coin_is_unchanged_as_the_world_runs();
    test_coin_survives_a_snapshot_round_trip();
    test_older_snapshots_load_with_no_coin();
    test_snapshot_rejects_negative_coin();
    test_demote_and_promote_keep_total_coin();
    std::cout << "coin tests passed\n";
    return 0;
}
