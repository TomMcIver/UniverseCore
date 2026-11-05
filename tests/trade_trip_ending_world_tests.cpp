// Trade trips end safely when the world changes under them (ADR 0003, ticket
// #74), at the headless Simulation seam. Each ending is injected into a world
// that really has a trip in flight, then checked for conservation: coin and
// goods are never created or destroyed, wherever they end up.
#include "settlement_test_support.hpp"

#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"
#include "universe/trade_trip.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <vector>

namespace universe {

// Test-only door into the private state, so a case can kill a merchant or
// retire a settlement at a chosen moment and then run the trade step directly.
struct SimulationTestAccess {
    static WorldState& state(Simulation& sim) { return sim.state_; }
    static void process_trade_period(Simulation& sim) { sim.process_trade_period(); }
    static void resolve_orphaned_trips(Simulation& sim) { sim.resolve_orphaned_trips(); }
    static void merge_settlements(Simulation& sim, const SettlementId survivor, const SettlementId absorbed) {
        sim.merge_settlements(survivor, absorbed);
    }
};

}  // namespace universe

namespace {

using namespace settlement_test;
using universe::SimulationTestAccess;

constexpr std::uint64_t kSweepSeeds = 12;
constexpr int kSearchDays = 365 * 12;
constexpr std::size_t kResourceKinds = 5;

std::int64_t stock_of(const universe::Settlement& settlement, const universe::ResourceType resource) {
    switch (resource) {
        case universe::ResourceType::Food:
            return settlement.commons_food.value;
        case universe::ResourceType::Wood:
            return settlement.materials.wood.value;
        case universe::ResourceType::Stone:
            return settlement.materials.stone.value;
        case universe::ResourceType::Timber:
            return settlement.materials.timber.value;
        case universe::ResourceType::BasicTool:
            return settlement.materials.basic_tool.value;
    }
    return 0;
}

// Every unit of one good: all settlement commons (active or not) plus what the
// carts hold.
std::int64_t goods_total(const universe::WorldState& state, const universe::ResourceType resource) {
    std::int64_t total = 0;
    for (const auto& settlement : state.settlements) {
        total += stock_of(settlement, resource);
    }
    for (const auto& trip : state.merchant_trips) {
        if (trip.resource == resource) {
            total += trip.phase == universe::TripPhase::Outbound ? trip.cargo_units : trip.unsold_units;
        }
    }
    return total;
}

struct Ledger {
    std::int64_t coin{};  // commons, households and what returning merchants carry
    std::array<std::int64_t, kResourceKinds> goods{};

