class_name Hud
extends Control

## Minimal native Control-node HUD for ticket #15's scope: connection state,
## sim time/tick/speed display, and Pause/Resume/Step/SetSpeed controls.
## Full parity with the PlayCanvas client's inspector (entity details, event
## log) is ticket #17's job, not this one.

signal pause_pressed
signal resume_pressed
signal step_pressed
signal speed_selected(speed: int)

var _status_label: Label
var _world_label: Label
var _feedback_label: Label

func _ready() -> void:
	var panel := PanelContainer.new()
	panel.set_anchors_preset(Control.PRESET_TOP_LEFT)
	panel.position = Vector2(12, 12)
	add_child(panel)

	var vbox := VBoxContainer.new()
	panel.add_child(vbox)

	_status_label = Label.new()
	_status_label.text = "Connecting..."
	vbox.add_child(_status_label)

	_world_label = Label.new()
	_world_label.text = "Year 0 | Tick 0 | Population 0"
	vbox.add_child(_world_label)

	_feedback_label = Label.new()
	_feedback_label.text = ""
	_feedback_label.modulate = Color(1.0, 0.45, 0.45)
	vbox.add_child(_feedback_label)

	var buttons := HBoxContainer.new()
	vbox.add_child(buttons)

	var pause_button := Button.new()
	pause_button.text = "Pause"
	pause_button.pressed.connect(func() -> void: pause_pressed.emit())
	buttons.add_child(pause_button)

	var resume_button := Button.new()
	resume_button.text = "Resume"
	resume_button.pressed.connect(func() -> void: resume_pressed.emit())
	buttons.add_child(resume_button)

	var step_button := Button.new()
	step_button.text = "Step"
	step_button.pressed.connect(func() -> void: step_pressed.emit())
	buttons.add_child(step_button)

	for speed in [1, 2, 4, 8]:
		var speed_button := Button.new()
		speed_button.text = "%dx" % speed
		var captured_speed: int = speed
		speed_button.pressed.connect(func() -> void: speed_selected.emit(captured_speed))
		buttons.add_child(speed_button)

func set_connection_status(text: String) -> void:
	_status_label.text = text

func set_world_info(year: int, tick: int, population: int) -> void:
	_world_label.text = "Year %d | Tick %d | Population %d" % [year, tick, population]

## Ticket #16: a command sent while disconnected must fail visibly, not
## silently vanish. Called by main.gd with the boolean UniverseClient.
## send_command already returns.
func show_command_result(succeeded: bool, action_name: String) -> void:
	_feedback_label.text = "" if succeeded else "%s failed -- not connected" % action_name
