extends RefCounted

## Ticket #55 (demo village look) tests, run by run_tests.gd through its
## _run_test/_assert_eq helpers so failures count toward the same total.

const TerrainGenerator := preload("res://scripts/terrain_generator.gd")
const DayNight := preload("res://scripts/world/day_night.gd")
const BuildingAssets := preload("res://scripts/world/building_assets.gd")
const WorldView := preload("res://scripts/world/world_view.gd")
const VillageDressing := preload("res://scripts/world/village_dressing.gd")
const VegetationScatter := preload("res://scripts/world/vegetation_scatter.gd")
const FoliageLib := preload("res://scripts/world/foliage.gd")

var _runner

func _init(runner) -> void:
	_runner = runner

func run() -> void:
	_test("terrain samples height via bilinear interpolation matching the mesh surface", _test_terrain_bilinear)
	_test("terrain heights are deterministic and seamless across tile borders", _test_terrain_deterministic_seamless)
	_test("terrain covers far and later-founded settlements, never a void (C1/C2)", _test_terrain_covers_settlements)
	_test("terrain puts villages in flat clearings with hills beyond", _test_terrain_flat_villages)
	_test("terrain tiles carry heightmap collision matching the rendered heights (C4)", _test_terrain_collision)
	_test("terrain colour differs by environment class and wild-food richness", _test_terrain_colour)
	_test("terrain fades to flat ground far from every settlement", _test_terrain_fades_far)
	_test("day_night opens on mid-afternoon light at tick 0", _test_day_night_afternoon_anchor)
	_test("day_night is brightest at midday and dimmest at midnight", _test_day_night_midday_vs_midnight)
	_test("day_night wraps correctly across multiple simulated days", _test_day_night_wraps)
	_test("buildings show their construction state and progress (B2)", _test_building_construction_state)
	_test("live building set uses no medieval-town pieces (fountain, watermill, stall, lantern)", _test_no_medieval_pieces)
	_test("world_view gives buildings simplified collision on the avatar layer", _test_world_view_building_collision)
	_test("world_view does not rebuild an unchanged building on every delta (E1)", _test_world_view_no_building_churn)
	_test("world_view turns each building's door toward its settlement's common", _test_world_view_building_facing)
	_test("fields come from cultivator households and grow and shrink with them", _test_fields_follow_cultivators)
	_test("fields are deterministic and never overlap cores, buildings or each other", _test_fields_deterministic_non_overlapping)
	_test("world_view turns each building's door along its canonical facingDegrees", _test_world_view_canonical_facing)
	_test("tracks join each region's settlements", _test_tracks_join_settlements)
	_test("dressing runs door paths to canonical track points and chains tracks from the common", _test_canonical_tracks_from_facing)
	_test("vegetation is deterministic and keeps clear of fields, paths and buildings", _test_vegetation_clear_and_deterministic)
	_test("forest follows region wood stock; felled trees become stumps", _test_forest_follows_wood_stock)
	_test("foliage is lit from above (black-grass fix) and retinted from mint", _test_foliage_prepared)

func _test(name: String, fn: Callable) -> void:
	_runner._run_test(name, fn)

func _eq(actual, expected, context: String = "") -> void:
	_runner._assert_eq(actual, expected, context)

func _root() -> Window:
	return _runner.root

# ----------------------------------------------------------------- fixtures

func _region(id: int, environment_class: String = "ProductiveArable", wood_stock: int = 1000) -> Dictionary:
	return {"id": id, "environmentClass": environment_class, "landAreaUnits": 100, "wildFoodCapacity": 100, "wildFoodStock": 50, "woodCapacity": 1000, "woodStock": wood_stock, "stoneCapacity": 500, "stoneRemaining": 500}

func _settlement(id: int, x: float, z: float, region_id: int = 1, active: bool = true) -> Dictionary:
	return {"id": id, "regionId": region_id, "active": active, "position": {"x": x, "y": 0.0, "z": z}}

func _building(id: int, settlement_id: int, x: float, z: float, type: String = "Dwelling", state: String = "Complete", progress: float = 1.0) -> Dictionary:
	return {"id": id, "settlementId": settlement_id, "regionId": 1, "type": type, "state": state, "progress": progress, "settlementCommonsOwned": true, "ownerHouseholdId": 0, "position": {"x": x, "y": 0.0, "z": z}}

