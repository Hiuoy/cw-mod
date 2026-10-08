@tool
class_name MkPlayerSpawn
extends MkEntity
## Where a player starts the match. Place four, facing into the start zone.

## The player who starts here (1 to 4). 0: any player.
@export_range(0, 4) var player: int = 0


func mk_class() -> String:
	return "player_spawn"


func mk_bounds() -> AABB:
	return AABB(Vector3(-16, -16, 0), Vector3(32, 32, 72))


func mk_color() -> Color:
	return Color(0.25, 0.85, 0.35)


func mk_props() -> Dictionary:
	return {"player": player}
