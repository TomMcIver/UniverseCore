#include "universe/simulation.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t kBenchmarkYears = 100;
constexpr std::int64_t kViableRegionArea = 1'000;   // long-horizon viable for detailed runs
constexpr std::int64_t kAggregateRegionArea = 10'000;

struct BenchmarkResult {
    std::string_view scenario;
    std::uint64_t years{};
    std::uint64_t start_population{};
    std::uint64_t final_population{};
    std::uint64_t final_detailed_population{};
    std::uint64_t final_aggregate_population{};
    std::int64_t total_food_produced{};
    std::uint64_t person_daily_updates{};
    std::uint64_t aggregate_settlement_updates{};
    std::int64_t elapsed_milliseconds{};
};

BenchmarkResult run_detailed_benchmark() {
    universe::Simulation simulation{universe::SimulationConfig{
        .seed = 42,
        .initial_population = 200,
        .initial_region_count = 1,
        .base_region_area_units = kViableRegionArea,
    }};

    const auto start_population = simulation.state().population;
    simulation.reset_lod_instrumentation();
    const auto started_at = std::chrono::steady_clock::now();
    simulation.run_years(kBenchmarkYears);
    const auto finished_at = std::chrono::steady_clock::now();

    const auto& state = simulation.state();
    return BenchmarkResult{
        .scenario = "detailed_200",
        .years = kBenchmarkYears,
        .start_population = start_population,
        .final_population = state.population,
        .final_detailed_population = simulation.detailed_population(),
        .final_aggregate_population = simulation.aggregate_population(),
        .total_food_produced = state.total_food_produced,
        .person_daily_updates = simulation.person_daily_update_count(),
        .aggregate_settlement_updates = simulation.aggregate_settlement_update_count(),
        .elapsed_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(finished_at - started_at).count(),
    };
}

BenchmarkResult run_aggregate_benchmark(
    const std::uint64_t aggregate_population,
    const std::uint64_t region_count,
    const std::string_view scenario_name) {
    universe::Simulation simulation{universe::SimulationConfig{
        .seed = 42,
        .initial_population = 0,
        .initial_region_count = region_count,
        .base_region_area_units = kAggregateRegionArea,
    }};

    const auto per_region = aggregate_population / region_count;
    for (const auto& region : simulation.state().regions) {
        simulation.add_aggregate_population(region.id, 0U, per_region);
    }

    const auto start_population = simulation.state().population;
    simulation.reset_lod_instrumentation();
    const auto started_at = std::chrono::steady_clock::now();
    simulation.run_years(kBenchmarkYears);
    const auto finished_at = std::chrono::steady_clock::now();

    const auto& state = simulation.state();
    return BenchmarkResult{
        .scenario = scenario_name,
        .years = kBenchmarkYears,
        .start_population = start_population,
        .final_population = state.population,
        .final_detailed_population = simulation.detailed_population(),
        .final_aggregate_population = simulation.aggregate_population(),
        .total_food_produced = state.total_food_produced,
        .person_daily_updates = simulation.person_daily_update_count(),
        .aggregate_settlement_updates = simulation.aggregate_settlement_update_count(),
        .elapsed_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(finished_at - started_at).count(),
    };
}

void print_result(const BenchmarkResult& result) {
    std::cout << "M4_LOD_BENCHMARK"
              << "|scenario=" << result.scenario
              << "|years=" << result.years
              << "|elapsed_ms=" << result.elapsed_milliseconds
              << "|start_population=" << result.start_population
              << "|population=" << result.final_population
              << "|detailed_population=" << result.final_detailed_population
              << "|aggregate_population=" << result.final_aggregate_population
              << "|food_produced=" << result.total_food_produced
              << "|person_daily_updates=" << result.person_daily_updates
              << "|aggregate_settlement_updates=" << result.aggregate_settlement_updates
              << '\n';
}

bool check(const bool condition, const std::string_view label) {
    std::cout << "M4_LOD_DEMO|check=" << label << "|result=" << (condition ? "pass" : "FAIL") << '\n';
    return condition;
}

