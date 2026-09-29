extends Node3D

## Presentation terrain for the live client (ticket #55, fixing live-client
## defects C1-C4). The simulation has no terrain heights, so everything here
## is presentation state derived deterministically from canonical data:
##
## - Height is a pure function of world (x, z), the world's region data and
##   the canonical settlement positions -- never of load order or time. Tiles
##   therefore line up seamlessly, and regenerating one reproduces it exactly.
## - Villages sit in flat clearings: relief is damped near every settlement
##   and rises into hills (scaled by the nearest region's environment class)
##   in a ring beyond them, which is where the forest ring grows.
## - C1/C2: instead of one fixed 300x300 patch centred on the first
##   settlement (which floated over a void and never followed the world),
##   64 m tiles are generated in world space wherever they are both near the
##   camera focus and near any settlement, including ones founded later. A
##   large ground skirt follows the camera below the tiles, and relief fades
##   to zero before the tiles end, so there is never an edge or a void.
## - C4: every tile carries a HeightMapShape3D collision body built from the
##   same height grid the mesh uses (physics layer 2, for the player avatar).
##
## Environment class and wild-food richness still tint the ground, as in
## ticket #18.

signal terrain_changed(area: Rect2)

const TILE_CELLS := 32
const CELL_SIZE := 2.0
const TILE_SIZE := TILE_CELLS * CELL_SIZE
const FOCUS_RADIUS := 700.0
const REFOCUS_DISTANCE := 32.0
const TILES_PER_FRAME := 2
const VILLAGE_FLAT_RADIUS := 26.0
const HILL_START := 34.0
const HILL_FULL := 95.0
const FADE_START := 250.0
const FADE_END := 320.0
const ROLLING_AMPLITUDE := 1.1
const SKIRT_SIZE := 9000.0
const SKIRT_DROP := 0.3
const WORLD_COLLISION_LAYER := 2
## terrain_changed area meaning "the whole world changed".
const EVERYWHERE := Rect2(-1.0e9, -1.0e9, 2.0e9, 2.0e9)

const HILL_HEIGHT_BY_CLASS := {
	"ProductiveArable": 7.0,
	"Woodland": 11.0,
	"Marginal": 15.0,
	"WaterWetland": 2.0,
}

const BASE_COLOR_BY_CLASS := {
	"ProductiveArable": Color(0.40, 0.56, 0.22),
	"Woodland": Color(0.29, 0.46, 0.19),
	"Marginal": Color(0.50, 0.50, 0.30),
	"WaterWetland": Color(0.30, 0.50, 0.34),
}

const DEFAULT_COLOR := Color(0.40, 0.56, 0.22)
const LUSH_COLOR := Color(0.36, 0.60, 0.22)
const DRY_HILL_COLOR := Color(0.55, 0.56, 0.33)
const FAR_COLOR := Color(0.38, 0.52, 0.24)

var _rolling := FastNoiseLite.new()
var _hills := FastNoiseLite.new()
var _tint := FastNoiseLite.new()
var _world_seed := 0
var _regions: Dictionary = {}
## settlement id -> {"pos": Vector2, "region_id": int}
var _settlements: Dictionary = {}
## Vector2i -> {"node": Node3D, "heights": PackedFloat32Array}
var _tiles: Dictionary = {}
var _queue: Array[Vector2i] = []
var _focus := Vector2.ZERO
var _last_refocus := Vector2(INF, INF)
var _material := StandardMaterial3D.new()
var _skirt: MeshInstance3D

func _init() -> void:
	_material.vertex_color_use_as_albedo = true
	_material.vertex_color_is_srgb = true
	_material.roughness = 0.95
	_seed_noise(0)

func _ready() -> void:
	_skirt = MeshInstance3D.new()
	_skirt.name = "GroundSkirt"
	var plane := PlaneMesh.new()
	plane.size = Vector2(SKIRT_SIZE, SKIRT_SIZE)
	_skirt.mesh = plane
	var skirt_material := StandardMaterial3D.new()
	skirt_material.albedo_color = FAR_COLOR
	skirt_material.roughness = 1.0
	_skirt.material_override = skirt_material
	_skirt.position = Vector3(_focus.x, -SKIRT_DROP, _focus.y)
	add_child(_skirt)

func _process(_delta: float) -> void:
	build_pending_tiles(TILES_PER_FRAME)

## Full world description from an initial_snapshot. Rebuilds only when the
## world seed or the settlement set actually changed (reconnects resend it).
func configure(regions: Array, settlements: Array) -> void:
	_regions = {}
	for region in regions:
		_regions[int(region.get("id", 0))] = region
	var next_seed := _world_seed_from(regions)
	var next := _settlement_table(settlements)
	if next_seed == _world_seed and _sorted_keys(next) == _sorted_keys(_settlements) and not _tiles.is_empty():
		return
	_seed_noise(next_seed)
	_settlements = next
	_clear_tiles()
	_refresh_wanted_tiles(true)
	terrain_changed.emit(EVERYWHERE)

