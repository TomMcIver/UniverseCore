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

var _agent_nodes: Dictionary
var _settlement_nodes: Dictionary
var _building_nodes: Dictionary
var _building_data: Dictionary
var _height_sampler := Callable()

func _init() -> void:
	_agent_nodes = {}
	_settlement_nodes = {}
	_building_nodes = {}
	_building_data = {}

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

func _upsert_agent(agent: Dictionary) -> void:
	_remove_agent(int(agent.get("id", 0)))
	_spawn_agent(agent)

func _remove_agent(id: int) -> void:
	if _agent_nodes.has(id):
		_agent_nodes[id].free()
		_agent_nodes.erase(id)

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
	area.position = _grounded_position(agent)
	add_child(area)
	_agent_nodes[id] = area

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
