extends Node3D

## Presentation-only village dressing (ticket #55): fenced fields, tracks
## and the common around each settlement, derived from canonical data only.
##
## - Fields are backed by the simulation, not decoration: per the #50
##   decision cultivators work field plots on the settlement edge, so each
##   household with living SubsistenceCultivator members gets one plot whose
##   length grows with that household's cultivator count. Plots appear,
##   grow, shrink and disappear as the simulated households change.
##   Placement is deterministic: candidate spots come from a hash of
##   (settlement id, household id, attempt), tried in id order.
## - Tracks join each region's settlements along a minimum spanning tree;
##   a path runs from every building's door to its settlement's common.
##   These are placeholders until canonical tracks exist (ticket #53).
## - Everything is draped over the terrain and rebuilt when the terrain,
##   the settlements, the buildings or the cultivator households change.

signal exclusions_changed(area: Rect2)

const FoliageLib := preload("res://scripts/world/foliage.gd")
const BuildingAssets := preload("res://scripts/world/building_assets.gd")
const WHEAT := preload("res://assets/nature/crops_wheatStageB.glb")
const FENCE := preload("res://assets/nature/fence_simple.glb")

const CULTIVATOR := "SubsistenceCultivator"
const FIELD_WIDTH := 7.0
const FIELD_BASE_LENGTH := 5.0
const FIELD_LENGTH_PER_CULTIVATOR := 2.5
const FIELD_MAX_LENGTH := 16.0
const FIELD_MIN_RADIUS := 11.0
const FIELD_RADIUS_SPREAD := 14.0
const FIELD_ATTEMPTS := 32
const FIELD_GAP := 1.4
const MIN_CORE_RADIUS := 7.0
const CORE_MARGIN := 2.0
const TRACK_WIDTH := 1.9
const PATH_WIDTH := 1.2
const SAMPLE_STEP := 1.5
const DECAL_LIFT := 0.07
const SOIL_LIFT := 0.05
const REBUILD_INTERVAL := 0.25
## How far beyond a queried area an item can still block a point in it
## (the largest core radius plus tree clearance, or half a field).
const EXCLUSION_REACH := 40.0

const DIRT := Color(0.58, 0.46, 0.32)
const PLAZA := Color(0.64, 0.58, 0.47)
const SOIL := Color(0.40, 0.29, 0.19)

var _terrain: Node3D
## id -> {"pos": Vector2, "region_id": int, "active": bool}
var _settlements: Dictionary = {}
## id -> {"pos": Vector2, "type": String, "settlement_id": int, "radius": float}
var _buildings: Dictionary = {}
## living person id -> {"settlement_id": int, "household_id": int, "cultivator": bool}
var _people: Dictionary = {}
## region id -> per-region derived layout (fields, segments, cores)
var _layouts: Dictionary = {}
var _signatures: Dictionary = {}
var _region_nodes: Dictionary = {}
var _dirty: Dictionary = {}
var _since_rebuild := REBUILD_INTERVAL
var _decal_material := StandardMaterial3D.new()

func _init() -> void:
	_decal_material.vertex_color_use_as_albedo = true
	_decal_material.vertex_color_is_srgb = true
	_decal_material.roughness = 1.0

func set_terrain(terrain: Node3D) -> void:
	_terrain = terrain

func apply_snapshot(payload: Dictionary) -> void:
	_settlements.clear()
	_buildings.clear()
	_people.clear()
	for settlement in payload.get("settlements", []):
		_ingest_settlement(settlement)
	for building in payload.get("buildings", []):
		_ingest_building(building)
	for agent in payload.get("agents", []):
		_ingest_agent(agent)
	for region_id in _region_ids():
		_dirty[region_id] = true

