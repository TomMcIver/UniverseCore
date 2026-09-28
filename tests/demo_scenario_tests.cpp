#include "universe/demo_scenario.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

universe::DemoOutcome outcome(const std::uint64_t initial, const std::uint64_t final_population,
                              const std::uint64_t buildings, const std::uint64_t settlements = 0,
                              const std::uint64_t discoveries = 0, const std::uint64_t institutions = 0) {
    return universe::DemoOutcome{
        .initial_population = initial,
        .final_population = final_population,
        .buildings_completed = buildings,
        .settlements_founded = settlements,
        .discoveries_made = discoveries,
        .institutions_founded = institutions,
    };
}

void test_score_rewards_visible_change_with_caps() {
    const auto scored = universe::score_demo_outcome(7U, outcome(200U, 260U, 3U, 1U, 2U, 1U));
    require(scored.eligible, "a run with completed buildings and survivors is eligible");
    require(scored.score == 30 + 30 + 50 + 60 + 20, "score sums buildings, growth, settlements, discoveries, institutions");

    const auto capped = universe::score_demo_outcome(7U, outcome(100U, 1000U, 99U, 9U, 9U, 9U));
    require(capped.score == 200 + 100 + 100 + 90 + 40, "each component is capped");
}

void test_score_penalises_decline_and_rejects_invisible_runs() {
    const auto declining = universe::score_demo_outcome(1U, outcome(200U, 150U, 2U));
    require(declining.score == 20 - 50, "decline is penalised at double weight");

    require(!universe::score_demo_outcome(1U, outcome(200U, 400U, 0U)).eligible, "no completed building is ineligible");
    require(!universe::score_demo_outcome(1U, outcome(200U, 0U, 5U)).eligible, "extinction is ineligible");
    require(!universe::score_demo_outcome(1U, outcome(0U, 0U, 0U)).eligible, "empty start is ineligible");
}

void test_ranking_is_total_and_deterministic() {
    const auto ranked = universe::rank_demo_candidates({
        universe::score_demo_outcome(5U, outcome(200U, 200U, 0U, 3U, 3U, 3U)),
        universe::score_demo_outcome(9U, outcome(200U, 210U, 1U)),
        universe::score_demo_outcome(3U, outcome(200U, 210U, 1U)),
        universe::score_demo_outcome(4U, outcome(200U, 260U, 5U)),
    });
    require(ranked[0].seed == 4U, "highest eligible score ranks first");
    require(ranked[1].seed == 3U && ranked[2].seed == 9U, "ties break on lower seed");
    require(ranked[3].seed == 5U, "ineligible candidates rank last regardless of score");
}

void test_candidate_run_is_deterministic() {
    const auto first = universe::run_demo_candidate(11U, 40U, 2U);
    const auto second = universe::run_demo_candidate(11U, 40U, 2U);
    require(first.score == second.score, "same seed gives same score");
    require(first.outcome.final_population == second.outcome.final_population, "same seed gives same population");
    require(first.outcome.buildings_completed == second.outcome.buildings_completed, "same seed gives same buildings");
    require(first.outcome.initial_population == 40U, "initial population is measured from the world");
}

void test_measure_ignores_state_at_or_before_window_start() {
    universe::WorldState state{};
    state.population = 10U;
    const universe::SimulationTime start{100};
    state.buildings.push_back(universe::Building{.state = universe::ConstructionState::Complete, .completed_at = start});
    state.buildings.push_back(universe::Building{.state = universe::ConstructionState::Complete,
                                                 .completed_at = universe::SimulationTime{101}});
    state.buildings.push_back(universe::Building{.state = universe::ConstructionState::ActiveConstruction});
    state.event_log.push_back(universe::SimulationEvent{.timestamp = start, .type = universe::EventType::SettlementFounded,
                                                        .payload = universe::SettlementFoundedEventPayload{}});
    state.event_log.push_back(universe::SimulationEvent{.timestamp = universe::SimulationTime{200},
                                                        .type = universe::EventType::DiscoveryMade,
                                                        .payload = universe::DiscoveryMadeEventPayload{}});

    const auto measured = universe::measure_demo_outcome(state, 8U, start);
    require(measured.buildings_completed == 1U, "only buildings completed after the window start count");
    require(measured.settlements_founded == 0U, "founding at the window start does not count");
    require(measured.discoveries_made == 1U, "discoveries after the window start count");
    require(measured.final_population == 10U && measured.initial_population == 8U, "populations are carried through");
}

void test_scenario_round_trips_and_rejects_bad_input() {
    const universe::DemoScenario scenario{.seed = 12345U, .initial_population = 200U, .years = 20U};
    const auto json = universe::format_demo_scenario_json(
        scenario, universe::score_demo_outcome(12345U, outcome(200U, 230U, 4U, 1U)));

    std::string error;
    const auto parsed = universe::parse_demo_scenario(json, error);
    require(parsed.has_value(), "formatted scenario parses");
    require(parsed->seed == 12345U && parsed->initial_population == 200U && parsed->years == 20U, "scenario round-trips");

    require(!universe::parse_demo_scenario("[]", error).has_value(), "non-object is rejected");
    require(!universe::parse_demo_scenario(R"({"seed": 1, "years": 20})", error).has_value(), "missing field is rejected");
    require(!universe::parse_demo_scenario(R"({"seed": -1, "initial_population": 200, "years": 20})", error).has_value(),
            "negative seed is rejected");
    require(!universe::parse_demo_scenario(R"({"seed": 1, "seed": 2, "initial_population": 200, "years": 20})", error)
                 .has_value(),
            "duplicate field is rejected");
    require(!universe::parse_demo_scenario(R"({"seed": 1, "initial_population": 0, "years": 20})", error).has_value(),
            "zero population is rejected");
    require(!universe::parse_demo_scenario(R"({"seed": 1, "initial_population": 200, "years": 5000})", error).has_value(),
            "excessive years is rejected");
}

}  // namespace

int main() {
    test_score_rewards_visible_change_with_caps();
    test_score_penalises_decline_and_rejects_invisible_runs();
    test_ranking_is_total_and_deterministic();
    test_candidate_run_is_deterministic();
    test_measure_ignores_state_at_or_before_window_start();
    test_scenario_round_trips_and_rejects_bad_input();
    std::cout << "demo scenario tests passed\n";
    return 0;
}
