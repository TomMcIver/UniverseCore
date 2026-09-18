extends Node3D

## Ticket #15: connects to UniverseCore over the existing engine-neutral
## protocol and renders the live world as placeholder geometry, with basic
## simulation controls. Reuses the confirmed terrain/camera/lighting
## direction from the ticket #12 prototype rather than redesigning it.

const UniverseClientScript := preload("res://scripts/net/universe_client.gd")
const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")
const WorldViewScript := preload("res://scripts/world/world_view.gd")
const HudScript := preload("res://scripts/ui/hud.gd")
const TerrainGeneratorScript := preload("res://scripts/terrain_generator.gd")
const OrbitCameraScript := preload("res://scripts/orbit_camera.gd")

var _client: Node
var _world_view: Node3D
var _hud: Control

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

	_client = UniverseClientScript.new()
	_client.name = "UniverseClient"
	add_child(_client)
	_client.connected.connect(_on_connected)
	_client.disconnected.connect(_on_disconnected)
	_client.message_received.connect(_on_message_received)

	_hud.pause_pressed.connect(func() -> void: _client.send_command(UniverseProtocol.command_pause()))
	_hud.resume_pressed.connect(func() -> void: _client.send_command(UniverseProtocol.command_resume()))
	_hud.step_pressed.connect(func() -> void: _client.send_command(UniverseProtocol.command_single_step()))
	_hud.speed_selected.connect(func(speed: int) -> void: _client.send_command(UniverseProtocol.command_set_speed(speed)))

	_client.connect_to_bridge()

	var screenshot_path := OS.get_environment("UNIVERSECORE_SCREENSHOT")
	if screenshot_path != "":
		await get_tree().create_timer(2.5).timeout
		var image := get_viewport().get_texture().get_image()
		image.save_png(screenshot_path)
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
		"error":
			push_warning("UniverseCore error: %s" % payload.get("message", "unknown"))
		_:
			pass
