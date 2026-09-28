#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "universe/ui/simulation_controller.hpp"
#include "universe/ui/view_models.hpp"

namespace universe::ui {

struct AgentRenderView {
    PersonId id{};
    HouseholdId household_id{};
    SettlementId settlement_id{};
    RegionId region_id{};
    bool alive{false};
    std::uint32_t age_years{};
    BiologicalSex sex{BiologicalSex::Female};
    Occupation occupation{Occupation::None};
    ActivityKind current_activity{ActivityKind::Idle};
    float x{};
    float y{};
    float z{};
};

struct SettlementRenderView {
    SettlementId id{};
    RegionId region_id{};
    bool active{false};
    std::uint32_t access_share_points{};
    float x{};
    float y{};
    float z{};
};

struct BuildingRenderView {
    BuildingId id{};
    SettlementId settlement_id{};
    RegionId region_id{};
    std::string type{};
    std::string state{};
    bool settlement_commons_owned{true};
    std::uint64_t owner_household_id{};
    float progress{};
    float x{};
    float y{};
    float z{};
    std::uint16_t facing_degrees{};
};

struct RegionRenderView {
    RegionId id{};
    std::string environment_class{};
    std::int64_t land_area_units{};
    std::int64_t wild_food_capacity{};
    std::int64_t wild_food_stock{};
    std::int64_t wood_capacity{};
    std::int64_t wood_stock{};
    std::int64_t stone_capacity{};
    std::int64_t stone_remaining{};
};

struct WorldSnapshotView {
    std::uint64_t tick{};
    std::uint64_t year{};
    std::uint64_t population{};
    std::vector<AgentRenderView> agents{};
    std::vector<SettlementRenderView> settlements{};
    std::vector<BuildingRenderView> buildings{};
    std::vector<RegionRenderView> regions{};
};

struct WorldDeltaView {
    std::uint64_t tick{};
    std::uint64_t year{};
    std::uint64_t population{};
    std::vector<AgentRenderView> added_agents{};
    std::vector<AgentRenderView> updated_agents{};
    std::vector<PersonId> removed_agent_ids{};
    std::vector<SettlementRenderView> added_settlements{};
    std::vector<SettlementRenderView> updated_settlements{};
    std::vector<SettlementId> removed_settlement_ids{};
    std::vector<BuildingRenderView> added_buildings{};
    std::vector<BuildingRenderView> updated_buildings{};
    std::vector<BuildingId> removed_building_ids{};
};

enum class ClientCommandType : std::uint8_t {
    Hello = 0,
    Pause = 1,
    Resume = 2,
    SetSpeed = 3,
    SingleStep = 4,
    RequestSnapshot = 5,
    RequestViewModel = 6,
};

struct ClientCommand {
    ClientCommandType type{ClientCommandType::Hello};
    std::uint32_t speed{};
    std::uint64_t entity_id{};
    ViewModelKind view_model_kind{ViewModelKind::PersonSummary};
};

[[nodiscard]] WorldSnapshotView build_world_snapshot(const SimulationController& controller);
[[nodiscard]] WorldDeltaView build_world_delta(const WorldSnapshotView& current, const WorldSnapshotView& previous);
[[nodiscard]] std::optional<ClientCommand> parse_client_command(std::string_view json, std::string& error_message);
[[nodiscard]] std::string make_hello_message(std::uint32_t protocol_version);
[[nodiscard]] std::string make_status_message(std::uint32_t protocol_version, std::uint64_t tick, bool paused, std::uint32_t speed);
[[nodiscard]] std::string make_snapshot_message(std::uint32_t protocol_version, const WorldSnapshotView& snapshot, bool initial);
[[nodiscard]] std::string make_delta_message(std::uint32_t protocol_version, const WorldDeltaView& delta);
[[nodiscard]] std::string make_world_event_message(std::uint32_t protocol_version, const SimulationEvent& event);
[[nodiscard]] std::string make_error_message(std::uint32_t protocol_version, std::string_view code, std::string_view message);

}  // namespace universe::ui
