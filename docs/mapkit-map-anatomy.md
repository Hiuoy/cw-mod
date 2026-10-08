# What a map is made of, and how mapkit builds one (2026-09-27)

This is the inventory of a retail Zombies map, taken from Die Maschine (`zm_silver`). It lists every kind
of thing the map holds, which file holds it, and where mapkit stands on it. It is the checklist for mapkit's
own map constructor: everything here is either written by mapkit already or still borrowed from Die
Maschine, and each borrowed row is something to reverse.

Sections 1 and 2 describe how a custom map is built today. Sections 3 to 8 are the inventory. The milestones
(P0 to P6) and their status are in [ROADMAP.md](ROADMAP.md#5-custom-maps-mapkit), each step's findings in
[mapkit-plan.md](mapkit-plan.md), and the older findings in [mapkit-roadmap.md](mapkit-roadmap.md).

How the numbers were taken: `ffinfo <zone> --assets` for the asset tables, `ffinfo zm_silver --trace` for sizes
and for which types mapkit's loaders decode, and `ffinfo zm_silver --entities` for the entity lists. The split
into "own" and "reference" comes from each asset's root: a root that holds only a name hash with the top bit
set is a by-name link to an asset in another zone. The hash sits where the engine's name getter reads it: +0 for
most types, but not all. The xanim, localizeentry, weapontunables and flametable rows were recounted on 2026-09-30
with the right offsets (zonekit `XAssetNameOffset`). This file lists names and counts only, never game data.

## 1. How a custom map is made today, from A to Z

A custom map is **laid over Die Maschine**. The lobby can only start real maps, so the game believes it is
loading Die Maschine. Two small zone files and a set of scripts, built from the creator's Godot map, replace
Die Maschine's level design as it loads. Die Maschine still provides everything mapkit cannot make yet.

### 1.1 Godot: make the map

- The creator places Mk nodes:
  - brushes for the geometry;
  - zones, zombie spawners and player spawns;
  - perks, Mystery Box spots, wall buys, the devices and the exfil.
- The game models shown in the editor are read from the creator's own install by `mkasset` and cached
  locally. They are only a preview.
- **Check** lists problems. **Export** writes `build/<name>.mkmap`: JSON with positions, names and settings
  only ([mkmap-format.md](../mapkit/mkmap-format.md)). It holds no game data.

### 1.2 Build: the dock runs `cwlink build zm_silver <name>.mkmap`

cwlink reads `zone/zm_silver.ff` from the creator's install. It also reads the **zone trace**, a one-time
recording of the game loading Die Maschine that says where each asset starts in that file. Then it:

1. **Places the map.** The map's (0,0,0) lands on the middle of Die Maschine's player spawns, at
   (1043, -67, 65), because zombies still walk Die Maschine's navmesh (section 2).
2. **Collision.** Each solid brush becomes a convex hull in the clip map's world tree. Die Maschine's
   terrain is flattened into a safety floor just under the lowest brush.
3. **Visible geometry.** All rendered brushes become one model of mapkit's own, `mapkit_<name>`, with its
   own mesh and skeleton. It is placed by one script_model, and a bgcache entry is written so the engine finds
   it. Brush materials must exist in Die Maschine's zone; the default is cinder block.
4. **World.** It writes a copy of Die Maschine's gfx_map (the drawn world) with the buildings, their
   far-distance versions, the terrain and the decals cut out. It adds an empty streamerworld and an empty
   districts asset, so Die Maschine's streamed buildings and their collision do not come back.
5. **Entities.** It builds a new entity list:
   - worldspawn and the AI spawner templates;
   - the player spawns;
   - the creator's objects: Die Maschine's structs moved to the creator's positions, plus copies for extras;
   - their content-manager parents and the helicopter paths;
   - generated respawn points, zombie rise points and minimap corners.

   It also builds a new trigger list with one volume per zone. Die Maschine's other entities are left out.
6. **Scripts.** Our level script (`mapkit/level/zm_silver/zm_silver.gsc` and `.csc`) and the generated
   `zm_silver_zones.gsc` are compiled with ACTS. The generated file holds the zone graph and `settings()` (exfil,
   crafting items).
7. **Output.** It writes `<game>/cw-mod/maps/<id>/`: `ww_1080_<id>.ff`, `ww_4k_<id>.ff`, `scripts/` and
   `map.json`. Each zone is read back and checked before it is written.

### 1.3 In the game

- cw-mod reads `map.json` and lists the map under Zombies > Private > **CUSTOM MAPS**. Picking it starts Die
  Maschine.
- While the match loads, the cw-mod DLL adds the map's two zones right after `zm_silver`. They load without
  a signature check (only zones under `cw-mod/maps` get this). The game loads the one that matches the
  texture setting.
- Our zones win over Die Maschine's (zone priority 27 against 7), so each asset with the same name replaces
  Die Maschine's:
  - clip map;
  - gfx_map;
  - streamerworld;
  - districts;
  - entity list;
  - trigger list.
- The GSC loader serves our scripts under Die Maschine's script names, only while that map is picked. The
  level script starts Zombies, runs the zone manager on the map's zones, powers the perks and applies
  `settings()`.
- Die Maschine picked from CORE stays stock.

## 2. What we still take from Die Maschine

**Loaded and used by name, never copied.** Die Maschine's whole zone still loads; that is the reason the map
loads under its name:
- zombies and AI types, weapons, effects, sounds, materials and shaders;
- its Mystery Box weapon list (`zm_silver_fixup`), AI, BGB and vehicle scripts;
- the weapon table, announcer and effects setup, which our level script copies from Die Maschine's.

**Used live, with nothing of ours in its place yet:**

| What | Effect on a custom map |
|---|---|
| **Lighting** | Sky, sun, colour grading and model lighting are Die Maschine's, baked into its lighting asset. |
| **Minimap picture** | Die Maschine's. mapkit only moves its corners. |

Since this was written, the navmesh became the map's own (P5 step 1, 2026-09-29). A map can set its own sky, sun
and fog (P4, 2026-09-29), and Die Maschine's placed ambient effects are gone (P6 step 2a, 2026-09-30: the map's
own static level FX list is empty). The 2026-09-30 census confirms the game uses both of the map's own assets.

