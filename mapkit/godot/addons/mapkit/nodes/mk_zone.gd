@tool
class_name MkZone
extends MkVolume
## An area of the map. Spawners and barriers belong to a zone, and doors open zones. Zombies spawn
## in the zones the players are in.

## The name spawners, barriers and doors use to refer to this zone.
@export var zone_name: String = "start_zone"
## Open from the start of the match: the area the players spawn in.
@export var active_at_start: bool = false


func mk_class() -> String:
	return "zone"


func mk_color() -> Color:
	return Color(0.2, 0.7, 0.95)


func mk_props() -> Dictionary:
	return {"zone_name": zone_name, "active_at_start": active_at_start}
