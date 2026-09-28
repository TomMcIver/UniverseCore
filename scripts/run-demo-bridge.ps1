param(
    [string]$Scenario = "scenarios/demo.json"
)

$ErrorActionPreference = "Stop"

# Starts the WebSocket bridge the Godot client connects to, seeded from the committed demo scenario.
if (-not (Test-Path $Scenario)) {
    throw "Scenario file not found: $Scenario"
}

$env:UNIVERSE_SCENARIO = $Scenario
node apps/web-client/server/ws-bridge.mjs
