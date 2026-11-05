// Trade is deterministic and resumable (ADR 0003, ticket #75). Everything runs at
// the headless Simulation seam: the same seed must give the same prices, merchants
// and coin however the run is scheduled, and a snapshot taken at any point must
// continue byte for byte like the world it came from.
//
// The trip tests (mid-trip schedules, save/load and conservation) run against the
// real merchant trips of ticket #72.
#include "settlement_test_support.hpp"

#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"
#include "universe/trade_trip.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace settlement_test;

constexpr std::uint64_t kSeedsChecked = 4;
constexpr int kYearsChecked = 8;
constexpr int kDaysPerYear = 365;

constexpr std::uint64_t kSweepSeeds = 6;
constexpr int kSweepLegs = 8;
constexpr int kSweepLegDays = 200;

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

// ---- trade trips -----------------------------------------------------------

using TradeLog = std::vector<std::pair<universe::SimulationEvent, universe::TradeEventPayload>>;

TradeLog trade_events(const universe::WorldState& state) {
    TradeLog log;
    for (const auto& event : state.event_log) {
        if (event.type == universe::EventType::TradeDeparted || event.type == universe::EventType::TradeCompleted) {
            log.emplace_back(event, std::get<universe::TradeEventPayload>(event.payload));
        }
    }
    return log;
}

bool same_trade_log(const TradeLog& a, const TradeLog& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].first.id != b[i].first.id || a[i].first.timestamp.ticks != b[i].first.timestamp.ticks ||
            a[i].first.type != b[i].first.type || !(a[i].second == b[i].second)) {
            return false;
        }
    }
    return true;
}

// Coin the merchants carry between settlements: it has left the destination and
// not yet reached the origin, so it still belongs to the world's total.
std::int64_t coin_in_transit(const universe::WorldState& state) {
    std::int64_t carried = 0;
    for (const auto& trip : state.merchant_trips) {
        carried += trip.coin_carried;
    }
    return carried;
}

struct TripWorld {
    std::uint64_t seed{};
    int first_trip_day{};  // whole days advanced when a trip was first in flight
};

constexpr int kTripSearchDays = kDaysPerYear * 12;
constexpr std::uint64_t kTripSeedsSearched = 12;

// First seed (and the day) at which a merchant is away; every run is deterministic
// so this is stable.
std::optional<TripWorld> find_trip_world() {
    for (std::uint64_t seed = 1; seed <= kTripSeedsSearched; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        for (int day = 1; day <= kTripSearchDays; ++day) {
            sim.advance(universe::days(1));
            if (!sim.state().merchant_trips.empty()) {
                return TripWorld{seed, day};
            }
        }
    }
    return std::nullopt;
}

const TripWorld& trip_world() {
    static const auto cached = find_trip_world();
    require(cached.has_value(), "some seed must send a merchant on a trip");
    return *cached;
}

void test_trips_are_deterministic() {
    const auto& world = trip_world();
    universe::Simulation first{default_layout_config(world.seed)};
    universe::Simulation second{default_layout_config(world.seed)};
    std::uint64_t days_with_trips = 0;
    for (int day = 0; day < kDaysPerYear * 4; ++day) {
        first.advance(universe::days(1));
        second.advance(universe::days(1));
        require(first.state().merchant_trips == second.state().merchant_trips, "trips must be identical on every day");
        days_with_trips += first.state().merchant_trips.empty() ? 0U : 1U;
    }
    require(days_with_trips > 0, "the run must have had trips in flight");
    require(same_trade_log(trade_events(first.state()), trade_events(second.state())), "trade events must be identical and in order");
    require(!trade_events(first.state()).empty(), "the run must have produced trade events");
    require(fingerprint(first) == fingerprint(second), "the whole world must match");
}

