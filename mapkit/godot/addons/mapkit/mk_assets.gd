@tool
class_name MkAssets
extends Node
## The game's own models, drawn in place of the boxes: what a Zombies object looks like in the game.
##
## mkasset (the mapkit\mkasset project, built from the repo) reads the models out of your own install and
## writes each one as a .glb into a local cache, user://game_models/ (Godot's user data folder for this
## project, outside the repo). The editor runs one mkasset in the background ("serve") and asks it for models as
## the map needs them: the first time a model shows takes a moment, after that it loads from the cache, in the
## editor and in the walk-through (F6) alike. The model picker's list and pictures are cached there too.
## Nothing from the game is ever written into this project.
##
## The plugin makes the one MkAssets (plugin.gd). Everything else goes through the static functions.

const SETTING_MKASSET := "mapkit/mkasset"
## mkasset's place in a repo build, relative to this project.
const MKASSET_IN_REPO := "res://../../build/t9_vs2022/x64/mkasset/mkasset.exe"
const SETTING_GAME_FOLDER := "mapkit/game_folder"
const CACHE_DIR := "user://game_models"
## The picker's pictures, one PNG per model, and the list of models mkasset reads.
const THUMBNAIL_DIR := "user://game_models/thumbnails"
const CATALOG_FILE := "user://game_models/catalog.json"
const THUMBNAIL_SIZE := 128
## The zones the models are read from: the base map and its common zone, the models a build can place. Each one
## needs a zone trace (cw-mod.json "mapkit_trace"); one without is skipped.
const ZONES := ["zm_silver", "zm_common"]

static var _server: MkAssets
## "<key>_<lod>" -> PackedScene, shared by every object showing that model.
static var _scenes := {}
## key -> Texture2D: the pictures loaded this session.
static var _thumbnails := {}

var _pid := -1
var _stdio: FileAccess
var _stderr: FileAccess
var _out := ""
var _err := ""
var _ready_seen := false
var _next_id := 1
var _calls := {}     # request id -> Callable(answer: Dictionary)
var _backlog: Array[Dictionary] = []  # requests made before mkasset said ready
var _start_failed := ""
var _waiting := {}   # "<key>_<lod>" -> Array of Callable
var _failed := {}    # "<key>_<lod>" or "thumbnail <key>" -> why (not asked again this session)
var _catalog := {}   # mkasset's catalog, once read this session
var _catalog_waiting: Array[Callable] = []
var _thumbnail_waiting := {}  # key -> Array of Callable
var _to_render: Array = []    # [key, PackedScene], in the order they came
var _rendering := false
var _stage: SubViewport
var _camera: Camera3D


## A model's key: its hash as 16 hex digits, from a hash (#, 0x or bare) or from its name, as mkasset reads it.
static func model_key(model: String) -> String:
	var text := model.strip_edges()
	if text.begins_with("#"):
		text = text.substr(1)
	elif text.to_lower().begins_with("0x"):
		text = text.substr(2)
	if text.length() == 16 and text.is_valid_hex_number():
		# The top bit only marks a reference to another zone's asset.
		return ("%X" % (text.substr(0, 1).hex_to_int() & 7)) + text.substr(1).to_upper()
	if text.is_empty():
		return ""
	# FNV-1a 64 over the lowercased name, top bit cleared (zonekit/hash.hpp HashName).
	var hash := -3750763034362895579 # 0xCBF29CE484222325
	for byte in text.to_lower().to_utf8_buffer():
		hash = (hash ^ byte) * 1099511628211 # 0x100000001B3, wrapping
	return "%016X" % (hash & 0x7FFFFFFFFFFFFFFF)


## Calls on_loaded(scene: PackedScene) with the model's LOD (0 = full detail): at once when it is cached,
## later when mkasset has to write it first, with null when it cannot be had (the Output panel says why).
static func request(model: String, lod: int, on_loaded: Callable) -> void:
	var key := model_key(model)
	if key.is_empty():
		on_loaded.call_deferred(null)
		return
	var id := "%s_%d" % [key, lod]
	if _scenes.has(id):
		on_loaded.call_deferred(_scenes[id])
		return
	var file := CACHE_DIR.path_join(id + ".glb")
	if FileAccess.file_exists(file):
		var scene := _load(file)
		if scene != null:
			_scenes[id] = scene
		on_loaded.call_deferred(scene)
		return
	if _server == null:
		# The walk-through, or the plugin is off: only cached models.
		on_loaded.call_deferred(null)
		return
	_server._ask(key, lod, id, on_loaded)


