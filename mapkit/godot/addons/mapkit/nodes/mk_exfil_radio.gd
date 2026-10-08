@tool
class_name MkExfilRadio
extends MkEntity
## The radio players use to call the exfil (MkExfil). It goes live for 2 minutes at the end of round 10 and
## every 5th round after. One per map.


func mk_class() -> String:
	return "exfil_radio"


## The radio the game spawns at the exfil_radio struct, turned as the struct is.
func mk_model() -> String:
	return "p9_zm_radio_pack_01b_surface"


func mk_bounds() -> AABB:
	return AABB(Vector3(-9, -13, 0), Vector3(17, 25, 9))


func mk_color() -> Color:
	return Color(0.2, 0.9, 0.4)
