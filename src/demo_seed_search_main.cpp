#include "universe/demo_scenario.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t kMaxSeedCount = 10'000;

struct SearchOptions {
    std::uint64_t seed_start{1};
    std::uint64_t seed_count{32};
    std::uint64_t population{200};
    std::uint64_t years{20};
    std::uint64_t top{10};
    // ADR 0002: the demo starts as one settlement, so that is what is searched.
    universe::DemoStartLayout layout{universe::DemoStartLayout::SingleSettlement};
    std::optional<std::string> write_scenario_path{};
};

bool parse_number(const std::string_view text, std::uint64_t& value) {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable
              << " [--seed-start N] [--seed-count N] [--population N] [--years N] [--top N]"
              << " [--layout single|default] [--write-scenario PATH]\n";
}

std::optional<SearchOptions> parse_options(const int argc, char** argv) {
    SearchOptions options{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << arg << '\n';
            return std::nullopt;
        }
        const std::string_view value{argv[++i]};
        if (arg == "--layout") {
            if (value == "single") {
                options.layout = universe::DemoStartLayout::SingleSettlement;
            } else if (value == "default") {
                options.layout = universe::DemoStartLayout::DefaultLayout;
            } else {
                std::cerr << "Invalid layout " << value << " (expected single or default)\n";
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--write-scenario") {
            options.write_scenario_path = std::string{value};
            continue;
        }

        std::uint64_t* target = nullptr;
        if (arg == "--seed-start") {
            target = &options.seed_start;
        } else if (arg == "--seed-count") {
            target = &options.seed_count;
        } else if (arg == "--population") {
            target = &options.population;
        } else if (arg == "--years") {
            target = &options.years;
        } else if (arg == "--top") {
            target = &options.top;
        }
        if (target == nullptr || !parse_number(value, *target)) {
            std::cerr << "Invalid argument " << arg << '\n';
            return std::nullopt;
        }
    }

    if (options.seed_count == 0U || options.seed_count > kMaxSeedCount ||
        options.population == 0U || options.population > universe::kDemoScenarioMaxPopulation ||
        options.years == 0U || options.years > universe::kDemoScenarioMaxYears) {
        std::cerr << "Argument out of range\n";
        return std::nullopt;
    }
    return options;
}

void print_table(const std::vector<universe::DemoCandidateScore>& ranked, const std::uint64_t top) {
    std::cout << std::left << std::setw(6) << "rank" << std::setw(10) << "seed" << std::setw(9) << "eligible"
              << std::setw(7) << "score" << std::setw(10) << "pop" << std::setw(11) << "buildings"
              << std::setw(13) << "settlements" << std::setw(13) << "discoveries" << std::setw(14) << "institutions"
              << std::setw(7) << "tiers" << "merges\n";
    for (std::size_t i = 0; i < ranked.size() && i < top; ++i) {
        const auto& candidate = ranked[i];
        const auto& outcome = candidate.outcome;
        std::cout << std::left << std::setw(6) << (i + 1) << std::setw(10) << candidate.seed
                  << std::setw(9) << (candidate.eligible ? "yes" : "no") << std::setw(7) << candidate.score
                  << std::setw(10) << (std::to_string(outcome.initial_population) + ">" + std::to_string(outcome.final_population))
                  << std::setw(11) << outcome.buildings_completed << std::setw(13) << outcome.settlements_founded
                  << std::setw(13) << outcome.discoveries_made << std::setw(14) << outcome.institutions_founded
                  << std::setw(7) << outcome.tier_promotions << outcome.settlements_merged << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        print_usage(argv[0]);
        return 0;
    }
    const auto options = parse_options(argc, argv);
    if (!options.has_value()) {
        print_usage(argv[0]);
        return 2;
    }

    std::vector<universe::DemoCandidateScore> candidates;
    candidates.reserve(options->seed_count);
    for (std::uint64_t offset = 0; offset < options->seed_count; ++offset) {
        const auto seed = options->seed_start + offset;
        candidates.push_back(universe::run_demo_candidate(seed, options->population, options->years, options->layout));
        std::cerr << "scored seed " << seed << '\n';
    }

    const auto ranked = universe::rank_demo_candidates(std::move(candidates));
    print_table(ranked, options->top);

    const auto& best = ranked.front();
    if (!best.eligible) {
        std::cerr << "No eligible seed: no candidate completed a building with a surviving population\n";
        return 1;
    }

    if (options->write_scenario_path.has_value()) {
        const universe::DemoScenario scenario{
            .seed = best.seed,
            .initial_population = options->population,
            .years = options->years,
            .layout = options->layout,
        };
        std::ofstream file{*options->write_scenario_path, std::ios::trunc};
        file << universe::format_demo_scenario_json(scenario, best);
        if (!file) {
            std::cerr << "Failed to write " << *options->write_scenario_path << '\n';
            return 1;
        }
        std::cout << "wrote " << *options->write_scenario_path << " (seed " << best.seed << ")\n";
    }
    return 0;
}
