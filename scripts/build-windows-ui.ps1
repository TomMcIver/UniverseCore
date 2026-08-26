param(
    [string]$Image = "universecore-winui-dev",
    [string]$BuildVolume = "universecore-winui-build",
    [switch]$RebuildImage,
    [switch]$CleanBuild
)

$ErrorActionPreference = "Stop"
$workspace = (Get-Location).Path

if ($RebuildImage) {
    podman image rm -f $Image *> $null
}

$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
podman image inspect $Image *> $null
$imageExistsExit = $LASTEXITCODE
$ErrorActionPreference = $previousErrorActionPreference

if ($imageExistsExit -ne 0) {
    podman build -t $Image -f Containerfile.windows-ui .
}

if ($CleanBuild) {
    podman volume rm -f $BuildVolume *> $null
}

$ErrorActionPreference = "Continue"
podman volume inspect $BuildVolume *> $null
$volumeExistsExit = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($volumeExistsExit -ne 0) {
    podman volume create $BuildVolume | Out-Null
}

$inner = "tr -d '\r' < /workspace/scripts/windows-ui-cross-build.sh > /tmp/windows-ui-cross-build.sh; chmod +x /tmp/windows-ui-cross-build.sh; /tmp/windows-ui-cross-build.sh /workspace /build-ui"

podman run --rm `
    -v "${workspace}:/workspace" `
    -v "${BuildVolume}:/build-ui" `
    -w /workspace `
    $Image `
    bash -lc "$inner"

$exePath = "build/windows-ui/UniverseCore/universe_ui.exe"
if (-not (Test-Path $exePath)) {
    throw "Windows UI packaging did not produce $exePath"
}

Write-Output "WINDOWS_UI_OUTPUT|$workspace/$exePath"