func apply_delta(payload: Dictionary) -> void:
	for settlement in payload.get("addedSettlements", []) + payload.get("updatedSettlements", []):
		var id := int(settlement.get("id", 0))
		var was_active: bool = _settlements.get(id, {}).get("active", null) == true
		var known := _settlements.has(id)
		_ingest_settlement(settlement)
		if not known or was_active != bool(_settlements[id]["active"]):
			_dirty[_settlements[id]["region_id"]] = true
	for raw_id in payload.get("removedSettlementIds", []):
		if _settlements.has(int(raw_id)):
			_dirty[_settlements[int(raw_id)]["region_id"]] = true
			_settlements.erase(int(raw_id))
	for building in payload.get("addedBuildings", []) + payload.get("updatedBuildings", []):
		var id := int(building.get("id", 0))
		var known := _buildings.has(id)
		_ingest_building(building)
		if not known:
			_mark_settlement_region(int(_buildings[id]["settlement_id"]))
	for raw_id in payload.get("removedBuildingIds", []):
		if _buildings.has(int(raw_id)):
			_mark_settlement_region(int(_buildings[int(raw_id)]["settlement_id"]))
			_buildings.erase(int(raw_id))
	for agent in payload.get("addedAgents", []) + payload.get("updatedAgents", []):
		_ingest_agent(agent)
	for raw_id in payload.get("removedAgentIds", []):
		_people.erase(int(raw_id))
	for region_id in _region_ids():
		if _signatures.get(region_id, -1) != _cultivator_signature(region_id):
			_dirty[region_id] = true

func on_terrain_changed(_area: Rect2) -> void:
	for region_id in _layouts.keys():
		_dirty[region_id] = true

func _process(delta: float) -> void:
	_since_rebuild += delta
	if _dirty.is_empty() or _since_rebuild < REBUILD_INTERVAL:
		return
	_since_rebuild = 0.0
	var region_id: int = _dirty.keys()[0]
	_dirty.erase(region_id)
	rebuild_region(region_id)

## Field plots of a region: [{"centre", "axis", "length", "width",
## "settlement_id", "household_id", "cultivators"}].
func fields_in_region(region_id: int) -> Array:
	return _layouts.get(region_id, {}).get("fields", [])

## Whether `point` is taken by a field, track, path, common or building.
## `core_clearance` also keeps it out of every settlement's built-up core
## (trees stay out of villages; grass does not need to).
func blocks(point: Vector2, pad: float, core_clearance: float = 0.0) -> bool:
	return is_blocked(exclusions_in(Rect2(point, Vector2.ZERO)), point, pad, core_clearance)

## Everything that can block a point inside `area`, pre-filtered so bulk
## callers (the vegetation scatter) test a handful of items per point.
func exclusions_in(area: Rect2) -> Dictionary:
	var reach := area.grow(EXCLUSION_REACH)
	var found := {"cores": [], "buildings": [], "segments": [], "fields": []}
	for layout in _layouts.values():
		if not (layout["bounds"] as Rect2).intersects(reach):
			continue
		for core in layout["cores"]:
			if reach.has_point(core["pos"]):
				found["cores"].append(core)
		for building in layout["buildings"]:
			if reach.has_point(building["pos"]):
				found["buildings"].append(building)
		for segment in layout["segments"]:
			if Rect2(segment[0], Vector2.ZERO).expand(segment[1]).intersects(reach):
				found["segments"].append(segment)
		for field in layout["fields"]:
			if reach.has_point(field["centre"]):
				found["fields"].append(field)
	return found

static func is_blocked(exclusions: Dictionary, point: Vector2, pad: float, core_clearance: float) -> bool:
	for core in exclusions["cores"]:
		var limit: float = BuildingAssets.PLAZA_RADIUS + pad
		if core_clearance > 0.0:
			limit = float(core["radius"]) + core_clearance
		if point.distance_to(core["pos"]) < limit:
			return true
	for building in exclusions["buildings"]:
		if point.distance_to(building["pos"]) < float(building["radius"]) + pad:
			return true
	for segment in exclusions["segments"]:
		var closest := Geometry2D.get_closest_point_to_segment(point, segment[0], segment[1])
		if closest.distance_to(point) < float(segment[2]) * 0.5 + pad:
			return true
	for field in exclusions["fields"]:
		if _field_contains(field, point, pad + 0.6):
			return true
	return false

func rebuild_region(region_id: int) -> void:
	var layout := _plan_region(region_id)
	_layouts[region_id] = layout
	_signatures[region_id] = _cultivator_signature(region_id)
	if _region_nodes.has(region_id):
		_region_nodes[region_id].free()
	var node := Node3D.new()
	node.name = "Region_%d" % region_id
	add_child(node)
	_region_nodes[region_id] = node
	var decals := _build_decals(layout)
	if decals != null:
		node.add_child(decals)
	for child in _build_fields(layout["fields"]):
		node.add_child(child)
	exclusions_changed.emit(layout["bounds"])