**Copied from the creator's install at build time.** This is why built zones must never be shared:
- Die Maschine's gfx_map, minus the parts cut above;
- its clip map structure;
- the contents of its own `ww_1080_` and `ww_4k_` zones (one settings asset each), carried over unchanged;
- template entities: worldspawn, the AI spawners, and one of each struct mapkit clones.

**Its gameplay objects, moved to the creator's positions.** This is where the current limits come from:
- perks: only Die Maschine's six machines;
- wall buys: only its twelve weapons;
- box spots: only as many as it has;
- the devices, copied as needed;
- the exfil, including its helicopter flight and its zombie waves (`exfil_silver_*`).

**Gone for good:** its buildings (drawn and solid), terrain, decals, its entity and trigger lists, its level
script, quests, voice lines, intro and zones.

**What ends each dependency:**

| Step | Replaces |
|---|---|
| P2 / P3 | Die Maschine's placement and meshes (props and the creator's own models) |
| P4 | Its lighting and materials |
| P5 | Its navmesh |
| P6 | Die Maschine as the host: the map loads under its own name, with its own asset set copied from each player's install |

## 3. The files of a map

Everything lives in `<game>/zone/`. A map is the zone named after it, **expanded into variants** by the
engine (`DB_ExpandZoneVariants`): language, region, texture tier and shaders. Every `.ff` has a `.fd` beside
it: the `.ff` is the launch build, and the `.fd` is a binary patch (VCDIFF) that brings it to the current
build. The game always loads the two together.

