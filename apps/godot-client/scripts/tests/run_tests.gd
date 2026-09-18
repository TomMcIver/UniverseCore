extends SceneTree

const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")
const WorldView := preload("res://scripts/world/world_view.gd")
const UniverseClient := preload("res://scripts/net/universe_client.gd")
const FakeSocket := preload("res://scripts/tests/fake_socket.gd")

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
	_run_test("should_forward_message drops world_delta while awaiting a fresh snapshot", Callable(self, "_test_trust_gate"))
	_run_test("client requests a fresh snapshot immediately on connect", Callable(self, "_test_client_requests_snapshot_on_connect"))
	_run_test("client drops a world_delta received before the post-connect snapshot arrives", Callable(self, "_test_client_drops_delta_before_snapshot"))
	_run_test("client forwards world_delta once the post-connect snapshot has arrived", Callable(self, "_test_client_forwards_delta_after_snapshot"))
	_run_test("send_command fails and does not touch the socket while disconnected", Callable(self, "_test_send_command_while_disconnected"))
	_run_test("send_command succeeds and reaches the socket while connected", Callable(self, "_test_send_command_while_connected"))
	_run_test("client reconnects after the retry interval elapses while closed", Callable(self, "_test_client_reconnects_after_interval"))
	_run_test("client emits disconnected exactly once when the connection drops", Callable(self, "_test_client_emits_disconnected_once"))

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
	view.queue_free()

func _test_trust_gate() -> void:
	_assert_eq(UniverseProtocol.should_forward_message("world_delta", true), false)
	_assert_eq(UniverseProtocol.should_forward_message("world_delta", false), true)
	_assert_eq(UniverseProtocol.should_forward_message("initial_snapshot", true), true)
	_assert_eq(UniverseProtocol.should_forward_message("simulation_status", true), true)

func _test_client_requests_snapshot_on_connect() -> void:
	var socket := FakeSocket.new()
	var client := UniverseClient.new(socket)
	socket.ready_state = WebSocketPeer.STATE_OPEN
	client._process(0.016)
	_assert_eq(socket.sent_texts.size(), 1)
	var sent: Dictionary = JSON.parse_string(socket.sent_texts[0])
	_assert_eq(sent.get("type"), "request_snapshot")

func _test_client_drops_delta_before_snapshot() -> void:
	var socket := FakeSocket.new()
	var client := UniverseClient.new(socket)
	var received: Array = []
	client.message_received.connect(func(msg: Dictionary) -> void: received.append(msg))
	socket.ready_state = WebSocketPeer.STATE_OPEN
	client._process(0.016)
	socket.push_incoming(JSON.stringify({"type": "world_delta", "payload": {}}))
	client._process(0.016)
	_assert_eq(received.size(), 0)

func _test_client_forwards_delta_after_snapshot() -> void:
	var socket := FakeSocket.new()
	var client := UniverseClient.new(socket)
	var received: Array = []
	client.message_received.connect(func(msg: Dictionary) -> void: received.append(msg))
	socket.ready_state = WebSocketPeer.STATE_OPEN
	client._process(0.016)
	socket.push_incoming(JSON.stringify({"type": "initial_snapshot", "payload": {"population": 5}}))
	client._process(0.016)
	socket.push_incoming(JSON.stringify({"type": "world_delta", "payload": {"population": 6}}))
	client._process(0.016)
	_assert_eq(received.size(), 2)
	_assert_eq(UniverseProtocol.message_type(received[0]), "initial_snapshot")
	_assert_eq(UniverseProtocol.message_type(received[1]), "world_delta")

func _test_send_command_while_disconnected() -> void:
	var socket := FakeSocket.new()
	var client := UniverseClient.new(socket)
	socket.ready_state = WebSocketPeer.STATE_CLOSED
	var ok: bool = client.send_command(UniverseProtocol.command_pause())
	_assert_eq(ok, false)
	_assert_eq(socket.sent_texts.size(), 0)

func _test_send_command_while_connected() -> void:
	var socket := FakeSocket.new()
	var client := UniverseClient.new(socket)
	socket.ready_state = WebSocketPeer.STATE_OPEN
	var ok: bool = client.send_command(UniverseProtocol.command_pause())
	_assert_eq(ok, true)
	_assert_eq(socket.sent_texts.size(), 1)

func _test_client_reconnects_after_interval() -> void:
	var socket := FakeSocket.new()
	var client := UniverseClient.new(socket)
	socket.ready_state = WebSocketPeer.STATE_CLOSED
	client._process(1.0)
	_assert_eq(socket.connect_call_count, 0)
	client._process(1.5)
	_assert_eq(socket.connect_call_count, 1)

func _test_client_emits_disconnected_once() -> void:
	var socket := FakeSocket.new()
	var client := UniverseClient.new(socket)
	# A boxed counter, not a plain int: GDScript lambdas capture value-typed
	# locals by value, so a bare "var disconnect_count := 0" mutated with
	# "disconnect_count += 1" inside the closure would silently increment a
	# copy and never touch this scope's variable. An Array is a reference
	# type, so mutating its contents from the closure is visible here.
	var disconnect_count := [0]
	client.disconnected.connect(func() -> void: disconnect_count[0] += 1)
	socket.ready_state = WebSocketPeer.STATE_OPEN
	client._process(0.016)
	socket.ready_state = WebSocketPeer.STATE_CLOSED
	client._process(0.5)
	client._process(0.5)
	_assert_eq(disconnect_count[0], 1)
