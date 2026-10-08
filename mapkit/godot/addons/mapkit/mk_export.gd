@tool
class_name MkExport
extends RefCounted
## Writes a map scene as the .mkmap source that cwlink builds from (format: mapkit/mkmap-format.md).
## Everything is in the game's units and axes, relative to the MkMap node.

const FORMAT := 1
const GENERATOR := "mapkit-godot 0.1"


## textures: when given, gets every texture a mesh surface names ({path relative to the .mkmap: Image}), for write
## to save next to it.
static func to_source(map: MkMap, textures = null) -> Dictionary:
	var brushes := []
	var entities := []
	var meshes := []
	for node in map.find_children("*", "", true, true):
		if node is CSGBox3D and (node as CSGBox3D).operation == CSGShape3D.OPERATION_UNION:
			brushes.append(_brush(map, node as CSGBox3D))
		elif node is MkMesh:
			var mesh := node as MkMesh
			meshes.append(_mesh(map, mesh, mesh.mk_meshes(), mesh.collision, mesh.game_material, textures))
		elif node is Node3D and node.has_method("mk_class"):
			entities.append(_entity(map, node))
	var loose := loose_meshes(map)
	for root in loose:
		meshes.append(_mesh(map, root, loose[root], "faces", "", textures))
	var source := {
		"mkmap": FORMAT,
		"generator": GENERATOR,
		"units": "inch",
		"axes": "x forward, y left, z up",
		"map": {"name": map.map_name, "title": map.title, "author": map.author, "mode": "zm"},
		"brushes": brushes,
		"entities": entities,
	}
	if not meshes.is_empty():
		source["meshes"] = meshes
	var sky := _sky(map, textures)
	if not sky.is_empty():
		source["sky"] = sky
	var lighting := _lighting(map)
	if not lighting.is_empty():
		source["lighting"] = lighting
	return source


## Returns "" on success, else what went wrong.
static func write(map: MkMap, path: String) -> String:
	var error := DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	if error != OK and error != ERR_ALREADY_EXISTS:
		return "can't create %s (%s)" % [path.get_base_dir(), error_string(error)]
	var textures := {}
	var source := to_source(map, textures)
	for relative in textures:
		var texture_path: String = path.get_base_dir().path_join(relative)
		DirAccess.make_dir_recursive_absolute(texture_path.get_base_dir())
		var dds := FileAccess.open(texture_path, FileAccess.WRITE)
		if dds == null:
			return "can't write %s (%s)" % [texture_path, error_string(FileAccess.get_open_error())]
		dds.store_buffer(_dds(textures[relative]))
		dds.close()
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return "can't write %s (%s)" % [path, error_string(FileAccess.get_open_error())]
	var text := JSON.stringify(source, "\t", false)
	# One line per vector ([x, y, z], and a mesh's [u, v]) keeps the file short and its diffs readable.
	text = RegEx.create_from_string("\\[\\s*([-\\d.e+]+),\\s*([-\\d.e+]+),\\s*([-\\d.e+]+)\\s*\\]").sub(text, "[$1, $2, $3]", true)
	text = RegEx.create_from_string("\\[\\s*([-\\d.e+]+),\\s*([-\\d.e+]+)\\s*\\]").sub(text, "[$1, $2]", true)
	file.store_string(text + "\n")
	file.close()
	return ""


static func _in_map(map: Node3D, node: Node3D) -> Transform3D:
	return map.global_transform.affine_inverse() * node.global_transform


## A box as its center and three half-axes (a rotated or scaled box stays exact).
static func _brush(map: MkMap, box: CSGBox3D) -> Dictionary:
	var t := _in_map(map, box)
	var half := box.size / 2
	var out := {
		"path": str(map.get_path_to(box)),
		"center": MkSpace.rounded(MkSpace.to_game(t.origin)),
		"half_axes": [
			MkSpace.rounded(MkSpace.to_game(t.basis.x * half.x)),
			MkSpace.rounded(MkSpace.to_game(t.basis.y * half.y)),
			MkSpace.rounded(MkSpace.to_game(t.basis.z * half.z)),
		],
		"material": "",
		"solid": true,
		"rendered": true,
	}
	if box is MkBrush:
		var brush := box as MkBrush
		out["material"] = brush.game_material
		out["solid"] = brush.solid
		out["rendered"] = brush.rendered
	return out


