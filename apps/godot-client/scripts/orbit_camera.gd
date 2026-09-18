extends Camera3D

## Elevated, adjustable-angle overview (Manor-Lords-style settlement view),
## not a Bannerlord-style ground-level tactical camera, per the
## "grounded-medieval, settlement-management" camera decision in spec #14.
##
## Controls (per live user feedback -- WASD-only with no drag or keyboard
## zoom wasn't enough):
## - WASD: pan
## - Right mouse button drag: pan (grab-the-ground style, doesn't
##   conflict with left-click entity selection in main.gd)
## - Mouse wheel: zoom
## - Q / E: zoom (keyboard alternative to the wheel, as requested)

@export var pan_speed: float = 20.0
@export var drag_pan_speed: float = 0.05
@export var zoom_speed: float = 10.0
@export var key_zoom_speed: float = 15.0
@export var min_height: float = 8.0
@export var max_height: float = 120.0
@export var pitch_degrees: float = -50.0

var _height: float = 28.0
var _dragging := false
var _drag_last_position := Vector2.ZERO

func _ready() -> void:
	rotation_degrees = Vector3(pitch_degrees, 0.0, 0.0)
	position = Vector3(24.0, _height, 24.0 + _height * 0.6)

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

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_WHEEL_UP:
			_zoom(zoom_speed)
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			_zoom(-zoom_speed)
		elif event.button_index == MOUSE_BUTTON_RIGHT:
			_dragging = event.pressed
			_drag_last_position = event.position
	elif event is InputEventMouseMotion and _dragging:
		var delta_screen: Vector2 = event.position - _drag_last_position
		_drag_last_position = event.position
		# Screen-space drag maps to world X/Z: dragging right moves the
		# view left (grab-the-ground feel), same for up/down vs Z.
		position.x -= delta_screen.x * drag_pan_speed
		position.z -= delta_screen.y * drag_pan_speed

func _zoom(amount: float) -> void:
	_height = clamp(_height - amount, min_height, max_height)
	position.y = _height
