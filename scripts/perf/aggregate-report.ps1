<#
.SYNOPSIS
  Aggregates one pipeline run directory into report.md using docs/perf/REPORT_TEMPLATE.md.
  Every value comes from a file in the run directory; a missing file produces
  "NOT COLLECTED (reason)" and never a guess.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutDir,
    [string]$Template = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ($Template -eq '') { $Template = Join-Path $root 'docs\perf\REPORT_TEMPLATE.md' }
$OutDir = (Resolve-Path $OutDir).Path

function Read-Json([string]$Path) {
    if (-not (Test-Path $Path)) { return $null }
    return Get-Content $Path -Raw | ConvertFrom-Json
}

function Not-Collected([string]$Why) { return "_NOT COLLECTED: $Why._" }

function Get-Percentile([double[]]$Sorted, [double]$Fraction) {
    if ($Sorted.Count -eq 0) { return 0.0 }
    $index = [math]::Min($Sorted.Count - 1, [math]::Max(0, [int][math]::Ceiling($Fraction * $Sorted.Count) - 1))
    return $Sorted[$index]
}

function Format-Ms([double]$Microseconds) { return ('{0:N2}' -f ($Microseconds / 1000.0)) }

function Strip-Ansi([string]$Text) { return ($Text -replace '\x1b\[[0-9;]*[A-Za-z]', '') }

function Read-Head([string]$Path, [int]$Lines) {
    if (-not (Test-Path $Path)) { return $null }
    return ((Get-Content $Path -TotalCount $Lines) -join "`n")
}

$status = Read-Json (Join-Path $OutDir 'status.json')
$env = Read-Json (Join-Path $OutDir 'environment.json')
$v = [ordered]@{}