## The meshes in the map that are under no MkMesh (a model dropped straight into the scene), by the node that holds
## them right under the map (the imported model's root): each exports as an MkMesh with the defaults (collision faces,
## its own look). Meshes a Zombies object draws (its game model) are not included.
static func loose_meshes(map: Node3D) -> Dictionary:
	var out := {}
	for node in map.find_children("*", "MeshInstance3D", true, true):
		var instance := node as MeshInstance3D
		if instance.mesh == null or not instance.is_visible_in_tree():
			continue
		var root: Node = instance
		var under_mapkit := false
		var parent := instance.get_parent()
		while parent != null and parent != map:
			if parent is MkMesh or parent.has_method("mk_class"):
				under_mapkit = true
				break
			root = parent
			parent = parent.get_parent()
		if under_mapkit or parent == null:
			continue
		if not out.has(root):
			out[root] = []
		out[root].append(instance)
	return out


## A mesh node (an MkMesh, or the root of a loose model): each mesh under it (a part) with its triangle surfaces, in map
## space, game units and axes. A surface keeps its Godot material's name, which cwlink matches against the game's
## materials, and its look (color_texture), which cwlink draws when the name is no game material.
static func _mesh(map: MkMap, node: Node, instances: Array, collision: String, game_material: String, textures = null) -> Dictionary:
	var parts := []
	for instance: MeshInstance3D in instances:
		var t := _in_map(map, instance)
		var normal_basis := t.basis.inverse().transposed()
		# A mirroring transform turns the faces inside out: swap the winding back.
		var mirrored := t.basis.determinant() < 0
		var surfaces := []
		for i in instance.mesh.get_surface_count():
			if not is_triangles(instance.mesh, i):
				continue
			var arrays := instance.mesh.surface_get_arrays(i)
			var points: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
			var normals = arrays[Mesh.ARRAY_NORMAL]
			var uvs = arrays[Mesh.ARRAY_TEX_UV]
			var indices = arrays[Mesh.ARRAY_INDEX]
			var out_points := []
			var out_normals := []
			var out_uvs := []
			for k in points.size():
				out_points.append(MkSpace.rounded(MkSpace.to_game(t * points[k])))
				var n := Vector3.UP
				if normals != null and k < normals.size():
					n = (normal_basis * normals[k]).normalized()
				out_normals.append(MkSpace.rounded(MkSpace.dir_to_game(n), 0.0001))
				var uv := Vector2.ZERO
				if uvs != null and k < uvs.size():
					uv = uvs[k]
				out_uvs.append([snappedf(uv.x, 0.0001), snappedf(uv.y, 0.0001)])
			var triangles := []
			var count: int = indices.size() if indices != null and indices.size() > 0 else points.size()
			for k in range(0, count - 2, 3):
				var a: int = indices[k] if indices != null and indices.size() > 0 else k
				var b: int = indices[k + 1] if indices != null and indices.size() > 0 else k + 1
				var c: int = indices[k + 2] if indices != null and indices.size() > 0 else k + 2
				# Godot's front faces are clockwise, as the game's are, and the axis swap keeps handedness.
				triangles.append([a, c, b] if mirrored else [a, b, c])
			var material := instance.get_active_material(i)
			var surface := {
				"material": material.resource_name if material != null else "",
				"vertices": out_points,
				"normals": out_normals,
				"uvs": out_uvs,
				"triangles": triangles,
			}
			var alpha := _alpha_mode(material)
			var look := _color_image(material, alpha)
			if look != null:
				_add_texture(surface, "color_texture", look, map, textures)
				_add_texture(surface, "normal_texture", _normal_image(material), map, textures)
				_add_texture(surface, "roughness_texture", _roughness_image(material), map, textures)
				_add_texture(surface, "metal_texture", _metal_image(material), map, textures)
				if alpha != "opaque":
					surface["alpha"] = alpha
				var glow := _emission_image(material)
				if glow != null:
					_add_texture(surface, "emission_texture", glow, map, textures)
					surface["emission_energy"] = snappedf((material as BaseMaterial3D).emission_energy_multiplier, 0.001)
			surfaces.append(surface)
		parts.append({"path": str(map.get_path_to(instance)), "surfaces": surfaces})
	return {
		"path": str(map.get_path_to(node)),
		"collision": collision,
		"game_material": game_material,
		"parts": parts,
	}


