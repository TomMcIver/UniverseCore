#!/usr/bin/env bash
set -euo pipefail

cmake -S . -B build/container-ui -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNIVERSECORE_BUILD_UI=ON
cmake --build build/container-ui