| File | On disk (.ff + .fd) | What it holds |
|---|---|---|
| `zm_silver.ff` | 49.6 + 22.2 MB (181 MB unpacked) | **The level zone.** 182,002 assets and 6,372 script strings: the world, the entities, the level scripts, and every model, texture, animation, effect, sound and weapon the map uses (section 4). |
| `techset_zm_silver.ff` | 21.4 + 19.1 MB (207 MB unpacked) | 562 techsets: the compiled shaders of Die Maschine's materials. |
| `en_zm_silver.ff` (also `es_ fr_ ge_ po_ pp_ tc_`) | 86 KB + 285 KB | One per language: 1,981 localized strings, 442 sound assets with their stream keys, and one sound bank (the spoken lines). |
| `ww_zm_silver.ff` | 586 + 50 KB | The region variant (`ww` = worldwide; the engine also knows jp, sa, kr15, cn and esports). It holds 18 models, 454 images, 191 materials, 69 techsets and 8 effects. These are probably the parts other regions swap out; not examined. |
| `1080_zm_silver.ff`, `4k_zm_silver.ff` | 0.5 / 1.5 MB | The texture tier: 145 images each, loaded by texture quality. |
| `ww_1080_zm_silver.ff`, `ww_4k_zm_silver.ff` | 1.6 KB | One settings asset (keyvaluepairs) each. **mapkit's slot:** a custom map ships its own `ww_1080_<id>` and `ww_4k_<id>`, which load after the level zone and win over it. |
| `zm.xpak` + `zm-000NN.xsub` (9 files, 1.7 GB) | shared | **Streamed data (KAPI packages).** The `.xpak` is the index and the `.xsub` files hold the data: large texture mips, most mesh LODs, the streamer's world cells and other stream-key payloads. They are shared by all launch Zombies content (later maps add `zm_postship`, `zm_sink`, and each language has its own `<lang>_zm.xpak`). Data is found by key across all packages, not by file. |

**Loaded with every Zombies map, not part of Die Maschine:**
- `zm_common.ff` (20.7 MB on disk; 223,067 assets, 10,014 script strings) and its 13 variants. It holds:
  - all of Zombies' shared scripts: 829 script files, including the perks, the Mystery Box, the devices, the
    exfil and the round logic;
  - 1,867 Lua UI files and 196 script links;
  - the machines and devices, the zombies' shared bodies, weapons and attachments.
- The engine's global zones (core), for every mode.

**How they load:** the level load asks for `[zm_common, zm_silver]`, and the engine expands each into its
variants. Each asset links by name; when two loaded zones hold an asset of the same name, the zone with the
higher priority wins (`ww_1080_`/`ww_4k_` = 27, the level zone = 7). That swap is how mapkit's zones replace
Die Maschine's world.

## 4. Everything in `zm_silver.ff`, by role

Columns:
- **Own / ref:** assets whose data is in Die Maschine's zone / by-name links to another zone's asset
  (usually zm_common or core).
- **Size:** the own assets' share of the unpacked zone.

The **mapkit** column says what mapkit does with each type:
- **Ours:** mapkit writes its own.
- **Trimmed:** mapkit writes Die Maschine's with parts cut out, or an empty one.
- **Read:** mapkit's loader decodes every one of them (checked on the whole zone), but mapkit writes none.
- **Borrowed:** used from Die Maschine as it is.
- **Not reversed:** mapkit cannot read it yet.

"By the name" means the meaning is guessed from the type's name and was not checked.

### 4.1 World: geometry, collision, streaming

