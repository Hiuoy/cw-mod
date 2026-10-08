@tool
class_name MkCraftingTable
extends MkEntity
## The Crafting Table: a menu that sells equipment, support items and self-revives for salvage (the game's
## prices). Place as many as you like.

## Every item the table can sell that a map may turn off (the game's zmEnable* settings, which the crafting
## menu checks): [name in the .mkmap, label]. Armor plates and the rest of the menu are always sold.
const ITEMS := [
	["frag", "Frag grenade"], ["semtex", "Semtex"], ["molotov", "Molotov"], ["hatchet", "Hatchet"], ["c4", "C4"],
	["decoy", "Decoy"], ["stun", "Stun grenade"], ["monkey", "Cymbal monkey"], ["stimshot", "Stimshot"],
	["self_revive", "Self revive"], ["turret", "Sentry turret"], ["chopper_gunner", "Chopper gunner"],
	["death_machine", "Death machine"], ["flamethrower", "Flamethrower"], ["bow", "Sparrow"],
	["napalm", "Napalm strike"], ["pineapple_gun", "Pineapple gun"], ["hand_cannon", "Hand cannon"],
	["rcxd", "RC-XD"],
]

## What the table sells. The game has one list for the whole map: with several tables, an item sells only if
## every table has it on.
@export_flags("Frag grenade", "Semtex", "Molotov", "Hatchet", "C4", "Decoy", "Stun grenade", "Cymbal monkey",
	"Stimshot", "Self revive", "Sentry turret", "Chopper gunner", "Death machine", "Flamethrower",
	"Sparrow", "Napalm strike", "Pineapple gun", "Hand cannon", "RC-XD") var items: int = (1 << 19) - 1


func mk_class() -> String:
	return "crafting_table"


## The table the game spawns at a crafting_table struct, turned as the struct is. Its front is +X.
func mk_model() -> String:
	return "p9_fxanim_zm_gp_crafting_xmodel"


func mk_bounds() -> AABB:
	return AABB(Vector3(-26, -41, 0), Vector3(50, 83, 74))


func mk_color() -> Color:
	return Color(0.55, 0.75, 0.3)


## "items": the names (ITEMS) of what it sells.
func mk_props() -> Dictionary:
	var sold := []
	for i in ITEMS.size():
		if items & (1 << i):
			sold.append(ITEMS[i][0])
	return {"items": sold}
