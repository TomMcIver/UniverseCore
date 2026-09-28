class_name BuildingAssets
extends RefCounted

## Building, settlement-centre and person visuals for the live client.
## Sources and licenses: apps/godot-client/assets/CREDITS.md (all CC0).
##
## Ticket #55 (demo village look, per the #48 decision): only the plain,
## Bronze/Iron Age-plausible Fantasy Town Kit pieces -- timber walls with
## wooden shutters, gable roofs, carts, fences. No fountain, lamp posts,
## glazed/stone houses or watermill; the settlement centre is a firepit.
##
## Kenney wall convention (verified by AABB: wall-wood.glb spans local
## x:[0.4, 0.5], z:[-0.5, 0.5]): a wall piece rotated by 0/90/180/270 degrees
## around Y lands on the +X/-Z/-X/+Z side of its 1x1 cell. Doors are on +X,
## so a building faces +X locally; world_view rotates it to face its
## settlement's centre.
##
## Construction state is shown, not faked (live-client defect B2): Planned is
## a staked-out plot, ActiveConstruction is roofless walls at a height that
## follows `progress`, Complete is the finished building.

const WALL := preload("res://assets/buildings/wall-wood.glb")
const WALL_DOOR := preload("res://assets/buildings/wall-wood-door.glb")
const WALL_WINDOW := preload("res://assets/buildings/wall-wood-window-shutters.glb")
const ROOF_GABLE := preload("res://assets/buildings/roof-gable.glb")
const ROOF_HIGH := preload("res://assets/buildings/roof-high-gable.glb")
const CART := preload("res://assets/buildings/cart.glb")
const FENCE := preload("res://assets/nature/fence_simple.glb")
const ROCK_SMALL := preload("res://assets/nature/rock_smallA.glb")
const STUMP := preload("res://assets/nature/stump_round.glb")
const FoliageLib := preload("res://scripts/world/foliage.gd")

const CHARACTER_FEMALE := preload("res://assets/characters/character-a.glb")
const CHARACTER_MALE := preload("res://assets/characters/character-b.glb")

const PERSON_SOURCE_HEIGHT := 2.7
const PERSON_TARGET_HEIGHT := 1.8

## Houses about 3.4 m across (the #48 scale decision), against 1.8 m people.
const SCALE_BY_TYPE := {"Dwelling": 3.4, "Workshop": 3.0, "FoodStore": 2.4}
## Cells along local Z: a workshop is a two-cell longhouse.
const CELLS_BY_TYPE := {"Dwelling": 1, "Workshop": 2, "FoodStore": 1}
const WALL_HEIGHT := 1.0
const ROOF_HEIGHT_BY_TYPE := {"Dwelling": 0.57, "Workshop": 0.57, "FoodStore": 1.11}
const MIN_CONSTRUCTION_WALL_FRACTION := 0.25

## Physics layer the player avatar collides with; picking uses Area3Ds on
## layer 1 with collide_with_bodies = false, so these never steal clicks.
const WORLD_COLLISION_LAYER := 2

const FIREPIT_RADIUS := 0.8
const PLAZA_RADIUS := 2.6

static func make_building(building_type: String, state: String = "Complete", progress: float = 1.0) -> Node3D:
	var kind := building_type if SCALE_BY_TYPE.has(building_type) else "Dwelling"
	var root := Node3D.new()
	root.name = "%s_%s" % [kind, state]
	var body := Node3D.new()
	body.scale = Vector3.ONE * float(SCALE_BY_TYPE[kind])
	root.add_child(body)

	var cells := int(CELLS_BY_TYPE[kind])
	# Centre multi-cell buildings on the origin along Z.
	var z_shift := -0.5 * float(cells - 1)
	if state == "Planned":
		_add_plot_stakes(body, cells, z_shift)
		return root

	var walls := Node3D.new()
	walls.position.z = z_shift
	body.add_child(walls)
	_add_walls(walls, kind, cells)
	if state == "Complete":
		_add_roofs(walls, kind, cells)
		_add_dressing(root, kind)
	else:
		walls.scale.y = wall_fraction(state, progress)
		_add_timber_pile(root, kind)
	return root

## Fraction of full wall height shown for a building in `state`.
static func wall_fraction(state: String, progress: float) -> float:
	match state:
		"Complete":
			return 1.0
		"Planned":
			return 0.0
		_:
			return lerp(MIN_CONSTRUCTION_WALL_FRACTION, 1.0, clamp(progress, 0.0, 1.0))

## World-space size (x, height, z) of a building's footprint box.
static func footprint(building_type: String, state: String = "Complete", progress: float = 1.0) -> Vector3:
	var kind := building_type if SCALE_BY_TYPE.has(building_type) else "Dwelling"
	var scale := float(SCALE_BY_TYPE[kind])
	var height := WALL_HEIGHT * wall_fraction(state, progress)
	if state == "Complete":
		height += float(ROOF_HEIGHT_BY_TYPE[kind])
	return Vector3(scale, max(height * scale, 0.3), scale * float(CELLS_BY_TYPE[kind]))

## Simplified box collision for the player avatar. Imported Kenney models
## ship no collision, so none is assumed. A staked-out plot has nothing to
## collide with.
static func make_collision_body(building_type: String, state: String, progress: float) -> StaticBody3D:
	if state == "Planned":
		return null
	var size := footprint(building_type, state, progress)
	var body := StaticBody3D.new()
	body.name = "Collision"
	body.collision_layer = WORLD_COLLISION_LAYER
	body.collision_mask = 0
	var shape := BoxShape3D.new()
	shape.size = size
	var collision := CollisionShape3D.new()
	collision.shape = shape
	collision.position.y = size.y * 0.5
	body.add_child(collision)
	return body

