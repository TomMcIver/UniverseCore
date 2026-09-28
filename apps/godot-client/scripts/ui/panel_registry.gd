class_name PanelRegistry
extends Control

## Places registered UiPanels into screen slots, routes protocol messages to
## them, requests view models for the current selection, and sends panel
## actions. The server validates every command; this only routes them.

const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")
const UiPanelScript := preload("res://scripts/ui/ui_panel.gd")

const REFRESH_MESSAGE_TYPES := ["initial_snapshot", "world_delta"]

var _panels: Array = []
var _slots: Dictionary = {}
var _command_sink := Callable()
## panel instance id -> {"kind": String, "entity_id": int}: what each panel is showing.
var _focus: Dictionary = {}

func _init() -> void:
	# Full-rect overlay; IGNORE lets clicks on empty areas reach the 3D pick.
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_preset(Control.PRESET_FULL_RECT)
	_slots[UiPanelScript.SLOT_TOP_BAR] = _make_slot(Control.PRESET_TOP_LEFT, Vector2(12, 12), Control.GROW_DIRECTION_END, Control.GROW_DIRECTION_END)
	_slots[UiPanelScript.SLOT_INSPECTOR] = _make_slot(Control.PRESET_TOP_RIGHT, Vector2(-12, 12), Control.GROW_DIRECTION_BEGIN, Control.GROW_DIRECTION_END)
	_slots[UiPanelScript.SLOT_TIMELINE] = _make_slot(Control.PRESET_BOTTOM_LEFT, Vector2(12, -12), Control.GROW_DIRECTION_END, Control.GROW_DIRECTION_BEGIN)
	_slots[UiPanelScript.SLOT_MODAL] = _make_slot(Control.PRESET_CENTER, Vector2.ZERO, Control.GROW_DIRECTION_BOTH, Control.GROW_DIRECTION_BOTH)

func _make_slot(preset: int, offset: Vector2, grow_h: int, grow_v: int) -> VBoxContainer:
	var slot := VBoxContainer.new()
	slot.mouse_filter = Control.MOUSE_FILTER_IGNORE
	slot.set_anchors_preset(preset)
	slot.grow_horizontal = grow_h
	slot.grow_vertical = grow_v
	slot.position += offset
	add_child(slot)
	return slot

## `sink` is (command: Dictionary) -> bool, true when the command was sent.
func set_command_sink(sink: Callable) -> void:
	_command_sink = sink

func register(panel: UiPanel) -> void:
	var slot_name := panel.panel_slot()
	if not _slots.has(slot_name):
		push_error("PanelRegistry: %s declares unknown slot '%s'" % [panel.name, slot_name])
		return
	_panels.append(panel)
	_slots[slot_name].add_child(panel)
	panel.action_requested.connect(_on_action_requested.bind(panel))

func panels() -> Array:
	return _panels.duplicate()

func slot_of(panel: UiPanel) -> String:
	for slot_name in _slots.keys():
		if panel.get_parent() == _slots[slot_name]:
			return slot_name
	return ""

## Points every panel that draws `entity_type` at this entity and requests its view models.
func select(entity_type: String, entity_id: int) -> void:
	for panel in _panels:
		var kinds: Dictionary = panel.view_model_kinds()
		for kind in kinds.keys():
			if entity_type in kinds[kind]:
				_focus[panel.get_instance_id()] = {"kind": kind, "entity_id": entity_id}
				break
	_request_focused_view_models()

func route_message(message: Dictionary) -> void:
	var message_type := UniverseProtocol.message_type(message)
	if message_type == "view_model":
		_route_view_model(UniverseProtocol.message_payload(message))
		return

	for panel in _panels:
		if message_type in panel.message_types():
			panel.on_message(message_type, message)

	if message_type in REFRESH_MESSAGE_TYPES:
		_request_focused_view_models()

func _route_view_model(payload: Dictionary) -> void:
	var kind := str(payload.get("kind", ""))
	var entity_id := int(payload.get("entityId", 0))
	var version := int(payload.get("version", 0))
	var data = payload.get("data", {})
	if typeof(data) != TYPE_DICTIONARY:
		return
	for panel in _panels:
		var focus: Dictionary = _focus.get(panel.get_instance_id(), {})
		if focus.get("kind", "") != kind or int(focus.get("entity_id", 0)) != entity_id:
			continue
		if panel.supported_version(kind) != version:
			push_warning("PanelRegistry: dropping %s v%d (panel supports v%d)" % [kind, version, panel.supported_version(kind)])
			continue
		panel.show_view_model(kind, entity_id, data)

func _request_focused_view_models() -> void:
	if not _command_sink.is_valid():
		return
	var requested := {}
	for focus in _focus.values():
		var key := "%s:%d" % [focus["kind"], focus["entity_id"]]
		if requested.has(key):
			continue
		requested[key] = true
		_command_sink.call(UniverseProtocol.command_request_view_model(focus["kind"], focus["entity_id"]))

func _on_action_requested(action_name: String, command: Dictionary, panel: UiPanel) -> void:
	if not (action_name in panel.actions()):
		push_error("PanelRegistry: %s emitted undeclared action '%s'" % [panel.name, action_name])
		return
	var sent: bool = _command_sink.is_valid() and _command_sink.call(command)
	panel.show_command_result(sent, action_name)
