// Trade is deterministic and resumable (ADR 0003, ticket #75). Everything runs at
// the headless Simulation seam: the same seed must give the same prices, merchants
// and coin however the run is scheduled, and a snapshot taken at any point must
// continue byte for byte like the world it came from.
//
// Trip-specific assertions (departure, mid-trip save/load, completion, profit
// split) depend on the trade trip mechanics of ticket #72. They are marked
// "PENDING #72" below and skipped until that lands; see run_trip_tests.
#include "settlement_test_support.hpp"

#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace settlement_test;

constexpr std::uint64_t kSeedsChecked = 4;
constexpr int kYearsChecked = 8;
constexpr int kDaysPerYear = 365;

// Flip once #72 lands and the PENDING #72 bodies below are filled in.
constexpr bool kTripMechanicsLanded = false;

// Fresh target for loading; its own seed and population must not matter.
universe::Simulation restore(const std::vector<std::byte>& saved) {
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(saved, restored).ok, "a saved snapshot must load");
    return restored;
}

std::size_t merchant_count(const universe::WorldState& state) {
    return static_cast<std::size_t>(std::count_if(state.people.begin(), state.people.end(), [](const universe::Person& person) {
        return person.alive && person.occupation == universe::Occupation::Merchant;
    }));
}

// Every active settlement's derived prices, in settlement order, flattened.
std::vector<std::int64_t> all_prices(const universe::Simulation& sim) {
    std::vector<std::int64_t> prices;
    for (const auto& settlement : sim.state().settlements) {
        if (!settlement.active) {
            continue;
        }
        const auto settlement_prices = sim.settlement_prices(settlement.id);
        if (!settlement_prices.has_value()) {
            continue;
        }
        prices.push_back(static_cast<std::int64_t>(settlement.id));
        prices.insert(prices.end(), settlement_prices->millicoin.begin(), settlement_prices->millicoin.end());
    }
    return prices;
}

// The trade-relevant facts compared at every step, plus the full snapshot.
struct TradeFingerprint {
    std::vector<std::int64_t> prices;
    std::int64_t total_coin{};
    std::size_t merchants{};
    std::vector<std::byte> snapshot;
    bool operator==(const TradeFingerprint&) const = default;
};

TradeFingerprint fingerprint(const universe::Simulation& sim) {
    return TradeFingerprint{all_prices(sim), total_coin(sim.state()), merchant_count(sim.state()), sim.save_snapshot()};
}

void test_same_seed_gives_identical_trade_state_every_year() {
    for (std::uint64_t seed = 1; seed <= kSeedsChecked; ++seed) {
        universe::Simulation first{default_layout_config(seed)};
        universe::Simulation second{default_layout_config(seed)};
        require(fingerprint(first) == fingerprint(second), "a new world must be identical for the same seed");
        for (int year = 0; year < kYearsChecked; ++year) {
            first.run_years(1);
            second.run_years(1);
            require(fingerprint(first) == fingerprint(second), "prices, merchants, coin and the snapshot must match every year");
        }
    }
}

void test_prices_are_a_pure_function_of_state() {
    for (std::uint64_t seed = 1; seed <= kSeedsChecked; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        sim.run_years(3);
        const auto before = all_prices(sim);
        require(!before.empty(), "an active settlement must have prices");
        require(before == all_prices(sim), "reading prices twice must give the same answer");
        const auto saved = sim.save_snapshot();
        require(saved == sim.save_snapshot(), "saving must not change the world");
        require(before == all_prices(sim), "saving must not change prices");
        require(all_prices(restore(saved)) == before, "a reloaded world must price identically");
    }
}

void test_total_coin_is_conserved_while_trade_runs() {
    for (std::uint64_t seed = 1; seed <= kSeedsChecked; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        const auto starting = total_coin(sim.state());
        require(starting > 0, "the world must start with coin");
        for (int year = 0; year < kYearsChecked; ++year) {
            sim.run_years(1);
            require(total_coin(sim.state()) == starting, "total coin must never change as trade runs");
            for (const auto& settlement : sim.state().settlements) {
                require(settlement.coin >= 0, "a commons never goes into debt");
            }
            for (const auto& household : sim.state().households) {
                require(household.coin >= 0, "a household never goes into debt");
            }
        }
    }
}