// Advance one world a day at a time and others by the same total in uneven chunks
// or one step, from the very beginning, with trips in flight along the way.
void test_equivalent_schedules_agree_with_trips_in_flight() {
    const auto& world = trip_world();
    const int total = world.first_trip_day + (kDaysPerYear * 2);
    universe::Simulation daily{default_layout_config(world.seed)};
    universe::Simulation chunked{default_layout_config(world.seed)};
    universe::Simulation single{default_layout_config(world.seed)};
    for (int day = 0; day < total; ++day) {
        daily.advance(universe::days(1));
    }
    int remaining = total;
    for (const int step : {world.first_trip_day - 1, 1, 2, 5, 11, 40}) {
        chunked.advance(universe::days(step));
        remaining -= step;
    }
    chunked.advance(universe::days(remaining));
    single.advance(universe::days(total));
    require(fingerprint(daily) == fingerprint(chunked), "uneven chunks must match day by day with trips in flight");
    require(fingerprint(daily) == fingerprint(single), "one big step must match day by day with trips in flight");
    require(same_trade_log(trade_events(daily.state()), trade_events(chunked.state())), "chunked schedule must give the same trade events");
    require(same_trade_log(trade_events(daily.state()), trade_events(single.state())), "single step must give the same trade events");
    require(!trade_events(daily.state()).empty(), "the compared run must contain trades");
}

// Steps day by day until some trip is in the wanted phase; the world stays
// mid-trip when this returns true.
bool advance_to_phase(universe::Simulation& sim, const universe::TripPhase phase) {
    for (int day = 0; day < kTripSearchDays; ++day) {
        sim.advance(universe::days(1));
        for (const auto& trip : sim.state().merchant_trips) {
            if (trip.phase == phase) {
                return true;
            }
        }
    }
    return false;
}

void test_save_and_load_mid_trip_continues_identically() {
    const auto& world = trip_world();
    int phases_tested = 0;
    for (const auto phase : {universe::TripPhase::Outbound, universe::TripPhase::Returning}) {
        universe::Simulation source{default_layout_config(world.seed)};
        if (!advance_to_phase(source, phase)) {
            std::cout << "note: no trip is ever observed in phase " << static_cast<int>(phase) << " at a period boundary\n";
            continue;
        }
        ++phases_tested;
        const auto saved = source.save_snapshot();
        auto restored = restore(saved);
        require(restored.state().merchant_trips == source.state().merchant_trips, "trips must survive a reload");
        require(restored.save_snapshot() == saved, "a mid-trip snapshot must round-trip byte for byte");

        for (int day = 0; day < 20; ++day) {
            source.advance(universe::days(1));
            restored.advance(universe::days(1));
            require(
                source.state().merchant_trips == restored.state().merchant_trips,
                "a reloaded world must carry its trips identically day by day");
            require(fingerprint(source) == fingerprint(restored), "a reloaded world must match the original while trips finish");
        }
        source.advance(universe::days(kDaysPerYear * 2));
        restored.advance(universe::days(kDaysPerYear * 2));
        require(fingerprint(source) == fingerprint(restored), "a reloaded world must match the original long after");
        require(same_trade_log(trade_events(source.state()), trade_events(restored.state())), "trade events must match after reload");
    }
    require(phases_tested > 0, "at least one trip phase must be observable mid-trip");
}

// Saving and reloading every single day across a trip equals one clean run.
void test_daily_save_and_load_equals_an_uninterrupted_run() {
    const auto& world = trip_world();
    const int total = world.first_trip_day + 40;
    universe::Simulation straight{default_layout_config(world.seed)};
    straight.advance(universe::days(total));

    universe::Simulation chained{default_layout_config(world.seed)};
    for (int day = 0; day < total; ++day) {
        chained.advance(universe::days(1));
        chained = restore(chained.save_snapshot());
    }
    require(fingerprint(straight) == fingerprint(chained), "a save and load every day must not change the outcome");
}

void require_trip_is_internally_consistent(const universe::MerchantTrip& trip) {
    require(trip.cargo_units > 0 && trip.coin_outlay >= 0, "a trip carries a real cargo");
    require(
        trip.coin_carried >= 0 && trip.unsold_units >= 0 && trip.unsold_units <= trip.cargo_units,
        "carried goods and coin stay within the cargo");
    require(
        trip.arrives_at.ticks > trip.departed_at.ticks && trip.returns_at.ticks >= trip.arrives_at.ticks, "a trip takes real time");
    if (trip.phase == universe::TripPhase::Outbound) {
        require(trip.coin_carried == 0 && trip.unsold_units == 0, "nothing comes home before arrival");
    }
}

