class_name WorldView
extends Node3D

const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")
const BuildingAssets := preload("res://scripts/world/building_assets.gd")

## Renders UniverseCore's authoritative world state as placeholder geometry.
## Ticket #15 scope: full replace on every snapshot (both initial_snapshot
## and world_delta are treated identically here) -- real incremental
## add/update/remove handling is a later ticket once the server-side delta
## work from spec #7 actually ships.
##
## Ticket #17: agents and buildings are wrapped in a pickable Area3D (with
## an explicit CollisionShape3D -- CSG shapes have no collision by default)
## carrying "entity_type"/"entity_id" metadata, so a raycast pick in
## main.gd can identify what was clicked. Settlement markers are not
## pickable; they're not in scope for the inspector.

## Ticket #40: near-player NPCs get real NavigationAgent3D avoidance; the
## bulk of NPCs share one pooled NavmeshManager.request_path() query
## instead of a per-agent avoidance node, per the map's decided split.
const NEAR_PLAYER_RADIUS := 40.0
const MOVE_SPEED_UNITS_PER_SECOND := 3.0
const ACTIVITY_LABEL_HEIGHT := 2.4

var _agent_nodes: Dictionary
var _settlement_nodes: Dictionary
var _building_nodes: Dictionary
var _building_data: Dictionary
var _height_sampler := Callable()

var _navmesh_manager: Node3D
## Per-agent tracking, all keyed by agent id: canonical target position,
## region id, last known current_activity, the activity Label3D, and any
## in-flight movement state.
var _agent_targets: Dictionary
var _agent_region_ids: Dictionary
var _agent_activity: Dictionary
var _agent_labels: Dictionary
var _agent_movement: Dictionary

## Ticket #40: movement must track canonical simulation time, not raw frame
## time -- pausing halts it, the speed multiplier scales it. simulation_status
## messages arrive far less often than every frame, so this is cached and
## applied every _process() tick rather than only reacting to the message.
var _paused := true
var _playback_speed := 1
var _last_effective_delta := 0.0

func _init() -> void:
	_agent_nodes = {}
	_settlement_nodes = {}
	_building_nodes = {}
	_building_data = {}
	_agent_targets = {}
	_agent_region_ids = {}
	_agent_activity = {}
	_agent_labels = {}
	_agent_movement = {}

func set_navmesh_manager(manager: Node3D) -> void:
	_navmesh_manager = manager

func set_simulation_playback(paused: bool, speed: int) -> void:
	_paused = paused
	_playback_speed = max(speed, 1)

## `height_sampler`, if given, is called as (world_x, world_z) -> float and
## used to ground every entity onto the real terrain surface instead of the
## flat height the protocol's settlement-ring placement implies. Optional
## so existing calls/tests that don't care about terrain grounding keep
## working unchanged.
func apply_snapshot(payload: Dictionary, height_sampler: Callable = Callable()) -> void:
	_height_sampler = height_sampler
	_clear_all()
	for agent in payload.get("agents", []):
		_spawn_agent(agent)
	for settlement in payload.get("settlements", []):
		_spawn_settlement(settlement)
	for building in payload.get("buildings", []):
		_spawn_building(building)

## Ticket #7: world_delta is a real diff (addedX/updatedX/removedXIds), not a
## restated full world -- applying it via apply_snapshot() would wipe every
## entity on the very first steady-state tick and never repopulate them,
## since the delta payload carries no "agents"/"settlements"/"buildings"
## keys at all. Upsert added/updated by id, free removed ids, leave
## everything else untouched.
func apply_delta(payload: Dictionary, height_sampler: Callable = Callable()) -> void:
	_height_sampler = height_sampler
	for agent in payload.get("addedAgents", []):
		_upsert_agent(agent)
	for agent in payload.get("updatedAgents", []):
		_upsert_agent(agent)
	for id in payload.get("removedAgentIds", []):
		_remove_agent(int(id))

	for settlement in payload.get("addedSettlements", []):
		_upsert_settlement(settlement)
	for settlement in payload.get("updatedSettlements", []):
		_upsert_settlement(settlement)
	for id in payload.get("removedSettlementIds", []):
		_remove_settlement(int(id))

	for building in payload.get("addedBuildings", []):
		_upsert_building(building)
	for building in payload.get("updatedBuildings", []):
		_upsert_building(building)
	for id in payload.get("removedBuildingIds", []):
		_remove_building(int(id))

