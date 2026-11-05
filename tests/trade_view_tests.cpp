// Trade trips in the presentation layer (ADR 0003, ticket #77): the economy view
// lists active trips and a merchant's life history lists them from recorded
// events only. Nothing here is canonical.
#include "settlement_test_support.hpp"

#include "universe/ui/life_history.hpp"
#include "universe/ui/simulation_controller.hpp"
#include "universe/ui/view_models.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <variant>

namespace {

using namespace settlement_test;

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// Advances one day at a time until a merchant is on a trip.
bool run_to_a_trip(universe::ui::SimulationController& controller) {
    for (int day = 0; day < 365 * 12; ++day) {
        controller.advance_days(1);
        if (!controller.simulation().state().merchant_trips.empty()) {
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
        universe::ui::SimulationController controller{default_layout_config(seed)};
        if (run_to_a_trip(controller)) {
            cached = seed;
            return seed;
        }
    }
    cached = std::optional<std::uint64_t>{};
    return std::nullopt;
}

universe::ui::SimulationController controller_on_a_trip() {
    const auto seed = seed_with_a_trip();
    require(seed.has_value(), "a world must schedule a trip");
    universe::ui::SimulationController controller{default_layout_config(*seed)};
    require(run_to_a_trip(controller), "the world schedules a trip");
    return controller;
}

void test_economy_view_lists_active_trips_with_expected_profit() {
    auto controller = controller_on_a_trip();
    const auto trip = controller.simulation().state().merchant_trips.front();
    const auto origin = universe::ui::build_settlement_economy(controller, trip.origin_settlement_id);
    const auto destination = universe::ui::build_settlement_economy(controller, trip.destination_settlement_id);
    require(origin.has_value() && destination.has_value(), "both settlements have an economy view");

    for (const auto* view : {&*origin, &*destination}) {
        const auto it = std::find_if(view->trips.begin(), view->trips.end(), [&](const universe::ui::TradeTripView& candidate) {
            return candidate.merchant_id == trip.merchant_id;
        });
        require(it != view->trips.end(), "a trip appears at both its origin and its destination");
        require(it->origin_settlement_id == trip.origin_settlement_id &&
                    it->destination_settlement_id == trip.destination_settlement_id && it->resource == trip.resource &&
                    it->cargo_units == trip.cargo_units && it->phase == trip.phase,
                "the trip view matches canonical state");
        require(it->expected_profit >= universe::kMinimumTripProfitCoin, "the expected profit comes from the departure event");
    }

    const auto message = universe::ui::make_view_model_message(
        2U, controller, universe::ui::ViewModelKind::SettlementEconomy, trip.origin_settlement_id);
    require(contains(message, "\"trips\":[{\"merchantId\":" + std::to_string(trip.merchant_id)), "the message carries the trip");
    require(contains(message, "\"phase\":\"Outbound\"") && contains(message, "\"expectedProfit\":"), "phase and expected profit are sent");
}

void test_settlements_off_the_route_have_no_trips() {
    auto controller = controller_on_a_trip();
    const auto trip = controller.simulation().state().merchant_trips.front();
    for (const auto& settlement : controller.settlements()) {
        if (settlement.id == trip.origin_settlement_id || settlement.id == trip.destination_settlement_id) {
            continue;
        }
        const auto view = universe::ui::build_settlement_economy(controller, settlement.id);
        require(view.has_value() && std::none_of(view->trips.begin(), view->trips.end(), [&](const auto& t) {
                    return t.merchant_id == trip.merchant_id;
                }),
                "an uninvolved settlement does not list the trip");
    }
}

void test_view_does_not_change_canonical_state() {
    auto controller = controller_on_a_trip();
    const auto before = controller.simulation().save_snapshot();
    const auto trip = controller.simulation().state().merchant_trips.front();
    (void)universe::ui::build_settlement_economy(controller, trip.origin_settlement_id);
    (void)universe::ui::build_person_life_history(controller, trip.merchant_id);
    require(controller.simulation().save_snapshot() == before, "building views never changes the snapshot");
}

void test_life_history_lists_a_departed_trip_from_its_event() {
    auto controller = controller_on_a_trip();
    const auto trip = controller.simulation().state().merchant_trips.front();

    const auto history = universe::ui::build_person_life_history(controller, trip.merchant_id);
    require(history.has_value(), "the merchant has a life history");
    const auto it = std::find_if(history->entries.begin(), history->entries.end(), [](const universe::ui::LifeHistoryEntry& entry) {
        return entry.kind == universe::ui::LifeEntryKind::TradeDeparted;
    });
    require(it != history->entries.end(), "the departure is listed");
    require(it->at.ticks == trip.departed_at.ticks, "the entry is dated by the event");
    const auto events = controller.events();
    require(std::any_of(events.begin(), events.end(), [&](const universe::SimulationEvent& event) {
                return event.id == it->source_id && event.type == universe::EventType::TradeDeparted;
            }),
            "the entry is backed by a recorded TradeDeparted event");
    require(contains(it->text, "Set out from"), "the entry reads as a departure");
}

void test_life_history_lists_a_completed_trip() {
    auto controller = controller_on_a_trip();
    std::optional<universe::PersonId> merchant;
    for (int day = 0; day < 60 && !merchant.has_value(); ++day) {
        controller.advance_days(1);
        for (const auto& event : controller.events()) {
            if (event.type == universe::EventType::TradeCompleted) {
                merchant = std::get<universe::TradeEventPayload>(event.payload).merchant_id;
                break;
            }
        }
    }
    require(merchant.has_value(), "a trip completes within two months");
    const auto history = universe::ui::build_person_life_history(controller, *merchant);
    require(history.has_value(), "the merchant has a life history");
    require(std::any_of(history->entries.begin(), history->entries.end(), [](const universe::ui::LifeHistoryEntry& entry) {
                return entry.kind == universe::ui::LifeEntryKind::TradeCompleted && entry.text.find("Came home from") == 0;
            }),
            "the return is listed from the TradeCompleted event");
}

}  // namespace

int main() {
    test_economy_view_lists_active_trips_with_expected_profit();
    test_settlements_off_the_route_have_no_trips();
    test_view_does_not_change_canonical_state();
    test_life_history_lists_a_departed_trip_from_its_event();
    test_life_history_lists_a_completed_trip();
    std::cout << "trade view tests passed\n";
    return 0;
}
