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
    // Settlements whose tier ends above where it began (Village if founded in the window).
    std::uint64_t tier_promotions{};
    std::uint64_t settlements_merged{};
};

struct DemoCandidateScore {
    std::uint64_t seed{};
    DemoOutcome outcome{};
    // Eligible only when at least one building completes and the population survives.
    bool eligible{};
    std::int64_t score{};
};

// How the demo world starts. ADR 0002: the demo begins as one settlement that
// grows and splits; the default layout is the simulation's own multi-settlement
// start and is what scenario files without a layout mean.
enum class DemoStartLayout : std::uint8_t {
    DefaultLayout = 0,
    SingleSettlement = 1,
};

struct DemoScenario {
    std::uint64_t seed{};
    std::uint64_t initial_population{};
    std::uint64_t years{};
    DemoStartLayout layout{DemoStartLayout::DefaultLayout};
};

inline constexpr std::uint64_t kDemoScenarioMaxPopulation = 100'000;
inline constexpr std::uint64_t kDemoScenarioMaxYears = 1'000;
// One region holding as much land as the default four regions together, so a
// single-settlement start is not starved of land.
inline constexpr std::int64_t kSingleSettlementRegionLandUnits = 480;

// The one place a scenario becomes a simulation config, shared by the seed
// search, the bridge and the tests so they all run the same world.
[[nodiscard]] SimulationConfig make_demo_simulation_config(const DemoScenario& scenario);

// Counts only change after `initial_state`, so the starting world never scores.
[[nodiscard]] DemoOutcome measure_demo_outcome(const WorldState& initial_state, const WorldState& final_state);

[[nodiscard]] DemoCandidateScore score_demo_outcome(std::uint64_t seed, const DemoOutcome& outcome);

[[nodiscard]] DemoCandidateScore run_demo_candidate(std::uint64_t seed,
                                                    std::uint64_t initial_population,
                                                    std::uint64_t years,
                                                    DemoStartLayout layout = DemoStartLayout::DefaultLayout);

// Eligible candidates first, then higher score, then lower seed.
[[nodiscard]] std::vector<DemoCandidateScore> rank_demo_candidates(std::vector<DemoCandidateScore> candidates);

// Accepts only the flat scenario file format written by format_demo_scenario_json.
[[nodiscard]] std::optional<DemoScenario> parse_demo_scenario(std::string_view json, std::string& error);

[[nodiscard]] std::string format_demo_scenario_json(const DemoScenario& scenario, const DemoCandidateScore& picked_by);

}  // namespace universe
