#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "universe/ui/simulation_controller.hpp"

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

struct WorldSnapshotView {
    std::uint64_t tick{};
    std::uint64_t year{};
    std::uint64_t population{};
    std::vector<AgentRenderView> agents{};
    std::vector<SettlementRenderView> settlements{};
};

enum class ClientCommandType : std::uint8_t {
    Hello = 0,
    Pause = 1,
    Resume = 2,
    SetSpeed = 3,
    SingleStep = 4,
    RequestSnapshot = 5,
    RequestEntityDetails = 6,
};

struct ClientCommand {
    ClientCommandType type{ClientCommandType::Hello};
    std::uint32_t speed{};
    std::uint64_t entity_id{};
};

[[nodiscard]] WorldSnapshotView build_world_snapshot(const SimulationController& controller);
[[nodiscard]] std::optional<ClientCommand> parse_client_command(std::string_view json, std::string& error_message);
[[nodiscard]] std::string make_hello_message(std::uint32_t protocol_version);
[[nodiscard]] std::string make_status_message(std::uint32_t protocol_version, std::uint64_t tick, bool paused, std::uint32_t speed);
[[nodiscard]] std::string make_snapshot_message(std::uint32_t protocol_version, const WorldSnapshotView& snapshot, bool initial);
[[nodiscard]] std::string make_entity_details_message(std::uint32_t protocol_version, const SimulationController& controller, PersonId person_id);
[[nodiscard]] std::string make_world_event_message(std::uint32_t protocol_version, const SimulationEvent& event);
[[nodiscard]] std::string make_error_message(std::uint32_t protocol_version, std::string_view code, std::string_view message);

}  // namespace universe::ui