| Type (id) | Own / ref | Size | What it is | mapkit |
|---|---|---|---|---|
| gfx_map (0x1B) | 1 | 3.06 MB | The drawn world. It holds:<br>- model groups: placed buildings and props, with per-instance transforms;<br>- group LODs; terrain drawing; 3,061 projected decals;<br>- draw materials; the LUT colour grade;<br>- links to the lighting and streamerworld assets. | **Trimmed:** model groups, group LODs, terrain and decals cut; the rest is Die Maschine's |
| clip_map (0x18) | 1 | 3.00 MB | Collision. It holds:<br>- the world tree and brush models (convex hulls);<br>- terrain heightfield tiles;<br>- dynamic entities plus 500 runtime debris slots;<br>- surfaces;<br>- cells, streamed through districts;<br>- zone-local script strings. | **Trimmed:** brush hulls in tree 0, terrain flattened into a floor |
| streamerworld (0xAC) | 1 | 0.67 MB | The streamer's model list and cell records | **Ours** (empty) |
| districts (0xAB) | 1 | 5 KB | The streamer's cells: 10 stream keys each (slot 9 = the cell's collision) | **Ours** (empty) |
| terraingfx (0xB1) | 1 | 1.08 MB | Terrain drawing plus map-wide height textures (the +224 block) | **Copied without its tiles** since P6 step 2a (2026-09-30): the +224 block stays (an empty one crashed every render thread on texture slot 105) |
| grouplodmodel (0xBB) | 11 | small | Merged far-distance versions of building groups (`zm_silver_bunker_01`, ...) | Read; unused once the gfx_map cut drops them |
| game_map (0x1A) | 1 | 3.37 MB | The AI path nodes (176-byte nodes, each bound to a navmesh face at load); the one asset of its pool, never looked up by name | **Copied** (P6 step 2a). Their `node_*` entities are dropped, and mapkit zones use volumes instead |
| com_map (0x19) | 1 | 0.65 MB | The primary lights: 948 lights of 688 bytes with cookie images, the same list as the lighting asset's (`Load_ComWorld`; found by name, `Com_FindComWorld_cand`). The client game copies them into its own array when a level starts (`Com_CopyPrimaryLightsToCG_cand`) | **Copied** (P6 step 2a), with every light black since 2026-10-05 (passed in the game that day): they are Die Maschine's lamps at Die Maschine's places |
| cpu_occlusion_data (0xA9) | 1 | 0.19 MB | Umbra occlusion-culling data (no pointers; two script strings) | **Copied** (P6 step 2a) |
| glasses (0x43) | 1 | small | Breakable glass (by the name); Die Maschine's holds none | **Copied** (P6 step 2a) |
| streamkey (0xB8) | 8,494 / 5,700 | 8.5 MB | Handles to data in the `.xsub` packages (cells, mesh LODs, texture mips, sounds) | Read (and the package lookup works: `ffinfo --mesh`); the navvolume's 2 are **copied** (P6 step 2b-1). The lighting's 8 streamed-texture keys (types 2 and 3 at +54) stay DM's while DM loads: the lighting sets their owner (+40) while it loads, and the override swap that follows the load lost it (2b-1 test 1 crash). Without DM's zones they are copied (2b-2); since 2026-10-05 the copies of the 6 baked ones name the empty shadow tree's data (their +8 and +48) |
| entitylist (0x8E) | 1 | 1.26 MB | The map's 2,380 entities (section 5) | **Ours** |
| triggerlist (0x80) | 1 | 0.19 MB | 327 trigger entities plus their shapes (323 models, 357 hulls, 576 slabs) | **Ours** |
| bgcache (0x6D) | 1 | 0.63 MB | 27,551 names that scripts and entities may use, 12,162 of them models. A model an entity names must be listed here, or it never draws. | Borrowed, plus **ours** for mapkit's models |
| keyvaluepairs (0x4B) | 1 | small | Zone settings (every zone has one) | Read, borrowed |

### 4.2 Look: models, materials, textures, shaders, lighting