## Whether a mesh's surface is triangles (the ones that export): an ArrayMesh says; Godot's primitive meshes (BoxMesh,
## QuadMesh and the like) always are, and have no call to ask.
static func is_triangles(mesh: Mesh, surface: int) -> bool:
	return not mesh.has_method("surface_get_primitive_type") \
		or mesh.call("surface_get_primitive_type", surface) == Mesh.PRIMITIVE_TRIANGLES


## A surface's texture (key: color_texture, normal_texture, roughness_texture, metal_texture, emission_texture) as a
## .dds next to the .mkmap.
static func _add_texture(surface: Dictionary, key: String, image: Image, map: MkMap, textures) -> void:
	if image == null:
		return
	var relative := "%s_textures/%s.dds" % [map.map_name, _image_key(image)]
	surface[key] = relative
	if textures != null:
		textures[relative] = image


## The texture formats the game draws, as the DXGI formats a .dds names them by.
const _DXGI := {
	Image.FORMAT_DXT1: 71,
	Image.FORMAT_DXT3: 74,
	Image.FORMAT_DXT5: 77,
	Image.FORMAT_BPTC_RGBA: 98,
	Image.FORMAT_BPTC_RGBFU: 95,
	Image.FORMAT_RGBA8: 28,
	Image.FORMAT_RGTC_R: 80,
	Image.FORMAT_R8: 61,
}
## The game's largest texture side mapkit writes (Die Maschine's largest are 2048).
const _MAX_TEXTURE_SIDE := 2048
## How much roughness a mip level's spread of normals adds (the normal map's b), fitted to Die Maschine's normal
## maps: b = sqrt(_NORMAL_SPREAD * (1 - |mean normal|^2)).
const _NORMAL_SPREAD := 2.0
## Converted normal and roughness maps by source and settings, so a build converts each once per editor session.
static var _converted := {}


## How a surface's alpha draws in the game: "opaque" (ignored), "clip" (cut out, Godot's alpha scissor and alpha
## hash) or "blend" (see-through, Godot's alpha and depth pre-pass).
static func _alpha_mode(material: Material) -> String:
	var base := material as BaseMaterial3D
	if base == null:
		return "opaque"
	match base.transparency:
		BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR, BaseMaterial3D.TRANSPARENCY_ALPHA_HASH:
			return "clip"
		BaseMaterial3D.TRANSPARENCY_ALPHA, BaseMaterial3D.TRANSPARENCY_ALPHA_DEPTH_PRE_PASS:
			return "blend"
	return "opaque"


