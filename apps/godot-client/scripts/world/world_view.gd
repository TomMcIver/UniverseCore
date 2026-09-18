class_name WorldView
extends Node3D

const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")

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

func _init() -> void:
	_agent_nodes = {}
	_settlement_nodes = {}
	_building_nodes = {}
	_building_data = {}

func apply_snapshot(payload: Dictionary) -> void:
	_clear_all()
	for agent in payload.get("agents", []):
		_spawn_agent(agent)
	for settlement in payload.get("settlements", []):
		_spawn_settlement(settlement)
	for building in payload.get("buildings", []):
		_spawn_building(building)

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

func _spawn_agent(agent: Dictionary) -> void:
	var body := CSGCylinder3D.new()
	body.radius = 0.35
	body.height = 1.7
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.55, 0.35, 0.4) if agent.get("sex", "Female") == "Female" else Color(0.3, 0.25, 0.2)
	body.material = material

	var id := int(agent.get("id", 0))
	var area := _make_pickable_area("agent", id, body, Vector3(0.7, 1.7, 0.7))
	area.position = UniverseProtocol.position_from(agent)
	add_child(area)
	_agent_nodes[id] = area

func _spawn_settlement(settlement: Dictionary) -> void:
	var marker := CSGCylinder3D.new()
	marker.radius = 1.5
	marker.height = 0.2
	marker.position = UniverseProtocol.position_from(settlement)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.6, 0.55, 0.35)
	marker.material = material
	add_child(marker)
	_settlement_nodes[int(settlement.get("id", 0))] = marker

func _spawn_building(building: Dictionary) -> void:
	var box := CSGBox3D.new()
	box.size = Vector3(3.0, 3.2, 3.0)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.55, 0.47, 0.37)
	box.material = material

	var id := int(building.get("id", 0))
	var area := _make_pickable_area("building", id, box, Vector3(3.0, 3.2, 3.0))
	area.position = UniverseProtocol.position_from(building)
	add_child(area)
	_building_nodes[id] = area
	_building_data[id] = building

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