| Type (id) | Own / ref | Size | What it is | mapkit |
|---|---|---|---|---|
| xmodel (0x06) | 3,254 / 10,869 | 1.9 MB | Models: header, LOD table, bounds, materials, skeleton, collision | Read; **ours** for the brush model; the lighting's four sky domes **copied** (P6 step 2b-1) |
| xmodelmesh (0x09) | 14,717 / 20,647 | 16.3 MB | One model LOD: resident geometry, or a stream key into the packages | Read, geometry extracted (`mkasset`); **ours** for the brush model; the sky domes' **copied** (P6 step 2b-1) |
| xskeleton (0x08) | 3,254 / 10,868 | 1.1 MB | Bones plus per-bone bounds (the bounds decide culling) | Read; **ours** (one bone per mapkit model); the sky domes' **copied** (P6 step 2b-1) |
| xcollision (0x07) | 2,202 / 5,688 | 0.2 MB | A model's own collision | Read; mapkit models link an empty one (not solid) |
| physpreset (0x02), physconstraints (0x03) | 30 / 16, 51 / 96 | 0.25 MB | Physics settings and joints | Read, borrowed |
| dynmodel (0xD1) | 4 | small | Dynamic models (by the name) | Not reversed |
| xmodelalias (0x3A) | 14 | small | Sets of model variants (by the name) | Not reversed |
| material (0x0A) | 8,066 / 12,778 | 10.7 MB | A surface: techset, image table and constants | Read; copied field by field for the creator's textures and sky (P4), whole for the sky domes and the gfx_map (P6 step 2b-1) |
| image (0x10) | 11,736 / 39,707 | 52.2 MB | Textures: headers and small mips here, large mips streamed from the packages | Read; the creator's own written resident (P4); the 2,325 the world copies link **copied** whole (P6 step 2b-1), 1,613 of them streamed |
| techset (0x0F) | 0 / 1,216 | small | Compiled shaders; the 562 own ones are in `techset_zm_silver.ff` | Read, borrowed |
| lighting (0xAA) | 1 | 7.06 MB | Baked lighting: sun, sky and model lighting (section 6). It also holds the level's 948 lights again, and 8 baked shadow trees (the engine's SST): the sun's shadow of the level's static geometry per lighting state, and a shadow region's, streamed by key (50.7 MB in all) | **Copied** (P4 for a sun or fog, always since P6 step 2a); its images, sky domes, klf and winddef copied too since P6 step 2b-1 (its 8 streamed-texture keys stay DM's while DM loads). Its +208/+216 image sets hold six images each, not five (the missed sixth crashed step 2a's first test; fixed). Since 2026-10-05 (passed in the game that day) the copy's lights are black and its baked trees are empty ones ([mapkit-plan.md](mapkit-plan.md), "The map's own lighting, step 1"). Its reflection probes, GI images and image sets are still Die Maschine's |
| postfxbundle (0x7B) | 7 / 6 | small | Screen effects (`pstfx_zm_wormhole`, `pstfx_slowed`, `pstfx_burn_loop`) | Borrowed |
| renderoverridebundle (0x7E) | 10 | small | Material overrides on characters and items (`rob_sr_item_gold`) | Borrowed |

### 4.3 Effects and weather

| Type (id) | Own / ref | Size | What it is | mapkit |
|---|---|---|---|---|
| fx (0x33) | 727 / 1,732 | 6.9 MB | Particle effects, snow and fog among them | Borrowed |
| staticlevelfxlist (0x7F) | 1 | 0.21 MB | The level's placed ambient effects: 2,482 of 139 kinds | **Ours, empty** (P6 step 2a) |
| fxlibraryvolume (0xBC) | 1 | 2.08 MB | Effect volumes (by the name) | Borrowed |
| vectorfield (0xD2) | 3 / 3 | small | Force fields that push particles (by the name) | Borrowed |
| winddef (0xD3) | 1 | small | The wind definition (a 128-B root, nothing else) | **Copied** (P6 step 2b-1: the lighting links it) |
| beam, tracer, tagfx, laser, flametable | 9+5+13+0+2 own | small | Weapon and effect visuals | Borrowed |
| surfacefxtable, impactsfxtable, entityfximpacts | 9+6+3 own | small | Impact effects per surface | impactsfxtable read; borrowed |
| klf (0x35) | 16 / 42 | small | Unknown; a lighting state's sky links one (712-B entries with pixels and an image each) | **Copied** (P6 step 2b-1: the lighting's, with the klf whose pixels it shares) |

### 4.4 Animation and cinematics

| Type (id) | Own / ref | Size | What it is | mapkit |
|---|---|---|---|---|
| xanim (0x05) | 2,203 / 5,071 | 28.2 MB | Animations: cinematics, animated props, map-specific AI moves | Borrowed |
| xcam (0x6C) | 12 / 77 | small | Camera animations for scenes | Borrowed |
| sanim (0x66) | 10 | 1.43 MB | Looping animated props the gfx_map places (floating objects, jellyfish) | Copied whole into mapkit's gfx_map, script strings re-pointed |
| streamerhint (0x79) | 4 | small | Pre-streaming for the intro and the outros | Borrowed |
| character (0x39) | 39 | small | Character setups (bodies, heads) | Borrowed |

### 4.5 AI

