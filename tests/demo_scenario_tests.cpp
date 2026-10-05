#include "universe/demo_scenario.hpp"

#include <cstddef>
#include <fstream>
#include <sstream>

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
    universe::WorldState initial{};
    initial.population = 8U;
    initial.time = universe::SimulationTime{100};
    universe::WorldState state{};
    state.population = 10U;
    const universe::SimulationTime start = initial.time;
    state.buildings.push_back(universe::Building{.state = universe::ConstructionState::Complete, .completed_at = start});
    state.buildings.push_back(universe::Building{.state = universe::ConstructionState::Complete,
                                                 .completed_at = universe::SimulationTime{101}});
    state.buildings.push_back(universe::Building{.state = universe::ConstructionState::ActiveConstruction});
    state.event_log.push_back(universe::SimulationEvent{.timestamp = start, .type = universe::EventType::SettlementFounded,
                                                        .payload = universe::SettlementFoundedEventPayload{}});
    state.event_log.push_back(universe::SimulationEvent{.timestamp = universe::SimulationTime{200},
                                                        .type = universe::EventType::DiscoveryMade,
                                                        .payload = universe::DiscoveryMadeEventPayload{}});

    const auto measured = universe::measure_demo_outcome(initial, state);
    require(measured.buildings_completed == 1U, "only buildings completed after the window start count");
    require(measured.settlements_founded == 0U, "founding at the window start does not count");
    require(measured.discoveries_made == 1U, "discoveries after the window start count");
    require(measured.final_population == 10U && measured.initial_population == 8U, "populations are carried through");
}

