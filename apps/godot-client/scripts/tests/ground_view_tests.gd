extends RefCounted

## Ticket #57 (player avatar and descent) tests, run by run_tests.gd through
## its _run_test/_run_async_test/_assert_eq helpers.

const GroundView := preload("res://scripts/ground/ground_view.gd")
const PlayerAvatar := preload("res://scripts/ground/player_avatar.gd")
const WorldView := preload("res://scripts/world/world_view.gd")
const BuildingAssets := preload("res://scripts/world/building_assets.gd")

var _runner

func _init(runner) -> void:
	_runner = runner

func run() -> void:
	_test("ground view selects the villager under the aim over a nearer off-aim one", _test_pick_prefers_aim)
	_test("ground view falls back to the nearest villager within reach, else none", _test_pick_falls_back_to_nearest)
	_test("ground view shows only aimed and selected labels; command view shows all", _test_label_policy)
	_test("avatar collides with world and villager layers, not pick areas", _test_collision_layers)
	_test("ground camera sits the arm length behind and above the avatar, facing it", _test_ground_camera_transform)
	_test("ground view gives each villager one blocker and hides other labels on the ground", _test_ground_view_blockers_and_labels)

func run_async() -> void:
	await _runner._run_async_test("avatar is stopped by a building box and slides, never entering it", _test_avatar_blocked_by_building)
	await _runner._run_async_test("avatar is stopped by a villager capsule and never moves the villager", _test_avatar_blocked_by_villager)

func _test(name: String, fn: Callable) -> void:
	_runner._run_test(name, fn)

func _eq(actual, expected, context: String = "") -> void:
	_runner._assert_eq(actual, expected, context)

func _root() -> Window:
	return _runner.root

# ----------------------------------------------------------------- pure

func _test_pick_prefers_aim() -> void:
	var positions := {
		1: Vector3(1.5, 0.0, 1.5),   # nearer, but 45 degrees off the aim
		2: Vector3(0.0, 0.0, -6.0),  # straight down the aim
	}
	_eq(GroundView.pick_target(Vector3.ZERO, Vector3(0.0, -0.3, -1.0), positions), 2)

func _test_pick_falls_back_to_nearest() -> void:
	var positions := {5: Vector3(2.0, 0.0, 2.0), 6: Vector3(3.0, 0.0, 3.0)}
	_eq(GroundView.pick_target(Vector3.ZERO, Vector3(0.0, 0.0, -1.0), positions), 5, "nothing aimed at: nearest within reach")
	_eq(GroundView.pick_target(Vector3.ZERO, Vector3(0.0, 0.0, -1.0), {7: Vector3(20.0, 0.0, 20.0)}), -1, "nobody aimed at or in reach")
	_eq(GroundView.pick_target(Vector3.ZERO, Vector3(0.0, 0.0, -1.0), {8: Vector3(0.0, 0.0, -20.0)}), -1, "aimed at but beyond aim range")

func _test_label_policy() -> void:
	_eq(GroundView.label_visible(false, 3, -1, -1), true, "command view shows every label")
	_eq(GroundView.label_visible(true, 3, -1, -1), false, "ground view hides unrelated labels")
	_eq(GroundView.label_visible(true, 3, 3, -1), true, "aimed villager keeps its label")
	_eq(GroundView.label_visible(true, 3, -1, 3), true, "selected villager keeps its label")

func _test_collision_layers() -> void:
	var avatar := PlayerAvatar.new()
	_root().add_child(avatar)
	_eq((avatar.collision_mask & PlayerAvatar.WORLD_LAYER) != 0, true)
	_eq((avatar.collision_mask & PlayerAvatar.VILLAGER_LAYER) != 0, true)
	_eq(avatar.collision_mask & 1, 0, "pick areas on layer 1 must not block the avatar")
	_eq(PlayerAvatar.WORLD_LAYER, BuildingAssets.WORLD_COLLISION_LAYER, "avatar must collide with the look's building/terrain layer")
	var blocker := GroundView.make_villager_blocker()
	_eq(blocker.collision_layer, PlayerAvatar.VILLAGER_LAYER)
	_eq(blocker.collision_mask, 0, "villager blockers never push anything")
	blocker.free()
	avatar.queue_free()

func _test_ground_camera_transform() -> void:
	var target := Vector3(10.0, 2.0, -4.0)
	var pose := GroundView.ground_camera_transform(target, 0.0, GroundView.DEFAULT_PITCH)
	var pivot := target + Vector3.UP * GroundView.PIVOT_HEIGHT
	_eq(is_equal_approx(pose.origin.distance_to(pivot), GroundView.ARM_LENGTH), true)
	_eq(pose.origin.y > pivot.y, true, "a downward pitch puts the camera above the pivot")
	var forward := -pose.basis.z
	_eq(forward.dot((pivot - pose.origin).normalized()) > 0.99, true, "camera looks at the avatar")