    [[nodiscard]] bool operator==(const Ledger& other) const = default;
};

Ledger ledger_of(const universe::WorldState& state) {
    Ledger ledger{.coin = total_coin(state)};
    for (const auto& trip : state.merchant_trips) {
        ledger.coin += trip.coin_carried;
    }
    for (std::size_t index = 0; index < ledger.goods.size(); ++index) {
        ledger.goods[index] = goods_total(state, static_cast<universe::ResourceType>(index));
    }
    return ledger;
}

// Demotion folds household-held materials into the settlement commons, so commons
// stock may rise (a move between partitions); it must never fall, and coin is exact.
bool nothing_lost_to_a_level_of_detail_change(const Ledger& before, const Ledger& after) {
    if (after.coin != before.coin) {
        return false;
    }
    for (std::size_t index = 0; index < before.goods.size(); ++index) {
        if (after.goods[index] < before.goods[index]) {
            return false;
        }
    }
    return true;
}

// The first moment each seed has a trip in flight, as a snapshot, so every case
// starts from the identical world without re-simulating years.
const std::map<std::uint64_t, std::vector<std::byte>>& worlds_with_a_trip() {
    static const auto worlds = [] {
        std::map<std::uint64_t, std::vector<std::byte>> found;
        for (std::uint64_t seed = 1; seed <= kSweepSeeds; ++seed) {
            universe::Simulation sim{default_layout_config(seed)};
            for (int day = 0; day < kSearchDays; ++day) {
                sim.advance(universe::days(1));
                if (!sim.state().merchant_trips.empty()) {
                    found.emplace(seed, sim.save_snapshot());
                    break;
                }
            }
        }
        return found;
    }();
    return worlds;
}

universe::Simulation load_world(const std::uint64_t seed) {
    universe::Simulation sim{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(worlds_with_a_trip().at(seed), sim).ok, "the in-flight snapshot loads");
    return sim;
}

universe::Person* person_in(universe::WorldState& state, const universe::PersonId id) {
    for (auto& person : state.people) {
        if (person.id == id) {
            return &person;
        }
    }
    return nullptr;
}

universe::Settlement* settlement_in(universe::WorldState& state, const universe::SettlementId id) {
    for (auto& settlement : state.settlements) {
        if (settlement.id == id) {
            return &settlement;
        }
    }
    return nullptr;
}

// Jumps to a trip's arrival or return and runs the trade step.
void arrive(universe::Simulation& sim, const universe::PersonId merchant_id) {
    auto& state = SimulationTestAccess::state(sim);
    state.time = sim.trip_for_merchant(merchant_id)->arrives_at;
    SimulationTestAccess::process_trade_period(sim);
}

void come_home(universe::Simulation& sim, const universe::PersonId merchant_id) {
    auto& state = SimulationTestAccess::state(sim);
    state.time = sim.trip_for_merchant(merchant_id)->returns_at;
    SimulationTestAccess::process_trade_period(sim);
}

// Whether this very trip is still in flight (the merchant may set off on a fresh one the moment they are home).
bool trip_exists(const universe::Simulation& sim, const universe::PersonId merchant_id, const universe::SimulationTime departed_at) {
    const auto trip = sim.trip_for_merchant(merchant_id);
    return trip.has_value() && trip->departed_at == departed_at;
}

// ------------------------------------------------------------------- cases

void case_merchant_dies_outbound(const std::uint64_t seed) {
    auto sim = load_world(seed);
    auto& state = SimulationTestAccess::state(sim);
    const auto trip = state.merchant_trips.front();
    const auto before = ledger_of(state);
    const auto origin_stock = stock_of(*settlement_in(state, trip.origin_settlement_id), trip.resource);

    person_in(state, trip.merchant_id)->alive = false;
    SimulationTestAccess::resolve_orphaned_trips(sim);

    require(!trip_exists(sim, trip.merchant_id, trip.departed_at), "a dead merchant's trip ends");
    require(ledger_of(state) == before, "a merchant dying outbound creates and destroys neither coin nor goods");
    require(stock_of(*settlement_in(state, trip.origin_settlement_id), trip.resource) >= origin_stock + trip.cargo_units,
        "the cargo returns to the origin commons");
}

void case_merchant_dies_returning(const std::uint64_t seed) {
    auto sim = load_world(seed);
    auto& state = SimulationTestAccess::state(sim);
    const auto merchant_id = state.merchant_trips.front().merchant_id;
    arrive(sim, merchant_id);
    const auto trip = sim.trip_for_merchant(merchant_id);
    require(trip.has_value() && trip->phase == universe::TripPhase::Returning, "the merchant is on the way home");
    const auto before = ledger_of(state);

    person_in(state, merchant_id)->alive = false;
    SimulationTestAccess::resolve_orphaned_trips(sim);

    require(!trip_exists(sim, merchant_id, trip->departed_at), "the trip ends when the returning merchant dies");
    require(ledger_of(state) == before, "a merchant dying on the way home loses neither coin nor goods");
}

void case_destination_inactive_on_arrival(const std::uint64_t seed) {
    auto sim = load_world(seed);
    auto& state = SimulationTestAccess::state(sim);
    const auto trip = state.merchant_trips.front();
    const auto before = ledger_of(state);

    settlement_in(state, trip.destination_settlement_id)->active = false;
    arrive(sim, trip.merchant_id);

    const auto after_arrival = sim.trip_for_merchant(trip.merchant_id);
    require(after_arrival.has_value(), "the merchant still has to walk home");
    require(after_arrival->phase == universe::TripPhase::Returning, "the merchant turns back");
    require(after_arrival->unsold_units == trip.cargo_units, "the whole cargo comes home unsold");
    require(after_arrival->coin_carried == 0, "nothing was sold to a gone destination");
    require(ledger_of(state) == before, "turning back loses nothing");

    const auto origin_stock_before = stock_of(*settlement_in(state, trip.origin_settlement_id), trip.resource);
    come_home(sim, trip.merchant_id);
    require(!trip_exists(sim, trip.merchant_id, trip.departed_at), "the trip is over once home");
    require(ledger_of(state) == before, "arriving home with the cargo loses nothing");
    require(stock_of(*settlement_in(state, trip.origin_settlement_id), trip.resource) >= origin_stock_before,
        "the origin takes its cargo back");
}

void case_origin_inactive_on_return(const std::uint64_t seed) {
    auto sim = load_world(seed);
    auto& state = SimulationTestAccess::state(sim);
    const auto trip = state.merchant_trips.front();
    arrive(sim, trip.merchant_id);
    const auto before = ledger_of(state);
    const auto* origin = settlement_in(state, trip.origin_settlement_id);
    const auto origin_coin = origin->coin;
    const auto origin_stock = stock_of(*origin, trip.resource);

    settlement_in(state, trip.origin_settlement_id)->active = false;
    come_home(sim, trip.merchant_id);

    require(!trip_exists(sim, trip.merchant_id, trip.departed_at), "the trip ends even though home is gone");
    require(ledger_of(state) == before, "a lost origin loses neither coin nor goods");
    const auto* after = settlement_in(state, trip.origin_settlement_id);
    require(after->coin == origin_coin && stock_of(*after, trip.resource) == origin_stock,
        "nothing is delivered to a settlement that no longer exists");
}

void case_merchant_dies_and_origin_inactive(const std::uint64_t seed) {
    auto sim = load_world(seed);
    auto& state = SimulationTestAccess::state(sim);
    const auto trip = state.merchant_trips.front();
    const auto before = ledger_of(state);
    const auto origin_stock = stock_of(*settlement_in(state, trip.origin_settlement_id), trip.resource);

    person_in(state, trip.merchant_id)->alive = false;
    settlement_in(state, trip.origin_settlement_id)->active = false;
    SimulationTestAccess::resolve_orphaned_trips(sim);

    require(!trip_exists(sim, trip.merchant_id, trip.departed_at), "the trip ends");
    require(ledger_of(state) == before, "death plus a lost origin still loses nothing");
    require(stock_of(*settlement_in(state, trip.origin_settlement_id), trip.resource) == origin_stock,
        "the cargo goes to an active settlement, not the lost one");
}

void case_origin_absorbed_during_trip(const std::uint64_t seed) {
    auto sim = load_world(seed);
    auto& state = SimulationTestAccess::state(sim);
    const auto trip = state.merchant_trips.front();
    universe::SettlementId survivor = 0;
    for (const auto& settlement : state.settlements) {
        if (settlement.active && settlement.id != trip.origin_settlement_id && settlement.id != trip.destination_settlement_id) {
            survivor = settlement.id;
            break;
        }
    }
    if (survivor == 0) {
        return;  // two-settlement world: no third party to absorb the origin
    }
    const auto before = ledger_of(state);
    SimulationTestAccess::merge_settlements(sim, survivor, trip.origin_settlement_id);
    arrive(sim, trip.merchant_id);
    come_home(sim, trip.merchant_id);

    require(!trip_exists(sim, trip.merchant_id, trip.departed_at), "a trip of an absorbed origin still finishes");
    require(ledger_of(state) == before, "absorption mid-trip loses nothing");
    const auto* absorbed = settlement_in(state, trip.origin_settlement_id);
    require(!absorbed->active && absorbed->coin == 0 && stock_of(*absorbed, trip.resource) == 0,
        "the absorbed origin ends empty: everything went to the survivor");
}

void case_demote_and_promote_keep_the_trip(const std::uint64_t seed) {
    auto sim = load_world(seed);
    auto& state = SimulationTestAccess::state(sim);
    const auto trip = state.merchant_trips.front();
    const auto before = ledger_of(state);
    std::vector<universe::RegionId> regions;
    for (const auto& region : state.regions) {
        regions.push_back(region.id);
    }

    for (const auto region_id : regions) {
        sim.demote_region_to_settlement_aggregate(region_id);
    }
    require(sim.trip_for_merchant(trip.merchant_id) == trip, "demotion keeps the trip exactly");
    require(nothing_lost_to_a_level_of_detail_change(before, ledger_of(state)), "demotion loses neither coin nor goods");
    for (const auto region_id : regions) {
        sim.promote_region_to_detailed(region_id);
    }
    require(sim.trip_for_merchant(trip.merchant_id) == trip, "promotion keeps the trip exactly");
    require(nothing_lost_to_a_level_of_detail_change(before, ledger_of(state)), "a demote and promote cycle loses neither coin nor goods");

    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(sim.save_snapshot(), restored).ok, "the cycled world saves and loads");
    arrive(sim, trip.merchant_id);
    come_home(sim, trip.merchant_id);
    require(!trip_exists(sim, trip.merchant_id, trip.departed_at), "the preserved trip still finishes");
    require(nothing_lost_to_a_level_of_detail_change(before, ledger_of(state)), "finishing after the cycle loses nothing");
}

