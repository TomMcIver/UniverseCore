class_name SettlementEconomyPanel
extends "res://scripts/ui/ui_panel.gd"

## Economy of the selected settlement: food, jobs and household wealth spread.

const JOB_LABELS := {
	"SubsistenceCultivator": "Farmers",
	"WildFoodGatherer": "Gatherers",
	"WoodGatherer": "Woodcutters",
	"StoneExtractor": "Quarriers",
	"CraftWorker": "Crafters",
	"Unallocated": "Idle workers",
}

var _title: Label
var _content: Label

func _init() -> void:
	name = "SettlementEconomy"
	var vbox := VBoxContainer.new()
	add_child(vbox)

	_title = Label.new()
	_title.text = "Settlement economy"
	vbox.add_child(_title)

	_content = Label.new()
	_content.text = "Select a settlement"
	_content.custom_minimum_size = Vector2(240, 0)
	_content.autowrap_mode = TextServer.AUTOWRAP_WORD
	vbox.add_child(_content)

func view_model_kinds() -> Dictionary:
	return {"settlement_economy": PackedStringArray(["settlement"])}

func show_view_model(_kind: String, entity_id: int, data: Dictionary) -> void:
	var food: Dictionary = data.get("food", {})
	var jobs: Dictionary = data.get("jobs", {})
	var wealth: Dictionary = data.get("wealth", {})

	_title.text = "%s %d%s" % [str(data.get("tier", "Settlement")), entity_id, "" if data.get("active", false) else " (abandoned)"]

	var lines := PackedStringArray([
		"People: %d in %d households" % [int(data.get("alivePopulation", 0)), int(data.get("households", 0))],
		"Food produced: %d  eaten: %d" % [int(food.get("producedTotal", 0)), int(food.get("consumedTotal", 0))],
		"Food short: %d" % int(food.get("unmetTotal", 0)),
		"Stored: %d households + %d commons" % [int(food.get("householdStored", 0)), int(food.get("commonsStored", 0))],
		"Land yields up to %d/day" % int(food.get("accessibleCapacityPerDay", 0)),
		"Jobs:",
	])
	for job in JOB_LABELS.keys():
		lines.append("  %s: %d" % [JOB_LABELS[job], int(jobs.get(job, 0))])
	lines.append("Household wealth: %d / %d / %d (min/median/max)" % [
		int(wealth.get("min", 0)), int(wealth.get("median", 0)), int(wealth.get("max", 0))])
	lines.append("Inequality: %.2f" % (float(wealth.get("inequalityPer10k", 0)) / 10000.0))
	_content.text = "\n".join(lines)
