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
const VillageDressingScript := preload("res://scripts/world/village_dressing.gd")
const VegetationScatterScript := preload("res://scripts/world/vegetation_scatter.gd")

var _client: Node
var _world_view: Node3D
var _panels: Control
var _terrain: Node3D
var _dressing: Node3D
var _vegetation: Node3D
var _sun: DirectionalLight3D
var _world_environment: WorldEnvironment
var _sky_material: ProceduralSkyMaterial
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

	_dressing = VillageDressingScript.new()
	_dressing.name = "VillageDressing"
	_dressing.set_terrain(_terrain)
	add_child(_dressing)
	_world_view.set_village_dressing(_dressing)

	_vegetation = VegetationScatterScript.new()
	_vegetation.name = "Vegetation"
	_vegetation.setup(_terrain, _dressing)
	add_child(_vegetation)

	_terrain.terrain_changed.connect(_on_terrain_changed)
	_dressing.exclusions_changed.connect(_vegetation.mark_dirty)
	_apply_day_night(0)

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
	_panels.set_highlight_sink(Callable(_world_view, "set_highlighted_entity"))
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
	_apply_capture_view(center)

## Debug/verification only, env-gated like the screenshot schedule: swap in a
## fixed camera so before/after screenshots can be taken from identical
## viewpoints. UNIVERSECORE_CAPTURE_VIEW = ground | oblique | overview.
const CAPTURE_VIEWS := {
	"ground": {"offset": Vector3(9.0, 1.7, 12.0), "look": Vector3(0.0, 2.2, 0.0)},
	"oblique": {"offset": Vector3(38.0, 34.0, 52.0), "look": Vector3(0.0, 0.0, 0.0)},
	"overview": {"offset": Vector3(90.0, 150.0, 170.0), "look": Vector3(-10.0, 0.0, -20.0)},
}

func _apply_capture_view(center: Vector2) -> void:
	var view := OS.get_environment("UNIVERSECORE_CAPTURE_VIEW")
	if not CAPTURE_VIEWS.has(view):
		return
	var spec: Dictionary = CAPTURE_VIEWS[view]
	var offset: Vector3 = spec["offset"]
	var look: Vector3 = spec["look"]
	var ground_at_eye: float = _terrain.get_height_at_world(center.x + offset.x, center.y + offset.z)
	var ground_at_target: float = _terrain.get_height_at_world(center.x + look.x, center.y + look.z)
	var capture := Camera3D.new()
	capture.name = "CaptureCamera"
	capture.fov = 60.0
	add_child(capture)
	capture.global_position = Vector3(center.x + offset.x, ground_at_eye + offset.y, center.y + offset.z)
	capture.look_at(Vector3(center.x + look.x, ground_at_target + look.y, center.y + look.z))
	capture.current = true

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

## Ticket #55: prototype variant A's environment. Neutral colour ambient
## (a sky-sourced ambient tinted the Kenney greens cyan), filmic tonemapping,
## soft ambient occlusion and a light glow; distance fog blends the far
## terrain into the horizon. Colours are driven per tick by DayNight.
func _setup_environment() -> void:
	_sky_material = ProceduralSkyMaterial.new()

	var sky := Sky.new()
	sky.sky_material = _sky_material

	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	environment.ssao_enabled = true
	environment.ssao_radius = 1.6
	environment.ssao_intensity = 1.8
	environment.glow_enabled = true
	environment.glow_intensity = 0.4
	environment.fog_enabled = true
	environment.fog_density = 0.0008
	environment.fog_sky_affect = 0.25
	environment.fog_sun_scatter = 0.15

	_world_environment = WorldEnvironment.new()
	_world_environment.name = "WorldEnvironment"
	_world_environment.environment = environment
	add_child(_world_environment)

func _setup_sun() -> void:
	_sun = DirectionalLight3D.new()
	_sun.name = "Sun"
	_sun.shadow_enabled = true
	_sun.directional_shadow_max_distance = 220.0
	_sun.shadow_blur = 1.5
	add_child(_sun)

func _setup_terrain() -> void:
	_terrain = Node3D.new()
	_terrain.name = "Terrain"
	_terrain.set_script(TerrainGeneratorScript)
	add_child(_terrain)

func _process(_delta: float) -> void:
	var camera := get_viewport().get_camera_3d()
	if camera != null:
		_terrain.set_focus(_ground_focus(camera))

## Where the camera's view centre meets the ground plane (or the point
## beneath the camera when it looks at the sky).
func _ground_focus(camera: Camera3D) -> Vector2:
	var origin := camera.global_position
	var forward := -camera.global_transform.basis.z
	if forward.y < -0.05:
		var distance: float = min(origin.y / -forward.y, 600.0)
		var hit := origin + forward * distance
		return Vector2(hit.x, hit.z)
	return Vector2(origin.x, origin.z)

func _on_terrain_changed(area: Rect2) -> void:
	_world_view.reground(area)
	_dressing.on_terrain_changed(area)
	if area == TerrainGeneratorScript.EVERYWHERE:
		_vegetation.mark_all_dirty()
	else:
		_vegetation.mark_dirty(area)

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
	_sun.rotation_degrees = lighting.sun_rotation_degrees
	var environment := _world_environment.environment
	environment.ambient_light_color = lighting.ambient_color
	environment.ambient_light_energy = lighting.ambient_energy
	environment.fog_light_color = lighting.fog_color
	_sky_material.sky_top_color = lighting.sky_top
	_sky_material.sky_horizon_color = lighting.sky_horizon
	_sky_material.ground_horizon_color = lighting.sky_horizon
	_sky_material.ground_bottom_color = lighting.sky_ground

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
			_terrain.configure(payload.get("regions", []), payload.get("settlements", []))
			_dressing.apply_snapshot(payload)
			if not _navmeshes_baked:
				_bake_region_navmeshes(payload.get("regions", []), payload.get("settlements", []))
				_navmeshes_baked = true
				_center_camera_on_settlements(payload.get("settlements", []))
			_world_view.apply_snapshot(payload, Callable(_terrain, "get_height_at_world"))
			_world_view.set_tick(int(message.get("tick", 0)))
			_apply_day_night(int(message.get("tick", 0)))
		"world_delta":
			# world_delta (ticket #7) is a real diff, not a restated world --
			# it carries no "regions"/full "settlements" list to regenerate
			# terrain from, and applying it as a full replace would wipe
			# every entity every tick. Terrain only reshapes around settlements
			# a delta adds or removes.
			_terrain.apply_delta(payload)
			_world_view.apply_delta(payload, Callable(_terrain, "get_height_at_world"))
			_dressing.apply_delta(payload)
			_world_view.set_tick(int(message.get("tick", 0)))
			_apply_day_night(int(message.get("tick", 0)))
		"simulation_status":
			_world_view.set_tick(int(message.get("tick", 0)))
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
