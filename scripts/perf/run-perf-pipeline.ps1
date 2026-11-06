<#
.SYNOPSIS
  Multi-language performance pipeline for UniverseCore.

.DESCRIPTION
  Tier 1  C++            RelWithDebInfo build, Google Benchmark, perf stat/record (Podman);
                         VTune hotspots + uarch-exploration if `vtune` and a native compiler exist.
  Tier 2  GDScript/bridge Godot client driven through a scripted 30 s session against the real
                         C++ bridge; GDScript zones, bridge round trips, per-frame Godot monitors;
                         Tracy capture only when a Tracy-enabled native bridge is supplied.
  Tier 3  TypeScript/JS  mitata micro-benchmarks on real bridge traffic captured as NDJSON.
  Tier 4  Build          cmake --profiling-output + per-object compile times from a clean build.

  Every tier ends ok, skipped (with the reason) or failed (with the reason) in status.json.
  Nothing is estimated: a number in the report came from a file in the output directory.

.EXAMPLE
  .\scripts\perf\run-perf-pipeline.ps1                      # all tiers
  .\scripts\perf\run-perf-pipeline.ps1 -Tiers 1,4           # C++ and build only
  .\scripts\perf\run-perf-pipeline.ps1 -Tiers 2 -Headless   # client without rendering
#>
[CmdletBinding()]
param(
    [int[]]$Tiers = @(1, 2, 3, 4),
    [string]$OutDir = '',
    [int]$Seconds = 30,
    [int]$Seed = 39,
    [int]$Years = 20,
    [int]$Population = 200,
    [string]$Scenario = 'scenarios/demo.json',
    [string]$Image = 'universecore-dev',
    [string]$PerfImage = 'universecore-perf',
    [string]$PerfVolume = 'uc-perf-build',
    [string]$GodotExe = '',
    [int]$BridgePort = 19281,
    [switch]$Headless,
    [switch]$NoReport
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'lib.ps1')
Set-Location $root