| Type (id) | Own / ref | Size | What it is | mapkit |
|---|---|---|---|---|
| aitype (0x38) | 11 | small | Die Maschine's zombie types (`spawner_bo5_zombie_zm_silver`, armored medium and heavy, panzer pilot, ...) | Borrowed |
| behaviortree, behaviorstatemachine, animstatemachine, animselectortable, animmappingtable | 3+8+3+3+4 | 0.3 MB | AI brains and animation selection | Borrowed |
| blackboard, aimtable | 0 / 1 each | small | Shared AI definitions | Borrowed |
| navmesh (0x75) | 1 | 168-byte root | The ground navmesh. Its root points at data loaded elsewhere; where that data lives is not traced. | **Ours** since P5 step 1 (2026-09-29): generated over the map's collision |
| navvolume (0x76) | 1 | 80-byte root | The 3D navigation volume (flying AI) | **Copied** (P6 step 2a; its two streamkeys too since step 2b-1) |
| footsteptable (0x50) | 4 | small | Footstep sounds | Borrowed |

### 4.6 Gameplay and scripts

| Type (id) | Own / ref | Size | What it is | mapkit |
|---|---|---|---|---|
| scriptparsetree (0x44) | 45 | 0.51 MB | Compiled GSC and CSC. Die Maschine's: `zm_silver` (the level script), `_zones`, `_ffotd`, `_gamemodes`, `_main_quest`, `_pap_quest`, `_ww_quest`, `_util`, `_sound`, `_vo`, `zm_silver_hud`, plus the AI, vehicle and BGB scripts it links | The level script, `_zones` and `_ffotd` are **ours** (served under the same names); the rest drop out because our level script does not include them |
| script_using (0x46), script_using_zm (0x4A) | 13 + 4 | small | The scripts the zone links on its own (24 bytes each: name, GSC, CSC) | Read; 3 of them now run our scripts |
| scriptbundle (0x57) | 1,591 / 73 | 3.05 MB | Data bundles. They include:<br>- scenes: the intro `cin_zm_silver_intro`, echoes, outros;<br>- exfil waves; item spawn lists;<br>- dialogue; trophies; music tracks;<br>- cull settings; a list of models to always stream. | Borrowed; no writer yet (exfil waves and crafting blueprints need one) |
| stringtable (0x3F) | 2 | small | The weapon table (`#hash_5e105c88ae5d540f`, which our level script sets as `level.var_d0ab70a2`) and one more | Borrowed |
| zbarrier (0x53) | 3 | small | Mystery Box, wooden window barrier, concrete wall barrier | Borrowed |
| vehicle (0x4C), vehiclesounddef (0x55) | 1 / 1, 1 | small | Vehicle definitions | vehicle read; borrowed |
| objective (0x73) | 16 / 9 | small | HUD objectives and waypoints | Borrowed |
| statuseffect, shellshock, rumble, gesture, gesturetable, triggereffectdesc | 0+0+25+5+0+7 own | small | Player effects: status effects, shell shock, controller rumble, gestures, adaptive-trigger feedback (the last by the name) | Borrowed |

### 4.7 Weapons

| Type (id) | Own / ref | What it is | mapkit |
|---|---|---|---|
| weapon (0x21) | 31 / 81 | Weapon definitions | Borrowed |
| weapontunables (0x27) | 26 / 81 | Weapon tuning | Borrowed |
| weaponblueprint, weaponcamo, weaponcamobinding, weaponstylesettings, weaponsecondarymovement | 0+3+7+28+7 own | Blueprints, camos and their bindings, styles | Borrowed |
| sharedweaponsounds, ballisticdesc | 3 / 18, 0 / 5 | Weapon sounds, ballistics | Borrowed |

### 4.8 Sound and text

| Type (id) | Own / ref | Size | What it is | mapkit |
|---|---|---|---|---|
| sound_bank (0x12) | 1 / 1 | 0.60 MB | The map's sound bank: aliases, rooms (bank +80), ducks, acoustics | **Copied** without the library (P6, 2026-10-03; works in the game); borrowed with it |
| sound_asset (0x13) | 1,379 / 19 | 0.12 MB | Sound entries (the audio itself is streamed) | Copied with the bank |
| sound_acoustics (0x16) | 1 | 0.13 MB | Triton acoustics: probes baked from Die Maschine's walls. For each sound and the listener's position they set the occlusion, the direction the sound arrives from and how its reverb is mixed, in every room that does not set `OverrideTriton` | Copied with the bank; `--no-acoustics` leaves it out |
| sound_duck, sound_alias_modifier | 14 / 1, 0 / 1 | small | Volume ducking and alias modifiers | Copied with the bank |
| surfacesounddef, entitysoundimpacts, impactsoundstable | 27+13+11 own | small | Footstep and impact sounds per surface | impactsoundstable read; borrowed |
| localizeentry (0x1D) | 0 / 5,709 | 0.09 MB | Localized text: every entry here is a link by name; the text is in `en_zm_silver` (1,981 own) and the other language zones | Read, borrowed |

