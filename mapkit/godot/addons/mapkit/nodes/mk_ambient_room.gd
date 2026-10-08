@tool
class_name MkAmbientRoom
extends MkVolume
## How an area sounds: its reverb and room tone, while a player stands in it. Outside every room the
## map sounds like Die Maschine's default room. Cover each indoor space with one. A room sounds the same all
## over, except in some of Die Maschine's rooms (zm_nacht_bunker_hallways seems to be one): there its baked
## acoustics change the echo and muffling from point to point, following Die Maschine's walls, not yours.

## The rooms Die Maschine's own areas use (its ambient_package triggers). Its sound bank has more, but only
## these have names to go by; a build counts them all and says when a room is not one of them.
const DIE_MASCHINE_ROOMS := "zm_nacht_bunker,zm_nacht_bunker_entrance,zm_nacht_bunker_hallways,zm_nacht_bunker_medium_room,zm_nacht_bunker_small_room,zm_nacht_interior"

## A room of Die Maschine's sound bank, by name: pick one of its rooms. A name the bank doesn't have plays the
## default room, like the rest of the map.
@export_custom(PROPERTY_HINT_ENUM_SUGGESTION, DIE_MASCHINE_ROOMS) var room: String = ""
## Where rooms overlap, the higher priority wins.
@export var priority: int = 1


func mk_class() -> String:
	return "ambient_room"


func mk_color() -> Color:
	return Color(0.65, 0.4, 0.95)


func mk_props() -> Dictionary:
	return {"room": room, "priority": priority}
