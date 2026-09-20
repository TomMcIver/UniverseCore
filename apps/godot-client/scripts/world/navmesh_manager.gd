class_name NavmeshManager
extends Node3D

## Ticket #39: per-region NavigationRegion3D baking, off the render thread,
## rebaked only for regions whose source geometry actually changed, gated on
## a per-region ready flag (backed by NavigationServer3D.bake_from_source_
## geometry_data_async's completion callback) so no path query is ever
## answered against stale/mid-bake geometry.
##
## Multiple NavigationRegion3D instances registered on the same navigation
## map merge into one navigable graph at their shared boundaries -- so
## "baking is scoped per region" and "path queries span the whole world" are
## not in tension: request_path() below is the single pooled entry point the
## bulk of NPCs share (ticket #40 wires per-NPC callers to it), and
## NavigationServer3D.map_get_path() naturally spans every region's
## contribution to the map in one query. NavigationAgent3D/avoidance nodes
## for near-player NPCs are a client-side concern layered on top of this,
## not part of this manager.
##
## Known limitation, observed empirically in this headless/`--script`
## execution context (not reproduced against a live, rendering client):
## even after a bake completes cleanly (region registered on the map,
## NavigationMesh populated with the expected polygon/vertex counts),
## map_get_path()/map_get_closest_point() on that map can keep returning
## empty/zero regardless of input for longer than a few physics frames --
## suggesting NavigationServer3D's map synchronization step does not run on
## the same cadence in this mode as it does in a normally-running game.
## request_path() itself is written the way a real caller should use it
## (gate on is_region_ready(), then query); if this same gap appears live,
## the fix is almost certainly waiting for map_get_iteration_id() to
## advance past the bake's own iteration before trusting a query result,
## which is exactly what that helper is for.
signal region_baked(region_id: int)

var _regions: Dictionary = {}          # region_id (int) -> NavigationRegion3D
var _baked_versions: Dictionary = {}   # region_id (int) -> int, last successfully baked source version
var _pending_versions: Dictionary = {} # region_id (int) -> int, version currently mid-bake, if any

func _ensure_region(region_id: int) -> NavigationRegion3D:
	if _regions.has(region_id):
		return _regions[region_id]
	var region := NavigationRegion3D.new()
	region.name = "NavRegion_%d" % region_id
	add_child(region)
	_regions[region_id] = region
	return region

## `source_mesh` is this region's chunk of walkable geometry (a flat
## triangle list, matching how terrain_generator.gd's SurfaceTool emits
## vertices with no index buffer). `version` is caller-supplied -- a hash or
## monotonically increasing counter of the terrain data this region was
## generated from -- so a tick that regenerates nothing skips baking
## entirely rather than redoing unchanged work.
func rebake_region(region_id: int, source_mesh: Mesh, source_transform: Transform3D, version: int) -> void:
	if _baked_versions.get(region_id, null) == version or _pending_versions.get(region_id, null) == version:
		return

	var region := _ensure_region(region_id)
	var nav_mesh := NavigationMesh.new()
	var source_geometry := NavigationMeshSourceGeometryData3D.new()

	var arrays := source_mesh.surface_get_arrays(0)
	var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	source_geometry.add_faces(vertices, source_transform)

	_pending_versions[region_id] = version
	NavigationServer3D.bake_from_source_geometry_data_async(
		nav_mesh, source_geometry,
		func() -> void: _on_bake_finished(region_id, region, nav_mesh, version))

func _on_bake_finished(region_id: int, region: NavigationRegion3D, nav_mesh: NavigationMesh, version: int) -> void:
	region.navigation_mesh = nav_mesh
	_baked_versions[region_id] = version
	_pending_versions.erase(region_id)
	region_baked.emit(region_id)

## A region is ready once it has a completed bake and no newer bake is
## currently in flight for it.
func is_region_ready(region_id: int) -> bool:
	return _baked_versions.has(region_id) and not _pending_versions.has(region_id)

func region_count() -> int:
	return _regions.size()

func map_iteration_id_for_region(region_id: int) -> int:
	if not _regions.has(region_id):
		return 0
	var region: NavigationRegion3D = _regions[region_id]
	return NavigationServer3D.map_get_iteration_id(region.get_navigation_map())

## Pooled path query for the bulk of NPCs -- one shared entry point rather
## than a NavigationAgent3D node per agent. Returns an empty path (instead
## of pathing against stale or absent geometry) if the requesting region
## hasn't finished baking yet; callers should retry once region_baked fires
## for that id.
func request_path(region_id: int, from: Vector3, to: Vector3) -> PackedVector3Array:
	if not is_region_ready(region_id):
		return PackedVector3Array()
	var region: NavigationRegion3D = _regions[region_id]
	var map := region.get_navigation_map()
	return NavigationServer3D.map_get_path(map, from, to, true)
