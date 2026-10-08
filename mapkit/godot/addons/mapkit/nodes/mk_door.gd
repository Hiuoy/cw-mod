@tool
class_name MkDoor
extends MkVolume
## A door or debris that blocks the way until a player buys it. The model blocks, with its own collision, and
## sinks into the floor when bought; the box is where players buy it (the prompt reaches a little past it).

@export_enum("door", "debris") var kind: String = "door"
@export_range(0, 100000, 50) var cost: int = 750
## The zones it opens, separated by commas.
@export var opens: String = ""
## The door itself: a game model with its own collision, placed at the node. Empty = Die Maschine's rusted metal
## door (p9_zm_ndu_door_metal_gray_rusted). Pick one that fills the opening: the gaps beside it stay open.
@export_custom(PROPERTY_HINT_NONE, "mk_model:Doors & gates") var model: String = "":
	set(value):
		model = value
		refresh()
## Opens by itself when a player turns the power on (MkPowerSwitch), instead of being bought. Without a power switch
## it is bought as usual.
@export var needs_power: bool = false


func _init() -> void:
	size = Vector3(2.5, 3, 0.5)


func mk_class() -> String:
	return "door"


func mk_color() -> Color:
	return Color(1.0, 0.55, 0.1)


func mk_props() -> Dictionary:
	return {"kind": kind, "cost": cost, "opens": _zone_list(), "model": model, "needs_power": needs_power}


func mk_model() -> String:
	return model


func _zone_list() -> Array:
	var zones := []
	for zone in opens.split(",", false):
		if not zone.strip_edges().is_empty():
			zones.append(zone.strip_edges())
	return zones


func _shown_in_walk() -> bool:
	return true
