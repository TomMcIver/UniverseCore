#include "universe/ui/web_protocol.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "protocol_text.hpp"

namespace universe::ui {

namespace {

using detail::activity_kind_label;
using detail::age_years_from_person;
using detail::biological_sex_label;
using detail::building_state_label;
using detail::building_type_label;
using detail::json_escape;
using detail::occupation_label;

[[nodiscard]] std::optional<std::string_view> extract_string_field(
    std::string_view json,
    std::string_view key) {
    const std::string needle = std::string{"\""} + std::string{key} + "\"";
    const auto key_pos = json.find(needle);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }

    const auto colon_pos = json.find(':', key_pos + needle.size());
    if (colon_pos == std::string_view::npos) {
        return std::nullopt;
    }

    const auto quote_start = json.find('"', colon_pos + 1U);
    if (quote_start == std::string_view::npos) {
        return std::nullopt;
    }

    const auto quote_end = json.find('"', quote_start + 1U);
    if (quote_end == std::string_view::npos || quote_end <= quote_start) {
        return std::nullopt;
    }

    return json.substr(quote_start + 1U, quote_end - quote_start - 1U);
}

[[nodiscard]] std::optional<std::uint64_t> extract_u64_field(
    std::string_view json,
    std::string_view key) {
    const std::string needle = std::string{"\""} + std::string{key} + "\"";
    const auto key_pos = json.find(needle);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }

    const auto colon_pos = json.find(':', key_pos + needle.size());
    if (colon_pos == std::string_view::npos) {
        return std::nullopt;
    }

    std::size_t value_pos = colon_pos + 1U;
    while (value_pos < json.size() && (json[value_pos] == ' ' || json[value_pos] == '\t')) {
        ++value_pos;
    }

    std::size_t end_pos = value_pos;
    while (end_pos < json.size() && json[end_pos] >= '0' && json[end_pos] <= '9') {
        ++end_pos;
    }

    if (end_pos == value_pos) {
        return std::nullopt;
    }

    std::uint64_t parsed = 0;
    const auto* begin = json.data() + value_pos;
    const auto* end = json.data() + end_pos;
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }

    return parsed;
}

[[nodiscard]] std::string environment_class_label(const EnvironmentClass environment_class) {
    switch (environment_class) {
        case EnvironmentClass::ProductiveArable:
            return "ProductiveArable";
        case EnvironmentClass::Woodland:
            return "Woodland";
        case EnvironmentClass::Marginal:
            return "Marginal";
        case EnvironmentClass::WaterWetland:
            return "WaterWetland";
    }
    return "Unknown";
}

[[nodiscard]] std::string event_type_label(const EventType event_type) {
    switch (event_type) {
        case EventType::Birth:
            return "Birth";
        case EventType::Death:
            return "Death";
        case EventType::SettlementFounded:
            return "SettlementFounded";
        case EventType::SettlementExtinct:
            return "SettlementExtinct";
        case EventType::DiscoveryMade:
            return "DiscoveryMade";
        case EventType::InstitutionFounded:
            return "InstitutionFounded";
        case EventType::SettlementMerged:
            return "SettlementMerged";
        case EventType::OccupationChanged:
            return "OccupationChanged";
        case EventType::HouseholdRelocated:
            return "HouseholdRelocated";
    }
    return "Unknown";
}

}  // namespace

