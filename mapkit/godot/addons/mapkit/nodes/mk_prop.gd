@tool
class_name MkProp
extends MkEntity
## A game model placed in the map, by the game's name for it or its hash. The editor draws the model once
## mkasset has read it (MkAssets); the box stands in until then, or when the model is not in the zones read.
## Turn it any way, and scale it evenly (the game has one scale per model, so an uneven scale is averaged).

## The model, by the game's name for it or its hash (#1234...). Pick shows them all, with pictures.
@export_custom(PROPERTY_HINT_NONE, "mk_model") var model: String = "":
	set(value):
		model = value
		refresh()
## Players and zombies collide with it: a hull wrapped around the model, so a concave model (a table, an arch) is
## filled in.
@export var solid: bool = true


func mk_class() -> String:
	return "prop"


func mk_bounds() -> AABB:
	return AABB(Vector3(-16, -16, 0), Vector3(32, 32, 32))


func mk_color() -> Color:
	return Color(0.6, 0.6, 0.6)


func mk_model() -> String:
	return model


func mk_props() -> Dictionary:
	return {"model": model, "solid": solid}


## Its scale goes to the game (MkExport writes it as the prop's "scale").
func mk_scalable() -> bool:
	return true
