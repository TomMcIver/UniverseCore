#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

template <typename T>
bool parse_number(const std::string_view text, T& value) {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable
              << " [--seed N] [--years N] [--population N] [--dump-events N]"
              << " [--food-scale N] [--initial-food N] [--food-system 0|1]"
              << " [--materials-system 0|1] [--wood-scale N] [--stone-scale N]"
              << " [--wood-regen-scale N] [--craft-scale N]"
              << " [--effort-scale N] [--report-interval N]"
              << " [--save PATH] [--load PATH]\n";
}

bool read_binary_file(const std::string_view path, std::vector<std::byte>& bytes) {
    std::ifstream file{std::string{path}, std::ios::binary};
    if (!file) {
        return false;
    }

    file.seekg(0, std::ios::end);
    const auto end_pos = file.tellg();
    if (end_pos < 0) {
        return false;
    }
    const auto file_size = static_cast<std::size_t>(end_pos);
    file.seekg(0, std::ios::beg);

    bytes.assign(file_size, std::byte{0});
    if (file_size > 0) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(file_size));
        if (!file) {
            return false;
        }
    }

    return true;
}

bool write_binary_file(const std::string_view path, const std::vector<std::byte>& bytes) {
    std::ofstream file{std::string{path}, std::ios::binary | std::ios::trunc};
    if (!file) {
        return false;
    }

    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            return false;
        }
    }

    return true;
}

}  // namespace

