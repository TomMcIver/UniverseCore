param(
    [string]$Image = "universecore-dev",
    [string]$BuildVolume = "universecore-build",
    [string]$OutputFile = "build/trade-calibration.txt",
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$SweepArgs
)

# Runs the 64-seed trade sweep (both layouts by default) in the canonical container
# and tees the table to $OutputFile. Slow: expect minutes per layout.
$ErrorActionPreference = "Stop"

podman volume exists $BuildVolume
if ($LASTEXITCODE -ne 0) {
    podman volume create $BuildVolume | Out-Null
}
New-Item -ItemType Directory -Force -Path (Split-Path $OutputFile) | Out-Null

$podmanArgs = @(
    "run", "--rm",
    "-v", "${PWD}:/workspace",
    "-v", "${BuildVolume}:/build",
    "-w", "/workspace",
    $Image,
    "bash", "/workspace/scripts/container-trade-calibration.sh"
) + @($SweepArgs)

podman @podmanArgs | Tee-Object -FilePath $OutputFile
