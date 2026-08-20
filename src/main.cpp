#include "universe/simulation.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <algorithm>
#include <string_view>

namespace {

template <typename T>
bool parse_number(const std::string_view text, T& value) {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable << " [--seed N] [--years N] [--population N] [--dump-events N]\n";
}

}  // namespace

int main(int argc, char** argv) {
    universe::SimulationConfig config{};
    std::uint64_t years = 100;
    std::uint64_t dump_events = 0;

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
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            print_usage(argv[0]);
            return 2;
        }
    }

    universe::Simulation simulation{config};

    std::cout << "UniverseCore prototype\n"
              << "seed=" << config.seed << '\n'
              << "year=0 population=" << config.initial_population << "\n\n";

    for (std::uint64_t i = 0; i < years; ++i) {
        simulation.step_year();
        const auto& state = simulation.state();
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
        const auto& state = simulation.state();
        const auto event_count = state.event_log.size();
        const auto dump_count = static_cast<std::size_t>(std::min<std::uint64_t>(dump_events, static_cast<std::uint64_t>(event_count)));
        const auto start_index = event_count - dump_count;

        std::cout << "\nlast_events=" << dump_count << '\n';
        for (std::size_t index = start_index; index < event_count; ++index) {
            const auto& event = state.event_log[index];
            std::cout << "event_id=" << event.id
                      << " year=" << event.year
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

    return 0;
}
