@tool
extends VBoxContainer
## The mapkit dock: start a map from the template, check it, export the .mkmap source, and build it into
## the game's map files with cwlink.

const TEMPLATE := "res://maps/zm_template.tscn"
const SETTING_GAME_FOLDER := "mapkit/game_folder"
const SETTING_CWLINK := "mapkit/cwlink"
## cwlink's place in a repo build, relative to this project.
const CWLINK_IN_REPO := "res://../../build/t9_vs2022/x64/cwlink/cwlink.exe"
## The retail map a build is laid over: its override zones replace its world and move its spawns and
## perks (cwlink build). Die Maschine is the one tested.
const BASE_MAP := "zm_silver"

var _map_label: Label
var _new_name: LineEdit
var _problems: ItemList
var _problem_paths: Array[NodePath] = []
var _status: Label
var _open_folder: Button
var _game_folder: LineEdit
var _folder_dialog: EditorFileDialog
var _export_dir := ""


func _init() -> void:
	name = "mapkit"

	_map_label = Label.new()
	_map_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_map_label)

	var new_row := HBoxContainer.new()
	_new_name = LineEdit.new()
	_new_name.placeholder_text = "zm_mymap"
	_new_name.size_flags_horizontal = SIZE_EXPAND_FILL
	new_row.add_child(_new_name)
	new_row.add_child(_button("New map", "A new map scene in res://maps/, copied from the template.", _new_map))
	add_child(new_row)

	var actions := HBoxContainer.new()
	actions.add_child(_button("Check", "Look for problems in the open map. Click a problem to select its node.", _check))
	actions.add_child(_button("Export", "Check, then write the open map to res://build/<map_name>.mkmap.", _export))
	actions.add_child(_button("Build", "Export, then build the game's map files with cwlink: laid over %s (Die Maschine), written to <game folder>/cw-mod/maps/%s/. Start Die Maschine to play it." % [BASE_MAP, BASE_MAP], _build))
	add_child(actions)

	_problems = ItemList.new()
	_problems.custom_minimum_size = Vector2(0, 160)
	_problems.size_flags_vertical = SIZE_EXPAND_FILL
	_problems.item_selected.connect(_select_problem)
	add_child(_problems)

	var status_row := HBoxContainer.new()
	_status = Label.new()
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status.size_flags_horizontal = SIZE_EXPAND_FILL
	status_row.add_child(_status)
	_open_folder = _button("Open folder", "Show the exported file.", func(): OS.shell_open(_export_dir))
	_open_folder.visible = false
	status_row.add_child(_open_folder)
	add_child(status_row)

	var folder_label := Label.new()
	folder_label.text = "Game folder (checks your map name against the shipped maps; Build writes there):"
	folder_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(folder_label)
	var folder_row := HBoxContainer.new()
	_game_folder = LineEdit.new()
	_game_folder.size_flags_horizontal = SIZE_EXPAND_FILL
	_game_folder.text_changed.connect(_save_game_folder)
	folder_row.add_child(_game_folder)
	folder_row.add_child(_button("...", "Pick the game folder.", func(): _folder_dialog.popup_file_dialog()))
	add_child(folder_row)

	_folder_dialog = EditorFileDialog.new()
	_folder_dialog.file_mode = EditorFileDialog.FILE_MODE_OPEN_DIR
	_folder_dialog.access = EditorFileDialog.ACCESS_FILESYSTEM
	_folder_dialog.dir_selected.connect(func(dir: String):
		_game_folder.text = dir
		_save_game_folder(dir))
	add_child(_folder_dialog)


func _ready() -> void:
	var settings := EditorInterface.get_editor_settings()
	if settings.has_setting(SETTING_GAME_FOLDER):
		_game_folder.text = settings.get_setting(SETTING_GAME_FOLDER)
	else:
		_game_folder.text = OS.get_environment("MAPKIT_GAME_DIR")
	refresh()


## Called when the edited scene changes.
func refresh(_root: Node = null) -> void:
	var map := _current_map()
	if map == null:
		_map_label.text = "Open a map scene (its root is an MkMap), or make one: type a name and press New map."
	else:
		_map_label.text = "Map: %s  \"%s\"" % [map.map_name, map.title]
	_problems.clear()
	_problem_paths.clear()


func _button(text: String, tooltip: String, pressed: Callable) -> Button:
	var button := Button.new()
	button.text = text
	button.tooltip_text = tooltip
	if pressed.is_valid():
		button.pressed.connect(pressed)
	return button


func _say(text: String) -> void:
	_status.text = text
	_open_folder.visible = false


func _save_game_folder(dir: String) -> void:
	EditorInterface.get_editor_settings().set_setting(SETTING_GAME_FOLDER, dir.strip_edges())


func _current_map() -> MkMap:
	var root := EditorInterface.get_edited_scene_root()
	if root == null:
		return null
	if root is MkMap:
		return root as MkMap
	for node in root.find_children("*", "", true, true):
		if node is MkMap:
			return node as MkMap
	return null


