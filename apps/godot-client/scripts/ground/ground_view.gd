extends Node3D

## Command view <-> ground view (ticket #57, from the #49 prototype).
##
## - G or the on-screen button descends from the overhead command view to a
##   third-person player avatar at the point the camera is looking at, with
##   an eased camera flight, and returns the same way.
## - The avatar collides with the terrain tiles and building boxes (world
##   layer) and with villagers, which get kinematic capsules it slides past.
##   Villager positions stay server-owned: nothing here ever moves them.
## - E selects the villager under the camera aim, falling back to the
##   nearest within reach, through the panel registry (same entity ID as a
##   command-view click). The target is highlighted before E is pressed.
## - In the ground view only the aimed-at and selected villagers show
##   activity labels; the command view shows them all.

const PlayerAvatarScript := preload("res://scripts/ground/player_avatar.gd")

const TRANSITION_SECONDS := 1.4
const ARM_LENGTH := 5.5
const ARM_MIN := 2.0
const ARM_MAX := 12.0
const ARM_STEP := 0.5
const PIVOT_HEIGHT := 1.6
const DEFAULT_PITCH := -0.32
const PITCH_MIN := -1.2
const PITCH_MAX := 0.35
const LOOK_SPEED := 0.006
const GROUND_FOV := 70.0
const INTERACT_RADIUS := 3.5
const AIM_RANGE := 9.0
const AIM_MAX_ANGLE := 0.26 # about 15 degrees
const LANDING_CLEARANCE := 4.0
const LANDING_STEPS := 25
const VILLAGER_CAPSULE_RADIUS := 0.35
const VILLAGER_CAPSULE_HEIGHT := 1.7
const BLOCKER_META := "avatar_blocker"

enum Mode { COMMAND, DESCENDING, GROUND, ASCENDING }

signal mode_changed(mode: int)

var mode := Mode.COMMAND

var _orbit: Camera3D
var _terrain: Node3D
var _world_view: Node3D
var _select_sink: Callable

var _avatar: CharacterBody3D
var _pivot: Node3D
var _arm: SpringArm3D
var _ground_camera: Camera3D
var _transition_camera: Camera3D
var _highlight: MeshInstance3D
var _button: Button
var _looking := false
var _yaw := 0.0
var _pitch := DEFAULT_PITCH
var _aimed_id := -1
var _selected_id := -1

## `select_sink` is called as (entity_type: String, entity_id: int).
func setup(orbit: Camera3D, terrain: Node3D, world_view: Node3D, select_sink: Callable) -> void:
	_orbit = orbit
	_terrain = terrain
	_world_view = world_view
	_select_sink = select_sink

func _ready() -> void:
	name = "GroundView"
	_avatar = PlayerAvatarScript.new()
	_avatar.name = "PlayerAvatar"
	add_child(_avatar)
	_set_avatar_active(false)

	_pivot = Node3D.new()
	_pivot.name = "GroundPivot"
	add_child(_pivot)
	_arm = SpringArm3D.new()
	_arm.spring_length = ARM_LENGTH
	_arm.collision_mask = PlayerAvatarScript.WORLD_LAYER
	_arm.margin = 0.3
	_pivot.add_child(_arm)
	_ground_camera = Camera3D.new()
	_ground_camera.name = "GroundCamera"
	_ground_camera.fov = GROUND_FOV
	_arm.add_child(_ground_camera)

	_transition_camera = Camera3D.new()
	_transition_camera.name = "TransitionCamera"
	add_child(_transition_camera)

	_highlight = _make_highlight()
	add_child(_highlight)
	_build_button()

	var capture_dir := OS.get_environment("UNIVERSECORE_DESCENT_CAPTURE_DIR")
	if capture_dir != "":
		_capture(capture_dir)

func _process(_delta: float) -> void:
	_sync_villager_blockers()
	if mode == Mode.GROUND:
		_pivot.global_position = _avatar.global_position + Vector3.UP * PIVOT_HEIGHT
		_pivot.rotation = Vector3(_pitch, _yaw, 0.0)
		_avatar.camera_yaw = _yaw
		_aimed_id = pick_target(_avatar.global_position, _aim_direction(), _villager_positions())
	else:
		_aimed_id = -1
	_update_highlight()
	_apply_label_policy()

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_G:
			toggle()
			get_viewport().set_input_as_handled()
		elif event.keycode == KEY_E and mode == Mode.GROUND:
			select_target()
			get_viewport().set_input_as_handled()
	if mode != Mode.GROUND:
		return
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_RIGHT:
			_looking = event.pressed
			get_viewport().set_input_as_handled()
		elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_UP:
			_arm.spring_length = clamp(_arm.spring_length - ARM_STEP, ARM_MIN, ARM_MAX)
		elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			_arm.spring_length = clamp(_arm.spring_length + ARM_STEP, ARM_MIN, ARM_MAX)
	elif event is InputEventMouseMotion and _looking:
		_yaw -= event.relative.x * LOOK_SPEED
		_pitch = clamp(_pitch - event.relative.y * LOOK_SPEED, PITCH_MIN, PITCH_MAX)

