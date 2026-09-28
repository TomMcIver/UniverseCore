class_name InspectorPanel
extends "res://scripts/ui/ui_panel.gd"

## Shows the selected person or building from its server-built view model.

const PLACEHOLDER := "Select a villager or building"

var _title: Label
var _content: Label

func _init() -> void:
	name = "Inspector"
	var vbox := VBoxContainer.new()
	add_child(vbox)

	_title = Label.new()
	_title.text = "Inspector"
	vbox.add_child(_title)

	_content = Label.new()
	_content.text = PLACEHOLDER
	_content.custom_minimum_size = Vector2(240, 0)
	_content.autowrap_mode = TextServer.AUTOWRAP_WORD
	vbox.add_child(_content)

func view_model_kinds() -> Dictionary:
	return {
		"person_summary": PackedStringArray(["agent"]),
		"building_summary": PackedStringArray(["building"]),
	}

func show_view_model(kind: String, entity_id: int, data: Dictionary) -> void:
	match kind:
		"person_summary":
			_show_person(entity_id, data)
		"building_summary":
			_show_building(entity_id, data)

func _show_person(entity_id: int, data: Dictionary) -> void:
	_title.text = "Villager %d" % entity_id
	_content.text = "\n".join(PackedStringArray([
		"Age: %d  %s" % [int(data.get("age", 0)), str(data.get("sex", ""))],
		"Alive: %s" % ("yes" if data.get("alive", false) else "no"),
		"Occupation: %s" % str(data.get("occupation", "")),
		"Doing now: %s" % str(data.get("currentActivity", "")),
		"Household: %d" % int(data.get("householdId", 0)),
		"Settlement: %d  Region: %d" % [int(data.get("settlementId", 0)), int(data.get("regionId", 0))],
		"Nutrition: %d  Health: %d" % [int(data.get("nutrition", 0)), int(data.get("health", 0))],
		"Memories: %d  Knowledge: %d" % [int(data.get("memoryCount", 0)), int(data.get("knowledgeCount", 0))],
		"Mother: %s" % _id_or(data.get("motherId"), "Unknown"),
		"Father: %s" % _id_or(data.get("fatherId"), "Unknown"),
	]))

func _show_building(entity_id: int, data: Dictionary) -> void:
	var labour: Dictionary = data.get("labour", {})
	var required := int(labour.get("required", 0))
	var progress := 100 if required <= 0 else int(round(100.0 * float(labour.get("completed", 0)) / float(required)))
	_title.text = "Building %d" % entity_id
	_content.text = "\n".join(PackedStringArray([
		"Type: %s" % str(data.get("buildingType", "")),
		"State: %s" % str(data.get("constructionState", "")),
		"Labour: %d%%" % clampi(progress, 0, 100),
		"Timber: %s" % _progress_pair(data.get("timber", {})),
		"Stone: %s" % _progress_pair(data.get("stone", {})),
		"Settlement: %d" % int(data.get("settlementId", 0)),
		"Owner household: %s" % _id_or(data.get("ownerHouseholdId"), "Commons"),
	]))

func _progress_pair(pair: Dictionary) -> String:
	return "%d / %d" % [int(pair.get("committed", 0)), int(pair.get("required", 0))]

func _id_or(value, fallback: String) -> String:
	return fallback if value == null else str(int(value))