void test_measure_counts_tier_promotions_and_merges() {
    using universe::SettlementTier;
    universe::WorldState initial{};
    initial.population = 50U;
    initial.settlements.push_back(universe::Settlement{.id = 1U, .tier = SettlementTier::Village});
    initial.settlements.push_back(universe::Settlement{.id = 2U, .tier = SettlementTier::Town});
    initial.settlements.push_back(universe::Settlement{.id = 3U, .tier = SettlementTier::Village});

    universe::WorldState final_state{};
    final_state.population = 60U;
    final_state.settlements.push_back(universe::Settlement{.id = 1U, .tier = SettlementTier::Town});
    final_state.settlements.push_back(universe::Settlement{.id = 2U, .tier = SettlementTier::Village});
    final_state.settlements.push_back(universe::Settlement{.id = 3U, .active = false, .tier = SettlementTier::City});
    final_state.settlements.push_back(universe::Settlement{.id = 4U, .tier = SettlementTier::Town});
    final_state.event_log.push_back(universe::SimulationEvent{.timestamp = universe::SimulationTime{5},
                                                              .type = universe::EventType::SettlementMerged,
                                                              .payload = universe::SettlementMergedEventPayload{}});

    const auto measured = universe::measure_demo_outcome(initial, final_state);
    require(measured.tier_promotions == 2U, "promotions count upgrades of live settlements, new ones from Village");
    require(measured.settlements_merged == 1U, "merge events after the window start count");

    const auto scored = universe::score_demo_outcome(1U, outcome(200U, 200U, 1U));
    auto with_change = outcome(200U, 200U, 1U);
    with_change.tier_promotions = 5U;
    with_change.settlements_merged = 5U;
    require(universe::score_demo_outcome(1U, with_change).score == scored.score + 120 + 80,
            "tier promotions and merges score with caps");
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

std::size_t active_settlement_count(const universe::Simulation& simulation) {
    std::size_t count = 0;
    for (const auto& settlement : simulation.state().settlements) {
        if (settlement.active) {
            ++count;
        }
    }
    return count;
}

void test_single_settlement_layout_starts_one_settlement() {
    universe::DemoScenario scenario{.seed = 3U, .initial_population = 200U, .years = 20U};
    universe::Simulation default_start{universe::make_demo_simulation_config(scenario)};
    require(active_settlement_count(default_start) > 1U, "the default layout starts several settlements");

    scenario.layout = universe::DemoStartLayout::SingleSettlement;
    universe::Simulation single_start{universe::make_demo_simulation_config(scenario)};
    require(active_settlement_count(single_start) == 1U, "the single-settlement layout starts exactly one settlement");
    require(single_start.state().regions.size() == 1U, "the single-settlement layout uses one region");
    require(single_start.state().population == 200U, "the single-settlement layout keeps the requested population");
}

void test_single_settlement_candidate_run_is_deterministic() {
    const auto first = universe::run_demo_candidate(5U, 200U, 3U, universe::DemoStartLayout::SingleSettlement);
    const auto second = universe::run_demo_candidate(5U, 200U, 3U, universe::DemoStartLayout::SingleSettlement);
    require(first.score == second.score && first.outcome.final_population == second.outcome.final_population,
            "a single-settlement candidate run is deterministic");
    const auto default_layout = universe::run_demo_candidate(5U, 200U, 3U);
    require(default_layout.outcome.final_population != first.outcome.final_population ||
                default_layout.score != first.score,
            "the layout changes the world the candidate runs in");
}

void test_scenario_layout_round_trips_and_legacy_files_use_the_default_layout() {
    const universe::DemoScenario single{
        .seed = 9U, .initial_population = 200U, .years = 20U, .layout = universe::DemoStartLayout::SingleSettlement};
    const auto json = universe::format_demo_scenario_json(single, universe::score_demo_outcome(9U, outcome(200U, 230U, 4U)));
    std::string error;
    const auto parsed = universe::parse_demo_scenario(json, error);
    require(parsed.has_value() && parsed->layout == universe::DemoStartLayout::SingleSettlement,
            "the single-settlement layout round-trips");

    const auto legacy = universe::parse_demo_scenario(R"({"seed": 4, "initial_population": 200, "years": 20})", error);
    require(legacy.has_value() && legacy->layout == universe::DemoStartLayout::DefaultLayout,
            "a scenario file without a layout uses the default layout");

    require(!universe::parse_demo_scenario(
                 R"({"seed": 4, "initial_population": 200, "years": 20, "layout": "ring"})", error)
                 .has_value(),
            "an unknown layout is rejected");
    require(!universe::parse_demo_scenario(
                 R"({"seed": 4, "initial_population": 200, "years": 20, "layout": 1})", error)
                 .has_value(),
            "a non-string layout is rejected");
}

// The committed demo scenario must still deliver what ticket #44 requires. If a
// change to the world rules breaks this, rerun universe_demo_seed_search with
// --write-scenario scenarios/demo.json and record the new scores.
void test_committed_demo_scenario_still_delivers_visible_change() {
    std::ifstream file{std::string{UNIVERSE_SOURCE_DIR} + "/scenarios/demo.json", std::ios::binary};
    require(static_cast<bool>(file), "scenarios/demo.json must be committed");
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string error;
    const auto scenario = universe::parse_demo_scenario(contents.str(), error);
    require(scenario.has_value(), "scenarios/demo.json must parse");
    require(scenario->layout == universe::DemoStartLayout::SingleSettlement, "the demo starts as a single settlement");

    const auto result = universe::run_demo_candidate(scenario->seed, scenario->initial_population, scenario->years, scenario->layout);
    require(result.eligible, "the demo scenario must complete a building and keep its population");
    require(result.outcome.final_population >= result.outcome.initial_population,
            "the demo village must not shrink over the demo window");
    require(result.outcome.discoveries_made > 0U || result.outcome.settlements_founded > 0U,
            "the demo scenario must show a new settlement or a discovery");
}

}  // namespace

int main() {
    test_score_rewards_visible_change_with_caps();
    test_score_penalises_decline_and_rejects_invisible_runs();
    test_ranking_is_total_and_deterministic();
    test_candidate_run_is_deterministic();
    test_measure_ignores_state_at_or_before_window_start();
    test_measure_counts_tier_promotions_and_merges();
    test_scenario_round_trips_and_rejects_bad_input();
    test_single_settlement_layout_starts_one_settlement();
    test_single_settlement_candidate_run_is_deterministic();
    test_scenario_layout_round_trips_and_legacy_files_use_the_default_layout();
    test_committed_demo_scenario_still_delivers_visible_change();
    std::cout << "demo scenario tests passed\n";
    return 0;
}
