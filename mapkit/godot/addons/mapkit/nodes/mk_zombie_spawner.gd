@tool
class_name MkZombieSpawner
extends MkEntity
## Where zombies enter the map. A spawner is used while players are in its zone.

## The zone it belongs to (a Zone's zone_name).
@export var zone: String = "start_zone"
## ground: the zombie climbs out of the ground here. barrier: it heads for the nearest window
## barrier of its zone and tears through.
@export_enum("ground", "barrier") var kind: String = "ground"


func mk_class() -> String:
	return "zombie_spawner"


func mk_bounds() -> AABB:
	return AABB(Vector3(-16, -16, 0), Vector3(32, 32, 72))


func mk_color() -> Color:
	return Color(0.9, 0.25, 0.2)


func mk_props() -> Dictionary:
	return {"zone": zone, "kind": kind}