void case_endings_are_deterministic(const std::uint64_t seed) {
    const auto run = [&]() {
        auto sim = load_world(seed);
        auto& state = SimulationTestAccess::state(sim);
        const auto trip = state.merchant_trips.front();
        settlement_in(state, trip.destination_settlement_id)->active = false;
        arrive(sim, trip.merchant_id);
        settlement_in(state, trip.origin_settlement_id)->active = false;
        come_home(sim, trip.merchant_id);
        sim.advance(universe::days(30));
        return sim.save_snapshot();
    };
    require(run() == run(), "the same ending replays to the same bytes");
}

void test_every_ending_is_reached_across_seeds() {
    const auto& worlds = worlds_with_a_trip();
    require(worlds.size() >= 3, "enough seeds schedule a trip for the sweep to mean something");
    for (const auto& [seed, snapshot] : worlds) {
        (void)snapshot;
        case_merchant_dies_outbound(seed);
        case_merchant_dies_returning(seed);
        case_destination_inactive_on_arrival(seed);
        case_origin_inactive_on_return(seed);
        case_merchant_dies_and_origin_inactive(seed);
        case_origin_absorbed_during_trip(seed);
        case_demote_and_promote_keep_the_trip(seed);
        case_endings_are_deterministic(seed);
    }
}

}  // namespace

int main() {
    test_every_ending_is_reached_across_seeds();
    std::cout << "trade_trip_ending_world_tests passed\n";
    return 0;
}
