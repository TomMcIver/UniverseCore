#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"

namespace universe::ui {

struct SimulationSummary {
    SeedValue seed{};
    SimulationTime time{};
    std::uint64_t year{};
    std::uint64_t population{};
    std::uint64_t total_people{};
    std::uint64_t alive_people{};
    std::uint64_t households{};
    std::uint64_t births{};
    std::uint64_t deaths{};

    std::int64_t food_stored{};
    std::int64_t food_produced{};
    std::int64_t food_consumed{};
    std::int64_t unmet_food{};
    std::uint64_t households_in_shortage{};

    std::uint64_t healthy_population{};
    std::uint64_t mildly_malnourished{};
    std::uint64_t severely_malnourished{};
    std::uint64_t critical_health{};
    std::int32_t average_health{};
};

struct SnapshotOperationResult {
    bool ok{false};
    std::string path{};
    std::uint64_t hash{};
    SnapshotErrorCode error_code{SnapshotErrorCode::None};
    std::string message{};
};

class SimulationController {
public:
    explicit SimulationController(SimulationConfig config = {});

    void reset(SimulationConfig config);
    [[nodiscard]] const Simulation& simulation() const noexcept;

    [[nodiscard]] SimulationSummary summary() const noexcept;

    [[nodiscard]] std::span<const Person> people() const noexcept;
    [[nodiscard]] std::span<const Household> households() const noexcept;
    [[nodiscard]] std::span<const SimulationEvent> events() const noexcept;

    void advance_days(std::uint64_t days);
    void advance_years(std::uint64_t years);

    [[nodiscard]] SnapshotOperationResult save_snapshot_file(const std::string& path) const;
    [[nodiscard]] SnapshotOperationResult load_snapshot_file(const std::string& path);

private:
    Simulation simulation_;
};

}  // namespace universe::ui
