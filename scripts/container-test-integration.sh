#!/usr/bin/env bash
set -euo pipefail

./scripts/container-build.sh
ctest --test-dir build/container -L integration --output-on-failure
