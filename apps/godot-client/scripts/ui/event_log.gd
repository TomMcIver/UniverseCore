class_name EventLog
extends "res://scripts/ui/ui_panel.gd"

## Timeline panel: a capped, newest-first list of world_event messages.

const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")
const MAX_ENTRIES := 40

var _list: VBoxContainer

func _init() -> void:
	name = "EventLog"
	custom_minimum_size = Vector2(280, 160)
	var scroll := ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(280, 160)
	add_child(scroll)
	_list = VBoxContainer.new()
	scroll.add_child(_list)

func panel_slot() -> String:
	return SLOT_TIMELINE

func message_types() -> PackedStringArray:
	return PackedStringArray(["world_event"])

func on_message(_message_type: String, message: Dictionary) -> void:
	var payload := UniverseProtocol.message_payload(message)
	var event_type := str(payload.get("eventType", "Unknown"))
	var entity_id := int(payload.get("entityId", 0))

	var label := Label.new()
	label.text = "%s -- entity %d" % [event_type, entity_id] if entity_id > 0 else event_type
	_list.add_child(label)
	_list.move_child(label, 0)

	while _list.get_child_count() > MAX_ENTRIES:
		_list.get_child(_list.get_child_count() - 1).free()

func entry_count() -> int:
	return _list.get_child_count()
