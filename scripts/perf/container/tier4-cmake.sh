#!/usr/bin/env bash
# Tier 4 (build automation), runs inside the universecore-dev container.
# Inputs (env): UC_OUT
#
# Measures a CLEAN configure + build:
#   * cmake --profiling-output  -> Chrome trace of configure-time script execution
#   * ninja .ninja_log          -> per-object compile/link wall time
# Builds in a throwaway dir inside the container, so build/container is untouched.
# Note: the container compiler is GCC. -ftime-trace is Clang-only, so per-header
# and template-instantiation timing is NOT collected here; compile-times.csv is
# per translation unit.
set -euo pipefail

: "${UC_OUT:?}"
mkdir -p "$UC_OUT"
BUILD=/tmp/uc-tier4-build
rm -rf "$BUILD"

echo "== configure with profiling"
start=$(date +%s.%N)
cmake -S /workspace -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DUNIVERSECORE_BUILD_TESTS=ON \
    --profiling-output="$UC_OUT/cmake-configure-trace.json" --profiling-format=google-trace
end=$(date +%s.%N)
echo "configure_seconds=$(awk "BEGIN{printf \"%.1f\", $end - $start}")" > "$UC_OUT/build-summary.txt"

echo "== clean build"
start=$(date +%s.%N)
cmake --build "$BUILD" -j "$(nproc)"
end=$(date +%s.%N)
echo "build_seconds=$(awk "BEGIN{printf \"%.1f\", $end - $start}")" >> "$UC_OUT/build-summary.txt"
echo "jobs=$(nproc)" >> "$UC_OUT/build-summary.txt"

# .ninja_log columns: start_ms end_ms mtime output command_hash (tab separated).
echo "object,wall_ms" > "$UC_OUT/compile-times.csv"
awk -F'\t' 'NR>1 && !seen[$4]++ {printf "%s,%d\n", $4, $2-$1}' "$BUILD/.ninja_log" \
    | sort -t, -k2 -nr >> "$UC_OUT/compile-times.csv"
echo "objects=$(($(wc -l < "$UC_OUT/compile-times.csv") - 1))" >> "$UC_OUT/build-summary.txt"
rm -rf "$BUILD"
cat "$UC_OUT/build-summary.txt"
