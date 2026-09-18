class_name InspectorPanel
extends PanelContainer

## Ticket #17: displays either an agent's entity_details payload (fetched
## from the server on click) or a building's snapshot data (already local
## -- there is no server round trip for building details).

var _content: Label

func _init() -> void:
	var vbox := VBoxContainer.new()
	add_child(vbox)

	var title := Label.new()
	title.text = "Inspector"
	vbox.add_child(title)

	_content = Label.new()
	_content.text = "Select an agent or building"
	_content.custom_minimum_size = Vector2(220, 0)
	_content.autowrap_mode = TextServer.AUTOWRAP_WORD
	vbox.add_child(_content)

func show_agent_details(payload: Dictionary) -> void:
	var family: Dictionary = payload.get("family", {})
	var settlement_info: Dictionary = payload.get("settlement", {})
	var mother_id := int(family.get("motherId", 0))
	var father_id := int(family.get("fatherId", 0))

	_content.text = "\n".join(PackedStringArray([
		"ID: %d" % int(payload.get("id", 0)),
		"Age: %d" % int(payload.get("age", 0)),
		"Sex: %s" % str(payload.get("sex", "")),
		"Occupation: %s" % str(payload.get("occupation", "")),
		"Alive: %s" % str(payload.get("alive", false)),
		"Household: %d" % int(payload.get("householdId", 0)),
		"Settlement: %d" % int(payload.get("settlementId", 0)),
		"Region: %d" % int(payload.get("regionId", 0)),
		"Nutrition: %d" % int(payload.get("nutrition", 0)),
		"Health: %d" % int(payload.get("health", 0)),
		"Memory items: %d" % int(payload.get("memoryCount", 0)),
		"Knowledge items: %d" % int(payload.get("knowledgeCount", 0)),
		"Mother: %s" % (str(mother_id) if mother_id > 0 else "Unavailable"),
		"Father: %s" % (str(father_id) if father_id > 0 else "Unavailable"),
		"Settlement active: %s" % str(settlement_info.get("active", false)),
		"Settlement access share points: %d" % int(settlement_info.get("accessSharePoints", 0)),
	]))

func show_building_details(building: Dictionary) -> void:
	var owner_id := int(building.get("ownerHouseholdId", 0))
	var progress := float(building.get("progress", 0.0))

	_content.text = "\n".join(PackedStringArray([
		"ID: %d" % int(building.get("id", 0)),
		"Type: %s" % str(building.get("type", "")),
		"State: %s" % str(building.get("state", "")),
		"Progress: %d%%" % int(round(progress * 100.0)),
		"Settlement: %d" % int(building.get("settlementId", 0)),
		"Region: %d" % int(building.get("regionId", 0)),
		"Commons owned: %s" % str(building.get("settlementCommonsOwned", true)),
		"Owner household: %s" % (str(owner_id) if owner_id > 0 else "None"),
	]))

func clear() -> void:
	_content.text = "Select an agent or building"