## Calls on_ready(catalog: Dictionary) with the models mkasset reads ({"zones", "models": [{"model", "zone",
## "mins", "maxs", "triangles"}]}): read fresh from mkasset once per session, else the last one cached, else {}.
static func catalog(on_ready: Callable) -> void:
	if _server == null:
		on_ready.call_deferred(_read_json(CATALOG_FILE))
		return
	_server._ask_catalog(on_ready)


## Calls on_ready(picture: Texture2D) with a picture of the model (null when there is none): from the cache, else
## drawn now, which needs the editor (the plugin) and mkasset.
static func thumbnail(model: String, on_ready: Callable) -> void:
	var key := model_key(model)
	if key.is_empty():
		on_ready.call_deferred(null)
		return
	if _thumbnails.has(key):
		on_ready.call_deferred(_thumbnails[key])
		return
	var file := THUMBNAIL_DIR.path_join(key + ".png")
	if FileAccess.file_exists(file):
		var image := Image.load_from_file(ProjectSettings.globalize_path(file))
		if image != null and not image.is_empty():
			_thumbnails[key] = ImageTexture.create_from_image(image)
			on_ready.call_deferred(_thumbnails[key])
			return
	if _server == null:
		on_ready.call_deferred(null)
		return
	_server._ask_thumbnail(key, on_ready)


## The model's picture when it was loaded this session, else fallback (it does not look in the cache).
static func cached_thumbnail(model: String, fallback: Texture2D = null) -> Texture2D:
	return _thumbnails.get(model_key(model), fallback)


static func _load(file: String) -> PackedScene:
	var document := GLTFDocument.new()
	var state := GLTFState.new()
	var error := document.append_from_file(file, state)
	if error != OK:
		push_warning("mapkit: could not read %s (error %d)" % [file, error])
		return null
	var root := document.generate_scene(state)
	if root == null:
		return null
	# In the editor the glTF loader makes import nodes, which draw nothing (a game run converts them itself).
	for importer: ImporterMeshInstance3D in root.find_children("*", "ImporterMeshInstance3D", true, false):
		var mesh := MeshInstance3D.new()
		mesh.name = importer.name
		mesh.transform = importer.transform
		if importer.mesh != null:
			mesh.mesh = importer.mesh.get_mesh()
		importer.replace_by(mesh)
		importer.free()
	for child in root.find_children("*", "", true, false):
		child.owner = root
	var scene := PackedScene.new()
	scene.pack(root)
	root.free()
	return scene


static func _read_json(file: String) -> Dictionary:
	if not FileAccess.file_exists(file):
		return {}
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(file))
	return parsed if parsed is Dictionary else {}


func _enter_tree() -> void:
	_server = self


func _exit_tree() -> void:
	if _server == self:
		_server = null
	_stop()


## Sends a request to mkasset. on_answer(answer: Dictionary) gets its answer: {"ok": true, ...}, or
## {"ok": false, "error": why}, also when mkasset cannot run or stops first.
func _call(request: Dictionary, on_answer: Callable) -> void:
	if not _start():
		on_answer.call_deferred({"ok": false, "error": "mkasset is not running: " + _start_failed})
		return
	request["id"] = _next_id
	_calls[_next_id] = on_answer
	_next_id += 1
	if _ready_seen:
		_send(request)
	else:
		_backlog.append(request)


func _ask(key: String, lod: int, id: String, on_loaded: Callable) -> void:
	if _failed.has(id):
		on_loaded.call_deferred(null)
		return
	if _waiting.has(id):
		_waiting[id].append(on_loaded)
		return
	_waiting[id] = [on_loaded]
	var out := ProjectSettings.globalize_path(CACHE_DIR.path_join(id + ".glb"))
	_call({"cmd": "export", "model": key, "lod": lod, "out": out}, _exported.bind(id))


func _exported(answer: Dictionary, id: String) -> void:
	var scene: PackedScene = null
	if answer.get("ok", false):
		scene = _load(CACHE_DIR.path_join(id + ".glb"))
		if scene != null:
			_scenes[id] = scene
	else:
		_failed[id] = answer.get("error", "")
		push_warning("mapkit: no game model %s: %s" % [id, answer.get("error", "")])
	for on_loaded in _waiting.get(id, []):
		on_loaded.call_deferred(scene)
	_waiting.erase(id)


func _ask_catalog(on_ready: Callable) -> void:
	if not _catalog.is_empty():
		on_ready.call_deferred(_catalog)
		return
	_catalog_waiting.append(on_ready)
	if _catalog_waiting.size() > 1:
		return
	var out := ProjectSettings.globalize_path(CATALOG_FILE)
	_call({"cmd": "catalog", "out": out, "brief": true}, func(answer: Dictionary) -> void:
		if not answer.get("ok", false):
			push_warning("mapkit: no model list from mkasset (%s); using the last one" % answer.get("error", ""))
		var read := _read_json(CATALOG_FILE)
		if answer.get("ok", false):
			_catalog = read
		for waiting in _catalog_waiting:
			waiting.call_deferred(read)
		_catalog_waiting.clear())


