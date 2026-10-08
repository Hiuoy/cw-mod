@tool
class_name MkMesh
extends Node3D
## Your own level geometry, made in Blender or any 3D tool: put the imported model (.glb, .gltf or .blend,
## dragged from the FileSystem dock) under this node. Every mesh under it is drawn in the game, and made solid by
## `collision`. Move, turn and scale it like any node.
##
## Materials: a mesh material draws as it looks here: its colour texture (tinted by its colour) or its colour, its
## normal map, roughness, metallic and transparency (cut out, or see-through as glass). Emission is exported, but no
## glow shows in the game yet. One named like a game material (for example mc/mtl_p7_cinder_block; name it so in
## Blender) draws with that game material instead. A model dropped straight under the map exports as if under an
## MkMesh with these defaults.

## How it is solid in the game: faces = every face (exact: rooms, stairs, anything concave; keep the mesh
## simple, every face is a piece of collision); hull = one shape wrapped around each mesh (fast, but concave parts
## are filled in); none = walk-through.
@export_enum("faces", "hull", "none") var collision: String = "faces"
## A game material for every surface whose material is not named after one, in place of its own look. Empty: each
## surface's own look.
@export var game_material: String = ""


func _ready() -> void:
	if not Engine.is_editor_hint():
		_add_walk_collision.call_deferred()


## The meshes under it: the ones MkExport writes.
func mk_meshes() -> Array[MeshInstance3D]:
	var meshes: Array[MeshInstance3D] = []
	for node in find_children("*", "MeshInstance3D", true, false):
		var instance := node as MeshInstance3D
		if instance.mesh != null and instance.is_visible_in_tree():
			meshes.append(instance)
	return meshes


## Walking the map (F6): the same collision as in the game.
func _add_walk_collision() -> void:
	if collision == "none":
		return
	for instance in mk_meshes():
		if collision == "faces":
			instance.create_trimesh_collision()
		else:
			instance.create_convex_collision()
