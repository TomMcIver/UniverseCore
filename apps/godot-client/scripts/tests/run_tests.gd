extends SceneTree

const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")
const WorldView := preload("res://scripts/world/world_view.gd")
const UniverseClient := preload("res://scripts/net/universe_client.gd")
const FakeSocket := preload("res://scripts/tests/fake_socket.gd")
const InspectorPanel := preload("res://scripts/ui/inspector_panel.gd")
const EventLog := preload("res://scripts/ui/event_log.gd")
const TerrainGenerator := preload("res://scripts/terrain_generator.gd")
const DayNight := preload("res://scripts/world/day_night.gd")

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
	_run_test("world_view spawns agents/buildings as pickable areas with entity metadata", Callable(self, "_test_world_view_pickable_metadata"))
	_run_test("world_view exposes raw building data for the inspector (no server round trip)", Callable(self, "_test_world_view_building_data"))
	_run_test("inspector panel formats agent details", Callable(self, "_test_inspector_agent_details"))
	_run_test("inspector panel formats building details", Callable(self, "_test_inspector_building_details"))
	_run_test("event log appends and counts entries", Callable(self, "_test_event_log_append"))
	_run_test("terrain finds the nearest region center for a world point", Callable(self, "_test_terrain_nearest_region_id"))
	_run_test("terrain derives region centers from settlement positions", Callable(self, "_test_terrain_region_centers_from_settlements"))
	_run_test("terrain height/color genuinely differ between environment classes", Callable(self, "_test_terrain_sample_varies_by_class"))
	_run_test("terrain color reflects wild-food richness within the same class", Callable(self, "_test_terrain_sample_richness_affects_color"))
	_run_test("day_night is brightest at midday and dimmest at midnight", Callable(self, "_test_day_night_midday_vs_midnight"))
	_run_test("day_night wraps correctly across multiple simulated days", Callable(self, "_test_day_night_wraps_across_days"))

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

func _test_world_view_pickable_metadata() -> void:
	var view := WorldView.new()
	root.add_child(view)
	view.apply_snapshot({
		"agents": [{"id": 5, "sex": "Male", "position": {"x": 1.0, "y": 0.9, "z": 2.0}}],
		"settlements": [],
		"buildings": [{"id": 9, "type": "Granary", "state": "Complete", "position": {"x": 3.0, "y": 0.0, "z": 4.0}}],
	})
	var agent_area: Area3D = view.get_child(0)
	_assert_eq(agent_area.get_meta("entity_type"), "agent")
	_assert_eq(int(agent_area.get_meta("entity_id")), 5)
	var building_area: Area3D = view.get_child(1)
	_assert_eq(building_area.get_meta("entity_type"), "building")
	_assert_eq(int(building_area.get_meta("entity_id")), 9)
	view.queue_free()

func _test_world_view_building_data() -> void:
	var view := WorldView.new()
	root.add_child(view)
	view.apply_snapshot({
		"agents": [],
		"settlements": [],
		"buildings": [{"id": 9, "type": "Granary", "state": "Complete", "progress": 1.0, "position": {}}],
	})
	_assert_eq(view.building_data(9).get("type"), "Granary")
	_assert_eq(view.building_data(404), {})
	view.queue_free()

func _test_inspector_agent_details() -> void:
	var inspector := InspectorPanel.new()
	root.add_child(inspector)
	inspector.show_agent_details({
		"id": 1, "age": 34, "sex": "Female", "occupation": "SubsistenceCultivator",
		"alive": true, "householdId": 1, "settlementId": 4, "regionId": 2,
		"nutrition": 820, "health": 877, "memoryCount": 0, "knowledgeCount": 0,
		"family": {"motherId": 0, "fatherId": 0},
		"settlement": {"active": true, "accessSharePoints": 4082},
	})
	var text: String = inspector._content.text
	_assert_eq(text.find("ID: 1") >= 0, true, "expected ID line")
	_assert_eq(text.find("Age: 34") >= 0, true, "expected Age line")
	_assert_eq(text.find("Mother: Unavailable") >= 0, true, "zero mother id should read Unavailable")
	inspector.queue_free()

