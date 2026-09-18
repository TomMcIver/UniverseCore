extends Node3D

func _ready() -> void:
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-45.0, -30.0, 0.0)
	sun.light_energy = 1.2
	add_child(sun)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.5, 3.0)
	camera.current = true
	add_child(camera)
	camera.look_at(Vector3(0.0, 1.0, 0.0), Vector3.UP)

	var character_scene: PackedScene = load("res://assets/characters/character-a.glb")
	var character: Node3D = character_scene.instantiate()
	add_child(character)

	print("character children: ", character.get_children())
	_print_mesh_info(character)

	await get_tree().create_timer(0.5).timeout
	var image := get_viewport().get_texture().get_image()
	image.save_png(OS.get_environment("UNIVERSECORE_SCREENSHOT"))
	get_tree().quit()

func _print_mesh_info(node: Node) -> void:
	if node is MeshInstance3D:
		var mesh_instance: MeshInstance3D = node
		print("MeshInstance3D: ", mesh_instance.name, " surfaces=", mesh_instance.mesh.get_surface_count() if mesh_instance.mesh else 0)
		if mesh_instance.mesh:
			for i in range(mesh_instance.mesh.get_surface_count()):
				var mat := mesh_instance.get_active_material(i)
				print("  surface ", i, " material=", mat, " albedo_texture=", (mat.albedo_texture if mat is StandardMaterial3D else "n/a"))
	for child in node.get_children():
		_print_mesh_info(child)