func _person(id: int, settlement_id: int, household_id: int, occupation: String, alive: bool = true) -> Dictionary:
	return {"id": id, "settlementId": settlement_id, "householdId": household_id, "regionId": 1, "occupation": occupation, "alive": alive, "sex": "Female", "age": 30, "currentActivity": "Work", "position": {"x": 0.0, "y": 0.9, "z": 0.0}}

func _terrain_with(regions: Array, settlements: Array, focus: Vector2) -> Node3D:
	var terrain := TerrainGenerator.new()
	_root().add_child(terrain)
	terrain.configure(regions, settlements)
	terrain.set_focus(focus)
	terrain.build_pending_tiles()
	return terrain

# ------------------------------------------------------------------ terrain

func _test_terrain_bilinear() -> void:
	var terrain := TerrainGenerator.new()
	var n := TerrainGenerator.TILE_CELLS + 1
	var heights := PackedFloat32Array()
	heights.resize(n * n)
	heights[0] = 0.0
	heights[1] = 10.0
	heights[n] = 20.0
	heights[n + 1] = 30.0
	terrain._tiles[Vector2i(0, 0)] = {"node": Node3D.new(), "heights": heights}
	var cell := TerrainGenerator.CELL_SIZE
	_eq(terrain.get_height_at_world(0.0, 0.0), 0.0, "exact vertex (0,0)")
	_eq(terrain.get_height_at_world(cell, 0.0), 10.0, "exact vertex (1,0)")
	_eq(terrain.get_height_at_world(0.0, cell), 20.0, "exact vertex (0,1)")
	_eq(terrain.get_height_at_world(cell * 0.5, 0.0), 5.0, "edge midpoint averages its two vertices")
	_eq(terrain.get_height_at_world(cell * 0.5, cell * 0.5), 15.0, "cell centre averages all four corners")
	terrain._tiles[Vector2i(0, 0)]["node"].free()
	terrain.free()

func _test_terrain_deterministic_seamless() -> void:
	var regions := [_region(1, "Woodland")]
	var settlements := [_settlement(1, 10.0, 10.0)]
	var a := _terrain_with(regions, settlements, Vector2(10.0, 10.0))
	var b := _terrain_with(regions, settlements, Vector2(10.0, 10.0))
	_eq(a.tile_count() > 0, true, "tiles should generate around the focus")
	for point in [Vector2(64.0, 30.0), Vector2(128.0, -64.0), Vector2(-64.0, 100.0)]:
		var left: float = a.get_height_at_world(point.x - 0.0001, point.y)
		var right: float = a.get_height_at_world(point.x, point.y)
		_eq(absf(left - right) < 0.01, true, "heights must agree across the tile border at %s" % point)
		_eq(is_equal_approx(a.height_at(point.x, point.y), b.height_at(point.x, point.y)), true, "same world must give the same height at %s" % point)
	a.free()
	b.free()

func _test_terrain_covers_settlements() -> void:
	var terrain := _terrain_with([_region(1), _region(2)], [_settlement(1, 0.0, 0.0), _settlement(2, 3000.0, 3000.0, 2)], Vector2(3000.0, 3000.0))
	_eq(terrain.has_tile_at(3000.0, 3000.0), true, "a settlement far from the first must still stand on generated ground")
	terrain.apply_delta({"addedSettlements": [_settlement(3, 6000.0, -2000.0, 2)]})
	terrain.set_focus(Vector2(6000.0, -2000.0))
	terrain.build_pending_tiles()
	_eq(terrain.has_tile_at(6000.0, -2000.0), true, "a settlement founded later must get ground under it")
	_eq(terrain.has_tile_at(6300.0, -2000.0), true, "ground must extend well past the village edge")
	terrain.free()

func _test_terrain_flat_villages() -> void:
	var terrain := TerrainGenerator.new()
	terrain.configure([_region(1, "Marginal")], [_settlement(1, 0.0, 0.0)])
	var near := _height_range(terrain, 6.0)
	var far := _height_range(terrain, 150.0)
	_eq(near < 1.5, true, "village ground should be nearly flat (range %.2f)" % near)
	_eq(far > near * 2.0, true, "hills beyond the village should rise (near %.2f, far %.2f)" % [near, far])
	terrain.free()

func _height_range(terrain: Node3D, radius: float) -> float:
	var low := INF
	var high := -INF
	for index in 48:
		var angle := TAU * float(index) / 48.0
		var h: float = terrain.height_at(cos(angle) * radius, sin(angle) * radius)
		low = min(low, h)
		high = max(high, h)
	return high - low

