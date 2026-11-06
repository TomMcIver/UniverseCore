// Isolated micro-benchmarks for the hot paths of the simulation and the C++ half
// of the client bridge. Built only with -DUNIVERSECORE_BUILD_BENCHMARKS=ON.
//
// These measure the same code the canonical build runs, on the committed demo
// world (seed 39) so results are comparable between runs and machines. A
// benchmark never feeds back into the simulation: worlds are built from fixed
// seeds and only observed.
//
// Run:   universe_benchmarks --benchmark_out=benchmarks.json --benchmark_out_format=json
// Filter: universe_benchmarks --benchmark_filter=Snapshot

#include "universe/demo_scenario.hpp"
#include "universe/simulation.hpp"
#include "universe/time.hpp"
#include "universe/ui/simulation_controller.hpp"
#include "universe/ui/web_protocol.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <span>
#include <vector>

namespace {

using universe::DemoScenario;
using universe::DemoStartLayout;
using universe::Simulation;

constexpr std::uint64_t kDemoSeed = 39;
constexpr std::uint64_t kDemoPopulation = 200;
constexpr std::uint32_t kProtocolVersion = 2U;

universe::SimulationConfig demo_config(const DemoStartLayout layout) {
    return universe::make_demo_simulation_config(DemoScenario{
        .seed = kDemoSeed,
        .initial_population = kDemoPopulation,
        .years = 20,
        .layout = layout,
    });
}

// A world that has already lived `warm_years`, so the benchmark measures a
// settled economy (buildings, trips, price memory) and not the empty start.
Simulation warmed_world(const DemoStartLayout layout, const std::uint64_t warm_years) {
    Simulation simulation{demo_config(layout)};
    simulation.run_years(warm_years);
    return simulation;
}

void report_world(benchmark::State& state, const Simulation& simulation) {
    state.counters["population"] = static_cast<double>(simulation.state().population);
    state.counters["settlements"] = static_cast<double>(simulation.state().settlements.size());
    state.counters["trips_in_flight"] = static_cast<double>(simulation.state().merchant_trips.size());
}

// Advance one simulated day on a world warmed to Arg(0) years. The world drifts a
// little across iterations, so a fixed small iteration count keeps runs comparable.
void BM_AdvanceOneDay(benchmark::State& state) {
    auto simulation = warmed_world(DemoStartLayout::SingleSettlement, static_cast<std::uint64_t>(state.range(0)));
    for (auto _ : state) {
        simulation.advance(universe::days(1));
        benchmark::DoNotOptimize(simulation.state().population);
    }
    report_world(state, simulation);
}
BENCHMARK(BM_AdvanceOneDay)->Arg(0)->Arg(5)->Arg(20)->Iterations(60)->Unit(benchmark::kMicrosecond);

// The default (multi-settlement) layout is the one where merchants actually trade.
void BM_AdvanceOneDayTradeActive(benchmark::State& state) {
    auto simulation = warmed_world(DemoStartLayout::DefaultLayout, 20);
    for (auto _ : state) {
        simulation.advance(universe::days(1));
        benchmark::DoNotOptimize(simulation.state().merchant_trips.size());
    }
    report_world(state, simulation);
}
BENCHMARK(BM_AdvanceOneDayTradeActive)->Iterations(60)->Unit(benchmark::kMicrosecond);

// One full simulated year from a fresh warmed world each iteration. World
// construction is excluded from the timing.
void BM_RunOneYear(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        auto simulation = warmed_world(DemoStartLayout::SingleSettlement, 2);
        state.ResumeTiming();
        simulation.run_years(1);
        benchmark::DoNotOptimize(simulation.state().population);
    }
}
BENCHMARK(BM_RunOneYear)->Iterations(5)->Unit(benchmark::kMillisecond);

void BM_SnapshotSave(benchmark::State& state) {
    const auto simulation = warmed_world(DemoStartLayout::DefaultLayout, 10);
    std::size_t bytes = 0;
    for (auto _ : state) {
        const auto data = simulation.save_snapshot();
        bytes = data.size();
        benchmark::DoNotOptimize(data.data());
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(bytes));
    state.counters["snapshot_bytes"] = static_cast<double>(bytes);
}
BENCHMARK(BM_SnapshotSave)->Unit(benchmark::kMicrosecond);

void BM_SnapshotLoad(benchmark::State& state) {
    const auto source = warmed_world(DemoStartLayout::DefaultLayout, 10);
    const auto data = source.save_snapshot();
    for (auto _ : state) {
        Simulation destination{demo_config(DemoStartLayout::DefaultLayout)};
        const auto result = Simulation::load_snapshot(std::span<const std::byte>{data}, destination);
        benchmark::DoNotOptimize(result.ok);
        if (!result.ok) {
            state.SkipWithError("snapshot failed to load");
            break;
        }
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(data.size()));
}
BENCHMARK(BM_SnapshotLoad)->Unit(benchmark::kMicrosecond);

// The C++ half of the client bridge: turning canonical state into the
// presentation view and the JSON the Godot and web clients receive.
void BM_BridgeBuildWorldView(benchmark::State& state) {
    universe::ui::SimulationController controller{demo_config(DemoStartLayout::DefaultLayout)};
    controller.advance_years(10);
    for (auto _ : state) {
        const auto view = universe::ui::build_world_snapshot(controller);
        benchmark::DoNotOptimize(view);
    }
}
BENCHMARK(BM_BridgeBuildWorldView)->Unit(benchmark::kMicrosecond);

void BM_BridgeSnapshotJson(benchmark::State& state) {
    universe::ui::SimulationController controller{demo_config(DemoStartLayout::DefaultLayout)};
    controller.advance_years(10);
    const auto view = universe::ui::build_world_snapshot(controller);
    std::size_t bytes = 0;
    for (auto _ : state) {
        const auto json = universe::ui::make_snapshot_message(kProtocolVersion, view, true);
        bytes = json.size();
        benchmark::DoNotOptimize(json.data());
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(bytes));
    state.counters["json_bytes"] = static_cast<double>(bytes);
}
BENCHMARK(BM_BridgeSnapshotJson)->Unit(benchmark::kMicrosecond);

void BM_BridgeDeltaJson(benchmark::State& state) {
    universe::ui::SimulationController controller{demo_config(DemoStartLayout::DefaultLayout)};
    controller.advance_years(10);
    const auto previous = universe::ui::build_world_snapshot(controller);
    controller.advance_days(1);
    const auto current = universe::ui::build_world_snapshot(controller);
    for (auto _ : state) {
        const auto delta = universe::ui::build_world_delta(current, previous);
        const auto json = universe::ui::make_delta_message(kProtocolVersion, delta);
        benchmark::DoNotOptimize(json.data());
    }
}
BENCHMARK(BM_BridgeDeltaJson)->Unit(benchmark::kMicrosecond);

}  // namespace

BENCHMARK_MAIN();
