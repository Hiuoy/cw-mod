@tool
class_name MkMysteryBox
extends MkEntity
## One place the Mystery Box can be. Place several; the box moves between them.

## The box is here when the match starts.
@export var start_here: bool = false


func mk_class() -> String:
	return "mystery_box"


## The chest Die Maschine places at each box location (its <area>_chest structs' "model").
func mk_model() -> String:
	return "p7_zm_der_magic_box"


## The box is long along its struct's X with its front on +Y, so cwlink turns the struct 90 degrees right of the
## node's facing.
func mk_model_yaw() -> float:
	return -90.0


func mk_bounds() -> AABB:
	return AABB(Vector3(-14, -30, 0), Vector3(28, 60, 32))


func mk_color() -> Color:
	return Color(0.3, 0.6, 1.0)


func mk_props() -> Dictionary:
	return {"start_here": start_here}
