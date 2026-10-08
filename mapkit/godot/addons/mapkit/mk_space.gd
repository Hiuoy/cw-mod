@tool
class_name MkSpace
extends RefCounted
## Godot space and game space.
##
## Godot: Y up, -Z forward, meters. The game: Z up, X forward, Y left, 1 unit = 1 inch. Both are
## right-handed, so converting is an axis swap plus a scale:
##     game (x, y, z) = (-godot.z, -godot.x, godot.y) * 39.37
## A player is 72 units (1.83 m) tall, and the eyes are at 60 units (1.52 m).

const UNITS_PER_METER := 39.37008

static var _materials := {}


static func dir_to_game(v: Vector3) -> Vector3:
	return Vector3(-v.z, -v.x, v.y)


static func dir_to_godot(v: Vector3) -> Vector3:
	return Vector3(-v.y, v.z, -v.x)


## A Godot position or offset (meters) in game units.
static func to_game(v: Vector3) -> Vector3:
	return dir_to_game(v) * UNITS_PER_METER


## A game position or offset (units) in Godot meters.
static func to_godot(v: Vector3) -> Vector3:
	return dir_to_godot(v) / UNITS_PER_METER


## A box in game units and axes, as the same box in Godot meters and axes.
static func aabb_to_godot(box: AABB) -> AABB:
	return _span(to_godot(box.position), to_godot(box.end))


static func aabb_to_game(box: AABB) -> AABB:
	return _span(to_game(box.position), to_game(box.end))


static func _span(a: Vector3, b: Vector3) -> AABB:
	var lo := Vector3(minf(a.x, b.x), minf(a.y, b.y), minf(a.z, b.z))
	var hi := Vector3(maxf(a.x, b.x), maxf(a.y, b.y), maxf(a.z, b.z))
	return AABB(lo, hi - lo)


## The game's angles for a Godot basis: pitch (down is positive), yaw, roll, in degrees. Scale is ignored.
static func to_game_angles(basis: Basis) -> Vector3:
	var b := basis.orthonormalized()
	var fwd := dir_to_game(-b.z)
	var left := dir_to_game(-b.x)
	var up := dir_to_game(b.y)
	var yaw := atan2(fwd.y, fwd.x)
	var pitch := atan2(-fwd.z, Vector2(fwd.x, fwd.y).length())
	var roll := atan2(left.z, up.z)
	return Vector3(rad_to_deg(pitch), rad_to_deg(yaw), rad_to_deg(roll))


## [x, y, z] rounded to step, for the .mkmap source.
static func rounded(v: Vector3, step := 0.001) -> Array:
	return [snappedf(v.x, step), snappedf(v.y, step), snappedf(v.z, step)]


## The 12 edges of a box, as pairs of points.
static func box_lines(box: AABB) -> PackedVector3Array:
	var lines := PackedVector3Array()
	for axis in 3:
		var u := (axis + 1) % 3
		var v := (axis + 2) % 3
		for i in 4:
			var a := box.position
			a[u] = box.end[u] if i & 1 else box.position[u]
			a[v] = box.end[v] if i & 2 else box.position[v]
			var b := a
			b[axis] = box.end[axis]
			lines.push_back(a)
			lines.push_back(b)
	return lines


## The 12 triangles of a box, three points each.
static func box_faces(box: AABB) -> PackedVector3Array:
	var faces := PackedVector3Array()
	for axis in 3:
		var u := (axis + 1) % 3
		var v := (axis + 2) % 3
		for side in 2:
			var quad: Array[Vector3] = []
			for corner in [[0, 0], [1, 0], [1, 1], [0, 1]]:
				var p := box.position
				p[axis] = box.end[axis] if side == 1 else box.position[axis]
				p[u] = box.end[u] if corner[0] == 1 else box.position[u]
				p[v] = box.end[v] if corner[1] == 1 else box.position[v]
				quad.append(p)
			faces.append_array(PackedVector3Array([quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]]))
	return faces


## A box as a TriangleMesh, which is what makes a gizmo clickable.
static func box_triangle_mesh(box: AABB) -> TriangleMesh:
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = box_faces(box)
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh.generate_triangle_mesh()


## One shared material per color: solid for objects, see-through for volumes.
static func preview_material(color: Color, see_through: bool) -> StandardMaterial3D:
	var key := "%s/%s" % [color.to_html(), see_through]
	if _materials.has(key):
		return _materials[key]
	var material := StandardMaterial3D.new()
	if see_through:
		material.albedo_color = Color(color, 0.12)
		material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		material.cull_mode = BaseMaterial3D.CULL_DISABLED
		material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	else:
		material.albedo_color = color
		material.roughness = 0.7
	_materials[key] = material
	return material
