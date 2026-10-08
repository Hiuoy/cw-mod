@tool
class_name MkPerkMachine
extends MkEntity
## A perk machine. Face it toward where players stand to buy.

## The machine models: the six Die Maschine places (its zm_perk_machine structs' "model") and two more zm_common
## holds. PhD Flopper's and Death Perception's are in other maps' zones: they show as the box.
const MODELS := {
	"juggernog": "p9_sur_machine_juggernog", "speed_cola": "p9_sur_machine_speed_cola",
	"quick_revive": "p9_sur_machine_quick_revive", "stamin_up": "p9_sur_machine_staminup",
	"deadshot_dealer": "p9_sur_vending_ads", "elemental_pop": "p9_sur_elemental_pop",
	"tombstone_soda": "p9_sur_machine_tombstone", "mule_kick": "p9_sur_machine_mule_kick",
}

@export_enum("juggernog", "speed_cola", "quick_revive", "stamin_up", "deadshot_dealer", "elemental_pop",
		"tombstone_soda", "mule_kick", "phd_flopper", "death_perception") var perk: String = "phd_flopper":
	set(value):
		perk = value
		refresh()


func mk_class() -> String:
	return "perk_machine"


func mk_model() -> String:
	return MODELS.get(perk, "")


## A machine's front faces its struct's right (zm_perks.gsc puts the buy trigger on anglestoright), so cwlink
## turns the struct 90 degrees left of the node's facing.
func mk_model_yaw() -> float:
	return 90.0


func mk_bounds() -> AABB:
	return AABB(Vector3(-20, -20, 0), Vector3(40, 40, 90))


func mk_color() -> Color:
	return Color(0.7, 0.3, 0.9)


func mk_props() -> Dictionary:
	return {"perk": perk}
