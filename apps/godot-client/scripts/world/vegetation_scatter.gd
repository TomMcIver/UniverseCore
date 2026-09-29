extends Node3D

## Presentation-only vegetation (ticket #55), scattered in 128 m chunks that
## follow the generated terrain tiles.
##
## - Deterministic: each chunk's candidate positions come from an RNG seeded
##   by the chunk coordinates and the terrain's world seed (itself derived
##   from canonical region data), always drawn in full before any
##   filtering, so exclusions never shift other plants.
## - The forest ring is backed by simulated woodland: tree density scales
##   with the nearest region's woodStock / woodCapacity and its environment
##   class. A candidate that the current stock can no longer support, but a
##   full stock would, renders as a stump -- harvesting visibly thins the
##   forest from the edges of each clump.
## - Wildflowers follow the region's wild-food richness.
## - Nothing grows on fields, tracks, paths, commons or buildings; trees
##   also keep clear of each settlement's built-up core. Chunks rebuild
##   when the village dressing or the terrain changes around them.
## - Trees carry simple cylinder collision on physics layer 2 for the
##   player avatar; ground cover does not.

const FoliageLib := preload("res://scripts/world/foliage.gd")
const VillageDressing := preload("res://scripts/world/village_dressing.gd")

const CHUNK_SIZE := 128.0
const CHUNKS_PER_FRAME := 1
const WORLD_COLLISION_LAYER := 2
const NATURE_SCALE := 3.2

const TREES := [
	preload("res://assets/nature/tree_oak.glb"),
	preload("res://assets/nature/tree_default.glb"),
	preload("res://assets/nature/tree_fat.glb"),
	preload("res://assets/nature/tree_pineRoundA.glb"),
	preload("res://assets/nature/tree_pineTallA.glb"),
]
const BUSH := preload("res://assets/nature/plant_bushLarge.glb")
const GRASS := preload("res://assets/nature/grass.glb")
const FLOWERS := [
	preload("res://assets/nature/flower_redA.glb"),
	preload("res://assets/nature/flower_yellowA.glb"),
	preload("res://assets/nature/flower_purpleA.glb"),
]
const ROCKS := [preload("res://assets/nature/rock_largeA.glb"), preload("res://assets/nature/rock_smallA.glb")]
const STUMP := preload("res://assets/nature/stump_round.glb")

const TREE_CANDIDATES := 560
const BUSH_CANDIDATES := 90
const GRASS_CANDIDATES := 1500
const FLOWER_CANDIDATES := 320
const ROCK_CANDIDATES := 40

const CLASS_FOREST := {"Woodland": 1.0, "ProductiveArable": 0.62, "Marginal": 0.35, "WaterWetland": 0.3}
const CLASS_ROCKS := {"Marginal": 0.9, "Woodland": 0.35, "ProductiveArable": 0.25, "WaterWetland": 0.1}

var _terrain: Node3D
var _dressing: Node3D
var _chunks: Dictionary = {}
var _dirty: Dictionary = {}
var _patch := FastNoiseLite.new()

func _init() -> void:
	_patch.frequency = 0.02
	_patch.fractal_octaves = 2

func setup(terrain: Node3D, dressing: Node3D) -> void:
	_terrain = terrain
	_dressing = dressing
	_patch.seed = terrain.world_seed() + 7

func mark_dirty(area: Rect2) -> void:
	for key in _chunks.keys():
		if _chunk_rect(key).intersects(area):
			_dirty[key] = true

func mark_all_dirty(_area: Rect2 = Rect2()) -> void:
	_patch.seed = _terrain.world_seed() + 7
	for key in _chunks.keys():
		_dirty[key] = true

func chunk_count() -> int:
	return _chunks.size()

func _process(_delta: float) -> void:
	if _terrain == null:
		return
	sync_chunks(CHUNKS_PER_FRAME)