func _plan_region(region_id: int) -> Dictionary:
	var settlement_ids: Array = []
	for id in _settlements.keys():
		if int(_settlements[id]["region_id"]) == region_id and bool(_settlements[id]["active"]):
			settlement_ids.append(id)
	settlement_ids.sort()
	var cores: Array = []
	var buildings: Array = []
	var segments: Array = []
	var bounds := Rect2(_settlements[settlement_ids[0]]["pos"], Vector2.ZERO) if not settlement_ids.is_empty() else Rect2()
	for id in settlement_ids:
		var centre: Vector2 = _settlements[id]["pos"]
		var radius := MIN_CORE_RADIUS
		for building_id in _buildings.keys():
			var building: Dictionary = _buildings[building_id]
			if int(building["settlement_id"]) != int(id):
				continue
			var pos: Vector2 = building["pos"]
			radius = max(radius, pos.distance_to(centre) + float(building["radius"]) + CORE_MARGIN)
			buildings.append(building)
			var to_centre := (centre - pos).normalized() if pos.distance_to(centre) > 0.01 else Vector2.RIGHT
			var door := pos + to_centre * float(building["radius"]) * 0.7
			segments.append([door, centre - to_centre * BuildingAssets.PLAZA_RADIUS * 0.6, PATH_WIDTH])
		cores.append({"pos": centre, "radius": radius, "id": id})
		bounds = bounds.expand(centre)
	for edge in _spanning_tree(settlement_ids):
		segments.append([_settlements[edge[0]]["pos"], _settlements[edge[1]]["pos"], TRACK_WIDTH])
	var fields := _place_fields(settlement_ids, cores, buildings, segments)
	return {
		"cores": cores,
		"buildings": buildings,
		"segments": segments,
		"fields": fields,
		"bounds": bounds.grow(FIELD_MIN_RADIUS + FIELD_RADIUS_SPREAD + FIELD_MAX_LENGTH + 40.0),
	}

## Prim's minimum spanning tree over settlement centres, in id order.
func _spanning_tree(settlement_ids: Array) -> Array:
	var edges: Array = []
	if settlement_ids.size() < 2:
		return edges
	var in_tree: Array = [settlement_ids[0]]
	var remaining: Array = settlement_ids.slice(1)
	while not remaining.is_empty():
		var best := [0, 0, INF]
		for a in in_tree:
			for b in remaining:
				var distance: float = (_settlements[a]["pos"] as Vector2).distance_to(_settlements[b]["pos"])
				if distance < float(best[2]):
					best = [a, b, distance]
		edges.append([best[0], best[1]])
		in_tree.append(best[1])
		remaining.erase(best[1])
	return edges

func _place_fields(settlement_ids: Array, cores: Array, buildings: Array, segments: Array) -> Array:
	var placed: Array = []
	for settlement_id in settlement_ids:
		var households := _cultivator_households(int(settlement_id))
		var household_ids := households.keys()
		household_ids.sort()
		var centre: Vector2 = _settlements[settlement_id]["pos"]
		for household_id in household_ids:
			var cultivators: int = households[household_id]
			var length: float = min(FIELD_BASE_LENGTH + FIELD_LENGTH_PER_CULTIVATOR * float(cultivators), FIELD_MAX_LENGTH)
			for attempt in FIELD_ATTEMPTS:
				var h := hash([int(settlement_id), int(household_id), attempt, 5551])
				var angle := float(h & 0xffff) / 65535.0 * TAU
				var radius := FIELD_MIN_RADIUS + float((h >> 16) & 0xff) / 255.0 * FIELD_RADIUS_SPREAD + float(attempt) * 0.9
				var direction := Vector2(cos(angle), sin(angle))
				var axis := direction if (h >> 24) & 1 == 0 else Vector2(-direction.y, direction.x)
				var field := {
					"centre": centre + direction * (radius + length * 0.5),
					"axis": axis,
					"length": length,
					"width": FIELD_WIDTH,
					"settlement_id": int(settlement_id),
					"household_id": int(household_id),
					"cultivators": cultivators,
				}
				if _field_fits(field, placed, cores, buildings, segments):
					placed.append(field)
					break
	return placed