## 5. The map entities

Die Maschine's entity list holds 2,380 entities and its trigger list 327. Scripts find entities by
targetname, classname or script keys; the engine spawns them at level load (`G_SpawnMapEntities`, entity 0
must be worldspawn).

| Classname | Count | What it is | mapkit |
|---|---|---|---|
| script_struct | 1,611 | Everything scripts look up. It covers:<br>- player spawns and respawn points; zombie rise points;<br>- the content structs of the box, wall buys, perks and devices;<br>- exfil parts; quest points. | Kept when placed, with their content parents; spawns and rise points generated |
| info_volume (trigger) | 187 | Zone volumes (`player_volume`) and other volumes | **Ours:** one per zone |
| trigger_multiple / _damage / _use_touch / _use / _box / _hurt | 82 / 23 / 22 / 8 / 4 / 1 | Quest, door, trap and hazard triggers | Left out (doors not built yet) |
| script_model | 140 | Scripted models: the tank, doors, quest props | Left out; the brush model is ours |
| node_pathnode, node_exposed, node_negotiation_* | 84, 34, 168 | AI path nodes. Each binds to the next compiled node in `game_map`, so they cannot be authored. | Left out |
| navmesh_extra_verts, nav_volume | 11, 1 | Navmesh helpers | Left out |
| info_vehicle_node, info_vehicle_node_rotate | 46, 46 | Helicopter paths: the exfil, chopper gunner and napalm strike | Kept; the exfil's paths are moved |
| zbarrier_* | 25 | Window barriers (24 wooden, 1 concrete) | Left out (barriers not built yet) |
| scriptbundle_scene / _zmintel / _itemspawnlist | 52 / 12 / 1 | Placed scenes, intel pickups, a supply stash | Left out |
| script_origin | 26 | Script points; the minimap corners among them | Minimap corners generated |
| actor_spawner_* | 5 | The zombie spawn templates | Kept |
| perf_camera, volume_performance, volume_fpstool, export_volume, occlusion_override | 100, 1, 1, 6, 5 | Tool and performance markers (by the name) | Left out |
| worldspawn | 1 | Level settings: gravity, wind, ambient track and more | Kept |
| reflection_probe, volume_outdoor, heli_height_lock, script_vehicle | 1 each | A probe marker, the outdoor volume, the helicopter height limit, a vehicle | Left out |

## 6. Sky, sun, time of day, colour, fog, weather, sound, minimap

