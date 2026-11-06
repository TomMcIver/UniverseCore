extends Node

## Automated gameplay for performance runs. Env-gated by UNIVERSECORE_PERF_DIR and
## run against a live bridge started from the demo scenario (the perf pipeline
## starts it). Drives the real client through a fixed script so two runs do the
## same thing, then writes the PerfZones CSV/JSON files and quits.
##
## UNIVERSECORE_PERF_SECONDS (default 30) is the measured window after connecting:
##   first third   -  4x, watching the village
##   second third  -  a 20-year jump (the heaviest bridge traffic)
##   last third    -  8x with a settlement economy panel open
##
## Prints PERF_RUN lines the pipeline parses; exits 0 on success, 1 if it never
## connected, so a failed run cannot be mistaken for a fast one.

const CONNECT_TIMEOUT_SECONDS := 30.0
const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")

var _dir := ""
var _panels: Node
var _world_view: Node3D
var _measuring := false

func run(dir: String, panels: Node, world_view: Node3D) -> void:
	_dir = dir
	_panels = panels
	_world_view = world_view
	var window := float(OS.get_environment("UNIVERSECORE_PERF_SECONDS")) if OS.get_environment("UNIVERSECORE_PERF_SECONDS") != "" else 30.0

	var waited := 0.0
	while _world_view.active_settlement_ids().is_empty() and waited < CONNECT_TIMEOUT_SECONDS:
		await get_tree().process_frame
		waited += get_process_delta_time()
	if _world_view.active_settlement_ids().is_empty():
		push_error("PERF_RUN: never received a world after %s s" % CONNECT_TIMEOUT_SECONDS)
		get_tree().quit(1)
		return

	var hud = _find_panel("Hud")
	var settlement_id: int = _world_view.active_settlement_ids()[0]
	print("PERF_RUN start window_s=%.0f agents=%d settlements=%d" % [window, _world_view.agent_count(), _world_view.active_settlement_ids().size()])

	_measuring = true
	var started := Time.get_ticks_msec()
	hud.action_requested.emit("set_speed", UniverseProtocol.command_set_speed(4))
	hud.action_requested.emit("resume", UniverseProtocol.command_resume())
	await _wait_until(started, window * 1.0 / 3.0)

	hud.request_jump()
	await _wait_until(started, window * 2.0 / 3.0)

	_panels.select("settlement", settlement_id)
	hud.action_requested.emit("set_speed", UniverseProtocol.command_set_speed(8))
	await _wait_until(started, window)

	_measuring = false
	var summary := PerfZones.flush(_dir)
	print("PERF_RUN done elapsed_s=%.1f zones=%d round_trips=%d frames=%d" % [
		float(Time.get_ticks_msec() - started) / 1000.0,
		(summary.get("zones", {}) as Dictionary).size(),
		int(summary.get("bridge_round_trips", 0)),
		int(summary.get("frames_sampled", 0)),
	])
	get_tree().quit(0)

func _process(delta: float) -> void:
	if _measuring:
		PerfZones.sample_frame(delta)

func _wait_until(started_msec: int, seconds: float) -> void:
	while float(Time.get_ticks_msec() - started_msec) / 1000.0 < seconds:
		await get_tree().process_frame

func _find_panel(panel_name: String) -> Node:
	for panel in _panels.panels():
		if panel.name == panel_name:
			return panel
	return null