func agent_count() -> int:
	return _agent_nodes.size()

func settlement_count() -> int:
	return _settlement_nodes.size()

func building_count() -> int:
	return _building_nodes.size()

## Buildings have no server round-trip for details (there is no
## "building_details" protocol message) -- their full data already arrived
## in the snapshot, so the inspector reads it straight from here, the same
## way the PlayCanvas client's findBuilding() does.
func building_data(id: int) -> Dictionary:
	return _building_data.get(id, {})

func _clear_all() -> void:
	# free(), not queue_free(): this is a full-replace on every snapshot, so
	# the old nodes must be gone from the tree immediately, not on the next
	# idle frame -- a caller applying two snapshots back to back (as the
	# tests do, and as a fast-incoming world_delta could) must see a clean
	# child list right away.
	for node in _agent_nodes.values():
		node.free()
	for node in _settlement_nodes.values():
		node.free()
	for node in _building_nodes.values():
		node.free()
	_agent_nodes.clear()
	_settlement_nodes.clear()
	_building_nodes.clear()
	_building_data.clear()
	_agent_targets.clear()
	_agent_region_ids.clear()
	_agent_activity.clear()
	_agent_labels.clear()
	_agent_movement.clear()

## Ticket #40: unlike settlements/buildings, an existing agent is updated
## in place, not destroyed and respawned -- a destroy+respawn on every
## delta tick (which upserts nearly every alive agent, since the server
## marks an agent "updated" whenever it's present in both snapshots,
## regardless of whether any field actually changed) would wipe in-flight
## movement/path state every single tick, making real movement impossible
## to observe.
func _upsert_agent(agent: Dictionary) -> void:
	var id := int(agent.get("id", 0))
	if _agent_nodes.has(id):
		_update_agent(id, agent)
	else:
		_spawn_agent(agent)

func _remove_agent(id: int) -> void:
	if _agent_nodes.has(id):
		_agent_nodes[id].free()
		_agent_nodes.erase(id)
	_agent_targets.erase(id)
	_agent_region_ids.erase(id)
	_agent_activity.erase(id)
	_agent_labels.erase(id)
	_agent_movement.erase(id)

func _upsert_settlement(settlement: Dictionary) -> void:
	_remove_settlement(int(settlement.get("id", 0)))
	_spawn_settlement(settlement)

func _remove_settlement(id: int) -> void:
	if _settlement_nodes.has(id):
		_settlement_nodes[id].free()
		_settlement_nodes.erase(id)

func _upsert_building(building: Dictionary) -> void:
	_remove_building(int(building.get("id", 0)))
	_spawn_building(building)

func _remove_building(id: int) -> void:
	if _building_nodes.has(id):
		_building_nodes[id].free()
		_building_nodes.erase(id)
	_building_data.erase(id)

func _spawn_agent(agent: Dictionary) -> void:
	var visual := BuildingAssets.make_person(str(agent.get("sex", "Female")))

	var id := int(agent.get("id", 0))
	var area := _make_pickable_area("agent", id, visual, Vector3(1.0, 1.8, 0.7))
	var target := _grounded_position(agent) + _household_spacing_offset_for_person(id)
	var activity := str(agent.get("currentActivity", "Idle"))

	var label := Label3D.new()
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.no_depth_test = true
	label.position = Vector3(0.0, ACTIVITY_LABEL_HEIGHT, 0.0)
	label.text = activity
	area.add_child(label)
	_agent_labels[id] = label

	# A newly-spawned agent renders at its real canonical position
	# immediately, not an invented placement -- movement is only ever a
	# real response to a schedule transition (see _update_agent's Travel
	# handling below), never a cosmetic join-time flourish.
	area.position = target
	add_child(area)
	_agent_nodes[id] = area
	_agent_targets[id] = target
	_agent_region_ids[id] = int(agent.get("regionId", 0))
	_agent_activity[id] = activity

