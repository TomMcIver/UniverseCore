class_name Hud
extends "res://scripts/ui/ui_panel.gd"

## Top-bar panel: connection state, the year counter, and time controls,
## including the demo's jump of JUMP_YEARS years at jump speed.

const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")

const SPEEDS := [1, 2, 4, 8, 16, 32, 64]
const JUMP_YEARS := 20

var _status_label: Label
var _year_label: Label
var _world_label: Label
var _feedback_label: Label
var _jump_button: Button
var _current_year := 0

func _init() -> void:
	name = "Hud"
	var vbox := VBoxContainer.new()
	add_child(vbox)

	_status_label = Label.new()
	_status_label.text = "Connecting..."
	vbox.add_child(_status_label)

	_year_label = Label.new()
	_year_label.text = "Year 0"
	_year_label.add_theme_font_size_override("font_size", 28)
	vbox.add_child(_year_label)

	_world_label = Label.new()
	_world_label.text = "Population 0"
	vbox.add_child(_world_label)

	_feedback_label = Label.new()
	_feedback_label.modulate = Color(1.0, 0.45, 0.45)
	vbox.add_child(_feedback_label)

	var buttons := HBoxContainer.new()
	vbox.add_child(buttons)
	_add_button(buttons, "Pause", "pause", UniverseProtocol.command_pause())
	_add_button(buttons, "Resume", "resume", UniverseProtocol.command_resume())
	_add_button(buttons, "Step", "single_step", UniverseProtocol.command_single_step())
	for speed in SPEEDS:
		_add_button(buttons, "%dx" % speed, "set_speed", UniverseProtocol.command_set_speed(speed))

	_jump_button = Button.new()
	_jump_button.text = "Jump %d years" % JUMP_YEARS
	_jump_button.pressed.connect(request_jump)
	vbox.add_child(_jump_button)

func _add_button(parent: Control, label: String, action_name: String, command: Dictionary) -> void:
	var button := Button.new()
	button.text = label
	button.pressed.connect(func() -> void: action_requested.emit(action_name, command))
	parent.add_child(button)

## Jumps from the year currently shown, so the target is always in the future.
func request_jump() -> void:
	action_requested.emit("run_until_year", UniverseProtocol.command_run_until_year(_current_year + JUMP_YEARS))

## The year the world last reported.
func current_year() -> int:
	return _current_year

func panel_slot() -> String:
	return SLOT_TOP_BAR

func message_types() -> PackedStringArray:
	return PackedStringArray(["connection_status", "simulation_status", "initial_snapshot", "world_delta"])

func actions() -> PackedStringArray:
	return PackedStringArray(["pause", "resume", "single_step", "set_speed", "run_until_year"])

func on_message(message_type: String, message: Dictionary) -> void:
	var payload := UniverseProtocol.message_payload(message)
	match message_type:
		"connection_status":
			_status_label.text = str(payload.get("status", ""))
		"simulation_status":
			_show_status(payload)
		"initial_snapshot", "world_delta":
			_current_year = int(payload.get("year", 0))
			_year_label.text = "Year %d" % _current_year
			_world_label.text = "Population %d" % int(payload.get("population", 0))

func _show_status(payload: Dictionary) -> void:
	var target = payload.get("targetYear")
	if target != null:
		_status_label.text = "Connected -- Jumping to year %d" % int(target)
		_jump_button.disabled = true
		return
	var paused: bool = payload.get("paused", true)
	_status_label.text = "Connected -- %s (%dx)" % ["Paused" if paused else "Running", int(payload.get("speed", 1))]
	_jump_button.disabled = false

## A command sent while disconnected must fail visibly, not silently vanish.
func show_command_result(succeeded: bool, action_name: String) -> void:
	_feedback_label.text = "" if succeeded else "%s failed -- not connected" % action_name
