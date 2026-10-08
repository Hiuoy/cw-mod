@tool
class_name MkArsenal
extends MkEntity
## The Arsenal: buys armor and upgrades a weapon's rarity. It is the machine MkArmorStation places (the game
## calls it the Arsenal), so the two build the same thing. Place as many as you like.

## With a power switch (MkPowerSwitch) in the map: it works only once the power is on, as in Die Maschine. Without
## one it always works from the start.
@export var needs_power: bool = true


func mk_class() -> String:
	return "arsenal"


## The machine the game spawns at an armor_machine struct, turned as the struct is. Its front is +X.
func mk_model() -> String:
	return "p9_fxanim_zm_gp_armor_station_xmodel"


func mk_bounds() -> AABB:
	return AABB(Vector3(-32, -40, 0), Vector3(64, 74, 101))


func mk_color() -> Color:
	return Color(0.4, 0.9, 0.9)


func mk_props() -> Dictionary:
	return {"needs_power": needs_power}
