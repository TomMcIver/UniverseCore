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

    std::unordered_map<SettlementId, std::uint64_t> settlement_order_index;
    settlement_order_index.reserve(settlements.size());
    std::uint64_t settlement_slot = 0;
    for (const auto& settlement : settlements) {
        settlement_order_index.emplace(settlement.id, settlement_slot);
        ++settlement_slot;
    }

    WorldSnapshotView snapshot{};
    snapshot.tick = static_cast<std::uint64_t>(summary.time.ticks < 0 ? 0 : summary.time.ticks);
    snapshot.year = summary.year;
    snapshot.population = summary.population;

    snapshot.settlements.reserve(settlements.size());
    for (const auto& settlement : settlements) {
        const auto ring_slot = settlement_order_index[settlement.id] % 24ULL;
        const auto ring_index = settlement_order_index[settlement.id] / 24ULL;
        const auto angle = static_cast<float>(ring_slot) * (6.28318530718F / 24.0F);
        const auto radius = 36.0F + static_cast<float>(ring_index * 16ULL);

        snapshot.settlements.push_back(SettlementRenderView{
            .id = settlement.id,
            .region_id = settlement.region_id,
            .active = settlement.active,
            .access_share_points = settlement.access_share_points,
            .x = std::cos(angle) * radius,
            .y = 0.0F,
            .z = std::sin(angle) * radius,
        });
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

    snapshot.buildings.reserve(buildings.size());
    for (const auto& building : buildings) {
        const auto settlement_it = settlement_view_by_id.find(building.settlement_id);
        if (settlement_it == settlement_view_by_id.end()) {
            continue;
        }

        const auto settlement = settlement_it->second;
        const auto b1 = stable_mix64(building.id * 0xBF58476D1CE4E5B9ULL);
        const auto b2 = stable_mix64(building.id * 0x9E3779B97F4A7C15ULL);
        const auto angle = normalized01(b1) * 6.28318530718F;
        const auto radius = 5.0F + (normalized01(b2) * 7.0F);
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

std::string make_snapshot_message(
    const std::uint32_t protocol_version,
    const WorldSnapshotView& snapshot,
    const bool initial) {
    std::ostringstream out;
    out << "{\"protocolVersion\":" << protocol_version
        << ",\"type\":\"" << (initial ? "initial_snapshot" : "world_delta") << "\""
        << ",\"tick\":" << snapshot.tick
        << ",\"payload\":{"
        << "\"year\":" << snapshot.year
        << ",\"population\":" << snapshot.population
        << ",\"agents\":[";

    for (std::size_t i = 0; i < snapshot.agents.size(); ++i) {
        const auto& agent = snapshot.agents[i];
        if (i > 0U) {
            out << ',';
        }
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

    out << "],\"settlements\":[";
    for (std::size_t i = 0; i < snapshot.settlements.size(); ++i) {
        const auto& settlement = snapshot.settlements[i];
        if (i > 0U) {
            out << ',';
        }
        out << "{\"id\":" << settlement.id
            << ",\"regionId\":" << settlement.region_id
            << ",\"active\":" << (settlement.active ? "true" : "false")
            << ",\"accessSharePoints\":" << settlement.access_share_points
            << ",\"position\":{\"x\":" << settlement.x << ",\"y\":" << settlement.y << ",\"z\":" << settlement.z << "}}";
    }

    out << "],\"buildings\":[";
    for (std::size_t i = 0; i < snapshot.buildings.size(); ++i) {
        const auto& building = snapshot.buildings[i];
        if (i > 0U) {
            out << ',';
        }
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

    out << "],\"regions\":[";
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