func _test_terrain_collision() -> void:
	var terrain := _terrain_with([_region(1, "Woodland")], [_settlement(1, 0.0, 0.0)], Vector2.ZERO)
	var shape: HeightMapShape3D = terrain.collision_shape_at(100.0, 40.0)
	_eq(shape != null, true, "a generated tile must have a collision shape")
	if shape == null:
		terrain.free()
		return
	_eq(shape.map_width, TerrainGenerator.TILE_CELLS + 1)
	_eq(shape.map_depth, TerrainGenerator.TILE_CELLS + 1)
	var key: Vector2i = terrain._tile_key(100.0, 40.0)
	_eq(shape.map_data == terrain._tiles[key]["heights"], true, "collision must use the rendered height grid")
	var body: StaticBody3D = terrain._tiles[key]["node"].get_node("Collision")
	_eq(body.collision_layer, TerrainGenerator.WORLD_COLLISION_LAYER)
	_eq((body.get_child(0) as Node3D).scale, Vector3(TerrainGenerator.CELL_SIZE, 1.0, TerrainGenerator.CELL_SIZE))
	terrain.free()

func _test_terrain_colour() -> void:
	var terrain := TerrainGenerator.new()
	var rich := _region(1, "Woodland")
	rich["wildFoodStock"] = 100
	var poor := _region(2, "Woodland")
	poor["wildFoodStock"] = 0
	var marginal := _region(3, "Marginal")
	terrain.configure([rich, poor, marginal], [])
	var rich_colour: Color = terrain._color_from(5.0, 5.0, 0.0, 10.0, 1)
	_eq(rich_colour != terrain._color_from(5.0, 5.0, 0.0, 10.0, 2), true, "wild-food richness should tint the ground")
	_eq(rich_colour != terrain._color_from(5.0, 5.0, 0.0, 10.0, 3), true, "environment class should tint the ground")
	terrain.free()

func _test_terrain_fades_far() -> void:
	var terrain := TerrainGenerator.new()
	terrain.configure([_region(1, "Marginal")], [_settlement(1, 0.0, 0.0)])
	_eq(terrain.height_at(TerrainGenerator.FADE_END + 20.0, 0.0), 0.0, "relief must reach zero before the tiles end")
	terrain.free()

# ---------------------------------------------------------------- lighting

func _test_day_night_afternoon_anchor() -> void:
	_eq(is_equal_approx(DayNight.hour_of_day(0), 15.0), true, "tick 0 should read 15:00")
	var sample: Dictionary = DayNight.sample(0)
	var elevation: float = -float((sample["sun_rotation_degrees"] as Vector3).x)
	_eq(elevation > 30.0 and elevation < 60.0, true, "afternoon sun should sit well above the horizon (%.1f deg)" % elevation)
	_eq(float(sample["sun_energy"]) > 1.0, true, "afternoon sun should be at full daytime strength")

func _test_day_night_midday_vs_midnight() -> void:
	var midday: Dictionary = DayNight.sample(DayNight.tick_at_hour(12.0))
	var midnight: Dictionary = DayNight.sample(DayNight.tick_at_hour(0.0))
	_eq(float(midday["sun_energy"]) > float(midnight["sun_energy"]), true)
	_eq(float(midday["ambient_energy"]) > float(midnight["ambient_energy"]), true)
	_eq(DayNight.daylight_amount(DayNight.tick_at_hour(12.0)) > 0.99, true)
	_eq(DayNight.daylight_amount(DayNight.tick_at_hour(0.0)) < 0.01, true)

func _test_day_night_wraps() -> void:
	var same_time_day_three := DayNight.DAY_TICKS * 3 + DayNight.tick_at_hour(9.0)
	_eq(is_equal_approx(DayNight.daylight_amount(same_time_day_three), DayNight.daylight_amount(DayNight.tick_at_hour(9.0))), true)

# ---------------------------------------------------------------- buildings

func _test_building_construction_state() -> void:
	_eq(BuildingAssets.make_collision_body("Dwelling", "Planned", 0.0) == null, true, "a staked plot has nothing to collide with")
	var active := BuildingAssets.footprint("Dwelling", "ActiveConstruction", 0.5)
	var complete := BuildingAssets.footprint("Dwelling", "Complete", 1.0)
	_eq(active.y < complete.y, true, "walls under construction stand lower than the finished building")
	_eq(BuildingAssets.footprint("Dwelling", "ActiveConstruction", 0.9).y > BuildingAssets.footprint("Dwelling", "ActiveConstruction", 0.1).y, true, "wall height follows progress")
	var active_visual := BuildingAssets.make_building("Dwelling", "ActiveConstruction", 0.5)
	var complete_visual := BuildingAssets.make_building("Dwelling", "Complete", 1.0)
	_eq(_count_scenes(active_visual, "roof") == 0, true, "no roof until construction completes")
	_eq(_count_scenes(complete_visual, "roof") > 0, true, "a finished building has its roof")
	active_visual.free()
	complete_visual.free()

