#!/usr/bin/env bash
set -euo pipefail

if [[ ! -x ./build/container/universe_sim ]]; then
	./scripts/container-build.sh
fi

./build/container/universe_sim "$@"