std::vector<universe::PersonId> materialized_person_ids(const universe::Simulation& simulation) {
    std::vector<universe::PersonId> ids;
    for (const auto& person : simulation.state().people) {
        if (person.alive && person.source_cohort_id.has_value()) {
            ids.push_back(person.id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// Full mixed-LOD lifecycle: detailed + 10k aggregate + 100k aggregate advancing
// together, then promote / demote / save / load / return without rerolling.
bool run_mixed_lod_lifecycle_demo() {
    universe::Simulation simulation{universe::SimulationConfig{
        .seed = 42,
        .initial_population = 200,
        .initial_region_count = 64,
        .base_region_area_units = kAggregateRegionArea,
    }};

    // The detailed starting population keeps its own regions; aggregate
    // population goes to regions that still have unclaimed access share.
    std::vector<universe::RegionId> aggregate_regions;
    for (const auto& region : simulation.state().regions) {
        std::uint32_t claimed = 0U;
        for (const auto& settlement : simulation.state().settlements) {
            if (settlement.active && settlement.region_id == region.id) {
                claimed += settlement.access_share_points;
            }
        }
        if (claimed == 0U) {
            aggregate_regions.push_back(region.id);
        }
    }

    // Split the free regions: the first group carries 10,000, the rest 100,000.
    if (aggregate_regions.size() < 4U) {
        std::cout << "M4_LOD_DEMO|error=insufficient_free_regions|count="
                  << aggregate_regions.size() << '\n';
        return false;
    }
    const auto small_group = aggregate_regions.size() / 4U;
    const auto large_group = aggregate_regions.size() - small_group;
    for (std::size_t i = 0; i < aggregate_regions.size(); ++i) {
        const auto population = (i < small_group)
            ? (10'000U / small_group)
            : (100'000U / large_group);
        simulation.add_aggregate_population(aggregate_regions[i], 0U, population);
    }

    const auto start_population = simulation.state().population;
    const auto start_detailed = simulation.detailed_population();
    const auto start_aggregate = simulation.aggregate_population();
    std::cout << "M4_LOD_DEMO|phase=start"
              << "|population=" << start_population
              << "|detailed=" << start_detailed
              << "|aggregate=" << start_aggregate << '\n';

    bool ok = true;
    ok &= check(start_detailed >= 200U, "detailed_cohabits_with_aggregate");
    // ~10,000 + ~100,000; exact totals vary slightly with per-region rounding.
    ok &= check(start_aggregate >= 109'000U, "aggregate_population_represented");

    simulation.reset_lod_instrumentation();
    const auto started_at = std::chrono::steady_clock::now();
    simulation.run_years(25);
    const auto finished_at = std::chrono::steady_clock::now();
    const auto mixed_elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(finished_at - started_at).count();

    std::cout << "M4_LOD_DEMO|phase=advanced_25y"
              << "|elapsed_ms=" << mixed_elapsed_ms
              << "|population=" << simulation.state().population
              << "|detailed=" << simulation.detailed_population()
              << "|aggregate=" << simulation.aggregate_population()
              << "|births=" << simulation.state().total_births
              << "|deaths=" << simulation.state().total_deaths << '\n';

    ok &= check(simulation.state().population != start_population, "mixed_world_advances");
    ok &= check(simulation.state().total_births > 0U, "births_occur");
    ok &= check(simulation.state().total_deaths > 0U, "deaths_occur");
    ok &= check(simulation.detailed_population() > 0U, "detailed_region_survives");

    // Promote one aggregate region to detailed representation.
    const auto promoted_region = aggregate_regions.front();
    const auto population_before_promote = simulation.state().population;
    const auto food_before_promote = simulation.state().total_food_stored;
    const auto events_before_promote = simulation.state().event_log.size();
    const auto records_before_promote = simulation.knowledge_records().size();
    const auto institutions_before_promote = simulation.institutions().size();

    simulation.promote_region_to_detailed(promoted_region);
    const auto promoted_ids = materialized_person_ids(simulation);

    ok &= check(simulation.state().population == population_before_promote, "promotion_conserves_population");
    ok &= check(simulation.state().total_food_stored == food_before_promote, "promotion_conserves_food");
    ok &= check(!promoted_ids.empty(), "promotion_realizes_individuals");
    std::cout << "M4_LOD_DEMO|phase=promoted"
              << "|region=" << promoted_region
              << "|realized_people=" << promoted_ids.size()
              << "|detailed=" << simulation.detailed_population() << '\n';

    // Demote it again.
    simulation.demote_region_to_settlement_aggregate(promoted_region);
    const auto population_after_demote = simulation.state().population;
    ok &= check(population_after_demote == population_before_promote, "demotion_conserves_population");
    std::cout << "M4_LOD_DEMO|phase=demoted"
              << "|population=" << population_after_demote
              << "|aggregate=" << simulation.aggregate_population() << '\n';

    // Save / load round trip.
    const auto snapshot = simulation.save_snapshot();
    universe::Simulation restored{universe::SimulationConfig{}};
    const auto load_result = universe::Simulation::load_snapshot(snapshot, restored);
    ok &= check(load_result.ok, "snapshot_round_trip_loads");
    if (!load_result.ok) {
        std::cout << "M4_LOD_DEMO|snapshot_error=" << load_result.message << '\n';
        return false;
    }
    ok &= check(restored.state().population == population_after_demote, "snapshot_conserves_population");
    ok &= check(restored.state().event_log.size() >= events_before_promote, "snapshot_retains_event_history");
    ok &= check(restored.knowledge_records().size() >= records_before_promote, "snapshot_retains_records");
    ok &= check(restored.institutions().size() >= institutions_before_promote, "snapshot_retains_institutions");

    // Return later: advance the restored world, then promote the same region and
    // confirm the previously realized identities come back rather than rerolling.
    restored.run_years(10);
    restored.promote_region_to_detailed(promoted_region);
    const auto reactivated_ids = materialized_person_ids(restored);

    std::vector<universe::PersonId> retained;
    std::set_intersection(
        promoted_ids.begin(), promoted_ids.end(),
        reactivated_ids.begin(), reactivated_ids.end(),
        std::back_inserter(retained));

    ok &= check(!retained.empty(), "returned_region_reactivates_prior_identities");
    std::cout << "M4_LOD_DEMO|phase=returned"
              << "|previously_realized=" << promoted_ids.size()
              << "|reactivated=" << reactivated_ids.size()
              << "|retained_identities=" << retained.size() << '\n';

    // Determinism: the same segmented path must reproduce byte-identical state.
    universe::Simulation replay{universe::SimulationConfig{}};
    const auto replay_result = universe::Simulation::load_snapshot(snapshot, replay);
    if (replay_result.ok) {
        replay.run_years(10);
        replay.promote_region_to_detailed(promoted_region);
        ok &= check(replay.save_snapshot() == restored.save_snapshot(), "return_path_is_deterministic");
    } else {
        ok &= check(false, "replay_snapshot_loads");
    }

    return ok;
}

}  // namespace

int main() {
    const auto detailed_result = run_detailed_benchmark();
    const auto aggregate_10k_result = run_aggregate_benchmark(10'000U, 4U, "aggregate_10000");
    const auto aggregate_100k_result = run_aggregate_benchmark(100'000U, 10U, "aggregate_100000");

    print_result(detailed_result);
    print_result(aggregate_10k_result);
    print_result(aggregate_100k_result);

    bool ok = true;

    // Aggregate execution must never touch the per-person daily path.
    ok &= check(aggregate_10k_result.person_daily_updates == 0U, "aggregate_10k_runs_no_person_loops");
    ok &= check(aggregate_100k_result.person_daily_updates == 0U, "aggregate_100k_runs_no_person_loops");
    ok &= check(detailed_result.person_daily_updates > 0U, "detailed_runs_person_loops");

    // Aggregate representation must be materially cheaper per represented person.
    const auto detailed_cost_per_person = (detailed_result.elapsed_milliseconds * 1'000'000) /
        std::max<std::int64_t>(1, static_cast<std::int64_t>(detailed_result.start_population));
    const auto aggregate_cost_per_person = (aggregate_100k_result.elapsed_milliseconds * 1'000'000) /
        std::max<std::int64_t>(1, static_cast<std::int64_t>(aggregate_100k_result.start_population));
    std::cout << "M4_LOD_BENCHMARK|scenario=cost_per_represented_person"
              << "|detailed_ns=" << detailed_cost_per_person
              << "|aggregate_ns=" << aggregate_cost_per_person << '\n';
    ok &= check(aggregate_cost_per_person * 10 < detailed_cost_per_person, "aggregate_materially_cheaper");

    // Populations must actually advance rather than sit at their injected value.
    ok &= check(aggregate_10k_result.final_population != aggregate_10k_result.start_population,
                "aggregate_10k_advances");
    ok &= check(aggregate_100k_result.final_population != aggregate_100k_result.start_population,
                "aggregate_100k_advances");
    ok &= check(aggregate_10k_result.final_aggregate_population == aggregate_10k_result.final_population,
                "aggregate_10k_stays_cohort_represented");
    ok &= check(aggregate_100k_result.final_aggregate_population == aggregate_100k_result.final_population,
                "aggregate_100k_stays_cohort_represented");

    ok &= run_mixed_lod_lifecycle_demo();

    if (!ok) {
        std::cerr << "Milestone 4 LOD demonstration failed\n";
        return 1;
    }

    std::cout << "M4_LOD_DEMO|result=all_checks_passed\n";
    return 0;
}
