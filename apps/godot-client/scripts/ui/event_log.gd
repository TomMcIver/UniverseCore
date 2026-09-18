class_name EventLog
extends PanelContainer

## Ticket #17: a scrolling log of world_event messages, capped so it
## doesn't grow forever over a long session.

const MAX_ENTRIES := 40

var _list: VBoxContainer

func _init() -> void:
	custom_minimum_size = Vector2(260, 160)

	var scroll := ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(260, 160)
	add_child(scroll)

	_list = VBoxContainer.new()
	scroll.add_child(_list)

func append_event(payload: Dictionary) -> void:
	var event_type := str(payload.get("eventType", "Unknown"))
	var entity_id := int(payload.get("entityId", 0))

	var label := Label.new()
	label.text = "%s -- entity %d" % [event_type, entity_id] if entity_id > 0 else event_type
	_list.add_child(label)
	_list.move_child(label, 0)

	while _list.get_child_count() > MAX_ENTRIES:
		var oldest := _list.get_child(_list.get_child_count() - 1)
		oldest.free()

func entry_count() -> int:
	return _list.get_child_count()
