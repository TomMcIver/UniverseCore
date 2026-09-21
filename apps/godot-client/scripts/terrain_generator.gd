extends MeshInstance3D

## Ticket #12 baseline: a procedurally generated ground mesh driven by pure
## FastNoiseLite (generate()) -- still used as-is by the throwaway
## prototype scene (scenes/prototype.tscn), left untouched.
##
## Ticket #18 adds generate_from_regions(), which re-derives per-vertex
## height and color from UniverseCore's real per-region environment data
## (the RegionRenderView fields added to the protocol for this ticket:
## environmentClass, wildFoodCapacity/Stock, etc.) instead of pure noise,
## so two regions with genuinely different simulated environment state
## actually look different -- not just randomly different.
##
## Mapping from environment data to visuals (documented per the ticket's
## explicit requirement that this interpretive layer be written down):
##
## - environmentClass sets a per-region height-amplitude multiplier and a
##   base ground color: ProductiveArable is flatter and greener (fertile,
##   farmed land); Woodland is hillier and a darker green (forest floor);
##   Marginal is the hilliest and most dirt-brown (poor, rocky land);
##   WaterWetland is nearly flat and blue-green tinted (wet lowland).
## - wildFoodStock / wildFoodCapacity (a 0..1 "richness" ratio, 0 when
##   capacity is 0) lerps the base color toward a more saturated green --
##   a region currently rich in wild food reads visually lusher than a
##   depleted one of the same environmentClass.
## - Regions carry no position of their own on the wire (only settlements
##   do), so each region's visual "center" is the average position of its
##   settlements; each terrain vertex takes its parameters from whichever
##   region center is nearest (a simple Voronoi-style assignment).

## 150x150 world units, sized to comfortably cover the region-clustered
## settlement layout (web_protocol.cpp: region cluster centers up to
## radius 115, plus up to 22 more local scatter -- ~137 worst case) since
## the previous fix for settlements being placed on one uniform ring.
@export var width: int = 150
@export var depth: int = 150
@export var cell_size: float = 2.0
@export var height_scale: float = 4.5
@export var noise_seed: int = 1337

const HEIGHT_MULTIPLIER_BY_CLASS := {
	"ProductiveArable": 0.6,
	"Woodland": 1.4,
	"Marginal": 1.8,
	"WaterWetland": 0.2,
}

const BASE_COLOR_BY_CLASS := {
	"ProductiveArable": Color(0.35, 0.50, 0.24),
	"Woodland": Color(0.20, 0.34, 0.16),
	"Marginal": Color(0.45, 0.38, 0.28),
	"WaterWetland": Color(0.24, 0.40, 0.42),
}

const LUSH_COLOR := Color(0.32, 0.62, 0.24)
const DEFAULT_COLOR := Color(0.35, 0.50, 0.24)

var _height_grid: Array = []
var _grid_half_width := 0.0
var _grid_half_depth := 0.0

func _ready() -> void:
	generate()

## Pure-noise fallback: used by the prototype scene and whenever no real
## snapshot data is available yet.
func generate() -> void:
	_generate_mesh(func(_world_x: float, _world_z: float, base_noise: float) -> Dictionary:
		return {"height": base_noise * height_scale, "color": _height_color(base_noise)})

## `regions` and `settlements` are the raw payload arrays (Dictionary per
## entry) exactly as received over the wire.
##
## Ticket #24/#25 gave regions/settlements real canonical positions that
## can land anywhere within a world extent far larger than this generator's
## fixed-size patch -- so the patch is now generated in local space as
## before, then this whole node is repositioned (via `position`) onto the
## average real settlement position, so the ground actually exists under
## the population instead of always sitting at world origin regardless of
## where the world's real content is. `get_height_at_world` accounts for
## this offset when converting a world query back to local grid space.
func generate_from_regions(regions: Array, settlements: Array) -> void:
	if regions.is_empty():
		generate()
		return

	var region_by_id: Dictionary = {}
	for region in regions:
		region_by_id[int(region.get("id", 0))] = region

	var region_centers := _region_centers_from_settlements(settlements)
	if region_centers.is_empty():
		generate()
		return

	var world_center := _first_settlement_position(settlements)
	position = Vector3(world_center.x, 0.0, world_center.y)

	_generate_mesh(func(local_x: float, local_z: float, base_noise: float) -> Dictionary:
		var world_point := Vector2(local_x, local_z) + world_center
		var nearest_id := _nearest_region_id(world_point, region_centers)
		var region: Dictionary = region_by_id.get(nearest_id, {})
		return _region_driven_sample(region, base_noise))

## The average of every settlement's position is not a reliable place to
## generate ground around: a long-lived, spread-out world (multiple
## distant settlements from founding/splitting) can average out to empty
## space between clusters. The first (lowest-id) settlement's own real
## position always has real content there -- and main.gd's camera framing
## uses the exact same rule, so the two always agree on where "the world"
## is.
func _first_settlement_position(settlements: Array) -> Vector2:
	var first: Dictionary = settlements[0]
	for settlement in settlements:
		if int(settlement.get("id", 0)) < int(first.get("id", 0)):
			first = settlement
	var pos: Dictionary = first.get("position", {})
	return Vector2(float(pos.get("x", 0.0)), float(pos.get("z", 0.0)))

