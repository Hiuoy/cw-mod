@tool
class_name MkEntity
extends Node3D
## A Zombies gameplay object. Its box shows the size it has in the game and the arrow shows where it
## faces; where mapkit knows the game's model for it, that model is drawn instead of the box (MkAssets).
## Put the node's origin where the object stands: on the floor.

var _preview: MeshInstance3D
## The game's model for it, once loaded (MkAssets), and which model and turn it is.
var _model: Node3D
var _model_id := ""


## The entity's class in the .mkmap source. cwlink turns each class into the game's own entities.
func mk_class() -> String:
	return ""


## Bounds in game units, in the entity's own game axes (X forward, Y left, Z up), origin on the floor.
func mk_bounds() -> AABB:
	return AABB(Vector3(-16, -16, 0), Vector3(32, 32, 72))


func mk_color() -> Color:
	return Color(0.8, 0.8, 0.8)


## The class's own settings, as they go into the .mkmap source.
func mk_props() -> Dictionary:
	return {}


## A volume (a zone, a door) is an area: drawn see-through, resized with handles.
func mk_is_volume() -> bool:
	return false


## The game model it shows in place of its box, by name or hash ("" = the box).
func mk_model() -> String:
	return ""


## How the model is turned from the node's facing, in degrees of yaw: the turn cwlink gives the game object it
## places for this node, so the model faces here the way it will in the game.
func mk_model_yaw() -> float:
	return 0.0


## Where the model's origin is from the node's origin, in game units along the node's axes (X forward, Y left,
## Z up): where cwlink puts the game object it places for this node.
func mk_model_offset() -> Vector3:
	return Vector3.ZERO


func _ready() -> void:
	refresh()


## Redraws the box, and the model. Call it after a setting that changes the size or the model.
func refresh() -> void:
	if not is_inside_tree():
		return
	if _preview == null:
		_preview = MeshInstance3D.new()
		# Internal: not saved with the scene and not listed in the scene tree.
		add_child(_preview, false, Node.INTERNAL_MODE_BACK)
	var box := MkSpace.aabb_to_godot(mk_bounds())
	var mesh := BoxMesh.new()
	mesh.size = box.size
	_preview.mesh = mesh
	_preview.position = box.get_center()
	_preview.material_override = MkSpace.preview_material(mk_color(), mk_is_volume())
	_preview.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF if mk_is_volume() \
		else GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	_refresh_model()
	_preview.visible = _box_shown() and (Engine.is_editor_hint() or _shown_in_walk())
	update_gizmos()


func _refresh_model() -> void:
	var model := mk_model()
	var id := "" if model.is_empty() else "%s@%s@%s" % [MkAssets.model_key(model), mk_model_yaw(), mk_model_offset()]
	if id == _model_id:
		return
	_model_id = id
	if _model != null:
		_model.queue_free()
		_model = null
	if not id.is_empty():
		MkAssets.request(model, 0, _show_model.bind(id))


func _show_model(scene: PackedScene, id: String) -> void:
	if scene == null or id != _model_id or not is_inside_tree():
		return
	_model = scene.instantiate() as Node3D
	# mkasset writes the model in Godot space, its origin the game object's origin.
	_model.rotation.y = deg_to_rad(mk_model_yaw())
	_model.position = MkSpace.to_godot(mk_model_offset())
	add_child(_model, false, Node.INTERNAL_MODE_BACK)
	_model.visible = Engine.is_editor_hint() or _shown_in_walk()
	_preview.visible = _box_shown() and (Engine.is_editor_hint() or _shown_in_walk())


## The box shows until the model does. A volume's box stays in the editor: it is the area, not the look.
func _box_shown() -> bool:
	return _model == null or (mk_is_volume() and Engine.is_editor_hint())


## Shown when walking the map (F6). Zones are hidden there.
func _shown_in_walk() -> bool:
	return not mk_is_volume()
