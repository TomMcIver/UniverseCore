# Shared helpers for the performance pipeline. Dot-source this file.
# Written for Windows PowerShell 5.1 (no ternary, no &&, no ArgumentList on ProcessStartInfo).

function Format-NativeArgument([string]$Value) {
    if ($Value -match '[\s"]' -or $Value.Length -eq 0) {
        return '"' + ($Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
    }
    return $Value
}

# Runs a native program, captures stdout and stderr to $LogFile, and returns
# @{ ExitCode; Stdout; Stderr; Seconds; TimedOut }. Never throws on a non-zero exit.
# -StdIn text is sent with CRLF normalised to LF, so scripts checked out with
# CRLF still run under bash inside the container.
function Invoke-Native {
    param(
        [Parameter(Mandatory)][string]$File,
        [string[]]$Arguments = @(),
        [string]$LogFile = '',
        [string]$StdIn = $null,
        [string]$WorkingDirectory = (Get-Location).Path,
        [int]$TimeoutSeconds = 0,
        [hashtable]$Environment = @{}
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.Arguments = (($Arguments | ForEach-Object { Format-NativeArgument $_ }) -join ' ')
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = ($null -ne $StdIn)
    $psi.CreateNoWindow = $true
    foreach ($key in $Environment.Keys) { $psi.EnvironmentVariables[$key] = [string]$Environment[$key] }

    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi
    [void]$process.Start()
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if ($null -ne $StdIn) {
        # Write raw UTF-8 bytes: StandardInput.Write would prepend a byte-order mark under
        # .NET Framework, which bash then tries to execute as part of line 1.
        $bytes = (New-Object System.Text.UTF8Encoding($false)).GetBytes(($StdIn -replace "`r`n", "`n"))
        $process.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
        $process.StandardInput.BaseStream.Flush()
        $process.StandardInput.Close()
    }
    $timedOut = $false
    if ($TimeoutSeconds -gt 0) {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            try { $process.Kill() } catch { }
            $process.WaitForExit()
        }
    } else {
        $process.WaitForExit()
    }
    $clock.Stop()
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    if ($LogFile -ne '') {
        $text = $stdout
        if ($stderr.Length -gt 0) { $text += "`n--- stderr ---`n" + $stderr }
        [System.IO.File]::WriteAllText($LogFile, $text)
    }
    return @{
        ExitCode = $process.ExitCode
        Stdout   = $stdout
        Stderr   = $stderr
        Seconds  = [math]::Round($clock.Elapsed.TotalSeconds, 1)
        TimedOut = $timedOut
    }
}

function Find-Tool([string[]]$Names) {
    foreach ($name in $Names) {
        $command = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { return $command.Source }
    }
    return $null
}

# Per-tier outcome, written to status.json so a report can never silently omit
# a tier: every tier is ok, skipped (with the reason) or failed (with the reason).
$script:TierStatus = [ordered]@{}

function Set-TierStatus([string]$Tier, [string]$State, [string]$Detail = '') {
    $script:TierStatus[$Tier] = [ordered]@{ state = $State; detail = $Detail }
    Write-Host ("[{0}] {1}{2}" -f $Tier, $State.ToUpper(), $(if ($Detail) { ' - ' + $Detail } else { '' }))
}

function Save-TierStatus([string]$Path) {
    ($script:TierStatus | ConvertTo-Json -Depth 4) | Set-Content -Path $Path -Encoding UTF8
}

function ConvertTo-PodmanPath([string]$Path) {
    return ($Path -replace '\\', '/')
}
