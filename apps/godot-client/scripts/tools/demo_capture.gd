extends Node

## Scripted capture of the whole demo sequence (#41), for verification and
## docs. Env-gated by UNIVERSECORE_DEMO_CAPTURE_DIR and run against a live
## bridge started from the demo scenario. It saves the client's own viewport
## only, never the desktop, so nothing outside the Godot window can be captured.
##
## Sequence: command view at year 0, the settlement economy panel, a jump of
## Hud.JUMP_YEARS years, the command view and economy again, descent to the
## ground view next to the marked demo NPC, and that NPC's recorded life history.

const CONNECT_SECONDS := 12.0
const JUMP_TIMEOUT_SECONDS := 180.0
const SETTLE_SECONDS := 2.5
const BESIDE_NPC_OFFSET := Vector3(2.5, 0.5, 2.5)

var _dir := ""
var _panels: Node
var _world_view: Node3D
var _ground_view: Node3D

func run(dir: String, panels: Node, world_view: Node3D, ground_view: Node3D) -> void:
	_dir = dir
	_panels = panels
	_world_view = world_view
	_ground_view = ground_view

	await get_tree().create_timer(CONNECT_SECONDS).timeout
	var hud = _find_panel("Hud")
	var settlements: Array = _world_view.active_settlement_ids()
	if hud == null or settlements.is_empty():
		push_error("DEMO_CAPTURE: no HUD or no settlement after %s s" % CONNECT_SECONDS)
		get_tree().quit(1)
		return
	var settlement_id: int = settlements[0]
	var start_year: int = hud.current_year()
	var start_population: int = _world_view.agent_count()
	_shot("1_command_view_year%d" % start_year)

	_panels.select("settlement", settlement_id)
	await _settle()
	_shot("2_economy_year%d" % start_year)

	hud.request_jump()
	var waited := 0.0
	while hud.current_year() < start_year + hud.JUMP_YEARS and waited < JUMP_TIMEOUT_SECONDS:
		await get_tree().process_frame
		waited += get_process_delta_time()
	await _settle()
	var end_year: int = hud.current_year()
	_shot("3_command_view_year%d" % end_year)

	settlements = _world_view.active_settlement_ids()
	_panels.select("settlement", settlement_id)
	await _settle()
	_shot("4_economy_year%d" % end_year)

	var focus_id: int = _world_view.highlighted_agent_id()
	_ground_view.descend()
	while _ground_view.mode != _ground_view.Mode.GROUND:
		await get_tree().process_frame
	var focus_node: Node3D = _world_view.agent_node(focus_id) if focus_id > 0 else null
	if focus_node != null:
		_ground_view.avatar().global_position = focus_node.global_position + BESIDE_NPC_OFFSET
	await _settle()
	_shot("5_ground_view_beside_demo_npc")

	if focus_id > 0:
		_panels.select("agent", focus_id)
		await _settle()
		_shot("6_demo_npc_life_history")

	print("DEMO_CAPTURE years=%d..%d settlements=%d people_shown=%d (start %d) focus_npc=%d jump_waited=%.1fs" % [
		start_year, end_year, settlements.size(), _world_view.agent_count(), start_population, focus_id, waited])
	get_tree().quit()

func _find_panel(panel_name: String) -> Node:
	for panel in _panels.panels():
		if panel.name == panel_name:
			return panel
	return null

func _settle() -> void:
	await get_tree().create_timer(SETTLE_SECONDS).timeout

func _shot(label: String) -> void:
	get_viewport().get_texture().get_image().save_png("%s/%s.png" % [_dir, label])
