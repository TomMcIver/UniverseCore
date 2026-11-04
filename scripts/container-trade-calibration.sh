#!/usr/bin/env bash
# Builds in /build (a named volume) and runs the slow trade calibration sweep
# (ADR 0003, #76). Extra arguments go to universe_trade_calibration, for example
# --layout default --seed-count 16. Run via scripts/run-trade-calibration.ps1.
set -euo pipefail

cmake -S /workspace -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build /build --target universe_trade_calibration
/build/universe_trade_calibration "$@"
