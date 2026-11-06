# UniverseCore performance report

<!--
Filled by scripts/perf/aggregate-report.ps1 from the files in the run directory.
Double-brace tokens are replaced; a section with no data says NOT COLLECTED and why.
An engineer may complete the "Findings" section by hand. Rule for that section:
no claim without a number from THIS run, and every number cites its source file.
-->

| | |
|---|---|
| Run directory | `{{run_dir}}` |
| Date | {{timestamp}} |
| Commit | `{{commit}}` {{dirty_note}} |
| Machine | {{cpu}} - {{logical_cpu}} logical CPUs, {{ram_gb}} GB, {{os}} |
| Workload | scenario `{{scenario}}`, seed {{seed}}, {{years}} years, {{population}} start population |
| Stack mix (source lines) | C++ ~81% / GDScript ~14% / TS+JS ~3% / PowerShell+CMake ~1% |

## 0. What ran

{{status_table}}

Anything marked `skipped` or `failed` above has **no data** below. It was not estimated.

---

## 1. Timeline view (high level): where wall-clock time goes

This section answers *which language tier owns a slow frame or a slow command*. It uses
timers only. Hardware behaviour is in section 2.

> The Godot client and the C++ simulation are **separate processes** joined by a WebSocket bridge
> (there is no GDExtension). A GDScript zone and a C++ zone are therefore on different clocks.
> They are aligned by simulated tick (`server_tick` on every GDScript row) and by round trips.

### 1.1 GDScript zones (Godot process, `tier2/gdscript_zones.csv`)

{{gdscript_zone_table}}

### 1.2 Bridge round trips: command sent in GDScript until the response arrives (`tier2/bridge_rtt.csv`)

Includes WebSocket + node relay + **C++ command handling + JSON building**.

{{bridge_rtt_table}}

### 1.3 Godot frame budget (`tier2/godot_frames.csv`)

{{frame_table}}

### 1.4 C++ per-phase cost (isolated, Google Benchmark, `tier1/gbench.json`)

| Benchmark | Mean time | CPU | Iterations | Notes |
|---|---|---|---|---|
{{gbench_table}}

C++ daily phases to read in Tracy when a Tracy-enabled build is captured:
`Simulation::advance` > `run_scheduled_systems_until` > `process_food_period`,
`process_schedule_period`, `process_trade_period`, `process_demography_period`
(`process_merchant_recruitment` inside it). Plots: `population`, `merchant_trips_in_flight`.

### 1.5 Timeline attribution (fill by hand from 1.1 - 1.4)

| Slice of one 60 fps frame (16.7 ms) | GDScript | Bridge / C++ | Rendering | Unaccounted |
|---|---|---|---|---|
| ms | | | | |
| % | | | | |

---

## 2. Hardware performance counters (low level)

This section answers *why* a hot function is slow: cache, branches, instruction throughput.
Counters are **only** shown when the machine really exposed them.

**Counter source for this run:** {{counter_mode}}

{{counter_note}}

### 2.1 Whole-run counters (`tier1/perf-stat.csv` or VTune summary)

{{perf_stat_table}}

Derived (computed only when both inputs exist):

| Metric | Value | Reading |
|---|---|---|
| IPC (instructions retired / cycles) | {{ipc}} | < 1.0 on a simulation loop usually means stalls; ~2+ is healthy |
| L1D miss rate | {{l1d_miss_rate}} | |
| LLC (L3) miss rate | {{llc_miss_rate}} | each LLC miss is a DRAM access |
| Branch misprediction rate | {{branch_miss_rate}} | |
| Cache misses per 1000 instructions | {{mpki}} | |

### 2.2 Hot functions (`tier1/perf-top-symbols.txt`, `perf-report.txt`, VTune hotspots)

{{hot_symbols}}

### 2.3 Per-function counters on the same samples (`tier1/perf-group-report.txt`)

{{group_report}}

### 2.4 Per-instruction / assembly-block attribution (`tier1/annotate-*.txt`)

For each of the three hottest functions, `perf annotate` lists the instructions that
received the samples. Copy the 5 hottest instruction blocks per function here:

| Function | Source line / block | Share of function | Instructions in block | Suspected cause (cache miss / branch / dependency chain) | Evidence |
|---|---|---|---|---|---|
| | | | | | |

{{annotate_files}}

---

## 3. TypeScript / JavaScript tier (mitata, `tier3/mitata.txt`)

Input: real bridge traffic, {{fixture_summary}}

{{mitata_block}}

---

## 4. Build automation tier (`tier4/`)

{{build_summary}}

Slowest translation units (`tier4/compile-times.csv`):

{{compile_table}}

`tier4/cmake-configure-trace.json` opens in `chrome://tracing` or https://ui.perfetto.dev.

---

## 5. Findings (engineer fills in; every number cites a file above)

| # | Finding | Evidence (file + value) | Tier | Confidence |
|---|---|---|---|---|
| 1 | | | | |

### Hypotheses to test next (not conclusions)

| Hypothesis | Experiment | Expected if true |
|---|---|---|
| | | |

## 6. Comparison with the previous run

{{comparison}}

Regression gate: a benchmark more than **10%** slower than the previous run, or a GDScript
zone p95 more than **20%** slower, needs a note here before merging.
