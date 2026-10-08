@tool
class_name MkPackAPunch
extends MkEntity
## The Pack-a-Punch machine: Pack-a-Punch tiers 1 to 3 (5000, 15000, 30000 points) and ammo mods. Place as
## many as you like; it works from the first round (no quest, no power).


func mk_class() -> String:
	return "pack_a_punch"


## The machine the game spawns at a weapon_machine struct, turned as the struct is (content_manager.gsc
## spawn_script_model). Its front is +X.
func mk_model() -> String:
	return "p9_fxanim_zm_gp_pap_xmodel"


func mk_bounds() -> AABB:
	return AABB(Vector3(-20, -30, 0), Vector3(40, 60, 72))


func mk_color() -> Color:
	return Color(0.95, 0.3, 0.7)