WorldSnapshotView build_world_snapshot(const SimulationController& controller) {
    const auto summary = controller.summary();
    const auto people = controller.people();
    const auto households = controller.households();
    const auto settlements = controller.settlements();
    const auto buildings = controller.buildings();
    const auto regions = controller.regions();

    std::unordered_map<HouseholdId, const Household*> household_by_id;
    household_by_id.reserve(households.size());
    for (const auto& household : households) {
        household_by_id.emplace(household.id, &household);
    }

    WorldSnapshotView snapshot{};
    snapshot.tick = static_cast<std::uint64_t>(summary.time.ticks < 0 ? 0 : summary.time.ticks);
    snapshot.year = summary.year;
    snapshot.population = summary.population;

    // Ticket #24/#25/#38: Region and Settlement now carry real canonical
    // (x, z) positions derived deterministically in UniverseCore itself
    // (SeedDomain::RegionPosition/SettlementPosition, with real min-distance
    // rejection sampling). This adapter used to invent presentation-only
    // placement here (per-region hashed cluster centers, Poisson-disc local
    // scatter) because no such canonical geography existed; that invented
    // placement is now superseded and forwards the real position directly.
    snapshot.settlements.reserve(settlements.size());
    for (const auto& settlement : settlements) {
        snapshot.settlements.push_back(SettlementRenderView{
            .id = settlement.id,
            .region_id = settlement.region_id,
            .active = settlement.active,
            .access_share_points = settlement.access_share_points,
            .x = static_cast<float>(settlement.position_x),
            .y = 0.0F,
            .z = static_cast<float>(settlement.position_z),
        });
    }

    std::unordered_map<SettlementId, SettlementRenderView> settlement_view_by_id;
    settlement_view_by_id.reserve(snapshot.settlements.size());
    for (const auto& settlement : snapshot.settlements) {
        settlement_view_by_id.emplace(settlement.id, settlement);
    }

    // Ticket #35/#38: a person's canonical position is always
    // Settlement.position + Household.settlement_offset -- constant
    // regardless of current_activity, since no sub-settlement geography
    // (Building position) exists yet to distinguish home from work. This
    // replaces the previous invented day-phase orbit/jitter, which had no
    // relationship to canonical state at all.
    snapshot.agents.reserve(people.size());
    for (const auto& person : people) {
        const auto household_it = household_by_id.find(person.household_id);
        if (household_it == household_by_id.end()) {
            continue;
        }
        const auto* household = household_it->second;
        const auto settlement_it = settlement_view_by_id.find(household->settlement_id);
        if (settlement_it == settlement_view_by_id.end()) {
            continue;
        }

        const auto base = settlement_it->second;

        snapshot.agents.push_back(AgentRenderView{
            .id = person.id,
            .household_id = person.household_id,
            .settlement_id = household->settlement_id,
            .region_id = household->region_id,
            .alive = person.alive,
            .age_years = age_years_from_person(summary, person),
            .sex = person.sex,
            .occupation = person.occupation,
            .current_activity = person.current_activity,
            .x = base.x + static_cast<float>(household->settlement_offset_x),
            .y = 0.9F,
            .z = base.z + static_cast<float>(household->settlement_offset_z),
        });
    }

    std::sort(snapshot.agents.begin(), snapshot.agents.end(), [](const AgentRenderView& lhs, const AgentRenderView& rhs) {
        return lhs.id < rhs.id;
    });

    // Building placement is canonical simulation state (ADR 0001); this
    // adapter forwards it unchanged.
    snapshot.buildings.reserve(buildings.size());
    for (const auto& building : buildings) {
        const auto settlement_it = settlement_view_by_id.find(building.settlement_id);
        if (settlement_it == settlement_view_by_id.end()) {
            continue;
        }

        const auto settlement = settlement_it->second;
        const auto x = static_cast<float>(building.position_x);
        const auto z = static_cast<float>(building.position_z);

        float progress = 0.0F;
        if (building.labour_required > 0) {
            const auto clamped = std::clamp<std::int64_t>(building.labour_completed, 0, building.labour_required);
            progress = static_cast<float>(clamped) / static_cast<float>(building.labour_required);
        } else if (building.state == ConstructionState::Complete) {
            progress = 1.0F;
        }

        snapshot.buildings.push_back(BuildingRenderView{
            .id = building.id,
            .settlement_id = building.settlement_id,
            .region_id = settlement.region_id,
            .type = building_type_label(building.type),
            .state = building_state_label(building.state),
            .settlement_commons_owned = building.settlement_commons_owned,
            .owner_household_id = building.owner_household_id.has_value() ? *building.owner_household_id : 0U,
            .progress = std::clamp(progress, 0.0F, 1.0F),
            .x = x,
            .y = 0.0F,
            .z = z,
            .facing_degrees = building.facing_degrees,
        });
    }

    std::sort(snapshot.buildings.begin(), snapshot.buildings.end(), [](const BuildingRenderView& lhs, const BuildingRenderView& rhs) {
        return lhs.id < rhs.id;
    });

    snapshot.regions.reserve(regions.size());
    for (const auto& region : regions) {
        snapshot.regions.push_back(RegionRenderView{
            .id = region.id,
            .environment_class = environment_class_label(region.environment_class),
            .land_area_units = region.land_area_units,
            .wild_food_capacity = region.wild_food_capacity,
            .wild_food_stock = region.wild_food_stock,
            .wood_capacity = region.wood_capacity,
            .wood_stock = region.wood_stock,
            .stone_capacity = region.stone_capacity,
            .stone_remaining = region.stone_remaining,
        });
    }

    return snapshot;
}

