// Trade trips (ADR 0003, ticket #72): a merchant leaves with cargo the origin
// commons funds, walks for real simulated days, sells at the destination's price
// and returns with the coin.
//
// The trade system runs once a day with the food period, on whole-day ticks, so
// a trip is resolved on the first daily tick on or after its arrival or return
// time. That tick schedule is fixed, so a trip can never finish early and every
// equivalent time-advance schedule produces the same world. No randomness is
// involved: choices are the best expected profit, ties to the lowest settlement
// then resource id.
#include "universe/simulation.hpp"
#include "universe/profiling.hpp"
#include "universe/trade_trip_endings.hpp"

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace universe {

namespace {

[[nodiscard]] std::int64_t& stock_of(Settlement& settlement, const ResourceType resource) noexcept {
    switch (resource) {
        case ResourceType::Food:
            return settlement.commons_food.value;
        case ResourceType::Wood:
            return settlement.materials.wood.value;
        case ResourceType::Stone:
            return settlement.materials.stone.value;
        case ResourceType::Timber:
            return settlement.materials.timber.value;
        case ResourceType::BasicTool:
            return settlement.materials.basic_tool.value;
    }
    return settlement.commons_food.value;
}

void add_coin(std::int64_t& balance, const std::int64_t amount, const char* what) {
    if (!checked_add_i64(balance, amount, balance)) {
        throw std::overflow_error(what);
    }
}

[[nodiscard]] std::vector<MerchantTrip>::iterator trip_position(std::vector<MerchantTrip>& trips, const PersonId merchant_id) {
    return std::lower_bound(trips.begin(), trips.end(), merchant_id, [](const MerchantTrip& trip, const PersonId id) {
        return trip.merchant_id < id;
    });
}

// The active settlement that inherits a lost settlement's goods and coin: its
// absorber (read from the merge records in the event journal, following chains)
// or else the nearest active settlement. Empty only when none is active.
[[nodiscard]] std::optional<SettlementId> heir_of(const WorldState& state, const SettlementId lost_id) {
    std::vector<TripSite> sites;
    sites.reserve(state.settlements.size());
    for (const auto& settlement : state.settlements) {
        sites.push_back(TripSite{
            .settlement_id = settlement.id,
            .x = settlement.position_x,
            .z = settlement.position_z,
            .active = settlement.active,
        });
    }
    std::vector<AbsorptionLink> links;
    for (const auto& event : state.event_log) {
        if (event.type == EventType::SettlementMerged) {
            const auto& merged = std::get<SettlementMergedEventPayload>(event.payload);
            links.push_back(AbsorptionLink{
                .absorbed_id = merged.absorbed_settlement_id, .survivor_id = merged.survivor_settlement_id});
        }
    }
    return resolve_receiving_settlement(lost_id, sites, links);
}

}  // namespace

bool Simulation::is_merchant_away(const PersonId person_id) const noexcept {
    const auto it = std::lower_bound(
        state_.merchant_trips.begin(), state_.merchant_trips.end(), person_id, [](const MerchantTrip& trip, const PersonId id) {
            return trip.merchant_id < id;
        });
    return it != state_.merchant_trips.end() && it->merchant_id == person_id;
}

std::optional<MerchantTrip> Simulation::trip_for_merchant(const PersonId person_id) const {
    const auto it = std::lower_bound(
        state_.merchant_trips.begin(), state_.merchant_trips.end(), person_id, [](const MerchantTrip& trip, const PersonId id) {
            return trip.merchant_id < id;
        });
    if (it == state_.merchant_trips.end() || it->merchant_id != person_id) {
        return std::nullopt;
    }
    return *it;
}

void Simulation::credit_settlement_stock(Settlement& settlement, const ResourceType resource, const std::int64_t units) {
    add_coin(stock_of(settlement, resource), units, "Settlement stock overflow while crediting trade goods");
}

EventId Simulation::emit_trade_event(const EventType type, const MerchantTrip& trip, const std::int64_t coin_profit) {
    return append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = type,
        .payload = TradeEventPayload{
            .merchant_id = trip.merchant_id,
            .origin_settlement_id = trip.origin_settlement_id,
            .destination_settlement_id = trip.destination_settlement_id,
            .resource = trip.resource,
            .cargo_units = trip.cargo_units,
            .coin_profit = coin_profit,
        },
    });
}

// What a merchant sees on reaching a settlement: all five of its current prices.
void Simulation::remember_prices(const PersonId merchant_id, const SettlementId settlement_id, const SettlementPrices& prices) {
    auto& memory = state_.merchant_price_memory;
    for (std::size_t index = 0; index < prices.millicoin.size(); ++index) {
        const auto resource = static_cast<ResourceType>(index);
        const MerchantPriceMemory row{
            .merchant_id = merchant_id,
            .settlement_id = settlement_id,
            .resource = resource,
            .price_millicoin = prices.millicoin[index],
            .seen_at = state_.time,
        };
        const auto position = std::lower_bound(memory.begin(), memory.end(), row, [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs.merchant_id, lhs.settlement_id, lhs.resource) <
                std::tie(rhs.merchant_id, rhs.settlement_id, rhs.resource);
        });
        if (position != memory.end() && position->merchant_id == merchant_id && position->settlement_id == settlement_id &&
            position->resource == resource) {
            *position = row;
        } else {
            memory.insert(position, row);
        }
    }
}