void test_coin_and_goods_are_conserved_across_trips() {
    const auto& world = trip_world();
    universe::Simulation sim{default_layout_config(world.seed)};
    const auto starting = total_coin(sim.state());
    std::uint64_t completions = 0;
    std::size_t events_seen = 0;
    for (int day = 0; day < kDaysPerYear * 6; ++day) {
        sim.advance(universe::days(1));
        const auto& state = sim.state();
        require(
            total_coin(state) + coin_in_transit(state) == starting,
            "settlement, household and in-transit coin must always equal the endowment");
        for (const auto& trip : state.merchant_trips) {
            require_trip_is_internally_consistent(trip);
        }
        for (; events_seen < state.event_log.size(); ++events_seen) {
            const auto& event = state.event_log[events_seen];
            if (event.type != universe::EventType::TradeCompleted) {
                continue;
            }
            ++completions;
            const auto& payload = std::get<universe::TradeEventPayload>(event.payload);
            std::optional<universe::TradeEventPayload> departure;
            for (std::size_t i = 0; i < events_seen; ++i) {
                const auto& candidate = state.event_log[i];
                if (candidate.type == universe::EventType::TradeDeparted &&
                    std::get<universe::TradeEventPayload>(candidate.payload).merchant_id == payload.merchant_id) {
                    departure = std::get<universe::TradeEventPayload>(candidate.payload);
                }
            }
            require(departure.has_value(), "every completed trade has a departure");
            require(departure->cargo_units == payload.cargo_units, "the goods that left are the goods accounted for");
            require(departure->coin_profit >= universe::kMinimumTripProfitCoin, "a trade is only made when it is expected to profit");
        }
    }
    require(completions > 0, "the run must complete trades");
}

// A trip resolves when its travel time has elapsed: stepping a day at a time and
// jumping the whole way land on the same world.
void test_trip_resolves_on_its_real_day_however_it_is_advanced() {
    const auto& world = trip_world();
    universe::Simulation stepped{default_layout_config(world.seed)};
    stepped.advance(universe::days(world.first_trip_day));
    const auto trip = stepped.state().merchant_trips.front();
    universe::Simulation jumped = restore(stepped.save_snapshot());

    auto still_away = [&trip](const universe::Simulation& sim) {
        return std::any_of(sim.state().merchant_trips.begin(), sim.state().merchant_trips.end(), [&trip](const universe::MerchantTrip& t) {
            return t.merchant_id == trip.merchant_id && t.departed_at.ticks == trip.departed_at.ticks;
        });
    };
    int days_taken = 0;
    while (still_away(stepped)) {
        require(stepped.state().time.ticks < trip.returns_at.ticks, "the trip must resolve once its return time has passed");
        stepped.advance(universe::days(1));
        ++days_taken;
        require(days_taken <= 10, "a trip is at most a week or so");
    }
    require(stepped.state().time.ticks >= trip.returns_at.ticks, "the merchant is never home before the return time");
    require(
        stepped.state().time.ticks - universe::days(1).ticks < trip.returns_at.ticks,
        "the trip resolves within a day of its return time");

    jumped.advance(universe::days(days_taken));
    require(fingerprint(stepped) == fingerprint(jumped), "one jump over a trip equals stepping through it");
}

void test_multi_seed_sweep_is_deterministic_and_conserving() {
    std::uint64_t seeds_with_trades = 0;
    for (std::uint64_t seed = 1; seed <= kSweepSeeds; ++seed) {
        universe::Simulation chunked{default_layout_config(seed)};
        universe::Simulation daily{default_layout_config(seed)};
        const auto starting = total_coin(chunked.state());
        for (int leg = 0; leg < kSweepLegs; ++leg) {
            chunked.advance(universe::days(kSweepLegDays));
            for (int day = 0; day < kSweepLegDays; ++day) {
                daily.advance(universe::days(1));
            }
            require(fingerprint(chunked) == fingerprint(daily), "chunked and daily schedules must agree for every seed");
            require(
                total_coin(chunked.state()) + coin_in_transit(chunked.state()) == starting, "coin must be conserved for every seed");
        }
        seeds_with_trades += trade_events(chunked.state()).empty() ? 0U : 1U;
        require(same_trade_log(trade_events(chunked.state()), trade_events(daily.state())), "trade events must agree for every seed");
    }
    require(seeds_with_trades > 0, "at least one swept seed must trade");
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
    test_trips_are_deterministic();
    test_equivalent_schedules_agree_with_trips_in_flight();
    test_save_and_load_mid_trip_continues_identically();
    test_daily_save_and_load_equals_an_uninterrupted_run();
    test_coin_and_goods_are_conserved_across_trips();
    test_trip_resolves_on_its_real_day_however_it_is_advanced();
    test_multi_seed_sweep_is_deterministic_and_conserving();
    std::cout << "trade determinism tests passed\n";
    return 0;
}
