# mapkit for Godot: the level builder

Build custom Zombies maps for Black Ops Cold War (cw-mod) in the Godot editor. You block out the
map with boxes, place the Zombies objects, walk it in first person to check the scale, and export
the map source (`.mkmap`). cwlink will build that file into the game's map files.

**What works today:** building the layout, checks, the walk-through, export, and **Build**:
cwlink turns the map into files the game loads, laid over Die Maschine (see "Build" below).
**Not yet:** brushes draw in one plain material, and the map is lit and skied as Die Maschine is.

## Setup

1. Get Godot 4 (standard build, not .NET) from godotengine.org. mapkit is tested with 4.7.2.
   Godot needs no installer: unzip it and run it.
2. In the Project Manager, choose **Import** and pick `mapkit/godot/project.godot`.
3. The `mapkit` dock appears on the right. If it doesn't: Project > Project Settings > Plugins,
   enable mapkit.

## Making a map

1. In the mapkit dock, type a name (`zm_` then lowercase letters, digits and `_`) and press
   **New map**. You get a copy of the template: two rooms, a door, and one of each object.
   To see every object and setting at once, open `maps/zm_debug.tscn` (see "The debug map" below).
2. **Geometry:** add `MkBrush` nodes (Add Node, search "Mk") for floors, walls and platforms.
   Size them with the box handles or `size`, and rotate them freely.
   - `game_material`: the game material it uses, by name. Empty uses the default.
   - `solid`: players and zombies collide with it.
   - `rendered`: turn it off for an invisible wall.
   A plain `CSGBox3D` exports too. Other CSG shapes and subtraction don't export yet; Check lists
   what it skips.
   **Your own models** (a room, stairs, a building made in Blender): export them from Blender as
   `.glb`, copy the file into this project, add an `MkMesh` under the map and drag the `.glb` from the
   FileSystem dock onto it (or drag it straight under the map: it then exports as an MkMesh with the defaults,
   solid face by face). A material draws as it looks in Godot: its colour texture (or its colour when it
   has no texture, tinted by its colour), its normal map, its roughness, its metallic, its emission (a glow, as bright
   as its emission energy says) and its transparency (alpha scissor or hash cut it out, alpha blend draws it
   see-through, as glass). The material can come in the `.glb` or be made in Godot: the build exports the
   material the viewport draws, so a `StandardMaterial3D` set as a mesh's Surface Material Override (or its
   Material Override) wins over the `.glb`'s. On a dragged-in model, turn on Editable Children first. Not
   exported: UV1 scale and offset (tile in Blender instead), triplanar mapping, an ORM texture, and shader
   materials (they draw with the default). To use a game material instead, name the
   Blender material after one
   (`mc/mtl_p7_barrier_block_concrete_rusty`, see `brush_materials.txt` in `<game>\cw-mod\mapkit`), or set the
   MkMesh's `game_material` (every surface not named after a game material then uses it). `collision`: `faces`
   (every face solid, for rooms), `hull` (a wrapped shape, fast) or `none`.
   You don't need a `.glb` for simple shapes: a `MeshInstance3D` under an MkMesh with one of Godot's own meshes
   (BoxMesh, SphereMesh, CylinderMesh, ...) exports the same way.
   **Text and signs:** put a `MeshInstance3D` under an MkMesh and give it a `TextMesh` (Inspector > Mesh > New
   TextMesh). Its letters export as real geometry in the material's colour: a sign on a wall, or text floating in
   the air. Set that MkMesh's `collision` to `none`. With `depth` 0 the text is flat and shows from the front only;
   a depth above 0 gives solid letters. Godot's `Label3D` (text that always turns to the camera) does not export. A
   `curve_step` of 2 to 4 keeps small text light: TextMesh writes three vertices per triangle.
   **Sky:** add a `WorldEnvironment` under the map. Give its environment a Sky background whose material is a
   `PanoramaSkyMaterial` with an equirectangular image, twice as wide as high (`.hdr` or `.exr` for an HDR sky,
   `.png` or `.jpg` otherwise). The viewport then shows the sky the game will show. Other sky materials (procedural,
   physical) are baked to a panorama when you export from the editor. The panorama's energy multiplier times the
   environment's background energy sets how bright it is: 1 draws a pixel value as bright as the same value in Die
   Maschine's sky. The sky's rotation is not exported yet.
   **Sun and fog:** add a `DirectionalLight3D` under the map for the sun. It exports the way its light points, its
   colour, and its energy (1 = as strong as Die Maschine's daytime sun). The game has one sun: Check warns about
   any other directional light. The WorldEnvironment's fog exports as the game's fog: its colour times its light
   energy, its density (exponential, or depth mode's begin and end) and its height fog (fog height and height
   density). Fog turned off there means no fog in the game either. Without a sun or a WorldEnvironment, the map
   keeps Die Maschine's daytime lighting.
3. **Zombies objects:** add them under the map and put each one's origin on the floor. The box
   shows its size in the game, and the arrow shows which way it faces.
4. **Walk it:** press F6 (Run Current Scene) to walk the map in first person, starting at the
   first player spawn.
   - Movement: WASD, the mouse to look, Shift to sprint, Space to jump.
   - Esc frees the mouse; click to capture it again.
   - Speed, jump height and eye height match the game.
5. **Check:** lists problems. Red ones stop the export; yellow ones are advice. Click a problem to
   select its node.
6. **Export:** writes `build/<map_name>.mkmap` inside this project.
7. **Build:** exports, then runs cwlink, which writes the game's files into
   `<game folder>/cw-mod/maps/zm_silver/`. Start Die Maschine in cw-mod to play your map. Close the
   game before you build: it keeps the map files open.

## Build

Build lays your map over Die Maschine (`zm_silver`): the game loads your files with Die Maschine's,
which supply the zombies, weapons, sounds and effects, and yours replace its world, its level script
and its entities. What you get in the game today:
- your brushes: solid ones as collision, rendered ones drawn. Die Maschine's buildings are gone;
- your meshes (MkMesh): drawn with your own textures and colours (or the game materials you named), with their
  normal maps, roughness and metallic (texture or value), cut out or see-through where their alpha says, solid as
  their `collision` says. Export writes each texture as a `.dds` next to the `.mkmap` (`build/<map>_textures/`).
  Emission is exported, but no glow shows in the game yet. A see-through surface is always drawn as glass (no
  metal). A model dropped in without an MkMesh exports as one (collision faces);
- your sky: the WorldEnvironment's sky in place of Die Maschine's;
- your sun and fog over Die Maschine's daytime lighting. Everything else in the lighting (exposure, the light your
  meshes get from their surroundings, reflections) is still Die Maschine's, baked for its own buildings;