func _test_no_medieval_pieces() -> void:
	var nodes: Array = [BuildingAssets.make_settlement_marker()]
	for type in ["Dwelling", "Workshop", "FoodStore"]:
		for state in ["Planned", "ActiveConstruction", "Complete"]:
			nodes.append(BuildingAssets.make_building(type, state, 0.5))
	for node in nodes:
		for banned in ["fountain", "watermill", "stall", "lantern", "chimney", "banner"]:
			_eq(_count_scenes(node, banned), 0, "%s must not appear in %s" % [banned, node.name])
		node.free()

func _count_scenes(node: Node, token: String) -> int:
	var count := 1 if node.scene_file_path.get_file().contains(token) else 0
	for child in node.get_children():
		count += _count_scenes(child, token)
	return count

func _test_world_view_building_collision() -> void:
	var view := WorldView.new()
	_root().add_child(view)
	view.apply_snapshot({"agents": [], "settlements": [_settlement(1, 0.0, 0.0)], "buildings": [_building(7, 1, 4.0, 0.0)]})
	var area: Node = view._building_nodes[7]
	var body: StaticBody3D = area.get_node_or_null("Collision")
	_eq(body != null, true, "a standing building needs collision for the avatar")
	if body != null:
		_eq(body.collision_layer, BuildingAssets.WORLD_COLLISION_LAYER)
	view.queue_free()

func _test_world_view_no_building_churn() -> void:
	var view := WorldView.new()
	_root().add_child(view)
	view.apply_snapshot({"agents": [], "settlements": [_settlement(1, 0.0, 0.0)], "buildings": [_building(7, 1, 4.0, 0.0, "Dwelling", "ActiveConstruction", 0.31)]})
	var first: Node = view._building_nodes[7]
	view.apply_delta({"updatedBuildings": [_building(7, 1, 4.0, 0.0, "Dwelling", "ActiveConstruction", 0.33)], "updatedSettlements": [_settlement(1, 0.0, 0.0)]})
	_eq(view._building_nodes[7] == first, true, "an unchanged-looking building must keep its node")
	_eq(float(view.building_data(7)["progress"]), 0.33, "inspector data still refreshes")
	view.apply_delta({"updatedBuildings": [_building(7, 1, 4.0, 0.0, "Dwelling", "ActiveConstruction", 0.62)]})
	_eq(view._building_nodes[7] != first, true, "a visible construction step rebuilds the building")
	view.queue_free()

func _test_world_view_building_facing() -> void:
	var view := WorldView.new()
	_root().add_child(view)
	view.apply_snapshot({"agents": [], "settlements": [_settlement(1, 0.0, 0.0)], "buildings": [_building(7, 1, 4.0, 0.0), _building(8, 1, 0.0, -4.0)]})
	for id in [7, 8]:
		var area: Node3D = view._building_nodes[id]
		var door_direction := area.transform.basis.x.normalized()
		var to_centre := (Vector3.ZERO - area.position)
		to_centre.y = 0.0
		_eq(door_direction.dot(to_centre.normalized()) > 0.99, true, "building %d's door should face the common" % id)
	view.queue_free()

func _faced_building(id: int, settlement_id: int, x: float, z: float, facing_degrees: int) -> Dictionary:
	var building := _building(id, settlement_id, x, z)
	building["facingDegrees"] = facing_degrees
	return building

func _test_world_view_canonical_facing() -> void:
	var view := WorldView.new()
	_root().add_child(view)
	view.apply_snapshot({"agents": [], "settlements": [_settlement(1, 0.0, 0.0)], "buildings": [_faced_building(7, 1, 20.0, 5.0, 90), _faced_building(8, 1, -20.0, 5.0, 180)]})
	var expected := {7: Vector3(0.0, 0.0, 1.0), 8: Vector3(-1.0, 0.0, 0.0)}
	for id in expected.keys():
		var door_direction: Vector3 = (view._building_nodes[id] as Node3D).transform.basis.x.normalized()
		_eq(door_direction.dot(expected[id]) > 0.99, true, "building %d's door should point along facingDegrees in world x/z" % id)
	view.queue_free()

