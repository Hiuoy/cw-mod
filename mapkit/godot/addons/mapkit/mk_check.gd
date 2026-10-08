@tool
class_name MkCheck
extends RefCounted
## The checks run before export: what would stop the map from building, or from playing well.
## Each problem is {"error": bool, "text": String, "node": Node or null}.


## A zone name the game can load: zm_, then lowercase letters, digits and '_'. At most 48 characters,
## so the longest variant name (ww_1080_<name>) still fits the engine's 64-byte name fields.
static func valid_map_name(map_name: String) -> bool:
	return map_name.length() <= 48 and RegEx.create_from_string("^zm_[a-z0-9_]+$").search(map_name) != null


## game_folder: the game's install folder, to check the name against the shipped maps. May be "".
static func run(map: MkMap, game_folder: String) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	if not valid_map_name(map.map_name):
		_add(out, true, "Map name '%s': use zm_ then lowercase letters, digits and '_' (48 characters at most)." % map.map_name, map)
	elif not game_folder.is_empty() and FileAccess.file_exists(game_folder.path_join("zone").path_join(map.map_name + ".ff")):
		_add(out, true, "Map name '%s' is a map the game ships. Pick another name." % map.map_name, map)

	var brushes: Array[CSGBox3D] = []
	var meshes: Array[MkMesh] = []
	var entities := [] # MkEntity and MkLight: called by duck typing, so untyped
	for node in map.find_children("*", "", true, true):
		if node is CSGBox3D and (node as CSGBox3D).operation == CSGShape3D.OPERATION_UNION:
			brushes.append(node as CSGBox3D)
		elif node is CSGShape3D:
			_add(out, false, "%s: only CSG boxes in union mode export; this one is skipped." % node.name, node)
		elif node is MkMesh:
			meshes.append(node as MkMesh)
		elif node is Node3D and node.has_method("mk_class"):
			entities.append(node)

	var solid_meshes := 0
	var loose := MkExport.loose_meshes(map)
	for root in loose:
		solid_meshes += 1
		var triangles := 0
		for instance: MeshInstance3D in loose[root]:
			triangles += _triangle_count(instance)
		if triangles > 4000:
			_add(out, false, "%s: not under an MkMesh, so it exports solid face by face (%d faces, each a piece of collision the game checks): put it under an MkMesh with collision hull or none, or keep it simpler." % [root.name, triangles], root)
	for mesh in meshes:
		var triangles := 0
		for instance in mesh.mk_meshes():
			for i in instance.mesh.get_surface_count():
				if not MkExport.is_triangles(instance.mesh, i):
					_add(out, false, "%s: surface %d of %s is not triangles; it is skipped." % [mesh.name, i, instance.name], mesh)
			triangles += _triangle_count(instance)
		if mesh.mk_meshes().is_empty():
			_add(out, false, "%s has no mesh under it: drag an imported model (.glb) onto it." % mesh.name, mesh)
		elif mesh.collision != "none":
			solid_meshes += 1
		if mesh.collision == "faces" and triangles > 4000:
			_add(out, false, "%s: %d faces, each a piece of collision the game checks: keep solid meshes simple, or use collision hull, or none plus brushes as clips." % [mesh.name, triangles], mesh)

	if brushes.is_empty() and solid_meshes == 0:
		_add(out, true, "Nothing to stand on: add an MkBrush (or a CSGBox3D) for the floor, or a solid MkMesh.", map)
	for brush in brushes:
		var world_size := brush.size * brush.global_basis.get_scale().abs()
		if world_size.x < 0.01 or world_size.y < 0.01 or world_size.z < 0.01:
			_add(out, true, "%s: the box is flat; give it some size on every axis." % brush.name, brush)
		if brush is MkBrush and not (brush as MkBrush).solid and not (brush as MkBrush).rendered:
			_add(out, false, "%s: neither solid nor rendered, so it does nothing in the game." % brush.name, brush)

	var by_class := {}
	for entity in entities:
		var entity_class: String = entity.mk_class()
		if not by_class.has(entity_class):
			by_class[entity_class] = []
		by_class[entity_class].append(entity)
		var scalable: bool = entity.has_method("mk_scalable") and entity.mk_scalable()
		if not (entity is MkVolume) and not scalable and not entity.scale.is_equal_approx(Vector3.ONE):
			_add(out, false, "%s: scale is ignored; the game object has a fixed size." % entity.name, entity)

	var spawns: Array = by_class.get("player_spawn", [])
	if spawns.is_empty():
		_add(out, true, "No player spawn: add four MkPlayerSpawn nodes.", map)
	elif spawns.size() < 4:
		_add(out, false, "%d player spawn(s): four players need four." % spawns.size(), map)
	for spawn in spawns:
		var foot: Vector3 = spawn.global_position
		if _inside_any(brushes, foot + Vector3(0, 0.9, 0)):
			_add(out, true, "%s is inside a brush." % spawn.name, spawn)
		elif not _hits_any(brushes, foot + Vector3(0, 0.25, 0), foot - Vector3(0, 1.0, 0)):
			_add(out, false, "%s has no floor under it." % spawn.name, spawn)

	var zones := {}
	for zone in by_class.get("zone", []):
		if zones.has(zone.zone_name):
			_add(out, true, "Two zones are named '%s'." % zone.zone_name, zone)
		zones[zone.zone_name] = zone
	if zones.is_empty():
		_add(out, false, "No zone: add an MkZone around the start area, with active_at_start on.", map)
	elif not zones.values().any(func(zone): return zone.active_at_start):
		_add(out, false, "No zone has active_at_start on, so no spawner is used at the start.", map)

	for ambient in by_class.get("ambient_room", []):
		var room_name: String = ambient.room.strip_edges()
		if room_name.is_empty():
			_add(out, true, "%s: set the room (a build lists Die Maschine's)." % ambient.name, ambient)
		elif room_name != ambient.room or room_name.contains(" ") or room_name.contains("\"") or room_name.contains("\\"):
			_add(out, true, "%s: room '%s' has a space, quote or backslash." % [ambient.name, ambient.room], ambient)
		elif not MkAmbientRoom.DIE_MASCHINE_ROOMS.split(",").has(room_name):
			_add(out, false, "%s: room '%s' is not one of Die Maschine's named rooms: if its sound bank doesn't have it, the area sounds like the rest of the map." % [ambient.name, ambient.room], ambient)

	var spawners: Array = by_class.get("zombie_spawner", [])
	if spawners.is_empty():
		_add(out, true, "No zombie spawner: add an MkZombieSpawner.", map)
	for node in spawners + by_class.get("barrier", []):
		if not zones.has(node.zone):
			_add(out, true, "%s: zone '%s' doesn't exist." % [node.name, node.zone], node)
	var barriers: Array = by_class.get("barrier", [])
	for spawner in spawners:
		if spawner.kind == "barrier" and not barriers.any(func(barrier): return barrier.zone == spawner.zone):
			_add(out, false, "%s: a barrier spawner, but zone '%s' has no MkBarrier, so its zombies go straight for the players." % [spawner.name, spawner.zone], spawner)
	var switches: Array = by_class.get("power_switch", [])
	for door in by_class.get("door", []):
		if door.needs_power and switches.is_empty():
			_add(out, false, "%s needs power, but the map has no MkPowerSwitch: it is bought as usual." % door.name, door)
		elif door.cost <= 0 and not door.needs_power:
			_add(out, false, "%s is free (cost 0)." % door.name, door)
		var opened: Array = door.mk_props()["opens"]
		if opened.is_empty():
			_add(out, false, "%s opens no zone." % door.name, door)
		for zone_name in opened:
			if not zones.has(zone_name):
				_add(out, true, "%s opens zone '%s', which doesn't exist." % [door.name, zone_name], door)

	for wall_buy in by_class.get("wall_buy", []):
		if wall_buy.weapon.strip_edges().is_empty():
			_add(out, true, "%s: set the weapon." % wall_buy.name, wall_buy)
	for prop in by_class.get("prop", []):
		if prop.model.strip_edges().is_empty():
			_add(out, true, "%s: set the model." % prop.name, prop)
		var scale: Vector3 = prop.global_transform.basis.get_scale()
		if maxf(scale.x, maxf(scale.y, scale.z)) - minf(scale.x, minf(scale.y, scale.z)) > 0.01:
			_add(out, false, "%s is scaled unevenly: the game scales a model evenly, by %.2f." % [prop.name,
				(scale.x + scale.y + scale.z) / 3.0], prop)

	var perks := {}
	for machine in by_class.get("perk_machine", []):
		if perks.has(machine.perk):
			_add(out, false, "%s: a second %s machine." % [machine.name, machine.perk], machine)
		perks[machine.perk] = true

	var boxes: Array = by_class.get("mystery_box", [])
	var starts := boxes.filter(func(box): return box.start_here).size()
	if boxes.size() > 1 and starts == 0:
		_add(out, false, "No Mystery Box location has start_here on.", boxes[0])
	elif starts > 1:
		_add(out, false, "%d Mystery Box locations have start_here on; the box starts in one." % starts, boxes[0])

	var exfils: Array = by_class.get("exfil", [])
	var radios: Array = by_class.get("exfil_radio", [])
	if exfils.size() > 1:
		_add(out, false, "%d exfil points: the build uses %s only." % [exfils.size(), exfils[0].name], exfils[1])
	if radios.size() > 1:
		_add(out, false, "%d exfil radios: the build uses %s only." % [radios.size(), radios[0].name], radios[1])
	if not exfils.is_empty() and radios.is_empty():
		_add(out, false, "%s: no MkExfilRadio, so players can't call the exfil." % exfils[0].name, exfils[0])
	elif exfils.is_empty() and not radios.is_empty():
		_add(out, false, "%s: no MkExfil, so the radio has no helicopter to call." % radios[0].name, radios[0])
	for exfil in exfils:
		for zone_name in exfil.mk_props()["zones"]:
			if not zones.has(zone_name):
				_add(out, true, "%s: zone '%s' doesn't exist." % [exfil.name, zone_name], exfil)

	var tables: Array = by_class.get("crafting_table", [])
	if tables.size() > 1 and tables.any(func(table): return table.items != tables[0].items):
		_add(out, false, "The crafting tables sell different items; the game has one list, so each item sells only if every table has it on.", tables[0])

	var environment := MkExport.sky_environment(map)
	if environment != null:
		var sky_material := environment.sky.sky_material
		if sky_material is PanoramaSkyMaterial:
			var panorama := (sky_material as PanoramaSkyMaterial).panorama
			if panorama == null:
				_add(out, false, "The WorldEnvironment's panorama sky has no panorama image: the game keeps Die Maschine's sky.", map)
			elif absf(float(panorama.get_width()) / panorama.get_height() - 2.0) > 0.02:
				_add(out, false, "The sky panorama is %d x %d; the game's is twice as wide as high, so it is stretched to that." % [panorama.get_width(), panorama.get_height()], map)
		elif DisplayServer.get_name() == "headless":
			_add(out, false, "The sky is no panorama: it is baked to one only when exporting from the editor.", map)
		if not environment.sky_rotation.is_zero_approx():
			_add(out, false, "The sky is turned (the environment's sky rotation): the game shows it unturned.", map)
	var sun := MkExport.sun_light(map)
	for node in map.find_children("*", "DirectionalLight3D", true, false):
		if node != sun and (node as DirectionalLight3D).is_visible_in_tree():
			_add(out, false, "%s: the game has one sun, %s; this directional light is not exported." % [node.name, sun.name], node)
	return out