func _field_fits(field: Dictionary, placed: Array, cores: Array, buildings: Array, segments: Array) -> bool:
	for core in cores:
		if _field_contains(field, core["pos"], float(core["radius"])):
			return false
	for building in buildings:
		if _field_contains(field, building["pos"], float(building["radius"]) + FIELD_GAP):
			return false
	for segment in segments:
		var closest := Geometry2D.get_closest_point_to_segment(field["centre"], segment[0], segment[1])
		if _field_contains(field, closest, float(segment[2]) * 0.5 + FIELD_GAP):
			return false
	for other in placed:
		if _fields_overlap(field, other, FIELD_GAP):
			return false
	return true

## Point-in-oriented-rectangle, with the rectangle grown by `pad`.
static func _field_contains(field: Dictionary, point: Vector2, pad: float) -> bool:
	var offset: Vector2 = point - field["centre"]
	var axis: Vector2 = field["axis"]
	var along := absf(offset.dot(axis))
	var across := absf(offset.dot(Vector2(-axis.y, axis.x)))
	return along <= float(field["length"]) * 0.5 + pad and across <= float(field["width"]) * 0.5 + pad

## Separating-axis test for two oriented rectangles.
func _fields_overlap(a: Dictionary, b: Dictionary, gap: float) -> bool:
	for field in [a, b]:
		var axis: Vector2 = field["axis"]
		for test_axis in [axis, Vector2(-axis.y, axis.x)]:
			if _projection_gap(a, b, test_axis) > gap:
				return false
	return true

func _projection_gap(a: Dictionary, b: Dictionary, test_axis: Vector2) -> float:
	var distance := absf(((b["centre"] as Vector2) - (a["centre"] as Vector2)).dot(test_axis))
	return distance - _half_extent_on(a, test_axis) - _half_extent_on(b, test_axis)

func _half_extent_on(field: Dictionary, test_axis: Vector2) -> float:
	var axis: Vector2 = field["axis"]
	return absf(axis.dot(test_axis)) * float(field["length"]) * 0.5 + absf(Vector2(-axis.y, axis.x).dot(test_axis)) * float(field["width"]) * 0.5

func _build_decals(layout: Dictionary) -> MeshInstance3D:
	var vertices := PackedVector3Array()
	var colors := PackedColorArray()
	var indices := PackedInt32Array()
	for segment in layout["segments"]:
		var lift := DECAL_LIFT if float(segment[2]) >= TRACK_WIDTH else DECAL_LIFT + 0.01
		_append_ribbon(vertices, colors, indices, segment[0], segment[1], float(segment[2]), lift)
	for core in layout["cores"]:
		_append_disc(vertices, colors, indices, core["pos"], BuildingAssets.PLAZA_RADIUS, DECAL_LIFT + 0.02)
	if vertices.is_empty():
		return null
	return _mesh_instance(vertices, colors, indices, _decal_material)

func _append_ribbon(vertices: PackedVector3Array, colors: PackedColorArray, indices: PackedInt32Array, from: Vector2, to: Vector2, width: float, lift: float) -> void:
	var length := from.distance_to(to)
	if length < 0.1:
		return
	var direction := (to - from) / length
	var side := Vector2(-direction.y, direction.x) * width * 0.5
	var steps := int(ceil(length / SAMPLE_STEP))
	var base := vertices.size()
	for step in steps + 1:
		var point := from + direction * (length * float(step) / float(steps))
		for offset in [side, -side]:
			var corner: Vector2 = point + offset
			vertices.append(Vector3(corner.x, _height(corner) + lift, corner.y))
			colors.append(DIRT.darkened(0.08 if step % 3 == 0 else 0.0))
	for step in steps:
		var i := base + step * 2
		indices.append_array([i, i + 2, i + 1, i + 1, i + 2, i + 3])

