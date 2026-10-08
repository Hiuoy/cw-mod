# The .mkmap map source, format 1

`.mkmap` is the map source cwlink builds a zone set from. The Godot plugin writes it
(`mapkit/godot`, `MkExport`). Any other editor can write it too, which keeps the editor
replaceable. The file is JSON in UTF-8 and holds only the creator's own work: game assets appear
by name, never as data.

**Space:** game units (1 unit = 1 inch), right-handed, X forward, Y left, Z up, like the engine.
Positions are relative to the map's origin. **Angles** are `[pitch, yaw, roll]` in degrees, with
pitch positive when looking down (the engine's convention). Numbers are rounded to 0.001 (angles to
0.01).

```json
{
	"mkmap": 1,
	"generator": "mapkit-godot 0.1",
	"units": "inch",
	"axes": "x forward, y left, z up",
	"map": { "name": "zm_template", "title": "Template", "author": "mapkit", "mode": "zm" },
	"brushes": [ ... ],
	"entities": [ ... ]
}
```

| Key | Meaning |
|---|---|
| `mkmap` | Format version. A reader refuses any version it doesn't know. |
| `generator` | The tool that wrote the file. Informational. |
| `map.name` | Zone name: `zm_` then `[a-z0-9_]`, at most 48 characters. It becomes `<name>.ff` and its variants. |
| `map.title` | The name players see. |
| `map.mode` | `zm`, the only mode so far. |

## brushes

Convex blocks of level geometry. Today every brush is a box. It may be rotated, scaled or sheared,
so it is stored as a center plus three half-axis vectors. The eight corners are
`center ± half_axes[0] ± half_axes[1] ± half_axes[2]`, and the six faces are planes through
`center ± half_axes[i]` with normal `±half_axes[i]`.

```json
{
	"path": "Geometry/Floor",
	"center": [255.906, 0.0, -9.843],
	"half_axes": [[0.0, -236.22, 0.0], [0.0, 0.0, 9.843], [-511.811, 0.0, 0.0]],
	"material": "",
	"solid": true,
	"rendered": true
}
```

| Key | Meaning |
|---|---|
| `path` | Where it sits in the editor's tree. Identifies the brush in messages; not used by the game. |
| `material` | Game material by name, resolved from the player's install at build time. `""` = the default. |
| `solid` | Collides with players and zombies. |
| `rendered` | Drawn. `solid` without `rendered` is a clip (an invisible wall). |

## meshes

The creator's own geometry (Godot's `MkMesh`: a model made in Blender or another tool). Optional. Each mesh is a
node holding one or more parts (the meshes under it), each made of triangle surfaces, **in map space**: the node's
transform is already applied, in game units and axes. A model dropped into the map under no `MkMesh` exports as one
too, with the defaults (`collision` `faces`, `game_material` `""`), its path the node right under the map.

```json
{
	"path": "Room",
	"collision": "faces",
	"game_material": "mc/mtl_p7_concrete_pillar_damage",
	"parts": [ {
		"path": "Room/test_room/Room",
		"surfaces": [ {
			"material": "mc/mtl_p7_barrier_block_concrete_rusty",
			"color_texture": "zm_template_textures/2f62d0d535d5f3ce.dds",
			"normal_texture": "zm_template_textures/9193c8a9ecb21f28.dds",
			"roughness_texture": "zm_template_textures/b783e85ee367dff3.dds",
			"metal_texture": "zm_template_textures/cf3c0d3936bd2032.dds",
			"alpha": "clip",
			"vertices": [[784.0, 40.0, 0.0], ...],
			"normals": [[1.0, 0.0, 0.0], ...],
			"uvs": [[0.3125, -1.25], ...],
			"triangles": [[0, 1, 2], ...]
		} ]
	} ]
}
```

