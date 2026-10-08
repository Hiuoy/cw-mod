@tool
extends EditorNode3DGizmoPlugin
## The editor view of Zombies objects: the outline of their game box, a facing arrow, a clickable
## box, and resize handles on volumes (zones, doors).

const SNAP := 0.125 # meters (about 5 units); hold Shift to drag freely
const HANDLE_NAMES := ["Right", "Left", "Top", "Back", "Front"]
# Per handle: the axis it moves along, in the node's own Godot axes.
const HANDLE_AXES := [Vector3.RIGHT, Vector3.LEFT, Vector3.UP, Vector3.BACK, Vector3.FORWARD]

var _plugin: EditorPlugin


func _init(plugin: EditorPlugin) -> void:
	_plugin = plugin
	create_material("lines", Color.WHITE, false, false, true)
	create_handle_material("handles")


func _get_gizmo_name() -> String:
	return "mapkit"


func _has_gizmo(node: Node3D) -> bool:
	return node is MkEntity


func _redraw(gizmo: EditorNode3DGizmo) -> void:
	gizmo.clear()
	var node := gizmo.get_node_3d() as MkEntity
	var box := MkSpace.aabb_to_godot(node.mk_bounds())
	var lines := MkSpace.box_lines(box)
	if not node.mk_is_volume():
		# The game's forward is Godot's -Z: an arrow out of the front face.
		var start := Vector3(0, box.position.y + box.size.y * 0.35, box.position.z)
		var tip := start + Vector3(0, 0, -0.5)
		lines.append_array(PackedVector3Array([start, tip, tip, tip + Vector3(0.15, 0, 0.15), tip, tip + Vector3(-0.15, 0, 0.15)]))
	gizmo.add_lines(lines, get_material("lines", gizmo), false, node.mk_color())
	gizmo.add_collision_triangles(MkSpace.box_triangle_mesh(box))
	if node is MkVolume:
		gizmo.add_handles(_handle_points(node as MkVolume), get_material("handles", gizmo), PackedInt32Array())


func _handle_points(volume: MkVolume) -> PackedVector3Array:
	var s := volume.size
	return PackedVector3Array([
		Vector3(s.x / 2, s.y / 2, 0), Vector3(-s.x / 2, s.y / 2, 0), Vector3(0, s.y, 0),
		Vector3(0, s.y / 2, s.z / 2), Vector3(0, s.y / 2, -s.z / 2),
	])


func _get_handle_name(_gizmo: EditorNode3DGizmo, handle_id: int, _secondary: bool) -> String:
	return HANDLE_NAMES[handle_id]


func _get_handle_value(gizmo: EditorNode3DGizmo, _handle_id: int, _secondary: bool) -> Variant:
	var volume := gizmo.get_node_3d() as MkVolume
	return [volume.size, volume.position]


## Moves one face and keeps the opposite one where it is. The top moves alone: the floor stays put.
func _set_handle(gizmo: EditorNode3DGizmo, handle_id: int, _secondary: bool, camera: Camera3D, screen_pos: Vector2) -> void:
	var volume := gizmo.get_node_3d() as MkVolume
	var to_local := volume.global_transform.affine_inverse()
	var ray_from := to_local * camera.project_ray_origin(screen_pos)
	var ray_to := to_local * (camera.project_ray_origin(screen_pos) + camera.project_ray_normal(screen_pos) * 4096.0)
	var axis: Vector3 = HANDLE_AXES[handle_id]
	var pivot := Vector3(0, 0.0 if handle_id == 2 else volume.size.y / 2, 0)
	var closest := Geometry3D.get_closest_points_between_segments(pivot - axis * 4096.0, pivot + axis * 4096.0, ray_from, ray_to)
	var reach := (closest[0] - pivot).dot(axis) # the dragged face's distance from the pivot
	var snap := not Input.is_key_pressed(KEY_SHIFT)
	var size := volume.size
	if handle_id == 2:
		size.y = maxf(snappedf(reach, SNAP) if snap else reach, 0.1)
		volume.size = size
		return
	var index := 0 if absf(axis.x) > 0.5 else 2
	var half := size[index] / 2
	var new_size := reach + half
	new_size = maxf(snappedf(new_size, SNAP) if snap else new_size, 0.1)
	size[index] = new_size
	volume.position += volume.transform.basis * (axis * (new_size - 2 * half) / 2)
	volume.size = size


func _commit_handle(gizmo: EditorNode3DGizmo, _handle_id: int, _secondary: bool, restore: Variant, cancel: bool) -> void:
	var volume := gizmo.get_node_3d() as MkVolume
	if cancel:
		volume.size = restore[0]
		volume.position = restore[1]
		return
	var undo := _plugin.get_undo_redo()
	undo.create_action("Resize %s" % volume.name)
	undo.add_do_property(volume, "size", volume.size)
	undo.add_do_property(volume, "position", volume.position)
	undo.add_undo_property(volume, "size", restore[0])
	undo.add_undo_property(volume, "position", restore[1])
	undo.commit_action(false)
