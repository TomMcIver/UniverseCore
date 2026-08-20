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
              << " [--seed N] [--years N] [--population N] [--dump-events N] [--save PATH] [--load PATH]\n";
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

    for (std::uint64_t i = 0; i < years; ++i) {
        simulation->step_year();
        const auto& state = simulation->state();
        if (state.year <= 10 || state.year % 10 == 0 || state.year == years) {
            std::uint64_t adults = 0;
            std::uint64_t children = 0;
            std::uint64_t alive_people = 0;

            for (const auto& person : state.people) {
                if (!person.alive) {
                    continue;
                }

                ++alive_people;
                const auto max_i64_as_u64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
                const auto current_year = (state.year > max_i64_as_u64)
                    ? std::numeric_limits<std::int64_t>::max()
                    : static_cast<std::int64_t>(state.year);
                const auto age = (current_year > person.birth_year)
                    ? static_cast<std::uint64_t>(current_year - person.birth_year)
                    : 0ULL;
                if (age >= 18U) {
                    ++adults;
                } else {
                    ++children;
                }
            }

            std::cout << "year=" << state.year
                      << " population=" << state.population
                      << " people_total=" << state.people.size()
                      << " alive_people=" << alive_people
                      << " households=" << state.households.size()
                      << " events_total=" << state.event_log.size()
                      << " adults=" << adults
                      << " children=" << children
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
                      << " type=" << (event.type == universe::EventType::Birth ? "Birth" : "Death")
                      << " subject=" << event.subject_person_id;
            if (event.mother_id.has_value()) {
                std::cout << " mother=" << *event.mother_id;
            }
            if (event.father_id.has_value()) {
                std::cout << " father=" << *event.father_id;
            }
            if (event.household_id.has_value()) {
                std::cout << " household=" << *event.household_id;
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