if ($OutDir -eq '') { $OutDir = Join-Path $root ('perf-results\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$rel = $OutDir.Substring($root.Length).TrimStart('\') -replace '\\', '/'   # path inside the container
if ($OutDir -notlike "$root*") { throw "OutDir must be inside the repository so the container can write it: $OutDir" }

$mount = (ConvertTo-PodmanPath $root) + ':/workspace'
$podmanFound = Find-Tool @('podman')

# ---- environment record -------------------------------------------------------
$git = Find-Tool @('git')
$environment = [ordered]@{
    timestamp   = (Get-Date).ToString('o')
    host        = $env:COMPUTERNAME
    os          = (Get-CimInstance Win32_OperatingSystem).Caption
    cpu         = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    logical_cpu = [Environment]::ProcessorCount
    ram_gb      = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
    commit      = if ($git) { (& git rev-parse --short HEAD) } else { 'unknown' }
    dirty       = if ($git) { [bool](& git status --porcelain -- src include benchmarks apps/godot-client/scripts) } else { $null }
    seed        = $Seed; years = $Years; population = $Population; scenario = $Scenario
    tools       = [ordered]@{
        podman = $podmanFound; vtune = (Find-Tool @('vtune', 'vtune.exe'))
        tracy_capture = (Find-Tool @('tracy-capture', 'capture-release', 'tracy-capture.exe'))
        node = (Find-Tool @('node')); npm = (Find-Tool @('npm', 'npm.cmd')); cmake = (Find-Tool @('cmake'))
    }
}

# ---- tier 1: C++ --------------------------------------------------------------
function Invoke-Tier1 {
    if (-not $podmanFound) { Set-TierStatus 'tier1_cpp' 'skipped' 'podman not found'; return }
    $dir = Join-Path $OutDir 'tier1'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    $exists = (Invoke-Native -File 'podman' -Arguments @('image', 'exists', $PerfImage)).ExitCode -eq 0
    if (-not $exists) {
        Write-Host 'building perf image (adds linux perf to the dev image)...'
        $built = Invoke-Native -File 'podman' -Arguments @('build', '-t', $PerfImage, '-f', 'Containerfile.perf', '.') -LogFile (Join-Path $dir 'perf-image-build.log')
        if ($built.ExitCode -ne 0) { Set-TierStatus 'tier1_cpp' 'failed' 'could not build the perf image (see tier1/perf-image-build.log)'; return }
    }
    $null = Invoke-Native -File 'podman' -Arguments @('volume', 'create', $PerfVolume)

    $script = Get-Content -Raw (Join-Path $PSScriptRoot 'container\tier1-cpp.sh')
    $run = Invoke-Native -File 'podman' -StdIn $script -LogFile (Join-Path $dir 'tier1-container.log') -Arguments @(
        'run', '--rm', '-i',
        '--cap-add', 'SYS_ADMIN', '--security-opt', 'seccomp=unconfined',
        '-v', $mount, '-v', "${PerfVolume}:/build", '-w', '/workspace',
        '-e', "UC_OUT=/workspace/$rel/tier1", '-e', "UC_SEED=$Seed", '-e', "UC_YEARS=$Years", '-e', "UC_POP=$Population",
        $PerfImage, 'bash', '-s')
    if ($run.ExitCode -ne 0) { Set-TierStatus 'tier1_cpp' 'failed' "container exit $($run.ExitCode), see tier1/tier1-container.log"; return }

    $mode = if (Test-Path "$dir\perf-mode.txt") { (Get-Content "$dir\perf-mode.txt" -Raw).Trim() } else { 'no perf' }
    Set-TierStatus 'tier1_cpp' 'ok' "benchmarks + perf ($mode counters) in $($run.Seconds)s"

    # Native Windows: VTune gives the hardware counters a Podman VM cannot.
    $vtune = $environment.tools.vtune
    if (-not $vtune) {
        Set-TierStatus 'tier1_vtune' 'skipped' 'vtune not on PATH (hardware counters need VTune on the host or perf on bare-metal Linux)'
        return
    }
    $cmake = $environment.tools.cmake
    if (-not $cmake) { Set-TierStatus 'tier1_vtune' 'skipped' 'cmake not found for a native build'; return }
    $nativeBuild = Join-Path $root 'build\perf-native'
    $cfg = Invoke-Native -File $cmake -LogFile (Join-Path $dir 'native-configure.log') -Arguments @(
        '-S', $root, '-B', $nativeBuild, '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DUNIVERSECORE_PROFILE_FLAGS=ON', '-DUNIVERSECORE_BUILD_TESTS=OFF')
    $bld = if ($cfg.ExitCode -eq 0) { Invoke-Native -File $cmake -LogFile (Join-Path $dir 'native-build.log') -Arguments @('--build', $nativeBuild, '--config', 'RelWithDebInfo', '--target', 'universe_sim') } else { $cfg }
    $exe = Get-ChildItem -Path $nativeBuild -Recurse -Filter 'universe_sim.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($bld.ExitCode -ne 0 -or -not $exe) { Set-TierStatus 'tier1_vtune' 'failed' 'native build failed (see tier1/native-*.log)'; return }

    $workload = @('--seed', "$Seed", '--years', "$Years", '--population', "$Population")
    foreach ($analysis in @('hotspots', 'uarch-exploration')) {
        $result = Join-Path $dir "vtune-$analysis"
        $collect = Invoke-Native -File $vtune -LogFile (Join-Path $dir "vtune-$analysis.log") -Arguments (@('-collect', $analysis, '-result-dir', $result, '-quiet', '--', $exe.FullName) + $workload)
        if ($collect.ExitCode -ne 0) { Set-TierStatus "tier1_vtune_$analysis" 'failed' "vtune exit $($collect.ExitCode)"; continue }
        $null = Invoke-Native -File $vtune -Arguments @('-report', 'summary', '-result-dir', $result, '-format', 'csv', '-report-output', (Join-Path $dir "vtune-$analysis-summary.csv"))
        $null = Invoke-Native -File $vtune -Arguments @('-report', 'hotspots', '-result-dir', $result, '-group-by', 'function', '-format', 'csv', '-report-output', (Join-Path $dir "vtune-$analysis-functions.csv"))
        Set-TierStatus "tier1_vtune_$analysis" 'ok' "$($collect.Seconds)s"
    }
}

# ---- tier 2: GDScript + bridge ------------------------------------------------
function Invoke-Tier2 {
    if (-not $podmanFound -and -not $env:UC_TRACY_NATIVE_BRIDGE) { Set-TierStatus 'tier2_gdscript' 'skipped' 'podman not found for the bridge'; return }
    $node = $environment.tools.node
    if (-not $node) { Set-TierStatus 'tier2_gdscript' 'skipped' 'node not found for the WebSocket bridge'; return }
    $dir = Join-Path $OutDir 'tier2'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    if ($GodotExe -eq '') {
        $GodotExe = if ($env:GODOT_EXE) { $env:GODOT_EXE } else { (Find-Tool @('godot', 'godot4', 'Godot_v4.7.2-stable_win64_console.exe')) }
    }
    if (-not $GodotExe -or -not (Test-Path $GodotExe)) { Set-TierStatus 'tier2_gdscript' 'skipped' 'Godot not found (pass -GodotExe or set GODOT_EXE)'; return }

    $bridgeBin = Join-Path $root 'build\container\universe_ui_bridge'
    if (-not $env:UC_TRACY_NATIVE_BRIDGE -and -not (Test-Path $bridgeBin)) {
        Write-Host 'building the release bridge in build/container...'
        $b = Invoke-Native -File 'podman' -StdIn "set -e`ncmake -S /workspace -B build/container -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNIVERSECORE_BUILD_TESTS=OFF`ncmake --build build/container --target universe_ui_bridge`n" `
            -LogFile (Join-Path $dir 'bridge-build.log') -Arguments @('run', '--rm', '-i', '-v', $mount, '-w', '/workspace', $Image, 'bash', '-s')
        if ($b.ExitCode -ne 0) { Set-TierStatus 'tier2_gdscript' 'failed' 'bridge build failed (tier2/bridge-build.log)'; return }
    }

    # Import assets once so a fresh checkout does not fail on missing .godot cache.
    $project = Join-Path $root 'apps\godot-client'
    if (-not (Test-Path (Join-Path $project '.godot'))) {
        $null = Invoke-Native -File $GodotExe -Arguments @('--headless', '--editor', '--import', '--path', $project) -LogFile (Join-Path $dir 'godot-import.log') -TimeoutSeconds 600
    }

    # Bridge: the real C++ simulation behind the same WebSocket the player uses.
    $bridgeEnv = @{ UNIVERSE_WS_PORT = "$BridgePort"; UNIVERSE_SCENARIO = $Scenario }
    if ($env:UC_TRACY_NATIVE_BRIDGE) { $bridgeEnv['UNIVERSE_BRIDGE_BIN'] = $env:UC_TRACY_NATIVE_BRIDGE }
    $bridgeLog = Join-Path $dir 'ws-bridge.out'
    $bridgeStart = New-Object System.Diagnostics.ProcessStartInfo
    $bridgeStart.FileName = $node
    $bridgeStart.Arguments = 'apps/web-client/server/ws-bridge.mjs'
    $bridgeStart.WorkingDirectory = $root
    $bridgeStart.UseShellExecute = $false
    $bridgeStart.RedirectStandardOutput = $true
    $bridgeStart.RedirectStandardError = $true
    $bridgeStart.CreateNoWindow = $true
    foreach ($k in $bridgeEnv.Keys) { $bridgeStart.EnvironmentVariables[$k] = $bridgeEnv[$k] }
    $bridge = [System.Diagnostics.Process]::Start($bridgeStart)
    $null = $bridge.StandardOutput.ReadToEndAsync()
    $null = $bridge.StandardError.ReadToEndAsync()

    $tracy = $null
    try {
        $listening = $false
        for ($i = 0; $i -lt 40 -and -not $listening; $i++) {
            Start-Sleep -Milliseconds 500
            $listening = [bool](Get-NetTCPConnection -LocalPort $BridgePort -State Listen -ErrorAction SilentlyContinue)
        }
        if (-not $listening) { Set-TierStatus 'tier2_gdscript' 'failed' "bridge never listened on port $BridgePort"; return }

        # Tracy: only meaningful against a native Tracy-enabled bridge, since a bridge in
        # Podman does not expose Tracy's port. Supplied via UC_TRACY_NATIVE_BRIDGE.
        $tracyExe = $environment.tools.tracy_capture
        if ($env:UC_TRACY_NATIVE_BRIDGE -and $tracyExe) {
            $tracy = Start-Process -FilePath $tracyExe -ArgumentList @('-o', (Join-Path $dir 'bridge.tracy'), '-s', "$($Seconds + 15)", '-f') -PassThru -WindowStyle Hidden
            Set-TierStatus 'tier2_tracy' 'ok' 'tracy-capture attached to the native bridge'
        } elseif ($env:UC_TRACY_NATIVE_BRIDGE) {
            Set-TierStatus 'tier2_tracy' 'skipped' 'tracy-capture not on PATH'
        } else {
            Set-TierStatus 'tier2_tracy' 'skipped' 'set UC_TRACY_NATIVE_BRIDGE to a native universe_ui_bridge built with -DUNIVERSECORE_ENABLE_TRACY=ON (see docs/perf/README.md)'
        }

        $godotArgs = @('--path', $project)
        if ($Headless) { $godotArgs = @('--headless') + $godotArgs }
        $help = (Invoke-Native -File $GodotExe -Arguments @('--help')).Stdout
        if ($help -match '--benchmark-file') { $godotArgs += @('--benchmark', '--benchmark-file', (Join-Path $dir 'godot-benchmark.json')) }

        # Godot is judged by what it reports, not by whether it exits promptly: the run is
        # done when it prints PERF_RUN done (its CSV/JSON files are written before that
        # line). If the process then lingers, it is stopped and the note says so.
        $env:UNIVERSECORE_PERF_DIR = $dir
        $env:UNIVERSECORE_PERF_SECONDS = "$Seconds"
        $env:UNIVERSE_WS_PORT = "$BridgePort"
        $godotLog = Join-Path $dir 'godot.log'
        $godotArgText = ($godotArgs | ForEach-Object { Format-NativeArgument $_ }) -join ' '
        $godot = Start-Process -FilePath $GodotExe -ArgumentList $godotArgText -PassThru `
            -RedirectStandardOutput $godotLog -RedirectStandardError (Join-Path $dir 'godot.err')
        $finished = $false
        $deadline = (Get-Date).AddSeconds($Seconds + 180)
        while ((Get-Date) -lt $deadline -and -not $finished) {
            Start-Sleep -Milliseconds 500
            $text = Get-Content $godotLog -Raw -ErrorAction SilentlyContinue
            if ($text -match 'PERF_RUN done') { $finished = $true }
            elseif ($godot.HasExited) { break }
        }
        $lingered = ''
        if (-not $godot.HasExited) {
            if ($finished -and $godot.WaitForExit(20000)) { }
            else { if ($finished) { $lingered = ' (Godot kept running after finishing and was stopped)' }; try { $godot.Kill() } catch { } }
        }
        $log = Get-Content $godotLog -Raw -ErrorAction SilentlyContinue
        if (-not $finished) { Set-TierStatus 'tier2_gdscript' 'failed' "no PERF_RUN done line within $($Seconds + 180)s; see tier2/godot.log and godot.err" }
        else { Set-TierStatus 'tier2_gdscript' 'ok' ((($log -split "`n" | Where-Object { $_ -match 'PERF_RUN done' } | Select-Object -First 1).Trim()) + $lingered) }
        Remove-Item Env:UNIVERSECORE_PERF_DIR, Env:UNIVERSECORE_PERF_SECONDS, Env:UNIVERSE_WS_PORT -ErrorAction SilentlyContinue
    } finally {
        if ($tracy -and -not $tracy.HasExited) { try { $tracy.WaitForExit(20000) | Out-Null; if (-not $tracy.HasExited) { $tracy.Kill() } } catch { } }
        if ($bridge -and -not $bridge.HasExited) { $bridge.Kill() }
    }
}

# ---- tier 3: TypeScript/JS ----------------------------------------------------
function Invoke-Tier3 {
    $node = $environment.tools.node
    $npm = $environment.tools.npm
    if (-not $node -or -not $npm) { Set-TierStatus 'tier3_js' 'skipped' 'node/npm not found'; return }
    if (-not $podmanFound) { Set-TierStatus 'tier3_js' 'skipped' 'podman not found to capture bridge traffic'; return }
    $dir = Join-Path $OutDir 'tier3'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    $cap = Invoke-Native -File 'podman' -StdIn (Get-Content -Raw (Join-Path $PSScriptRoot 'container\tier3-fixture.sh')) -LogFile (Join-Path $dir 'fixture-capture.log') -Arguments @(
        'run', '--rm', '-i', '-v', $mount, '-w', '/workspace',
        '-e', "UC_OUT=/workspace/$rel/tier3", '-e', "UC_SCENARIO=$Scenario", $Image, 'bash', '-s')
    $fixture = Join-Path $dir 'fixture.ndjson'
    if ($cap.ExitCode -ne 0 -or -not (Test-Path $fixture)) { Set-TierStatus 'tier3_js' 'failed' 'could not capture bridge traffic (tier3/fixture-capture.log)'; return }

    $web = Join-Path $root 'apps\web-client'
    if (-not (Test-Path (Join-Path $web 'node_modules\mitata'))) {
        $i = Invoke-Native -File $npm -Arguments @('ci') -WorkingDirectory $web -LogFile (Join-Path $dir 'npm-ci.log') -TimeoutSeconds 600
        if ($i.ExitCode -ne 0) { Set-TierStatus 'tier3_js' 'failed' 'npm ci failed (tier3/npm-ci.log)'; return }
    }
    $env:UC_PERF_FIXTURE = $fixture
    $table = Invoke-Native -File $node -Arguments @('bench/protocol.bench.mjs') -WorkingDirectory $web -LogFile (Join-Path $dir 'mitata.txt') -TimeoutSeconds 600
    $json = Invoke-Native -File $node -Arguments @('bench/protocol.bench.mjs', '--json') -WorkingDirectory $web -TimeoutSeconds 600
    if ($json.ExitCode -eq 0) { [System.IO.File]::WriteAllText((Join-Path $dir 'mitata.json'), $json.Stdout) }
    if ($table.ExitCode -ne 0) { Set-TierStatus 'tier3_js' 'failed' "mitata exit $($table.ExitCode)"; return }
    Set-TierStatus 'tier3_js' 'ok' "mitata on $((Get-Content $fixture | Measure-Object -Line).Lines) real bridge messages"
}

# ---- tier 4: build automation ---------------------------------------------------
function Invoke-Tier4 {
    if (-not $podmanFound) { Set-TierStatus 'tier4_build' 'skipped' 'podman not found'; return }
    $dir = Join-Path $OutDir 'tier4'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $run = Invoke-Native -File 'podman' -StdIn (Get-Content -Raw (Join-Path $PSScriptRoot 'container\tier4-cmake.sh')) -LogFile (Join-Path $dir 'tier4-container.log') -Arguments @(
        'run', '--rm', '-i', '-v', $mount, '-w', '/workspace', '-e', "UC_OUT=/workspace/$rel/tier4", $Image, 'bash', '-s')
    if ($run.ExitCode -ne 0) { Set-TierStatus 'tier4_build' 'failed' "container exit $($run.ExitCode), see tier4/tier4-container.log"; return }
    $summary = (Get-Content (Join-Path $dir 'build-summary.txt')) -join ', '
    Set-TierStatus 'tier4_build' 'ok' $summary
}

# ---- run -------------------------------------------------------------------------
$environment | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $OutDir 'environment.json') -Encoding UTF8
Write-Host "output: $OutDir"
foreach ($tier in ($Tiers | Sort-Object)) {
    try {
        switch ($tier) {
            1 { Invoke-Tier1 }
            2 { Invoke-Tier2 }
            3 { Invoke-Tier3 }
            4 { Invoke-Tier4 }
            default { Write-Warning "unknown tier $tier" }
        }
    } catch {
        Set-TierStatus ("tier{0}" -f $tier) 'failed' $_.Exception.Message
    }
    Save-TierStatus (Join-Path $OutDir 'status.json')
}

if (-not $NoReport) {
    & (Join-Path $PSScriptRoot 'aggregate-report.ps1') -OutDir $OutDir
}
Write-Host "done: $OutDir"