func toggle() -> void:
	if mode == Mode.COMMAND:
		descend()
	elif mode == Mode.GROUND:
		ascend()

func descend() -> void:
	if mode != Mode.COMMAND:
		return
	var back := Vector3(_orbit.global_transform.basis.z.x, 0.0, _orbit.global_transform.basis.z.z).normalized()
	var target := _clear_landing_spot(_ground_point_under_screen_centre(), back)
	_ensure_ground_at(target)
	target.y = _terrain.get_height_at_world(target.x, target.z)
	_avatar.global_position = target + Vector3.UP * 0.2
	_set_avatar_active(true)
	_yaw = _orbit.global_rotation.y
	_pitch = DEFAULT_PITCH
	_arm.spring_length = ARM_LENGTH

	_set_orbit_enabled(false)
	await _fly(_orbit.global_transform, ground_camera_transform(target, _yaw, _pitch), Mode.DESCENDING)
	_ground_camera.make_current()
	_avatar.input_enabled = true
	_set_mode(Mode.GROUND)

func ascend() -> void:
	if mode != Mode.GROUND:
		return
	_avatar.input_enabled = false
	_avatar.stop()
	_place_orbit_over(_avatar.global_position)
	await _fly(_ground_camera.global_transform, _orbit.global_transform, Mode.ASCENDING)
	_set_avatar_active(false)
	_orbit.make_current()
	_set_orbit_enabled(true)
	_set_mode(Mode.COMMAND)

func select_target() -> void:
	var id := _aimed_id
	if id < 0:
		return
	var node: Node3D = _world_view.agent_node(id)
	if node == null:
		return
	_selected_id = id
	_avatar.face_towards(node.global_position)
	_avatar.play_interact()
	_select_sink.call("agent", id)

## The villager to act on: the one closest to the aim ray within AIM_RANGE
## and AIM_MAX_ANGLE, else the nearest within INTERACT_RADIUS, else -1.
## `positions` maps villager id -> world position.
static func pick_target(from: Vector3, aim: Vector3, positions: Dictionary) -> int:
	var flat_aim := Vector2(aim.x, aim.z).normalized()
	var best_aimed := -1
	var best_angle := AIM_MAX_ANGLE
	var nearest := -1
	var nearest_distance := INTERACT_RADIUS
	var ids := positions.keys()
	ids.sort()
	for id in ids:
		var to: Vector3 = positions[id] - from
		var flat := Vector2(to.x, to.z)
		var distance := flat.length()
		if distance < nearest_distance:
			nearest_distance = distance
			nearest = int(id)
		if distance < 0.01 or distance > AIM_RANGE or flat_aim == Vector2.ZERO:
			continue
		var angle := absf(flat_aim.angle_to(flat))
		if angle < best_angle:
			best_angle = angle
			best_aimed = int(id)
	return best_aimed if best_aimed >= 0 else nearest

## Ground camera pose for an avatar standing at `target`, before the spring
## arm shortens it against walls.
static func ground_camera_transform(target: Vector3, yaw: float, pitch: float) -> Transform3D:
	var pivot_basis := Basis.from_euler(Vector3(pitch, yaw, 0.0))
	return Transform3D(pivot_basis, target + Vector3.UP * PIVOT_HEIGHT + pivot_basis.z * ARM_LENGTH)

## Which activity labels show: every one in the command view; in the ground
## view only the aimed-at and selected villagers'.
static func label_visible(ground: bool, id: int, aimed_id: int, selected_id: int) -> bool:
	return not ground or id == aimed_id or id == selected_id

func aimed_id() -> int:
	return _aimed_id

func selected_id() -> int:
	return _selected_id

func avatar() -> CharacterBody3D:
	return _avatar

# -- Internals ------------------------------------------------------------

func _set_mode(next: Mode) -> void:
	mode = next
	_button.text = "Ascend (G)" if mode == Mode.GROUND else "Descend (G)"
	_button.disabled = mode == Mode.DESCENDING or mode == Mode.ASCENDING
	mode_changed.emit(mode)