## Creates chunks over generated terrain, frees chunks whose terrain is gone
## and rebuilds up to `budget` dirty chunks (-1 = all).
func sync_chunks(budget: int = -1) -> void:
	var wanted: Dictionary = {}
	for tile_key in _terrain.tile_keys():
		wanted[Vector2i(floori(tile_key.x / 2.0), floori(tile_key.y / 2.0))] = true
	for key in _chunks.keys():
		if not wanted.has(key):
			_chunks[key].free()
			_chunks.erase(key)
			_dirty.erase(key)
	var pending: Array = []
	for key in wanted.keys():
		if not _chunks.has(key) or _dirty.has(key):
			pending.append(key)
	var built := 0
	for key in pending:
		if budget >= 0 and built >= budget:
			break
		_rebuild_chunk(key)
		built += 1

func _rebuild_chunk(key: Vector2i) -> void:
	_dirty.erase(key)
	if _chunks.has(key):
		_chunks[key].free()
	var node := Node3D.new()
	node.name = "Chunk_%d_%d" % [key.x, key.y]
	add_child(node)
	_chunks[key] = node

	var rect := _chunk_rect(key)
	var nearby: Array = _terrain.settlements_within(rect.get_center(), CHUNK_SIZE * 0.71 + 330.0)
	if nearby.is_empty():
		return
	var exclusions: Dictionary = _dressing.exclusions_in(rect) if _dressing != null else {"cores": [], "buildings": [], "segments": [], "fields": []}
	var rng := RandomNumberGenerator.new()
	rng.seed = hash([key.x, key.y, _terrain.world_seed(), 4817])
	var placed := {}
	var trees := _scatter_trees(rng, rect, nearby, exclusions, placed)
	_scatter_ground(rng, rect, nearby, exclusions, placed)
	var summary: Dictionary = {}
	for scene in placed.keys():
		summary[scene.resource_path] = placed[scene]["points"]
	node.set_meta("placements", summary)
	for scene in placed.keys():
		var instance: MultiMeshInstance3D = FoliageLib.make_multimesh(scene, placed[scene]["xforms"], placed[scene]["range"])
		if instance != null:
			if placed[scene]["shadowless"]:
				instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			node.add_child(instance)
	if not trees.is_empty():
		node.add_child(_tree_collision(trees))

func _scatter_trees(rng: RandomNumberGenerator, rect: Rect2, nearby: Array, exclusions: Dictionary, placed: Dictionary) -> Array:
	var trunks: Array = []
	for index in TREE_CANDIDATES:
		var point := rect.position + Vector2(rng.randf(), rng.randf()) * CHUNK_SIZE
		var keep_roll := rng.randf()
		var variant := rng.randi() % TREES.size()
		var size := rng.randf_range(1.0, 1.5)
		var turn := rng.randf() * TAU
		var nearest: Dictionary = _terrain.nearest_settlement_among(point, nearby)
		var region: Dictionary = _terrain.region(int(nearest["region_id"]))
		var full := _forest_density(point, float(nearest["distance"]), region)
		if keep_roll >= full:
			continue
		if VillageDressing.is_blocked(exclusions, point, 1.8, 6.0):
			continue
		var standing := keep_roll < full * _wood_ratio(region)
		var scene: PackedScene = TREES[variant] if standing else STUMP
		var scale := NATURE_SCALE * size * (1.0 if standing else 0.8)
		_add(placed, scene, point, turn, scale, -0.1, 0.0, false)
		if standing:
			trunks.append(Vector3(point.x, _terrain.get_height_at_world(point.x, point.y), point.y))
	return trunks

func _scatter_ground(rng: RandomNumberGenerator, rect: Rect2, nearby: Array, exclusions: Dictionary, placed: Dictionary) -> void:
	var layers := [
		{"count": BUSH_CANDIDATES, "scenes": [BUSH], "range": 240.0, "shadowless": false, "scale": [0.55, 0.85], "pad": 0.9},
		{"count": GRASS_CANDIDATES, "scenes": [GRASS], "range": 140.0, "shadowless": true, "scale": [0.35, 0.8], "pad": 0.3},
		{"count": FLOWER_CANDIDATES, "scenes": FLOWERS, "range": 110.0, "shadowless": true, "scale": [0.4, 0.7], "pad": 0.3},
		{"count": ROCK_CANDIDATES, "scenes": ROCKS, "range": 280.0, "shadowless": false, "scale": [0.6, 1.3], "pad": 0.8},
	]
	for layer_index in layers.size():
		var layer: Dictionary = layers[layer_index]
		var scenes: Array = layer["scenes"]
		for index in int(layer["count"]):
			var point := rect.position + Vector2(rng.randf(), rng.randf()) * CHUNK_SIZE
			var keep_roll := rng.randf()
			var scene: PackedScene = scenes[rng.randi() % scenes.size()]
			var scale := NATURE_SCALE * rng.randf_range(layer["scale"][0], layer["scale"][1])
			var turn := rng.randf() * TAU
			var nearest: Dictionary = _terrain.nearest_settlement_among(point, nearby)
			var density := _ground_density(layer_index, float(nearest["distance"]), _terrain.region(int(nearest["region_id"])))
			if keep_roll >= density:
				continue
			if VillageDressing.is_blocked(exclusions, point, float(layer["pad"]), 0.0):
				continue
			_add(placed, scene, point, turn, scale, -0.02, float(layer["range"]), bool(layer["shadowless"]))

