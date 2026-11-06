extends RefCounted

## Tests for the client performance work: cached villager goals, the per-frame
## walking bounds and the streaming time budget. Run by run_tests.gd through its
## _run_test/_assert_eq helpers so failures count toward the same total.
## The stale-cache cases matter most: a cache that outlives the thing it was
## derived from would walk villagers to a field that is no longer there.

const WorldView := preload("res://scripts/world/world_view.gd")
const TerrainGenerator := preload("res://scripts/terrain_generator.gd")
const VegetationScatter := preload("res://scripts/world/vegetation_scatter.gd")

var _runner

## A dressing whose fields the test can move, reporting a layout version the
## way the real VillageDressing does.
class MovableDressing extends Node3D:
	var field_centre := Vector2(50.0, 0.0)
	var version := 1

	func fields_in_region(_region_id: int) -> Array:
		return [{"household_id": 17, "centre": field_centre}]

	func layout_version() -> int:
		return version


class UnversionedDressing extends Node3D:
	var field_centre := Vector2(50.0, 0.0)

	func fields_in_region(_region_id: int) -> Array:
		return [{"household_id": 17, "centre": field_centre}]

## Stands in for VillageDressing in the vegetation scatter: only the exclusions
## it reports matter, and the test controls them.
class FakeExclusions extends Node3D:
	var segments: Array = []

	func exclusions_in(_area: Rect2) -> Dictionary:
		return {"cores": [], "buildings": [], "segments": segments.duplicate(), "fields": []}

func _init(runner) -> void:
	_runner = runner

func run() -> void:
	_test("pacer never limits jobs at a healthy frame rate", _test_pacer_idle_at_healthy_frame_rate)
	_test("pacer limits jobs while frames run long and releases when they recover", _test_pacer_limits_then_recovers)
	_test("pacer spends credit on the measured cost of each job", _test_pacer_charges_cost)
	_test("terrain build_pending_tiles defers tiles when the pacer says no", _test_terrain_paced)
	_test("world_view re-resolves a field slot when the dressing re-lays its fields", _test_slot_follows_layout_version)
	_test("world_view never caches a field slot for a dressing without a layout version", _test_slot_unversioned_not_cached)
	_test("world_view keeps cached slots when an update changes nothing and drops them when it does", _test_slot_invalidated_by_changed_facts)
	_test("world_view re-seats a standing villager on changed ground", _test_standing_villager_regrounds)
	_test("world_view caps route plans per frame and still gets everyone walking", _test_route_plans_capped)
	_test("vegetation rebuilds a chunk for terrain changes but skips unchanged exclusions", _test_vegetation_skips_unchanged_exclusions)

func _test(name: String, fn: Callable) -> void:
	_runner._run_test(name, fn)

func _eq(actual, expected, context: String = "") -> void:
	_runner._assert_eq(actual, expected, context)

func _root() -> Window:
	return _runner.root

# ------------------------------------------------------------------ pacer

func _test_pacer_idle_at_healthy_frame_rate() -> void:
	var pacer := StreamPacer.new()
	for _frame in range(120):
		pacer.begin_frame(1.0 / 60.0)
		_eq(pacer.can_run(), true, "a 60 fps frame never blocks a streaming job")
		pacer.charge(50000)
	_eq(pacer.is_under_pressure(), false)

func _test_pacer_limits_then_recovers() -> void:
	var pacer := StreamPacer.new()
	for _frame in range(10):
		pacer.begin_frame(0.060)
	_eq(pacer.is_under_pressure(), true, "60 ms frames put the pacer under pressure")
	for _frame in range(30):
		pacer.begin_frame(1.0 / 60.0)
	_eq(pacer.is_under_pressure(), false, "it lets go again once frames are healthy")
	_eq(pacer.can_run(), true)

func _test_pacer_charges_cost() -> void:
	var pacer := StreamPacer.new()
	for _frame in range(10):
		pacer.begin_frame(0.060)
	_eq(pacer.can_run(), true, "a frame under pressure starts with credit")
	pacer.charge(40000)
	_eq(pacer.can_run(), false, "a 40 ms job spends more than the frame's credit")
	var waited := 0
	while not pacer.can_run() and waited < 20:
		pacer.begin_frame(0.060)
		waited += 1
	_eq(waited >= 3 and waited < 20, true, "the debt is paid back over a few frames (%d)" % waited)

