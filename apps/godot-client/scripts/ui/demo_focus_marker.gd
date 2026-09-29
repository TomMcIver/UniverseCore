class_name DemoFocusMarker
extends "res://scripts/ui/ui_panel.gd"

## No visible UI. Tracks the demo_focus view model for the selected
## settlement and asks the world to mark that villager, so the demo's
## chosen NPC is always findable in the ground view. Recomputed by the
## registry's normal refresh-on-world_delta, so it follows population change.

func _init() -> void:
	name = "DemoFocusMarker"
	custom_minimum_size = Vector2.ZERO

func panel_slot() -> String:
	return SLOT_TIMELINE

func view_model_kinds() -> Dictionary:
	return {"demo_focus": PackedStringArray(["settlement"])}

func show_view_model(_kind: String, _entity_id: int, data: Dictionary) -> void:
	var person_id = data.get("personId")
	world_highlight_requested.emit("agent", 0 if person_id == null else int(person_id))
