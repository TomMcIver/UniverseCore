extends Node3D

const BuildingAssets := preload("res://scripts/world/building_assets.gd")

func _ready() -> void:
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-45.0, -30.0, 0.0)
	sun.light_energy = 1.2
	add_child(sun)

	var dwelling := BuildingAssets.make_building("Dwelling")
	dwelling.position = Vector3(-5.0, 0.0, 0.0)
	add_child(dwelling)

	var workshop := BuildingAssets.make_building("Workshop")
	workshop.position = Vector3(1.0, 0.0, 0.0)
	add_child(workshop)

	var foodstore := BuildingAssets.make_building("FoodStore")
	foodstore.position = Vector3(6.0, 0.0, 0.0)
	add_child(foodstore)

	var person := BuildingAssets.make_person("Female")
	person.position = Vector3(-2.0, 0.0, 2.0)
	add_child(person)

	var camera := Camera3D.new()
	camera.position = Vector3(1.0, 6.0, 16.0)
	add_child(camera)
	camera.current = true
	camera.look_at(Vector3(1.0, 1.5, 0.0), Vector3.UP)

	await get_tree().create_timer(0.5).timeout
	var image := get_viewport().get_texture().get_image()
	image.save_png(OS.get_environment("UNIVERSECORE_SCREENSHOT"))
	get_tree().quit()
