#!/usr/bin/env bash
# One-at-a-time parameter sweep of the trade constants (ADR 0003, #76). Each
# variant is a separate compile of universe_core with -DUNIVERSE_TRADE_*=value
# (include/universe/trade_tuning.hpp), built under /build/sweep-<name> in the
# named volume, then measured with the calibration executable. Run inside the
# container: scripts/container-trade-sweep.sh [seed-count] [name=FLAG=value ...]
# With no variants it runs the default list below. Output: one block per variant.
set -euo pipefail

SEEDS="${1:-16}"
shift || true
DEFAULTS=(
  "baseline=UNIVERSE_TRADE_MAX_MERCHANTS_PER_SETTLEMENT=2"
  "margin250=UNIVERSE_TRADE_MARGIN_THRESHOLD_PERMILLE=250"
  "margin1000=UNIVERSE_TRADE_MARGIN_THRESHOLD_PERMILLE=1000"
  "margin3000=UNIVERSE_TRADE_MARGIN_THRESHOLD_PERMILLE=3000"
  "merchants1=UNIVERSE_TRADE_MAX_MERCHANTS_PER_SETTLEMENT=1"
  "merchants4=UNIVERSE_TRADE_MAX_MERCHANTS_PER_SETTLEMENT=4"
  "load25=UNIVERSE_TRADE_BASE_CARGO_UNITS=25"
  "load100=UNIVERSE_TRADE_BASE_CARGO_UNITS=100"
  "load200=UNIVERSE_TRADE_BASE_CARGO_UNITS=200"
  "cap10=UNIVERSE_TRADE_STOCK_CAP_PERCENT=10"
  "cap50=UNIVERSE_TRADE_STOCK_CAP_PERCENT=50"
  "share0=UNIVERSE_TRADE_PROFIT_SHARE_PERMILLE=0"
  "share500=UNIVERSE_TRADE_PROFIT_SHARE_PERMILLE=500"
  "stale200=UNIVERSE_TRADE_STALE_DAYS=200"
  "stale1000=UNIVERSE_TRADE_STALE_DAYS=1000"
)
VARIANTS=("$@")
if [ "${#VARIANTS[@]}" -eq 0 ]; then
  VARIANTS=("${DEFAULTS[@]}")
fi

for spec in "${VARIANTS[@]}"; do
  name="${spec%%=*}"
  flags="${spec#*=}"
  dir="/build/sweep-${name}"
  defs=""
  # A spec may carry several comma-separated NAME=value pairs.
  IFS=',' read -ra pairs <<< "$flags"
  for pair in "${pairs[@]}"; do defs="$defs -D$pair"; done
  cmake -S /workspace -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="$defs" > /dev/null
  cmake --build "$dir" --target universe_trade_calibration > /dev/null
  echo "=== variant ${name} (${defs# })"
  "$dir/universe_trade_calibration" --seed-count "$SEEDS" --years 20 --brief 1 | grep -E '^(layout|summary|observed|paired|invariants|INVARIANT)'
done