func _test_ground_view_blockers_and_labels() -> void:
	var view := WorldView.new()
	_root().add_child(view)
	view.apply_snapshot({"agents": [
		{"id": 1, "sex": "Female", "position": {"x": 0.0, "y": 0.0, "z": -5.0}, "currentActivity": "Work"},
		{"id": 2, "sex": "Male", "position": {"x": 20.0, "y": 0.0, "z": 20.0}, "currentActivity": "Sleep"},
	], "settlements": [], "buildings": []})
	var orbit := Camera3D.new()
	var terrain := Node3D.new()
	var ground := GroundView.new()
	ground.setup(orbit, terrain, view, func(_t: String, _id: int) -> void: pass)
	_root().add_child(ground)
	ground._sync_villager_blockers()
	ground._sync_villager_blockers()
	for id in [1, 2]:
		_eq(view.agent_node(id).find_children("AvatarBlocker", "StaticBody3D", false, false).size(), 1, "exactly one blocker on villager %d" % id)
	ground.mode = GroundView.Mode.GROUND
	ground._aimed_id = 1
	ground._apply_label_policy()
	_eq(view.agent_label(1).visible, true)
	_eq(view.agent_label(2).visible, false)
	ground.mode = GroundView.Mode.COMMAND
	ground._apply_label_policy()
	_eq(view.agent_label(2).visible, true)
	ground.queue_free()
	view.queue_free()
	orbit.free()
	terrain.free()

# ---------------------------------------------------------------- physics

func _floor() -> StaticBody3D:
	var body := StaticBody3D.new()
	body.collision_layer = PlayerAvatar.WORLD_LAYER
	var shape := BoxShape3D.new()
	shape.size = Vector3(60.0, 1.0, 60.0)
	var collision := CollisionShape3D.new()
	collision.shape = shape
	collision.position.y = -0.5
	body.add_child(collision)
	return body

func _walk(avatar: CharacterBody3D, input: Vector2, frames: int) -> void:
	avatar.auto_input = input
	for i in frames:
		await _runner.physics_frame
	avatar.auto_input = Vector2.ZERO

func _test_avatar_blocked_by_building() -> void:
	var floor := _floor()
	_root().add_child(floor)
	var wall := StaticBody3D.new()
	wall.collision_layer = PlayerAvatar.WORLD_LAYER
	var shape := BoxShape3D.new()
	shape.size = Vector3(3.0, 3.0, 3.0)
	var collision := CollisionShape3D.new()
	collision.shape = shape
	collision.position = Vector3(0.0, 1.5, -5.0)
	wall.add_child(collision)
	_root().add_child(wall)
	var avatar := PlayerAvatar.new()
	_root().add_child(avatar)
	avatar.position = Vector3(0.0, 0.1, 0.0)
	await _runner.physics_frame
	await _walk(avatar, Vector2(0.0, -1.0), 180)
	_eq(avatar.global_position.z > -3.5 + PlayerAvatar.CAPSULE_RADIUS - 0.05, true, "avatar must stop at the wall face (z=%.2f)" % avatar.global_position.z)
	_eq(avatar.global_position.z < -2.0, true, "avatar should have walked up to the wall (z=%.2f)" % avatar.global_position.z)
	avatar.queue_free()
	wall.queue_free()
	floor.queue_free()

func _test_avatar_blocked_by_villager() -> void:
	var floor := _floor()
	_root().add_child(floor)
	var villager := Node3D.new()
	villager.position = Vector3(0.0, 0.0, -4.0)
	villager.add_child(GroundView.make_villager_blocker())
	_root().add_child(villager)
	var avatar := PlayerAvatar.new()
	_root().add_child(avatar)
	avatar.position = Vector3(0.0, 0.1, 0.0)
	await _runner.physics_frame
	await _walk(avatar, Vector2(0.0, -1.0), 150)
	var gap := Vector2(avatar.global_position.x, avatar.global_position.z).distance_to(Vector2(0.0, -4.0))
	_eq(gap > PlayerAvatar.CAPSULE_RADIUS + GroundView.VILLAGER_CAPSULE_RADIUS - 0.05, true, "avatar must not overlap the villager (gap %.2f)" % gap)
	_eq(villager.position, Vector3(0.0, 0.0, -4.0), "the villager must never be pushed")
	avatar.queue_free()
	villager.queue_free()
	floor.queue_free()
