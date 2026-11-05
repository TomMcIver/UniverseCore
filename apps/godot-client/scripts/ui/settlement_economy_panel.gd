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

const PRICE_ORDER := ["Food", "Wood", "Stone", "Timber", "BasicTool"]

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
	return 4

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
	lines.append_array(_trade_lines(data.get("trade", null)))
	lines.append("Household wealth: %d / %d / %d (min/median/max)" % [
		int(wealth.get("min", 0)), int(wealth.get("median", 0)), int(wealth.get("max", 0))])
	lines.append("Inequality: %.2f" % (float(wealth.get("inequalityPer10k", 0)) / 10000.0))
	lines.append("Buildings: %d complete, %d building, %d planned" % [
		int(buildings.get("complete", 0)), int(buildings.get("underConstruction", 0)), int(buildings.get("planned", 0))])
	_content.text = "\n".join(lines)

## Coin, derived prices and merchants (ADR 0003). Prices arrive in milli-coin.
func _trade_lines(trade: Variant) -> PackedStringArray:
	if typeof(trade) != TYPE_DICTIONARY:
		return PackedStringArray(["Trade: no data"])
	var lines := PackedStringArray([
		"Trade:",
		"  Coin: %s" % _grouped(int(trade.get("coin", 0))),
		"  Merchants: %d" % int(trade.get("merchants", 0)),
	])
	var prices: Variant = trade.get("pricesMillicoin", null)
	if typeof(prices) != TYPE_DICTIONARY:
		lines.append("  Prices: none (abandoned)")
	else:
		lines.append("  Prices (coin/unit):")
		for resource in PRICE_ORDER:
			lines.append("    %s: %s coin/unit" % [resource, _coin_from_milli(int(prices.get(resource, 0)))])
	lines.append_array(_trip_lines(trade.get("trips", null)))
	return lines

## Active merchant trips to or from this settlement (ADR 0003, #72/#77).
func _trip_lines(trips: Variant) -> PackedStringArray:
	var lines := PackedStringArray()
	if typeof(trips) != TYPE_ARRAY or (trips as Array).is_empty():
		return lines
	lines.append("  Trips:")
	for trip in trips:
		if typeof(trip) != TYPE_DICTIONARY:
			continue
		var phase := "heading out" if str(trip.get("phase", "")) == "Outbound" else "coming home"
		lines.append("    Merchant %d: %s %s, settlement %d to %d (%s), expected profit %s coin" % [
			int(trip.get("merchantId", 0)), _grouped(int(trip.get("cargoUnits", 0))),
			str(trip.get("resource", "")).to_lower(), int(trip.get("origin", 0)),
			int(trip.get("destination", 0)), phase, _grouped(int(trip.get("expectedProfit", 0)))])
	return lines

## 2500 -> "2.50", 20000 -> "20", 80 -> "0.08"; whole coin drops the decimals.
static func _coin_from_milli(milli: int) -> String:
	var whole := milli / 1000
	var cents := (absi(milli) % 1000) / 10
	if cents == 0:
		return _grouped(whole)
	return "%s.%02d" % [_grouped(whole), cents]

## 5659018 -> "5,659,018": totals run into the millions, which is unreadable raw.
static func _grouped(value: int) -> String:
	var digits := str(absi(value))
	var out := ""
	for i in digits.length():
		if i > 0 and (digits.length() - i) % 3 == 0:
			out += ","
		out += digits[i]
	return ("-" if value < 0 else "") + out