# ---- header ------------------------------------------------------------------
$v.run_dir = $OutDir.Substring($root.Length).TrimStart('\')
foreach ($key in 'timestamp', 'commit', 'cpu', 'logical_cpu', 'ram_gb', 'os', 'scenario', 'seed', 'years', 'population') {
    $v[$key] = if ($env -and $null -ne $env.$key) { $env.$key } else { 'unknown' }
}
$v.dirty_note = if ($env -and $env.dirty) { '(**working tree had uncommitted changes in src/include/benchmarks/client scripts**)' } else { '' }

# ---- status table ---------------------------------------------------------------
if ($status) {
    $rows = foreach ($p in $status.PSObject.Properties) { "| $($p.Name) | $($p.Value.state) | $($p.Value.detail -replace '\|', '/') |" }
    $v.status_table = "| Step | State | Detail |`n|---|---|---|`n" + ($rows -join "`n")
} else { $v.status_table = Not-Collected 'status.json missing' }

# ---- 1.1 GDScript zones -----------------------------------------------------------
# Computed from the raw rows, not from gdscript_summary.json, so the report cannot
# disagree with the CSV a reader can open.
$summary = Read-Json (Join-Path $OutDir 'tier2\gdscript_summary.json')
$zonePath = Join-Path $OutDir 'tier2\gdscript_zones.csv'
if ((Test-Path $zonePath) -and ((Import-Csv $zonePath).Count -gt 0)) {
    $rows = Import-Csv $zonePath | Group-Object zone | ForEach-Object {
        $us = [double[]]($_.Group | ForEach-Object { [double]$_.duration_us } | Sort-Object)
        $total = ($us | Measure-Object -Sum).Sum
        [pscustomobject]@{ zone = $_.Name; n = $us.Count; mean = $total / $us.Count; p50 = (Get-Percentile $us 0.5); p95 = (Get-Percentile $us 0.95); p99 = (Get-Percentile $us 0.99); max = $us[-1]; total = $total }
    } | Sort-Object total -Descending | ForEach-Object {
        "| ``$($_.zone)`` | $($_.n) | $(Format-Ms $_.mean) | $(Format-Ms $_.p50) | $(Format-Ms $_.p95) | $(Format-Ms $_.p99) | $(Format-Ms $_.max) | $(Format-Ms $_.total) |"
    }
    $godotVersion = if ($summary) { "Godot $($summary.godot_version)." } else { '' }
    $v.gdscript_zone_table = "| Zone | Calls | Mean ms | p50 ms | p95 ms | p99 ms | Max ms | Total ms |`n|---|---|---|---|---|---|---|---|`n" + ($rows -join "`n") + "`n`n$godotVersion"
} else { $v.gdscript_zone_table = Not-Collected 'tier2 did not produce gdscript_zones.csv' }

# ---- 1.2 bridge round trips ---------------------------------------------------------
$rttPath = Join-Path $OutDir 'tier2\bridge_rtt.csv'
if ((Test-Path $rttPath) -and ((Import-Csv $rttPath).Count -gt 0)) {
    $rows = Import-Csv $rttPath | Group-Object command | ForEach-Object {
        $us = [double[]]($_.Group | ForEach-Object { [double]$_.rtt_us } | Sort-Object)
        $bytes = ($_.Group | Measure-Object -Property response_bytes -Average).Average
        "| ``$($_.Name)`` | $($_.Count) | $(Format-Ms (Get-Percentile $us 0.5)) | $(Format-Ms (Get-Percentile $us 0.95)) | $(Format-Ms $us[-1]) | $([math]::Round($bytes)) |"
    }
    $v.bridge_rtt_table = "| Command | Round trips | p50 ms | p95 ms | Max ms | Mean response bytes |`n|---|---|---|---|---|---|`n" + ($rows -join "`n")
} else { $v.bridge_rtt_table = Not-Collected 'no bridge round trips recorded' }

# ---- 1.3 frames --------------------------------------------------------------------------
$framePath = Join-Path $OutDir 'tier2\godot_frames.csv'
if ((Test-Path $framePath) -and ((Import-Csv $framePath).Count -gt 0)) {
    $frames = Import-Csv $framePath
    $d = [double[]]($frames | ForEach-Object { [double]$_.delta_ms } | Sort-Object)
    $over16 = ($d | Where-Object { $_ -gt 16.7 }).Count
    $over33 = ($d | Where-Object { $_ -gt 33.3 }).Count
    $draw = ($frames | Measure-Object -Property draw_calls -Average).Average
    $nodes = ($frames | Measure-Object -Property nodes -Maximum).Maximum
    $hasRender = $frames[0].PSObject.Properties.Name -contains 'render_gpu_ms'
    $renderRows = ''
    if ($hasRender) {
        $gpu = [double[]]($frames | ForEach-Object { [double]$_.render_gpu_ms } | Sort-Object)
        $cpuR = [double[]]($frames | ForEach-Object { [double]$_.render_cpu_ms } | Sort-Object)
        if ($gpu[-1] -le 0) {
            $renderRows = "| GPU render time | not reported by the driver (0 on every frame), so GPU-bound vs script-bound cannot be separated |`n"
        } else {
            $renderRows = "| Render thread CPU p50 / p95 / max (ms) | $(('{0:N2}' -f (Get-Percentile $cpuR 0.5))) / $(('{0:N2}' -f (Get-Percentile $cpuR 0.95))) / $(('{0:N2}' -f $cpuR[-1])) |`n| GPU render p50 / p95 / max (ms) | $(('{0:N2}' -f (Get-Percentile $gpu 0.5))) / $(('{0:N2}' -f (Get-Percentile $gpu 0.95))) / $(('{0:N2}' -f $gpu[-1])) |`n"
        }
    }
    $v.frame_table = @"
| Metric | Value |
|---|---|
$renderRows
| Frames sampled | $($d.Count) |
| Frame time p50 / p95 / p99 / max (ms) | $(('{0:N2}' -f (Get-Percentile $d 0.5))) / $(('{0:N2}' -f (Get-Percentile $d 0.95))) / $(('{0:N2}' -f (Get-Percentile $d 0.99))) / $(('{0:N2}' -f $d[-1])) |
| Frames over 16.7 ms (below 60 fps) | $over16 ($(('{0:P1}' -f ($over16 / $d.Count)))) |
| Frames over 33.3 ms (below 30 fps) | $over33 ($(('{0:P1}' -f ($over33 / $d.Count)))) |
| Mean draw calls per frame | $([math]::Round($draw)) |
| Peak scene node count | $nodes |
"@
} else { $v.frame_table = Not-Collected 'no frames sampled' }

# ---- 1.4 google benchmark ---------------------------------------------------------------------
$gb = Read-Json (Join-Path $OutDir 'tier1\gbench.json')
if ($gb) {
    $means = $gb.benchmarks | Where-Object { $_.aggregate_name -eq 'mean' -or -not $_.aggregate_name }
    $rows = foreach ($b in $means) {
        $name = ($b.name -replace '_mean$', '')
        $unit = $b.time_unit
        $extra = @()
        foreach ($c in 'population', 'settlements', 'trips_in_flight', 'snapshot_bytes', 'json_bytes') {
            if ($null -ne $b.$c) { $extra += "$c=$([math]::Round([double]$b.$c))" }
        }
        "| ``$name`` | $(('{0:N1}' -f [double]$b.real_time)) $unit | $(('{0:N1}' -f [double]$b.cpu_time)) $unit | $($b.iterations) | $($extra -join ', ') |"
    }
    $v.gbench_table = $rows -join "`n"
} else { $v.gbench_table = '| _NOT COLLECTED: tier1 produced no gbench.json_ | | | | |' }

# ---- 2 counters ---------------------------------------------------------------------------------
$modePath = Join-Path $OutDir 'tier1\perf-mode.txt'
$vtuneSummary = Get-ChildItem (Join-Path $OutDir 'tier1') -Filter 'vtune-uarch-exploration-summary.csv' -ErrorAction SilentlyContinue | Select-Object -First 1
$mode = if (Test-Path $modePath) { (Get-Content $modePath -Raw).Trim() } else { 'none' }
$v.counter_mode = switch ($mode) {
    'hardware' { 'Linux perf, **hardware counters**' }
    'software' { 'Linux perf, **software events only** (the VM exposes no hardware counters)' }
    default    { 'none collected' }
}
if ($vtuneSummary) { $v.counter_mode += '; VTune uarch-exploration on the native build (see below)' }
$v.counter_note = ''
if ($mode -eq 'software') {
    $probe = Read-Head (Join-Path $OutDir 'tier1\perf-probe.txt') 6
    $v.counter_note = "> **No L1/L2/L3, branch-miss or retired-instruction numbers exist for this run.** Podman on Windows runs in a VM that does not pass the CPU's performance counters through, so ``perf`` reports ``<not supported>``. Section 2 shows software events and cpu-clock hotspots only. To get hardware counters, run tier 1 under VTune on the Windows host (``vtune`` on PATH) or run this pipeline on bare-metal Linux.`n>`n> perf said:`n> ``````n$($probe -replace "`n", "`n> ")`n> ``````"
} elseif ($mode -eq 'none' -and -not $vtuneSummary) {
    $v.counter_note = '> No counter data. Tier 1 did not run or perf was unavailable (tier1/perf-unavailable.txt).'
}

$statPath = Join-Path $OutDir 'tier1\perf-stat.csv'
$counters = @{}
if (Test-Path $statPath) {
    $lines = Get-Content $statPath | Where-Object { $_ -and -not $_.StartsWith('#') }
    $rows = foreach ($line in $lines) {
        $f = $line.Split(',')
        if ($f.Count -ge 3) {
            $value = $f[0]; $event = $f[2]
            if ($value -match '^[0-9.]+$') { $counters[$event] = [double]$value }
            "| ``$event`` | $value | $($f[1]) |"
        }
    }
    $v.perf_stat_table = "| Event | Count | Unit |`n|---|---|---|`n" + ($rows -join "`n")
} else { $v.perf_stat_table = Not-Collected 'no perf-stat.csv' }

if ($vtuneSummary) {
    $v.perf_stat_table += "`n`n**VTune uarch-exploration summary** (`tier1/$($vtuneSummary.Name)`):`n`n``````n$(Read-Head $vtuneSummary.FullName 60)`n``````"
}

function Ratio($num, $den, [string]$fmt) {
    if ($counters.ContainsKey($num) -and $counters.ContainsKey($den) -and $counters[$den] -gt 0) { return ($fmt -f ($counters[$num] / $counters[$den])) }
    return 'not available'
}
$v.ipc = Ratio 'instructions' 'cycles' '{0:N2}'
$v.l1d_miss_rate = Ratio 'L1-dcache-load-misses' 'L1-dcache-loads' '{0:P2}'
$v.llc_miss_rate = Ratio 'LLC-load-misses' 'LLC-loads' '{0:P2}'
$v.branch_miss_rate = Ratio 'branch-misses' 'branches' '{0:P2}'
$v.mpki = if ($counters.ContainsKey('cache-misses') -and $counters.ContainsKey('instructions') -and $counters['instructions'] -gt 0) { '{0:N2}' -f ($counters['cache-misses'] / $counters['instructions'] * 1000) } else { 'not available' }

$top = Read-Head (Join-Path $OutDir 'tier1\perf-top-symbols.txt') 30
$v.hot_symbols = if ($top) { "``````n$top`n``````" } else { Not-Collected 'no perf-top-symbols.txt' }
$group = Read-Head (Join-Path $OutDir 'tier1\perf-group-report.txt') 45
$v.group_report = if ($group) { "``````n$group`n``````" } else { Not-Collected 'needs hardware counters (perf-group-report.txt not produced)' }
$ann = Get-ChildItem (Join-Path $OutDir 'tier1') -Filter 'annotate-*.txt' -ErrorAction SilentlyContinue
$v.annotate_files = if ($ann) { 'Annotated files: ' + (($ann | ForEach-Object { "``tier1/$($_.Name)``" }) -join ', ') } else { Not-Collected 'no annotate output' }

# ---- 3 JS ---------------------------------------------------------------------------------------------
$mitata = Join-Path $OutDir 'tier3\mitata.txt'
if (Test-Path $mitata) {
    $v.mitata_block = "``````n$(Strip-Ansi (Get-Content $mitata -Raw))`n``````"
    $fixture = Join-Path $OutDir 'tier3\fixture.ndjson'
    $v.fixture_summary = if (Test-Path $fixture) { "$((Get-Content $fixture | Measure-Object -Line).Lines) messages, $([math]::Round((Get-Item $fixture).Length / 1KB)) KB (``tier3/fixture.ndjson``)." } else { 'fixture missing.' }
} else { $v.mitata_block = Not-Collected 'tier 3 did not run'; $v.fixture_summary = 'none.' }

# ---- 4 build -----------------------------------------------------------------------------------------------
$bs = Join-Path $OutDir 'tier4\build-summary.txt'
$v.build_summary = if (Test-Path $bs) { "``````n$(Get-Content $bs -Raw)`n``````" } else { Not-Collected 'tier 4 did not run' }
$ct = Join-Path $OutDir 'tier4\compile-times.csv'
if (Test-Path $ct) {
    $rows = Import-Csv $ct | Select-Object -First 15 | ForEach-Object { "| ``$($_.object)`` | $([math]::Round([double]$_.wall_ms / 1000, 1)) |" }
    $v.compile_table = "| Object | Wall seconds |`n|---|---|`n" + ($rows -join "`n")
} else { $v.compile_table = Not-Collected 'no compile-times.csv' }

# ---- 6 comparison ---------------------------------------------------------------------------------------------
$resultsRoot = Split-Path $OutDir -Parent
$previous = Get-ChildItem $resultsRoot -Directory | Where-Object { $_.FullName -ne $OutDir -and $_.Name -lt (Split-Path $OutDir -Leaf) -and (Test-Path (Join-Path $_.FullName 'tier1\gbench.json')) } |
    Sort-Object Name -Descending | Select-Object -First 1
if ($previous -and $gb) {
    $old = Read-Json (Join-Path $previous.FullName 'tier1\gbench.json')
    $oldMean = @{}
    foreach ($b in ($old.benchmarks | Where-Object { $_.aggregate_name -eq 'mean' -or -not $_.aggregate_name })) { $oldMean[($b.name -replace '_mean$', '')] = [double]$b.real_time }
    $rows = foreach ($b in ($gb.benchmarks | Where-Object { $_.aggregate_name -eq 'mean' -or -not $_.aggregate_name })) {
        $name = ($b.name -replace '_mean$', '')
        if ($oldMean.ContainsKey($name) -and $oldMean[$name] -gt 0) {
            $delta = ([double]$b.real_time - $oldMean[$name]) / $oldMean[$name]
            $flag = if ($delta -gt 0.10) { ' **REGRESSION**' } elseif ($delta -lt -0.10) { ' faster' } else { '' }
            "| ``$name`` | $(('{0:N1}' -f $oldMean[$name])) | $(('{0:N1}' -f [double]$b.real_time)) | $(('{0:P1}' -f $delta))$flag |"
        }
    }
    $v.comparison = "Previous run: ``$($previous.Name)``.`n`n| Benchmark | Previous | This run | Change |`n|---|---|---|---|`n" + ($rows -join "`n")
} else { $v.comparison = Not-Collected 'no earlier run with tier1/gbench.json to compare against' }

# ---- fill the template ------------------------------------------------------------------------------------------------
$text = Get-Content $Template -Raw
foreach ($key in $v.Keys) { $text = $text.Replace('{{' + $key + '}}', [string]$v[$key]) }
$leftover = [regex]::Matches($text, '\{\{[a-z0-9_]+\}\}') | ForEach-Object { $_.Value } | Select-Object -Unique
if ($leftover) { Write-Warning ("unfilled placeholders: " + ($leftover -join ', ')) }
$reportPath = Join-Path $OutDir 'report.md'
[System.IO.File]::WriteAllText($reportPath, $text)
Write-Host "report: $reportPath"
