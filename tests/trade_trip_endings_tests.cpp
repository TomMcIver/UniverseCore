// Safe trip endings (ADR 0003, ticket #74), pure-function level. The Simulation
// level tests (death en route, demote/promote mid-trip, seed sweep) need #72's
// trip state and are listed in the ticket hand-off.
#include "settlement_test_support.hpp"

#include "universe/trade_trip_endings.hpp"

#include <iostream>
#include <vector>

namespace {

using namespace settlement_test;
using universe::AbsorptionLink;
using universe::TripEndReason;
using universe::TripHold;
using universe::TripSite;

const std::vector<TripSite> kSites{
    {.settlement_id = 1, .x = 0, .z = 0, .active = true},
    {.settlement_id = 2, .x = 100, .z = 0, .active = true},
    {.settlement_id = 3, .x = -100, .z = 0, .active = false},
    {.settlement_id = 4, .x = -150, .z = 0, .active = true},
};
const TripHold kHold{.resource = universe::ResourceType::Wood, .cargo_amount = 37, .coin = 1'234};

void test_active_origin_receives_the_hold_unchanged() {
    for (const auto reason : {TripEndReason::MerchantDied, TripEndReason::DestinationInactive}) {
        const auto delivery = universe::end_trip(reason, 1, kHold, kSites, {});
        require(delivery.has_value() && delivery->receiver_id == 1, "origin receives");
        require(delivery->hold == kHold, "goods and coin are conserved");
        require(!delivery->sale_made, "no sale");
    }
}

void test_absorbed_origin_delivers_to_the_absorber() {
    const std::vector<AbsorptionLink> links{{.absorbed_id = 3, .survivor_id = 2}};
    const auto delivery = universe::end_trip(TripEndReason::OriginInactiveOnReturn, 3, kHold, kSites, links);
    require(delivery.has_value() && delivery->receiver_id == 2, "absorber receives, not the nearer settlement 4");
    require(delivery->hold == kHold, "hold unchanged");
}

void test_absorption_chain_is_followed() {
    auto sites = kSites;
    sites.push_back({.settlement_id = 5, .x = 9, .z = 9, .active = false});
    const std::vector<AbsorptionLink> links{{.absorbed_id = 5, .survivor_id = 3}, {.absorbed_id = 3, .survivor_id = 2}};
    require(universe::resolve_receiving_settlement(5, sites, links) == 2U, "chain 5 -> 3 -> 2");
}

void test_abandoned_origin_goes_to_the_nearest_active() {
    require(universe::resolve_receiving_settlement(3, kSites, {}) == 4U, "settlement 4 (50 away) beats 1 (100 away)");
}

void test_ties_pick_the_lowest_id_and_cycles_terminate() {
    const std::vector<TripSite> sites{
        {.settlement_id = 9, .x = 0, .z = 0, .active = false},
        {.settlement_id = 8, .x = 10, .z = 0, .active = true},
        {.settlement_id = 7, .x = -10, .z = 0, .active = true},
    };
    require(universe::resolve_receiving_settlement(9, sites, {}) == 7U, "tie goes to the lowest id");
    const std::vector<AbsorptionLink> cycle{{.absorbed_id = 9, .survivor_id = 6}, {.absorbed_id = 6, .survivor_id = 9}};
    require(universe::resolve_receiving_settlement(9, sites, cycle) == 7U, "cycle falls back to nearest");
}

void test_extreme_coordinates_do_not_overflow() {
    const std::vector<TripSite> sites{
        {.settlement_id = 1, .x = INT64_MIN, .z = INT64_MIN, .active = false},
        {.settlement_id = 2, .x = INT64_MAX, .z = INT64_MAX, .active = true},
        {.settlement_id = 3, .x = 0, .z = 0, .active = true},
    };
    require(universe::resolve_receiving_settlement(1, sites, {}).has_value(), "defined result");
}

void test_no_active_settlement_has_no_receiver() {
    const std::vector<TripSite> none{{.settlement_id = 1, .x = 0, .z = 0, .active = false}};
    require(!universe::end_trip(TripEndReason::MerchantDied, 1, kHold, none, {}).has_value(), "nobody to receive");
    require(!universe::resolve_receiving_settlement(1, {}, {}).has_value(), "empty world");
}

}  // namespace

int main() {
    test_active_origin_receives_the_hold_unchanged();
    test_absorbed_origin_delivers_to_the_absorber();
    test_absorption_chain_is_followed();
    test_abandoned_origin_goes_to_the_nearest_active();
    test_ties_pick_the_lowest_id_and_cycles_terminate();
    test_extreme_coordinates_do_not_overflow();
    test_no_active_settlement_has_no_receiver();
    std::cout << "trade trip ending tests passed\n";
    return 0;
}