func _set_avatar_active(active: bool) -> void:
	_avatar.visible = active
	_avatar.process_mode = Node.PROCESS_MODE_INHERIT if active else Node.PROCESS_MODE_DISABLED
	_avatar.velocity = Vector3.ZERO

func _fly(from: Transform3D, to: Transform3D, transition_mode: Mode) -> void:
	_set_mode(transition_mode)
	var from_fov := _orbit.fov if transition_mode == Mode.DESCENDING else GROUND_FOV
	var to_fov := GROUND_FOV if transition_mode == Mode.DESCENDING else _orbit.fov
	_transition_camera.global_transform = from
	_transition_camera.fov = from_fov
	_transition_camera.make_current()
	var tween := create_tween()
	tween.set_trans(Tween.TRANS_CUBIC).set_ease(Tween.EASE_IN_OUT)
	tween.tween_method(func(t: float) -> void:
		_transition_camera.global_transform = from.interpolate_with(to, t)
		_transition_camera.fov = lerp(from_fov, to_fov, t),
		0.0, 1.0, TRANSITION_SECONDS)
	await tween.finished

func _aim_direction() -> Vector3:
	return -_ground_camera.global_transform.basis.z

func _villager_positions() -> Dictionary:
	var positions := {}
	for id in _world_view.agent_ids():
		positions[int(id)] = (_world_view.agent_node(int(id)) as Node3D).global_position
	return positions

## Keep the orbit camera's height, pitch and yaw, but centre it on `focus`.
func _place_orbit_over(focus: Vector3) -> void:
	var forward := -_orbit.global_transform.basis.z
	var height: float = _orbit.global_position.y
	var distance: float = (height - focus.y) / maxf(-forward.y, 0.1)
	_orbit.global_position = focus - forward * distance

func _set_orbit_enabled(enabled: bool) -> void:
	_orbit.set_process(enabled)
	_orbit.set_process_unhandled_input(enabled)

## Step back toward the command camera until nothing stands within
## LANDING_CLEARANCE, so the avatar lands beside a crowd, not inside it.
func _clear_landing_spot(target: Vector3, back: Vector3) -> Vector3:
	var occupied: Array[Vector3] = []
	for id in _world_view.agent_ids():
		occupied.append((_world_view.agent_node(int(id)) as Node3D).global_position)
	for node in _world_view._building_nodes.values():
		occupied.append((node as Node3D).global_position)
	for step in LANDING_STEPS:
		var candidate := target + back * float(step)
		var clear := true
		for p in occupied:
			if Vector2(p.x - candidate.x, p.z - candidate.z).length() < LANDING_CLEARANCE:
				clear = false
				break
		if clear:
			return candidate
	return target

## Terrain tiles stream around the camera focus; make sure the landing tile
## (and its collision) exists before the avatar is placed on it.
func _ensure_ground_at(point: Vector3) -> void:
	_terrain.set_focus(Vector2(point.x, point.z))
	var guard := 64
	while not _terrain.has_tile_at(point.x, point.z) and guard > 0:
		_terrain.build_pending_tiles(1)
		guard -= 1

func _ground_point_under_screen_centre() -> Vector3:
	var centre := get_viewport().get_visible_rect().size * 0.5
	var from := _orbit.project_ray_origin(centre)
	var dir := _orbit.project_ray_normal(centre)
	var t := 0.0
	while t < 1200.0:
		var p := from + dir * t
		var ground: float = _terrain.get_height_at_world(p.x, p.z)
		if p.y <= ground:
			return Vector3(p.x, ground, p.z)
		t += 0.5
	return Vector3(from.x, _terrain.get_height_at_world(from.x, from.z), from.z)

## Villagers are solid to the avatar only: a static capsule on the villager
## layer rides along under each villager node. The avatar slides past it;
## nothing ever pushes a villager.
func _sync_villager_blockers() -> void:
	for id in _world_view.agent_ids():
		var node: Node3D = _world_view.agent_node(int(id))
		if node.has_meta(BLOCKER_META):
			continue
		node.add_child(make_villager_blocker())
		node.set_meta(BLOCKER_META, true)

static func make_villager_blocker() -> StaticBody3D:
	var body := StaticBody3D.new()
	body.name = "AvatarBlocker"
	body.collision_layer = PlayerAvatarScript.VILLAGER_LAYER
	body.collision_mask = 0
	var shape := CapsuleShape3D.new()
	shape.radius = VILLAGER_CAPSULE_RADIUS
	shape.height = VILLAGER_CAPSULE_HEIGHT
	var collision := CollisionShape3D.new()
	collision.shape = shape
	collision.position.y = VILLAGER_CAPSULE_HEIGHT * 0.5
	body.add_child(collision)
	return body

