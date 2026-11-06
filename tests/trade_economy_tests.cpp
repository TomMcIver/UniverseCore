// Trade that actually sells (ADR 0003, ticket #86). The arithmetic is tested on
// its own, then the world properties the calibration found missing: a merchant
// stops carrying goods to a buyer it knows cannot pay, a fission-founded
// settlement has coin, trips mostly sell, and the new state survives save and load.
#include "settlement_test_support.hpp"
#include "snapshot_test_support.hpp"

#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"
#include "universe/trade_trip.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <variant>
#include <vector>

namespace {

using namespace settlement_test;

const universe::TradeCapability kWalking = universe::baseline_trade_capability();

// ---------------------------------------------------------------- arithmetic

void test_a_buyer_without_coin_yields_no_trip() {
    const auto offer = universe::evaluate_trip(10'000, 40'000, 1'000, 1'000, kWalking, 0);
    require(!offer.has_value(), "a destination known to hold no coin is not worth a trip");
    const auto dust = universe::evaluate_trip(10'000, 40'000, 1'000, 1'000, kWalking, 39);
    require(!dust.has_value(), "coin that buys no whole unit at the destination price is not worth a trip");
}

void test_remembered_coin_caps_the_cargo() {
    const auto uncapped = universe::evaluate_trip(10'000, 40'000, 1'000, 1'000, kWalking);
    require(uncapped.has_value() && uncapped->cargo_units == 50, "an unknown buyer is assumed able to pay for a full load");
    const auto unknown = universe::evaluate_trip(10'000, 40'000, 1'000, 1'000, kWalking, universe::kCoinUnknown);
    require(unknown == uncapped, "an explicitly unknown balance behaves as before");

    // 200 coin at 40 coin a unit buys five units.
    const auto capped = universe::evaluate_trip(10'000, 40'000, 1'000, 1'000, kWalking, 200);
    require(capped.has_value() && capped->cargo_units == 5, "the cargo is what the buyer's remembered coin pays for");
    require(capped->expected_revenue == 200 && capped->expected_profit == 150, "expected revenue never exceeds the remembered coin");

    const auto rich = universe::evaluate_trip(10'000, 40'000, 1'000, 1'000, kWalking, 1'000'000);
    require(rich == uncapped, "a buyer with plenty of coin does not shrink the load");
}

// --------------------------------------------------------------------- world

struct TradeRun {
    std::uint64_t departed{};
    std::uint64_t completed{};
    std::uint64_t sold_something{};
};

// Steps a world one day at a time, checking per day what must always hold, and
// tallies what trade did.
TradeRun run_checked(universe::Simulation& sim, const int years) {
    TradeRun run;
    const auto coin_total = total_coin(sim.state());
    std::size_t cursor = sim.state().event_log.size();
    for (int day = 0; day < years * 365; ++day) {
        sim.advance(universe::days(1));
        const auto& state = sim.state();
        std::int64_t carried = 0;
        for (const auto& trip : state.merchant_trips) {
            carried += trip.coin_carried;
            // A departing merchant never carries more to a buyer than the buyer is
            // remembered able to pay for.
            if (trip.departed_at.ticks != state.time.ticks) {
                continue;
            }
            for (const auto& row : state.merchant_price_memory) {
                if (row.merchant_id == trip.merchant_id && row.settlement_id == trip.destination_settlement_id &&
                    row.resource == trip.resource && row.settlement_coin != universe::kCoinUnknown) {
                    require(universe::units_affordable(row.settlement_coin, row.price_millicoin) >= trip.cargo_units,
                            "a trip never carries more than the buyer is remembered to afford");
                }
            }
        }
        require(total_coin(state) + carried == coin_total, "total coin never changes, counting coin on the road");
        for (const auto& settlement : state.settlements) {
            require(settlement.coin >= 0, "no coin balance goes negative");
        }
        for (; cursor < state.event_log.size(); ++cursor) {
            const auto& event = state.event_log[cursor];
            if (event.type == universe::EventType::TradeDeparted) {
                ++run.departed;
            } else if (event.type == universe::EventType::TradeCompleted) {
                ++run.completed;
                run.sold_something += std::get<universe::TradeEventPayload>(event.payload).coin_profit > 0 ? 1U : 0U;
            }
        }
    }
    return run;
}

const universe::Settlement* find_settlement_by_id(const universe::WorldState& state, const universe::SettlementId id) {
    for (const auto& settlement : state.settlements) {
        if (settlement.id == id) {
            return &settlement;
        }
    }
    return nullptr;
}

// Single-settlement worlds grow by fission, so they found settlements reliably.
void test_a_founded_settlement_starts_with_coin_and_nothing_is_created() {
    std::uint64_t founded = 0;
    for (const std::uint64_t seed : {39U, 40U, 41U}) {
        universe::Simulation sim{single_settlement_config(seed)};
        const auto coin_total = total_coin(sim.state());
        std::size_t cursor = 0;
        for (int day = 0; day < 20 * 365; ++day) {
            sim.advance(universe::days(1));
            const auto& state = sim.state();
            std::int64_t carried = 0;
            for (const auto& trip : state.merchant_trips) {
                carried += trip.coin_carried;
            }
            require(total_coin(state) + carried == coin_total, "founding a settlement moves coin, it never creates it");
            for (; cursor < state.event_log.size(); ++cursor) {
                const auto& event = state.event_log[cursor];
                if (event.type != universe::EventType::SettlementFounded) {
                    continue;
                }
                const auto& payload = std::get<universe::SettlementFoundedEventPayload>(event.payload);
                const auto* daughter = find_settlement_by_id(state, payload.settlement_id);
                require(daughter != nullptr, "a founded settlement exists");
                ++founded;
                require(daughter->coin > 0, "a settlement founded by fission starts with a share of its founder's coin");
            }
        }
    }
    require(founded > 0, "the seeds must found at least one settlement for this test to mean anything");
}

// The calibration found 99.9 percent of trips selling nothing: a buyer ran out of
// coin and the merchant went on carrying goods to it every day. With the buyer's
// means remembered, trips must now mostly sell.
void test_trips_sell_and_the_loop_ends() {
    std::uint64_t completed = 0;
    std::uint64_t sold_something = 0;
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        const auto run = run_checked(sim, 20);
        completed += run.completed;
        sold_something += run.sold_something;
    }
    require(completed > 0, "trade must happen in these worlds");
    require(sold_something * 10 >= completed * 8, "at least 80 percent of completed trips must sell something");
}