func _new_map() -> void:
	var map_name := _new_name.text.strip_edges()
	if not MkCheck.valid_map_name(map_name):
		_say("'%s': use zm_ then lowercase letters, digits and '_'." % map_name)
		return
	var path := "res://maps/%s.tscn" % map_name
	if FileAccess.file_exists(path):
		_say("%s already exists." % path)
		return
	var text := FileAccess.get_file_as_string(TEMPLATE)
	if text.is_empty():
		_say("The template %s is missing." % TEMPLATE)
		return
	# The copy is a new scene: drop the template's uid, then rename.
	text = RegEx.create_from_string(" uid=\"uid://[^\"]*\"").sub(text, "", false)
	text = text.replace("zm_template", map_name).replace("title = \"Template\"", "title = \"%s\"" % map_name)
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		_say("Can't write %s (%s)." % [path, error_string(FileAccess.get_open_error())])
		return
	file.store_string(text)
	file.close()
	EditorInterface.get_resource_filesystem().scan()
	EditorInterface.open_scene_from_path(path)
	_say("Made %s from the template." % path)


func _check() -> Array[Dictionary]:
	var map := _current_map()
	refresh()
	if map == null:
		_say("No map open.")
		return []
	var problems := MkCheck.run(map, _game_folder.text.strip_edges())
	var root := EditorInterface.get_edited_scene_root()
	var theme := EditorInterface.get_editor_theme()
	for problem in problems:
		var icon := theme.get_icon("StatusError" if problem["error"] else "StatusWarning", "EditorIcons")
		_problems.add_item(problem["text"], icon)
		var node: Node = problem["node"]
		_problem_paths.append(root.get_path_to(node) if node != null else NodePath())
	var errors := MkCheck.error_count(problems)
	if problems.is_empty():
		_say("No problems.")
	else:
		_say("%d error(s), %d warning(s)." % [errors, problems.size() - errors])
	return problems


func _select_problem(index: int) -> void:
	var root := EditorInterface.get_edited_scene_root()
	if root == null or index >= _problem_paths.size() or _problem_paths[index].is_empty():
		return
	var node := root.get_node_or_null(_problem_paths[index])
	if node == null:
		return
	var selection := EditorInterface.get_selection()
	selection.clear()
	selection.add_node(node)
	EditorInterface.edit_node(node)


func _export() -> void:
	var path := _export_map()
	if path.is_empty():
		return
	_say("Exported %s" % path)
	_export_dir = path.get_base_dir()
	_open_folder.visible = true


## Checks and exports the open map. Returns the .mkmap path, or "" after saying why not.
func _export_map() -> String:
	var map := _current_map()
	if map == null:
		_say("No map open.")
		return ""
	if MkCheck.error_count(_check()) > 0:
		_say("Fix the errors first (red), then export.")
		return ""
	var path := ProjectSettings.globalize_path("res://build/%s.mkmap" % map.map_name)
	var failure := MkExport.write(map, path)
	if not failure.is_empty():
		_say(failure)
		return ""
	return path


func _cwlink_path() -> String:
	var settings := EditorInterface.get_editor_settings()
	if settings.has_setting(SETTING_CWLINK) and FileAccess.file_exists(settings.get_setting(SETTING_CWLINK)):
		return settings.get_setting(SETTING_CWLINK)
	var in_repo := ProjectSettings.globalize_path(CWLINK_IN_REPO).simplify_path()
	return in_repo if FileAccess.file_exists(in_repo) else ""


## Export, then cwlink build: the override zones for BASE_MAP, straight into the game's cw-mod/maps.
func _build() -> void:
	var game := _game_folder.text.strip_edges()
	if game.is_empty() or not DirAccess.dir_exists_absolute(game.path_join("zone")):
		_say("Set the game folder first (the one that holds the zone folder).")
		return
	var cwlink := _cwlink_path()
	if cwlink.is_empty():
		_say("cwlink.exe not found. Build it (the mapkit\\cwlink project), or set %s in Editor Settings." % SETTING_CWLINK)
		return
	var trace := game.path_join("cw-mod/mapkit/trace/%s.mktrace" % BASE_MAP)
	if not FileAccess.file_exists(trace):
		_say("No zone trace of %s yet: add \"mapkit_trace\": [\"%s\"] to cw-mod.json and start Die Maschine once." % [BASE_MAP, BASE_MAP])
		return
	var source := _export_map()
	if source.is_empty():
		return
	_say("Building with cwlink (the editor waits a few seconds)...")
	# Let the status draw before the blocking run.
	await get_tree().process_frame
	await get_tree().process_frame
	var output := []
	var code := OS.execute(cwlink, ["--game", game, "build", BASE_MAP, source, "--trace", trace], output, true)
	var text: String = "\n".join(output)
	print(text)
	_problems.clear()
	_problem_paths.clear()
	for line in text.split("\n", false):
		_problems.add_item(line.strip_edges())
		_problem_paths.append(NodePath())
	if code != 0:
		_say("Build FAILED (cwlink exit %d): the last lines above say why. (A running game locks the map files.)" % code)
		return
	_say("Built. Start Die Maschine in cw-mod to play it (cwlink's report is above and in the Output panel).")
	_export_dir = game.path_join("cw-mod/maps/%s" % BASE_MAP)
	_open_folder.visible = true