func _region_centers_from_settlements(settlements: Array) -> Dictionary:
	var sum_by_region: Dictionary = {}
	var count_by_region: Dictionary = {}
	for settlement in settlements:
		var region_id := int(settlement.get("regionId", 0))
		var pos: Dictionary = settlement.get("position", {})
		var point := Vector2(float(pos.get("x", 0.0)), float(pos.get("z", 0.0)))
		sum_by_region[region_id] = sum_by_region.get(region_id, Vector2.ZERO) + point
		count_by_region[region_id] = int(count_by_region.get(region_id, 0)) + 1

	var centers: Dictionary = {}
	for region_id in sum_by_region.keys():
		centers[region_id] = sum_by_region[region_id] / float(count_by_region[region_id])
	return centers

func _nearest_region_id(point: Vector2, region_centers: Dictionary) -> int:
	var best_id := 0
	var best_distance := INF
	for region_id in region_centers.keys():
		var distance: float = point.distance_squared_to(region_centers[region_id])
		if distance < best_distance:
			best_distance = distance
			best_id = int(region_id)
	return best_id

func _region_driven_sample(region: Dictionary, base_noise: float) -> Dictionary:
	var environment_class := str(region.get("environmentClass", "ProductiveArable"))
	var multiplier: float = HEIGHT_MULTIPLIER_BY_CLASS.get(environment_class, 1.0)
	var base_color: Color = BASE_COLOR_BY_CLASS.get(environment_class, DEFAULT_COLOR)

	var capacity := float(region.get("wildFoodCapacity", 0))
	var stock := float(region.get("wildFoodStock", 0))
	var richness: float = clamp(stock / capacity, 0.0, 1.0) if capacity > 0.0 else 0.0

	return {
		"height": base_noise * height_scale * multiplier,
		"color": base_color.lerp(LUSH_COLOR, richness * 0.5),
	}

func _generate_mesh(sample: Callable) -> void:
	var noise := FastNoiseLite.new()
	noise.seed = noise_seed
	noise.frequency = 0.045
	noise.fractal_octaves = 4

	var half_width := float(width) * cell_size * 0.5
	var half_depth := float(depth) * cell_size * 0.5

	var heights: Array = []
	var colors: Array = []
	for z in range(depth + 1):
		var height_row: Array = []
		var color_row: Array = []
		for x in range(width + 1):
			var world_x := float(x) * cell_size - half_width
			var world_z := float(z) * cell_size - half_depth
			var base_noise := noise.get_noise_2d(float(x), float(z))
			var sampled: Dictionary = sample.call(world_x, world_z, base_noise)
			height_row.append(float(sampled.get("height", 0.0)))
			color_row.append(sampled.get("color", DEFAULT_COLOR) as Color)
		heights.append(height_row)
		colors.append(color_row)

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)

	for z in range(depth):
		for x in range(width):
			var x0 := float(x) * cell_size - half_width
			var x1 := float(x + 1) * cell_size - half_width
			var z0 := float(z) * cell_size - half_depth
			var z1 := float(z + 1) * cell_size - half_depth

			var h00: float = heights[z][x]
			var h10: float = heights[z][x + 1]
			var h01: float = heights[z + 1][x]
			var h11: float = heights[z + 1][x + 1]

			var c00: Color = colors[z][x]
			var c10: Color = colors[z][x + 1]
			var c01: Color = colors[z + 1][x]
			var c11: Color = colors[z + 1][x + 1]

			var p00 := Vector3(x0, h00, z0)
			var p10 := Vector3(x1, h10, z0)
			var p01 := Vector3(x0, h01, z1)
			var p11 := Vector3(x1, h11, z1)

			_add_triangle(st, p00, c00, p10, c10, p01, c01)
			_add_triangle(st, p10, c10, p11, c11, p01, c01)

	st.generate_normals()
	var generated_mesh := st.commit()

	var material := StandardMaterial3D.new()
	material.vertex_color_use_as_albedo = true
	material.roughness = 1.0
	generated_mesh.surface_set_material(0, material)

	mesh = generated_mesh
	_height_grid = heights
	_grid_half_width = half_width
	_grid_half_depth = half_depth

## Samples the generated terrain's height at a world (x, z) position, via
## nearest-vertex lookup on the same grid _generate_mesh built. Used to
## ground agents/buildings onto the real terrain surface instead of the
## flat height the protocol's settlement-ring placement implies.
func get_height_at_world(world_x: float, world_z: float) -> float:
	if _height_grid.is_empty():
		return 0.0
	var local_x := world_x - position.x
	var local_z := world_z - position.z
	var grid_x: int = clamp(int(round((local_x + _grid_half_width) / cell_size)), 0, width)
	var grid_z: int = clamp(int(round((local_z + _grid_half_depth) / cell_size)), 0, depth)
	return _height_grid[grid_z][grid_x]

func _add_triangle(st: SurfaceTool, a: Vector3, ca: Color, b: Vector3, cb: Color, c: Vector3, cc: Color) -> void:
	st.set_color(ca)
	st.add_vertex(a)
	st.set_color(cb)
	st.add_vertex(b)
	st.set_color(cc)
	st.add_vertex(c)

func _height_color(h: float) -> Color:
	var t: float = clamp((h + 1.0) / 2.0, 0.0, 1.0)
	var dirt := Color(0.40, 0.32, 0.22)
	var grass := Color(0.29, 0.46, 0.21)
	return dirt.lerp(grass, t)
