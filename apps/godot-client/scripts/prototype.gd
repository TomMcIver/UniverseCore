extends Node3D

## Wayfinder ticket #12 prototype: a naturalistic terrain patch, an
## atmospheric lighting setup, an elevated overview camera, and a handful of
## placeholder buildings/units. Throwaway code whose job is to answer "does
## this feel like the Bannerlord/Manor Lords direction" before spec #14's
## real build (tickets #15-#19) begins. No networking, no real assets.

const TerrainGeneratorScript := preload("res://scripts/terrain_generator.gd")
const OrbitCameraScript := preload("res://scripts/orbit_camera.gd")

func _ready() -> void:
	_setup_environment()
	_setup_sun()
	_setup_terrain()
	_setup_camera()
	_setup_placeholder_settlement()
	var screenshot_path := OS.get_environment("UNIVERSECORE_SCREENSHOT")
	if screenshot_path != "":
		await get_tree().create_timer(1.0).timeout
		_capture_screenshot(screenshot_path)

func _capture_screenshot(path: String) -> void:
	var image := get_viewport().get_texture().get_image()
	image.save_png(path)
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
	environment.fog_density = 0.02
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

func _setup_placeholder_settlement() -> void:
	var building_positions := [
		Vector2(18.0, 16.0),
		Vector2(23.0, 20.0),
		Vector2(16.0, 24.0),
		Vector2(28.0, 18.0),
		Vector2(22.0, 27.0),
	]
	for pos in building_positions:
		add_child(_make_building(pos))

	var unit_positions := [
		Vector2(20.0, 22.0),
		Vector2(25.0, 24.0),
		Vector2(19.0, 19.0),
	]
	for pos in unit_positions:
		add_child(_make_unit(pos))

func _make_building(pos: Vector2) -> CSGBox3D:
	var building := CSGBox3D.new()
	building.size = Vector3(3.0, 3.2, 3.0)
	building.position = Vector3(pos.x, 1.6, pos.y)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.55, 0.47, 0.37)
	building.material = material
	return building

func _make_unit(pos: Vector2) -> CSGCylinder3D:
	var unit := CSGCylinder3D.new()
	unit.radius = 0.35
	unit.height = 1.7
	unit.position = Vector3(pos.x, 0.85, pos.y)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.32, 0.26, 0.20)
	unit.material = material
	return unit
