class_name BuildingAssets
extends RefCounted

## Ticket #19 (real assets) + follow-up fixes from live user testing.
## Sources and licenses are recorded in apps/godot-client/assets/CREDITS.md
## -- all CC0, no attribution required, but recorded anyway.
##
## Every building is a real enclosed four-wall box + roof, not a facade:
## four copies of the same wall piece (one with a door), all left at their
## default local position but rotated 0/90/180/270 around Y, form a
## correct box. This is the standard Kenney grid-snap convention -- each
## piece's origin is off-center on purpose (confirmed by inspecting its
## AABB: wall-wood.glb spans local x:[0.4, 0.5], not centered on 0), so
## rotating four identical copies around that shared, off-center origin
## walks the panel around all four sides automatically. Verified visually
## via scripts/tests/debug_building.gd before wiring this in.
##
## Workshop reuses the same box with a watermill wheel mounted on one
## side, rather than the standalone "windmill.glb"/"watermill.glb" models
## alone: both turned out to be mechanical accent pieces (just blades, or
## just a wheel) with no supporting structure of their own -- neither
## reads as a building by itself. This was caught by rendering them in
## isolation, not assumed from their filenames.

const WALL := preload("res://assets/buildings/wall-wood.glb")
const WALL_DOOR := preload("res://assets/buildings/wall-wood-door.glb")
const ROOF_GABLE := preload("res://assets/buildings/roof-gable.glb")
const WATERMILL_WHEEL := preload("res://assets/buildings/watermill.glb")
const STALL := preload("res://assets/buildings/stall-red.glb")
const FOUNTAIN := preload("res://assets/buildings/fountain-round.glb")

const CHARACTER_FEMALE := preload("res://assets/characters/character-a.glb")
const CHARACTER_MALE := preload("res://assets/characters/character-b.glb")

const WALL_HEIGHT := 1.0
const PERSON_SOURCE_HEIGHT := 2.7
const PERSON_TARGET_HEIGHT := 1.8

## watermill.glb's own pivot sits at its vertical *center*, not its base
## (AABB y:[-0.899547, +0.899547], confirmed by inspecting it directly) --
## unlike the wall/roof pieces, which are base-pivoted. Corrected the same
## way the standalone windmill blades needed correcting before this
## became a wheel-on-a-wall instead.
const WATERMILL_HALF_HEIGHT := 0.899547

## Scale multipliers so buildings actually read as bigger than a person.
## The raw Kenney modules are all authored at the same ~1-unit grid scale
## regardless of what they represent, so a "1.8-unit-tall" person ended up
## taller than the dwelling assembly until these were added -- a real
## scale bug, not a stylistic choice.
const DWELLING_SCALE := 2.5
const WORKSHOP_SCALE := 2.8
const FOODSTORE_SCALE := 1.3

static func make_building(building_type: String) -> Node3D:
	match building_type:
		"Workshop":
			return _make_workshop()
		"FoodStore":
			var stall: Node3D = STALL.instantiate()
			stall.scale = Vector3.ONE * FOODSTORE_SCALE
			return stall
		_:
			return _make_walled_box(DWELLING_SCALE)

static func _make_workshop() -> Node3D:
	var root := _make_walled_box(WORKSHOP_SCALE)

	var wheel: Node3D = WATERMILL_WHEEL.instantiate()
	wheel.rotation_degrees.y = 90.0
	wheel.position = Vector3(0.55, WATERMILL_HALF_HEIGHT, 0.0)
	root.add_child(wheel)

	return root

## Four walls (one with a door) rotated around a shared origin + a roof
## cap, scaled as a whole. Shared by Dwelling and Workshop so both are
## real enclosed structures rather than one facade and one mismatched prop.
static func _make_walled_box(scale_factor: float) -> Node3D:
	var root := Node3D.new()

	root.add_child(WALL_DOOR.instantiate())
	for i in range(1, 4):
		var wall: Node3D = WALL.instantiate()
		wall.rotation_degrees.y = 90.0 * i
		root.add_child(wall)

	var roof: Node3D = ROOF_GABLE.instantiate()
	roof.position = Vector3(0.0, WALL_HEIGHT, 0.0)
	root.add_child(roof)

	root.scale = Vector3.ONE * scale_factor
	return root

static func make_settlement_marker() -> Node3D:
	return FOUNTAIN.instantiate()

static func make_person(sex: String) -> Node3D:
	var scene: PackedScene = CHARACTER_FEMALE if sex == "Female" else CHARACTER_MALE
	var instance: Node3D = scene.instantiate()
	instance.scale = Vector3.ONE * (PERSON_TARGET_HEIGHT / PERSON_SOURCE_HEIGHT)
	return instance
