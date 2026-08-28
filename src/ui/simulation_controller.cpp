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
    std::unordered_map<PersonId, std::int64_t> food_need_by_person;
    food_need_by_person.reserve(state.people.size());

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
        labour_capacity_by_person.emplace(person.id, available_capacity);
        food_need_by_person.emplace(
            person.id,
            person_age_years >= 18U ? labour_parameters.adult_daily_food_units : labour_parameters.child_daily_food_units);

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

            const auto demand_it = food_need_by_person.find(member_id);
            if (demand_it != food_need_by_person.end()) {
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
        .total_labour_capacity = state.total_labour_capacity,
        .total_labour_demand = state.total_labour_demand,
        .total_labour_allocated = state.total_labour_allocated,
        .labour_shortfall = state.labour_shortfall,
        .zero_capable_worker_households = zero_capable_worker_households,
        .labour_constrained_households = labour_constrained_households,
        .eligible_workers = state.eligible_workers,
        .active_workers = state.active_workers,
        .unallocated_workers = state.unallocated_workers,
        .cultivators = state.cultivators,
        .gatherers = state.gatherers,
        .wood_gatherers = state.wood_gatherers,
        .stone_extractors = state.stone_extractors,
        .craft_workers = state.craft_workers,
        .cultivation_labour_available = state.cultivation_labour_available,
        .cultivation_labour_allocated = state.cultivation_labour_allocated,
        .gathering_labour_available = state.gathering_labour_available,
        .gathering_labour_allocated = state.gathering_labour_allocated,
        .wood_labour_available = state.wood_labour_available,
        .wood_labour_allocated = state.wood_labour_allocated,
        .stone_labour_available = state.stone_labour_available,
        .stone_labour_allocated = state.stone_labour_allocated,
        .craft_labour_available = state.craft_labour_available,
        .craft_labour_allocated = state.craft_labour_allocated,
        .material_labour_demand = state.material_labour_demand,
        .material_labour_allocated = state.material_labour_allocated,
        .material_labour_shortfall = state.material_labour_shortfall,
        .material_production_shortfall = state.material_production_shortfall,
        .total_household_wealth = state.total_household_wealth,
        .settlement_commons_food = state.total_settlement_commons_food,
        .transfer_volume = state.total_transfer_volume,
        .exchange_volume = state.total_exchange_volume,
        .active_buildings = state.active_buildings,
        .active_projects = state.active_projects,
        .wood_capacity = state.total_wood_capacity,
        .wood_stock = state.total_wood_stock,
        .stone_capacity = state.total_stone_capacity,
        .stone_remaining = state.total_stone_remaining,
        .wood_inventory = state.total_wood_inventory,
        .stone_inventory = state.total_stone_inventory,
        .timber_inventory = state.total_timber_inventory,
        .basic_tool_inventory = state.total_basic_tool_inventory,
        .basic_tool_broken = state.total_basic_tool_broken,
        .average_tool_coverage_per_10k = state.average_tool_coverage_per_10k,
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

std::span<const AgentMemory> SimulationController::agent_memories() const noexcept {
    return simulation_.agent_memories();
}

std::span<const KnowledgeTransmission> SimulationController::knowledge_transmissions() const noexcept {
    return simulation_.knowledge_transmissions();
}

std::span<const KnowledgeRecord> SimulationController::knowledge_records() const noexcept {
    return simulation_.knowledge_records();
}

std::span<const Institution> SimulationController::institutions() const noexcept {
    return simulation_.institutions();
}

std::vector<AgentMemory> SimulationController::memories_for_person(const PersonId person_id) const {
    return simulation_.memories_for_person(person_id);
}

std::vector<KnowledgeTransmission> SimulationController::transmissions_for_recipient(const PersonId person_id) const {
    return simulation_.transmissions_for_recipient(person_id);
}

std::optional<KnowledgeTransmission> SimulationController::transmission_for_memory(const MemoryId recipient_memory_id) const {
    return simulation_.transmission_for_memory(recipient_memory_id);
}

std::optional<KnowledgeRecordAccess> SimulationController::record_access_for_memory(const MemoryId reader_memory_id) const {
    return simulation_.record_access_for_memory(reader_memory_id);
}

bool SimulationController::person_remembers_event(const PersonId person_id, const EventId event_id) const noexcept {
    return simulation_.person_remembers_event(person_id, event_id);
}

std::uint64_t SimulationController::memory_count_for_person(const PersonId person_id) const noexcept {
    return simulation_.memory_count_for_person(person_id);
}

std::span<const AgentKnowledge> SimulationController::agent_knowledge() const noexcept {
    return simulation_.agent_knowledge();
}

std::vector<AgentKnowledge> SimulationController::knowledge_for_person(const PersonId person_id) const {
    return simulation_.knowledge_for_person(person_id);
}

std::uint64_t SimulationController::knowledge_count_for_person(const PersonId person_id) const noexcept {
    return simulation_.knowledge_count_for_person(person_id);
}

std::optional<KnownLifeStatus> SimulationController::known_life_status(
    const PersonId knower_id,
    const PersonId subject_id) const noexcept {
    return simulation_.known_life_status(knower_id, subject_id);
}

std::optional<HouseholdId> SimulationController::known_household_for_person(
    const PersonId knower_id,
    const PersonId subject_id) const noexcept {
    return simulation_.known_household_for_person(knower_id, subject_id);
}

std::optional<SettlementStatusKnowledgeFact> SimulationController::known_settlement_status(
    const PersonId knower_id,
    const SettlementId settlement_id) const noexcept {
    return simulation_.known_settlement_status(knower_id, settlement_id);
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
                (std::min(region->wild_food_stock, region->wild_food_harvest_limit_per_day) * static_cast<std::int64_t>(settlement.access_share_points)) / 10'000;
            summary.accessible_food_capacity_per_day = productive + wild;
        }

        std::int64_t settlement_required_food = 0;
        std::int64_t settlement_stored_food = 0;

        for (const auto& household : state.households) {
            if (household.settlement_id != settlement.id) {
                continue;
            }

            ++summary.households;
            summary.food_stored += household.resources.food_inventory.value;
            summary.food_unmet_total += household.resources.unmet_food_total;
            settlement_stored_food += household.resources.food_inventory.value;

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
                const auto labour_capacity = person_labour_capacity_units(
                    true,
                    age,
                    person->nutrition_points,
                    person->health_points,
                    labour_parameters,
                    100U);
                const auto required_food = age >= 18U
                    ? labour_parameters.adult_daily_food_units
                    : labour_parameters.child_daily_food_units;
                settlement_required_food += required_food;
                summary.labour_capacity += labour_capacity;

                const bool occupation_eligible =
                    age >= 18U &&
                    person->nutrition_points >= 160 &&
                    person->health_points >= 160 &&
                    labour_capacity > 0;
                if (!occupation_eligible) {
                    continue;
                }

                ++summary.eligible_workers;
                if (person->occupation == Occupation::SubsistenceCultivator) {
                    ++summary.active_workers;
                    ++summary.cultivators;
                    summary.cultivation_labour_available += labour_capacity;
                } else if (person->occupation == Occupation::WildFoodGatherer) {
                    ++summary.active_workers;
                    ++summary.gatherers;
                    summary.gathering_labour_available += labour_capacity;
                } else if (person->occupation == Occupation::WoodGatherer) {
                    ++summary.active_workers;
                    ++summary.wood_gatherers;
                    summary.wood_labour_available += labour_capacity;
                } else if (person->occupation == Occupation::StoneExtractor) {
                    ++summary.active_workers;
                    ++summary.stone_extractors;
                    summary.stone_labour_available += labour_capacity;
                } else if (person->occupation == Occupation::CraftWorker) {
                    ++summary.active_workers;
                    ++summary.craft_workers;
                    summary.craft_labour_available += labour_capacity;
                } else {
                    ++summary.unallocated_workers;
                }
            }
        }

        summary.wood_inventory = settlement.materials.wood.value;
        summary.stone_inventory = settlement.materials.stone.value;
        summary.timber_inventory = settlement.materials.timber.value;
        summary.basic_tool_inventory = settlement.materials.basic_tool.value;
        summary.cumulative_basic_tool_broken = settlement.cumulative_basic_tool_broken;
        summary.tool_coverage_per_10k = settlement.last_tool_coverage_per_10k;

        if (summary.accessible_food_capacity_per_day > 0) {
            summary.pressure_per_10k = static_cast<std::uint32_t>(
                std::min<std::int64_t>(
                    100'000,
                    (settlement_required_food * 10'000) / summary.accessible_food_capacity_per_day));
        } else if (settlement_required_food > 0) {
            summary.pressure_per_10k = 100'000U;
        }

        std::int64_t output_target = 0;
        if (settlement_required_food > 0 && summary.accessible_food_capacity_per_day > 0) {
            const auto buffer_days = settlement_stored_food / std::max<std::int64_t>(1, settlement_required_food);
            if (buffer_days >= 60) {
                output_target = settlement_required_food / 2;
            } else if (buffer_days >= 30) {
                output_target = settlement_required_food;
            } else if (buffer_days >= 7) {
                output_target = settlement_required_food + (settlement_required_food / 4);
            } else {
                output_target = settlement_required_food + (settlement_required_food / 2);
            }
            output_target = std::min(output_target, summary.accessible_food_capacity_per_day);
        }

        std::int64_t cultivation_opportunity = 0;
        std::int64_t gathering_opportunity = 0;
        if (region != nullptr && settlement.active) {
            cultivation_opportunity =
                (region->productive_capacity_per_day * static_cast<std::int64_t>(settlement.access_share_points)) / 10'000;
            gathering_opportunity =
                (std::min(region->wild_food_stock, region->wild_food_harvest_limit_per_day) * static_cast<std::int64_t>(settlement.access_share_points)) / 10'000;
        }

        const auto total_opportunity = cultivation_opportunity + gathering_opportunity;
        if (total_opportunity > 0 && output_target > 0) {
            summary.labour_demand = output_target;
            const auto cultivation_demand = (output_target * cultivation_opportunity) / total_opportunity;
            const auto gathering_demand = output_target - cultivation_demand;

            summary.cultivation_labour_allocated = std::min(summary.cultivation_labour_available, std::max<std::int64_t>(0, cultivation_demand));
            summary.gathering_labour_allocated = std::min(summary.gathering_labour_available, std::max<std::int64_t>(0, gathering_demand));
        }

        const auto timber_target = std::max<std::int64_t>(8, static_cast<std::int64_t>(summary.alive_population / 2));
        const auto tool_target = std::max<std::int64_t>(4, static_cast<std::int64_t>((summary.eligible_workers + 2) / 3));
        summary.material_shortfall =
            std::max<std::int64_t>(0, timber_target - summary.timber_inventory) +
            std::max<std::int64_t>(0, tool_target - summary.basic_tool_inventory);

        summary.wood_labour_allocated = std::min(summary.wood_labour_available, std::max<std::int64_t>(0, summary.material_shortfall));
        summary.stone_labour_allocated = std::min(summary.stone_labour_available, std::max<std::int64_t>(0, summary.material_shortfall));
        summary.craft_labour_allocated = std::min(summary.craft_labour_available, std::max<std::int64_t>(0, summary.material_shortfall));

        summary.labour_allocated = summary.cultivation_labour_allocated + summary.gathering_labour_allocated;
        summary.labour_shortfall = std::max<std::int64_t>(0, summary.labour_demand - summary.labour_allocated);

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