func _ask_thumbnail(key: String, on_ready: Callable) -> void:
	if _failed.has("thumbnail " + key):
		on_ready.call_deferred(null)
		return
	if _thumbnail_waiting.has(key):
		_thumbnail_waiting[key].append(on_ready)
		return
	_thumbnail_waiting[key] = [on_ready]
	var id := "%s_0" % key
	if _scenes.has(id) or FileAccess.file_exists(CACHE_DIR.path_join(id + ".glb")):
		# A model the map shows: its cached file.
		request(key, 0, _to_stage.bind(key))
		return
	# Any other: written for the picture only, and deleted after, so browsing does not fill the cache.
	var file := THUMBNAIL_DIR.path_join(key + ".glb")
	_call({"cmd": "export", "model": key, "lod": 0, "out": ProjectSettings.globalize_path(file)},
		func(answer: Dictionary) -> void:
			var scene: PackedScene = _load(file) if answer.get("ok", false) else null
			DirAccess.remove_absolute(ProjectSettings.globalize_path(file))
			_to_stage(scene, key))


func _to_stage(scene: PackedScene, key: String) -> void:
	if scene == null:
		_failed["thumbnail " + key] = true
		_thumbnail_done(key, null)
		return
	_to_render.append([key, scene])
	if not _rendering:
		_render_all()


func _render_all() -> void:
	_rendering = true
	while not _to_render.is_empty() and is_inside_tree():
		var next: Array = _to_render.pop_front()
		var image: Image = await _render(next[1])
		var picture: Texture2D = null
		if image != null:
			DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(THUMBNAIL_DIR))
			image.save_png(ProjectSettings.globalize_path(THUMBNAIL_DIR.path_join(next[0] + ".png")))
			picture = ImageTexture.create_from_image(image)
			_thumbnails[next[0]] = picture
		_thumbnail_done(next[0], picture)
	_rendering = false


func _thumbnail_done(key: String, picture: Texture2D) -> void:
	for on_ready in _thumbnail_waiting.get(key, []):
		on_ready.call_deferred(picture)
	_thumbnail_waiting.erase(key)


## Draws a model in the picture stage: seen from its front right and a little above, gray until textures come.
func _render(scene: PackedScene) -> Image:
	if _stage == null:
		_make_stage()
	var model := scene.instantiate() as Node3D
	_stage.add_child(model)
	var box := AABB()
	var first := true
	for mesh: MeshInstance3D in model.find_children("*", "MeshInstance3D", true, false):
		var part := mesh.global_transform * mesh.get_aabb()
		box = part if first else box.merge(part)
		first = false
	if first:
		model.free()
		return null
	var center := box.get_center()
	var radius := maxf(box.size.length() / 2.0, 0.01)
	# Game +X and -Y: the front of most objects, and of the perk machines (their -Y), in one view.
	var direction := MkSpace.dir_to_godot(Vector3(1.0, -0.8, 0.55)).normalized()
	var distance := radius / sin(deg_to_rad(_camera.fov / 2.0)) * 1.02
	_camera.near = maxf(distance - radius * 1.5, 0.01)
	_camera.far = distance + radius * 1.5
	_camera.look_at_from_position(center + direction * distance, center)
	_stage.render_target_update_mode = SubViewport.UPDATE_ONCE
	await RenderingServer.frame_post_draw
	var image := _stage.get_texture().get_image()
	model.queue_free()
	if image == null or image.is_empty():
		return null
	image.resize(THUMBNAIL_SIZE, THUMBNAIL_SIZE, Image.INTERPOLATE_LANCZOS)
	return image


func _make_stage() -> void:
	_stage = SubViewport.new()
	_stage.size = Vector2i(THUMBNAIL_SIZE, THUMBNAIL_SIZE) * 2
	_stage.own_world_3d = true
	_stage.transparent_bg = true
	_stage.msaa_3d = Viewport.MSAA_4X
	_stage.render_target_update_mode = SubViewport.UPDATE_DISABLED
	var environment := Environment.new()
	environment.background_mode = Environment.BG_CLEAR_COLOR
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.62, 0.64, 0.68)
	environment.ambient_light_energy = 0.9
	_camera = Camera3D.new()
	_camera.fov = 30.0
	_camera.environment = environment
	_stage.add_child(_camera)
	var key_light := DirectionalLight3D.new()
	key_light.light_energy = 1.1
	key_light.rotation_degrees = Vector3(-50, -30, 0)
	_stage.add_child(key_light)
	var back_light := DirectionalLight3D.new()
	back_light.light_energy = 0.35
	back_light.rotation_degrees = Vector3(-20, 150, 0)
	_stage.add_child(back_light)
	add_child(_stage)


