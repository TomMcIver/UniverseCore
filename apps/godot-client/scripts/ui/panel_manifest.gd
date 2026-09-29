extends RefCounted

## The panels the client registers at startup. A new mechanic adds its panel
## script here; main.gd never needs to change.

const PANEL_SCRIPTS := [
	preload("res://scripts/ui/hud.gd"),
	preload("res://scripts/ui/inspector_panel.gd"),
	preload("res://scripts/ui/settlement_economy_panel.gd"),
	preload("res://scripts/ui/event_log.gd"),
	preload("res://scripts/ui/life_history_panel.gd"),
	preload("res://scripts/ui/demo_focus_marker.gd"),
]

static func register_all(registry) -> void:
	for script in PANEL_SCRIPTS:
		registry.register(script.new())
