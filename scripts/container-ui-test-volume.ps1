param(
    [string]$Image = "universecore-dev",
    [string]$BuildVolume = "universecore-ui-build",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

if ($Clean) {
    podman volume rm -f $BuildVolume *> $null
}

$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
podman volume inspect $BuildVolume *> $null
$probeExit = $LASTEXITCODE
$ErrorActionPreference = $previousErrorActionPreference
if ($probeExit -ne 0) {
    podman volume create $BuildVolume | Out-Null
}

$inner = "set -e; set -u; set -o pipefail; cmake -S /workspace -B /build-ui -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNIVERSECORE_BUILD_UI=ON; cmake --build /build-ui; ctest --test-dir /build-ui -R universe_ui_smoke --output-on-failure"

podman run --rm `
    -v "${PWD}:/workspace" `
    -v "${BuildVolume}:/build-ui" `
    -w /workspace `
    $Image `
    bash -lc "$inner"
