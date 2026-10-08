@tool
class_name MkModelCatalog
extends RefCounted
## The game's models as the model picker lists them: each one's name, category, size and zone.
##
## Which models there are comes from your own install: mkasset lists the models of the zones it reads
## (MkAssets.catalog). Their names come from game_model_names.txt next to this script (names and hashes only,
## written by mapkit/mkasset/model_names.py); a model without a name there shows as its hash.

const NAMES_FILE := "res://addons/mapkit/game_model_names.txt"
const ALL := "All"
const UNNAMED := "Unnamed"
const OTHER := "Other"
## The categories, in the picker's order: a model is in the first one whose pattern matches its name.
const CATEGORIES := [
	["Perk machines", "^p9_sur_(machine_(juggernog|speed_cola|quick_revive|staminup|tombstone|mule_kick|perk)|vending_ads|elemental_pop)"],
	["Mystery Box", "magic_box"],
	["Pack-a-Punch & Arsenal", "packapunch|_pap_|machine_weapon"],
	["Wall buys", "^p9_zm_chalk_buy_.*_t9$"],
	["Wall buy chalk (rarities)", "chalk_buy"],
	["Characters", "^c_"],
	["Weapons & equipment", "^wpn_|^attach_"],
	["Vehicles", "^veh_"],
	["Doors & gates", "(^|_)(doors?|gates?)(_|$)"],
	["Barricades", "barricade|barrier_window|energy_barrier"],
	["Intel", "intel|audiolog|documents"],
	["Debris", "debris|rubble|blocker|trash|garbage|chunk|dest"],
	["Foliage & rocks", "foliage|tree|grass|bush|shrub|rock|boulder|snow|leaves|log_"],
	["Lights", "light|lamp|lantern|bulb"],
	["Crates & containers", "crate|box|container|barrel|locker|cabinet|shelf|stash"],
	["Pipes & wires", "pipe|conduit|wire|cable|rebar|hose"],
	["Machines & screens", "machine|computer|console|screen|terminal|generator|monitor|radio|switch|panel"],
	["Furniture", "table|chair|desk|bed|bench|sofa|stool"],
	["Walls & floors", "wall|pillar|stair|railing|floor|beam|concrete|brick|roof|window|frame|column|plank|board|sheet|ceiling"],
	["Aether & effects", "aether|crystal|^fx|_fx|essence|energy"],
]

static var _names := {}          # key -> name
static var _patterns: Array = [] # [category, RegEx]


## The game's name for a model key ("" when not known).
static func name_of(key: String) -> String:
	_load_names()
	return _names.get(key, "")


## Every category, in order, with All first and Unnamed last.
static func categories() -> PackedStringArray:
	var out := PackedStringArray([ALL])
	for rule in CATEGORIES:
		out.append(rule[0])
	out.append_array([OTHER, UNNAMED])
	return out


static func category_of(model_name: String) -> String:
	if model_name.is_empty():
		return UNNAMED
	if _patterns.is_empty():
		for rule in CATEGORIES:
			_patterns.append([rule[0], RegEx.create_from_string(rule[1])])
	for rule in _patterns:
		if rule[1].search(model_name) != null:
			return rule[0]
	return OTHER


## The picker's entries from mkasset's catalog (MkAssets.catalog), sorted by name, the unnamed ones last:
## {"key", "name", "category", "zone", "size" (game units: length, width, height), "triangles"}.
static func entries(catalog: Dictionary) -> Array[Dictionary]:
	_load_names()
	var out: Array[Dictionary] = []
	for model in catalog.get("models", []):
		var key: String = model.get("model", "")
		var name: String = _names.get(key, "")
		var mins: Array = model.get("mins", [0, 0, 0])
		var maxs: Array = model.get("maxs", [0, 0, 0])
		out.append({"key": key, "name": name, "category": category_of(name), "zone": model.get("zone", ""),
			"size": Vector3(maxs[0] - mins[0], maxs[1] - mins[1], maxs[2] - mins[2]),
			"triangles": int(model.get("triangles", 0))})
	out.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		if a.name.is_empty() != b.name.is_empty():
			return b.name.is_empty()
		return a.name < b.name if not a.name.is_empty() else a.key < b.key)
	return out


## How a model is written in a model field: its name, else #<hash>.
static func field_value(entry: Dictionary) -> String:
	return entry.name if not entry.name.is_empty() else "#" + entry.key


static func _load_names() -> void:
	if not _names.is_empty():
		return
	var file := FileAccess.open(NAMES_FILE, FileAccess.READ)
	if file == null:
		push_warning("mapkit: %s is missing: the models show as hashes" % NAMES_FILE)
		_names[""] = ""
		return
	while not file.eof_reached():
		var line := file.get_line()
		if line.length() > 17 and not line.begins_with("#"):
			_names[line.substr(0, 16)] = line.substr(17)
