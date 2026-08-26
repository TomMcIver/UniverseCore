#!/usr/bin/env bash
set -euo pipefail

IMAGE="${1:-universecore-dev}"
BUILD_VOLUME="${UNIVERSECORE_BUILD_VOLUME:-universecore-build}"
SUITE="${UNIVERSECORE_TEST_SUITE:-all}"

case "${SUITE}" in
    all|fast|integration|long)
        ;;
    *)
        echo "Invalid UNIVERSECORE_TEST_SUITE: ${SUITE} (expected all|fast|integration|long)" >&2
        exit 2
        ;;
esac

if ! podman volume inspect "${BUILD_VOLUME}" >/dev/null 2>&1; then
    podman volume create "${BUILD_VOLUME}" >/dev/null
fi

CTEST_ARGS=(--output-on-failure)
if [[ "${SUITE}" != "all" ]]; then
    CTEST_ARGS=(-L "${SUITE}" --output-on-failure)
fi

podman run --rm \
    -v "${PWD}:/workspace" \
    -v "${BUILD_VOLUME}:/build" \
    -w /workspace \
    "${IMAGE}" \
    bash -lc "set -euo pipefail; cmake -S /workspace -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release; cmake --build /build; ctest --test-dir /build ${CTEST_ARGS[*]}"
