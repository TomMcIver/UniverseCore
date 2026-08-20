#!/usr/bin/env bash
set -euo pipefail

cmake -S . -B build/container -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/container