int main(int argc, char** argv) {
    universe::SimulationConfig config{};
    std::uint64_t years = 100;
    std::uint64_t dump_events = 0;
    std::uint64_t report_interval = 10;
    std::optional<std::string> save_path;
    std::optional<std::string> load_path;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};

        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }

        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << arg << '\n';
            return 2;
        }

        const std::string_view value{argv[++i]};
        if (arg == "--seed") {
            if (!parse_number(value, config.seed)) {
                std::cerr << "Invalid seed\n";
                return 2;
            }
        } else if (arg == "--years") {
            if (!parse_number(value, years)) {
                std::cerr << "Invalid years\n";
                return 2;
            }
        } else if (arg == "--population") {
            if (!parse_number(value, config.initial_population)) {
                std::cerr << "Invalid population\n";
                return 2;
            }
        } else if (arg == "--dump-events") {
            if (!parse_number(value, dump_events)) {
                std::cerr << "Invalid dump-events count\n";
                return 2;
            }
        } else if (arg == "--food-scale") {
            if (!parse_number(value, config.food_production_scale_percent)) {
                std::cerr << "Invalid food-scale\n";
                return 2;
            }
        } else if (arg == "--initial-food") {
            if (!parse_number(value, config.initial_household_food_units)) {
                std::cerr << "Invalid initial-food\n";
                return 2;
            }
        } else if (arg == "--food-system") {
            std::uint32_t enabled = 0;
            if (!parse_number(value, enabled) || enabled > 1U) {
                std::cerr << "Invalid food-system (expected 0 or 1)\n";
                return 2;
            }
            config.food_system_enabled = (enabled == 1U);
        } else if (arg == "--effort-scale") {
            if (!parse_number(value, config.productive_effort_scale_percent)) {
                std::cerr << "Invalid effort-scale\n";
                return 2;
            }
        } else if (arg == "--materials-system") {
            std::uint32_t enabled = 0;
            if (!parse_number(value, enabled) || enabled > 1U) {
                std::cerr << "Invalid materials-system (expected 0 or 1)\n";
                return 2;
            }
            config.materials_system_enabled = (enabled == 1U);
        } else if (arg == "--wood-scale") {
            if (!parse_number(value, config.wood_resource_scale_percent)) {
                std::cerr << "Invalid wood-scale\n";
                return 2;
            }
        } else if (arg == "--stone-scale") {
            if (!parse_number(value, config.stone_resource_scale_percent)) {
                std::cerr << "Invalid stone-scale\n";
                return 2;
            }
        } else if (arg == "--wood-regen-scale") {
            if (!parse_number(value, config.wood_regeneration_scale_percent)) {
                std::cerr << "Invalid wood-regen-scale\n";
                return 2;
            }
        } else if (arg == "--craft-scale") {
            if (!parse_number(value, config.material_crafting_scale_percent)) {
                std::cerr << "Invalid craft-scale\n";
                return 2;
            }
        } else if (arg == "--report-interval") {
            if (!parse_number(value, report_interval) || report_interval == 0U) {
                std::cerr << "Invalid report-interval\n";
                return 2;
            }
        } else if (arg == "--save") {
            save_path = std::string{value};
        } else if (arg == "--load") {
            load_path = std::string{value};
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            print_usage(argv[0]);
            return 2;
        }
    }

    std::optional<universe::Simulation> simulation;
    if (load_path.has_value()) {
        std::vector<std::byte> snapshot_bytes;
        if (!read_binary_file(*load_path, snapshot_bytes)) {
            std::cerr << "Failed to read snapshot file: " << *load_path << '\n';
            return 2;
        }

        universe::Simulation loaded{universe::SimulationConfig{}};
        const auto load_result = universe::Simulation::load_snapshot(snapshot_bytes, loaded);
        if (!load_result.ok) {
            std::cerr << "Snapshot load failed ("
                      << static_cast<unsigned>(load_result.code)
                      << "): " << load_result.message << '\n';
            return 2;
        }

        std::cout << "loaded_snapshot=" << *load_path
                  << " bytes=" << snapshot_bytes.size()
                  << " snapshot_hash=" << universe::snapshot_hash64(snapshot_bytes)
                  << '\n';
        simulation.emplace(std::move(loaded));
    } else {
        simulation.emplace(config);
    }

    const auto& initial_state = simulation->state();
    std::cout << "UniverseCore prototype\n"
              << "seed=" << initial_state.seed << '\n'
              << "year=" << initial_state.year
              << " population=" << initial_state.population << "\n\n";

    std::uint64_t last_births = initial_state.total_births;
    std::uint64_t last_deaths = initial_state.total_deaths;
    std::uint64_t last_population = initial_state.population;
    std::uint64_t last_active_settlements = 0;
    for (const auto& settlement : initial_state.settlements) {
        if (settlement.active) {
            ++last_active_settlements;
        }
    }

    for (std::uint64_t i = 0; i < years; ++i) {
        simulation->step_year();
        const auto& state = simulation->state();
        if (state.year <= 10 || state.year % report_interval == 0 || state.year == years) {
            std::uint64_t adults = 0;
            std::uint64_t children = 0;
            std::uint64_t alive_people = 0;
            std::uint64_t reproductive_females = 0;
            std::uint64_t reproductive_males = 0;
            std::uint64_t settlements_active = 0;
            std::uint64_t settlements_extinct = 0;
            std::uint64_t largest_settlement = 0;
            std::uint64_t smallest_active_settlement = std::numeric_limits<std::uint64_t>::max();
            std::uint64_t settlements_under_10 = 0;
            std::uint64_t settlements_under_25 = 0;
            std::uint64_t high_pressure_settlements = 0;
            std::uint64_t total_settlement_population = 0;
            std::uint64_t nutrition_under_350 = 0;
            std::uint64_t nutrition_under_180 = 0;
            std::uint64_t health_under_400 = 0;
            std::uint64_t health_under_220 = 0;
            std::uint64_t age_0_9 = 0;
            std::uint64_t age_10_19 = 0;
            std::uint64_t age_20_39 = 0;
            std::uint64_t age_40_59 = 0;
            std::uint64_t age_60_plus = 0;
            std::int64_t total_nutrition = 0;
            std::int64_t total_health = 0;
            const auto max_i64_as_u64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
            const auto current_year = (state.year > max_i64_as_u64)
                ? std::numeric_limits<std::int64_t>::max()
                : static_cast<std::int64_t>(state.year);

            for (const auto& person : state.people) {
                if (!person.alive) {
                    continue;
                }

                ++alive_people;
                const auto age = (current_year > person.birth_year)
                    ? static_cast<std::uint64_t>(current_year - person.birth_year)
                    : 0ULL;
                if (age >= 18U) {
                    ++adults;
                } else {
                    ++children;
                }

                if (person.sex == universe::BiologicalSex::Female && age >= 18U && age <= 44U) {
                    ++reproductive_females;
                }
                if (person.sex == universe::BiologicalSex::Male && age >= 18U && age <= 59U) {
                    ++reproductive_males;
                }

                if (age <= 9U) {
                    ++age_0_9;
                } else if (age <= 19U) {
                    ++age_10_19;
                } else if (age <= 39U) {
                    ++age_20_39;
                } else if (age <= 59U) {
                    ++age_40_59;
                } else {
                    ++age_60_plus;
                }

                if (person.nutrition_points < 350) {
                    ++nutrition_under_350;
                }
                if (person.nutrition_points < 180) {
                    ++nutrition_under_180;
                }
                if (person.health_points < 400) {
                    ++health_under_400;
                }
                if (person.health_points < 220) {
                    ++health_under_220;
                }
                total_nutrition += person.nutrition_points;
                total_health += person.health_points;
            }

            for (const auto& settlement : state.settlements) {
                if (settlement.active) {
                    ++settlements_active;
                } else {
                    ++settlements_extinct;
                }

                std::int64_t settlement_required = 0;
                std::uint64_t settlement_alive = 0;
                for (const auto& household : state.households) {
                    if (household.settlement_id != settlement.id) {
                        continue;
                    }
                    for (const auto member_id : household.member_ids) {
                        for (const auto& person : state.people) {
                            if (person.id != member_id || !person.alive) {
                                continue;
                            }
                            ++settlement_alive;
                            const auto age = (current_year > person.birth_year)
                                ? static_cast<std::uint64_t>(current_year - person.birth_year)
                                : 0ULL;
                            settlement_required += (age >= 18U) ? 10 : 6;
                            break;
                        }
                    }
                }

                total_settlement_population += settlement_alive;
                largest_settlement = std::max(largest_settlement, settlement_alive);
                if (settlement.active) {
                    smallest_active_settlement = std::min(smallest_active_settlement, settlement_alive);
                    if (settlement_alive < 10U) {
                        ++settlements_under_10;
                    }
                    if (settlement_alive < 25U) {
                        ++settlements_under_25;
                    }
                }

                std::int64_t settlement_capacity = 0;
                for (const auto& region : state.regions) {
                    if (region.id != settlement.region_id) {
                        continue;
                    }
                    settlement_capacity =
                        (region.productive_capacity_per_day * static_cast<std::int64_t>(settlement.access_share_points)) / 10'000 +
                        (region.wild_food_harvest_limit_per_day * static_cast<std::int64_t>(settlement.access_share_points)) / 10'000;
                    break;
                }

                if (settlement.active && settlement_required > 0 && settlement_capacity > 0) {
                    const auto pressure = (settlement_required * 10'000) / settlement_capacity;
                    if (pressure >= 5'000) {
                        ++high_pressure_settlements;
                    }
                }
            }

            const auto avg_settlement_population = settlements_active > 0
                ? (total_settlement_population / settlements_active)
                : 0U;
            if (settlements_active == 0U) {
                smallest_active_settlement = 0U;
            }

            const auto births_period = state.total_births - last_births;
            const auto deaths_period = state.total_deaths - last_deaths;
            const auto population_change = static_cast<std::int64_t>(state.population) - static_cast<std::int64_t>(last_population);
            const auto settlements_founded_period = settlements_active > last_active_settlements
                ? (settlements_active - last_active_settlements)
                : 0U;
            const auto settlements_lost_period = settlements_active < last_active_settlements
                ? (last_active_settlements - settlements_active)
                : 0U;
            const auto births_per_1k = alive_people > 0
                ? ((births_period * 1'000U) / alive_people)
                : 0U;
            const auto deaths_per_1k = alive_people > 0
                ? ((deaths_period * 1'000U) / alive_people)
                : 0U;
            const auto avg_nutrition = alive_people > 0
                ? (total_nutrition / static_cast<std::int64_t>(alive_people))
                : 0;
            const auto avg_health = alive_people > 0
                ? (total_health / static_cast<std::int64_t>(alive_people))
                : 0;

            std::uint64_t fertile_households = 0;
            for (const auto& household : state.households) {
                bool has_reproductive_female = false;
                bool has_reproductive_male = false;
                for (const auto member_id : household.member_ids) {
                    for (const auto& person : state.people) {
                        if (person.id != member_id || !person.alive) {
                            continue;
                        }

                        const auto age = (current_year > person.birth_year)
                            ? static_cast<std::uint64_t>(current_year - person.birth_year)
                            : 0ULL;
                        if (person.sex == universe::BiologicalSex::Female && age >= 18U && age <= 44U) {
                            has_reproductive_female = true;
                        }
                        if (person.sex == universe::BiologicalSex::Male && age >= 18U && age <= 59U) {
                            has_reproductive_male = true;
                        }
                        if (has_reproductive_female && has_reproductive_male) {
                            break;
                        }
                    }
                    if (has_reproductive_female && has_reproductive_male) {
                        break;
                    }
                }

                if (has_reproductive_female && has_reproductive_male) {
                    ++fertile_households;
                }
            }

            last_births = state.total_births;
            last_deaths = state.total_deaths;
            last_population = state.population;
            last_active_settlements = settlements_active;

            std::cout << "year=" << state.year
                      << " population=" << state.population
                      << " population_change=" << population_change
                      << " people_total=" << state.people.size()
                      << " alive_people=" << alive_people
                      << " births_period=" << births_period
                      << " deaths_period=" << deaths_period
                      << " births_per_1k=" << births_per_1k
                      << " deaths_per_1k=" << deaths_per_1k
                      << " reproductive_females=" << reproductive_females
                      << " reproductive_males=" << reproductive_males
                      << " fertile_households=" << fertile_households
                      << " households=" << state.households.size()
                      << " settlements_active=" << settlements_active
                      << " settlements_extinct=" << settlements_extinct
                      << " settlements_founded_period=" << settlements_founded_period
                      << " settlements_lost_period=" << settlements_lost_period
                      << " largest_settlement=" << largest_settlement
                      << " smallest_active_settlement=" << smallest_active_settlement
                      << " settlements_under_10=" << settlements_under_10
                      << " settlements_under_25=" << settlements_under_25
                      << " avg_settlement_population=" << avg_settlement_population
                      << " high_pressure_settlements=" << high_pressure_settlements
                      << " eligible_workers=" << state.eligible_workers
                      << " active_workers=" << state.active_workers
                      << " unallocated_workers=" << state.unallocated_workers
                      << " cultivators=" << state.cultivators
                      << " gatherers=" << state.gatherers
                      << " labour_capacity=" << state.total_labour_capacity
                      << " labour_demand=" << state.total_labour_demand
                      << " labour_allocated=" << state.total_labour_allocated
                      << " labour_shortfall=" << state.labour_shortfall
                      << " cultivation_available=" << state.cultivation_labour_available
                      << " cultivation_allocated=" << state.cultivation_labour_allocated
                      << " gathering_available=" << state.gathering_labour_available
                      << " gathering_allocated=" << state.gathering_labour_allocated
                      << " wood_workers=" << state.wood_gatherers
                      << " stone_workers=" << state.stone_extractors
                      << " craft_workers=" << state.craft_workers
                      << " wood_labour_available=" << state.wood_labour_available
                      << " wood_labour_allocated=" << state.wood_labour_allocated
                      << " stone_labour_available=" << state.stone_labour_available
                      << " stone_labour_allocated=" << state.stone_labour_allocated
                      << " craft_labour_available=" << state.craft_labour_available
                      << " craft_labour_allocated=" << state.craft_labour_allocated
                      << " material_labour_demand=" << state.material_labour_demand
                      << " material_labour_allocated=" << state.material_labour_allocated
                      << " material_labour_shortfall=" << state.material_labour_shortfall
                      << " material_shortfall=" << state.material_production_shortfall
                      << " events_total=" << state.event_log.size()
                      << " food_stored=" << state.total_food_stored
                      << " food_produced=" << state.total_food_produced
                      << " food_consumed=" << state.total_food_consumed
                      << " unmet_food=" << state.total_unmet_food
                      << " wood_stock=" << state.total_wood_stock
                      << " wood_capacity=" << state.total_wood_capacity
                      << " stone_remaining=" << state.total_stone_remaining
                      << " stone_capacity=" << state.total_stone_capacity
                      << " wood_inventory=" << state.total_wood_inventory
                      << " stone_inventory=" << state.total_stone_inventory
                      << " timber_inventory=" << state.total_timber_inventory
                      << " basic_tool_inventory=" << state.total_basic_tool_inventory
                      << " basic_tool_broken=" << state.total_basic_tool_broken
                      << " avg_tool_coverage_per_10k=" << state.average_tool_coverage_per_10k
                      << " wild_stock=" << state.total_wild_food_stock
                      << " wild_capacity=" << state.total_wild_food_capacity
                      << " productive_capacity_per_day=" << state.total_productive_capacity_per_day
                      << " households_in_shortage=" << state.households_in_shortage
                      << " population_under_food_stress=" << state.population_under_food_stress
                      << " nutrition_under_350=" << nutrition_under_350
                      << " nutrition_under_180=" << nutrition_under_180
                      << " health_under_400=" << health_under_400
                      << " health_under_220=" << health_under_220
                      << " avg_nutrition=" << avg_nutrition
                      << " avg_health=" << avg_health
                      << " adults=" << adults
                      << " children=" << children
                      << " age_0_9=" << age_0_9
                      << " age_10_19=" << age_10_19
                      << " age_20_39=" << age_20_39
                      << " age_40_59=" << age_40_59
                      << " age_60_plus=" << age_60_plus
                      << " births_total=" << state.total_births
                      << " deaths_total=" << state.total_deaths
                      << '\n';
        }
    }

    if (dump_events > 0) {
        const auto& state = simulation->state();
        const auto event_count = state.event_log.size();
        const auto dump_count = static_cast<std::size_t>(std::min<std::uint64_t>(dump_events, static_cast<std::uint64_t>(event_count)));
        const auto start_index = event_count - dump_count;

        std::cout << "\nlast_events=" << dump_count << '\n';
        for (std::size_t index = start_index; index < event_count; ++index) {
            const auto& event = state.event_log[index];
            const auto event_year = universe::DemographicCalendarAdapter::demographic_year(event.timestamp);
            std::cout << "event_id=" << event.id
                      << " time_ticks=" << event.timestamp.ticks
                      << " year=" << event_year
                      << " type=";

            if (event.type == universe::EventType::Birth) {
                const auto& payload = std::get<universe::BirthEventPayload>(event.payload);
                std::cout << "Birth"
                          << " child=" << payload.child_id
                          << " mother=" << payload.mother_id;
                if (payload.father_id.has_value()) {
                    std::cout << " father=" << *payload.father_id;
                }
                std::cout << " household=" << payload.household_id;
            } else if (event.type == universe::EventType::Death) {
                const auto& payload = std::get<universe::DeathEventPayload>(event.payload);
                std::cout << "Death"
                          << " person=" << payload.person_id
                          << " household=" << payload.household_id;
            } else if (event.type == universe::EventType::SettlementFounded) {
                const auto& payload = std::get<universe::SettlementFoundedEventPayload>(event.payload);
                std::cout << "SettlementFounded"
                          << " settlement=" << payload.settlement_id
                          << " region=" << payload.region_id;
                if (payload.source_settlement_id.has_value()) {
                    std::cout << " source_settlement=" << *payload.source_settlement_id;
                }
            } else if (event.type == universe::EventType::SettlementExtinct) {
                const auto& payload = std::get<universe::SettlementExtinctEventPayload>(event.payload);
                std::cout << "SettlementExtinct"
                          << " settlement=" << payload.settlement_id
                          << " region=" << payload.region_id;
            } else if (event.type == universe::EventType::OccupationChanged) {
                const auto& payload = std::get<universe::OccupationChangedEventPayload>(event.payload);
                std::cout << "OccupationChanged"
                          << " person=" << payload.person_id
                          << " from=" << static_cast<unsigned>(payload.from)
                          << " to=" << static_cast<unsigned>(payload.to);
            }
            std::cout << '\n';
        }
    }

    if (save_path.has_value()) {
        const auto snapshot_bytes = simulation->save_snapshot();
        if (!write_binary_file(*save_path, snapshot_bytes)) {
            std::cerr << "Failed to write snapshot file: " << *save_path << '\n';
            return 2;
        }

        std::cout << "saved_snapshot=" << *save_path
                  << " bytes=" << snapshot_bytes.size()
                  << " snapshot_hash=" << universe::snapshot_hash64(snapshot_bytes)
                  << '\n';
    }

    return 0;
}
