@tool
class_name MkMap
extends Node3D
## The root of a mapkit map: brushes and Zombies objects go under it. Export writes everything
## under it to <map_name>.mkmap, in map space (this node's origin is the game's origin).
##
## Run the scene (F6) to walk it in first person at game scale, from the first player spawn.

## The map's zone name: zm_ then lowercase letters, digits and '_'. The game loads <map_name>.ff.
@export var map_name: String = "zm_untitled"
## The name players see.
@export var title: String = "Untitled"
@export var author: String = ""


func _ready() -> void:
	if not Engine.is_editor_hint():
		_prepare_walk.call_deferred()


## Running the scene: collision on the brushes, a sun and a sky if the scene has none, and a walker
## at the first player spawn.
func _prepare_walk() -> void:
	for node in find_children("*", "CSGShape3D", true, false):
		var shape := node as CSGShape3D
		shape.use_collision = not (shape is MkBrush) or (shape as MkBrush).solid
	# Models dropped in without an MkMesh export solid face by face (MkExport.loose_meshes): walk them the same way.
	var loose := MkExport.loose_meshes(self)
	for root in loose:
		for instance in loose[root]:
			(instance as MeshInstance3D).create_trimesh_collision()
	if find_children("*", "DirectionalLight3D", true, false).is_empty():
		var sun := DirectionalLight3D.new()
		sun.rotation_degrees = Vector3(-55, 35, 0)
		sun.shadow_enabled = true
		add_child(sun)
	if find_children("*", "WorldEnvironment", true, false).is_empty():
		var sky := Sky.new()
		sky.sky_material = ProceduralSkyMaterial.new()
		var environment := Environment.new()
		environment.background_mode = Environment.BG_SKY
		environment.sky = sky
		environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
		environment.tonemap_mode = Environment.TONE_MAPPER_FILMIC
		var world := WorldEnvironment.new()
		world.environment = environment
		add_child(world)
	if get_viewport().get_camera_3d() != null:
		return
	var walker := MkWalker.new()
	add_child(walker)
	for node in find_children("*", "", true, true):
		if node is MkPlayerSpawn:
			var spawn := node as Node3D
			walker.global_position = spawn.global_position + Vector3(0, 0.05, 0)
			walker.rotation.y = spawn.global_rotation.y
			break
	walker.remember_start()