func _test_inspector_building_details() -> void:
	var inspector := InspectorPanel.new()
	root.add_child(inspector)
	inspector.show_building_details({
		"id": 9, "type": "Granary", "state": "Complete", "progress": 1.0,
		"settlementId": 4, "regionId": 2, "settlementCommonsOwned": true, "ownerHouseholdId": 0,
	})
	var text: String = inspector._content.text
	_assert_eq(text.find("Type: Granary") >= 0, true, "expected Type line")
	_assert_eq(text.find("Progress: 100%") >= 0, true, "expected Progress line")
	_assert_eq(text.find("Owner household: None") >= 0, true, "zero owner id should read None")
	inspector.queue_free()

func _test_event_log_append() -> void:
	var log := EventLog.new()
	root.add_child(log)
	_assert_eq(log.entry_count(), 0)
	log.append_event({"eventType": "Birth", "entityId": 42})
	_assert_eq(log.entry_count(), 1)
	log.append_event({"eventType": "SettlementFounded"})
	_assert_eq(log.entry_count(), 2)
	log.queue_free()

func _test_terrain_nearest_region_id() -> void:
	var terrain := TerrainGenerator.new()
	var centers := {1: Vector2(0.0, 0.0), 2: Vector2(100.0, 100.0)}
	_assert_eq(terrain._nearest_region_id(Vector2(5.0, 5.0), centers), 1)
	_assert_eq(terrain._nearest_region_id(Vector2(90.0, 92.0), centers), 2)

func _test_terrain_region_centers_from_settlements() -> void:
	var terrain := TerrainGenerator.new()
	var settlements := [
		{"regionId": 1, "position": {"x": 0.0, "z": 0.0}},
		{"regionId": 1, "position": {"x": 10.0, "z": 0.0}},
		{"regionId": 2, "position": {"x": 100.0, "z": 100.0}},
	]
	var centers: Dictionary = terrain._region_centers_from_settlements(settlements)
	_assert_eq(centers[1], Vector2(5.0, 0.0))
	_assert_eq(centers[2], Vector2(100.0, 100.0))

func _test_terrain_sample_varies_by_class() -> void:
	var terrain := TerrainGenerator.new()
	var arable: Dictionary = terrain._region_driven_sample({"environmentClass": "ProductiveArable", "wildFoodCapacity": 100, "wildFoodStock": 50}, 1.0)
	var marginal: Dictionary = terrain._region_driven_sample({"environmentClass": "Marginal", "wildFoodCapacity": 100, "wildFoodStock": 50}, 1.0)
	_assert_eq(arable.height != marginal.height, true, "different environment classes should produce different height")
	_assert_eq(arable.color != marginal.color, true, "different environment classes should produce different color")

func _test_terrain_sample_richness_affects_color() -> void:
	var terrain := TerrainGenerator.new()
	var rich: Dictionary = terrain._region_driven_sample({"environmentClass": "Woodland", "wildFoodCapacity": 100, "wildFoodStock": 100}, 1.0)
	var poor: Dictionary = terrain._region_driven_sample({"environmentClass": "Woodland", "wildFoodCapacity": 100, "wildFoodStock": 0}, 1.0)
	_assert_eq(rich.color != poor.color, true, "wild food richness should affect color even within the same environment class")
	_assert_eq(rich.height, poor.height, "richness should not affect height, only color")

func _test_day_night_midday_vs_midnight() -> void:
	var midday_tick := 0
	var midnight_tick := DayNight.DAY_TICKS / 2
	var midday := DayNight.sample(midday_tick)
	var midnight := DayNight.sample(midnight_tick)
	_assert_eq(midday.sun_energy > midnight.sun_energy, true, "midday should be brighter than midnight")
	_assert_eq(midday.ambient_energy > midnight.ambient_energy, true, "midday ambient should be brighter than midnight")
	_assert_eq(DayNight.daylight_amount(midday_tick) > 0.99, true, "midday daylight amount should be ~1.0")
	_assert_eq(DayNight.daylight_amount(midnight_tick) < 0.01, true, "midnight daylight amount should be ~0.0")

func _test_day_night_wraps_across_days() -> void:
	var tick_day_3_midday := DayNight.DAY_TICKS * 3 + DayNight.DAY_TICKS / 2
	var tick_day_0_midday := DayNight.DAY_TICKS / 2
	_assert_eq(
		is_equal_approx(DayNight.daylight_amount(tick_day_3_midday), DayNight.daylight_amount(tick_day_0_midday)),
		true,
		"the same time-of-day should read the same daylight amount regardless of which day it falls on"
	)