func _test_canonical_tracks_from_facing() -> void:
	# Two dwellings on one track heading +X, set back 6 units to the -Z side
	# and facing back (+Z, 90 degrees) onto it, as simulation.cpp places them.
	var dressing := _dressing_with({
		"settlements": [_settlement(1, 0.0, 0.0)],
		"buildings": [_faced_building(20, 1, 12.0, -6.0, 90), _faced_building(21, 1, 21.0, -6.0, 90)],
		"agents": [],
	})
	dressing.rebuild_region(1)
	var track_segments: Array = []
	var path_ends: Array = []
	for segment in dressing._layouts[1]["segments"]:
		if float(segment[2]) == VillageDressing.TRACK_WIDTH:
			track_segments.append(segment)
		else:
			path_ends.append(segment[1])
	_eq(path_ends.has(Vector2(12.0, 0.0)) and path_ends.has(Vector2(21.0, 0.0)), true, "door paths should end on the track points (%s)" % str(path_ends))
	_eq(track_segments.size(), 2, "one track chained from the common through both track points")
	_eq(dressing.blocks(Vector2(16.5, 0.0), 0.0), true, "the canonical track is not free ground")
	_free_dressing(dressing)

# ----------------------------------------------------------------- dressing

func _dressing_with(payload: Dictionary) -> Node3D:
	var terrain := TerrainGenerator.new()
	terrain.configure(payload.get("regions", [_region(1)]), payload.get("settlements", []))
	var dressing := VillageDressing.new()
	dressing.set_terrain(terrain)
	dressing.apply_snapshot(payload)
	dressing.set_meta("terrain", terrain)
	return dressing

func _free_dressing(dressing: Node3D) -> void:
	(dressing.get_meta("terrain") as Node).free()
	dressing.free()

func _village_payload() -> Dictionary:
	return {
		"settlements": [_settlement(1, 0.0, 0.0)],
		"buildings": [_building(10, 1, 4.0, 0.0)],
		"agents": [
			_person(1, 1, 1, "SubsistenceCultivator"),
			_person(2, 1, 1, "SubsistenceCultivator"),
			_person(3, 1, 2, "WildFoodGatherer"),
			_person(4, 1, 3, "SubsistenceCultivator"),
			_person(5, 1, 3, "SubsistenceCultivator", false),
		],
	}

func _test_fields_follow_cultivators() -> void:
	var dressing := _dressing_with(_village_payload())
	dressing.rebuild_region(1)
	var fields: Array = dressing.fields_in_region(1)
	_eq(fields.size(), 2, "one plot per household with living cultivators")
	var by_household := {}
	for field in fields:
		by_household[field["household_id"]] = field
	_eq(float(by_household[1]["length"]) > float(by_household[3]["length"]), true, "more cultivators, longer plot")
	dressing.apply_delta({"addedAgents": [_person(6, 1, 3, "SubsistenceCultivator")]})
	dressing.rebuild_region(1)
	for field in dressing.fields_in_region(1):
		if int(field["household_id"]) == 3:
			_eq(int(field["cultivators"]), 2, "a new cultivator grows the household's plot")
	dressing.apply_delta({"removedAgentIds": [1, 2]})
	dressing.rebuild_region(1)
	_eq(dressing.fields_in_region(1).size(), 1, "a household with no cultivators left loses its plot")
	_free_dressing(dressing)

func _test_fields_deterministic_non_overlapping() -> void:
	var payload := _village_payload()
	payload["settlements"].append(_settlement(2, 30.0, 12.0))
	payload["buildings"].append(_building(11, 2, 34.0, 12.0, "Workshop"))
	for id in range(10, 22):
		payload["agents"].append(_person(id, 1 + id % 2, 100 + id, "SubsistenceCultivator"))
	var a := _dressing_with(payload)
	var b := _dressing_with(payload)
	a.rebuild_region(1)
	b.rebuild_region(1)
	var fields_a: Array = a.fields_in_region(1)
	var fields_b: Array = b.fields_in_region(1)
	_eq(fields_a.size(), fields_b.size())
	_eq(fields_a.size() >= 8, true, "most cultivator households should get a plot (%d)" % fields_a.size())
	for index in fields_a.size():
		_eq(fields_a[index]["centre"], fields_b[index]["centre"], "placement must be deterministic")
		for other in range(index + 1, fields_a.size()):
			_eq(a._fields_overlap(fields_a[index], fields_a[other], 0.0), false, "fields must not overlap")
		for settlement in payload["settlements"]:
			var centre := Vector2(settlement["position"]["x"], settlement["position"]["z"])
			_eq(VillageDressing._field_contains(fields_a[index], centre, VillageDressing.MIN_CORE_RADIUS - 0.5), false, "fields stay off settlement cores")
		_eq(VillageDressing._field_contains(fields_a[index], Vector2(34.0, 12.0), 2.0), false, "fields stay off buildings")
	_free_dressing(a)
	_free_dressing(b)