## Ticket #40: refresh the visible activity label and canonical target for
## an already-spawned agent, and trigger a proof-of-wiring walk whenever
## the schedule transitions *into* Travel (not on every tick it happens to
## still be Travel).
func _update_agent(id: int, agent: Dictionary) -> void:
	var target := _grounded_position(agent) + _household_spacing_offset_for_person(id)
	_agent_targets[id] = target
	_agent_region_ids[id] = int(agent.get("regionId", 0))

	var activity := str(agent.get("currentActivity", "Idle"))
	var previous_activity: String = _agent_activity.get(id, "")
	_agent_activity[id] = activity
	if _agent_labels.has(id):
		_agent_labels[id].text = activity

	if activity == "Travel" and previous_activity != "Travel" and not _agent_movement.has(id):
		var node: Node3D = _agent_nodes[id]
		node.position = target + _jitter_offset_for_person(id)
		_request_path_to_target(id)

func _spawn_settlement(settlement: Dictionary) -> void:
	var marker := BuildingAssets.make_settlement_marker()
	marker.position = _grounded_position(settlement)
	add_child(marker)
	_settlement_nodes[int(settlement.get("id", 0))] = marker

func _spawn_building(building: Dictionary) -> void:
	var visual := BuildingAssets.make_building(str(building.get("type", "Dwelling")))

	var id := int(building.get("id", 0))
	var area := _make_pickable_area("building", id, visual, Vector3(3.0, 5.0, 3.0))
	area.position = _grounded_position(building)
	add_child(area)
	_building_nodes[id] = area
	_building_data[id] = building

## The protocol's position.y comes from server-side presentation logic (a
## flat settlement-ring layout, unaware of the client's own procedurally
## generated terrain) -- so it's frequently wrong once real terrain height
## exists. Override y with the actual terrain height at (x, z) whenever a
## sampler is available; otherwise fall back to whatever the protocol sent.
func _grounded_position(entry: Dictionary) -> Vector3:
	var pos := UniverseProtocol.position_from(entry)
	if _height_sampler.is_valid():
		pos.y = _height_sampler.call(pos.x, pos.z)
	return pos

func _make_pickable_area(entity_type: String, entity_id: int, visual: Node3D, collision_extents: Vector3) -> Area3D:
	var area := Area3D.new()
	area.set_meta("entity_type", entity_type)
	area.set_meta("entity_id", entity_id)
	area.collision_layer = 1
	area.collision_mask = 0

	var shape := BoxShape3D.new()
	shape.size = collision_extents
	var collision_shape := CollisionShape3D.new()
	collision_shape.shape = shape
	area.add_child(collision_shape)
	area.add_child(visual)
	return area

## A small, deterministic (per person id, not random-per-frame) offset from
## the canonical target -- gives every agent a real, non-trivial distance to
## walk when proving the pipeline, without inventing a meaningful "place"
## for them to be (per the map's decision, home and work are the same point
## this slice).
func _jitter_offset_for_person(id: int) -> Vector3:
	var angle := float(id % 360) * (PI / 180.0)
	var radius := 3.0 + float(id % 5)
	return Vector3(cos(angle) * radius, 0.0, sin(angle) * radius)

## Every member of a household shares the exact same canonical target
## (Settlement.position + Household.settlement_offset -- there is no
## per-person position in canonical state), so without this, everyone in
## a household renders exactly on top of each other. A small, deterministic
## per-person spread keeps household-mates visually distinct without
## inventing any canonical position they don't actually have.
func _household_spacing_offset_for_person(id: int) -> Vector3:
	# A short id%N cycle collides too often for household-mates (typically
	# 2-6 people) to land on visibly distinct spots, and the radius was too
	# small to read as separated at normal zoom. A multiplicative hash
	# spreads angle across the full circle per id, with radius varying
	# independently so even same-angle collisions still separate visually.
	var angle_hash := (id * 2654435761) % 360
	var radius_hash := (id * 40503) % 100
	var angle := float(angle_hash) * (PI / 180.0)
	var radius := 1.5 + (float(radius_hash) / 100.0) * 2.0
	return Vector3(cos(angle) * radius, 0.0, sin(angle) * radius)

func _is_near_player(pos: Vector3) -> bool:
	var camera := get_viewport().get_camera_3d() if get_viewport() != null else null
	if camera == null:
		return false
	return camera.global_position.distance_to(pos) <= NEAR_PLAYER_RADIUS

