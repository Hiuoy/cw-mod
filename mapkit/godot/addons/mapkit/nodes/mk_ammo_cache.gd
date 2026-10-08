@tool
class_name MkAmmoCache
extends MkEntity
## An ammo crate: refills the ammo of the weapon in hand, for points (the game's price, by weapon tier).
## Place as many as you like.


func mk_class() -> String:
	return "ammo_cache"


## The crate the game spawns at an ammo_cache_spawn struct, turned as the struct is. Its use side is +X.
func mk_model() -> String:
	return "p9_usa_large_ammo_crate_01_sur"


func mk_bounds() -> AABB:
	return AABB(Vector3(-19, -33, 0), Vector3(38, 66, 34))


func mk_color() -> Color:
	return Color(0.75, 0.6, 0.3)
