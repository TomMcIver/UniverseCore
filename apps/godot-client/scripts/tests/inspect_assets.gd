extends SceneTree

func _initialize() -> void:
	var paths := [
		"res://assets/buildings/wall-wood.glb",
		"res://assets/buildings/wall-wood-door.glb",
		"res://assets/buildings/roof-gable.glb",
		"res://assets/buildings/roof-gable-end.glb",
		"res://assets/buildings/windmill.glb",
		"res://assets/buildings/stall-red.glb",
		"res://assets/buildings/fountain-round.glb",
		"res://assets/characters/character-a.glb",
	]
	for path in paths:
		var scene: PackedScene = load(path)
		if scene == null:
			print("%s -> FAILED TO LOAD" % path)
			continue
		var instance: Node3D = scene.instantiate()
		get_root().add_child(instance)
		var aabb := _combined_aabb(instance)
		print("%s -> size=%s center=%s" % [path, aabb.size, aabb.position])
		instance.queue_free()
	quit(0)

func _combined_aabb(node: Node3D) -> AABB:
	var result := AABB()
	var first := true
	for child in node.get_children():
		if child is MeshInstance3D:
			var mesh_aabb: AABB = child.get_aabb()
			mesh_aabb = child.transform * mesh_aabb
			if first:
				result = mesh_aabb
				first = false
			else:
				result = result.merge(mesh_aabb)
		if child is Node3D:
			var child_aabb := _combined_aabb(child)
			if child_aabb.size != Vector3.ZERO:
				child_aabb = child.transform * child_aabb
				if first:
					result = child_aabb
					first = false
				else:
					result = result.merge(child_aabb)
	return result
