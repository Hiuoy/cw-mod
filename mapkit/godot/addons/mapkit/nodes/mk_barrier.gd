@tool
class_name MkBarrier
extends MkEntity
## A boarded window zombies tear through and players rebuild (Die Maschine's barrier, moved here). Put the node on
## the floor in the middle of the opening, its arrow pointing into the play area. Zombies from the zone's barrier
## spawners (MkZombieSpawner kind barrier, outside) walk to its outer side, tear the boards down and climb in; the
## climb goes through whatever the wall is. Players cannot pass: a build puts a player clip over the boards, the box
## shown here, which bullets and zombies go through. Keep the opening inside the box. The concrete one asks for power
## and stays (not working yet).

## The zone it belongs to (a Zone's zone_name): that zone's barrier spawners send their zombies to the nearest one.
@export var zone: String = "start_zone"
## wood: Die Maschine's boarded window. concrete: its wall of concrete chunks.
@export_enum("wood", "concrete") var kind: String = "wood":
	set(value):
		kind = value
		refresh()


func mk_class() -> String:
	return "barrier"


## The player clip a build puts there (cwlink BarrierClips): the bounds of the barrier's own collision model, which
## stands just outside the node. The concrete one is a hole in a wall, 34 units up.
func mk_bounds() -> AABB:
	if kind == "concrete":
		return AABB(Vector3(2, -37, 34), Vector3(8, 75, 66))
	return AABB(Vector3(-11, -53, -3), Vector3(8, 106, 117))


func mk_color() -> Color:
	return Color(0.6, 0.4, 0.2)


func mk_props() -> Dictionary:
	return {"zone": zone, "kind": kind}
