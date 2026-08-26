#!/usr/bin/env bash
set -euo pipefail

IMAGE="${1:-universecore-dev}"
BUILD_VOLUME="${UNIVERSECORE_UI_BUILD_VOLUME:-universecore-ui-build}"
CLEAN="${UNIVERSECORE_UI_CLEAN:-0}"

if [[ "${CLEAN}" == "1" ]]; then
    podman volume rm -f "${BUILD_VOLUME}" >/dev/null 2>&1 || true
fi

if ! podman volume inspect "${BUILD_VOLUME}" >/dev/null 2>&1; then
    podman volume create "${BUILD_VOLUME}" >/dev/null
fi

podman run --rm \
    -v "${PWD}:/workspace" \
    -v "${BUILD_VOLUME}:/build-ui" \
    -w /workspace \
    "${IMAGE}" \
    bash -lc "set -euo pipefail; cmake -S /workspace -B /build-ui -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNIVERSECORE_BUILD_UI=ON; cmake --build /build-ui; ctest --test-dir /build-ui -R universe_ui_smoke --output-on-failure"