## Ticket #40: routes the agent to its stored canonical target, choosing
## the near-player (NavigationAgent3D + avoidance) or pooled
## (NavmeshManager.request_path()) path depending on distance to the
## camera. Falls back to a direct two-point path if the pooled manager
## has no region data yet (e.g. navmesh still baking), so an agent is
## never left permanently stuck rather than merely late.
func _request_path_to_target(id: int) -> void:
	if not _agent_nodes.has(id):
		return
	var node: Node3D = _agent_nodes[id]
	var target: Vector3 = _agent_targets.get(id, node.position)
	if node.position.distance_to(target) < 0.01:
		return

	if _is_near_player(node.position):
		_begin_near_movement(id, node, target)
		return

	var region_id: int = _agent_region_ids.get(id, 0)
	var path := PackedVector3Array()
	if _navmesh_manager != null:
		path = _navmesh_manager.request_path(region_id, node.position, target)
	if path.size() < 2:
		path = PackedVector3Array([node.position, target])
	_agent_movement[id] = {"mode": "far", "path": path, "index": 1}

func _begin_near_movement(id: int, node: Node3D, target: Vector3) -> void:
	var nav_agent := NavigationAgent3D.new()
	nav_agent.avoidance_enabled = true
	nav_agent.path_desired_distance = 0.5
	nav_agent.target_desired_distance = 0.5
	node.add_child(nav_agent)
	nav_agent.target_position = target
	_agent_movement[id] = {"mode": "near", "nav_agent": nav_agent, "target": target}

## Movement progression is driven by _process(delta) scaled by
## _last_effective_delta (canonical simulation time: zero while paused,
## scaled by the speed multiplier otherwise) rather than raw frame time
## directly, so pausing the simulation visibly halts NPC movement and
## changing speed visibly scales it.
func _process(delta: float) -> void:
	_last_effective_delta = 0.0 if _paused else delta * float(_playback_speed)
	if _last_effective_delta <= 0.0:
		return

	for id in _agent_movement.keys().duplicate():
		_advance_agent_movement(id)

func _advance_agent_movement(id: int) -> void:
	if not _agent_nodes.has(id):
		_agent_movement.erase(id)
		return

	var node: Node3D = _agent_nodes[id]
	var state: Dictionary = _agent_movement[id]

	if state.get("mode", "") == "near":
		var nav_agent: NavigationAgent3D = state.get("nav_agent")
		var target: Vector3 = state.get("target", node.position)
		if nav_agent == null or not is_instance_valid(nav_agent):
			_agent_movement.erase(id)
			return
		if node.position.distance_to(target) < 0.05:
			nav_agent.queue_free()
			_agent_movement.erase(id)
			return

		# Move directly rather than depending on NavigationAgent3D's
		# velocity_computed signal to actually update position: that signal
		# only fires once the underlying navigation map has finished
		# synchronizing the agent's avoidance simulation, and relying on it
		# left near-player NPCs permanently stuck if that sync never
		# completed in time. get_next_path_position() falls back to the
		# agent's own current position when the map has no real path yet
		# (a degenerate waypoint); walk straight toward the real target
		# instead of stalling on it.
		var next_pos := nav_agent.get_next_path_position()
		if next_pos.distance_to(node.position) < 0.01:
			next_pos = target
		var to_next := next_pos - node.position
		var distance := to_next.length()
		var step := MOVE_SPEED_UNITS_PER_SECOND * _last_effective_delta
		if distance <= step:
			node.position = next_pos
		else:
			node.position += to_next.normalized() * step
		return

	var path: PackedVector3Array = state.get("path", PackedVector3Array())
	var index: int = state.get("index", 1)
	if index >= path.size():
		_agent_movement.erase(id)
		return

	var dest: Vector3 = path[index]
	var to_dest := dest - node.position
	var distance := to_dest.length()
	var step := MOVE_SPEED_UNITS_PER_SECOND * _last_effective_delta
	if distance <= step:
		node.position = dest
		state["index"] = index + 1
		_agent_movement[id] = state
	else:
		node.position += to_dest.normalized() * step
