extends Node3D

## Ticket #15: connects to UniverseCore over the existing engine-neutral
## protocol and renders the live world as placeholder geometry, with basic
## simulation controls. Reuses the confirmed terrain/camera/lighting
## direction from the ticket #12 prototype rather than redesigning it.

const UniverseClientScript := preload("res://scripts/net/universe_client.gd")
const UniverseProtocol := preload("res://scripts/net/universe_protocol.gd")
const WorldViewScript := preload("res://scripts/world/world_view.gd")
const PanelRegistryScript := preload("res://scripts/ui/panel_registry.gd")
const PanelManifest := preload("res://scripts/ui/panel_manifest.gd")
const TerrainGeneratorScript := preload("res://scripts/terrain_generator.gd")
const OrbitCameraScript := preload("res://scripts/orbit_camera.gd")
const DayNight := preload("res://scripts/world/day_night.gd")
const NavmeshManagerScript := preload("res://scripts/world/navmesh_manager.gd")

var _client: Node
var _world_view: Node3D
var _panels: Control
var _terrain: MeshInstance3D
var _sun: DirectionalLight3D
var _world_environment: WorldEnvironment
var _navmesh_manager: Node3D
var _navmeshes_baked := false
var _camera: Camera3D

func _ready() -> void:
	_setup_environment()
	_setup_sun()
	_setup_terrain()
	_setup_camera()

	_navmesh_manager = NavmeshManagerScript.new()
	_navmesh_manager.name = "NavmeshManager"
	add_child(_navmesh_manager)

	_world_view = WorldViewScript.new()
	_world_view.name = "WorldView"
	add_child(_world_view)
	_world_view.set_navmesh_manager(_navmesh_manager)

	_client = UniverseClientScript.new()
	_client.name = "UniverseClient"
	add_child(_client)
	_client.connected.connect(_route_connection_status.bind("Connected"))
	_client.disconnected.connect(_route_connection_status.bind("Disconnected"))
	_client.reconnecting.connect(_route_connection_status.bind("Reconnecting..."))
	_client.message_received.connect(_on_message_received)

	_panels = PanelRegistryScript.new()
	_panels.name = "Panels"
	add_child(_panels)
	_panels.set_command_sink(Callable(_client, "send_command"))
	PanelManifest.register_all(_panels)

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

## Ticket #40: terrain is one continuous mesh (Voronoi-sampled per vertex,
## not chunked by region -- see terrain_generator.gd), so there is no
## existing per-region sub-mesh to bake a NavigationRegion3D from directly.
## Approximate each region's walkable area as a flat quad covering its
## settlements' bounding box plus a margin, at the terrain height sampled
## at that bounding box's center -- enough for the pooled/near-player
## pathing pipeline this ticket wires up to have real geometry to path
## against, without requiring true per-region terrain chunking (a larger
## change, not required by this slice).
func _bake_region_navmeshes(regions: Array, settlements: Array) -> void:
	var bounds_by_region: Dictionary = {}
	for settlement in settlements:
		var region_id := int(settlement.get("regionId", 0))
		var pos: Dictionary = settlement.get("position", {})
		var point := Vector2(float(pos.get("x", 0.0)), float(pos.get("z", 0.0)))
		if not bounds_by_region.has(region_id):
			bounds_by_region[region_id] = {"min": point, "max": point}
		else:
			var b: Dictionary = bounds_by_region[region_id]
			b["min"] = Vector2(min(b["min"].x, point.x), min(b["min"].y, point.y))
			b["max"] = Vector2(max(b["max"].x, point.x), max(b["max"].y, point.y))
			bounds_by_region[region_id] = b

	var margin := 30.0
	for region_id in bounds_by_region.keys():
		var b: Dictionary = bounds_by_region[region_id]
		var center: Vector2 = (b["min"] + b["max"]) * 0.5
		var half_extent := Vector2(
			max((b["max"].x - b["min"].x) * 0.5 + margin, margin),
			max((b["max"].y - b["min"].y) * 0.5 + margin, margin))
		var height: float = _terrain.get_height_at_world(center.x, center.y)
		var mesh := _make_region_navmesh_quad(half_extent)
		var region_transform := Transform3D(Basis(), Vector3(center.x, height, center.y))
		_navmesh_manager.rebake_region(int(region_id), mesh, region_transform, 1)

func _make_region_navmesh_quad(half_extent: Vector2) -> Mesh:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var p00 := Vector3(-half_extent.x, 0.0, -half_extent.y)
	var p10 := Vector3(half_extent.x, 0.0, -half_extent.y)
	var p01 := Vector3(-half_extent.x, 0.0, half_extent.y)
	var p11 := Vector3(half_extent.x, 0.0, half_extent.y)
	st.add_vertex(p00)
	st.add_vertex(p10)
	st.add_vertex(p01)
	st.add_vertex(p10)
	st.add_vertex(p11)
	st.add_vertex(p01)
	st.generate_normals()
	return st.commit()

