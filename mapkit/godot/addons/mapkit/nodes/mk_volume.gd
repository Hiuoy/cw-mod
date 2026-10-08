@tool
class_name MkVolume
extends MkEntity
## An entity that covers an area. The origin is the middle of the floor of the box. Resize it with
## the handles (hold Shift to turn off snapping) or with size.

## Width, height and depth in meters (Godot's X, Y, Z before rotation).
@export var size := Vector3(6, 3, 6):
	set(value):
		size = Vector3(maxf(value.x, 0.1), maxf(value.y, 0.1), maxf(value.z, 0.1))
		refresh()


func mk_is_volume() -> bool:
	return true


func mk_bounds() -> AABB:
	return MkSpace.aabb_to_game(AABB(Vector3(-size.x / 2, 0, -size.z / 2), size))
