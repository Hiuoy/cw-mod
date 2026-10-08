@tool
class_name MkExfil
extends MkEntity
## Where the exfil helicopter lands. The node's arrow is the way the landed helicopter faces.
## Exfil works as in the game's own maps: from round 10, every 5th round, a player can call it at the radio
## (MkExfilRadio) for 2 minutes. The helicopter flies in, circles the landing point while zombies attack, and
## players must reach the exfil zones and hold out until the timer runs down; it then lands and takes them.
## One per map. Give the helicopter open sky: it comes in from about 5,800 units away and 2,300 up, circles
## 700 to 900 units up in a loop about 1,800 across, and lands here (Die Maschine's flight, moved and turned).

## Seconds to hold out once the helicopter arrives.
@export_range(30, 600, 5) var hold_seconds: int = 90
## Zombies attack from up to this far around the landing point.
@export_range(200, 3000, 50) var attack_radius: int = 600
## ...and this far above or below it.
@export_range(50, 1000, 25) var attack_height: int = 200
## The zones players must get to, separated by commas. Empty: the zones the landing point is in.
@export var zones: String = ""
## For testing: the radio can be used from the start of the match, not only after round 10.
@export var radio_live_at_start: bool = false


func mk_class() -> String:
	return "exfil"


## The landing area: roughly the helicopter's size, flat on the floor.
func mk_bounds() -> AABB:
	return AABB(Vector3(-300, -200, 0), Vector3(600, 400, 8))


func mk_color() -> Color:
	return Color(0.2, 0.9, 0.4)


func mk_props() -> Dictionary:
	var zone_list := []
	for zone in zones.split(",", false):
		if not zone.strip_edges().is_empty():
			zone_list.append(zone.strip_edges())
	return {"hold_seconds": hold_seconds, "attack_radius": attack_radius, "attack_height": attack_height,
		"zones": zone_list, "radio_live_at_start": radio_live_at_start}