## Ticket #24/#25 gave regions/settlements real canonical positions that
## can land anywhere within a world extent far larger than the camera's
## fixed starting point near origin (orbit_camera.gd's default) or the
## terrain patch generated around it -- so the camera can easily start out
## with nothing at all in view even though the population is real and
## alive. Center it on the actual average settlement position once, on the
## first snapshot, so there's always something to look at on connect.
func _center_camera_on_settlements(settlements: Array) -> void:
	if _camera == null:
		return
	var active := _active_settlements(settlements)
	if active.is_empty():
		return
	var center := _first_settlement_position(active)
	_camera.center_on(center.x, center.y)
	_panels.select("settlement", _first_settlement_id(active))

func _active_settlements(settlements: Array) -> Array:
	var active: Array = []
	for settlement in settlements:
		if bool(settlement.get("active", false)):
			active.append(settlement)
	return active

## The average of every settlement's position is not a reliable place to
## look: a long-lived, spread-out world (multiple distant settlements from
## founding/splitting) can average out to empty space between clusters
## that has nothing in it at all. The first (lowest-id) settlement's own
## real position always has real content there -- but only among *active*
## settlements: a settlement can go extinct/merge away (ticket #33) while
## keeping a lower id than every settlement still standing, which centered
## the camera/terrain on a long-dead, empty spot (observed directly against
## a long-running world).
func _first_settlement_id(settlements: Array) -> int:
	var first_id := int(settlements[0].get("id", 0))
	for settlement in settlements:
		first_id = min(first_id, int(settlement.get("id", 0)))
	return first_id

func _first_settlement_position(settlements: Array) -> Vector2:
	var first: Dictionary = settlements[0]
	for settlement in settlements:
		if int(settlement.get("id", 0)) < int(first.get("id", 0)):
			first = settlement
	var pos: Dictionary = first.get("position", {})
	return Vector2(float(pos.get("x", 0.0)), float(pos.get("z", 0.0)))

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
	environment.fog_enabled = false
	environment.fog_light_color = Color(0.78, 0.80, 0.83)
	environment.fog_density = 0.01
	environment.fog_sun_scatter = 0.2

	_world_environment = WorldEnvironment.new()
	_world_environment.name = "WorldEnvironment"
	_world_environment.environment = environment
	add_child(_world_environment)

func _setup_sun() -> void:
	_sun = DirectionalLight3D.new()
	_sun.name = "Sun"
	_sun.rotation_degrees = Vector3(-55.0, -35.0, 0.0)
	_sun.light_energy = 1.1
	_sun.light_color = Color(1.0, 0.95, 0.85)
	_sun.shadow_enabled = true
	add_child(_sun)

func _setup_terrain() -> void:
	_terrain = MeshInstance3D.new()
	_terrain.name = "Terrain"
	_terrain.set_script(TerrainGeneratorScript)
	add_child(_terrain)

func _setup_camera() -> void:
	var camera := Camera3D.new()
	camera.name = "OverviewCamera"
	camera.set_script(OrbitCameraScript)
	camera.current = true
	add_child(camera)
	_camera = camera

func _apply_day_night(tick: int) -> void:
	var lighting := DayNight.sample(tick)
	_sun.light_color = lighting.sun_color
	_sun.light_energy = lighting.sun_energy
	_world_environment.environment.ambient_light_energy = lighting.ambient_energy
	_world_environment.environment.fog_light_color = lighting.fog_color

func _route_connection_status(status: String) -> void:
	_panels.route_message({"type": "connection_status", "payload": {"status": status}})

func _on_message_received(message: Dictionary) -> void:
	_apply_world_message(message)
	_panels.route_message(message)

func _apply_world_message(message: Dictionary) -> void:
	var message_type := UniverseProtocol.message_type(message)
	var payload := UniverseProtocol.message_payload(message)

	match message_type:
		"initial_snapshot":
			_terrain.generate_from_regions(payload.get("regions", []), payload.get("settlements", []))
			if not _navmeshes_baked:
				_bake_region_navmeshes(payload.get("regions", []), payload.get("settlements", []))
				_navmeshes_baked = true
				_center_camera_on_settlements(payload.get("settlements", []))
			_world_view.apply_snapshot(payload, Callable(_terrain, "get_height_at_world"))
			_apply_day_night(int(message.get("tick", 0)))
		"world_delta":
			# world_delta (ticket #7) is a real diff, not a restated world --
			# it carries no "regions"/full "settlements" list to regenerate
			# terrain from, and applying it as a full replace would wipe
			# every entity every tick. Terrain is generated once from the
			# initial_snapshot and stays static; only the entities change.
			_world_view.apply_delta(payload, Callable(_terrain, "get_height_at_world"))
			_apply_day_night(int(message.get("tick", 0)))
		"simulation_status":
			_apply_day_night(int(message.get("tick", 0)))
			_world_view.set_simulation_playback(bool(payload.get("paused", true)), int(payload.get("speed", 1)))
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

	_panels.select(str(collider.get_meta("entity_type")), int(collider.get_meta("entity_id")))
