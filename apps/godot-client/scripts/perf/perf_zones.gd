class_name PerfZones
extends RefCounted

## Timing zones for the GDScript half of the client, plus the round-trip time of
## every command that crosses the bridge to the C++ simulation.
##
## Why not Tracy here: Godot has no GDScript binding for Tracy, and this client
## is a separate process from the C++ simulation (a WebSocket bridge, not a
## GDExtension), so there is no in-process C++/GDScript boundary to trace. The
## honest cross-language signals are (1) these zones, (2) bridge round trips,
## which include the C++ command handling, and (3) the server tick stamped on
## every row so a GDScript sample can be lined up with C++ output by simulated
## time. C++ internals are measured separately (Tracy zones, perf, benchmarks).
##
## Off unless UNIVERSECORE_PERF_DIR is set. When off, begin() returns at once and
## end() is a no-op, so instrumented code costs one static call and a branch.
##
## Usage:
##   var t := PerfZones.begin()
##   ... work ...
##   PerfZones.end("world_view.advance_agents", t)

const MAX_ROWS := 400000
## The response message that closes the round trip for each command.
const RESPONSE_FOR := {
	"request_snapshot": "initial_snapshot",
	"resume": "simulation_status",
	"pause": "simulation_status",
	"set_speed": "simulation_status",
	"single_step": "simulation_status",
	"run_until_year": "simulation_status",
}

static var _enabled := OS.get_environment("UNIVERSECORE_PERF_DIR") != ""
static var _origin_usec := Time.get_ticks_usec()
static var _origin_unix := Time.get_unix_time_from_system()
static var _server_tick := 0
static var _zone_rows: PackedStringArray = PackedStringArray()
static var _frame_rows: PackedStringArray = PackedStringArray()
static var _rtt_rows: PackedStringArray = PackedStringArray()
static var _pending_commands: Dictionary = {}  # response type -> [command, sent_usec]
static var _stats: Dictionary = {}  # zone -> PackedFloat64Array of durations (us)

static func enabled() -> bool:
	return _enabled

## Start of a zone, in microseconds; 0 when profiling is off.
static func begin() -> int:
	if not _enabled:
		return 0
	return Time.get_ticks_usec()

static func end(zone: String, started_usec: int) -> void:
	if not _enabled:
		return
	var now := Time.get_ticks_usec()
	var duration := now - started_usec
	# Packed arrays are copied on read, so the appended array must be stored back.
	var durations: PackedFloat64Array = _stats.get(zone, PackedFloat64Array())
	durations.append(float(duration))
	_stats[zone] = durations
	if _zone_rows.size() < MAX_ROWS:
		_zone_rows.append("%s,%d,%d,%d,%d" % [zone, started_usec - _origin_usec, duration, _server_tick, Engine.get_process_frames()])

static func set_server_tick(tick: int) -> void:
	_server_tick = tick

## Call when a command is sent to the bridge.
static func command_sent(command_type: String) -> void:
	if not _enabled or not RESPONSE_FOR.has(command_type):
		return
	# One FIFO per response type: commands sent before the first reply arrives
	# each get their own round trip instead of overwriting each other.
	var response: String = RESPONSE_FOR[command_type]
	var queue: Array = _pending_commands.get(response, [])
	queue.append([command_type, Time.get_ticks_usec()])
	_pending_commands[response] = queue

## Call for every message received from the bridge.
static func message_received(message_type: String, bytes: int) -> void:
	if not _enabled:
		return
	var queue: Array = _pending_commands.get(message_type, [])
	if queue.is_empty():
		return
	var pending: Array = queue.pop_front()
	var rtt := Time.get_ticks_usec() - int(pending[1])
	_rtt_rows.append("%s,%s,%d,%d,%d" % [pending[0], message_type, int(pending[1]) - _origin_usec, rtt, bytes])

static var _render_viewport := RID()

## Turns on Godot's own CPU/GPU render timing for a viewport. Without this a
## slow frame cannot be told apart as script-bound or GPU-bound. GPU time needs
## driver timestamp-query support; when it is missing Godot reports 0 and the
## report says so rather than treating 0 as fast.
static func enable_render_timing(viewport: Viewport) -> void:
	if not _enabled:
		return
	_render_viewport = viewport.get_viewport_rid()
	RenderingServer.viewport_set_measure_render_time(_render_viewport, true)

## One row per rendered frame from Godot's own monitors.
static func sample_frame(delta: float) -> void:
	if not _enabled or _frame_rows.size() >= MAX_ROWS:
		return
	var render_cpu_ms := 0.0
	var render_gpu_ms := 0.0
	if _render_viewport.is_valid():
		render_cpu_ms = RenderingServer.viewport_get_measured_render_time_cpu(_render_viewport)
		render_gpu_ms = RenderingServer.viewport_get_measured_render_time_gpu(_render_viewport)
	_frame_rows.append("%d,%d,%.3f,%.0f,%.3f,%.3f,%d,%d,%d,%d,%.3f,%.3f" % [
		Time.get_ticks_usec() - _origin_usec,
		_server_tick,
		delta * 1000.0,
		Performance.get_monitor(Performance.TIME_FPS),
		Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
		Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0,
		int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT)),
		int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)),
		int(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME)),
		int(Performance.get_monitor(Performance.MEMORY_STATIC)),
		render_cpu_ms,
		render_gpu_ms,
	])

static func _percentile(sorted_values: PackedFloat64Array, fraction: float) -> float:
	if sorted_values.is_empty():
		return 0.0
	var index := clampi(int(ceil(fraction * sorted_values.size())) - 1, 0, sorted_values.size() - 1)
	return sorted_values[index]

## Per-zone summary: count, total, mean, p50, p95, p99 and max in microseconds.
static func summary() -> Dictionary:
	var zones := {}
	for zone in _stats.keys():
		var values: PackedFloat64Array = _stats[zone].duplicate()
		values.sort()
		var total := 0.0
		for value in values:
			total += value
		zones[zone] = {
			"count": values.size(),
			"total_us": total,
			"mean_us": total / maxf(values.size(), 1.0),
			"p50_us": _percentile(values, 0.50),
			"p95_us": _percentile(values, 0.95),
			"p99_us": _percentile(values, 0.99),
			"max_us": values[values.size() - 1] if values.size() > 0 else 0.0,
		}
	return zones

static func _write(path: String, header: String, rows: PackedStringArray) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("PerfZones: cannot write %s (error %d)" % [path, FileAccess.get_open_error()])
		return
	file.store_line(header)
	for row in rows:
		file.store_line(row)
	file.close()

## Writes the four output files into `dir` and returns the summary.
static func flush(dir: String) -> Dictionary:
	if not _enabled:
		return {}
	_write(dir.path_join("gdscript_zones.csv"), "zone,start_us,duration_us,server_tick,frame", _zone_rows)
	_write(dir.path_join("godot_frames.csv"), "t_us,server_tick,delta_ms,fps,process_ms,physics_ms,nodes,draw_calls,primitives,static_mem_bytes,render_cpu_ms,render_gpu_ms", _frame_rows)
	_write(dir.path_join("bridge_rtt.csv"), "command,response,sent_us,rtt_us,response_bytes", _rtt_rows)
	var result := {
		"origin_unix": _origin_unix,
		"godot_version": Engine.get_version_info()["string"],
		"frames_sampled": _frame_rows.size(),
		"bridge_round_trips": _rtt_rows.size(),
		"zones": summary(),
	}
	var file := FileAccess.open(dir.path_join("gdscript_summary.json"), FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(result, "  "))
		file.close()
	return result