| Key | Meaning |
|---|---|
| `collision` | `faces`: every triangle is solid (a slab 4 units deep behind its front face), exact for concave shapes. `hull`: one convex shape wrapped around each part. `none`: walk-through |
| `game_material` | The game material for surfaces whose `material` names none, in place of their own look. `""` = their own look (`color_texture`), else the default |
| `material` | The editor material's name. When it is a game material the base map has, the surface draws with it |
| `color_texture` | The editor material's own look: a `.dds` (DX10 header; BC1, BC3, BC7 or RGBA8, with its mip levels) path relative to the `.mkmap`. The albedo texture times the albedo colour (the tint is baked in), or a small swatch of the albedo colour. Its alpha is the opacity (see `alpha`). Drawn when `material` is no game material and the mesh has no `game_material`. Optional |
| `normal_texture` | With `color_texture`: its normal map, a `.dds` (BC7 or RGBA8) in the game's convention: x, y in r, g with **y pointing down the image** (+v; glTF's and Godot's point up, so an editor flips green), b the roughness each mip level's spread of normals adds (0 at level 0, growing down the levels), a 1. Optional: a flat normal map |
| `roughness_texture` | With `color_texture`: its perceptual roughness (0 smooth, 1 rough, as glTF's) in r, a `.dds` (BC4 or R8), or a 4 x 4 swatch of one value. Optional: fully rough |
| `metal_texture` | With `color_texture`: its metalness (0 not metal, 1 metal, as glTF's metallic) in r, a `.dds` (BC4 or R8), or a 4 x 4 swatch of one value. The surface then draws with a metal material (and counts as metal for bullet impacts). Optional: not metal |
| `alpha` | With `color_texture`: how its alpha draws. `opaque` (ignored), `clip` (cut out; the editor has already made alpha 0 or 1 at its own threshold), `blend` (see-through, drawn as glass; a metal map is ignored). Optional: `opaque` |
| `emission_texture` | With `color_texture`: the light the surface gives off, a `.dds` colour like `color_texture` (the editor's emission colour times its emission texture, or a swatch of the colour). The surface then draws with a glowing material; a metal map is ignored, and a `clip` or `blend` surface does not glow. Optional: no glow |
| `emission_energy` | With `emission_texture`: how strong the glow is (Godot's emission energy): 1 = about as bright as Die Maschine's lit signs, and the strength grows with it. Optional: 1 |
| `normals`, `uvs` | Empty, or one per vertex. Missing normals come from the faces |
| `triangles` | Indices into the surface's vertices, **clockwise seen from the front** (the game's winding; Godot's too) |

## sky

The map's own sky (in Godot, the sky of a `WorldEnvironment` in the map). Optional: without it the map keeps its base
map's sky.

```json
"sky": { "image": "zm_template_textures/sky_13e836a24310ff26.dds", "energy": 1.0 }
```

| Key | Meaning |
|---|---|
| `image` | An equirectangular panorama, a `.dds` path relative to the `.mkmap`: twice as wide as high, one level, the top row straight up and the middle row the horizon, **already turned to the game's directions**: the column at u faces yaw 135° − 360° × u, so u runs clockwise seen from above and +X is at u = 3/8 (the editor turns it; Godot's own panorama has +X at u = 0). BC6H for a sky with HDR, else sRGB colours (BC7) |
| `energy` | How bright: 1 = a pixel value is as bright as the same value in the base map's own sky. Die Maschine's sky material multiplies its panorama by 90.5; cwlink uses 90.5 × `energy`. Optional: 1 |

## lighting

The map's own sun and fog, written over the base map's daytime lighting (in Godot, a DirectionalLight3D and a
WorldEnvironment's fog). Optional, and so is each part: what the map does not set stays the base map's. With either
part, cwlink copies the base map's whole lighting asset (Die Maschine's is 7 MB) into the map's zones.

```json
"lighting": {
	"sun": { "direction": [-0.9397, 0.0, -0.342], "color": [1.0, 0.523, 0.263], "energy": 1.0 },
	"fog": { "enabled": true, "start": 0.0, "halfway": 1500.0, "base_height": 0.0, "halfway_height": 300.0,
		"color": [0.75, 0.8, 0.9], "opacity": 1.0 }
}
```

| Key | Meaning |
|---|---|
| `sun.direction` | The direction the sun's light travels, a unit vector in the map's axes (so the sun sits the opposite way) |
| `sun.color` | Its colour, linear 0..1 |
| `sun.energy` | How strong: 1 = Die Maschine's daytime sun. Optional: 1 |
| `fog.enabled` | `false`: no fog at all (the base map's is removed too) |
| `fog.start` | Where the fog starts, in units from the eye |
| `fog.halfway` | How far past `start` the fog covers half the view |
| `fog.base_height` | The height (map space) the fog is thickest at |
| `fog.halfway_height` | How far above `base_height` it thins by half. 0: the same at every height |
| `fog.color` | Its colour as displayed, 0..1 (cwlink stores it × 255, as the game's) |
| `fog.opacity` | The most it covers, 0..1. Optional: 1 |

## entities

Zombies gameplay objects. `origin` is where the object stands (its floor point). cwlink maps each
`class` to the game's own entities; the class names below belong to mapkit, not to the engine. What
each becomes in the game is in `docs/mapkit-roadmap.md` ("Map entities").

```json
{
	"path": "Gameplay/Juggernog",
	"class": "perk_machine",
	"origin": [-137.795, -212.598, 0.0],
	"angles": [0.0, 90.0, 0.0],
	"props": { "perk": "juggernog" }
}
```

Volumes (`zone`, `ambient_room`, `door`) also carry `bounds`: `{ "min": [...], "max": [...] }`. It is in the
entity's own axes, relative to `origin`, before `angles` are applied.

| class | props |
|---|---|
| `player_spawn` | `player`: 1 to 4, 0 = any |
| `zombie_spawner` | `zone`: a zone's `zone_name`. `kind`: `ground` (climbs out of the ground and hunts the players) or `barrier` (climbs out of the ground, then goes to the nearest `barrier` of its zone, tears the boards down and climbs in; with no barrier in its zone it acts as `ground`) |
| `zone` (volume) | `zone_name`, `active_at_start` |
| `ambient_room` (volume) | How the area sounds while a player is in it: reverb, room tone, gunfire tails. `room`: a room of the base map's sound bank, by name (`cwlink build` lists the ones the base map's own areas use, and says when the bank has no room of that name; such a room plays the default room). `priority`: where rooms overlap, the higher wins (missing = 1). Outside every room the map sounds like the base map's default room |
| `door` (volume) | `kind`: `door` or `debris`. `cost`. `opens`: an array of zone names. `model`: the door, a game model whose own collision blocks (game name or `#hash`; empty = `p9_zm_ndu_door_metal_gray_rusted`). `needs_power`: opens by itself when the power comes on, instead of being bought (only with a `power_switch`; missing = false) |
| `barrier` | A boarded window, `origin` on the floor in the middle of the opening, yaw pointing into the play area. `zone`: the zone whose barrier spawners use it. `kind`: `wood` (Die Maschine's boarded window) or `concrete` (its wall of concrete chunks; missing = `wood`) |
| `wall_buy` | `weapon` (game name), `cost` |
| `perk_machine` | `perk`: `juggernog`, `speed_cola`, `quick_revive`, `stamin_up`, `deadshot_dealer`, `elemental_pop`, `tombstone_soda`, `mule_kick`, `phd_flopper`, `death_perception` |
| `mystery_box` | `start_here` |
| `pack_a_punch`, `ammo_cache`, `wunderfizz`, `exfil_radio` | none |
| `arsenal`, `armor_station` | The same machine (the game calls it the Arsenal). `needs_power`: with a `power_switch`, it works only once the power is on (missing = true); without one it always works |
| `power_switch` | none. Without one the power is on from the start; with one, the perk machines (all but Quick Revive in a solo game), the Armor Stations and the doors with `needs_power` wait for it. Its front is its yaw |
| `crafting_table` | `items`: an array of what it sells, of `frag`, `semtex`, `molotov`, `hatchet`, `c4`, `decoy`, `stun`, `monkey`, `stimshot`, `self_revive`, `turret`, `chopper_gunner`, `death_machine`, `flamethrower`, `bow`, `napalm`, `pineapple_gun`, `hand_cannon`, `rcxd` (missing = all) |
| `exfil` | where the helicopter lands, facing its yaw. `hold_seconds`, `attack_radius`, `attack_height` (units), `zones`: an array of zone names players must reach (empty = the zones holding `origin`). `radio_live_at_start`: for testing, the radio works from the start, not only after rounds 10, 15, 20... (missing = false) |
| `prop` | `model` (game name or `#hash`), `solid`, `scale` (even, 1 = the model's size; missing = 1). All three `angles` apply |
| `light` | `color` `[r, g, b]` (0 to 1), `intensity`, `radius` (units) |

## Rules a reader enforces

These are the rules the Godot plugin's Check applies before it exports:
- `map.name` is valid and isn't the name of a shipped map;
- at least one brush, one `player_spawn` and one `zombie_spawner`;
- every `zone` a spawner or barrier names exists, and so does every zone a door opens;
- `zone_name` values are unique;
- every `ambient_room` names a `room`;
- `wall_buy.weapon` and `prop.model` are set.
