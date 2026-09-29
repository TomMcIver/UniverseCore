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
##
## Ticket #56 (route-based villager movement, ADR 0001): a villager's
## position is presentation state computed here from canonical facts sent by
## the server (home position, activity target kind, daily depart/return
## ticks) -- there is no server-side path/point list and no client-side
## random spread or NavigationAgent3D for villagers any more. Geometry
## ownership is split on purpose: Common/Woodland/Quarry/Workshop targets are
## real points the server already knows and sends; a cultivator's Field
## target is whatever VillageDressing (set_village_dressing) already laid
## out for that household, looked up here rather than recomputed, so what's
## walked can never disagree with what's drawn. The avatar (a separate
## ticket) collides with these same Area3D nodes, whose root keeps moving in
## place -- movement here must never replace or re-parent that node.

## A villager's estimated current simulation tick is advanced locally between
## server messages (see _process), corrected to the server's real tick on
## every snapshot/delta so drift never accumulates. This is a client-local
## approximation only: the server has no sub-day ticking, so this is the only
## way a walk is visible before the next multi-day jump.
const WALK_SPEED_UNITS_PER_SECOND := 1.3
## Above this playback speed, walking is not drawn at all -- villagers snap
## straight to their current activity spot (decision M4).
const WALKING_VISIBLE_MAX_SPEED := 8
const ACTIVITY_LABEL_HEIGHT := 2.4
const HIGHLIGHT_HEIGHT := 3.3
const HIGHLIGHT_NODE_NAME := "Highlight"
## Construction progress is shown in this many visible wall-height steps.
const CONSTRUCTION_VISUAL_STEPS := 10

var _agent_nodes: Dictionary
## The one villager carrying the floating marker (0 = none); survives respawns.
var _highlighted_agent_id := 0
var _settlement_nodes: Dictionary
var _building_nodes: Dictionary
var _building_data: Dictionary
var _height_sampler := Callable()
var _village_dressing: Node3D

## Per-agent presentation facts, keyed by agent id: household/region/target
## kind/target point/schedule, plus the activity Label3D.
var _agent_facts: Dictionary
var _agent_labels: Dictionary

## Ticket #40/#56: movement must track canonical simulation time, not raw
## frame time -- pausing halts it, the speed multiplier scales it.
## simulation_status messages arrive far less often than every frame, so the
## playback state is cached and applied every _process() tick, and the
## estimated tick is corrected to the server's real tick whenever a fresh
## message carries one.
var _paused := true
var _playback_speed := 1
var _estimated_tick := 0.0

func _init() -> void:
	_agent_nodes = {}
	_settlement_nodes = {}
	_building_nodes = {}
	_building_data = {}
	_agent_facts = {}
	_agent_labels = {}

func set_navmesh_manager(_manager: Node3D) -> void:
	pass  # Villagers no longer path via the navmesh; kept only so main.gd's existing wiring is harmless.

func set_village_dressing(dressing: Node3D) -> void:
	_village_dressing = dressing

func set_simulation_playback(paused: bool, speed: int) -> void:
	_paused = paused
	_playback_speed = max(speed, 1)

## Corrects the locally-estimated tick to the server's real tick. Called
## whenever a message (snapshot, delta or status) carries one, so drift from
## the frame-time approximation below never accumulates across many messages.
func set_tick(tick: int) -> void:
	_estimated_tick = float(tick)

## `height_sampler`, if given, is called as (world_x, world_z) -> float and
## used to ground every entity onto the real terrain surface. Optional so
## existing calls/tests that don't care about terrain grounding keep working
## unchanged.
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
	_agent_facts.clear()
	_agent_labels.clear()

## An existing agent is updated in place, not destroyed and respawned -- a
## destroy+respawn on every delta tick (which upserts nearly every alive
## agent, since the server marks an agent "updated" whenever it's present in
## both snapshots, regardless of whether any field actually changed) would be
## needlessly churny and would restart the activity label/visual each time.
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
	_agent_facts.erase(id)
	_agent_labels.erase(id)

## Same churn fix as buildings (E1): settlements arrive as "updated" in
## every delta, so only respawn when the firepit would look different.
func _upsert_settlement(settlement: Dictionary) -> void:
	var id := int(settlement.get("id", 0))
	var active := bool(settlement.get("active", true))
	if _settlement_nodes.has(id) and bool(_settlement_nodes[id].get_meta("active", true)) == active:
		return
	_remove_settlement(id)
	_spawn_settlement(settlement)