void test_equivalent_advance_schedules_give_identical_trade_state() {
    constexpr int kDays = kDaysPerYear * 3;
    universe::Simulation day_by_day{default_layout_config(2)};
    universe::Simulation one_chunk{default_layout_config(2)};
    universe::Simulation uneven{default_layout_config(2)};
    for (int day = 0; day < kDays; ++day) {
        day_by_day.advance(universe::days(1));
    }
    one_chunk.advance(universe::days(kDays));
    int remaining = kDays;
    for (const int step : {1, 6, 29, 100, 7}) {
        uneven.advance(universe::days(step));
        remaining -= step;
    }
    uneven.advance(universe::days(remaining));
    require(fingerprint(day_by_day) == fingerprint(one_chunk), "day-by-day and one-chunk schedules must match");
    require(fingerprint(day_by_day) == fingerprint(uneven), "an uneven schedule must match as well");
}

// Saves at several points, including days that are not year boundaries, restores
// into a blank world and checks both continue identically afterwards.
void test_save_and_load_at_any_day_continues_identically() {
    for (std::uint64_t seed = 1; seed <= 2; ++seed) {
        for (const int day : {1, 90, 364, 366, 500, 900}) {
            universe::Simulation source{default_layout_config(seed)};
            source.advance(universe::days(day));
            const auto saved = source.save_snapshot();
            auto restored = restore(saved);
            require(restored.save_snapshot() == saved, "a reloaded world must save back the same bytes");
            require(all_prices(restored) == all_prices(source), "prices must survive reload");
            require(total_coin(restored.state()) == total_coin(source.state()), "coin must survive reload");

            for (int step = 0; step < 4; ++step) {
                source.advance(universe::days(200));
                restored.advance(universe::days(200));
                require(fingerprint(source) == fingerprint(restored), "a reloaded world must continue identically");
            }
        }
    }
}

// A chain of save and load cycles must equal one uninterrupted run.
void test_repeated_save_and_load_equals_an_uninterrupted_run() {
    for (std::uint64_t seed = 1; seed <= 2; ++seed) {
        universe::Simulation straight{default_layout_config(seed)};
        straight.advance(universe::days(kDaysPerYear * 6));

        universe::Simulation chained{default_layout_config(seed)};
        for (int leg = 0; leg < 6; ++leg) {
            chained.advance(universe::days(kDaysPerYear));
            chained = restore(chained.save_snapshot());
        }
        require(fingerprint(straight) == fingerprint(chained), "save/load every year must not change the outcome");
    }
}

void test_different_seeds_diverge() {
    universe::Simulation a{default_layout_config(1)};
    universe::Simulation b{default_layout_config(2)};
    a.run_years(2);
    b.run_years(2);
    require(
        all_prices(a) != all_prices(b) || total_coin(a.state()) != total_coin(b.state()),
        "different seeds must not collapse to one trade state");
}

// ---- PENDING #72: trip-specific determinism --------------------------------
// Fill these in once the trade trip mechanics land (MerchantTrip state, trade
// departed / trade completed events). Each stub states what it must assert.

// PENDING #72: find a seed and day at which at least one merchant is away.
//   - run twice from the same seed: trips (merchant, origin, destination, cargo,
//     outlay, departure, arrival, return, phase) are identical at every day.
//   - the trade departed and trade completed events are identical, in order.
void test_trips_are_deterministic_pending() {}

// PENDING #72: save while a trip is in each phase (outbound, at destination,
// returning), reload, and assert the restored world's trips equal the source's
// and that both reach the same completion day, coin and stock afterwards.
void test_save_and_load_mid_trip_continues_identically_pending() {}

// PENDING #72: total coin (settlements + households + any coin in transit on
// trips) is unchanged on every day of a trip, at departure and at completion,
// and a trade that would not profit is never made.
void test_coin_is_conserved_across_a_trip_pending() {}

// PENDING #72: a trip takes its real simulated days, so advancing in one chunk
// or day by day resolves it on the same day, never earlier.
void test_trips_take_their_real_days_pending() {}

void run_trip_tests() {
    if (!kTripMechanicsLanded) {
        std::cout << "SKIPPED: trip assertions pending #72 (trade trip mechanics)\n";
        return;
    }
    test_trips_are_deterministic_pending();
    test_save_and_load_mid_trip_continues_identically_pending();
    test_coin_is_conserved_across_a_trip_pending();
    test_trips_take_their_real_days_pending();
}

}  // namespace

int main() {
    test_same_seed_gives_identical_trade_state_every_year();
    test_prices_are_a_pure_function_of_state();
    test_total_coin_is_conserved_while_trade_runs();
    test_equivalent_advance_schedules_give_identical_trade_state();
    test_save_and_load_at_any_day_continues_identically();
    test_repeated_save_and_load_equals_an_uninterrupted_run();
    test_different_seeds_diverge();
    run_trip_tests();
    std::cout << "trade determinism tests passed\n";
    return 0;
}
