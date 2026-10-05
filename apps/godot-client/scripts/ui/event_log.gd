class_name EventLog
extends "res://scripts/ui/ui_panel.gd"

## Timeline panel: a capped, newest-first list of world_event messages.

const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")
const MAX_ENTRIES := 40
const LIST_WIDTH := 270

var _list: VBoxContainer

func _init() -> void:
	name = "EventLog"
	custom_minimum_size = Vector2(280, 160)
	var scroll := ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(280, 160)
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	add_child(scroll)
	_list = VBoxContainer.new()
	# Without a width the list collapses and every label wraps one word per line.
	_list.custom_minimum_size = Vector2(LIST_WIDTH, 0)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_list)

func panel_slot() -> String:
	return SLOT_TIMELINE

func message_types() -> PackedStringArray:
	return PackedStringArray(["world_event"])

func on_message(_message_type: String, message: Dictionary) -> void:
	var payload := UniverseProtocol.message_payload(message)
	var summary := str(payload.get("summary", ""))
	if summary == "":
		summary = str(payload.get("eventType", "Unknown"))

	var label := Label.new()
	label.text = summary
	label.autowrap_mode = TextServer.AUTOWRAP_WORD
	label.custom_minimum_size = Vector2(LIST_WIDTH, 0)
	_list.add_child(label)
	_list.move_child(label, 0)

	while _list.get_child_count() > MAX_ENTRIES:
		_list.get_child(_list.get_child_count() - 1).free()

func entry_count() -> int:
	return _list.get_child_count()
