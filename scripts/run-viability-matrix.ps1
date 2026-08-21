param(
    [Parameter(Mandatory = $true)][int]$SeedStart,
    [Parameter(Mandatory = $true)][int]$SeedEnd,
    [int]$Years = 1000,
    [int]$Population = 200,
    [int]$FoodScale = 100,
    [int]$EffortScale = 100,
    [string]$OutputDir = "build/viability",
    [string]$Image = "universecore-dev",
    [string]$SimBin = "/workspace/build/container/universe_sim",
    [int]$TerminalWindowYears = 30,
    [string]$Horizons = "100,250,500,750,1000"
)

$ErrorActionPreference = "Stop"

function Parse-KeyVals([string]$Line) {
    $dict = @{}
    foreach ($token in ($Line -split ' ')) {
        if ($token -match '^([A-Za-z0-9_]+)=(-?[0-9]+)$') {
            $dict[$matches[1]] = [int64]$matches[2]
        }
    }
    return $dict
}

function Average-Field([System.Collections.ArrayList]$Rows, [string]$Field) {
    if ($Rows.Count -eq 0) { return 0.0 }
    $sum = 0.0
    foreach ($row in $Rows) {
        if ($row.ContainsKey($Field)) { $sum += [double]$row[$Field] }
    }
    return $sum / [double]$Rows.Count
}

function Classify-Extinction([System.Collections.ArrayList]$TerminalRows) {
    if ($TerminalRows.Count -eq 0) { return "OTHER" }

    $avgShortfall = Average-Field $TerminalRows "labour_shortfall"
    $avgFoodStress = Average-Field $TerminalRows "population_under_food_stress"
    $avgMal = Average-Field $TerminalRows "nutrition_under_180"
    $avgHealthLow = Average-Field $TerminalRows "health_under_220"
    $avgShortageHouseholds = Average-Field $TerminalRows "households_in_shortage"
    $avgReproF = Average-Field $TerminalRows "reproductive_females"
    $avgReproM = Average-Field $TerminalRows "reproductive_males"
    $avgFertileHouseholds = Average-Field $TerminalRows "fertile_households"
    $avgPopulation = Average-Field $TerminalRows "population"
    $avgSettlements = Average-Field $TerminalRows "settlements_active"
    $avgSmallestSettlement = Average-Field $TerminalRows "smallest_active_settlement"

    if (($avgReproF -le 0 -or $avgReproM -le 0 -or $avgFertileHouseholds -le 0) -and $avgPopulation -le 20) {
        return "SEX_RATIO_REPRODUCTIVE_BOTTLENECK"
    }
    if (($avgFoodStress -gt 0 -or $avgMal -gt 0 -or $avgShortageHouseholds -gt 0) -and $avgShortfall -gt 0 -and $avgPopulation -gt 20) {
        return "LABOUR_COLLAPSE"
    }
    if ($avgHealthLow -gt 0 -and ($avgFoodStress -gt 0 -or $avgMal -gt 0)) {
        return "HEALTH_COLLAPSE"
    }
    if ($avgFoodStress -gt 0 -or $avgMal -gt 0 -or $avgShortageHouseholds -gt 0) {
        return "RESOURCE_COLLAPSE"
    }
    if ($avgReproF -le 0 -or $avgReproM -le 0 -or $avgFertileHouseholds -le 0) {
        return "SEX_RATIO_REPRODUCTIVE_BOTTLENECK"
    }
    if ($avgPopulation -le 40 -and $avgSettlements -ge 2 -and $avgSmallestSettlement -le 10) {
        return "SETTLEMENT_FRAGMENTATION"
    }
    if ($avgPopulation -le 40) {
        return "DEMOGRAPHIC_ATTRITION"
    }
    return "OTHER"
}

function First-Crossing([System.Collections.ArrayList]$Rows, [int]$Threshold) {
    foreach ($row in $Rows) {
        $p = [int]$row["population"]
        if ($p -gt 0 -and $p -lt $Threshold) {
            return [int]$row["year"]
        }
    }
    return -1
}

