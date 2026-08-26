#!/usr/bin/env bash
set -euo pipefail

./scripts/container-ui-build.sh
ctest --test-dir build/container-ui -R universe_ui_smoke --output-on-failure