| Thing | Where it lives | mapkit |
|---|---|---|
| **Sky** | Sky models and materials in `zm_silver.ff`: `skybox_zm_silver_override`, `_dark_override`, `_dark_override_lightning`, `skybox_default_black`. The worldspawn `skyboxmodel` key names one, but the engine's worldspawn spawn does not read it, so the sky is baked into the lighting and gfx_map. The dark-override skies are Die Maschine's Dark Aether look. | Borrowed. Needs a lighting writer. |
| **Sun and time of day** | Baked into the lighting asset (sun direction and colour, the sun-and-sky preset, worldspawn `ssi` = `default_day`) | Borrowed. Needs `Load_Lighting` transcribed. |
| **Colour grade** | The LUT material `lut_zm_silver` at gfx_map +1304, LUT images at +1320..+1344. The worldspawn `vcolor`/`vbloom` = `zm_silver_main_lgt9` are baked. | Borrowed. The LUT can be relinked by name. |
| **Fog** | Not located (lighting or gfx_map) | Unknown |
| **Weather (snow)** | Snow and fog particle effects (fx), placed by the static level FX list, the effect volumes and script exploders. Also snow decals (gfx_map decals, cut by mapkit) and the snowy terrain material. | Effects borrowed; decals cut; terrain flattened |
| **Wind** | winddef, plus the worldspawn wind keys (`wind_global_vector` and the wind speeds, which the engine reads) | Borrowed; the worldspawn keys are settable |
| **Ambient sound, reverb, music** | Worldspawn `ambienttrack`, the sound bank, music-track bundles. Reverb has two parts: the ambient rooms, set by `ambient_package` triggers in the trigger list (room names from the sound bank; a room also plays its room tone and sets the gunfire's indoor or outdoor tails), and sound_acoustics, Triton acoustics baked from Die Maschine's geometry. A room's own settings are the same all over it, while Triton's change with position, unless the room sets `OverrideTriton`. Also `zm_silver_sound.gsc`/`.csc` (ambient emitters, no longer run under our level script). | Rooms **ours** (`MkAmbientRoom`, [mapkit-plan.md](mapkit-plan.md) "Ambient rooms"); the bank **copied** without the library; the rest borrowed |
| **Minimap** | The `minimap_corner` entities (the map's bounds) and a minimap image that is not in `zm_silver.ff` (not found yet) | Corners ours; the picture is Die Maschine's |
| **Intro cinematic** | Scene bundles, xanims, xcam and stream hints, played by `zclassic::intro_cinematic` | Dropped; the method is in [mapkit-plan.md](mapkit-plan.md) "The intro cinematic" |

## 7. The shared layer: why many things are "in every map"

Perk machines, the Mystery Box, wall-buy logic, the ammo crate, Armor Station, Der Wunderfizz, the crafting
table, the exfil logic, the zombies' base scripts and the UI all live in **zm_common**, not in the map. A map
provides placement (structs), its own zone graph, its own quests, and the assets only it uses. That is why a
mapkit map only has to write entity structs for those objects: their models and scripts load anyway.

## 8. What mapkit needs to reverse, in order

A constructor that needs no base map has to write every row above that is "Borrowed" and that belongs to the
level rather than to the asset library. The order follows the plan's milestones.

| # | Work | Types | Milestone |
|---|---|---|---|
| 1 | **Placement:** the gfx_map's 992-byte model groups (instance transforms, the model per instance, the 912-byte block, culling) | gfx_map, clip_map +24 | P2 |
| 2 | **Solid props:** an xcollision writer, or brush hulls around props | xcollision | P2/P3 |
| 3 | **Own meshes:** glTF to resident xmodel, with the model groups from step 1 | xmodel, xmodelmesh, xskeleton | P3 |
| 4 | **Textures:** image pixel reader, then image and material writers inside the shaders the loaded techsets hold | image, material, techset | P4 |
| 5 | **Lighting:** transcribe `Load_Lighting`; sky, sun, colour grade, fog, model lighting | lighting, gfx_map sun and LUT fields | P4 |
| 6 | **Terrain:** keep the +224 height-texture block, drop the drawn tiles; later, own terrain | terraingfx, clip_map +160 | P4 |
| 7 | **Navmesh:** where its data lives, then a generator (Havok AI data) plus nodes | navmesh, navvolume, game_map | P5 |
| 8 | **Level effects and sound:** placed ambient effects, effect volumes, reverb, ambient track | staticlevelfxlist, fxlibraryvolume, sound_acoustics, winddef | P4/P5 |
| 9 | **Script data:** a scriptbundle writer for exfil waves, crafting blueprints, scenes and item lists | scriptbundle | with gameplay work |
| 10 | **Visibility and extras:** occlusion, the world's lights, glass, streaming cells for large maps | cpu_occlusion_data, com_map, glasses, streamerworld, districts | P4/P5 |
| 11 | **Minimap image** and the map's own text (title, hints): an image override and localized strings | image, localizeentry | P4 |
| 12 | **Own asset set:** copy from each player's install everything the map uses beyond zm_common and core, then load under the map's own name | all asset-library types | P6 |

What mapkit already has for this: loaders for 27 of the zone's 92 types, verified on all 182,002 assets. It
can write the entity list, trigger list, clip map, a trimmed gfx_map, empty streamerworld and districts,
bgcache, static models with their meshes and skeletons, and by-name references to any asset.
