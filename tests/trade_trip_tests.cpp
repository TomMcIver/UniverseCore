// A merchant completes a trade trip (ADR 0003, ticket #72). The arithmetic is
// tested on its own; the rest runs at the headless Simulation seam.
#include "settlement_test_support.hpp"
#include "snapshot_test_support.hpp"

#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"
#include "universe/trade_trip.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace {

using namespace settlement_test;
using universe::TradeCapability;

constexpr std::int64_t kSpeed = universe::kWalkingUnitsPerDay;

// ---------------------------------------------------------------- arithmetic

void test_distance_is_the_straight_line_rounded_up() {
    require(universe::trade_distance_units(0, 0, 3'000, 4'000) == 5'000, "a 3-4-5 triangle is exactly 5000 units");
    require(universe::trade_distance_units(0, 0, 1, 1) == 2, "a diagonal of 1.41 units rounds up to 2");
    require(universe::trade_distance_units(-2'000, -1'000, 2'000, -1'000) == 4'000, "crossing the origin counts the full span");
    require(universe::trade_distance_units(5, 5, 5, 5) == 0, "no distance between a point and itself");
}

void test_travel_takes_real_time_rounded_up() {
    const auto day = universe::days(1).ticks;
    require(universe::travel_ticks(kSpeed, kSpeed) == day, "one day's walk takes one day");
    require(universe::travel_ticks(500, kSpeed) == day / 2, "half a day's walk takes half a day");
    require(universe::travel_ticks(1, kSpeed) == 87, "a single unit rounds up to whole seconds (86.4)");
    require(universe::travel_ticks(0, kSpeed) == 1, "a trip never takes zero time");
    require(universe::travel_ticks(100, 0) == std::numeric_limits<std::int64_t>::max(), "no speed never arrives");
}

void test_reach_is_three_days_one_way_and_scales_with_speed() {
    const TradeCapability walking = universe::baseline_trade_capability();
    require(walking.speed_units_per_day == kSpeed && walking.cargo_multiplier_permille == 1'000, "the baseline is walking pace and a load of one");
    require(universe::trip_within_reach(3 * kSpeed, walking), "exactly three days one way is allowed");
    require(!universe::trip_within_reach((3 * kSpeed) + 1, walking), "a unit beyond three days is refused");

    const TradeCapability doubled{.speed_units_per_day = 2 * kSpeed, .cargo_multiplier_permille = 1'000};
    require(universe::trip_within_reach(6 * kSpeed, doubled), "doubling the speed doubles the reach");
    require(!universe::trip_within_reach((6 * kSpeed) + 1, doubled), "the doubled reach has the same sharp edge");
    require(!universe::trip_within_reach(6 * kSpeed, walking), "the doubled reach is out of walking range");
}

void test_cargo_capacity_scales_with_the_multiplier() {
    const TradeCapability walking = universe::baseline_trade_capability();
    const TradeCapability doubled{.speed_units_per_day = kSpeed, .cargo_multiplier_permille = 2'000};
    require(universe::cargo_capacity_units(walking) == universe::kBaseCargoUnits, "the baseline load is the base load");
    require(universe::cargo_capacity_units(doubled) == 2 * universe::kBaseCargoUnits, "doubling the load doubles the cargo");
    require(universe::cargo_capacity_units(TradeCapability{.speed_units_per_day = kSpeed, .cargo_multiplier_permille = 0}) == 0, "no load carries nothing");
}

void test_rounding_never_creates_coin() {
    // 7 units at 1.5 coin: the buyer pays 11 (10.5 up), the seller receives 10 (10.5 down).
    require(universe::trade_outlay_coin(7, 1'500) == 11, "the outlay rounds up");
    require(universe::trade_revenue_coin(7, 1'500) == 10, "the revenue rounds down");
    require(universe::trade_outlay_coin(4, 2'500) == universe::trade_revenue_coin(4, 2'500), "exact amounts agree");
    require(universe::units_affordable(10, 1'500) == 6, "ten coin buys six units at 1.5 (6.66 down)");
    require(universe::trade_outlay_coin(universe::units_affordable(10, 1'500), 1'500) <= 10, "what is affordable can always be paid");
}

void test_a_trip_that_does_not_pay_is_never_offered() {
    const auto walking = universe::baseline_trade_capability();
    require(!universe::evaluate_trip(10'000, 10'000, 1'000, 10'000, walking).has_value(), "equal prices do not pay");
    require(!universe::evaluate_trip(10'000, 9'000, 1'000, 10'000, walking).has_value(), "a lower destination price does not pay");
    // Cheaper than one coin of profit after rounding: 1 unit would be 10.000 against 10.001.
    require(!universe::evaluate_trip(10'000, 10'001, 4, 1'000, walking).has_value(), "a profit under one coin is not worth the trip");
    require(!universe::evaluate_trip(10'000, 20'000, 0, 1'000, walking).has_value(), "no stock, no trip");
    require(!universe::evaluate_trip(10'000, 20'000, 1'000, 0, walking).has_value(), "a commons with no coin cannot fund a cargo");
    require(!universe::evaluate_trip(0, 20'000, 1'000, 1'000, walking).has_value(), "a free origin price is not a price");
}

void test_a_profitable_trip_is_sized_by_load_stock_and_coin() {
    const auto walking = universe::baseline_trade_capability();
    const auto rich = universe::evaluate_trip(10'000, 20'000, 10'000, 1'000'000, walking);
    require(rich.has_value(), "a doubled price pays");
    require(rich->cargo_units == universe::kBaseCargoUnits, "plenty of stock and coin is limited by the load");
    require(rich->coin_outlay == 500 && rich->expected_revenue == 1'000 && rich->expected_profit == 500, "50 units bought at 10 and sold at 20");

    const auto doubled = universe::evaluate_trip(
        10'000, 20'000, 10'000, 1'000'000, TradeCapability{.speed_units_per_day = kSpeed, .cargo_multiplier_permille = 2'000});
    require(doubled.has_value() && doubled->cargo_units == 2 * rich->cargo_units, "doubling the load doubles the cargo");

    const auto thin_stock = universe::evaluate_trip(10'000, 20'000, 100, 1'000'000, walking);
    require(thin_stock.has_value() && thin_stock->cargo_units == 25, "a commons sends at most a quarter of its stock");

    const auto poor = universe::evaluate_trip(10'000, 20'000, 10'000, 100, walking);
    require(poor.has_value() && poor->cargo_units == 10 && poor->coin_outlay <= 100, "a commons can only fund what its coin pays for");
}

void test_proceeds_split_conserves_every_coin() {
    for (std::int64_t outlay = 0; outlay <= 40; outlay += 3) {
        for (std::int64_t carried = 0; carried <= 90; ++carried) {
            const auto split = universe::split_trade_proceeds(carried, outlay);
            require(split.household_share + split.commons_return == carried, "the household and the commons always receive exactly what came home");
            require(split.household_share >= 0 && split.commons_return >= 0, "neither side is ever charged");
            if (carried <= outlay) {
                require(split.household_share == 0, "a loss earns the household nothing");
            }
        }
    }
    const auto gain = universe::split_trade_proceeds(150, 100);
    require(gain.household_share == 10 && gain.commons_return == 140, "a 50 coin profit gives the household 20 percent");
    const auto crumb = universe::split_trade_proceeds(104, 100);
    require(crumb.household_share == 0 && crumb.commons_return == 104, "a share that rounds to nothing stays with the commons");
}

// ---------------------------------------------------------------- simulation

// Steps a world one day at a time until some merchant is on a trip. Returns the
// settlements as they stood the day before, so a departure can be audited.
bool run_to_first_trip(universe::Simulation& sim, std::vector<universe::Settlement>& before_departure) {
    for (int day = 0; day < 365 * 12; ++day) {
        before_departure = sim.state().settlements;
        sim.advance(universe::days(1));
        if (!sim.state().merchant_trips.empty()) {
            return true;
        }
    }
    return false;
}

std::optional<std::uint64_t> seed_with_a_trip() {
    static std::optional<std::optional<std::uint64_t>> cached;
    if (cached.has_value()) {
        return *cached;
    }
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        universe::Simulation sim{default_layout_config(seed)};
        std::vector<universe::Settlement> before;
        if (run_to_first_trip(sim, before)) {
            cached = seed;
            return seed;
        }
    }
    cached = std::optional<std::uint64_t>{};
    return std::nullopt;
}

const universe::Settlement* settlement_in(const std::vector<universe::Settlement>& settlements, const universe::SettlementId id) {
    for (const auto& settlement : settlements) {
        if (settlement.id == id) {
            return &settlement;
        }
    }
    return nullptr;
}

std::int64_t stock_in(const universe::Settlement& settlement, const universe::ResourceType resource) {
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

const universe::Household* household_of_person(const universe::WorldState& state, const universe::PersonId person_id) {
    for (const auto& household : state.households) {
        if (std::find(household.member_ids.begin(), household.member_ids.end(), person_id) != household.member_ids.end()) {
            return &household;
        }
    }
    return nullptr;
}

const universe::SimulationEvent* last_trade_event(const universe::WorldState& state, const universe::EventType type) {
    for (auto it = state.event_log.rbegin(); it != state.event_log.rend(); ++it) {
        if (it->type == type) {
            return &*it;
        }
    }
    return nullptr;
}

void test_a_departure_takes_the_cargo_and_records_the_event() {
    const auto seed = seed_with_a_trip();
    require(seed.has_value(), "trade must pay somewhere: a world must schedule a trip");
    universe::Simulation sim{default_layout_config(*seed)};
    std::vector<universe::Settlement> before;
    require(run_to_first_trip(sim, before), "the world schedules a trip");

    const auto trip = sim.state().merchant_trips.front();
    require(sim.is_merchant_away(trip.merchant_id), "a merchant with a trip is away");
    require(sim.trip_for_merchant(trip.merchant_id) == trip, "the trip is readable by merchant id");
    require(trip.phase == universe::TripPhase::Outbound, "a new trip starts outbound");
    require(trip.origin_settlement_id != trip.destination_settlement_id, "a trip runs between two settlements");
    require(trip.departed_at.ticks == sim.time().ticks || trip.departed_at.ticks <= sim.time().ticks, "departure is not in the future");
    require(trip.departed_at < trip.arrives_at && trip.arrives_at < trip.returns_at, "the timeline runs forward");
    require(trip.coin_carried == 0 && trip.unsold_units == 0, "nothing comes home before arrival");

    const auto* origin_before = settlement_in(before, trip.origin_settlement_id);
    const auto* origin_after = settlement_in(sim.state().settlements, trip.origin_settlement_id);
    require(origin_before != nullptr && origin_after != nullptr, "the origin exists");
    // Other systems also move stock on the departure day, so the cargo is checked
    // against the settlement's own commons accounting: it was held, and it left.
    require(stock_in(*origin_before, trip.resource) >= trip.cargo_units, "the origin held the cargo it sent");
    require(trip.cargo_units > 0 && trip.cargo_units <= universe::kBaseCargoUnits, "the cargo is within the walking load");
    require(trip.coin_outlay <= origin_before->coin, "the commons could fund the cargo");

    const auto* event = last_trade_event(sim.state(), universe::EventType::TradeDeparted);
    require(event != nullptr, "a departure is recorded");
    require(event->timestamp.ticks == trip.departed_at.ticks, "the event is stamped with the departure time");
    const auto& payload = std::get<universe::TradeEventPayload>(event->payload);
    require(payload.merchant_id == trip.merchant_id && payload.origin_settlement_id == trip.origin_settlement_id &&
                payload.destination_settlement_id == trip.destination_settlement_id && payload.resource == trip.resource &&
                payload.cargo_units == trip.cargo_units,
            "the departed event names the merchant, both settlements and the cargo");
    require(payload.coin_profit >= universe::kMinimumTripProfitCoin, "a trip is only scheduled when it is expected to profit");

    const auto distance = universe::trade_distance_units(
        origin_after->position_x,
        origin_after->position_z,
        settlement_in(sim.state().settlements, trip.destination_settlement_id)->position_x,
        settlement_in(sim.state().settlements, trip.destination_settlement_id)->position_z);
    require(universe::trip_within_reach(distance, universe::baseline_trade_capability()), "a trip is within three days one way");
    require(trip.arrives_at.ticks - trip.departed_at.ticks == universe::travel_ticks(distance, kSpeed), "the trip takes its real travel time");
}

// Everything that must hold on every day of a run with trade.
void require_trade_invariants(const universe::Simulation& sim, const std::int64_t coin_total) {
    const auto& state = sim.state();
    require(total_coin(state) == coin_total, "total coin never changes");
    std::int64_t previous_merchant = 0;
    for (const auto& trip : state.merchant_trips) {
        require(static_cast<std::int64_t>(trip.merchant_id) > previous_merchant, "trips are sorted by merchant id, one per merchant");
        previous_merchant = static_cast<std::int64_t>(trip.merchant_id);
        const universe::Person* merchant = nullptr;
        for (const auto& person : state.people) {
            if (person.id == trip.merchant_id) {
                merchant = &person;
            }
        }
        require(merchant != nullptr && merchant->alive && merchant->occupation == universe::Occupation::Merchant, "a trip belongs to a living merchant");
        // The schedule runs just before the trade system, so only the departure tick itself is not yet Travel.
        require(
            merchant->current_activity == universe::ActivityKind::Travel || state.time.ticks == trip.departed_at.ticks,
            "a merchant on a trip spends their days travelling, doing no settlement work");
    }
    for (const auto& settlement : state.settlements) {
        require(settlement.coin >= 0 && settlement.commons_food.value >= 0, "no balance goes negative");
    }
}

void test_a_trip_takes_its_time_and_the_world_conserves_coin() {
    const auto seed = seed_with_a_trip();
    require(seed.has_value(), "a world must schedule a trip");
    universe::Simulation sim{default_layout_config(*seed)};
    const auto coin_total = total_coin(sim.state());

    std::uint64_t departures = 0;
    std::uint64_t completions = 0;
    std::size_t events_seen = 0;
    for (int day = 0; day < 365 * 8; ++day) {
        const auto households_before = sim.state().households;
        sim.advance(universe::days(1));
        require_trade_invariants(sim, coin_total);

        for (; events_seen < sim.state().event_log.size(); ++events_seen) {
            const auto& event = sim.state().event_log[events_seen];
            if (event.type == universe::EventType::TradeDeparted) {
                ++departures;
            } else if (event.type == universe::EventType::TradeCompleted) {
                ++completions;
                const auto& payload = std::get<universe::TradeEventPayload>(event.payload);
                // Find the matching departure: the latest earlier one for this merchant.
                const universe::SimulationEvent* departure = nullptr;
                for (std::size_t i = 0; i < events_seen; ++i) {
                    const auto& candidate = sim.state().event_log[i];
                    if (candidate.type == universe::EventType::TradeDeparted &&
                        std::get<universe::TradeEventPayload>(candidate.payload).merchant_id == payload.merchant_id) {
                        departure = &candidate;
                    }
                }
                require(departure != nullptr, "a completed trade has a departure");
                const auto& departed = std::get<universe::TradeEventPayload>(departure->payload);
                require(departed.origin_settlement_id == payload.origin_settlement_id &&
                            departed.destination_settlement_id == payload.destination_settlement_id &&
                            departed.resource == payload.resource && departed.cargo_units == payload.cargo_units,
                        "the completed event names the same merchant, settlements and cargo as the departure");
                const auto origin = settlement_in(sim.state().settlements, payload.origin_settlement_id);
                const auto destination = settlement_in(sim.state().settlements, payload.destination_settlement_id);
                require(origin != nullptr && destination != nullptr, "both settlements exist");
                const auto distance = universe::trade_distance_units(
                    origin->position_x, origin->position_z, destination->position_x, destination->position_z);
                const auto round_trip = 2 * universe::travel_ticks(distance, kSpeed);
                const auto elapsed = event.timestamp.ticks - departure->timestamp.ticks;
                require(elapsed >= round_trip, "a trip never finishes before its real round-trip time");
                require(elapsed < round_trip + universe::days(1).ticks, "a trip finishes on the first daily tick after it is due");
                // The merchant is home again, and may set out on a new trip the same tick.
                const auto next_trip = sim.trip_for_merchant(payload.merchant_id);
                require(!next_trip.has_value() || next_trip->departed_at.ticks >= event.timestamp.ticks, "a completed trip is replaced only by a later one");
            }
        }
    }
    require(departures > 0U, "the world schedules trips");
    require(completions > 0U, "trips complete");
    require(completions <= departures, "no trip completes without departing");
}

void test_the_household_share_and_commons_return_follow_the_split() {
    const auto seed = seed_with_a_trip();
    require(seed.has_value(), "a world must schedule a trip");
    universe::Simulation sim{default_layout_config(*seed)};
    std::uint64_t checked = 0;
    for (int day = 0; day < 365 * 8 && checked < 3U; ++day) {
        const auto households_before = sim.state().households;
        const auto trips_before = sim.state().merchant_trips;
        sim.advance(universe::days(1));
        for (const auto& trip : trips_before) {
            const universe::SimulationEvent* event = nullptr;
            for (auto it = sim.state().event_log.rbegin(); it != sim.state().event_log.rend() && it->timestamp.ticks == sim.time().ticks; ++it) {
                if (it->type == universe::EventType::TradeCompleted &&
                    std::get<universe::TradeEventPayload>(it->payload).merchant_id == trip.merchant_id) {
                    event = &*it;
                }
            }
            if (event == nullptr) {
                continue;  // abandoned instead of completed (the merchant died or was folded)
            }
            const auto* household_after = household_of_person(sim.state(), trip.merchant_id);
            require(household_after != nullptr, "the merchant has a household");
            std::int64_t coin_before = -1;
            for (const auto& household : households_before) {
                if (household.id == household_after->id) {
                    coin_before = household.coin;
                }
            }
            require(coin_before >= 0, "the household existed before");
            const auto carried = std::get<universe::TradeEventPayload>(event->payload).coin_profit + trip.coin_outlay;
            const auto split = universe::split_trade_proceeds(carried, trip.coin_outlay);
            require(household_after->coin - coin_before == split.household_share, "the household keeps exactly its share of the profit");
            ++checked;
        }
    }
    require(checked > 0U, "at least one returning trip was audited");
}

void test_equivalent_time_advance_schedules_give_identical_worlds_with_trade() {
    const auto seed = seed_with_a_trip();
    require(seed.has_value(), "a world must schedule a trip");
    const auto total_days = 365 * 5;
    universe::Simulation day_by_day{default_layout_config(*seed)};
    universe::Simulation weekly{default_layout_config(*seed)};
    universe::Simulation one_chunk{default_layout_config(*seed)};
    for (int day = 0; day < total_days; ++day) {
        day_by_day.advance(universe::days(1));
    }
    for (int week = 0; week < total_days / 7; ++week) {
        weekly.advance(universe::days(7));
    }
    weekly.advance(universe::days(total_days % 7));
    one_chunk.advance(universe::days(total_days));
    const auto reference = one_chunk.save_snapshot();
    require(day_by_day.save_snapshot() == reference, "day by day equals one chunk, trips included");
    require(weekly.save_snapshot() == reference, "weekly equals one chunk, trips included");
    require(std::any_of(reference.begin(), reference.end(), [](std::byte) { return true; }), "the snapshot is not empty");
}

void test_same_seed_same_trips() {
    const auto seed = seed_with_a_trip();
    require(seed.has_value(), "a world must schedule a trip");
    universe::Simulation first{default_layout_config(*seed)};
    universe::Simulation second{default_layout_config(*seed)};
    first.run_years(6);
    second.run_years(6);
    require(first.state().merchant_trips == second.state().merchant_trips, "the same seed gives the same trips");
    require(first.state().event_log.size() == second.state().event_log.size(), "the same seed gives the same events");
    require(first.save_snapshot() == second.save_snapshot(), "the same seed gives the same snapshot");
}

void test_save_and_load_mid_trip_continues_identically() {
    const auto seed = seed_with_a_trip();
    require(seed.has_value(), "a world must schedule a trip");
    universe::Simulation source{default_layout_config(*seed)};
    std::vector<universe::Settlement> before;
    require(run_to_first_trip(source, before), "the world schedules a trip");
    source.advance(universe::days(1));  // in flight, not yet at the departure tick
    require(!source.state().merchant_trips.empty(), "a trip is still in flight");
    const auto saved = source.save_snapshot();

    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto result = universe::Simulation::load_snapshot(saved, restored);
    if (!result.ok) {
        std::cerr << "load error: " << result.message << '\n';
    }
    require(result.ok, "a snapshot taken mid-trip must load");
    require(restored.state().merchant_trips == source.state().merchant_trips, "the trip survives a reload");
    require(restored.save_snapshot() == saved, "a mid-trip snapshot round-trips byte for byte");

    source.run_years(2);
    restored.run_years(2);
    require(source.save_snapshot() == restored.save_snapshot(), "a reloaded world finishes the trip exactly as the original does");
}

void test_demoting_every_region_ends_trips_without_losing_coin() {
    const auto seed = seed_with_a_trip();
    require(seed.has_value(), "a world must schedule a trip");
    universe::Simulation sim{default_layout_config(*seed)};
    std::vector<universe::Settlement> before;
    require(run_to_first_trip(sim, before), "the world schedules a trip");
    const auto coin = total_coin(sim.state());
    std::vector<universe::RegionId> regions;
    for (const auto& region : sim.state().regions) {
        regions.push_back(region.id);
    }
    for (const auto region_id : regions) {
        sim.demote_region_to_settlement_aggregate(region_id);
    }
    require(sim.state().merchant_trips.empty(), "folded merchants end their trips");
    require(total_coin(sim.state()) == coin, "ending a trip never changes total coin");
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(sim.save_snapshot(), restored).ok, "a demoted world saves and loads");
}

void test_a_v28_snapshot_loads_with_every_merchant_at_home() {
    universe::Simulation source{default_layout_config(3)};
    require(source.state().merchant_trips.empty(), "a fresh world has no trips");
    const auto v28 = snapshot_test::downgrade_to_v28(source.save_snapshot(), source.state());
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(v28, restored).ok, "a v28 snapshot must still load");
    require(restored.state().merchant_trips.empty(), "a snapshot from before trips has none in progress");
}

void test_an_older_snapshot_cannot_carry_trade_events() {
    const auto seed = seed_with_a_trip();
    require(seed.has_value(), "a world must schedule a trip");
    universe::Simulation source{default_layout_config(*seed)};
    std::vector<universe::Settlement> before;
    require(run_to_first_trip(source, before), "the world schedules a trip");
    const auto v28 = snapshot_test::downgrade_to_v28(source.save_snapshot(), source.state());
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto result = universe::Simulation::load_snapshot(v28, restored);
    require(!result.ok && result.code == universe::SnapshotErrorCode::InvalidEnum, "a v28 file naming a trade event must be rejected");
}

void refresh_hash(std::vector<std::byte>& bytes) {
    const auto payload_size = bytes.size() - snapshot_test::kHeaderSize;
    snapshot_test::write_u64_at(bytes, 8U, payload_size);
    snapshot_test::write_u64_at(
        bytes, 16U, universe::snapshot_hash64(std::span<const std::byte>{bytes.data() + snapshot_test::kHeaderSize, payload_size}));
}

void test_snapshot_rejects_corrupt_trips() {
    const auto seed = seed_with_a_trip();
    require(seed.has_value(), "a world must schedule a trip");
    universe::Simulation source{default_layout_config(*seed)};
    std::vector<universe::Settlement> before;
    require(run_to_first_trip(source, before), "the world schedules a trip");
    const auto valid = source.save_snapshot();
    const auto trip_count = source.state().merchant_trips.size();
    const auto section_start = valid.size() - snapshot_test::trade_trip_section_size(source.state());
    const auto record = section_start + snapshot_test::kCountSize;  // first trip

    constexpr std::size_t kOrigin = 8U;
    constexpr std::size_t kDestination = 16U;
    constexpr std::size_t kResource = 24U;
    constexpr std::size_t kCargo = 25U;
    constexpr std::size_t kCarried = 41U;
    constexpr std::size_t kArrives = 65U;
    constexpr std::size_t kReturns = 73U;

    const auto expect_failure = [&](std::vector<std::byte> bytes, const universe::SnapshotErrorCode code, const char* message) {
        refresh_hash(bytes);
        universe::Simulation destination{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        const auto result = universe::Simulation::load_snapshot(bytes, destination);
        require(!result.ok && result.code == code, message);
    };

    auto not_a_merchant = valid;
    snapshot_test::write_u64_at(not_a_merchant, record, 0xFFFFFFULL);
    expect_failure(not_a_merchant, universe::SnapshotErrorCode::InvalidReference, "a trip for nobody must be rejected");

    auto unknown_origin = valid;
    snapshot_test::write_u64_at(unknown_origin, record + kOrigin, 0xFFFFFFULL);
    expect_failure(unknown_origin, universe::SnapshotErrorCode::InvalidReference, "a trip from an unknown settlement must be rejected");

    auto same_place = valid;
    for (std::size_t i = 0; i < 8U; ++i) {
        same_place[record + kDestination + i] = same_place[record + kOrigin + i];
    }
    expect_failure(same_place, universe::SnapshotErrorCode::InvalidState, "a trip that starts and ends in one place must be rejected");

    auto no_cargo = valid;
    snapshot_test::write_u64_at(no_cargo, record + kCargo, 0ULL);
    expect_failure(no_cargo, universe::SnapshotErrorCode::InvalidState, "a trip with no cargo must be rejected");

    auto bad_resource = valid;
    bad_resource[record + kResource] = static_cast<std::byte>(9);
    expect_failure(bad_resource, universe::SnapshotErrorCode::InvalidEnum, "an unknown resource must be rejected");

    auto bad_phase = valid;
    bad_phase[record + 81U] = static_cast<std::byte>(2);
    expect_failure(bad_phase, universe::SnapshotErrorCode::InvalidEnum, "an unknown phase must be rejected");

    auto outbound_with_coin = valid;
    snapshot_test::write_u64_at(outbound_with_coin, record + kCarried, 5ULL);
    expect_failure(outbound_with_coin, universe::SnapshotErrorCode::InvalidState, "an outbound trip cannot carry coin home");

    auto returns_before_arrival = valid;
    snapshot_test::write_u64_at(returns_before_arrival, record + kReturns, 1ULL);
    expect_failure(returns_before_arrival, universe::SnapshotErrorCode::InvalidState, "a trip cannot return before it arrives");

    auto arrives_before_departure = valid;
    snapshot_test::write_u64_at(arrives_before_departure, record + kArrives, 0ULL);
    expect_failure(arrives_before_departure, universe::SnapshotErrorCode::InvalidState, "a trip cannot arrive before it departs");

    auto too_many = valid;
    snapshot_test::write_u64_at(too_many, section_start, static_cast<std::uint64_t>(trip_count + 1'000U));
    expect_failure(too_many, universe::SnapshotErrorCode::InvalidStructure, "a trip count beyond the payload must be rejected");
}

}  // namespace

int main() {
    test_distance_is_the_straight_line_rounded_up();
    test_travel_takes_real_time_rounded_up();
    test_reach_is_three_days_one_way_and_scales_with_speed();
    test_cargo_capacity_scales_with_the_multiplier();
    test_rounding_never_creates_coin();
    test_a_trip_that_does_not_pay_is_never_offered();
    test_a_profitable_trip_is_sized_by_load_stock_and_coin();
    test_proceeds_split_conserves_every_coin();
    test_a_departure_takes_the_cargo_and_records_the_event();
    test_a_trip_takes_its_time_and_the_world_conserves_coin();
    test_the_household_share_and_commons_return_follow_the_split();
    test_equivalent_time_advance_schedules_give_identical_worlds_with_trade();
    test_same_seed_same_trips();
    test_save_and_load_mid_trip_continues_identically();
    test_demoting_every_region_ends_trips_without_losing_coin();
    test_a_v28_snapshot_loads_with_every_merchant_at_home();
    test_an_older_snapshot_cannot_carry_trade_events();
    test_snapshot_rejects_corrupt_trips();
    std::cout << "trade trip tests passed\n";
    return 0;
}