## What a surface shows in the editor, as an image the game can draw: the material's albedo texture times its albedo
## colour (the tint), or a 4 x 4 swatch of the colour when it has no texture. Null for a material with no albedo (a
## ShaderMaterial). Its alpha is the surface's opacity when `alpha` is "blend"; for "clip" it is made 0 or 1 at the
## material's alpha scissor threshold, so the game's own threshold cuts the same edge. An untinted, uncut texture goes
## as imported (block-compressed, with mipmaps) when it can; otherwise it is converted and compressed here.
static func _color_image(material: Material, alpha: String) -> Image:
	var base := material as BaseMaterial3D
	if base == null:
		return null
	var tint := base.albedo_color
	if alpha == "opaque":
		tint.a = 1.0
	var threshold := base.alpha_scissor_threshold if base.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR else 0.5
	if base.albedo_texture == null:
		var swatch := Image.create_empty(4, 4, false, Image.FORMAT_RGBA8)
		var a := tint.a
		if alpha == "clip":
			a = 1.0 if a >= threshold else 0.0
		swatch.fill(Color(tint.r, tint.g, tint.b, a))
		return swatch
	var source := base.albedo_texture.get_image()
	if source == null or source.is_empty():
		return null
	var w := source.get_width()
	var h := source.get_height()
	if tint.is_equal_approx(Color.WHITE) and alpha != "clip" and source.is_compressed() and source.has_mipmaps() \
			and _DXGI.has(source.get_format()) and w % 4 == 0 and h % 4 == 0 and w <= _MAX_TEXTURE_SIDE and h <= _MAX_TEXTURE_SIDE:
		return source.duplicate() as Image
	var key := "color %s %s %s %f" % [_image_key(source), tint.to_html(), alpha, threshold]
	if _converted.has(key):
		return _converted[key]
	var image := source.duplicate() as Image
	_uncompressed(image, Image.FORMAT_RGBA8)
	if not tint.is_equal_approx(Color.WHITE) or alpha == "clip":
		# The tint multiplies the sRGB bytes: Godot multiplies in linear light, the same up to the sRGB curve's
		# difference from a pure power.
		var bytes := image.get_data()
		var cut := roundi(threshold * 255.0)
		for p in range(0, bytes.size(), 4):
			bytes[p] = roundi(bytes[p] * tint.r)
			bytes[p + 1] = roundi(bytes[p + 1] * tint.g)
			bytes[p + 2] = roundi(bytes[p + 2] * tint.b)
			var a := roundi(bytes[p + 3] * tint.a)
			if alpha == "clip":
				a = 255 if a >= cut else 0
			bytes[p + 3] = a
		image.set_data(image.get_width(), image.get_height(), false, Image.FORMAT_RGBA8, bytes)
	image.generate_mipmaps()
	# BC1 (BC3 with alpha); if this Godot cannot compress, the texture goes uncompressed.
	image.compress(Image.COMPRESS_S3TC, Image.COMPRESS_SOURCE_SRGB)
	_converted[key] = image
	return image


## An image made ready for converting: uncompressed in `format`, no mipmaps, whole 4 x 4 blocks, at most
## _MAX_TEXTURE_SIDE.
static func _uncompressed(image: Image, format: Image.Format) -> void:
	if image.is_compressed():
		image.decompress()
	image.clear_mipmaps()
	image.convert(format)
	if image.get_width() % 4 != 0 or image.get_height() % 4 != 0:
		image.resize_to_po2()
	while image.get_width() > _MAX_TEXTURE_SIDE or image.get_height() > _MAX_TEXTURE_SIDE:
		image.shrink_x2()


## The material's normal map as the game's: x, y in r, g with y pointing DOWN the image (the game's bitangent runs
## along +v; Godot's and glTF's normal maps point y up, so it is flipped here), scaled by normal_scale; b the
## roughness each mip level's spread of normals adds (0 at level 0, growing down the levels as in Die Maschine's);
## a 1. BC7 (uncompressed when this Godot cannot compress to it). Null without a normal map.
static func _normal_image(material: Material) -> Image:
	var base := material as BaseMaterial3D
	if base == null or not base.normal_enabled or base.normal_texture == null:
		return null
	var source := base.normal_texture.get_image()
	if source == null or source.is_empty():
		return null
	var key := "normal %s %f" % [_image_key(source), base.normal_scale]
	if _converted.has(key):
		return _converted[key]
	var image := source.duplicate() as Image
	_uncompressed(image, Image.FORMAT_RGBAF)
	var w := image.get_width()
	var h := image.get_height()
	# Level 0 as unit vectors (x, y, z) in the game's convention; the mipmaps then average whole vectors, so a
	# level's vector is shorter the more its normals spread.
	var values := image.get_data().to_float32_array()
	var scale := base.normal_scale
	for p in range(0, values.size(), 4):
		var x := values[p] * 2.0 - 1.0
		var y := values[p + 1] * 2.0 - 1.0
		var n := Vector3(x * scale, -y * scale, sqrt(maxf(1.0 - x * x - y * y, 0.0))).normalized()
		values[p] = n.x
		values[p + 1] = n.y
		values[p + 2] = n.z
		values[p + 3] = 1.0
	image.set_data(w, h, false, Image.FORMAT_RGBAF, values.to_byte_array())
	image.generate_mipmaps(false)
	values = image.get_data().to_float32_array()
	var bytes := PackedByteArray()
	bytes.resize(values.size()) # one byte per float: RGBA8 over the same levels
	for p in range(0, values.size(), 4):
		var m := Vector3(values[p], values[p + 1], values[p + 2])
		var length_squared := m.length_squared()
		var spread := sqrt(clampf(_NORMAL_SPREAD * (1.0 - length_squared), 0.0, 1.0))
		var n := m / sqrt(length_squared) if length_squared > 1e-8 else Vector3(0, 0, 1)
		bytes[p] = clampi(roundi((n.x * 0.5 + 0.5) * 255.0), 0, 255)
		bytes[p + 1] = clampi(roundi((n.y * 0.5 + 0.5) * 255.0), 0, 255)
		bytes[p + 2] = roundi(spread * 255.0)
		bytes[p + 3] = 255
	var out := Image.create_from_data(w, h, true, Image.FORMAT_RGBA8, bytes)
	out.compress(Image.COMPRESS_BPTC)
	if not _DXGI.has(out.get_format()):
		out = Image.create_from_data(w, h, true, Image.FORMAT_RGBA8, bytes)
	_converted[key] = out
	return out