func _remove_settlement(id: int) -> void:
	if _settlement_nodes.has(id):
		_settlement_nodes[id].free()
		_settlement_nodes.erase(id)

## Live-client defect E1: the server lists every present building as
## "updated" in every delta, and this used to free and respawn each one ten
## times a second. Only a change in how the building looks (type, state,
## visible construction step, position) rebuilds it now; anything else just
## refreshes the inspector data.
func _upsert_building(building: Dictionary) -> void:
	var id := int(building.get("id", 0))
	if _building_nodes.has(id) and _building_look_key(_building_data.get(id, {})) == _building_look_key(building):
		_building_data[id] = building
		return
	_remove_building(id)
	_spawn_building(building)

func _building_look_key(building: Dictionary) -> String:
	var step := int(floor(float(building.get("progress", 1.0)) * float(CONSTRUCTION_VISUAL_STEPS)))
	var pos := UniverseProtocol.position_from(building)
	return "%s|%s|%d|%.2f|%.2f" % [building.get("type", ""), building.get("state", ""), step, pos.x, pos.z]

func _remove_building(id: int) -> void:
	if _building_nodes.has(id):
		_building_nodes[id].free()
		_building_nodes.erase(id)
	_building_data.erase(id)

## The agent facts kept for movement/positioning, extracted once per
## snapshot/delta message rather than every frame.
func _agent_facts_from(agent: Dictionary) -> Dictionary:
	var target_raw = agent.get("target")
	var target = null
	if typeof(target_raw) == TYPE_DICTIONARY:
		target = Vector2(float(target_raw.get("x", 0.0)), float(target_raw.get("z", 0.0)))
	var home_pos := UniverseProtocol.position_from(agent)
	return {
		"household_id": int(agent.get("householdId", 0)),
		"region_id": int(agent.get("regionId", 0)),
		"home": Vector2(home_pos.x, home_pos.z),
		"target_kind": str(agent.get("targetKind", "Home")),
		"target": target,
		"depart_tick": int(agent.get("departTick", 0)),
		"return_depart_tick": int(agent.get("returnDepartTick", 0)),
	}

func _spawn_agent(agent: Dictionary) -> void:
	var visual := BuildingAssets.make_person(str(agent.get("sex", "Female")))

	var id := int(agent.get("id", 0))
	var area := _make_pickable_area("agent", id, visual, Vector3(1.0, 1.8, 0.7))
	var activity := str(agent.get("currentActivity", "Idle"))

	var label := Label3D.new()
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.no_depth_test = true
	label.position = Vector3(0.0, ACTIVITY_LABEL_HEIGHT, 0.0)
	label.text = activity
	area.add_child(label)
	_agent_labels[id] = label

	_agent_facts[id] = _agent_facts_from(agent)
	area.position = _resolve_agent_position(id)
	add_child(area)
	_agent_nodes[id] = area
	_attach_highlight_if_marked(id)

func _update_agent(id: int, agent: Dictionary) -> void:
	_agent_facts[id] = _agent_facts_from(agent)
	if _agent_labels.has(id):
		_agent_labels[id].text = str(agent.get("currentActivity", "Idle"))

func _spawn_settlement(settlement: Dictionary) -> void:
	var id := int(settlement.get("id", 0))
	var active := bool(settlement.get("active", true))
	var area := _make_pickable_area("settlement", id, BuildingAssets.make_settlement_marker(active), Vector3(2.0, 4.0, 2.0))
	area.set_meta("active", active)
	area.position = _grounded_position(settlement)
	add_child(area)
	_settlement_nodes[id] = area

func _spawn_building(building: Dictionary) -> void:
	var building_type := str(building.get("type", "Dwelling"))
	var state := str(building.get("state", "Complete"))
	var progress := float(building.get("progress", 1.0))
	var visual := BuildingAssets.make_building(building_type, state, progress)
	var size := BuildingAssets.footprint(building_type, state, progress)

	var id := int(building.get("id", 0))
	var area := _make_pickable_area("building", id, visual, Vector3(size.x, max(size.y, 1.5), size.z))
	(area.get_child(0) as CollisionShape3D).position.y = max(size.y, 1.5) * 0.5
	var body := BuildingAssets.make_collision_body(building_type, state, progress)
	if body != null:
		area.add_child(body)
	area.position = _building_ground_position(building, size)
	area.rotation.y = _building_rotation(building)
	add_child(area)
	_building_nodes[id] = area
	_building_data[id] = building