func _append_disc(vertices: PackedVector3Array, colors: PackedColorArray, indices: PackedInt32Array, centre: Vector2, radius: float, lift: float) -> void:
	var base := vertices.size()
	vertices.append(Vector3(centre.x, _height(centre) + lift, centre.y))
	colors.append(PLAZA)
	var segments := 20
	for index in segments:
		var angle := TAU * float(index) / float(segments)
		var rim := centre + Vector2(cos(angle), sin(angle)) * radius
		vertices.append(Vector3(rim.x, _height(rim) + lift, rim.y))
		colors.append(PLAZA.darkened(0.06))
	for index in segments:
		indices.append_array([base, base + 1 + (index + 1) % segments, base + 1 + index])

func _build_fields(fields: Array) -> Array:
	var nodes: Array = []
	if fields.is_empty():
		return nodes
	var vertices := PackedVector3Array()
	var colors := PackedColorArray()
	var indices := PackedInt32Array()
	var crops: Array[Transform3D] = []
	var fences: Array[Transform3D] = []
	for field in fields:
		_append_soil(vertices, colors, indices, field)
		_append_crops(crops, field)
		_append_fence(fences, field)
	nodes.append(_mesh_instance(vertices, colors, indices, _decal_material))
	for instance in [FoliageLib.make_multimesh(WHEAT, crops, 260.0), FoliageLib.make_multimesh(FENCE, fences, 320.0)]:
		if instance != null:
			nodes.append(instance)
	return nodes

func _append_soil(vertices: PackedVector3Array, colors: PackedColorArray, indices: PackedInt32Array, field: Dictionary) -> void:
	var axis: Vector2 = field["axis"]
	var across := Vector2(-axis.y, axis.x)
	var columns := int(ceil(float(field["length"])))
	var rows := int(ceil(float(field["width"])))
	var base := vertices.size()
	for column in columns + 1:
		for row in rows + 1:
			var u := (float(column) / float(columns) - 0.5) * float(field["length"])
			var v := (float(row) / float(rows) - 0.5) * float(field["width"])
			var point: Vector2 = field["centre"] + axis * u + across * v
			vertices.append(Vector3(point.x, _height(point) + SOIL_LIFT, point.y))
			var furrow := 0.5 + 0.5 * sin(v * 2.4)
			colors.append(SOIL.lerp(SOIL.lightened(0.18), furrow))
	for column in columns:
		for row in rows:
			var i := base + column * (rows + 1) + row
			indices.append_array([i, i + 1, i + rows + 1, i + 1, i + rows + 2, i + rows + 1])

func _append_crops(crops: Array[Transform3D], field: Dictionary) -> void:
	var axis: Vector2 = field["axis"]
	var across := Vector2(-axis.y, axis.x)
	var half_length := float(field["length"]) * 0.5 - 0.7
	var half_width := float(field["width"]) * 0.5 - 0.7
	var seed_value := hash([field["settlement_id"], field["household_id"], 91])
	var u := -half_length
	var index := 0
	while u <= half_length:
		var v := -half_width
		while v <= half_width:
			var point: Vector2 = field["centre"] + axis * u + across * v
			var turn := float(hash([seed_value, index]) & 0xffff) / 65535.0 * TAU
			var basis := Basis(Vector3.UP, turn).scaled(Vector3.ONE * 2.2)
			crops.append(Transform3D(basis, Vector3(point.x, _height(point), point.y)))
			v += 1.1
			index += 1
		u += 1.3

## Fence runs of 2 m pieces around the plot, with a gate gap on the side
## facing the settlement.
func _append_fence(fences: Array[Transform3D], field: Dictionary) -> void:
	var axis: Vector2 = field["axis"]
	var across := Vector2(-axis.y, axis.x)
	var half := Vector2(float(field["length"]) * 0.5 + 0.5, float(field["width"]) * 0.5 + 0.5)
	var centre: Vector2 = field["centre"]
	var corners := [
		centre - axis * half.x - across * half.y,
		centre + axis * half.x - across * half.y,
		centre + axis * half.x + across * half.y,
		centre - axis * half.x + across * half.y,
	]
	var home: Vector2 = _settlements.get(field["settlement_id"], {}).get("pos", centre)
	var gate_edge := 0
	var gate_distance := INF
	for edge in 4:
		var middle: Vector2 = (corners[edge] + corners[(edge + 1) % 4]) * 0.5
		if middle.distance_to(home) < gate_distance:
			gate_distance = middle.distance_to(home)
			gate_edge = edge
	for edge in 4:
		var a: Vector2 = corners[edge]
		var b: Vector2 = corners[(edge + 1) % 4]
		var direction := (b - a).normalized()
		var count := int(a.distance_to(b) / 2.0)
		for piece in count:
			if edge == gate_edge and piece == count / 2:
				continue
			var point := a + direction * (float(piece) + 0.5) * 2.0
			var basis := Basis(Vector3.UP, atan2(-direction.y, direction.x)).scaled(Vector3.ONE * 2.0)
			fences.append(Transform3D(basis, Vector3(point.x, _height(point), point.y)))

