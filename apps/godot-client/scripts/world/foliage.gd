extends RefCounted

## Kenney Nature Kit meshes prepared once for multimesh scattering.
##
## - Palette: the Nature Kit's greens are mint/teal by design and clash with
##   the Fantasy Town Kit; mint hues are shifted toward the Town Kit's leaf
##   green at load time instead of editing the source files.
## - "Black grass" (ticket #48 known defect): grass.glb and the other thin
##   foliage ship half their vertex normals pointing downward (the back-to-
##   back blade faces), so every blade whose visible face carried a downward
##   normal was lit from below and rendered near-black under the sun.
##   Foliage meshes get their normals replaced with straight up, so every
##   blade shades like the ground it grows from -- the standard low-poly
##   foliage treatment.

const UP_NORMAL_SCENES := ["grass", "flower", "bush", "crops"]
## Nature Kit wheat is near-white; ripen it so fields read as grain from above.
const CROP_TINT := Color(0.95, 0.78, 0.42)

static var _prepared: Dictionary = {}
static var _tinted_materials: Dictionary = {}

## Returns {"mesh": Mesh, "transform": Transform3D} for the first mesh in
## `scene` (Nature Kit models are a single mesh with 1-2 surfaces).
static func prepared(scene: PackedScene) -> Dictionary:
	var key := scene.resource_path
	if _prepared.has(key):
		return _prepared[key]
	var instance := scene.instantiate()
	var found := _first_mesh(instance, Transform3D.IDENTITY)
	instance.free()
	if found.is_empty():
		return {}
	var source: Mesh = found["mesh"]
	var face_up := false
	for token in UP_NORMAL_SCENES:
		if key.get_file().begins_with(token):
			face_up = true
	var result := {"mesh": _rebuilt_mesh(source, face_up, key.get_file().begins_with("crops")), "transform": found["transform"]}
	_prepared[key] = result
	return result

static func tinted_material(material: Material) -> Material:
	if not (material is BaseMaterial3D):
		return material
	if _tinted_materials.has(material):
		return _tinted_materials[material]
	var base: BaseMaterial3D = material
	var color := base.albedo_color
	var out: BaseMaterial3D = base
	if color.h > 0.38 and color.h < 0.56 and color.s > 0.25:
		out = base.duplicate()
		out.albedo_color = Color.from_hsv(0.29, min(color.s * 1.05, 0.75), color.v * 0.82)
	_tinted_materials[material] = out
	return out

## Applies the palette shift to every mesh under `node` (for foliage that
## is instantiated as a scene rather than multimesh-scattered).
static func tint_tree(node: Node) -> void:
	if node is MeshInstance3D:
		var mesh_instance: MeshInstance3D = node
		for surface in mesh_instance.mesh.get_surface_count():
			mesh_instance.set_surface_override_material(surface, tinted_material(mesh_instance.mesh.surface_get_material(surface)))
	for child in node.get_children():
		tint_tree(child)

static func make_multimesh(scene: PackedScene, transforms: Array[Transform3D], visibility_end: float = 0.0) -> MultiMeshInstance3D:
	var prep := prepared(scene)
	if prep.is_empty() or transforms.is_empty():
		return null
	var local: Transform3D = prep["transform"]
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.mesh = prep["mesh"]
	multimesh.instance_count = transforms.size()
	for index in transforms.size():
		multimesh.set_instance_transform(index, transforms[index] * local)
	var instance := MultiMeshInstance3D.new()
	instance.multimesh = multimesh
	if visibility_end > 0.0:
		instance.visibility_range_end = visibility_end
		instance.visibility_range_end_margin = visibility_end * 0.1
		instance.visibility_range_fade_mode = GeometryInstance3D.VISIBILITY_RANGE_FADE_SELF
	return instance

static func _rebuilt_mesh(source: Mesh, face_up: bool, is_crop: bool = false) -> Mesh:
	var mesh := ArrayMesh.new()
	for surface in source.get_surface_count():
		var arrays := source.surface_get_arrays(surface)
		if face_up:
			var normals := PackedVector3Array()
			normals.resize((arrays[Mesh.ARRAY_VERTEX] as PackedVector3Array).size())
			normals.fill(Vector3.UP)
			arrays[Mesh.ARRAY_NORMAL] = normals
			arrays[Mesh.ARRAY_TANGENT] = null
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
		var material := tinted_material(source.surface_get_material(surface))
		if is_crop and material is BaseMaterial3D:
			material = (material as BaseMaterial3D).duplicate()
			material.albedo_color = (material as BaseMaterial3D).albedo_color * CROP_TINT
		mesh.surface_set_material(surface, material)
	return mesh

static func _first_mesh(node: Node, parent: Transform3D) -> Dictionary:
	var local := parent
	if node is Node3D:
		local = parent * (node as Node3D).transform
	if node is MeshInstance3D and (node as MeshInstance3D).mesh != null:
		return {"mesh": (node as MeshInstance3D).mesh, "transform": local}
	for child in node.get_children():
		var found := _first_mesh(child, local)
		if not found.is_empty():
			return found
	return {}