- a flat floor under the whole map, just below your lowest brush;
- your zones and zombie spawners: zombies rise at the spawners of the zones players are in;
- your barriers: Die Maschine's boarded window (or its concrete wall) where you put them. A barrier spawner's
  zombies tear the boards down and climb in. Players cannot pass: a build puts a player clip over the boards, the
  size of the node's box, which bullets go through. The concrete one does not work yet: it asks for power and
  stays;
- your doors: a player buys one at its box, its model sinks into the floor, and the zones it opens
  come alive. The model is what blocks (its own collision), so pick one that fills the opening;
- the player spawns, perk machines, Mystery Box locations and wall buys where you put them. Your map's
  origin lands on Die Maschine's start area. The perks, wall-buy weapons and box locations are Die
  Maschine's: a build moves them, so it can place only as many of each as Die Maschine has, and only its
  six perks and twelve wall-buy weapons. A perk machine and a box location are solid, with or without power, and
  whether the box is there or not;
- ammo crates, Arsenals (Armor Stations), Wunderfizzes, crafting tables and Pack-a-Punch machines where
  you put them, as many as you like, and the crafting items you turned off turned off. Pack-a-Punch works
  from the first round: no quest, no power. The Armor Stations wait for the power when the map has a power switch
  (turn `needs_power` off to have one work from the start);
- your props: the game's models where you put them, turned and scaled as in the editor (one even scale per
  model). A solid prop blocks with a hull wrapped around its model, so a concave model (a table, an arch) is
  filled in: you can't walk under or through it. Only models from Die Maschine's and the common zones show (the
  model picker lists only those);
- the exfil: Die Maschine's, helicopter flight included, moved and turned to your MkExfil, with your
  hold time and attack area, called at your MkExfilRadio (`radio_live_at_start` makes the radio work from the
  first round, for testing);
- the power: without an MkPowerSwitch everything is powered from the start. With one (Die Maschine's console, used
  from its front), the perk machines (all but Quick Revive in a solo game), the Armor Stations and the doors with
  `needs_power` wait for a player to turn it on; those doors then open by themselves.

The build output says what it placed and what it left out.

Build needs three things, and says which one is missing:
- the **game folder** set at the bottom of the dock;
- `cwlink.exe`, built from the repo (`mapkit\cwlink`), or its path in Editor Settings under
  `mapkit/cwlink`;
- a **zone trace** of Die Maschine: add `"mapkit_trace": ["zm_silver"]` to `cw-mod.json` and start
  Die Maschine once.