WorldDeltaView build_world_delta(const WorldSnapshotView& current, const WorldSnapshotView& previous) {
    WorldDeltaView delta{};
    delta.tick = current.tick;
    delta.year = current.year;
    delta.population = current.population;

    std::unordered_map<PersonId, const AgentRenderView*> previous_agents_by_id;
    previous_agents_by_id.reserve(previous.agents.size());
    for (const auto& agent : previous.agents) {
        previous_agents_by_id.emplace(agent.id, &agent);
    }
    std::unordered_set<PersonId> current_agent_ids;
    current_agent_ids.reserve(current.agents.size());
    for (const auto& agent : current.agents) {
        current_agent_ids.insert(agent.id);
        const auto previous_it = previous_agents_by_id.find(agent.id);
        const bool was_present = previous_it != previous_agents_by_id.end();
        if (!agent.alive) {
            // An agent is removed from the wire once dead, regardless of
            // whether its record still lingers in the underlying query view --
            // it never appears as "updated" once its removal is observed.
            if (was_present && previous_it->second->alive) {
                delta.removed_agent_ids.push_back(agent.id);
            }
            continue;
        }
        if (was_present) {
            delta.updated_agents.push_back(agent);
        } else {
            delta.added_agents.push_back(agent);
        }
    }
    for (const auto& [agent_id, agent] : previous_agents_by_id) {
        if (!current_agent_ids.contains(agent_id) && agent->alive) {
            delta.removed_agent_ids.push_back(agent_id);
        }
    }
    std::sort(delta.removed_agent_ids.begin(), delta.removed_agent_ids.end());

    std::unordered_map<SettlementId, const SettlementRenderView*> previous_settlements_by_id;
    previous_settlements_by_id.reserve(previous.settlements.size());
    for (const auto& settlement : previous.settlements) {
        previous_settlements_by_id.emplace(settlement.id, &settlement);
    }
    std::unordered_set<SettlementId> current_settlement_ids;
    current_settlement_ids.reserve(current.settlements.size());
    for (const auto& settlement : current.settlements) {
        current_settlement_ids.insert(settlement.id);
        const auto previous_it = previous_settlements_by_id.find(settlement.id);
        const bool was_present = previous_it != previous_settlements_by_id.end();
        if (!settlement.active) {
            if (was_present && previous_it->second->active) {
                delta.removed_settlement_ids.push_back(settlement.id);
            }
            continue;
        }
        if (was_present) {
            delta.updated_settlements.push_back(settlement);
        } else {
            delta.added_settlements.push_back(settlement);
        }
    }
    for (const auto& [settlement_id, settlement] : previous_settlements_by_id) {
        if (!current_settlement_ids.contains(settlement_id) && settlement->active) {
            delta.removed_settlement_ids.push_back(settlement_id);
        }
    }
    std::sort(delta.removed_settlement_ids.begin(), delta.removed_settlement_ids.end());

    std::unordered_map<BuildingId, const BuildingRenderView*> previous_buildings_by_id;
    previous_buildings_by_id.reserve(previous.buildings.size());
    for (const auto& building : previous.buildings) {
        previous_buildings_by_id.emplace(building.id, &building);
    }
    std::unordered_set<BuildingId> current_building_ids;
    current_building_ids.reserve(current.buildings.size());
    for (const auto& building : current.buildings) {
        current_building_ids.insert(building.id);
        if (previous_buildings_by_id.contains(building.id)) {
            delta.updated_buildings.push_back(building);
        } else {
            delta.added_buildings.push_back(building);
        }
    }
    for (const auto& [building_id, building] : previous_buildings_by_id) {
        if (!current_building_ids.contains(building_id)) {
            delta.removed_building_ids.push_back(building_id);
        }
    }
    std::sort(delta.removed_building_ids.begin(), delta.removed_building_ids.end());

    return delta;
}

