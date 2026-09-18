extends MeshInstance3D

## Throwaway prototype terrain: a procedurally generated ground mesh whose
## height and dirt/grass blend come from FastNoiseLite, standing in for the
## real Terrain3D + UniverseCore-environment-data terrain built in the
## "Procedural terrain from UniverseCore environment data" ticket.

@export var width: int = 48
@export var depth: int = 48
@export var cell_size: float = 1.0
@export var height_scale: float = 4.5
@export var noise_seed: int = 1337

func _ready() -> void:
	generate()

func generate() -> void:
	var noise := FastNoiseLite.new()
	noise.seed = noise_seed
	noise.frequency = 0.045
	noise.fractal_octaves = 4

	var heights: Array = []
	for z in range(depth + 1):
		var row: Array = []
		for x in range(width + 1):
			row.append(noise.get_noise_2d(float(x), float(z)) * height_scale)
		heights.append(row)

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)

	for z in range(depth):
		for x in range(width):
			var x0 := float(x) * cell_size
			var x1 := float(x + 1) * cell_size
			var z0 := float(z) * cell_size
			var z1 := float(z + 1) * cell_size

			var h00: float = heights[z][x]
			var h10: float = heights[z][x + 1]
			var h01: float = heights[z + 1][x]
			var h11: float = heights[z + 1][x + 1]

			var p00 := Vector3(x0, h00, z0)
			var p10 := Vector3(x1, h10, z0)
			var p01 := Vector3(x0, h01, z1)
			var p11 := Vector3(x1, h11, z1)

			_add_triangle(st, p00, p10, p01)
			_add_triangle(st, p10, p11, p01)

	st.generate_normals()
	var generated_mesh := st.commit()

	var material := StandardMaterial3D.new()
	material.vertex_color_use_as_albedo = true
	material.roughness = 1.0
	generated_mesh.surface_set_material(0, material)

	mesh = generated_mesh

func _add_triangle(st: SurfaceTool, a: Vector3, b: Vector3, c: Vector3) -> void:
	st.set_color(_height_color(a.y))
	st.add_vertex(a)
	st.set_color(_height_color(b.y))
	st.add_vertex(b)
	st.set_color(_height_color(c.y))
	st.add_vertex(c)

func _height_color(h: float) -> Color:
	var t: float = clamp((h + height_scale) / (height_scale * 2.0), 0.0, 1.0)
	var dirt := Color(0.40, 0.32, 0.22)
	var grass := Color(0.29, 0.46, 0.21)
	return dirt.lerp(grass, t)
