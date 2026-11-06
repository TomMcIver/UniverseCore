#!/usr/bin/env bash
# Tier 1 (C++), runs inside the universecore-perf container.
# Inputs (env): UC_OUT (output dir), UC_SEED, UC_YEARS, UC_POP, UC_PERF_FREQ (default 999)
#
# 1. Build with symbols + frame pointers + optimisation (RelWithDebInfo).
# 2. Google Benchmark micro-benchmarks (3 repetitions, aggregates only).
# 3. perf stat / perf record on a headless simulation run.
#    Hardware counters are used only if the VM exposes them; otherwise the run falls
#    back to software events (task-clock, cpu-clock) and says so in perf-mode.txt.
set -euo pipefail

: "${UC_OUT:?}" "${UC_SEED:=39}" "${UC_YEARS:=20}" "${UC_POP:=200}" "${UC_PERF_FREQ:=999}"
mkdir -p "$UC_OUT"

echo "== build (RelWithDebInfo, frame pointers, benchmarks)"
cmake -S /workspace -B /build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DUNIVERSECORE_PROFILE_FLAGS=ON \
    -DUNIVERSECORE_BUILD_BENCHMARKS=ON \
    -DUNIVERSECORE_BUILD_TESTS=OFF
cmake --build /build --target universe_sim universe_benchmarks

echo "== google benchmark"
/build/universe_benchmarks \
    --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
    --benchmark_out="$UC_OUT/gbench.json" --benchmark_out_format=json \
    | tee "$UC_OUT/gbench.txt"

PERF="$(ls /usr/lib/linux-tools/*/perf 2>/dev/null | head -n 1 || true)"
if [ -z "$PERF" ]; then
    echo "perf is not installed in this image (build Containerfile.perf)" | tee "$UC_OUT/perf-unavailable.txt"
    exit 0
fi

# perf.data must NOT live on the Windows bind mount: perf mmaps its output and the mount
# rejects it ("failed to write perf data, error: Bad address"). Keep it inside the container.
PERF_DATA=/tmp/uc-perf.data
PERF_GROUP_DATA=/tmp/uc-perf-group.data

WORKLOAD=(/build/universe_sim --seed "$UC_SEED" --years "$UC_YEARS" --population "$UC_POP")
HW_EVENTS="cycles,instructions,branches,branch-misses,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,cache-references,cache-misses"

# Hardware counters work only when the event is counted, not "<not supported>".
probe="$("$PERF" stat -e cycles,instructions -- true 2>&1 || true)"
if echo "$probe" | grep -Eq 'not supported|not counted'; then
    MODE=software
    echo "software" > "$UC_OUT/perf-mode.txt"
    echo "$probe" > "$UC_OUT/perf-probe.txt"
    echo "== perf (SOFTWARE events only: hardware counters are not exposed in this VM)"
    "$PERF" stat -x, -e task-clock,context-switches,cpu-migrations,page-faults \
        -o "$UC_OUT/perf-stat.csv" -- "${WORKLOAD[@]}" > "$UC_OUT/workload-stdout.txt"
    RECORD_EVENT=cpu-clock
else
    MODE=hardware
    echo "hardware" > "$UC_OUT/perf-mode.txt"
    echo "== perf stat (hardware counters)"
    "$PERF" stat -x, -e "$HW_EVENTS" -o "$UC_OUT/perf-stat.csv" -- "${WORKLOAD[@]}" > "$UC_OUT/workload-stdout.txt"
    RECORD_EVENT=cycles
fi

echo "== perf record ($RECORD_EVENT @ ${UC_PERF_FREQ} Hz, frame-pointer call graphs)"
"$PERF" record -F "$UC_PERF_FREQ" -g --call-graph fp -e "$RECORD_EVENT" \
    -o "$PERF_DATA" -- "${WORKLOAD[@]}" > /dev/null 2> "$UC_OUT/perf-record.log" || {
    echo "perf record FAILED (see perf-record.log); no hot-function profile for this run" | tee "$UC_OUT/perf-record-failed.txt"
}
"$PERF" report --stdio --no-children -i "$PERF_DATA" --percent-limit 0.5 > "$UC_OUT/perf-report.txt" 2>&1 || true
"$PERF" report --stdio --no-children -g none -i "$PERF_DATA" --sort symbol \n    -F overhead,sample,symbol --percent-limit 0.5 > "$UC_OUT/perf-top-symbols.txt" 2>&1 || true

# Per-instruction attribution for the three hottest symbols (the "assembly block" view).
awk '$3 == "[.]" && n < 3 { print $4; n++ }' "$UC_OUT/perf-top-symbols.txt" | while read -r symbol; do
    safe="$(echo "$symbol" | tr -c 'A-Za-z0-9_' '_' | cut -c1-80)"
    "$PERF" annotate --stdio -i "$PERF_DATA" --symbol="$symbol" > "$UC_OUT/annotate-$safe.txt" 2>&1 || true
done

if [ "$MODE" = hardware ]; then
    # Group the counters on the same samples so each hot symbol also shows its
    # cache-miss and branch-miss share, not only its cycle share.
    "$PERF" record -F "$UC_PERF_FREQ" -e '{cycles,instructions,cache-misses,branch-misses,LLC-load-misses}' \
        -o "$PERF_GROUP_DATA" -- "${WORKLOAD[@]}" > /dev/null 2>&1 || true
    "$PERF" report --stdio -i "$PERF_GROUP_DATA" --group --sort symbol --percent-limit 0.5 \
        > "$UC_OUT/perf-group-report.txt" 2>&1 || true
fi
echo "== tier 1 done (mode=$MODE)"