std::optional<ClientCommand> parse_client_command(std::string_view json, std::string& error_message) {
    const auto type = extract_string_field(json, "type");
    if (!type.has_value()) {
        error_message = "Missing type";
        return std::nullopt;
    }

    if (*type == "hello") {
        return ClientCommand{.type = ClientCommandType::Hello};
    }
    if (*type == "pause") {
        return ClientCommand{.type = ClientCommandType::Pause};
    }
    if (*type == "resume") {
        return ClientCommand{.type = ClientCommandType::Resume};
    }
    if (*type == "request_snapshot") {
        return ClientCommand{.type = ClientCommandType::RequestSnapshot};
    }
    if (*type == "single_step") {
        return ClientCommand{.type = ClientCommandType::SingleStep};
    }
    if (*type == "set_speed") {
        const auto speed = extract_u64_field(json, "speed");
        if (!speed.has_value() || *speed == 0U || *speed > 64U) {
            error_message = "Invalid speed";
            return std::nullopt;
        }
        return ClientCommand{
            .type = ClientCommandType::SetSpeed,
            .speed = static_cast<std::uint32_t>(*speed),
        };
    }
    if (*type == "request_view_model") {
        const auto kind_label = extract_string_field(json, "kind");
        const auto kind = kind_label.has_value() ? parse_view_model_kind(*kind_label) : std::nullopt;
        if (!kind.has_value()) {
            error_message = "Unknown view model kind";
            return std::nullopt;
        }
        const auto entity_id = extract_u64_field(json, "entityId");
        if (!entity_id.has_value() || *entity_id == 0U) {
            error_message = "Invalid entityId";
            return std::nullopt;
        }
        return ClientCommand{
            .type = ClientCommandType::RequestViewModel,
            .entity_id = *entity_id,
            .view_model_kind = *kind,
        };
    }

    error_message = "Unknown command type";
    return std::nullopt;
}

std::string make_hello_message(const std::uint32_t protocol_version) {
    return "{\"protocolVersion\":" + std::to_string(protocol_version) +
        ",\"type\":\"hello\",\"payload\":{\"server\":\"universe_ui_bridge\"}}";
}

std::string make_status_message(
    const std::uint32_t protocol_version,
    const std::uint64_t tick,
    const bool paused,
    const std::uint32_t speed) {
    return "{\"protocolVersion\":" + std::to_string(protocol_version) +
        ",\"type\":\"simulation_status\",\"tick\":" + std::to_string(tick) +
        ",\"payload\":{\"paused\":" + std::string{paused ? "true" : "false"} +
        ",\"speed\":" + std::to_string(speed) + "}}";
}

namespace {

void write_agent_json(std::ostringstream& out, const AgentRenderView& agent) {
    out << "{\"id\":" << agent.id
        << ",\"householdId\":" << agent.household_id
        << ",\"settlementId\":" << agent.settlement_id
        << ",\"regionId\":" << agent.region_id
        << ",\"alive\":" << (agent.alive ? "true" : "false")
        << ",\"age\":" << agent.age_years
        << ",\"sex\":\"" << (agent.sex == BiologicalSex::Female ? "Female" : "Male") << "\""
        << ",\"occupation\":\"" << occupation_label(agent.occupation) << "\""
        << ",\"currentActivity\":\"" << activity_kind_label(agent.current_activity) << "\""
        << ",\"position\":{\"x\":" << agent.x << ",\"y\":" << agent.y << ",\"z\":" << agent.z << "}}";
}

void write_settlement_json(std::ostringstream& out, const SettlementRenderView& settlement) {
    out << "{\"id\":" << settlement.id
        << ",\"regionId\":" << settlement.region_id
        << ",\"active\":" << (settlement.active ? "true" : "false")
        << ",\"accessSharePoints\":" << settlement.access_share_points
        << ",\"position\":{\"x\":" << settlement.x << ",\"y\":" << settlement.y << ",\"z\":" << settlement.z << "}}";
}

void write_building_json(std::ostringstream& out, const BuildingRenderView& building) {
    out << "{\"id\":" << building.id
        << ",\"settlementId\":" << building.settlement_id
        << ",\"regionId\":" << building.region_id
        << ",\"type\":\"" << building.type << "\""
        << ",\"state\":\"" << building.state << "\""
        << ",\"settlementCommonsOwned\":" << (building.settlement_commons_owned ? "true" : "false")
        << ",\"ownerHouseholdId\":" << building.owner_household_id
        << ",\"progress\":" << building.progress
        << ",\"facingDegrees\":" << building.facing_degrees
        << ",\"position\":{\"x\":" << building.x << ",\"y\":" << building.y << ",\"z\":" << building.z << "}}";
}

template <typename T, typename Writer>
void write_json_array(std::ostringstream& out, const std::vector<T>& items, Writer&& writer) {
    out << '[';
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0U) {
            out << ',';
        }
        writer(out, items[i]);
    }
    out << ']';
}

template <typename Id>
void write_id_array(std::ostringstream& out, const std::vector<Id>& ids) {
    out << '[';
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0U) {
            out << ',';
        }
        out << ids[i];
    }
    out << ']';
}

}  // namespace