func _add(placed: Dictionary, scene: PackedScene, point: Vector2, turn: float, scale: float, sink: float, visibility: float, shadowless: bool) -> void:
	if not placed.has(scene):
		placed[scene] = {"xforms": [] as Array[Transform3D], "points": [], "range": visibility, "shadowless": shadowless}
	var basis := Basis(Vector3.UP, turn).scaled(Vector3.ONE * scale)
	var origin := Vector3(point.x, _terrain.get_height_at_world(point.x, point.y) + sink, point.y)
	placed[scene]["xforms"].append(Transform3D(basis, origin))
	placed[scene]["points"].append(point)

## {scene resource path: Array[Vector2]} of everything a chunk placed
## (MultiMesh transforms are not readable back on every rendering server).
func placements(key: Vector2i) -> Dictionary:
	return _chunks[key].get_meta("placements", {}) if _chunks.has(key) else {}

## Tree density for a full-stock forest: a ring around the villages,
## clumped by noise, scaled by environment class.
func _forest_density(point: Vector2, distance: float, region: Dictionary) -> float:
	var ring := smoothstep(26.0, 62.0, distance) * (1.0 - smoothstep(230.0, 300.0, distance))
	var clumps := smoothstep(-0.3, 0.3, _patch.get_noise_2d(point.x, point.y))
	var class_factor: float = CLASS_FOREST.get(str(region.get("environmentClass", "ProductiveArable")), 0.6)
	return ring * clumps * class_factor * 0.85

func _wood_ratio(region: Dictionary) -> float:
	var capacity := float(region.get("woodCapacity", 0))
	if capacity <= 0.0:
		return 0.0
	return clamp(float(region.get("woodStock", 0)) / capacity, 0.0, 1.0)

func _ground_density(layer_index: int, distance: float, region: Dictionary) -> float:
	match layer_index:
		0:
			return smoothstep(8.0, 20.0, distance) * (1.0 - smoothstep(140.0, 220.0, distance)) * 0.5
		1:
			return (1.0 - smoothstep(110.0, 180.0, distance)) * 0.85
		2:
			var capacity := float(region.get("wildFoodCapacity", 0))
			var richness: float = clamp(float(region.get("wildFoodStock", 0)) / capacity, 0.0, 1.0) if capacity > 0.0 else 0.0
			return (1.0 - smoothstep(90.0, 140.0, distance)) * (0.25 + 0.6 * richness)
		_:
			return float(CLASS_ROCKS.get(str(region.get("environmentClass", "ProductiveArable")), 0.3)) * (1.0 - smoothstep(260.0, 300.0, distance))

func _tree_collision(trunks: Array) -> StaticBody3D:
	var body := StaticBody3D.new()
	body.name = "TreeCollision"
	body.collision_layer = WORLD_COLLISION_LAYER
	body.collision_mask = 0
	var shape := CylinderShape3D.new()
	shape.radius = 0.45
	shape.height = 4.0
	for trunk in trunks:
		var collision := CollisionShape3D.new()
		collision.shape = shape
		collision.position = trunk + Vector3(0.0, 2.0, 0.0)
		body.add_child(collision)
	return body

func _chunk_rect(key: Vector2i) -> Rect2:
	return Rect2(Vector2(key) * CHUNK_SIZE, Vector2.ONE * CHUNK_SIZE)