## The material's roughness as the game's roughness map (perceptual roughness, 0 smooth to 1 rough, as Godot's
## and glTF's): its roughness texture's channel times its roughness, BC4; a 4 x 4 swatch of its roughness when it
## has no texture.
static func _roughness_image(material: Material) -> Image:
	var base := material as BaseMaterial3D
	if base == null:
		return null
	return _channel_image(base.roughness_texture, base.roughness_texture_channel, base.roughness)


## The material's metalness as the game's metal map (0 not metal, 1 metal, as Godot's and glTF's metallic): its
## metallic texture's channel times its metallic, BC4; a 4 x 4 swatch of its metallic when it has no texture. Null
## when the material is not metal at all (metallic 0), which keeps it on the game's plainer material.
static func _metal_image(material: Material) -> Image:
	var base := material as BaseMaterial3D
	if base == null or base.metallic <= 0.0:
		return null
	return _channel_image(base.metallic_texture, base.metallic_texture_channel, base.metallic)


## The light a surface gives off, as the game's emissive map (an sRGB colour, BC1): the emission colour times the
## emission texture (Godot's Multiply operator, which glTF imports use) or plus it (Add), or a 4 x 4 swatch of the colour
## without a texture. The strength (emission_energy_multiplier) goes separately. Null when emission is off or black.
static func _emission_image(material: Material) -> Image:
	var base := material as BaseMaterial3D
	if base == null or not base.emission_enabled:
		return null
	var color := base.emission
	if base.emission_texture == null:
		if color.r <= 0.0 and color.g <= 0.0 and color.b <= 0.0:
			return null
		var swatch := Image.create_empty(4, 4, false, Image.FORMAT_RGBA8)
		swatch.fill(Color(color.r, color.g, color.b, 1.0))
		return swatch
	var source := base.emission_texture.get_image()
	if source == null or source.is_empty():
		return null
	var multiply := base.emission_operator == BaseMaterial3D.EMISSION_OP_MULTIPLY
	var key := "emission %s %s %s" % [_image_key(source), color.to_html(false), multiply]
	if _converted.has(key):
		return _converted[key]
	var image := source.duplicate() as Image
	_uncompressed(image, Image.FORMAT_RGBA8)
	# As the tint: on the sRGB bytes (Godot works in linear light; the same up to the sRGB curve's difference from a
	# pure power, and Add clips at white).
	var bytes := image.get_data()
	for p in range(0, bytes.size(), 4):
		for c in 3:
			var value: float = color[c]
			bytes[p + c] = roundi(bytes[p + c] * value) if multiply else mini(bytes[p + c] + roundi(value * 255.0), 255)
		bytes[p + 3] = 255
	image.set_data(image.get_width(), image.get_height(), false, Image.FORMAT_RGBA8, bytes)
	image.generate_mipmaps()
	image.compress(Image.COMPRESS_S3TC, Image.COMPRESS_SOURCE_SRGB)
	_converted[key] = image
	return image