func _apply_label_policy() -> void:
	var ground := mode != Mode.COMMAND
	for id in _world_view.agent_ids():
		var label: Label3D = _world_view.agent_label(int(id))
		if label != null:
			label.visible = label_visible(ground, int(id), _aimed_id, _selected_id)

func _update_highlight() -> void:
	var id := _aimed_id if _aimed_id >= 0 else (_selected_id if mode == Mode.GROUND else -1)
	var node: Node3D = _world_view.agent_node(id) if id >= 0 else null
	_highlight.visible = node != null
	if node != null:
		_highlight.global_position = node.global_position + Vector3.UP * 0.06
		var colour := Color(1.0, 0.82, 0.2) if id == _aimed_id else Color(0.45, 0.8, 1.0)
		(_highlight.material_override as StandardMaterial3D).albedo_color = colour

func _make_highlight() -> MeshInstance3D:
	var ring := TorusMesh.new()
	ring.inner_radius = 0.55
	ring.outer_radius = 0.7
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_color = Color(1.0, 0.82, 0.2)
	var mesh := MeshInstance3D.new()
	mesh.name = "TargetHighlight"
	mesh.mesh = ring
	mesh.material_override = material
	mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mesh.visible = false
	return mesh

func _build_button() -> void:
	var layer := CanvasLayer.new()
	layer.name = "GroundViewControls"
	add_child(layer)
	_button = Button.new()
	_button.name = "DescendButton"
	_button.text = "Descend (G)"
	_button.focus_mode = Control.FOCUS_NONE
	_button.custom_minimum_size = Vector2(160, 40)
	_button.set_anchors_and_offsets_preset(Control.PRESET_CENTER_BOTTOM)
	_button.offset_top = -56
	_button.offset_bottom = -16
	_button.offset_left = -80
	_button.offset_right = 80
	_button.pressed.connect(toggle)
	layer.add_child(_button)

# -- Scripted capture for docs screenshots (env-gated) -------------------

func _capture(dir: String) -> void:
	await get_tree().create_timer(float(OS.get_environment("UNIVERSECORE_DESCENT_CAPTURE_WAIT")) if OS.get_environment("UNIVERSECORE_DESCENT_CAPTURE_WAIT") != "" else 12.0).timeout
	_shot(dir, "1_command_view")
	descend()
	await get_tree().create_timer(TRANSITION_SECONDS * 0.5).timeout
	_shot(dir, "2_mid_descent")
	while mode != Mode.GROUND:
		await get_tree().process_frame
	await get_tree().create_timer(0.5).timeout

	var target_id := _nearest_villager_beyond(3.0)
	var walked := 0.0
	var walking_shot := false
	while walked < 10.0 and target_id >= 0:
		var node: Node3D = _world_view.agent_node(target_id)
		if node == null:
			break
		var to_target := node.global_position - _avatar.global_position
		to_target.y = 0.0
		_yaw = atan2(-to_target.x, -to_target.z)
		if to_target.length() < 1.8:
			break
		_avatar.auto_input = Vector2(0.0, -1.0)
		await get_tree().process_frame
		walked += get_process_delta_time()
		if walked > 1.5 and not walking_shot:
			_shot(dir, "3_ground_walking")
			walking_shot = true
	_avatar.auto_input = Vector2.ZERO
	if not walking_shot:
		_shot(dir, "3_ground_walking")
	await get_tree().create_timer(0.6).timeout
	select_target()
	await get_tree().create_timer(1.5).timeout
	_shot(dir, "4_villager_selected")
	print("GROUND_VIEW_CAPTURE aimed=%d selected=%d avatar=%s on_floor=%s" % [_aimed_id, _selected_id, _avatar.global_position, _avatar.is_on_floor()])
	ascend()
	while mode != Mode.COMMAND:
		await get_tree().process_frame
	await get_tree().create_timer(0.5).timeout
	_shot(dir, "5_ascended_to_command_view")
	get_tree().quit()

func _nearest_villager_beyond(min_distance: float) -> int:
	var best := -1
	var best_distance := INF
	var positions := _villager_positions()
	for id in positions.keys():
		var d: float = (positions[id] as Vector3).distance_to(_avatar.global_position)
		if d > min_distance and d < best_distance:
			best_distance = d
			best = int(id)
	return best

func _shot(dir: String, label: String) -> void:
	get_viewport().get_texture().get_image().save_png("%s/%s.png" % [dir, label])
