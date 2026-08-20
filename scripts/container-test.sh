#!/usr/bin/env bash
set -euo pipefail

./scripts/container-build.sh
ctest --test-dir build/container --output-on-failure