## Door (local +X) along the canonical facing, where the simulation's angle
## points along (cos, sin) in world x/z; a Y rotation of -angle maps +X there.
## Older bridges without facingDegrees fall back to facing the common.
func _building_rotation(building: Dictionary) -> float:
	if building.has("facingDegrees"):
		return -deg_to_rad(float(building.get("facingDegrees", 0)))
	return _facing_settlement_centre(building)

func _facing_settlement_centre(building: Dictionary) -> float:
	var settlement_id := int(building.get("settlementId", 0))
	if not _settlement_nodes.has(settlement_id):
		return 0.0
	var centre: Vector3 = _settlement_nodes[settlement_id].position
	var pos := UniverseProtocol.position_from(building)
	var direction := Vector2(centre.x - pos.x, centre.z - pos.z)
	if direction.length() < 0.01:
		return 0.0
	return atan2(-direction.y, direction.x)

## Sits the building on the lowest ground under its footprint so no corner
## floats on a slope.
func _building_ground_position(building: Dictionary, size: Vector3) -> Vector3:
	var pos := UniverseProtocol.position_from(building)
	if not _height_sampler.is_valid():
		return pos
	var half := Vector2(size.x, size.z).length() * 0.5
	var lowest := INF
	for offset in [Vector2.ZERO, Vector2(half, 0.0), Vector2(-half, 0.0), Vector2(0.0, half), Vector2(0.0, -half)]:
		lowest = min(lowest, float(_height_sampler.call(pos.x + offset.x, pos.z + offset.y)))
	pos.y = lowest - 0.05
	return pos

## Terrain reshaped (a settlement was founded or removed nearby): re-seat
## everything on the new ground. Only heights change; villager x/z are
## recomputed every frame from schedule state regardless.
func reground(area: Rect2) -> void:
	if not _height_sampler.is_valid():
		return
	for id in _building_nodes.keys():
		var node: Node3D = _building_nodes[id]
		if area.has_point(Vector2(node.position.x, node.position.z)):
			var building: Dictionary = _building_data[id]
			var size := BuildingAssets.footprint(str(building.get("type", "Dwelling")), str(building.get("state", "Complete")), float(building.get("progress", 1.0)))
			node.position.y = _building_ground_position(building, size).y
	for node in _settlement_nodes.values():
		if area.has_point(Vector2(node.position.x, node.position.z)):
			node.position.y = float(_height_sampler.call(node.position.x, node.position.z))

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

## A small, deterministic (per person id, not random-per-frame) offset so
## household-mates or co-workers standing at the same slot don't overlap.
## Decision M3: fixed per-person side-offset, no live dodging.
func _crowd_offset_for_person(id: int) -> Vector2:
	var angle_hash := (id * 2654435761) % 360
	var radius_hash := (id * 40503) % 100
	var angle := float(angle_hash) * (PI / 180.0)
	var radius := 0.8 + (float(radius_hash) / 100.0) * 1.4
	return Vector2(cos(angle) * radius, sin(angle) * radius)

## A cultivator's field is laid out by VillageDressing per household (it owns
## that geometry -- see the class comment); look it up rather than
## recomputing it, so a walked-to field can never disagree with a drawn one.
func _field_target_for(facts: Dictionary) -> Vector2:
	if _village_dressing == null:
		return facts["home"]
	var household_id: int = facts["household_id"]
	for field in _village_dressing.fields_in_region(int(facts["region_id"])):
		if int(field.get("household_id", -1)) == household_id:
			return field["centre"]
	return facts["home"]  # No field placed for this household yet; stay home rather than walk to an invented spot.

func _work_point_for(id: int, facts: Dictionary) -> Vector2:
	var kind: String = facts["target_kind"]
	if kind == "Home":
		return facts["home"]
	if kind == "Field":
		return _field_target_for(facts)
	var target = facts["target"]
	return target if target != null else facts["home"]

