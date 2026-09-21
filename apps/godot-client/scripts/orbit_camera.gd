extends Camera3D

## Elevated, adjustable-angle overview (Manor-Lords-style settlement view),
## not a Bannerlord-style ground-level tactical camera, per the
## "grounded-medieval, settlement-management" camera decision in spec #14.
##
## Controls (per live user feedback -- WASD-only with no drag or keyboard
## zoom wasn't enough, then pan+zoom alone wasn't enough either):
## - WASD: pan (world-space, unaffected by yaw -- kept simple/predictable)
## - Right mouse button drag: pan (grab-the-ground style, doesn't
##   conflict with left-click entity selection in main.gd)
## - Mouse wheel: zoom
## - Q / E: zoom (keyboard alternative to the wheel, as requested)
## - Middle mouse button drag: tilt (vertical drag = pitch) and rotate
##   (horizontal drag = yaw)
## - Z / C: yaw rotate left / right (keyboard alternative to middle-drag)
## - R / F: tilt up / down (keyboard alternative to middle-drag)

@export var pan_speed: float = 20.0
@export var drag_pan_speed: float = 0.05
@export var zoom_speed: float = 10.0
@export var key_zoom_speed: float = 15.0
@export var min_height: float = 8.0
@export var max_height: float = 120.0
@export var pitch_degrees: float = -50.0
@export var min_pitch_degrees: float = -85.0
@export var max_pitch_degrees: float = -15.0
@export var drag_tilt_speed: float = 0.15
@export var key_tilt_speed: float = 30.0
@export var key_yaw_speed: float = 60.0

var _height: float = 28.0
var _dragging := false
var _tilting := false
var _drag_last_position := Vector2.ZERO
var _yaw_degrees: float = 0.0
var _pitch: float

func _ready() -> void:
	_pitch = pitch_degrees
	rotation_degrees = Vector3(_pitch, _yaw_degrees, 0.0)
	position = Vector3(24.0, _height, 24.0 + _height * 0.6)

## The starting position above is an arbitrary fixed point near world
## origin -- fine while everything the client rendered was presentation-
## invented and clustered near origin, but real canonical region/settlement
## positions (tickets #24/#25) can land anywhere within a much larger world
## extent, so the camera can easily start out looking at empty terrain with
## nothing in view. Called once real settlement data arrives so the camera
## actually finds it, without otherwise disturbing user-controlled zoom/
## pan/rotation state.
func center_on(world_x: float, world_z: float) -> void:
	position = Vector3(world_x, _height, world_z + _height * 0.6)

func _process(delta: float) -> void:
	var input_dir := Vector3.ZERO
	if Input.is_key_pressed(KEY_W):
		input_dir.z -= 1.0
	if Input.is_key_pressed(KEY_S):
		input_dir.z += 1.0
	if Input.is_key_pressed(KEY_A):
		input_dir.x -= 1.0
	if Input.is_key_pressed(KEY_D):
		input_dir.x += 1.0
	if input_dir.length() > 0.0:
		position += input_dir.normalized() * pan_speed * delta

	if Input.is_key_pressed(KEY_Q):
		_zoom(key_zoom_speed * delta)
	if Input.is_key_pressed(KEY_E):
		_zoom(-key_zoom_speed * delta)

	if Input.is_key_pressed(KEY_Z):
		_rotate(-key_yaw_speed * delta)
	if Input.is_key_pressed(KEY_C):
		_rotate(key_yaw_speed * delta)
	if Input.is_key_pressed(KEY_R):
		_tilt(key_tilt_speed * delta)
	if Input.is_key_pressed(KEY_F):
		_tilt(-key_tilt_speed * delta)

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_WHEEL_UP:
			_zoom(zoom_speed)
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			_zoom(-zoom_speed)
		elif event.button_index == MOUSE_BUTTON_RIGHT:
			_dragging = event.pressed
			_drag_last_position = event.position
		elif event.button_index == MOUSE_BUTTON_MIDDLE:
			_tilting = event.pressed
			_drag_last_position = event.position
	elif event is InputEventMouseMotion and _dragging:
		var delta_screen: Vector2 = event.position - _drag_last_position
		_drag_last_position = event.position
		# Screen-space drag maps to world X/Z: dragging right moves the
		# view left (grab-the-ground feel), same for up/down vs Z.
		position.x -= delta_screen.x * drag_pan_speed
		position.z -= delta_screen.y * drag_pan_speed
	elif event is InputEventMouseMotion and _tilting:
		var delta_screen: Vector2 = event.position - _drag_last_position
		_drag_last_position = event.position
		_rotate(-delta_screen.x * drag_tilt_speed)
		_tilt(-delta_screen.y * drag_tilt_speed)

func _zoom(amount: float) -> void:
	_height = clamp(_height - amount, min_height, max_height)
	position.y = _height

func _rotate(delta_degrees: float) -> void:
	_yaw_degrees = fmod(_yaw_degrees + delta_degrees, 360.0)
	rotation_degrees.y = _yaw_degrees

func _tilt(delta_degrees: float) -> void:
	_pitch = clamp(_pitch + delta_degrees, min_pitch_degrees, max_pitch_degrees)
	rotation_degrees.x = _pitch
