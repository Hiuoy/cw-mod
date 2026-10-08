@tool
class_name MkWunderfizz
extends MkEntity
## Der Wunderfizz: a menu that sells any perk the map has, for points (the game's prices).
## Place as many as you like. It works from the start (no power switch yet).


func mk_class() -> String:
	return "wunderfizz"


## The machine the game spawns at a perk_machine_choice struct, turned as the struct is. Its front is +X.
func mk_model() -> String:
	return "p9_fxanim_zm_gp_wunderfizz_on_xmodel"


func mk_bounds() -> AABB:
	return AABB(Vector3(-1, -28, 0), Vector3(52, 56, 97))


func mk_color() -> Color:
	return Color(0.85, 0.4, 0.85)
