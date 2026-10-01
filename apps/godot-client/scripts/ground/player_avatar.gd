extends CharacterBody3D

## The presenter's third-person player avatar (see CONTEXT.md "Player
## avatar"). Presentation-only: the simulation never learns it exists.
## Movement shape follows the Jeh3no Third Person Controller v2.2 (MIT) as a
## reference -- camera-relative input, face the move direction, walk/sprint.

const CHARACTER := preload("res://assets/characters/character-a.glb")
const WALK_SPEED := 2.4
const SPRINT_SPEED := 6.0
const ACCELERATION := 14.0
const TURN_SPEED := 10.0
const GRAVITY := 20.0
const MOVING_THRESHOLD := 0.3
const INTERACT_SECONDS := 1.0
const CAPSULE_RADIUS := 0.3
const CAPSULE_HEIGHT := 1.7
## The Kenney character is 2.7 units tall; people in this world are 1.8 m.
const VISUAL_SCALE := 1.8 / 2.7

const AVATAR_LAYER := 8
const WORLD_LAYER := 2
const VILLAGER_LAYER := 4

## World yaw (radians) that input is relative to; set by the ground view.
var camera_yaw := 0.0
## Overrides keyboard input while non-zero (scripted capture runs).
var auto_input := Vector2.ZERO
var auto_sprint := false
var input_enabled := false

var _visual: Node3D
var _anim: AnimationPlayer
var _current_anim := ""
var _interact_time := 0.0

func _init() -> void:
	collision_layer = AVATAR_LAYER
	collision_mask = WORLD_LAYER | VILLAGER_LAYER
	floor_snap_length = 0.6

func _ready() -> void:
	var shape := CapsuleShape3D.new()
	shape.radius = CAPSULE_RADIUS
	shape.height = CAPSULE_HEIGHT
	var collision := CollisionShape3D.new()
	collision.shape = shape
	collision.position.y = CAPSULE_HEIGHT * 0.5
	add_child(collision)

	_visual = CHARACTER.instantiate()
	_visual.scale = Vector3.ONE * VISUAL_SCALE
	add_child(_visual)
	_anim = _visual.find_child("AnimationPlayer", true, false)
	_play("idle")

func _physics_process(delta: float) -> void:
	var input := auto_input
	var sprinting := auto_sprint
	if input_enabled and input == Vector2.ZERO:
		input = Vector2(
			float(Input.is_key_pressed(KEY_D)) - float(Input.is_key_pressed(KEY_A)),
			float(Input.is_key_pressed(KEY_S)) - float(Input.is_key_pressed(KEY_W)))
		sprinting = Input.is_key_pressed(KEY_SHIFT)
	step(delta, input, sprinting)

## One movement step; separate from _physics_process so tests can drive it.
func step(delta: float, input: Vector2, sprinting: bool) -> void:
	var direction := Vector3(input.x, 0.0, input.y).rotated(Vector3.UP, camera_yaw)
	if direction.length() > 1.0:
		direction = direction.normalized()
	var target := direction * (SPRINT_SPEED if sprinting else WALK_SPEED)

	velocity.x = move_toward(velocity.x, target.x, ACCELERATION * delta)
	velocity.z = move_toward(velocity.z, target.z, ACCELERATION * delta)
	velocity.y = -0.1 if is_on_floor() else velocity.y - GRAVITY * delta
	move_and_slide()

	if direction.length() > 0.05:
		_visual.rotation.y = lerp_angle(_visual.rotation.y, atan2(direction.x, direction.z), TURN_SPEED * delta)
	_update_animation(delta)

func face_towards(point: Vector3) -> void:
	var d := point - global_position
	_visual.rotation.y = atan2(d.x, d.z)

func play_interact() -> void:
	_play("interact-right")
	_interact_time = INTERACT_SECONDS

func stop() -> void:
	velocity = Vector3.ZERO
	auto_input = Vector2.ZERO
	_play("idle")

func _update_animation(delta: float) -> void:
	var horizontal := Vector2(velocity.x, velocity.z).length()
	if _interact_time > 0.0 and horizontal < MOVING_THRESHOLD:
		_interact_time -= delta
		return
	_interact_time = 0.0
	if horizontal < MOVING_THRESHOLD:
		_play("idle")
	elif horizontal > WALK_SPEED + 0.5:
		_play("sprint")
	else:
		_play("walk")

func _play(anim_name: String) -> void:
	if _anim == null or _current_anim == anim_name or not _anim.has_animation(anim_name):
		return
	_current_anim = anim_name
	_anim.play(anim_name)
