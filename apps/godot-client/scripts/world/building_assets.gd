class_name BuildingAssets
extends RefCounted

## Ticket #19: real CC0 asset kit models replacing the CSG placeholder
## geometry. Sources and licenses are recorded in
## apps/godot-client/assets/CREDITS.md -- all CC0, no attribution required,
## but recorded anyway per that ticket's explicit requirement.
##
## Dwelling is assembled from two modular Kenney Fantasy Town Kit pieces
## (a single wall-with-door panel + a gable roof cap) rather than a fully
## enclosed four-wall box: the kit ships walls/roofs as separate grid
## modules with no complete "house" mesh, and this session had no way to
## visually verify a 4-wall enclosure aligns correctly beyond checking
## bounding boxes headlessly. A single detailed wall+roof facade reads
## reasonably as a small building from the elevated overview camera this
## client uses; it is not a fully-modeled enclosed structure. Workshop and
## FoodStore use single complete meshes (windmill, market stall) and carry
## no such caveat.

const WALL_DOOR := preload("res://assets/buildings/wall-wood-door.glb")
const ROOF_GABLE := preload("res://assets/buildings/roof-gable.glb")
const WINDMILL := preload("res://assets/buildings/windmill.glb")
const STALL := preload("res://assets/buildings/stall-red.glb")
const FOUNTAIN := preload("res://assets/buildings/fountain-round.glb")

const CHARACTER_FEMALE := preload("res://assets/characters/character-a.glb")
const CHARACTER_MALE := preload("res://assets/characters/character-b.glb")

const DWELLING_WALL_HEIGHT := 1.0
const PERSON_SOURCE_HEIGHT := 2.7
const PERSON_TARGET_HEIGHT := 1.8

static func make_building(building_type: String) -> Node3D:
	match building_type:
		"Workshop":
			return WINDMILL.instantiate()
		"FoodStore":
			return STALL.instantiate()
		_:
			return _make_dwelling()

static func _make_dwelling() -> Node3D:
	var root := Node3D.new()
	root.add_child(WALL_DOOR.instantiate())
	var roof: Node3D = ROOF_GABLE.instantiate()
	roof.position = Vector3(0.0, DWELLING_WALL_HEIGHT, 0.0)
	root.add_child(roof)
	return root

static func make_settlement_marker() -> Node3D:
	return FOUNTAIN.instantiate()

static func make_person(sex: String) -> Node3D:
	var scene: PackedScene = CHARACTER_FEMALE if sex == "Female" else CHARACTER_MALE
	var instance: Node3D = scene.instantiate()
	instance.scale = Vector3.ONE * (PERSON_TARGET_HEIGHT / PERSON_SOURCE_HEIGHT)
	return instance
