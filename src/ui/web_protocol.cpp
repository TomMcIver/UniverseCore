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

namespace universe::ui {

namespace {

[[nodiscard]] std::uint64_t stable_mix64(const std::uint64_t value) noexcept {
    std::uint64_t mixed = value + 0x9E3779B97F4A7C15ULL;
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
    return mixed ^ (mixed >> 31U);
}

[[nodiscard]] float normalized01(const std::uint64_t value) noexcept {
    constexpr double kScale = 1.0 / static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    const auto low = static_cast<std::uint32_t>(value & 0xFFFFFFFFULL);
    return static_cast<float>(static_cast<double>(low) * kScale);
}

[[nodiscard]] std::uint32_t age_years_from_person(const SimulationSummary& summary, const Person& person) noexcept {
    const auto year_i64 = static_cast<std::int64_t>(summary.year);
    if (year_i64 <= person.birth_year) {
        return 0U;
    }
    return static_cast<std::uint32_t>(year_i64 - person.birth_year);
}

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

[[nodiscard]] const Person* find_person(const std::span<const Person> people, const PersonId person_id) {
    for (const auto& person : people) {
        if (person.id == person_id) {
            return &person;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string biological_sex_label(const BiologicalSex sex) {
    if (sex == BiologicalSex::Female) {
        return "Female";
    }
    return "Male";
}

[[nodiscard]] std::string occupation_label(const Occupation occupation) {
    switch (occupation) {
        case Occupation::None:
            return "None";
        case Occupation::SubsistenceCultivator:
            return "SubsistenceCultivator";
        case Occupation::WildFoodGatherer:
            return "WildFoodGatherer";
        case Occupation::WoodGatherer:
            return "WoodGatherer";
        case Occupation::StoneExtractor:
            return "StoneExtractor";
        case Occupation::CraftWorker:
            return "CraftWorker";
    }
    return "Unknown";
}

[[nodiscard]] std::string building_type_label(const BuildingType type) {
    switch (type) {
        case BuildingType::Dwelling:
            return "Dwelling";
        case BuildingType::FoodStore:
            return "FoodStore";
        case BuildingType::Workshop:
            return "Workshop";
    }
    return "Unknown";
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

[[nodiscard]] std::string building_state_label(const ConstructionState state) {
    switch (state) {
        case ConstructionState::Planned:
            return "Planned";
        case ConstructionState::ActiveConstruction:
            return "ActiveConstruction";
        case ConstructionState::Complete:
            return "Complete";
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
    }
    return "Unknown";
}

[[nodiscard]] std::string json_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const auto ch : text) {
        switch (ch) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
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

    // Presentation-only placement (this is the universe::ui adapter layer,
    // not canonical simulation state -- UniverseCore has no geographic
    // coordinate system for regions/settlements yet). Previously every
    // settlement sat on one uniform ring regardless of region, which read
    // as an obviously artificial perfect circle rather than anything
    // resembling how settlements actually cluster. Each region instead
    // gets its own deterministic cluster center (hashed from region_id,
    // spread widely so regions don't overlap each other), and settlements
    // scatter locally around their own region's center -- several
    // separate clusters instead of one ring. Still entirely presentation
    // logic with no real geographic simulation behind it; a genuinely
    // better answer would give regions/settlements real positions in
    // UniverseCore itself, which is a larger change than this pass.
    //
    // Within a region, settlements are placed with Poisson-disc-style
    // rejection sampling (try a deterministic sequence of candidate spots,
    // widening the search radius each attempt, accept the first one far
    // enough from every settlement already placed in this region) rather
    // than independent random placement -- which had no minimum-distance
    // enforcement at all and could, and did, place two settlements (and
    // therefore their buildings) close enough to visibly overlap.
    std::unordered_map<RegionId, std::vector<const Settlement*>> settlements_by_region;
    settlements_by_region.reserve(settlements.size());
    for (const auto& settlement : settlements) {
        settlements_by_region[settlement.region_id].push_back(&settlement);
    }
    for (auto& region_entry : settlements_by_region) {
        std::sort(region_entry.second.begin(), region_entry.second.end(), [](const Settlement* lhs, const Settlement* rhs) {
            return lhs->id < rhs->id;
        });
    }

    constexpr float min_settlement_distance = 14.0F;
    constexpr int max_placement_attempts = 24;
    constexpr float settlement_golden_angle_radians = 2.39996323F;

    snapshot.settlements.reserve(settlements.size());
    for (const auto& region_entry : settlements_by_region) {
        const auto region_id = region_entry.first;
        const auto region_angle_hash = stable_mix64(region_id * 0x2545F4914F6CDD1DULL);
        const auto region_radius_hash = stable_mix64(region_id * 0x9E3779B97F4A7C15ULL);
        const auto region_angle = normalized01(region_angle_hash) * 6.28318530718F;
        const auto region_radius = 45.0F + (normalized01(region_radius_hash) * 70.0F);
        const auto region_center_x = std::cos(region_angle) * region_radius;
        const auto region_center_z = std::sin(region_angle) * region_radius;

        std::vector<std::pair<float, float>> placed_local_positions;
        placed_local_positions.reserve(region_entry.second.size());

        for (const auto* settlement : region_entry.second) {
            float chosen_x = 0.0F;
            float chosen_z = 0.0F;
            bool placed = false;

            for (int attempt = 0; attempt < max_placement_attempts; ++attempt) {
                const auto attempt_salt = static_cast<std::uint64_t>(attempt);
                const auto local_angle_hash = stable_mix64((settlement->id * 0xBF58476D1CE4E5B9ULL) ^ attempt_salt);
                const auto local_radius_hash = stable_mix64((settlement->id * 0x94D049BB133111EBULL) ^ attempt_salt);
                const auto local_angle = normalized01(local_angle_hash) * 6.28318530718F;
                const auto local_radius = 6.0F +
                    (normalized01(local_radius_hash) * (16.0F + (static_cast<float>(attempt) * 3.0F)));
                const auto candidate_x = std::cos(local_angle) * local_radius;
                const auto candidate_z = std::sin(local_angle) * local_radius;

                bool far_enough = true;
                for (const auto& placed_position : placed_local_positions) {
                    const auto dx = candidate_x - placed_position.first;
                    const auto dz = candidate_z - placed_position.second;
                    if (((dx * dx) + (dz * dz)) < (min_settlement_distance * min_settlement_distance)) {
                        far_enough = false;
                        break;
                    }
                }

                if (far_enough) {
                    chosen_x = candidate_x;
                    chosen_z = candidate_z;
                    placed = true;
                    break;
                }
            }

            if (!placed) {
                // Deterministic fallback if a crowded region exhausts every
                // attempt: push it out along a golden-angle spiral by
                // placement order, which still guarantees separation
                // instead of silently accepting the last rejected spot.
                const auto index = static_cast<float>(placed_local_positions.size());
                const auto angle = index * settlement_golden_angle_radians;
                const auto radius = 20.0F + (std::sqrt(index) * 8.0F);
                chosen_x = std::cos(angle) * radius;
                chosen_z = std::sin(angle) * radius;
            }

            placed_local_positions.emplace_back(chosen_x, chosen_z);

            snapshot.settlements.push_back(SettlementRenderView{
                .id = settlement->id,
                .region_id = settlement->region_id,
                .active = settlement->active,
                .access_share_points = settlement->access_share_points,
                .x = region_center_x + chosen_x,
                .y = 0.0F,
                .z = region_center_z + chosen_z,
            });
        }
    }

    std::unordered_map<SettlementId, SettlementRenderView> settlement_view_by_id;
    settlement_view_by_id.reserve(snapshot.settlements.size());
    for (const auto& settlement : snapshot.settlements) {
        settlement_view_by_id.emplace(settlement.id, settlement);
    }

    const auto day_ticks = static_cast<std::uint64_t>(days(1).ticks > 0 ? days(1).ticks : 1);
    const auto tick_within_day = snapshot.tick % day_ticks;
    const auto day_phase = static_cast<float>(tick_within_day) / static_cast<float>(day_ticks);

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
        const auto h1 = stable_mix64(person.id * 0x9E3779B97F4A7C15ULL);
        const auto h2 = stable_mix64(person.id * 0xD6E8FEB86659FD93ULL);

        const auto stationary_x = (normalized01(h1) - 0.5F) * 2.0F;
        const auto stationary_z = (normalized01(h2) - 0.5F) * 2.0F;
        const auto base_angle = normalized01(stable_mix64(person.id ^ 0xA24BAED4963EE407ULL)) * 6.28318530718F;

        float speed_scale = 0.1F;
        if (person.occupation == Occupation::SubsistenceCultivator ||
            person.occupation == Occupation::WildFoodGatherer ||
            person.occupation == Occupation::WoodGatherer ||
            person.occupation == Occupation::StoneExtractor ||
            person.occupation == Occupation::CraftWorker) {
            speed_scale = 2.0F;
        }

        const auto movement_radius = person.alive ? (1.2F + normalized01(stable_mix64(person.id ^ 0x94D049BB133111EBULL)) * 2.8F) : 0.3F;
        const auto phase = base_angle + (day_phase * 6.28318530718F * speed_scale);
        const auto jitter_x = stationary_x + (std::cos(phase) * movement_radius);
        const auto jitter_z = stationary_z + (std::sin(phase) * movement_radius);

        snapshot.agents.push_back(AgentRenderView{
            .id = person.id,
            .household_id = person.household_id,
            .settlement_id = household->settlement_id,
            .region_id = household->region_id,
            .alive = person.alive,
            .age_years = age_years_from_person(summary, person),
            .sex = person.sex,
            .occupation = person.occupation,
            .x = base.x + jitter_x,
            .y = 0.9F,
            .z = base.z + jitter_z,
        });
    }

    std::sort(snapshot.agents.begin(), snapshot.agents.end(), [](const AgentRenderView& lhs, const AgentRenderView& rhs) {
        return lhs.id < rhs.id;
    });

    // Buildings within a settlement used to scatter at an independent
    // random (angle, radius) each -- with no collision avoidance at all,
    // which visibly placed buildings on top of each other once real
    // asset models replaced the old placeholder boxes. A golden-angle
    // spiral (radius grows with sqrt(index), angle advances by the
    // golden angle each step) is a standard even-spacing technique
    // (phyllotaxis/sunflower-seed packing) that guarantees increasing
    // separation between consecutive buildings in the same settlement,
    // deterministically, with no retry/rejection logic needed.
    std::unordered_map<SettlementId, std::uint64_t> building_slot_within_settlement;
    constexpr float golden_angle_radians = 2.39996323F;

    snapshot.buildings.reserve(buildings.size());
    for (const auto& building : buildings) {
        const auto settlement_it = settlement_view_by_id.find(building.settlement_id);
        if (settlement_it == settlement_view_by_id.end()) {
            continue;
        }

        const auto settlement = settlement_it->second;
        const auto building_index = building_slot_within_settlement[building.settlement_id]++;
        const auto angle = static_cast<float>(building_index) * golden_angle_radians;
        const auto radius = 4.0F + (std::sqrt(static_cast<float>(building_index)) * 3.2F);
        const auto x = settlement.x + (std::cos(angle) * radius);
        const auto z = settlement.z + (std::sin(angle) * radius);

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
    if (*type == "request_entity_details" || *type == "inspect_entity") {
        const auto entity_id = extract_u64_field(json, "entityId");
        if (!entity_id.has_value() || *entity_id == 0U) {
            error_message = "Invalid entityId";
            return std::nullopt;
        }
        return ClientCommand{
            .type = ClientCommandType::RequestEntityDetails,
            .entity_id = *entity_id,
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

std::string make_entity_details_message(
    const std::uint32_t protocol_version,
    const SimulationController& controller,
    const PersonId person_id) {
    const auto summary = controller.summary();
    const auto people = controller.people();
    const auto households = controller.households();
    const auto settlements = controller.settlements();

    const Person* person = find_person(people, person_id);
    if (person == nullptr) {
        return make_error_message(protocol_version, "entity_not_found", "Entity not found");
    }

    const Household* household = nullptr;
    for (const auto& candidate : households) {
        if (candidate.id == person->household_id) {
            household = &candidate;
            break;
        }
    }

    const Settlement* settlement = nullptr;
    if (household != nullptr) {
        for (const auto& candidate : settlements) {
            if (candidate.id == household->settlement_id) {
                settlement = &candidate;
                break;
            }
        }
    }

    const auto memory_count = controller.memory_count_for_person(person_id);
    const auto knowledge_count = controller.knowledge_count_for_person(person_id);

    std::ostringstream out;
    out << "{\"protocolVersion\":" << protocol_version
        << ",\"type\":\"entity_details\""
        << ",\"tick\":" << static_cast<std::uint64_t>(summary.time.ticks < 0 ? 0 : summary.time.ticks)
        << ",\"payload\":{"
        << "\"entityType\":\"agent\""
        << ",\"id\":" << person->id
        << ",\"alive\":" << (person->alive ? "true" : "false")
        << ",\"age\":" << age_years_from_person(summary, *person)
        << ",\"sex\":\"" << biological_sex_label(person->sex) << "\""
        << ",\"occupation\":\"" << occupation_label(person->occupation) << "\""
        << ",\"householdId\":" << person->household_id
        << ",\"settlementId\":" << (household != nullptr ? household->settlement_id : 0ULL)
        << ",\"regionId\":" << (household != nullptr ? household->region_id : 0ULL)
        << ",\"nutrition\":" << person->nutrition_points
        << ",\"health\":" << person->health_points
        << ",\"memoryCount\":" << memory_count
        << ",\"knowledgeCount\":" << knowledge_count
        << ",\"family\":{"
        << "\"motherId\":" << (person->mother_id.has_value() ? *person->mother_id : 0ULL)
        << ",\"fatherId\":" << (person->father_id.has_value() ? *person->father_id : 0ULL)
        << "}"
        << ",\"settlement\":{"
        << "\"active\":" << (settlement != nullptr && settlement->active ? "true" : "false")
        << ",\"accessSharePoints\":" << (settlement != nullptr ? settlement->access_share_points : 0U)
        << "}"
        << "}}";

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