// Cargo and coin go back to the origin commons. Goods already sold stay sold.
void Simulation::abandon_trip(const MerchantTrip& trip) {
    // A lost origin (extinct, or absorbed) hands everything to its heir; with no
    // active settlement at all it stays with the origin so nothing vanishes.
    const auto heir = heir_of(state_, trip.origin_settlement_id);
    auto* receiver = find_settlement_mutable(heir.value_or(trip.origin_settlement_id));
    if (receiver == nullptr) {
        return;
    }
    if (trip.phase == TripPhase::Outbound) {
        credit_settlement_stock(*receiver, trip.resource, trip.cargo_units);
        return;
    }
    credit_settlement_stock(*receiver, trip.resource, trip.unsold_units);
    add_coin(receiver->coin, trip.coin_carried, "Settlement coin overflow while abandoning a trip");
}

void Simulation::settle_trade_stock() {
    process_local_exchange(state_.year);
    refresh_food_aggregates();
    refresh_material_aggregates();
    refresh_economic_aggregates();
}

void Simulation::resolve_orphaned_trips() {
    if (state_.merchant_trips.empty()) {
        return;
    }
    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }
    const auto trips_before = state_.merchant_trips.size();
    std::erase_if(state_.merchant_trips, [&](const MerchantTrip& trip) {
        const auto person_it = person_by_id.find(trip.merchant_id);
        const bool merchant_gone = person_it == person_by_id.end() || !person_it->second->alive ||
            person_it->second->occupation != Occupation::Merchant;
        if (!merchant_gone && trip.origin_settlement_id != trip.destination_settlement_id) {
            return false;
        }
        abandon_trip(trip);
        return true;
    });
    if (state_.merchant_trips.size() != trips_before) {
        settle_trade_stock();
    }
}