## Settlement centre: a stone-ringed firepit with log seats (replaces the
## fountain, which read as a medieval town square). An extinct settlement's
## fire is cold.
static func make_settlement_marker(active: bool = true) -> Node3D:
	var root := Node3D.new()
	root.name = "Firepit"
	for index in 8:
		var angle := TAU * float(index) / 8.0
		var rock: Node3D = ROCK_SMALL.instantiate()
		rock.scale = Vector3.ONE * 2.4
		rock.position = Vector3(cos(angle), 0.0, sin(angle)) * FIREPIT_RADIUS
		rock.rotation.y = angle
		FoliageLib.tint_tree(rock)
		root.add_child(rock)
	var embers := MeshInstance3D.new()
	var disc := CylinderMesh.new()
	disc.top_radius = FIREPIT_RADIUS * 0.7
	disc.bottom_radius = FIREPIT_RADIUS * 0.7
	disc.height = 0.08
	embers.mesh = disc
	var ember_material := StandardMaterial3D.new()
	ember_material.albedo_color = Color(0.22, 0.12, 0.08)
	ember_material.emission_enabled = active
	ember_material.emission = Color(1.0, 0.42, 0.12)
	ember_material.emission_energy_multiplier = 0.6
	embers.material_override = ember_material
	embers.position.y = 0.04
	root.add_child(embers)
	for index in 3:
		var angle := TAU * (float(index) / 3.0 + 0.08)
		var seat: Node3D = STUMP.instantiate()
		seat.scale = Vector3.ONE * 2.6
		seat.position = Vector3(cos(angle), 0.0, sin(angle)) * (FIREPIT_RADIUS + 1.1)
		FoliageLib.tint_tree(seat)
		root.add_child(seat)
	return root

static func make_person(sex: String) -> Node3D:
	var scene: PackedScene = CHARACTER_FEMALE if sex == "Female" else CHARACTER_MALE
	var instance: Node3D = scene.instantiate()
	instance.scale = Vector3.ONE * (PERSON_TARGET_HEIGHT / PERSON_SOURCE_HEIGHT)
	return instance

static func _add_walls(walls: Node3D, kind: String, cells: int) -> void:
	for cell in cells:
		var origin := Vector3(0.0, 0.0, float(cell))
		var front: PackedScene = WALL_DOOR if cell == 0 else WALL_WINDOW
		var back: PackedScene = WALL_WINDOW if kind == "Dwelling" else WALL
		_add_piece(walls, front, origin, 0.0)
		_add_piece(walls, back, origin, 180.0)
		if cell == 0:
			_add_piece(walls, WALL, origin, 90.0)
		if cell == cells - 1:
			_add_piece(walls, WALL if kind == "FoodStore" else WALL_WINDOW, origin, 270.0)

static func _add_roofs(walls: Node3D, kind: String, cells: int) -> void:
	for cell in cells:
		var roof: Node3D = (ROOF_HIGH if kind == "FoodStore" else ROOF_GABLE).instantiate()
		roof.position = Vector3(0.0, WALL_HEIGHT, float(cell))
		if cells > 1:
			roof.rotation_degrees.y = 90.0
		walls.add_child(roof)

static func _add_piece(parent: Node3D, scene: PackedScene, origin: Vector3, degrees: float) -> void:
	var piece: Node3D = scene.instantiate()
	piece.position = origin
	piece.rotation_degrees.y = degrees
	parent.add_child(piece)

## Four fence runs outlining the footprint, as surveyed stakes.
static func _add_plot_stakes(body: Node3D, cells: int, z_shift: float) -> void:
	var half_z := 0.5 * float(cells)
	var runs := [
		[Vector3(0.5, 0.0, z_shift), 90.0, float(cells)],
		[Vector3(-0.5, 0.0, z_shift), 90.0, float(cells)],
		[Vector3(0.0, 0.0, z_shift - half_z), 0.0, 1.0],
		[Vector3(0.0, 0.0, z_shift + half_z), 0.0, 1.0],
	]
	for run in runs:
		var fence: Node3D = FENCE.instantiate()
		fence.position = run[0]
		fence.rotation_degrees.y = run[1]
		fence.scale = Vector3(run[2], 0.8, 1.0)
		FoliageLib.tint_tree(fence)
		body.add_child(fence)

static func _add_timber_pile(root: Node3D, kind: String) -> void:
	var scale := float(SCALE_BY_TYPE[kind])
	var offset := Vector3(0.0, 0.0, scale * 0.5 * float(CELLS_BY_TYPE[kind]) + 1.1)
	for index in 3:
		var log_piece: Node3D = FENCE.instantiate()
		log_piece.scale = Vector3(1.6, 1.2, 1.6)
		log_piece.position = offset + Vector3(0.0, 0.14 * float(index), 0.12 * float(index))
		FoliageLib.tint_tree(log_piece)
		root.add_child(log_piece)

static func _add_dressing(root: Node3D, kind: String) -> void:
	if kind != "Workshop":
		return
	var scale := float(SCALE_BY_TYPE[kind])
	var cart: Node3D = CART.instantiate()
	cart.scale = Vector3.ONE * 2.0
	cart.position = Vector3(scale * 0.5 + 1.6, 0.0, scale * 0.7)
	cart.rotation_degrees.y = 20.0
	root.add_child(cart)
	var block: Node3D = STUMP.instantiate()
	block.scale = Vector3.ONE * 2.8
	block.position = Vector3(scale * 0.5 + 1.4, 0.0, -scale * 0.6)
	FoliageLib.tint_tree(block)
	root.add_child(block)
