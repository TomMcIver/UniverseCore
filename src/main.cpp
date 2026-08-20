#include "universe/simulation.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
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
    std::cout << "Usage: " << executable << " [--seed N] [--years N] [--population N]\n";
}

}  // namespace

int main(int argc, char** argv) {
    universe::SimulationConfig config{};
    std::uint64_t years = 100;

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
            std::cout << "year=" << state.year
                      << " population=" << state.population
                      << " births_total=" << state.total_births
                      << " deaths_total=" << state.total_deaths
                      << '\n';
        }
    }

    return 0;
}
