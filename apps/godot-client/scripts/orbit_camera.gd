extends Camera3D

## Throwaway prototype camera: an elevated, adjustable-angle overview
## (Manor-Lords-style settlement view), not a Bannerlord-style ground-level
## tactical camera, per the "grounded-medieval, settlement-management"
## camera decision in spec #14.

@export var pan_speed: float = 20.0
@export var zoom_speed: float = 10.0
@export var min_height: float = 8.0
@export var max_height: float = 60.0
@export var pitch_degrees: float = -50.0

var _height: float = 28.0

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

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_WHEEL_UP:
			_height = clamp(_height - zoom_speed, min_height, max_height)
			position.y = _height
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			_height = clamp(_height + zoom_speed, min_height, max_height)
			position.y = _height