func _test_tracks_join_settlements() -> void:
	var dressing := _dressing_with({"settlements": [_settlement(1, 0.0, 0.0), _settlement(2, 30.0, 0.0), _settlement(3, 0.0, 40.0), _settlement(4, 900.0, 0.0, 2)], "buildings": [], "agents": []})
	dressing.rebuild_region(1)
	var tracks := 0
	for segment in dressing._layouts[1]["segments"]:
		if float(segment[2]) == VillageDressing.TRACK_WIDTH:
			tracks += 1
	_eq(tracks, 2, "a spanning tree over three settlements has two tracks")
	_eq(dressing.blocks(Vector2(15.0, 0.0), 0.0), true, "the track between neighbours is not free ground")
	_free_dressing(dressing)

# --------------------------------------------------------------- vegetation

func _vegetation_positions(wood_stock: int) -> Dictionary:
	var regions := [_region(1, "Woodland", wood_stock)]
	var payload := _village_payload()
	payload["regions"] = regions
	var terrain := _terrain_with(regions, payload["settlements"], Vector2.ZERO)
	var dressing := VillageDressing.new()
	dressing.set_terrain(terrain)
	dressing.apply_snapshot(payload)
	dressing.rebuild_region(1)
	var scatter := VegetationScatter.new()
	scatter.setup(terrain, dressing)
	scatter.sync_chunks()
	var result := {"trees": 0, "stumps": 0, "points": [], "blocked": 0}
	var tree_paths: Array = []
	for tree_scene in VegetationScatter.TREES:
		tree_paths.append(tree_scene.resource_path)
	var keys: Array = scatter._chunks.keys()
	keys.sort()
	for key in keys:
		var placements: Dictionary = scatter.placements(key)
		var paths: Array = placements.keys()
		paths.sort()
		for path in paths:
			var points: Array = placements[path]
			for point in points:
				result["points"].append(point)
				if dressing.blocks(point, 0.0):
					result["blocked"] += 1
			if path == VegetationScatter.STUMP.resource_path:
				result["stumps"] += points.size()
			elif tree_paths.has(path):
				result["trees"] += points.size()
	scatter.free()
	dressing.free()
	terrain.free()
	return result

func _test_vegetation_clear_and_deterministic() -> void:
	var first := _vegetation_positions(1000)
	var second := _vegetation_positions(1000)
	_eq(first["points"].size() > 100, true, "the scatter should place plenty of plants (%d)" % first["points"].size())
	_eq(first["points"] == second["points"], true, "the same world must scatter identically")
	_eq(first["blocked"], 0, "nothing may grow on fields, paths, commons or buildings")

func _test_forest_follows_wood_stock() -> void:
	var full := _vegetation_positions(1000)
	var half := _vegetation_positions(500)
	_eq(full["trees"] > 0, true, "a full woodland stock grows a forest ring")
	_eq(half["trees"] < full["trees"], true, "a harvested region has fewer standing trees")
	_eq(half["stumps"] > full["stumps"], true, "felled trees remain as stumps")
	_eq(half["trees"] + half["stumps"], full["trees"] + full["stumps"], "every felled tree leaves exactly one stump")

func _test_foliage_prepared() -> void:
	var grass: Mesh = FoliageLib.prepared(VegetationScatter.GRASS)["mesh"]
	var normals: PackedVector3Array = grass.surface_get_arrays(0)[Mesh.ARRAY_NORMAL]
	var all_up := true
	for normal in normals:
		all_up = all_up and normal.y > 0.999
	_eq(normals.size() > 0, true, "grass should have normals")
	_eq(all_up, true, "grass normals must all face up so no blade renders black")
	var material: BaseMaterial3D = grass.surface_get_material(0)
	_eq(material.albedo_color.h < 0.38, true, "mint Nature Kit green should be shifted toward leaf green (hue %.2f)" % material.albedo_color.h)