## Applies settlement additions/removals from a world_delta. Settlement
## positions are canonical and never move, so only new or removed ids
## reshape the ground, and only around them.
func apply_delta(payload: Dictionary) -> void:
	var changed: Array[Vector2] = []
	for settlement in payload.get("addedSettlements", []):
		var id := int(settlement.get("id", 0))
		if not _settlements.has(id):
			_settlements[id] = _settlement_entry(settlement)
			changed.append(_settlements[id]["pos"])
	for raw_id in payload.get("removedSettlementIds", []):
		var id := int(raw_id)
		if _settlements.has(id):
			changed.append(_settlements[id]["pos"])
			_settlements.erase(id)
	for point in changed:
		_invalidate_around(point)
	if not changed.is_empty():
		_refresh_wanted_tiles(true)

## Camera ground focus, in world x/z. Tiles stream in and out around it.
func set_focus(point: Vector2) -> void:
	_focus = point
	if _skirt != null:
		_skirt.position = Vector3(point.x, -SKIRT_DROP, point.y)
	_refresh_wanted_tiles(false)

func build_pending_tiles(max_tiles: int = -1) -> void:
	var built := 0
	while not _queue.is_empty() and (max_tiles < 0 or built < max_tiles):
		var key: Vector2i = _queue.pop_front()
		if not _tiles.has(key):
			_build_tile(key)
			built += 1

func tile_count() -> int:
	return _tiles.size()

func has_tile_at(world_x: float, world_z: float) -> bool:
	return _tiles.has(_tile_key(world_x, world_z))

## Collision shape of the tile covering (x, z), or null.
func collision_shape_at(world_x: float, world_z: float) -> HeightMapShape3D:
	var key := _tile_key(world_x, world_z)
	if not _tiles.has(key):
		return null
	var body: StaticBody3D = _tiles[key]["node"].get_node("Collision")
	return (body.get_child(0) as CollisionShape3D).shape

## Height of the rendered surface at world (x, z): bilinear over the tile's
## height grid (matching how the mesh interpolates), or the analytic height
## where no tile has been generated yet.
func get_height_at_world(world_x: float, world_z: float) -> float:
	var key := _tile_key(world_x, world_z)
	if not _tiles.has(key):
		return height_at(world_x, world_z)
	var heights: PackedFloat32Array = _tiles[key]["heights"]
	var n := TILE_CELLS + 1
	var fx: float = clamp((world_x - float(key.x) * TILE_SIZE) / CELL_SIZE, 0.0, float(TILE_CELLS))
	var fz: float = clamp((world_z - float(key.y) * TILE_SIZE) / CELL_SIZE, 0.0, float(TILE_CELLS))
	var x0: int = min(int(floor(fx)), TILE_CELLS - 1)
	var z0: int = min(int(floor(fz)), TILE_CELLS - 1)
	var tx := fx - float(x0)
	var tz := fz - float(z0)
	var top: float = lerp(heights[z0 * n + x0], heights[z0 * n + x0 + 1], tx)
	var bottom: float = lerp(heights[(z0 + 1) * n + x0], heights[(z0 + 1) * n + x0 + 1], tx)
	return lerp(top, bottom, tz)

## Analytic terrain height. `nearby` optionally restricts the settlement
## search (an optimisation for tile generation); empty means all.
func height_at(world_x: float, world_z: float, nearby: Array = []) -> float:
	var nearest := _nearest(Vector2(world_x, world_z), nearby)
	return _height_from(world_x, world_z, float(nearest["distance"]), int(nearest["region_id"]))

## {"distance": float, "region_id": int, "id": int} of the nearest known
## settlement (distance INF when there are none).
func nearest_settlement(point: Vector2) -> Dictionary:
	return _nearest(point, [])

## Same as nearest_settlement, searching only `ids` (from settlements_within).
func nearest_settlement_among(point: Vector2, ids: Array) -> Dictionary:
	if ids.is_empty():
		return {"distance": INF, "region_id": 0, "id": 0}
	return _nearest(point, ids)

func settlements_within(point: Vector2, radius: float) -> Array:
	return _settlements_within(point, radius)

## Keys of the currently generated tiles (the scatter follows them).
func tile_keys() -> Array:
	return _tiles.keys()

func region(region_id: int) -> Dictionary:
	return _regions.get(region_id, {})

func world_seed() -> int:
	return _world_seed