func _test_terrain_paced() -> void:
	var terrain := TerrainGenerator.new()
	_root().add_child(terrain)
	terrain.configure([_region(1)], [_settlement(1, 0.0, 0.0)])
	terrain.set_focus(Vector2.ZERO)
	var pacer := StreamPacer.new()
	terrain.set_pacer(pacer)
	for _frame in range(10):
		pacer.begin_frame(0.060)
	terrain.build_pending_tiles(100)
	var paced := terrain.tile_count()
	_eq(paced >= 1 and paced < 6, true, "under pressure only a few tiles are built in one frame (%d)" % paced)
	for _frame in range(40):
		pacer.begin_frame(1.0 / 60.0)
	terrain.build_pending_tiles()
	_eq(terrain.tile_count() > paced, true, "the deferred tiles are built once frames recover")
	terrain.free()

# --------------------------------------------------------------- world view

func _field_worker(view: WorldView) -> void:
	view.apply_snapshot({
		"agents": [_field_agent(9)],
		"settlements": [], "buildings": [],
	})
	view.set_simulation_playback(false, 1)
	view.set_tick(500)

func _field_agent(id: int) -> Dictionary:
	return {
		"id": id, "sex": "Female", "position": {"x": 0.0, "y": 0.0, "z": 0.0},
		"householdId": 17, "regionId": 1,
		"currentActivity": "Work", "targetKind": "Field",
		"target": null, "departTick": 100, "returnDepartTick": 1000000,
	}

func _test_slot_follows_layout_version() -> void:
	var view := WorldView.new()
	_root().add_child(view)
	var dressing := MovableDressing.new()
	_root().add_child(dressing)
	view.set_village_dressing(dressing)
	_field_worker(view)
	var first := view._resolve_agent_position(9)
	_eq(first.x > 45.0 and first.x < 60.0, true, "the goal is the household's field")
	dressing.field_centre = Vector2(-80.0, 40.0)
	_eq(view._resolve_agent_position(9).x > 45.0, true, "without a version bump the cached slot stands")
	dressing.version += 1
	var moved := view._resolve_agent_position(9)
	_eq(moved.x < -70.0 and moved.z > 30.0, true, "a re-laid field moves the goal as soon as the layout version changes (%s)" % str(moved))
	view.queue_free()
	dressing.queue_free()

func _test_slot_unversioned_not_cached() -> void:
	var view := WorldView.new()
	_root().add_child(view)
	var dressing := UnversionedDressing.new()
	_root().add_child(dressing)
	view.set_village_dressing(dressing)
	_field_worker(view)
	_eq(view._resolve_agent_position(9).x > 45.0, true)
	dressing.field_centre = Vector2(-80.0, 40.0)
	_eq(view._resolve_agent_position(9).x < -70.0, true, "a dressing that cannot report changes is always asked afresh")
	view.queue_free()
	dressing.queue_free()

func _test_slot_invalidated_by_changed_facts() -> void:
	var view := WorldView.new()
	_root().add_child(view)
	var snapshot_agent := {
		"id": 11, "sex": "Male", "position": {"x": 0.0, "y": 0.0, "z": 0.0},
		"currentActivity": "Work", "targetKind": "Common",
		"target": {"x": 100.0, "z": 0.0}, "departTick": 100, "returnDepartTick": 1000000,
	}
	view.apply_snapshot({"agents": [snapshot_agent], "settlements": [], "buildings": []})
	view.set_tick(500)
	var before := view._resolve_agent_position(11)
	var slots_before: Dictionary = view._agent_slots[11]
	view.apply_delta({"updatedAgents": [snapshot_agent.duplicate(true)]})
	_eq(view._agent_slots[11] == slots_before and is_same(view._agent_slots[11], slots_before), true, "an update that changes nothing keeps the cached slots")
	var moved := snapshot_agent.duplicate(true)
	moved["target"] = {"x": -100.0, "z": 0.0}
	view.apply_delta({"updatedAgents": [moved]})
	var after := view._resolve_agent_position(11)
	_eq(before.x > 80.0 and after.x < -80.0, true, "a changed target is never answered from the old slot (%s -> %s)" % [str(before), str(after)])
	view.queue_free()

