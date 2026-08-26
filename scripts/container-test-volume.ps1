param(
    [string]$Image = "universecore-dev",
    [string]$BuildVolume = "universecore-build",
    [ValidateSet("all", "fast", "integration", "long")]
    [string]$Suite = "all"
)

$ErrorActionPreference = "Stop"

podman volume inspect $BuildVolume *> $null
if ($LASTEXITCODE -ne 0) {
    podman volume create $BuildVolume | Out-Null
}

$ctestArgs = "--output-on-failure"
if ($Suite -ne "all") {
    $ctestArgs = "-L $Suite --output-on-failure"
}

$inner = "set -e; set -u; set -o pipefail; cmake -S /workspace -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release; cmake --build /build; ctest --test-dir /build $ctestArgs"

podman run --rm `
    -v "${PWD}:/workspace" `
    -v "${BuildVolume}:/build" `
    -w /workspace `
    $Image `
    bash -lc "$inner"