func _mesh_instance(vertices: PackedVector3Array, colors: PackedColorArray, indices: PackedInt32Array, material: Material) -> MeshInstance3D:
	_face_triangles_up(vertices, indices)
	var normals := PackedVector3Array()
	normals.resize(vertices.size())
	normals.fill(Vector3.UP)
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_COLOR] = colors
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.surface_set_material(0, material)
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return instance

## Ribbons, discs and plots are built in whichever winding their axes give;
## rewind every triangle to the terrain's front-face orientation (x, then z,
## seen from above) so none is back-face culled or lit from below.
func _face_triangles_up(vertices: PackedVector3Array, indices: PackedInt32Array) -> void:
	for start in range(0, indices.size(), 3):
		var a := vertices[indices[start]]
		var b := vertices[indices[start + 1]]
		var c := vertices[indices[start + 2]]
		var cross := (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x)
		if cross < 0.0:
			var swap := indices[start + 1]
			indices[start + 1] = indices[start + 2]
			indices[start + 2] = swap

func _height(point: Vector2) -> float:
	return _terrain.get_height_at_world(point.x, point.y) if _terrain != null else 0.0

func _cultivator_households(settlement_id: int) -> Dictionary:
	var households: Dictionary = {}
	for person in _people.values():
		if bool(person["cultivator"]) and int(person["settlement_id"]) == settlement_id:
			var household_id := int(person["household_id"])
			households[household_id] = int(households.get(household_id, 0)) + 1
	return households

func _cultivator_signature(region_id: int) -> int:
	var entries: Array = []
	for person in _people.values():
		if not bool(person["cultivator"]):
			continue
		var settlement: Dictionary = _settlements.get(int(person["settlement_id"]), {})
		if int(settlement.get("region_id", -1)) == region_id:
			entries.append([int(person["settlement_id"]), int(person["household_id"])])
	entries.sort()
	return hash(entries)

func _region_ids() -> Array:
	var ids: Dictionary = {}
	for settlement in _settlements.values():
		ids[int(settlement["region_id"])] = true
	return ids.keys()

func _mark_settlement_region(settlement_id: int) -> void:
	if _settlements.has(settlement_id):
		_dirty[_settlements[settlement_id]["region_id"]] = true

func _ingest_settlement(settlement: Dictionary) -> void:
	var pos: Dictionary = settlement.get("position", {})
	_settlements[int(settlement.get("id", 0))] = {
		"pos": Vector2(float(pos.get("x", 0.0)), float(pos.get("z", 0.0))),
		"region_id": int(settlement.get("regionId", 0)),
		"active": bool(settlement.get("active", true)),
	}

func _ingest_building(building: Dictionary) -> void:
	var pos: Dictionary = building.get("position", {})
	var size := BuildingAssets.footprint(str(building.get("type", "Dwelling")))
	_buildings[int(building.get("id", 0))] = {
		"pos": Vector2(float(pos.get("x", 0.0)), float(pos.get("z", 0.0))),
		"type": str(building.get("type", "Dwelling")),
		"settlement_id": int(building.get("settlementId", 0)),
		"radius": Vector2(size.x, size.z).length() * 0.5,
	}

func _ingest_agent(agent: Dictionary) -> void:
	var id := int(agent.get("id", 0))
	if not bool(agent.get("alive", true)):
		_people.erase(id)
		return
	_people[id] = {
		"settlement_id": int(agent.get("settlementId", 0)),
		"household_id": int(agent.get("householdId", 0)),
		"cultivator": str(agent.get("occupation", "")) == CULTIVATOR,
	}
