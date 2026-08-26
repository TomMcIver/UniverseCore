param(
    [switch]$Rebuild,
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"

$exePath = "build/windows-ui/UniverseCore/universe_ui.exe"

if ($Rebuild -or -not (Test-Path $exePath)) {
    ./scripts/build-windows-ui.ps1
}

if ($BuildOnly) {
    Write-Output "WINDOWS_UI_READY|$exePath"
    exit 0
}

$process = Start-Process -FilePath $exePath -WorkingDirectory "build/windows-ui/UniverseCore" -PassThru
Start-Sleep -Seconds 3
$alive = -not $process.HasExited
$windowTitle = ""
if ($alive) {
    try {
        $windowTitle = (Get-Process -Id $process.Id -ErrorAction Stop).MainWindowTitle
    } catch {
        $windowTitle = ""
    }
}

Write-Output "WINDOWS_UI_LAUNCHED|$alive"
Write-Output "WINDOWS_UI_PID|$($process.Id)"
Write-Output "WINDOW_TITLE|$windowTitle"