Set the **game folder** at the bottom of the dock. Check then warns you if your map's name is
already used by a map the game ships. It starts from `MAPKIT_GAME_DIR` if that is set.

## The objects

| Node | What it is | Settings |
|---|---|---|
| `MkMap` | The root of a map. Everything under it is exported. | `map_name`, `title`, `author` |
| `MkBrush` | Level geometry | `game_material`, `solid`, `rendered` |
| `MkMesh` | Your own model (a `.glb` from Blender, put under it): drawn with its own textures and solid in the game | `collision` (faces, hull, none), `game_material` (empty: the model's own look) |
| `MkPlayerSpawn` | Where a player starts. Place four. | `player` (1 to 4, 0 = any) |
| `MkZone` | An area of the map. Zombies spawn in the zones the players are in. | `zone_name`, `active_at_start`, `size` |
| `MkAmbientRoom` | How an area sounds (its reverb, room tone and gunfire tails) while a player is in it. Cover each indoor space with one; elsewhere the map sounds like Die Maschine's default room. A name Die Maschine's sound bank doesn't have plays the default room too, so pick one of its rooms: the field suggests the six its own areas use (`zm_nacht_bunker`, `zm_nacht_bunker_entrance`, `zm_nacht_bunker_hallways`, `zm_nacht_bunker_medium_room`, `zm_nacht_bunker_small_room`, `zm_nacht_interior`). A room sounds the same all over, except in some of Die Maschine's rooms (`zm_nacht_bunker_hallways` seems to be one): there its baked acoustics change the echo and muffling from point to point, and they follow Die Maschine's walls, not yours. Rooms with your own settings come later. | `room` (a room of Die Maschine's sound bank, by name), `priority` (where rooms overlap, the higher wins), `size` |
| `MkZombieSpawner` | Where zombies come in | `zone`, `kind` (ground, barrier) |
| `MkBarrier` | A boarded window zombies tear down and climb through. Put it on the floor in the middle of the opening, its arrow into the play area. Its box is what stops players in the game (bullets pass): keep the opening inside it. | `zone` (its barrier spawners use it), `kind` (wood, concrete) |
| `MkDoor` | A door or debris that blocks the way until bought. The model blocks and sinks into the floor when bought; the box is where it is bought. | `kind`, `cost`, `opens` (zones, comma-separated), `size`, `model` (the door: a game model with its own collision; empty = Die Maschine's metal door), `needs_power` (opens by itself when the power comes on) |
| `MkWallBuy` | A weapon bought off a wall | `weapon` (Pick shows each weapon's chalk), `cost` |
| `MkPerkMachine` | A perk machine | `perk` |
| `MkMysteryBox` | One place the Mystery Box can be. It blocks like the box even while the box is elsewhere and only the bear shows, so give it a base of your own to stand on if you like. | `start_here` |
| `MkPackAPunch` | Pack-a-Punch: tiers 1 to 3 and ammo mods. Place as many as you like; it works from the first round. | none |
| `MkPowerSwitch` | The power switch, used from its front (the arrow). Without one, the power is on from the start; with one, the perks, Armor Stations and power doors wait for it. | none |
| `MkArsenal` | The Arsenal: armor and weapon rarity upgrades. The same machine as `MkArmorStation`. | `needs_power` |
| `MkCraftingTable` | The Crafting Table: equipment, support items and self-revives for salvage. Place as many as you like. | `items`: what it sells (tick boxes). The game keeps one list per map, so with several tables an item sells only if every table has it. |
| `MkAmmoCache` | An ammo crate. Place as many as you like. | none |
| `MkArmorStation` | The Armor Station, which the game calls the Arsenal: armor and weapon rarity upgrades. Place as many as you like. | `needs_power` (with a power switch: works once the power is on) |
| `MkWunderfizz` | Der Wunderfizz, which sells any perk. Place as many as you like. | none |
| `MkExfil` | Where the exfil helicopter lands; the arrow is the way it faces once landed. One per map. | `hold_seconds`, `attack_radius`, `attack_height`, `zones` (players must reach one; empty = the zones the landing point is in), `radio_live_at_start` (testing) |
| `MkExfilRadio` | The radio players call the exfil at, from round 10, every 5th round, for 2 minutes. One per map. | none |
| `MkProp` | A game model, by name or hash (`#1234...`). Turn it any way; scale it evenly. | `model`, `solid` |
| `MkLight` | A light for the Godot preview, and later for the game's lighting | color, energy, range |

Zones, ambient rooms and doors are volumes. Drag the handles on their faces to resize them; hold Shift to turn
off snapping.

**Game names** (weapons, models, materials) have to be typed in for now. Browsing the game's own
assets comes later.

## The debug map

`maps/zm_debug.tscn` (map name `zm_debug`) holds one of everything the editor can place, each with a label
that says what it is and what to expect. Open it to see how an object is set up, or Build it to test them all in
the game:
- **Start room:** four player spawns; all ten perk machines (a build places Die Maschine's six and skips Tombstone,
  Mule Kick, PHD Flopper and Death Perception); Die Maschine's twelve wall buys; the Mystery Box's start; the power
  switch; an ammo crate; two zombie spawners.
- **Machines hall** (a door, 500): Pack-a-Punch, the Arsenal (works without power), the Armor Station (waits for
  the power), Der Wunderfizz, the crafting table, a box location, and a wood barrier with a barrier spawner outside.
- **Materials gallery** (debris that opens when the power comes on): rows of samples made in Godot alone, no `.glb`:
  - roughness from 0 to 1, and gold from smooth to rough;
  - colours on Godot's primitive shapes;
  - texture maps: colour, tint, normal, roughness and metal;
  - cut-outs, glass and glow;
  - a surface named after a game material, and an MkMesh `game_material`;
  - a shader material (the default in the game), text as an object, and a mirrored and a squashed mesh.

  It also has a concrete barrier.
- **Yard** (a door with its own model, 1000):
  - the exfil, its radio live from round 1;
  - props, among them one not solid, one scaled and one tilted;
  - brushes: game materials, a ramp and stairs to a platform, an invisible wall, a walk-through wall, a plain
    CSGBox3D, and a turned, stretched brush;
  - one ring with each MkMesh collision (faces, hull, none);
  - a shed and a pavilion.
- **Sound:** a room for each of Die Maschine's six named rooms. The porch overlaps the hall with priority 2. The
  open yard plays the default room.
- **Sky, sun and fog:** the sky's horizon is marked with the game's axes (+X, -Y, -X, +Y), so you can check which
  way the sky faces in the game.

Its textures (`maps/zm_debug/`) are generated, not taken from the game.

## Game models

The Zombies objects are drawn with the game's own models, read from your install, turned the way the
game will turn them: perk machines, the Mystery Box, Pack-a-Punch, the Arsenal, the crafting table, the
ammo crate, the Armor Station, Der Wunderfizz, the exfil radio, the power switch, wall buys (their chalk),
and doors and props (the model you pick). The others (spawners, barriers, zones, the exfil's landing area)
stay boxes. It needs:
- `mkasset.exe`, built from the repo (`mapkit\mkasset`), or its path in Editor Settings under
  `mapkit/mkasset`;
- the **game folder** set in the dock;
- zone traces of Die Maschine and its common zone: `"mapkit_trace": ["zm_silver", "zm_common"]` in
  `cw-mod.json`, then start Die Maschine once. The perk machines come from `zm_common`.

The editor runs mkasset in the background while it is open. The first time a model shows takes a
moment (mkasset needs about 2 s to start); after that the model loads from a local cache in Godot's
user data folder (`user://game_models`, outside this project), in the editor and in the walk-through.
Why a model does not show is in the Output panel. PHD Flopper and Death Perception stay boxes: their
machines are in other maps' zones. Tombstone and Mule Kick show their machines, but a build skips them
(Die Maschine places none to move).

**Picking a model.** A model field (`MkProp.model`, `MkDoor.model`, `MkWallBuy.weapon`) has a picture of
the model and a **Pick** button. Pick opens a window with every model mkasset reads (about 4700), by
category (Doors & gates, Perk machines, Wall buys, Foliage & rocks, ...), with a search box over names
and hashes. Each model's picture is drawn the first time it scrolls into view and cached in
`user://game_models/thumbnails`; the model itself is only kept when you use it. Double-click a model or
press **Use this model**; Ctrl+Z undoes it. The pictures are gray until textures come (the plan's step 5).

The names come from `addons/mapkit/game_model_names.txt`: names and hashes only (no game data), about
93% of the models; the rest show as their hash. `mapkit/mkasset/model_names.py` rewrites it from an
`mkasset catalog`, looking each hash up with ACTS (`acts -t lookup`).

## Units

Godot works in meters with Y up. The game uses inches with Z up and X forward. Export converts
between them: 1 m = 39.37 game units.

For scale: a player is 1.83 m tall (72 units), a perk machine's box is about 2.3 m, and a new
door is 2.5 m wide. The brush grid draws a line every 16 units and a heavier one every 128.

## What stays out of a map

A map holds only your own work. Game models, materials and sounds are referenced by name, and the
player's own install supplies them. Never put files taken from the game into this project or into
a map you share.
