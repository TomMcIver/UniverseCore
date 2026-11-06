# Performance suite

A repeatable, four-tier performance pipeline and a unified report. Everything is off by
default, so the canonical Podman build and its tests are unchanged.

```powershell
.\scripts\perf\run-perf-pipeline.ps1                  # all tiers -> perf-results\<timestamp>\report.md
.\scripts\perf\run-perf-pipeline.ps1 -Tiers 1,4       # C++ and build only
.\scripts\perf\run-perf-pipeline.ps1 -Tiers 2 -Headless -Seconds 60
```

## What this repo actually is (and what that means for profiling)

| Tier | Share | Reality here | What the suite does |
|---|---|---|---|
| C++ | ~81% | `universe_core` simulation + `universe_ui_bridge`; canonical build runs in Podman | RelWithDebInfo build, Google Benchmark, `perf stat/record`; VTune if present |
| GDScript | ~14% | Godot 4.7 client, **separate process**, talks WebSocket to the bridge. **There is no GDExtension.** | GDScript timing zones, bridge round trips, Godot frame monitors |
| TS/JS | ~3% | PlayCanvas web client + `ws-bridge.mjs` | mitata micro-benchmarks on captured real traffic |
| Build | ~1% | CMake + PowerShell | `cmake --profiling-output`, per-object compile time |

Consequence: there is no in-process C++/GDScript boundary to trace. A GDScript sample and a C++
sample are on different clocks. They are lined up by **simulated tick** (stamped on every
GDScript row) and by **bridge round trips** (which include the C++ command handling).

## Limits you should know before trusting a number

1. **Hardware counters inside Podman on Windows.** Podman runs in a VM that normally does not expose the CPU's
   performance counters. `perf stat -e cycles` then prints `<not supported>`. The pipeline detects this, falls
   back to software events and `cpu-clock` hotspots, and writes `perf-mode.txt = software`. The report then says
   plainly that no L1/L2/L3, branch-miss or retired-instruction data exists. For those counters use
   **VTune on the Windows host** (the pipeline does this automatically if `vtune` is on PATH and a native compiler
   exists) or run on bare-metal Linux.
2. **Tracy for GDScript** does not exist as a stock feature. Godot would need a custom engine build with Tracy
   support (not attempted here). The GDScript tier uses `PerfZones` instead.
3. **Tracy for C++** needs a *native* bridge process, because Tracy's TCP port is not exposed by the Podman bridge.
   See "Tracy" below.
4. **Compile timing** is per translation unit. `-ftime-trace` is Clang-only and the container compiler is GCC.
5. The daily trade tick runs once per food period, so merchant trips are rarely visible at a day boundary
   (see ADR 0003). Profiles of `process_trade_period` are correspondingly small in the default demo world.
6. Profiling zones never touch canonical state. With `UNIVERSECORE_TRACY` undefined every `UC_*` macro is empty.

## Tier 1: C++

### Instrumentation (`include/universe/profiling.hpp`)

```cpp
#include "universe/profiling.hpp"

void Simulation::process_trade_period() {
    UC_ZONE("process_trade_period");           // named scoped zone; compiled out unless Tracy is on
    ...
}

// Once per simulated day, in run_scheduled_systems_until:
UC_FRAME_MARK();                                  // day boundary shows up as a Tracy frame
UC_PLOT("population", state_.population);         // numeric timeline
UC_PLOT("merchant_trips_in_flight", state_.merchant_trips.size());
```

Zones already placed: `Simulation::advance`, `run_scheduled_systems_until`, `process_food_period`,
`process_schedule_period`, `process_trade_period`, `process_demography_period`,
`process_merchant_recruitment`, `step_year`, `snapshot_serialize`, `snapshot_deserialize`.

Add a zone to any function you suspect. Keep zones out of per-person inner loops unless you are
profiling that loop on purpose, because a zone costs tens of nanoseconds when a profiler is connected.

### Isolated loop stress tests (`benchmarks/sim_benchmarks.cpp`, Google Benchmark)

```cpp
void BM_AdvanceOneDay(benchmark::State& state) {
    auto simulation = warmed_world(DemoStartLayout::SingleSettlement, static_cast<std::uint64_t>(state.range(0)));
    for (auto _ : state) {
        simulation.advance(universe::days(1));
        benchmark::DoNotOptimize(simulation.state().population);
    }
    state.counters["population"] = static_cast<double>(simulation.state().population);
}
BENCHMARK(BM_AdvanceOneDay)->Arg(0)->Arg(5)->Arg(20)->Iterations(60)->Unit(benchmark::kMicrosecond);
```

Benchmarks: `AdvanceOneDay` (0/5/20 warm years), `AdvanceOneDayTradeActive`, `RunOneYear`, `SnapshotSave`,
`SnapshotLoad`, `BridgeBuildWorldView`, `BridgeSnapshotJson`, `BridgeDeltaJson`. All run on the committed
demo world so numbers are comparable between runs.

### Manual commands (what the pipeline runs)