## One channel of a texture times a factor, as a one-channel game map (BC4, R8 when this Godot cannot compress to
## it); a 4 x 4 swatch of the factor when there is no texture. Null when the texture has no pixels.
static func _channel_image(texture: Texture2D, channel: BaseMaterial3D.TextureChannel, factor: float) -> Image:
	if texture == null:
		var swatch := Image.create_empty(4, 4, false, Image.FORMAT_R8)
		swatch.fill(Color(factor, 0, 0))
		return swatch
	var source := texture.get_image()
	if source == null or source.is_empty():
		return null
	var key := "channel %s %d %f" % [_image_key(source), channel, factor]
	if _converted.has(key):
		return _converted[key]
	var image := source.duplicate() as Image
	_uncompressed(image, Image.FORMAT_RGBA8)
	var rgba := image.get_data()
	var bytes := PackedByteArray()
	bytes.resize(rgba.size() / 4)
	for i in bytes.size():
		var value: float
		if channel == BaseMaterial3D.TEXTURE_CHANNEL_GRAYSCALE:
			value = (rgba[i * 4] * 0.299 + rgba[i * 4 + 1] * 0.587 + rgba[i * 4 + 2] * 0.114)
		else:
			value = rgba[i * 4 + channel]
		bytes[i] = clampi(roundi(value * factor), 0, 255)
	var out := Image.create_from_data(image.get_width(), image.get_height(), false, Image.FORMAT_R8, bytes)
	out.generate_mipmaps()
	out.compress(Image.COMPRESS_S3TC) # one channel: BC4 (RGTC_R)
	if not _DXGI.has(out.get_format()):
		out = Image.create_from_data(image.get_width(), image.get_height(), false, Image.FORMAT_R8, bytes)
		out.generate_mipmaps()
	_converted[key] = out
	return out


## The widest sky panorama mapkit writes (Die Maschine's is 8192 x 4096, streamed; mapkit's stays in the zone).
const _MAX_SKY_WIDTH := 4096
## How the game wraps its sky panorama round the horizon: the yaw (degrees, game axes: 0 = +X, 90 = +Y) its left edge
## faces, and whether it runs clockwise seen from above. Godot's left edge faces Godot's -Z (game +X) and runs clockwise.
## The game's, from the sky test (2026-09-29, a sky in Godot's layout): +X showed u = 3/8, +Y u = 1/8, text unmirrored.
const _SKY_LEFT_EDGE_YAW := 135.0
const _SKY_CLOCKWISE := true


## The map's environment: the first WorldEnvironment in the map that has one. Null: none.
static func world_environment(map: Node) -> Environment:
	for node in map.find_children("*", "WorldEnvironment", true, false):
		if (node as WorldEnvironment).environment != null:
			return (node as WorldEnvironment).environment
	return null


## The map's sun: the first DirectionalLight3D in the map that is shown. Null: none.
static func sun_light(map: Node) -> DirectionalLight3D:
	for node in map.find_children("*", "DirectionalLight3D", true, false):
		if (node as DirectionalLight3D).is_visible_in_tree():
			return node as DirectionalLight3D
	return null


## The map's own lighting over Die Maschine's daytime lighting (mkmap-format.md, "lighting"): the sun (sun_light: the
## direction its light travels, its colour, its energy, 1 = as strong as Die Maschine's daytime sun) and the fog of
## world_environment. Empty when the map has neither.
static func _lighting(map: MkMap) -> Dictionary:
	var out := {}
	var sun := sun_light(map)
	if sun != null:
		var travel := (map.global_basis.inverse() * -sun.global_basis.z).normalized()
		var color := sun.light_color.srgb_to_linear()
		out["sun"] = {
			"direction": MkSpace.rounded(MkSpace.dir_to_game(travel), 0.0001),
			"color": [snappedf(color.r, 0.001), snappedf(color.g, 0.001), snappedf(color.b, 0.001)],
			"energy": snappedf(sun.light_energy, 0.001),
		}
	var environment := world_environment(map)
	if environment != null:
		out["fog"] = _fog(map, environment)
	return out