func _height_from(world_x: float, world_z: float, distance: float, region_id: int) -> float:
	var fade := 1.0 - smoothstep(FADE_START, FADE_END, distance)
	if fade <= 0.0:
		return 0.0
	var flat: float = lerp(0.3, 1.0, smoothstep(8.0, VILLAGE_FLAT_RADIUS, distance))
	# Non-negative, so the ground skirt below never shows through a dip.
	var rolling := (_rolling.get_noise_2d(world_x, world_z) * 0.5 + 0.5) * ROLLING_AMPLITUDE * flat
	var hill_mask := smoothstep(HILL_START, HILL_FULL, distance)
	var hills := (_hills.get_noise_2d(world_x, world_z) * 0.5 + 0.5) * hill_mask * _hill_height(region_id)
	return (rolling + hills) * fade

func _color_from(world_x: float, world_z: float, height: float, distance: float, region_id: int) -> Color:
	var region_data: Dictionary = _regions.get(region_id, {})
	var environment_class := str(region_data.get("environmentClass", "ProductiveArable"))
	var color: Color = BASE_COLOR_BY_CLASS.get(environment_class, DEFAULT_COLOR)
	var capacity := float(region_data.get("wildFoodCapacity", 0))
	var stock := float(region_data.get("wildFoodStock", 0))
	var richness: float = clamp(stock / capacity, 0.0, 1.0) if capacity > 0.0 else 0.0
	color = color.lerp(LUSH_COLOR, richness * 0.4)
	var tint := _tint.get_noise_2d(world_x, world_z) * 0.5 + 0.5
	color = color.darkened(0.07).lerp(color.lightened(0.06), tint)
	color = color.lerp(DRY_HILL_COLOR, smoothstep(4.0, 14.0, height) * 0.35)
	return color.lerp(FAR_COLOR, smoothstep(FADE_START - 40.0, FADE_END, distance))

func _hill_height(region_id: int) -> float:
	var region_data: Dictionary = _regions.get(region_id, {})
	return float(HILL_HEIGHT_BY_CLASS.get(str(region_data.get("environmentClass", "ProductiveArable")), 7.0))

func _nearest(point: Vector2, candidates: Array) -> Dictionary:
	var best := {"distance": INF, "region_id": 0, "id": 0}
	var ids: Array = candidates if not candidates.is_empty() else _settlements.keys()
	for id in ids:
		var entry: Dictionary = _settlements[id]
		var distance: float = point.distance_to(entry["pos"])
		if distance < float(best["distance"]):
			best = {"distance": distance, "region_id": entry["region_id"], "id": int(id)}
	return best

func _settlements_within(point: Vector2, radius: float) -> Array:
	var ids: Array = []
	for id in _settlements.keys():
		if point.distance_to(_settlements[id]["pos"]) <= radius:
			ids.append(id)
	return ids

func _build_tile(key: Vector2i) -> void:
	var origin := Vector2(key) * TILE_SIZE
	var nearby := _settlements_within(origin + Vector2.ONE * TILE_SIZE * 0.5, FADE_END + TILE_SIZE)
	var n := TILE_CELLS + 1
	var ext := n + 2
	var grid := PackedFloat32Array()
	grid.resize(ext * ext)
	for gz in ext:
		for gx in ext:
			grid[gz * ext + gx] = height_at(origin.x + float(gx - 1) * CELL_SIZE, origin.y + float(gz - 1) * CELL_SIZE, nearby)

	var heights := PackedFloat32Array()
	heights.resize(n * n)
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var colors := PackedColorArray()
	for z in n:
		for x in n:
			var h := grid[(z + 1) * ext + x + 1]
			heights[z * n + x] = h
			vertices.append(Vector3(float(x) * CELL_SIZE, h, float(z) * CELL_SIZE))
			var slope_x := grid[(z + 1) * ext + x] - grid[(z + 1) * ext + x + 2]
			var slope_z := grid[z * ext + x + 1] - grid[(z + 2) * ext + x + 1]
			normals.append(Vector3(slope_x, 2.0 * CELL_SIZE, slope_z).normalized())
			var world := origin + Vector2(float(x), float(z)) * CELL_SIZE
			var nearest := _nearest(world, nearby)
			colors.append(_color_from(world.x, world.y, h, float(nearest["distance"]), int(nearest["region_id"])))

	var indices := PackedInt32Array()
	for z in TILE_CELLS:
		for x in TILE_CELLS:
			var i00 := z * n + x
			indices.append_array([i00, i00 + 1, i00 + n, i00 + 1, i00 + n + 1, i00 + n])

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_COLOR] = colors
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.surface_set_material(0, _material)

	var tile := Node3D.new()
	tile.name = "Tile_%d_%d" % [key.x, key.y]
	tile.position = Vector3(origin.x, 0.0, origin.y)
	var mesh_instance := MeshInstance3D.new()
	mesh_instance.mesh = mesh
	tile.add_child(mesh_instance)
	tile.add_child(_make_collision(heights))
	add_child(tile)
	_tiles[key] = {"node": tile, "heights": heights}

