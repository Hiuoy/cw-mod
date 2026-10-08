@tool
class_name MkPowerSwitch
extends MkEntity
## The power switch (Die Maschine's console). Without one, the power is on from the start. With one, the perk
## machines (all but Quick Revive when playing alone), the Armor Stations and the doors with needs_power wait for a
## player to use it. Players use it from its front (the arrow).


func mk_class() -> String:
	return "power_switch"


## Die Maschine's power switch (its elec_switch model), a console on the floor. Its front is +X.
func mk_model() -> String:
	return "p9_zm_ndu_power_on_switch"


func mk_bounds() -> AABB:
	return AABB(Vector3(-4, -8, 36), Vector3(8, 16, 24))


func mk_color() -> Color:
	return Color(0.9, 0.9, 0.9)