## Resolves where an agent should be drawn right now, from schedule facts
## plus the locally-estimated tick. Called every frame for every visible
## agent (cheap arithmetic; the field lookup above is the only per-frame walk
## over other data, bounded by one settlement's field count).
func _resolve_agent_position(id: int) -> Vector3:
	var facts: Dictionary = _agent_facts.get(id, {})
	if facts.is_empty():
		return Vector3.ZERO
	var home: Vector2 = facts["home"] + _crowd_offset_for_person(id)
	var kind: String = facts["target_kind"]
	var flat: Vector2
	if kind == "Home":
		flat = home
	else:
		var work: Vector2 = _work_point_for(id, facts) + _crowd_offset_for_person(id * 7919 + 1)
		if _playback_speed > WALKING_VISIBLE_MAX_SPEED:
			# Decision M4: above this speed, snap to the activity spot instead
			# of drawing the walk.
			var away_ticks: int = facts["depart_tick"]
			var back_ticks: int = facts["return_depart_tick"]
			flat = work if (_estimated_tick >= float(away_ticks) and _estimated_tick < float(back_ticks)) else home
		else:
			flat = _walking_position(home, work, facts)
	return Vector3(flat.x, _height_sampler.call(flat.x, flat.y) if _height_sampler.is_valid() else 0.0, flat.y)

func _walking_position(home: Vector2, work: Vector2, facts: Dictionary) -> Vector2:
	var distance := home.distance_to(work)
	var travel_seconds := maxf(0.5, distance / WALK_SPEED_UNITS_PER_SECOND)
	var depart: float = float(facts["depart_tick"])
	var arrive := depart + travel_seconds
	var back_depart: float = float(facts["return_depart_tick"])
	var back_arrive := back_depart + travel_seconds
	var tick := _estimated_tick

	if tick < depart:
		return home
	if tick < arrive:
		return home.lerp(work, (tick - depart) / travel_seconds)
	if tick < back_depart:
		return work
	if tick < back_arrive:
		return work.lerp(home, (tick - back_depart) / travel_seconds)
	return home

## Marks one villager, such as the demo's chosen NPC, with a floating marker.
## Only agents can be marked; entity_id 0 clears the mark.
func set_highlighted_entity(entity_type: String, entity_id: int) -> void:
	if entity_type != "agent":
		return
	_detach_highlight()
	_highlighted_agent_id = entity_id
	_attach_highlight_if_marked(entity_id)

func highlighted_agent_id() -> int:
	return _highlighted_agent_id

func _detach_highlight() -> void:
	if not _agent_nodes.has(_highlighted_agent_id):
		return
	var marker: Node = _agent_nodes[_highlighted_agent_id].get_node_or_null(HIGHLIGHT_NODE_NAME)
	if marker != null:
		marker.free()

func _attach_highlight_if_marked(id: int) -> void:
	if id == 0 or id != _highlighted_agent_id or not _agent_nodes.has(id):
		return
	var node: Node3D = _agent_nodes[id]
	if node.get_node_or_null(HIGHLIGHT_NODE_NAME) == null:
		node.add_child(_make_highlight_marker())

func _make_highlight_marker() -> MeshInstance3D:
	var cone := CylinderMesh.new()
	cone.top_radius = 0.5
	cone.bottom_radius = 0.0
	cone.height = 0.9
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_color = Color(1.0, 0.78, 0.12)
	var marker := MeshInstance3D.new()
	marker.name = HIGHLIGHT_NODE_NAME
	marker.mesh = cone
	marker.material_override = material
	marker.position = Vector3(0.0, HIGHLIGHT_HEIGHT, 0.0)
	return marker

func _process(delta: float) -> void:
	if not _paused:
		# The server has no sub-day ticking, so this is a client-local
		# approximation of elapsed simulation seconds between messages,
		# corrected to the server's real tick via set_tick() on every
		# message. Good enough to animate a walk; never trusted for anything
		# canonical.
		_estimated_tick += delta * float(_playback_speed) * 86400.0

	for id in _agent_nodes.keys():
		var node: Node3D = _agent_nodes[id]
		var previous := node.position
		var resolved := _resolve_agent_position(id)
		node.position = resolved
		var moved := Vector2(resolved.x - previous.x, resolved.z - previous.z)
		if moved.length() > 0.01:
			node.rotation.y = atan2(moved.x, moved.y)