// -------------------------------------------------------------- save and load

// Runs default-layout worlds until a merchant is on a trip; leaves `sim` there.
bool reach_a_trip(universe::Simulation& sim) {
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        sim = universe::Simulation{default_layout_config(seed)};
        for (int day = 0; day < 20 * 365; ++day) {
            sim.advance(universe::days(1));
            if (!sim.state().merchant_trips.empty()) {
                return true;
            }
        }
    }
    return false;
}

void test_a_mid_trip_save_continues_identically_after_reload() {
    universe::Simulation source{default_layout_config(1)};
    require(reach_a_trip(source), "a world must put a merchant on a trip");
    bool any_known = false;
    for (const auto& row : source.state().merchant_price_memory) {
        any_known = any_known || row.settlement_coin != universe::kCoinUnknown;
    }
    require(any_known, "merchants remember the coin of the settlements they have seen");

    const auto bytes = source.save_snapshot();
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(bytes, restored).ok, "a v30 snapshot loads");
    require(restored.state().merchant_trips == source.state().merchant_trips, "the trip survives reload");
    require(restored.state().merchant_price_memory == source.state().merchant_price_memory, "remembered coin survives reload");
    for (int step = 0; step < 90; ++step) {
        source.advance(universe::days(1));
        restored.advance(universe::days(1));
    }
    require(source.save_snapshot() == restored.save_snapshot(), "a mid-trip save continues identically to the original");
}

void test_a_v29_snapshot_loads_with_unknown_buyer_coin() {
    universe::Simulation source{default_layout_config(1)};
    require(reach_a_trip(source), "a world must put a merchant on a trip");
    const auto v29 = snapshot_test::downgrade_to_v29(source.save_snapshot(), source.state());
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(v29, restored).ok, "a v29 snapshot must still load");
    require(restored.state().merchant_trips == source.state().merchant_trips, "the trips survive the migration");
    require(restored.state().merchant_price_memory.size() == source.state().merchant_price_memory.size(),
            "the merchants keep their remembered prices");
    for (const auto& row : restored.state().merchant_price_memory) {
        require(row.settlement_coin == universe::kCoinUnknown, "an older memory does not know the buyer's coin");
    }
    for (int day = 0; day < 60; ++day) {
        restored.advance(universe::days(1));
    }
}

void refresh_hash(std::vector<std::byte>& bytes) {
    const auto payload_size = bytes.size() - snapshot_test::kHeaderSize;
    snapshot_test::write_u64_at(bytes, 8U, payload_size);
    snapshot_test::write_u64_at(
        bytes, 16U, universe::snapshot_hash64(std::span<const std::byte>{bytes.data() + snapshot_test::kHeaderSize, payload_size}));
}

void test_snapshot_rejects_a_corrupt_trade_economy_section() {
    universe::Simulation source{default_layout_config(1)};
    require(reach_a_trip(source), "a world must put a merchant on a trip");
    const auto valid = source.save_snapshot();
    const auto& state = source.state();
    const auto section_start = valid.size() - snapshot_test::trade_economy_section_size(state);
    const auto first_coin = section_start + snapshot_test::kCountSize;

    const auto expect_failure = [&](std::vector<std::byte> bytes, const universe::SnapshotErrorCode code, const char* message) {
        refresh_hash(bytes);
        universe::Simulation destination{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        const auto result = universe::Simulation::load_snapshot(bytes, destination);
        require(!result.ok && result.code == code, message);
    };

    auto wrong_rows = valid;
    snapshot_test::write_u64_at(wrong_rows, section_start, state.merchant_price_memory.size() + 1U);
    expect_failure(wrong_rows, universe::SnapshotErrorCode::InvalidStructure, "a coin count that does not match the memory must be rejected");

    auto negative_coin = valid;
    snapshot_test::write_u64_at(negative_coin, first_coin, static_cast<std::uint64_t>(-5));
    expect_failure(negative_coin, universe::SnapshotErrorCode::InvalidState, "a remembered balance below unknown must be rejected");
}

}  // namespace

int main() {
    test_a_buyer_without_coin_yields_no_trip();
    test_remembered_coin_caps_the_cargo();
    test_a_founded_settlement_starts_with_coin_and_nothing_is_created();
    test_trips_sell_and_the_loop_ends();
    test_a_mid_trip_save_continues_identically_after_reload();
    test_a_v29_snapshot_loads_with_unknown_buyer_coin();
    test_snapshot_rejects_a_corrupt_trade_economy_section();
    std::cout << "universe_trade_economy_tests passed\n";
    return 0;
}