void Simulation::process_trade_period() {
    UC_ZONE("process_trade_period");
    if (state_.merchant_trips.empty() && state_.merchant_price_memory.empty()) {
        return;
    }
    resolve_orphaned_trips();

    // Arrivals and returns, in merchant id order.
    for (std::size_t index = 0; index < state_.merchant_trips.size();) {
        auto& trip = state_.merchant_trips[index];

        if (trip.phase == TripPhase::Outbound && state_.time >= trip.arrives_at) {
            auto* destination = find_settlement_mutable(trip.destination_settlement_id);
            const auto destination_prices = destination != nullptr && destination->active
                ? settlement_prices(trip.destination_settlement_id)
                : std::optional<SettlementPrices>{};
            if (!destination_prices.has_value()) {
                // The destination is gone: nothing is sold and the merchant walks
                // the whole cargo home.
                trip.coin_carried = 0;
                trip.unsold_units = trip.cargo_units;
                trip.phase = TripPhase::Returning;
            } else {
            // The destination pays what its commons can: what it cannot pay for
            // stays on the cart and goes home.
            const auto price = destination_prices->of(trip.resource);
            const auto sold = std::min(trip.cargo_units, units_affordable(destination->coin, price));
            const auto revenue = trade_revenue_coin(sold, price);
            destination->coin -= revenue;
            credit_settlement_stock(*destination, trip.resource, sold);
            trip.coin_carried = revenue;
            trip.unsold_units = trip.cargo_units - sold;
            trip.phase = TripPhase::Returning;
            remember_prices(trip.merchant_id, trip.destination_settlement_id, *destination_prices);
            }
        }

        if (trip.phase == TripPhase::Returning && state_.time >= trip.returns_at) {
            auto* origin = find_settlement_mutable(trip.origin_settlement_id);
            if (origin == nullptr) {
                throw std::logic_error("Trade trip origin settlement is missing");
            }
            // Home may be gone by the time the merchant is back: the goods and
            // the commons share go to its heir (absorber, else nearest active).
            Settlement* receiver = origin;
            if (!origin->active) {
                if (const auto heir = heir_of(state_, origin->id); heir.has_value()) {
                    receiver = find_settlement_mutable(*heir);
                }
            }
            const auto proceeds = split_trade_proceeds(trip.coin_carried, trip.coin_outlay);
            Household* merchant_household = nullptr;
            for (const auto& person : state_.people) {
                if (person.id == trip.merchant_id) {
                    for (auto& household : state_.households) {
                        if (household.id == person.household_id) {
                            merchant_household = &household;
                        }
                    }
                    break;
                }
            }
            if (merchant_household != nullptr) {
                add_coin(merchant_household->coin, proceeds.household_share, "Household coin overflow while settling a trade");
                add_coin(receiver->coin, proceeds.commons_return, "Settlement coin overflow while settling a trade");
            } else {
                add_coin(receiver->coin, proceeds.household_share + proceeds.commons_return, "Settlement coin overflow while settling a trade");
            }
            credit_settlement_stock(*receiver, trip.resource, trip.unsold_units);
            if (origin->active) {
                if (const auto home_prices = settlement_prices(trip.origin_settlement_id); home_prices.has_value()) {
                    remember_prices(trip.merchant_id, trip.origin_settlement_id, *home_prices);
                }
            }
            emit_trade_event(EventType::TradeCompleted, trip, trip.coin_carried - trip.coin_outlay);
            state_.merchant_trips.erase(state_.merchant_trips.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ++index;
    }

    // Household ownership of materials is a partition of the commons stock, and the
    // legs above moved stock, so re-partition before leaving (and again after
    // departures below).
    settle_trade_stock();
    if (state_.merchant_price_memory.empty()) {
        return;
    }

    // Departures: every idle merchant considers each settlement it remembers.
    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }
    std::unordered_map<HouseholdId, SettlementId> settlement_of_household;
    settlement_of_household.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        settlement_of_household.emplace(household.id, household.settlement_id);
    }
    std::unordered_map<SettlementId, std::optional<SettlementPrices>> price_cache;
    const auto capability = baseline_trade_capability();
    const auto& memory = state_.merchant_price_memory;

    for (std::size_t first = 0; first < memory.size();) {
        const auto merchant_id = memory[first].merchant_id;
        std::size_t last = first;
        while (last < memory.size() && memory[last].merchant_id == merchant_id) {
            ++last;
        }
        const auto rows_begin = first;
        first = last;

        if (is_merchant_away(merchant_id)) {
            continue;
        }
        const auto person_it = person_by_id.find(merchant_id);
        if (person_it == person_by_id.end() || !person_it->second->alive ||
            person_it->second->occupation != Occupation::Merchant) {
            continue;
        }
        const auto home_it = settlement_of_household.find(person_it->second->household_id);
        if (home_it == settlement_of_household.end()) {
            continue;
        }
        auto* origin = find_settlement_mutable(home_it->second);
        if (origin == nullptr || !origin->active || origin->coin <= 0) {
            continue;
        }
        auto prices_it = price_cache.find(origin->id);
        if (prices_it == price_cache.end()) {
            prices_it = price_cache.emplace(origin->id, settlement_prices(origin->id)).first;
        }
        if (!prices_it->second.has_value()) {
            continue;
        }
        const auto& origin_prices = *prices_it->second;

        struct Choice {
            const MerchantPriceMemory* row{};
            TripOffer offer{};
            std::int64_t travel{};
        };
        std::optional<Choice> best;
        for (auto row_index = rows_begin; row_index < last; ++row_index) {
            const auto& row = memory[row_index];
            if (row.settlement_id == origin->id || (row.resource == ResourceType::Food && origin->days_under_food_strain > 0U)) {
                continue;
            }
            // A price seen too long ago is not treated as current (#73): a merchant
            // plans a trip only on what it has recently observed.
            if (!remembered_price_is_current(row.seen_at, state_.time)) {
                continue;
            }
            const auto* destination = find_settlement(row.settlement_id);
            if (destination == nullptr || !destination->active) {
                continue;
            }
            const auto distance = trade_distance_units(
                origin->position_x, origin->position_z, destination->position_x, destination->position_z);
            if (!trip_within_reach(distance, capability)) {
                continue;
            }
            const auto offer = evaluate_trip(
                origin_prices.of(row.resource), row.price_millicoin, stock_of(*origin, row.resource), origin->coin, capability);
            if (offer.has_value() && (!best.has_value() || offer->expected_profit > best->offer.expected_profit)) {
                best = Choice{.row = &row, .offer = *offer, .travel = travel_ticks(distance, capability.speed_units_per_day)};
            }
        }
        if (!best.has_value()) {
            continue;
        }

        SimulationTime arrives{};
        SimulationTime returns{};
        if (!checked_add_time(state_.time, SimulationDuration{best->travel}, arrives) ||
            !checked_add_time(arrives, SimulationDuration{best->travel}, returns)) {
            throw std::overflow_error("Trade trip time overflow");
        }
        const MerchantTrip trip{
            .merchant_id = merchant_id,
            .origin_settlement_id = origin->id,
            .destination_settlement_id = best->row->settlement_id,
            .resource = best->row->resource,
            .cargo_units = best->offer.cargo_units,
            .coin_outlay = best->offer.coin_outlay,
            .coin_carried = 0,
            .unsold_units = 0,
            .departed_at = state_.time,
            .arrives_at = arrives,
            .returns_at = returns,
            .phase = TripPhase::Outbound,
        };
        stock_of(*origin, trip.resource) -= trip.cargo_units;
        state_.merchant_trips.insert(trip_position(state_.merchant_trips, merchant_id), trip);
        emit_trade_event(EventType::TradeDeparted, trip, best->offer.expected_profit);
    }
    settle_trade_stock();
}

}  // namespace universe
