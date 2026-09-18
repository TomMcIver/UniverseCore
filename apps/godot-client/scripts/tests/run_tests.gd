extends SceneTree

const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")
const WorldView := preload("res://scripts/world/world_view.gd")

## Minimal headless test runner for the Godot client's pure/testable seams
## (UniverseProtocol encode/decode, WorldView spawn/replace logic).
##
## GUT (Godot Unit Test) was spec #14's stated testing choice, but this
## session had no verified path to vendor the GUT addon (no confirmed
## internet-fetch of third-party addon code was attempted for a dependency
## this size). These tests are already isolated, pure assertions with no
## dependency on this runner's shape, so migrating them into GUT test_*.gd
## files later is a mechanical wrapper change, not a rewrite. Flagged here
## rather than silently deviating from the spec.
##
## Run with:
##   Godot_v4.7.2-stable_win64_console.exe --headless --path . --script res://scripts/tests/run_tests.gd

var _failures := 0
var _total := 0
var _current_failed := false

func _initialize() -> void:
	_run_test("command_pause round-trips through encode/decode as type=pause", Callable(self, "_test_command_pause"))
	_run_test("command_set_speed carries the speed field", Callable(self, "_test_command_set_speed"))
	_run_test("command_request_entity_details carries entityId", Callable(self, "_test_command_entity_details"))
	_run_test("message_type/message_payload extract envelope fields correctly", Callable(self, "_test_message_accessors"))
	_run_test("position_from reads x/y/z with a safe zero-vector default", Callable(self, "_test_position_from"))
	_run_test("world_view spawns one node per agent/settlement/building", Callable(self, "_test_world_view_spawns"))
	_run_test("world_view fully replaces nodes on the next snapshot", Callable(self, "_test_world_view_replace"))

	print("\n%d/%d tests passed" % [_total - _failures, _total])
	quit(1 if _failures > 0 else 0)

func _run_test(name: String, fn: Callable) -> void:
	_total += 1
	_current_failed = false
	fn.call()
	if _current_failed:
		_failures += 1
		print("  [FAIL] %s" % name)
	else:
		print("  [ok] %s" % name)

func _assert_eq(actual, expected, context: String = "") -> void:
	if actual != expected:
		_current_failed = true
		print("    [assert] expected %s, got %s%s" % [str(expected), str(actual), (" (%s)" % context) if context != "" else ""])

func _test_command_pause() -> void:
	var decoded: Dictionary = UniverseProtocol.decode_message(UniverseProtocol.encode(UniverseProtocol.command_pause()))
	_assert_eq(decoded.get("type"), "pause")

func _test_command_set_speed() -> void:
	_assert_eq(UniverseProtocol.command_set_speed(4).get("speed"), 4)

func _test_command_entity_details() -> void:
	_assert_eq(UniverseProtocol.command_request_entity_details(7).get("entityId"), 7)

func _test_message_accessors() -> void:
	var msg := {"type": "initial_snapshot", "payload": {"population": 200}}
	_assert_eq(UniverseProtocol.message_type(msg), "initial_snapshot")
	_assert_eq(UniverseProtocol.message_payload(msg).get("population"), 200)
	_assert_eq(UniverseProtocol.message_payload({"type": "hello"}), {})

func _test_position_from() -> void:
	var with_position := UniverseProtocol.position_from({"position": {"x": 1.5, "y": 0.9, "z": -2.0}})
	_assert_eq(with_position, Vector3(1.5, 0.9, -2.0))
	var without_position := UniverseProtocol.position_from({})
	_assert_eq(without_position, Vector3.ZERO)

func _test_world_view_spawns() -> void:
	var view := WorldView.new()
	root.add_child(view)
	view.apply_snapshot({
		"agents": [{"id": 1, "sex": "Female", "position": {"x": 1.0, "y": 0.9, "z": 2.0}}],
		"settlements": [{"id": 4, "position": {"x": 3.0, "y": 0.0, "z": 4.0}}],
		"buildings": [],
	})
	_assert_eq(view.agent_count(), 1)
	_assert_eq(view.settlement_count(), 1)
	_assert_eq(view.building_count(), 0)
	_assert_eq(view.get_child_count(), 2)
	view.queue_free()

func _test_world_view_replace() -> void:
	var view := WorldView.new()
	root.add_child(view)
	view.apply_snapshot({"agents": [{"id": 1, "position": {}}], "settlements": [], "buildings": []})
	_assert_eq(view.agent_count(), 1)
	view.apply_snapshot({"agents": [], "settlements": [], "buildings": []})
	_assert_eq(view.agent_count(), 0)
	_assert_eq(view.get_child_count(), 0)
	_assert_eq(view.get_child_count(), 0)
	view.queue_free()