static func error_count(problems: Array[Dictionary]) -> int:
	return problems.filter(func(problem): return problem["error"]).size()


## The triangles of a mesh's triangle surfaces (the ones that export).
static func _triangle_count(instance: MeshInstance3D) -> int:
	var triangles := 0
	for i in instance.mesh.get_surface_count():
		if not MkExport.is_triangles(instance.mesh, i):
			continue
		var arrays := instance.mesh.surface_get_arrays(i)
		var indices = arrays[Mesh.ARRAY_INDEX]
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		triangles += (indices.size() if indices != null and indices.size() > 0 else vertices.size()) / 3
	return triangles


static func _add(out: Array[Dictionary], is_error: bool, text: String, node: Node) -> void:
	out.append({"error": is_error, "text": text, "node": node})


static func _inside_any(brushes: Array[CSGBox3D], point: Vector3) -> bool:
	for brush in brushes:
		if AABB(-brush.size / 2, brush.size).has_point(brush.global_transform.affine_inverse() * point):
			return true
	return false


static func _hits_any(brushes: Array[CSGBox3D], from: Vector3, to: Vector3) -> bool:
	for brush in brushes:
		var local := brush.global_transform.affine_inverse()
		if AABB(-brush.size / 2, brush.size).intersects_segment(local * from, local * to) != null:
			return true
	return false