```powershell
podman build -t universecore-perf -f Containerfile.perf .            # adds linux perf to the dev image
podman volume create uc-perf-build
# Build + benchmarks + perf, output into perf-results\manual\tier1
$rel = 'perf-results/manual'
podman run --rm -i --cap-add SYS_ADMIN --security-opt seccomp=unconfined `
  -v "${PWD}:/workspace" -v uc-perf-build:/build -w /workspace `
  -e UC_OUT=/workspace/$rel/tier1 -e UC_SEED=39 -e UC_YEARS=20 -e UC_POP=200 `
  universecore-perf bash -s < scripts/perf/container/tier1-cpp.sh
```

(If the checkout has CRLF line endings, use the pipeline instead; it normalises them.)

### VTune on the host (hardware counters, per-function and per-block)

```powershell
cmake -S . -B build\perf-native -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUNIVERSECORE_PROFILE_FLAGS=ON -DUNIVERSECORE_BUILD_TESTS=OFF
cmake --build build\perf-native --config RelWithDebInfo --target universe_sim
vtune -collect hotspots          -result-dir r_hot  -- build\perf-native\universe_sim.exe --seed 39 --years 20 --population 200
vtune -collect uarch-exploration -result-dir r_uarch -- build\perf-native\universe_sim.exe --seed 39 --years 20 --population 200
vtune -report summary  -result-dir r_uarch -format csv      # L1/L2/L3 bound, branch mispredicts, retiring, front/back-end bound
vtune -report hotspots -result-dir r_uarch -group-by function -format csv
vtune -help report                                          # check group-by options (e.g. source line / basic block) for your VTune version
```

### Tracy (C++ timeline, native bridge)

```powershell
cmake -S . -B build\tracy -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUNIVERSECORE_PROFILE_FLAGS=ON `
      -DUNIVERSECORE_ENABLE_TRACY=ON -DUNIVERSECORE_BUILD_TESTS=OFF
cmake --build build\tracy --config RelWithDebInfo --target universe_ui_bridge
$env:UC_TRACY_NATIVE_BRIDGE = (Resolve-Path build\tracy\RelWithDebInfo\universe_ui_bridge.exe)   # path varies by generator
.\scripts\perf\run-perf-pipeline.ps1 -Tiers 2      # tracy-capture records bridge.tracy alongside the Godot session
# Open perf-results\...\tier2\bridge.tracy in the Tracy profiler UI.
```

## Tier 2: GDScript and the bridge

`apps/godot-client/scripts/perf/perf_zones.gd` (`PerfZones`) is inert unless `UNIVERSECORE_PERF_DIR` is set.

```gdscript
var t := PerfZones.begin()                        # returns 0 immediately when profiling is off
_advance_agents(delta)
PerfZones.end("world_view.advance_agents", t)     # records start, duration, server tick, frame

# Cross-bridge timing, already wired in universe_client.gd:
PerfZones.command_sent("set_speed")               # remembers when the command left
PerfZones.message_received("simulation_status", packet.size())   # closes the round trip
PerfZones.set_server_tick(int(message.get("tick", 0)))           # aligns GDScript rows to simulated time
```

Zones in place: `client.decode_message`, `main.apply_world_message`, `panels.route_message`,
`world_view.apply_snapshot`, `world_view.apply_delta`, `world_view.advance_agents`,
`terrain.apply_delta`, `dressing.apply_delta`.

`scripts/tools/perf_run.gd` drives a fixed session: 4x watching, a 20-year jump, 8x with an economy panel open.
Output files: `gdscript_zones.csv`, `gdscript_summary.json`, `godot_frames.csv`, `bridge_rtt.csv`.

## Tier 3: TypeScript / JavaScript

```powershell
cd apps\web-client
npm ci
$env:UC_PERF_FIXTURE = '..\..\perf-results\<run>\tier3\fixture.ndjson'    # real bridge traffic
node bench\protocol.bench.mjs            # table
node bench\protocol.bench.mjs --json > mitata.json
```

## Tier 4: build

```powershell
.\scripts\perf\run-perf-pipeline.ps1 -Tiers 4      # clean configure + build in a throwaway dir
```

Outputs `cmake-configure-trace.json` (open in https://ui.perfetto.dev), `compile-times.csv`, `build-summary.txt`.

## Aggregate and compare

```powershell
.\scripts\perf\aggregate-report.ps1 -OutDir perf-results\<run>     # rebuilds report.md from the raw files
```

Raw artefacts per run (`perf-results/<timestamp>/`): `environment.json`, `status.json`,
`tier1/{gbench.json,perf.data,perf-stat.csv,perf-report.txt,annotate-*.txt,vtune-*}`,
`tier2/{gdscript_zones.csv,godot_frames.csv,bridge_rtt.csv,*.tracy}`,
`tier3/{fixture.ndjson,mitata.json}`, `tier4/{cmake-configure-trace.json,compile-times.csv}`, `report.md`.
Section 6 of the report compares against the newest earlier run that has `tier1/gbench.json`.
