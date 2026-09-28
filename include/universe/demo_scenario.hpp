#pragma once

#include "universe/simulation.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace universe {

// What a candidate demo run visibly changed over its time window.
struct DemoOutcome {
    std::uint64_t initial_population{};
    std::uint64_t final_population{};
    std::uint64_t buildings_completed{};
    std::uint64_t settlements_founded{};
    std::uint64_t discoveries_made{};
    std::uint64_t institutions_founded{};
};

struct DemoCandidateScore {
    std::uint64_t seed{};
    DemoOutcome outcome{};
    // Eligible only when at least one building completes and the population survives.
    bool eligible{};
    std::int64_t score{};
};

struct DemoScenario {
    std::uint64_t seed{};
    std::uint64_t initial_population{};
    std::uint64_t years{};
};

inline constexpr std::uint64_t kDemoScenarioMaxPopulation = 100'000;
inline constexpr std::uint64_t kDemoScenarioMaxYears = 1'000;

// Counts only change strictly after `window_start`, so the starting world never scores.
[[nodiscard]] DemoOutcome measure_demo_outcome(const WorldState& final_state,
                                               std::uint64_t initial_population,
                                               SimulationTime window_start);

[[nodiscard]] DemoCandidateScore score_demo_outcome(std::uint64_t seed, const DemoOutcome& outcome);

[[nodiscard]] DemoCandidateScore run_demo_candidate(std::uint64_t seed,
                                                    std::uint64_t initial_population,
                                                    std::uint64_t years);

// Eligible candidates first, then higher score, then lower seed.
[[nodiscard]] std::vector<DemoCandidateScore> rank_demo_candidates(std::vector<DemoCandidateScore> candidates);

// Accepts only the flat scenario file format written by format_demo_scenario_json.
[[nodiscard]] std::optional<DemoScenario> parse_demo_scenario(std::string_view json, std::string& error);

[[nodiscard]] std::string format_demo_scenario_json(const DemoScenario& scenario, const DemoCandidateScore& picked_by);

}  // namespace universe
