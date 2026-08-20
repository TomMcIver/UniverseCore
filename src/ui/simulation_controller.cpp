#include "universe/ui/simulation_controller.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <vector>

namespace universe::ui {

namespace {

[[nodiscard]] std::uint64_t count_alive(const std::span<const Person> people) noexcept {
    std::uint64_t alive = 0;
    for (const auto& person : people) {
        if (person.alive) {
            ++alive;
        }
    }
    return alive;
}

}  // namespace

SimulationController::SimulationController(SimulationConfig config)
    : simulation_(config) {}

void SimulationController::reset(const SimulationConfig config) {
    simulation_ = Simulation{config};
}

const Simulation& SimulationController::simulation() const noexcept {
    return simulation_;
}

SimulationSummary SimulationController::summary() const noexcept {
    const auto& state = simulation_.state();

    std::uint64_t healthy_population = 0;
    std::uint64_t mildly_malnourished = 0;
    std::uint64_t severely_malnourished = 0;
    std::uint64_t critical_health = 0;
    std::int64_t total_health = 0;
    std::uint64_t health_count = 0;

    for (const auto& person : state.people) {
        if (!person.alive) {
            continue;
        }

        ++health_count;
        total_health += person.health_points;

        if (person.health_points < 200 || person.nutrition_points < 180) {
            ++critical_health;
        } else if (person.nutrition_points < 350 || person.health_points < 400) {
            ++severely_malnourished;
        } else if (person.nutrition_points < 550 || person.health_points < 600) {
            ++mildly_malnourished;
        } else {
            ++healthy_population;
        }
    }

    const auto avg_health = health_count > 0
        ? static_cast<std::int32_t>(total_health / static_cast<std::int64_t>(health_count))
        : 0;

    return SimulationSummary{
        .seed = state.seed,
        .time = state.time,
        .year = state.year,
        .population = state.population,
        .total_people = static_cast<std::uint64_t>(state.people.size()),
        .alive_people = count_alive(state.people),
        .households = static_cast<std::uint64_t>(state.households.size()),
        .births = state.total_births,
        .deaths = state.total_deaths,
        .food_stored = state.total_food_stored,
        .food_produced = state.total_food_produced,
        .food_consumed = state.total_food_consumed,
        .unmet_food = state.total_unmet_food,
        .households_in_shortage = state.households_in_shortage,
        .healthy_population = healthy_population,
        .mildly_malnourished = mildly_malnourished,
        .severely_malnourished = severely_malnourished,
        .critical_health = critical_health,
        .average_health = avg_health,
    };
}

std::span<const Person> SimulationController::people() const noexcept {
    return simulation_.state().people;
}

std::span<const Household> SimulationController::households() const noexcept {
    return simulation_.state().households;
}

std::span<const SimulationEvent> SimulationController::events() const noexcept {
    return simulation_.state().event_log;
}

void SimulationController::advance_days(const std::uint64_t days_count) {
    simulation_.advance(days(static_cast<std::int64_t>(days_count)));
}

void SimulationController::advance_years(const std::uint64_t years_count) {
    simulation_.run_years(years_count);
}

SnapshotOperationResult SimulationController::save_snapshot_file(const std::string& path) const {
    SnapshotOperationResult result{};
    result.path = path;

    const auto bytes = simulation_.save_snapshot();

    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) {
        result.error_code = SnapshotErrorCode::InvalidState;
        result.message = "Failed to open snapshot output file";
        return result;
    }

    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            result.error_code = SnapshotErrorCode::InvalidState;
            result.message = "Failed to write snapshot bytes";
            return result;
        }
    }

    result.ok = true;
    result.hash = snapshot_hash64(bytes);
    return result;
}

SnapshotOperationResult SimulationController::load_snapshot_file(const std::string& path) {
    SnapshotOperationResult result{};
    result.path = path;

    std::ifstream file{path, std::ios::binary};
    if (!file) {
        result.error_code = SnapshotErrorCode::InvalidState;
        result.message = "Failed to open snapshot input file";
        return result;
    }

    file.seekg(0, std::ios::end);
    const auto end_pos = file.tellg();
    if (end_pos < 0) {
        result.error_code = SnapshotErrorCode::InvalidStructure;
        result.message = "Failed to read snapshot size";
        return result;
    }

    const auto bytes_count = static_cast<std::size_t>(end_pos);
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> bytes(bytes_count);
    if (bytes_count > 0) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes_count));
        if (!file) {
            result.error_code = SnapshotErrorCode::TruncatedData;
            result.message = "Failed to read snapshot bytes";
            return result;
        }
    }

    const auto load_result = Simulation::load_snapshot(bytes, simulation_);
    if (!load_result.ok) {
        result.error_code = load_result.code;
        result.message = load_result.message;
        return result;
    }

    result.ok = true;
    result.hash = snapshot_hash64(bytes);
    return result;
}

}  // namespace universe::ui