## An environment's fog as the game's world fog. Exponential fog (density per metre) covers half the view every
## ln 2 / density metres; depth fog starts at depth_begin and is taken as half covered midway to depth_end. Height fog
## (fog_height_density above 0) thins by half every ln 2 / height density metres above fog_height; without it the fog
## is the same at every height. The colour is the fog light colour times its energy, as displayed.
static func _fog(map: MkMap, environment: Environment) -> Dictionary:
	if not environment.fog_enabled:
		return {"enabled": false}
	var inches := MkSpace.UNITS_PER_METER
	var start := 0.0
	var halfway := log(2.0) / maxf(environment.fog_density, 0.00001) * inches
	if environment.fog_mode == Environment.FOG_MODE_DEPTH:
		start = environment.fog_depth_begin * inches
		halfway = maxf(environment.fog_depth_end - environment.fog_depth_begin, 0.01) * inches / 2.0
	var height := 0.0
	if environment.fog_height_density > 0.0:
		height = log(2.0) / environment.fog_height_density * inches
	var color := environment.fog_light_color * environment.fog_light_energy
	return {
		"enabled": true,
		"start": snappedf(start, 0.1),
		"halfway": snappedf(halfway, 0.1),
		"base_height": snappedf((environment.fog_height - map.global_position.y) * inches, 0.1),
		"halfway_height": snappedf(height, 0.1),
		"color": [snappedf(color.r, 0.001), snappedf(color.g, 0.001), snappedf(color.b, 0.001)],
		"opacity": 1.0,
	}


## The environment whose sky the map shows: the first WorldEnvironment in the map with a sky background. Null: none.
static func sky_environment(map: Node) -> Environment:
	for node in map.find_children("*", "WorldEnvironment", true, false):
		var environment := (node as WorldEnvironment).environment
		if environment != null and environment.background_mode == Environment.BG_SKY and environment.sky != null \
				and environment.sky.sky_material != null:
			return environment
	return null


## The map's sky as the game draws it: the image (see sky_image) and its energy, the environment's background energy
## times a panorama's own (1 = the pixel values as bright as Die Maschine's sky's). Empty when the map has no sky or it
## cannot be made.
static func _sky(map: MkMap, textures) -> Dictionary:
	var environment := sky_environment(map)
	if environment == null:
		return {}
	var image := sky_image(environment)
	if image == null:
		return {}
	var energy := environment.background_energy_multiplier
	if environment.sky.sky_material is PanoramaSkyMaterial:
		energy *= (environment.sky.sky_material as PanoramaSkyMaterial).energy_multiplier
	var relative := "%s_textures/sky_%s.dds" % [map.map_name, _image_key(image)]
	if textures != null:
		textures[relative] = image
	return {"image": relative, "energy": snappedf(energy, 0.001)}


## A sky as the game's sky image: an equirectangular panorama, 2:1 and a power of two wide (at most _MAX_SKY_WIDTH), one
## level, turned to the game's directions; BC6H when it holds HDR, else sRGB colours in BC7. The panorama of a
## PanoramaSkyMaterial, or any other sky material baked to one (only where Godot draws: not in a headless run). Null when
## there is none or it cannot be compressed.
static func sky_image(environment: Environment) -> Image:
	var material := environment.sky.sky_material
	var source: Image = null
	if material is PanoramaSkyMaterial:
		var panorama := (material as PanoramaSkyMaterial).panorama
		source = panorama.get_image() if panorama != null else null
	elif DisplayServer.get_name() != "headless":
		source = RenderingServer.sky_bake_panorama(environment.sky.get_rid(), 1.0, false,
			Vector2i(_MAX_SKY_WIDTH, _MAX_SKY_WIDTH / 2))
	if source == null or source.is_empty():
		return null
	var key := "sky %s %f %s" % [_image_key(source), _SKY_LEFT_EDGE_YAW, _SKY_CLOCKWISE]
	if _converted.has(key):
		return _converted[key]
	var hdr := _is_hdr(source.get_format())
	var image := source.duplicate() as Image
	if image.is_compressed():
		image.decompress()
	image.clear_mipmaps()
	image.convert(Image.FORMAT_RGBAH if hdr else Image.FORMAT_RGBA8)
	var width := 4
	while width * 2 <= mini(image.get_width(), _MAX_SKY_WIDTH):
		width *= 2
	if image.get_width() != width or image.get_height() != width / 2:
		# Bilinear: a sharper filter overshoots round a bright sun, below zero, which BC6H cannot hold.
		image.resize(width, width / 2, Image.INTERPOLATE_BILINEAR)
	_turn_sky(image)
	image.compress(Image.COMPRESS_BPTC, Image.COMPRESS_SOURCE_GENERIC if hdr else Image.COMPRESS_SOURCE_SRGB)
	if image.get_format() != Image.FORMAT_BPTC_RGBFU and image.get_format() != Image.FORMAT_BPTC_RGBA:
		return null
	_converted[key] = image
	return image


