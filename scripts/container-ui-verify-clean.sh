#!/usr/bin/env bash
set -euo pipefail

rm -rf build/container-ui-clean
cmake -S . -B build/container-ui-clean -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNIVERSECORE_BUILD_UI=ON
cmake --build build/container-ui-clean
ctest --test-dir build/container-ui-clean -R universe_ui_smoke --output-on-failure