func _test_standing_villager_regrounds() -> void:
	var view := WorldView.new()
	_root().add_child(view)
	var ground := [0.0]
	view.apply_snapshot({
		"agents": [{"id": 5, "sex": "Female", "position": {"x": 4.0, "y": 0.0, "z": 2.0}, "currentActivity": "Idle", "targetKind": "Home"}],
		"settlements": [], "buildings": [],
	}, func(_x: float, _z: float) -> float: return ground[0])
	view.set_simulation_playback(false, 1)
	var node: Node3D = view.get_child(0)
	for _frame in range(3):
		view._process(0.016)
	_eq(node.position.y, 0.0)
	ground[0] = 2.5
	for _frame in range(WorldView.HOLD_REGROUND_FRAMES + 1):
		view._process(0.016)
	_eq(is_equal_approx(node.position.y, 2.5), true, "a villager standing still follows ground that changed under them (%f)" % node.position.y)
	view.queue_free()

func _test_route_plans_capped() -> void:
	var view := WorldView.new()
	_root().add_child(view)
	var agents: Array = []
	var count := WorldView.MAX_ROUTE_PLANS_PER_FRAME * 3
	for id in range(1, count + 1):
		agents.append({
			"id": id, "sex": "Female", "position": {"x": float(id), "y": 0.0, "z": 0.0},
			"currentActivity": "Work", "targetKind": "Common",
			"target": {"x": 200.0, "z": 0.0}, "departTick": 100, "returnDepartTick": 1000000,
		})
	view.apply_snapshot({"agents": agents, "settlements": [], "buildings": []})
	view.set_simulation_playback(false, 1)
	view.set_tick(50)
	view._process(0.016)
	# Everyone is at home and idle until the departure tick.
	_eq(view._agent_routes.size(), 0, "no one plans a route while there is nowhere to go")
	view.set_tick(500)
	view._process(0.016)
	_eq(view._agent_routes.size(), WorldView.MAX_ROUTE_PLANS_PER_FRAME, "only the cap's worth of agents plan in the first frame")
	for _frame in range(4):
		view._process(0.016)
	_eq(view._agent_routes.size(), count, "the rest plan on the following frames")
	view.queue_free()

# --------------------------------------------------------------- vegetation

func _region(id: int) -> Dictionary:
	return {"id": id, "environmentClass": "ProductiveArable", "landAreaUnits": 100, "wildFoodCapacity": 100, "wildFoodStock": 50, "woodCapacity": 1000, "woodStock": 1000, "stoneCapacity": 500, "stoneRemaining": 500}

func _settlement(id: int, x: float, z: float) -> Dictionary:
	return {"id": id, "regionId": 1, "active": true, "position": {"x": x, "y": 0.0, "z": z}}

func _test_vegetation_skips_unchanged_exclusions() -> void:
	var terrain := TerrainGenerator.new()
	_root().add_child(terrain)
	terrain.configure([_region(1)], [_settlement(1, 0.0, 0.0)])
	terrain.set_focus(Vector2.ZERO)
	terrain.build_pending_tiles()
	var dressing := FakeExclusions.new()
	var scatter := VegetationScatter.new()
	scatter.setup(terrain, dressing)
	scatter.sync_chunks()
	var keys: Array = scatter._chunks.keys()
	keys.sort()
	var key: Vector2i = keys[0]
	var everywhere := Rect2(-1.0e6, -1.0e6, 2.0e6, 2.0e6)
	var built: Node = scatter._chunks[key]

	scatter.mark_exclusions_dirty(everywhere)
	_sync_until_settled(scatter)
	_eq(is_same(scatter._chunks[key], built), true, "unchanged exclusions leave the chunk alone")
	_eq(scatter._soft_dirty.is_empty(), true, "and the chunk is no longer marked")

	scatter.mark_dirty(everywhere)
	scatter.sync_chunks()
	_eq(not is_same(scatter._chunks[key], built), true, "a terrain or settlement change always rebuilds")

	var current: Node = scatter._chunks[key]
	var rect: Rect2 = scatter._chunk_rect(key)
	dressing.segments = [[rect.get_center(), rect.get_center() + Vector2(20.0, 0.0), 2.0]]
	scatter.mark_exclusions_dirty(everywhere)
	_sync_until_settled(scatter)
	_eq(not is_same(scatter._chunks[key], current), true, "changed exclusions rebuild the chunk")
	scatter.free()
	dressing.free()
	terrain.free()

## Unchanged-exclusion checks are capped per sync, so a settle takes a few.
func _sync_until_settled(scatter: Node3D) -> void:
	for _pass in range(40):
		scatter.sync_chunks()
		if scatter._soft_dirty.is_empty():
			return
