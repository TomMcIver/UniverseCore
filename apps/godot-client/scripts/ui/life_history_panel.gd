class_name LifeHistoryPanel
extends "res://scripts/ui/ui_panel.gd"

## A selected person's recorded life, oldest first. Every line comes from a
## real simulation record; nothing here is invented.

var _title: Label
var _content: Label

func _init() -> void:
	name = "LifeHistory"
	custom_minimum_size = Vector2(320, 0)
	visible = false
	var vbox := VBoxContainer.new()
	add_child(vbox)

	_title = Label.new()
	_title.text = "Life history"
	vbox.add_child(_title)

	var scroll := ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(320, 220)
	vbox.add_child(scroll)

	_content = Label.new()
	_content.text = "Select a villager"
	_content.custom_minimum_size = Vector2(300, 0)
	_content.autowrap_mode = TextServer.AUTOWRAP_WORD
	scroll.add_child(_content)

func panel_slot() -> String:
	return SLOT_MODAL

func view_model_kinds() -> Dictionary:
	return {"person_life_history": PackedStringArray(["agent"])}

func show_view_model(_kind: String, entity_id: int, data: Dictionary) -> void:
	visible = true
	_title.text = "Villager %d%s" % [entity_id, "" if data.get("alive", false) else " (deceased)"]

	var entries: Array = data.get("entries", [])
	if entries.is_empty():
		_content.text = "No recorded life events yet."
		return

	var lines := PackedStringArray()
	var omitted := int(data.get("omittedEntries", 0))
	if omitted > 0:
		lines.append("(%d earlier entries not shown)" % omitted)
	for entry in entries:
		lines.append(str(entry.get("text", "")))
	_content.text = "\n".join(lines)
