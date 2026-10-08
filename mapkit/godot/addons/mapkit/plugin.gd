@tool
extends EditorPlugin
## mapkit: the Zombies object gizmos and the mapkit dock. The node types (MkMap, MkBrush, MkZone,
## MkPerkMachine, ...) are global classes, so they are in Add Node with or without the plugin.

const Gizmo := preload("res://addons/mapkit/gizmo.gd")
const Dock := preload("res://addons/mapkit/dock.gd")
const Assets := preload("res://addons/mapkit/mk_assets.gd")
const Inspector := preload("res://addons/mapkit/mk_inspector.gd")

var _gizmo: EditorNode3DGizmoPlugin
var _dock: Control
var _assets: Node
var _inspector: EditorInspectorPlugin


func _enter_tree() -> void:
	# The game models (MkAssets): runs mkasset in the background while the editor is open.
	_assets = Assets.new()
	add_child(_assets)
	# The model fields' pictures and Pick button (the model picker).
	_inspector = Inspector.new()
	add_inspector_plugin(_inspector)
	_gizmo = Gizmo.new(self)
	add_node_3d_gizmo_plugin(_gizmo)
	_dock = Dock.new()
	add_control_to_dock(DOCK_SLOT_RIGHT_UL, _dock)
	scene_changed.connect(_dock.refresh)


func _exit_tree() -> void:
	scene_changed.disconnect(_dock.refresh)
	remove_control_from_docks(_dock)
	_dock.queue_free()
	remove_node_3d_gizmo_plugin(_gizmo)
	remove_inspector_plugin(_inspector)
	_assets.queue_free()
