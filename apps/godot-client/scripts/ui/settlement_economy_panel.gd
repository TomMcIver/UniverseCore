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

func supported_version(_kind: String) -> int:
	return 2

func show_view_model(_kind: String, entity_id: int, data: Dictionary) -> void:
	var food: Dictionary = data.get("food", {})
	var jobs: Dictionary = data.get("jobs", {})
	var wealth: Dictionary = data.get("wealth", {})
	var buildings: Dictionary = data.get("buildings", {})

	_title.text = "%s %d%s" % [str(data.get("tier", "Settlement")), entity_id, "" if data.get("active", false) else " (abandoned)"]

	var lines := PackedStringArray([
		"People: %d in %d households" % [int(data.get("alivePopulation", 0)), int(data.get("households", 0))],
		"Food produced: %s  eaten: %s (since the start)" % [_grouped(int(food.get("producedTotal", 0))), _grouped(int(food.get("consumedTotal", 0)))],
		"Food short: %s (since the start)" % _grouped(int(food.get("unmetTotal", 0))),
		"Stored: %s in households, %s in commons" % [_grouped(int(food.get("householdStored", 0))), _grouped(int(food.get("commonsStored", 0)))],
		"Land yields up to %s/day" % _grouped(int(food.get("accessibleCapacityPerDay", 0))),
		"Jobs:",
	])
	for job in JOB_LABELS.keys():
		lines.append("  %s: %d" % [JOB_LABELS[job], int(jobs.get(job, 0))])
	lines.append("Household wealth: %d / %d / %d (min/median/max)" % [
		int(wealth.get("min", 0)), int(wealth.get("median", 0)), int(wealth.get("max", 0))])
	lines.append("Inequality: %.2f" % (float(wealth.get("inequalityPer10k", 0)) / 10000.0))
	lines.append("Buildings: %d complete, %d building, %d planned" % [
		int(buildings.get("complete", 0)), int(buildings.get("underConstruction", 0)), int(buildings.get("planned", 0))])
	_content.text = "\n".join(lines)

## 5659018 -> "5,659,018": totals run into the millions, which is unreadable raw.
static func _grouped(value: int) -> String:
	var digits := str(absi(value))
	var out := ""
	for i in digits.length():
		if i > 0 and (digits.length() - i) % 3 == 0:
			out += ","
		out += digits[i]
	return ("-" if value < 0 else "") + out