static func _is_hdr(format: Image.Format) -> bool:
	return format in [Image.FORMAT_RF, Image.FORMAT_RGF, Image.FORMAT_RGBF, Image.FORMAT_RGBAF, Image.FORMAT_RH,
		Image.FORMAT_RGH, Image.FORMAT_RGBH, Image.FORMAT_RGBAH, Image.FORMAT_RGBE9995, Image.FORMAT_BPTC_RGBF,
		Image.FORMAT_BPTC_RGBFU]


## Turns a panorama from Godot's layout to the game's (_SKY_LEFT_EDGE_YAW, _SKY_CLOCKWISE), column by column.
static func _turn_sky(image: Image) -> void:
	if is_zero_approx(_SKY_LEFT_EDGE_YAW) and _SKY_CLOCKWISE:
		return
	var w := image.get_width()
	var h := image.get_height()
	var turned := Image.create_empty(w, h, false, image.get_format())
	for x in w:
		var u := (x + 0.5) / w
		var yaw := _SKY_LEFT_EDGE_YAW + (-u if _SKY_CLOCKWISE else u) * 360.0
		var column := int(fposmod(-yaw / 360.0, 1.0) * w) % w
		turned.blit_rect(image, Rect2i(column, 0, 1, h), Vector2i(x, 0))
	image.copy_from(turned)


## A name for an image's file: the same pixels always give the same name, so a texture used twice is written once.
static func _image_key(image: Image) -> String:
	var hashing := HashingContext.new()
	hashing.start(HashingContext.HASH_MD5)
	hashing.update(("%d %d %d" % [image.get_width(), image.get_height(), image.get_format()]).to_utf8_buffer())
	hashing.update(image.get_data())
	return hashing.finish().hex_encode().substr(0, 16)


## The image as a .dds file with a DX10 header: every mip level, level 0 first.
static func _dds(image: Image) -> PackedByteArray:
	var out := PackedByteArray()
	out.resize(148)
	out.encode_u32(0, 0x20534444) # "DDS "
	out.encode_u32(4, 124)
	out.encode_u32(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000) # caps, height, width, pixel format, mip count
	out.encode_u32(12, image.get_height())
	out.encode_u32(16, image.get_width())
	out.encode_u32(28, image.get_mipmap_count() + 1)
	out.encode_u32(76, 32)
	out.encode_u32(80, 0x4) # a FourCC follows
	out.encode_u32(84, 0x30315844) # "DX10"
	out.encode_u32(108, 0x1000 | 0x400000 | 0x8) # texture, mipmap, complex
	out.encode_u32(128, _DXGI[image.get_format()])
	out.encode_u32(132, 3) # a 2D texture
	out.encode_u32(140, 1) # one of it
	out.append_array(image.get_data())
	return out


## node: an MkEntity or MkLight, called by duck typing (untyped on purpose).
static func _entity(map: MkMap, node) -> Dictionary:
	var t := _in_map(map, node)
	var out := {
		"path": str(map.get_path_to(node)),
		"class": node.mk_class(),
		"origin": MkSpace.rounded(MkSpace.to_game(t.origin)),
		"angles": MkSpace.rounded(MkSpace.to_game_angles(t.basis), 0.01),
		"props": node.mk_props(),
	}
	if node.has_method("mk_scalable") and node.mk_scalable():
		# The game scales a model evenly (modelscale): the mean of the node's three scales.
		var scale := t.basis.get_scale()
		out["props"]["scale"] = snappedf((scale.x + scale.y + scale.z) / 3.0, 0.001)
	if node.mk_is_volume():
		var bounds: AABB = node.mk_bounds()
		out["bounds"] = {"min": MkSpace.rounded(bounds.position), "max": MkSpace.rounded(bounds.end)}
	return out