function Median-Int([int[]]$Values) {
    if ($Values.Count -eq 0) { return 0 }
    $sorted = $Values | Sort-Object
    $n = $sorted.Count
    if ($n % 2 -eq 1) {
        return [int]$sorted[[int]($n / 2)]
    }
    return [int](($sorted[[int]($n / 2) - 1] + $sorted[[int]($n / 2)]) / 2)
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$horizonList = $Horizons.Split(',') | ForEach-Object { [int]($_.Trim()) } | Where-Object { $_ -gt 0 }

$perSeedPerYear = New-Object System.Collections.ArrayList
$summaries = New-Object System.Collections.ArrayList
$seedRowsMap = @{}

for ($seed = $SeedStart; $seed -le $SeedEnd; $seed++) {
    $args = @(
        "run", "--rm",
        "-v", "${PWD}:/workspace",
        "-w", "/workspace",
        $Image,
        $SimBin,
        "--seed", "$seed",
        "--years", "$Years",
        "--population", "$Population",
        "--report-interval", "1",
        "--food-scale", "$FoodScale",
        "--effort-scale", "$EffortScale"
    )

    $output = podman @args
    $yearLines = $output | Where-Object { $_ -like "year=*" }

    $rows = New-Object System.Collections.ArrayList
    foreach ($line in $yearLines) {
        $parsed = Parse-KeyVals $line
        if ($parsed.Count -eq 0) { continue }
        $rows.Add($parsed) | Out-Null

        if ([int]$parsed["year"] -le 0) {
            continue
        }

        $rowWithSeed = [ordered]@{ seed = $seed }
        foreach ($k in $parsed.Keys) { $rowWithSeed[$k] = $parsed[$k] }
        $perSeedPerYear.Add([pscustomobject]$rowWithSeed) | Out-Null
    }

    if ($rows.Count -eq 0) {
        throw "No yearly rows parsed for seed $seed"
    }

    $seedRowsMap[$seed] = $rows

    $final = $rows[$rows.Count - 1]
    $finalPopulation = [int]$final["population"]
    $survived = if ($finalPopulation -gt 0) { 1 } else { 0 }

    $extinctionYear = -1
    foreach ($row in $rows) {
        if ([int]$row["population"] -eq 0) {
            $extinctionYear = [int]$row["year"]
            break
        }
    }

    $maxPopulation = 0
    $minNonzeroPopulation = [int]::MaxValue
    $settlementsFoundedTotal = 0
    $settlementsLostTotal = 0
    $foodDeficitPeriods = 0
    $labourShortfallPeriods = 0
    $severeMalnutritionPeriods = 0
    $resourcePressurePeriods = 0

    foreach ($row in $rows) {
        $p = [int]$row["population"]
        if ($p -gt $maxPopulation) { $maxPopulation = $p }
        if ($p -gt 0 -and $p -lt $minNonzeroPopulation) { $minNonzeroPopulation = $p }

        $settlementsFoundedTotal += [int]$row["settlements_founded_period"]
        $settlementsLostTotal += [int]$row["settlements_lost_period"]

        if ([int]$row["population_under_food_stress"] -gt 0 -or [int]$row["households_in_shortage"] -gt 0 -or [int]$row["nutrition_under_350"] -gt 0) {
            $foodDeficitPeriods++
        }
        if ([int]$row["labour_shortfall"] -gt 0) { $labourShortfallPeriods++ }
        if ([int]$row["nutrition_under_180"] -gt 0) { $severeMalnutritionPeriods++ }
        if ([int]$row["high_pressure_settlements"] -gt 0) { $resourcePressurePeriods++ }
    }

    if ($minNonzeroPopulation -eq [int]::MaxValue) { $minNonzeroPopulation = 0 }

    $cause = "SURVIVED"
    if ($survived -eq 0) {
        $terminalRows = New-Object System.Collections.ArrayList
        $startYear = if ($extinctionYear -gt 0) { [Math]::Max(0, $extinctionYear - $TerminalWindowYears) } else { [Math]::Max(0, $Years - $TerminalWindowYears) }
        foreach ($row in $rows) {
            $y = [int]$row["year"]
            $p = [int]$row["population"]
            if ($y -ge $startYear -and ($extinctionYear -lt 0 -or $y -le $extinctionYear) -and $p -gt 0) {
                $terminalRows.Add($row) | Out-Null
            }
        }
        if ($terminalRows.Count -eq 0) {
            foreach ($row in $rows) {
                $terminalRows.Add($row) | Out-Null
                if ($terminalRows.Count -ge $TerminalWindowYears) { break }
            }
        }
        $cause = Classify-Extinction $terminalRows
    }

    $summary = [pscustomobject]@{
        seed = $seed
        final_population = $finalPopulation
        survived = $survived
        extinction_year = $extinctionYear
        max_population = $maxPopulation
        min_nonzero_population = $minNonzeroPopulation
        active_settlements_final = [int]$final["settlements_active"]
        settlements_founded_total = $settlementsFoundedTotal
        settlements_lost_total = $settlementsLostTotal
        births_total = [int]$final["births_total"]
        deaths_total = [int]$final["deaths_total"]
        food_deficit_periods = $foodDeficitPeriods
        labour_shortfall_periods = $labourShortfallPeriods
        severe_malnutrition_periods = $severeMalnutritionPeriods
        resource_pressure_periods = $resourcePressurePeriods
        first_below_100 = First-Crossing $rows 100
        first_below_50 = First-Crossing $rows 50
        first_below_25 = First-Crossing $rows 25
        first_below_10 = First-Crossing $rows 10
        cause = $cause
    }

    $summaries.Add($summary) | Out-Null
    Write-Output "seed=$seed final_population=$finalPopulation survived=$survived extinction_year=$extinctionYear cause=$cause"
}

$perSeedPerYear | Export-Csv -Path (Join-Path $OutputDir "per_seed_per_year.csv") -NoTypeInformation
$summaries | Export-Csv -Path (Join-Path $OutputDir "per_seed_summary.csv") -NoTypeInformation

$horizonRows = New-Object System.Collections.ArrayList
foreach ($h in $horizonList) {
    $pops = @()
    foreach ($seed in $seedRowsMap.Keys) {
        $rows = $seedRowsMap[$seed]
        $target = $null
        foreach ($row in $rows) {
            if ([int]$row["year"] -eq $h) {
                $target = $row
                break
            }
        }
        if ($null -eq $target) { $target = $rows[$rows.Count - 1] }
        $pops += [int]$target["population"]
    }

    $sorted = $pops | Sort-Object
    $runs = $sorted.Count
    $survivors = ($sorted | Where-Object { $_ -gt 0 }).Count
    $p10Index = [Math]::Max(0, [int]([Math]::Floor($runs * 0.1)) - 1)
    $p90Index = [Math]::Min($runs - 1, [int]([Math]::Floor($runs * 0.9)))

    $horizonRows.Add([pscustomobject]@{
        horizon_years = $h
        runs = $runs
        survivors = $survivors
        extinctions = $runs - $survivors
        survival_percent = [int](($survivors * 10000) / [Math]::Max(1, $runs))
        median_population = Median-Int $sorted
        p10_population = [int]$sorted[$p10Index]
        p90_population = [int]$sorted[$p90Index]
    }) | Out-Null
}

$horizonRows | Export-Csv -Path (Join-Path $OutputDir "horizon_summary.csv") -NoTypeInformation

$extinctionYears = @($summaries | Where-Object { $_.extinction_year -gt 0 } | ForEach-Object { [int]$_.extinction_year })
$survivorFinalPops = @($summaries | Where-Object { $_.survived -eq 1 } | ForEach-Object { [int]$_.final_population })
$causeGroups = $summaries | Group-Object cause | Sort-Object Name

$summaryPath = Join-Path $OutputDir "summary.txt"
"runs=$($summaries.Count)" | Out-File -FilePath $summaryPath -Encoding utf8
"seed_range=$SeedStart-$SeedEnd" | Out-File -FilePath $summaryPath -Encoding utf8 -Append
"median_extinction_year=$(Median-Int $extinctionYears)" | Out-File -FilePath $summaryPath -Encoding utf8 -Append
"median_survivor_final_population=$(Median-Int $survivorFinalPops)" | Out-File -FilePath $summaryPath -Encoding utf8 -Append
foreach ($row in $horizonRows) {
    "horizon=$($row.horizon_years) survivors=$($row.survivors)/$($row.runs) median_population=$($row.median_population)" | Out-File -FilePath $summaryPath -Encoding utf8 -Append
}
"causes=" | Out-File -FilePath $summaryPath -Encoding utf8 -Append
foreach ($g in $causeGroups) {
    "  $($g.Name): $($g.Count)" | Out-File -FilePath $summaryPath -Encoding utf8 -Append
}

Write-Output "wrote $(Join-Path $OutputDir 'per_seed_per_year.csv')"
Write-Output "wrote $(Join-Path $OutputDir 'per_seed_summary.csv')"
Write-Output "wrote $(Join-Path $OutputDir 'horizon_summary.csv')"
Write-Output "wrote $summaryPath"