#!/usr/bin/env bash
# Captures real bridge traffic (NDJSON) for the TypeScript benchmarks.
# Inputs (env): UC_OUT, UC_SCENARIO (repo-relative scenario file), UC_FIXTURE_YEAR (default 6)
#
# The bridge reads one JSON command per line from stdin and answers with
# initial_snapshot, simulation_status, world_delta and world_event lines. stdin is
# held open with sleep so the bridge keeps running until the year is reached.
set -euo pipefail

: "${UC_OUT:?}" "${UC_SCENARIO:=scenarios/demo.json}" "${UC_FIXTURE_YEAR:=6}" "${UC_FIXTURE_HOLD:=60}"
mkdir -p "$UC_OUT"

cd /workspace
if [ ! -x ./build/container/universe_ui_bridge ]; then
    cmake -S . -B build/container -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNIVERSECORE_BUILD_TESTS=OFF
    cmake --build build/container --target universe_ui_bridge
fi

{
    echo '{"type":"hello"}'
    sleep 1
    echo '{"type":"request_snapshot"}'
    sleep 1
    echo "{\"type\":\"run_until_year\",\"year\":${UC_FIXTURE_YEAR}}"
    sleep "$UC_FIXTURE_HOLD"
} | ./build/container/universe_ui_bridge --scenario "$UC_SCENARIO" > "$UC_OUT/fixture.ndjson"

echo "captured $(wc -l < "$UC_OUT/fixture.ndjson") lines, $(wc -c < "$UC_OUT/fixture.ndjson") bytes"
