@tool
class_name MkLight
extends OmniLight3D
## A light. It lights the Godot preview now; the map's lighting build will use it later.
## Color, energy and range are the light's own settings.


func mk_class() -> String:
	return "light"


func mk_bounds() -> AABB:
	return AABB(Vector3(-4, -4, -4), Vector3(8, 8, 8))


func mk_color() -> Color:
	return light_color


func mk_is_volume() -> bool:
	return false


func mk_props() -> Dictionary:
	return {
		"color": [snappedf(light_color.r, 0.001), snappedf(light_color.g, 0.001), snappedf(light_color.b, 0.001)],
		"intensity": snappedf(light_energy, 0.001),
		"radius": snappedf(omni_range * MkSpace.UNITS_PER_METER, 0.1),
	}