## Starts mkasset serve unless it runs. False (and why, once, in the Output panel) when it cannot.
func _start() -> bool:
	if _pid != -1:
		return true
	var game := _game_folder()
	var mkasset := _mkasset_path()
	var why := ""
	if game.is_empty() or not DirAccess.dir_exists_absolute(game.path_join("zone")):
		why = "set the game folder in the mapkit dock"
	elif mkasset.is_empty():
		why = "mkasset.exe not found: build the mapkit\\mkasset project, or set %s in Editor Settings" % SETTING_MKASSET
	var arguments := PackedStringArray(["--game", game])
	if why.is_empty():
		for zone in ZONES:
			if FileAccess.file_exists(game.path_join("cw-mod/mapkit/trace/%s.mktrace" % zone)):
				arguments.append_array(["--zone", zone])
		if arguments.size() == 2:
			why = "no zone trace yet: add \"mapkit_trace\": %s to cw-mod.json and start Die Maschine once" % JSON.stringify(ZONES)
	if why.is_empty():
		arguments.append("serve")
		var process := OS.execute_with_pipe(mkasset, arguments, false)
		if process.is_empty():
			why = "could not start %s" % mkasset
		else:
			_pid = process["pid"]
			_stdio = process["stdio"]
			_stderr = process["stderr"]
			_out = ""
			_err = ""
			_ready_seen = false
			DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(CACHE_DIR))
			_start_failed = ""
			return true
	if why != _start_failed:
		push_warning("mapkit: game models are off: " + why)
		_start_failed = why
	return false


func _stop() -> void:
	if _pid == -1:
		return
	if OS.is_process_running(_pid):
		_send({"cmd": "quit"})
		OS.kill(_pid)
	_pid = -1
	_stdio = null
	_stderr = null


func _send(request: Dictionary) -> void:
	_stdio.store_string(JSON.stringify(request) + "\n")
	_stdio.flush()


func _process(_delta: float) -> void:
	if _pid == -1:
		return
	_err += _read(_stderr)
	while "\n" in _err:
		var line := _err.get_slice("\n", 0)
		_err = _err.substr(line.length() + 1)
		print("mkasset: ", line.strip_edges())
	_out += _read(_stdio)
	while "\n" in _out:
		var line := _out.get_slice("\n", 0)
		_out = _out.substr(line.length() + 1)
		_answer(line)
	if not OS.is_process_running(_pid):
		push_warning("mapkit: mkasset stopped (exit %d); see its lines above" % OS.get_process_exit_code(_pid))
		_pid = -1
		var calls := _calls.values()
		_calls.clear()
		_backlog.clear()
		for on_answer in calls:
			on_answer.call({"ok": false, "error": "mkasset stopped"})


static func _read(pipe: FileAccess) -> String:
	if pipe == null:
		return ""
	var bytes := pipe.get_buffer(65536)
	return bytes.get_string_from_utf8() if not bytes.is_empty() else ""


func _answer(line: String) -> void:
	var answer = JSON.parse_string(line)
	if not answer is Dictionary:
		return
	if answer.get("ready", false):
		_ready_seen = true
		print("mkasset: ready, %d models from %s" % [answer.get("models", 0), ", ".join(PackedStringArray(answer.get("zones", [])))])
		for request in _backlog:
			_send(request)
		_backlog.clear()
		return
	var id := int(answer.get("id", -1))
	if _calls.has(id):
		var on_answer: Callable = _calls[id]
		_calls.erase(id)
		on_answer.call(answer)


## The editor's settings, or null outside the editor (looked up by name: this script also runs in the walk).
static func _editor_settings() -> Object:
	if not Engine.is_editor_hint():
		return null
	return Engine.get_singleton(&"EditorInterface").get_editor_settings()


static func _game_folder() -> String:
	var settings := _editor_settings()
	if settings != null and settings.has_setting(SETTING_GAME_FOLDER):
		return String(settings.get_setting(SETTING_GAME_FOLDER)).strip_edges()
	return OS.get_environment("MAPKIT_GAME_DIR")


static func _mkasset_path() -> String:
	var settings := _editor_settings()
	if settings != null and settings.has_setting(SETTING_MKASSET) and FileAccess.file_exists(settings.get_setting(SETTING_MKASSET)):
		return settings.get_setting(SETTING_MKASSET)
	var in_repo := ProjectSettings.globalize_path(MKASSET_IN_REPO).simplify_path()
	return in_repo if FileAccess.file_exists(in_repo) else ""
