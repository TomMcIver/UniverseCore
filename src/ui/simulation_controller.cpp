#include "universe/ui/simulation_controller.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <unordered_map>
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
    const auto labour_parameters = FoodModelParameters{};
    const auto settlement_views = settlement_summaries();

    std::uint64_t healthy_population = 0;
    std::uint64_t mildly_malnourished = 0;
    std::uint64_t severely_malnourished = 0;
    std::uint64_t critical_health = 0;
    std::int64_t total_health = 0;
    std::uint64_t health_count = 0;
    std::uint64_t child_population = 0;
    std::uint64_t working_age_population = 0;
    std::uint64_t elderly_population = 0;

    std::unordered_map<PersonId, std::int64_t> labour_capacity_by_person;
    labour_capacity_by_person.reserve(state.people.size());
    std::unordered_map<PersonId, std::int64_t> labour_demand_by_person;
    labour_demand_by_person.reserve(state.people.size());
    std::int64_t total_labour_capacity = 0;
    std::int64_t total_labour_demand = 0;

    for (const auto& person : state.people) {
        const auto age = (state.year > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(state.year);
        const auto person_age_years = (age > person.birth_year)
            ? static_cast<std::uint32_t>(age - person.birth_year)
            : 0U;

        if (!person.alive) {
            labour_capacity_by_person.emplace(person.id, 0);
            continue;
        }

        if (person_age_years < 18U) {
            ++child_population;
        } else if (person_age_years < 65U) {
            ++working_age_population;
        } else {
            ++elderly_population;
        }

        const auto available_capacity = person_labour_capacity_units(
            person.alive,
            person_age_years,
            person.nutrition_points,
            person.health_points,
            labour_parameters,
            100U);
        const auto demand_capacity = age_based_labour_capacity_units(person_age_years, labour_parameters);
        labour_capacity_by_person.emplace(person.id, available_capacity);
        labour_demand_by_person.emplace(person.id, demand_capacity);
        total_labour_capacity += available_capacity;
        total_labour_demand += demand_capacity;

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

    std::uint64_t zero_capable_worker_households = 0;
    std::uint64_t labour_constrained_households = 0;
    std::uint64_t active_settlements = 0;
    std::uint64_t extinct_settlements = 0;
    std::uint64_t largest_settlement_population = 0;
    std::uint64_t high_pressure_settlements = 0;
    std::uint64_t total_settlement_population = 0;

    for (const auto& household : state.households) {
        std::int64_t household_capacity = 0;
        std::int64_t household_demand = 0;

        for (const auto member_id : household.member_ids) {
            const auto labour_it = labour_capacity_by_person.find(member_id);
            if (labour_it != labour_capacity_by_person.end()) {
                household_capacity += labour_it->second;
            }

            const auto demand_it = labour_demand_by_person.find(member_id);
            if (demand_it != labour_demand_by_person.end()) {
                household_demand += demand_it->second;
            }
        }

        if (household_capacity <= 0) {
            ++zero_capable_worker_households;
        }
        if (household_demand > household_capacity) {
            ++labour_constrained_households;
        }
    }

    for (const auto& settlement : settlement_views) {
        if (settlement.active) {
            ++active_settlements;
        } else {
            ++extinct_settlements;
        }
        total_settlement_population += settlement.alive_population;
        largest_settlement_population = std::max(largest_settlement_population, settlement.alive_population);
        if (settlement.active && settlement.pressure_per_10k >= 5'000U) {
            ++high_pressure_settlements;
        }
    }

    const auto average_settlement_population = active_settlements > 0
        ? (total_settlement_population / active_settlements)
        : 0U;

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
        .active_settlements = active_settlements,
        .extinct_settlements = extinct_settlements,
        .average_settlement_population = average_settlement_population,
        .largest_settlement_population = largest_settlement_population,
        .high_pressure_settlements = high_pressure_settlements,
        .food_stored = state.total_food_stored,
        .food_produced = state.total_food_produced,
        .food_consumed = state.total_food_consumed,
        .unmet_food = state.total_unmet_food,
        .households_in_shortage = state.households_in_shortage,
        .total_labour_capacity = total_labour_capacity,
        .total_labour_demand = total_labour_demand,
        .zero_capable_worker_households = zero_capable_worker_households,
        .labour_constrained_households = labour_constrained_households,
        .child_population = child_population,
        .working_age_population = working_age_population,
        .elderly_population = elderly_population,
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

std::span<const Settlement> SimulationController::settlements() const noexcept {
    return simulation_.state().settlements;
}

std::span<const RegionEnvironmentState> SimulationController::regions() const noexcept {
    return simulation_.state().regions;
}

std::span<const SimulationEvent> SimulationController::events() const noexcept {
    return simulation_.state().event_log;
}

std::vector<SettlementSummaryView> SimulationController::settlement_summaries() const {
    const auto& state = simulation_.state();
    const auto labour_parameters = FoodModelParameters{};

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state.people.size());
    for (const auto& person : state.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<RegionId, const RegionEnvironmentState*> region_by_id;
    region_by_id.reserve(state.regions.size());
    for (const auto& region : state.regions) {
        region_by_id.emplace(region.id, &region);
    }

    std::vector<SettlementSummaryView> summaries;
    summaries.reserve(state.settlements.size());
    for (const auto& settlement : state.settlements) {
        SettlementSummaryView summary{};
        summary.id = settlement.id;
        summary.region_id = settlement.region_id;
        summary.active = settlement.active;

        const auto region_it = region_by_id.find(settlement.region_id);
        const RegionEnvironmentState* region = (region_it == region_by_id.end()) ? nullptr : region_it->second;
        if (region != nullptr && settlement.active) {
            const auto productive =
                (region->productive_capacity_per_day * static_cast<std::int64_t>(settlement.access_share_points)) / 10'000;
            const auto wild =
                (region->wild_food_harvest_limit_per_day * static_cast<std::int64_t>(settlement.access_share_points)) / 10'000;
            summary.accessible_food_capacity_per_day = productive + wild;
        }

        for (const auto& household : state.households) {
            if (household.settlement_id != settlement.id) {
                continue;
            }

            ++summary.households;
            summary.food_stored += household.resources.food_inventory.value;
            summary.food_unmet_total += household.resources.unmet_food_total;

            for (const auto member_id : household.member_ids) {
                const auto person_it = person_by_id.find(member_id);
                if (person_it == person_by_id.end() || !person_it->second->alive) {
                    continue;
                }

                const auto* person = person_it->second;
                const auto current_year = (state.year > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    ? std::numeric_limits<std::int64_t>::max()
                    : static_cast<std::int64_t>(state.year);
                const auto age = (current_year > person->birth_year)
                    ? static_cast<std::uint32_t>(current_year - person->birth_year)
                    : 0U;

                ++summary.alive_population;
                summary.labour_capacity += person_labour_capacity_units(
                    true,
                    age,
                    person->nutrition_points,
                    person->health_points,
                    labour_parameters,
                    100U);
                summary.labour_demand += age_based_labour_capacity_units(age, labour_parameters);

                if (summary.accessible_food_capacity_per_day > 0) {
                    summary.pressure_per_10k = static_cast<std::uint32_t>(
                        std::min<std::int64_t>(
                            100'000,
                            (summary.labour_demand * 10'000) / summary.accessible_food_capacity_per_day));
                } else if (summary.labour_demand > 0) {
                    summary.pressure_per_10k = 100'000U;
                }
            }
        }

        summaries.push_back(summary);
    }

    return summaries;
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