## HeightMapShape3D spans (map_width - 1) units centred on its node, so the
## shape is centred on the tile and scaled by the cell size in x/z.
func _make_collision(heights: PackedFloat32Array) -> StaticBody3D:
	var shape := HeightMapShape3D.new()
	shape.map_width = TILE_CELLS + 1
	shape.map_depth = TILE_CELLS + 1
	shape.map_data = heights
	var collision := CollisionShape3D.new()
	collision.shape = shape
	collision.scale = Vector3(CELL_SIZE, 1.0, CELL_SIZE)
	var body := StaticBody3D.new()
	body.name = "Collision"
	body.collision_layer = WORLD_COLLISION_LAYER
	body.collision_mask = 0
	body.position = Vector3(TILE_SIZE * 0.5, 0.0, TILE_SIZE * 0.5)
	body.add_child(collision)
	return body

func _refresh_wanted_tiles(force: bool) -> void:
	if not force and _focus.distance_to(_last_refocus) < REFOCUS_DISTANCE:
		return
	_last_refocus = _focus
	var reach := FADE_END + TILE_SIZE * 0.75
	var wanted: Dictionary = {}
	for id in _settlements.keys():
		var pos: Vector2 = _settlements[id]["pos"]
		if pos.distance_to(_focus) > FOCUS_RADIUS + reach:
			continue
		var min_key := _tile_key(pos.x - reach, pos.y - reach)
		var max_key := _tile_key(pos.x + reach, pos.y + reach)
		for tz in range(min_key.y, max_key.y + 1):
			for tx in range(min_key.x, max_key.x + 1):
				var key := Vector2i(tx, tz)
				var centre := (Vector2(key) + Vector2(0.5, 0.5)) * TILE_SIZE
				if centre.distance_to(pos) <= reach and centre.distance_to(_focus) <= FOCUS_RADIUS:
					wanted[key] = true
	for key in _tiles.keys():
		if not wanted.has(key):
			_free_tile(key)
	var missing: Array[Vector2i] = []
	for key in wanted.keys():
		if not _tiles.has(key):
			missing.append(key)
	missing.sort_custom(func(a: Vector2i, b: Vector2i) -> bool:
		return _key_distance(a) < _key_distance(b))
	_queue = missing

func _key_distance(key: Vector2i) -> float:
	return ((Vector2(key) + Vector2(0.5, 0.5)) * TILE_SIZE).distance_to(_focus)

func _invalidate_around(point: Vector2) -> void:
	var reach := FADE_END + TILE_SIZE
	for key in _tiles.keys():
		if ((Vector2(key) + Vector2(0.5, 0.5)) * TILE_SIZE).distance_to(point) <= reach:
			_free_tile(key)
	terrain_changed.emit(Rect2(point - Vector2.ONE * reach, Vector2.ONE * reach * 2.0))

func _free_tile(key: Vector2i) -> void:
	_tiles[key]["node"].free()
	_tiles.erase(key)

func _clear_tiles() -> void:
	for key in _tiles.keys():
		_free_tile(key)
	_queue.clear()

func _tile_key(world_x: float, world_z: float) -> Vector2i:
	return Vector2i(int(floor(world_x / TILE_SIZE)), int(floor(world_z / TILE_SIZE)))

func _settlement_table(settlements: Array) -> Dictionary:
	var table: Dictionary = {}
	for settlement in settlements:
		table[int(settlement.get("id", 0))] = _settlement_entry(settlement)
	return table

func _settlement_entry(settlement: Dictionary) -> Dictionary:
	var pos: Dictionary = settlement.get("position", {})
	return {
		"pos": Vector2(float(pos.get("x", 0.0)), float(pos.get("z", 0.0))),
		"region_id": int(settlement.get("regionId", 0)),
	}

## A stable per-world seed from canonical region data (the protocol carries
## no world seed), so each generated world gets its own terrain.
func _world_seed_from(regions: Array) -> int:
	var parts: Array = []
	for region in regions:
		parts.append([int(region.get("id", 0)), int(region.get("landAreaUnits", 0)), int(region.get("woodCapacity", 0)), int(region.get("stoneCapacity", 0))])
	parts.sort()
	return hash(parts) & 0x7fffffff

func _seed_noise(noise_seed: int) -> void:
	_world_seed = noise_seed
	_rolling.seed = noise_seed
	_rolling.frequency = 0.008
	_rolling.fractal_octaves = 3
	_hills.seed = noise_seed + 1
	_hills.frequency = 0.018
	_hills.fractal_octaves = 4
	_tint.seed = noise_seed + 2
	_tint.frequency = 0.05

func _sorted_keys(table: Dictionary) -> Array:
	var keys := table.keys()
	keys.sort()
	return keys
