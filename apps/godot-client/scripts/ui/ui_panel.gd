class_name UiPanel
extends PanelContainer

## Base for every registered panel. A panel declares what it draws and where;
## PanelRegistry does the wiring, so adding a panel never touches main.gd.

const SLOT_TOP_BAR := "top_bar"
const SLOT_INSPECTOR := "inspector"
const SLOT_TIMELINE := "timeline"
const SLOT_MODAL := "modal"

## Emitted with a name from actions() and the protocol command to send.
signal action_requested(action_name: String, command: Dictionary)
## Asks the registry to select an entity, as if it had been clicked in the world.
signal select_requested(entity_type: String, entity_id: int)
## Asks the world to mark an entity (entity_id 0 clears the mark).
signal world_highlight_requested(entity_type: String, entity_id: int)

func panel_slot() -> String:
	return SLOT_INSPECTOR

## {view_model_kind: PackedStringArray of entity types it applies to}.
func view_model_kinds() -> Dictionary:
	return {}

## view_model versions this panel can render; anything else is dropped.
func supported_version(_kind: String) -> int:
	return 1

## Envelope message types (other than view_model) this panel wants verbatim.
func message_types() -> PackedStringArray:
	return PackedStringArray()

## Action names this panel may emit via action_requested.
func actions() -> PackedStringArray:
	return PackedStringArray()

func show_view_model(_kind: String, _entity_id: int, _data: Dictionary) -> void:
	pass

func on_message(_message_type: String, _message: Dictionary) -> void:
	pass

func show_command_result(_succeeded: bool, _action_name: String) -> void:
	pass
