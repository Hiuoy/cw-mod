@tool
class_name MkWallBuy
extends MkEntity
## A weapon bought off a wall. Put the origin at the foot of the wall and face it out of the wall.

## The chalk drawing of each weapon (p9_zm_chalk_buy_<weapon>).
const CHALK := "p9_zm_chalk_buy_"
## A wall buy's chalk hangs this high above the node, the foot of its wall (cwlink kWallBuyHeight).
const HEIGHT := 40.0

## The weapon, by the game's name for it. Pick shows each weapon's chalk.
@export_custom(PROPERTY_HINT_NONE, "mk_model:Wall buys:p9_zm_chalk_buy_") var weapon: String = "":
	set(value):
		weapon = value
		refresh()
@export_range(0, 100000, 50) var cost: int = 500


func mk_class() -> String:
	return "wall_buy"


func mk_bounds() -> AABB:
	return AABB(Vector3(-2, -24, 32), Vector3(4, 48, 20))


func mk_color() -> Color:
	return Color(0.95, 0.85, 0.2)


func mk_model() -> String:
	return CHALK + weapon if not weapon.is_empty() else ""


## The chalk is drawn on its -Y side, and cwlink turns its struct 90 degrees left of the node's facing, so the
## drawing faces out of the wall.
func mk_model_yaw() -> float:
	return 90.0


func mk_model_offset() -> Vector3:
	return Vector3(0, 0, HEIGHT)


func mk_props() -> Dictionary:
	return {"weapon": weapon, "cost": cost}
