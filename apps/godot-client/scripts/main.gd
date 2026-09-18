extends Node3D

## Ticket #15: connects to UniverseCore over the existing engine-neutral
## protocol and renders the live world as placeholder geometry, with basic
## simulation controls. Reuses the confirmed terrain/camera/lighting
## direction from the ticket #12 prototype rather than redesigning it.

const UniverseClientScript := preload("res://scripts/net/universe_client.gd")
const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")
const WorldViewScript := preload("res://scripts/world/world_view.gd")
const HudScript := preload("res://scripts/ui/hud.gd")
const InspectorPanelScript := preload("res://scripts/ui/inspector_panel.gd")
const EventLogScript := preload("res://scripts/ui/event_log.gd")
const TerrainGeneratorScript := preload("res://scripts/terrain_generator.gd")
const OrbitCameraScript := preload("res://scripts/orbit_camera.gd")

var _client: Node
var _world_view: Node3D
var _hud: Control
var _inspector: PanelContainer
var _event_log: PanelContainer

func _ready() -> void:
	_setup_environment()
	_setup_sun()
	_setup_terrain()
	_setup_camera()

	_world_view = WorldViewScript.new()
	_world_view.name = "WorldView"
	add_child(_world_view)

	_hud = HudScript.new()
	_hud.name = "Hud"
	_hud.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(_hud)

	_inspector = InspectorPanelScript.new()
	_inspector.name = "Inspector"
	_inspector.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_inspector.position = Vector2(-232, 12)
	_hud.add_child(_inspector)

	_event_log = EventLogScript.new()
	_event_log.name = "EventLog"
	_event_log.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	_event_log.position = Vector2(12, -172)
	_hud.add_child(_event_log)

	_client = UniverseClientScript.new()
	_client.name = "UniverseClient"
	add_child(_client)
	_client.connected.connect(_on_connected)
	_client.disconnected.connect(_on_disconnected)
	_client.reconnecting.connect(_on_reconnecting)
	_client.message_received.connect(_on_message_received)

	_hud.pause_pressed.connect(func() -> void:
		_hud.show_command_result(_client.send_command(UniverseProtocol.command_pause()), "Pause"))
	_hud.resume_pressed.connect(func() -> void:
		_hud.show_command_result(_client.send_command(UniverseProtocol.command_resume()), "Resume"))
	_hud.step_pressed.connect(func() -> void:
		_hud.show_command_result(_client.send_command(UniverseProtocol.command_single_step()), "Step"))
	_hud.speed_selected.connect(func(speed: int) -> void:
		_hud.show_command_result(_client.send_command(UniverseProtocol.command_set_speed(speed)), "Set speed"))

	_client.connect_to_bridge()

	var schedule_env := OS.get_environment("UNIVERSECORE_SCREENSHOT_SCHEDULE")
	var dir_env := OS.get_environment("UNIVERSECORE_SCREENSHOT_DIR")
	if schedule_env != "" and dir_env != "":
		_run_screenshot_schedule(schedule_env, dir_env)

	# Debug/verification only, env-var gated: simulate a click at a fixed
	# screen point without a real mouse, so the full pick -> request ->
	# inspector round trip can be captured in a screenshot. "x,y,delay".
	var debug_click_env := OS.get_environment("UNIVERSECORE_DEBUG_CLICK")
	if debug_click_env != "":
		_run_debug_click(debug_click_env)

func _run_debug_click(debug_click_env: String) -> void:
	var parts := debug_click_env.split(",")
	await get_tree().create_timer(float(parts[2])).timeout
	_handle_click(Vector2(float(parts[0]), float(parts[1])))

func _run_screenshot_schedule(schedule_env: String, dir: String) -> void:
	var previous_seconds := 0.0
	var index := 0
	for part in schedule_env.split(","):
		var seconds := float(part)
		await get_tree().create_timer(seconds - previous_seconds).timeout
		previous_seconds = seconds
		var image := get_viewport().get_texture().get_image()
		image.save_png("%s/shot_%d_t%s.png" % [dir, index, part])
		index += 1
	get_tree().quit()

func _setup_environment() -> void:
	var sky_material := ProceduralSkyMaterial.new()
	sky_material.sky_top_color = Color(0.38, 0.52, 0.72)
	sky_material.sky_horizon_color = Color(0.75, 0.78, 0.80)
	sky_material.ground_horizon_color = Color(0.55, 0.52, 0.46)

	var sky := Sky.new()
	sky.sky_material = sky_material

	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.fog_enabled = true
	environment.fog_light_color = Color(0.78, 0.80, 0.83)
	environment.fog_density = 0.01
	environment.fog_sun_scatter = 0.2

	var world_environment := WorldEnvironment.new()
	world_environment.name = "WorldEnvironment"
	world_environment.environment = environment
	add_child(world_environment)

func _setup_sun() -> void:
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.rotation_degrees = Vector3(-55.0, -35.0, 0.0)
	sun.light_energy = 1.1
	sun.light_color = Color(1.0, 0.95, 0.85)
	sun.shadow_enabled = true
	add_child(sun)

func _setup_terrain() -> void:
	var terrain := MeshInstance3D.new()
	terrain.name = "Terrain"
	terrain.set_script(TerrainGeneratorScript)
	add_child(terrain)

func _setup_camera() -> void:
	var camera := Camera3D.new()
	camera.name = "OverviewCamera"
	camera.set_script(OrbitCameraScript)
	camera.current = true
	add_child(camera)

func _on_connected() -> void:
	_hud.set_connection_status("Connected")

func _on_disconnected() -> void:
	_hud.set_connection_status("Disconnected")

func _on_reconnecting() -> void:
	_hud.set_connection_status("Reconnecting...")

func _on_message_received(message: Dictionary) -> void:
	var message_type := UniverseProtocol.message_type(message)
	var payload := UniverseProtocol.message_payload(message)

	match message_type:
		"initial_snapshot", "world_delta":
			_world_view.apply_snapshot(payload)
			_hud.set_world_info(
				int(payload.get("year", 0)),
				int(message.get("tick", 0)),
				int(payload.get("population", 0))
			)
		"simulation_status":
			var paused: bool = payload.get("paused", true)
			var speed: int = int(payload.get("speed", 1))
			_hud.set_connection_status("Connected -- %s (%dx)" % ["Paused" if paused else "Running", speed])
		"entity_details":
			_inspector.show_agent_details(payload)
		"world_event":
			_event_log.append_event(payload)
		"error":
			push_warning("UniverseCore error: %s" % payload.get("message", "unknown"))
		_:
			pass

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
		_handle_click(event.position)

func _handle_click(screen_position: Vector2) -> void:
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return

	var from := camera.project_ray_origin(screen_position)
	var to := from + camera.project_ray_normal(screen_position) * 1000.0
	var query := PhysicsRayQueryParameters3D.create(from, to)
	query.collide_with_areas = true
	query.collide_with_bodies = false

	var result := get_world_3d().direct_space_state.intersect_ray(query)
	if result.is_empty():
		return

	var collider = result.get("collider")
	if collider == null or not collider.has_meta("entity_type"):
		return

	var entity_type: String = collider.get_meta("entity_type")
	var entity_id: int = int(collider.get_meta("entity_id"))

	if entity_type == "agent":
		_client.send_command(UniverseProtocol.command_request_entity_details(entity_id))
	elif entity_type == "building":
		_inspector.show_building_details(_world_view.building_data(entity_id))