std::string make_snapshot_message(
    const std::uint32_t protocol_version,
    const WorldSnapshotView& snapshot,
    const bool initial) {
    (void)initial;
    std::ostringstream out;
    out << "{\"protocolVersion\":" << protocol_version
        << ",\"type\":\"initial_snapshot\""
        << ",\"tick\":" << snapshot.tick
        << ",\"payload\":{"
        << "\"year\":" << snapshot.year
        << ",\"population\":" << snapshot.population
        << ",\"agents\":";
    write_json_array(out, snapshot.agents, write_agent_json);

    out << ",\"settlements\":";
    write_json_array(out, snapshot.settlements, write_settlement_json);

    out << ",\"buildings\":";
    write_json_array(out, snapshot.buildings, write_building_json);

    out << ",\"regions\":[";
    for (std::size_t i = 0; i < snapshot.regions.size(); ++i) {
        const auto& region = snapshot.regions[i];
        if (i > 0U) {
            out << ',';
        }
        out << "{\"id\":" << region.id
            << ",\"environmentClass\":\"" << region.environment_class << "\""
            << ",\"landAreaUnits\":" << region.land_area_units
            << ",\"wildFoodCapacity\":" << region.wild_food_capacity
            << ",\"wildFoodStock\":" << region.wild_food_stock
            << ",\"woodCapacity\":" << region.wood_capacity
            << ",\"woodStock\":" << region.wood_stock
            << ",\"stoneCapacity\":" << region.stone_capacity
            << ",\"stoneRemaining\":" << region.stone_remaining
            << "}";
    }

    out << "]}}";
    return out.str();
}

std::string make_delta_message(const std::uint32_t protocol_version, const WorldDeltaView& delta) {
    std::ostringstream out;
    out << "{\"protocolVersion\":" << protocol_version
        << ",\"type\":\"world_delta\""
        << ",\"tick\":" << delta.tick
        << ",\"payload\":{"
        << "\"year\":" << delta.year
        << ",\"population\":" << delta.population

        << ",\"addedAgents\":";
    write_json_array(out, delta.added_agents, write_agent_json);
    out << ",\"updatedAgents\":";
    write_json_array(out, delta.updated_agents, write_agent_json);
    out << ",\"removedAgentIds\":";
    write_id_array(out, delta.removed_agent_ids);

    out << ",\"addedSettlements\":";
    write_json_array(out, delta.added_settlements, write_settlement_json);
    out << ",\"updatedSettlements\":";
    write_json_array(out, delta.updated_settlements, write_settlement_json);
    out << ",\"removedSettlementIds\":";
    write_id_array(out, delta.removed_settlement_ids);

    out << ",\"addedBuildings\":";
    write_json_array(out, delta.added_buildings, write_building_json);
    out << ",\"updatedBuildings\":";
    write_json_array(out, delta.updated_buildings, write_building_json);
    out << ",\"removedBuildingIds\":";
    write_id_array(out, delta.removed_building_ids);

    out << "}}";
    return out.str();
}

std::string make_world_event_message(const std::uint32_t protocol_version, const SimulationEvent& event) {
    std::ostringstream out;
    out << "{\"protocolVersion\":" << protocol_version
        << ",\"type\":\"world_event\""
        << ",\"tick\":" << static_cast<std::uint64_t>(event.timestamp.ticks < 0 ? 0 : event.timestamp.ticks)
        << ",\"payload\":{"
        << "\"eventId\":" << event.id
        << ",\"eventType\":\"" << event_type_label(event.type) << "\""
        << ",\"timestampTicks\":" << static_cast<std::uint64_t>(event.timestamp.ticks < 0 ? 0 : event.timestamp.ticks);

    if (event.type == EventType::Birth) {
        const auto& payload = std::get<BirthEventPayload>(event.payload);
        out << ",\"entityId\":" << payload.child_id;
    } else if (event.type == EventType::Death) {
        const auto& payload = std::get<DeathEventPayload>(event.payload);
        out << ",\"entityId\":" << payload.person_id;
    } else if (event.type == EventType::OccupationChanged) {
        const auto& payload = std::get<OccupationChangedEventPayload>(event.payload);
        out << ",\"entityId\":" << payload.person_id;
    } else if (event.type == EventType::HouseholdRelocated) {
        const auto& payload = std::get<HouseholdRelocatedEventPayload>(event.payload);
        out << ",\"entityId\":" << payload.household_id;
    }

    out << "}}";
    return out.str();
}

std::string make_error_message(
    const std::uint32_t protocol_version,
    const std::string_view code,
    const std::string_view message) {
    return "{\"protocolVersion\":" + std::to_string(protocol_version) +
    ",\"type\":\"error\",\"payload\":{\"code\":\"" + json_escape(code) +
    "\",\"message\":\"" + json_escape(message) + "\"}}";
}

}  // namespace universe::ui
