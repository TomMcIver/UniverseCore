class_name UniverseProtocol
extends RefCounted

## Pure encode/decode helpers for UniverseCore's engine-neutral JSON-over-
## WebSocket protocol (see include/universe/ui/web_protocol.hpp and
## src/ui/web_protocol.cpp for the authoritative C++ side). No networking
## here on purpose -- this is the test seam per spec #14's testing decisions.

const HOST := "127.0.0.1"
const PORT := 19081

## Simulation seconds (ticks) that pass per real second at playback speed 1x:
## one simulated day (86400 ticks) per 120 real seconds. Must match
## kRealSecondsPerSimulatedDayAtOneX in src/ui_bridge_main.cpp; at Nx both the
## bridge and the client run N times faster.
const SIMULATION_SECONDS_PER_REAL_SECOND_AT_1X := 720.0

## Either override lets parallel bridges (e.g. one per git worktree) avoid the
## default port; UNIVERSE_WS_PORT matches what ws-bridge.mjs reads.
static func url() -> String:
	var port := PORT
	for name in ["UNIVERSECORE_WS_PORT", "UNIVERSE_WS_PORT"]:
		var port_env := OS.get_environment(name)
		if port_env.is_valid_int():
			port = int(port_env)
			break
	return "ws://%s:%d" % [HOST, port]

## -- Outbound commands (client -> UniverseCore) --
## Field names/shapes must match parse_client_command in web_protocol.cpp
## exactly: "type" plus "speed" (set_speed) or "kind"/"entityId" (request_view_model).

static func command_hello() -> Dictionary:
	return {"type": "hello"}

static func command_pause() -> Dictionary:
	return {"type": "pause"}

static func command_resume() -> Dictionary:
	return {"type": "resume"}

static func command_request_snapshot() -> Dictionary:
	return {"type": "request_snapshot"}

static func command_single_step() -> Dictionary:
	return {"type": "single_step"}

static func command_set_speed(speed: int) -> Dictionary:
	return {"type": "set_speed", "speed": speed}

static func command_request_view_model(kind: String, entity_id: int) -> Dictionary:
	return {"type": "request_view_model", "kind": kind, "entityId": entity_id}

## Runs the simulation at jump speed and pauses at the start of `year`.
static func command_run_until_year(year: int) -> Dictionary:
	return {"type": "run_until_year", "year": year}

## Canonical ticks are seconds; a simulated year is 365 days.
const TICKS_PER_YEAR := 365 * 24 * 60 * 60

static func year_of_tick(tick: int) -> int:
	return int(tick / TICKS_PER_YEAR) if tick > 0 else 0

static func encode(command: Dictionary) -> String:
	return JSON.stringify(command)

## -- Inbound messages (UniverseCore -> client) --
## Envelope shape: {"protocolVersion":N,"type":"...","tick":N,"payload":{...}}
## (tick is absent only on "hello"). See make_*_message in web_protocol.cpp.

static func decode_message(text: String) -> Dictionary:
	var parsed = JSON.parse_string(text)
	if typeof(parsed) != TYPE_DICTIONARY:
		return {}
	return parsed

static func message_type(message: Dictionary) -> String:
	return String(message.get("type", ""))

static func message_payload(message: Dictionary) -> Dictionary:
	var payload = message.get("payload", {})
	if typeof(payload) != TYPE_DICTIONARY:
		return {}
	return payload

## Reconnect trust gate (ticket #16): never trust a world_delta as a valid
## continuation until a fresh initial_snapshot has actually arrived on the
## current connection. Every other message type always forwards.
static func should_forward_message(message_type: String, awaiting_initial_snapshot: bool) -> bool:
	if message_type == "world_delta" and awaiting_initial_snapshot:
		return false
	return true

static func position_from(entry: Dictionary) -> Vector3:
	var pos: Dictionary = entry.get("position", {})
	return Vector3(
		float(pos.get("x", 0.0)),
		float(pos.get("y", 0.0)),
		float(pos.get("z", 0.0))
	)
