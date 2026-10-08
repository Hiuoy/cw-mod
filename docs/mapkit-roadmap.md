# Mapkit groundwork: the zone formats and the first maps (M1–M5)

> **Closed 2026-09-26.** These were mapkit's first milestones. The work continued as P0 to P6
> ([mapkit-plan.md](mapkit-plan.md)), and the status of everything is in
> [ROADMAP.md](ROADMAP.md#5-custom-maps-mapkit). This file keeps the zone formats reversed for the groundwork and
> the log of its first test maps.

**Goal:** community-made Zombies maps for Black Ops Cold War build **1.34.0.15931218**. A creator
models in Blender, lays out the map and its gameplay in Godot with the mapkit plugin, presses Build
and gets a zone set, then presses Play and the cw-mod client loads it. Treyarch never shipped mod tools for this game, so
mapkit reads and writes the fastfile format itself.

**Legal line (same as the rest of cw-mod):** a map zone holds only the creator's own content.
Game assets (models, materials, sounds, scripts) are referenced by name and resolved from the
player's own install at load time. They are never extracted into a distributed map, and nothing
read from the game (zones, `oo2core_8_win64.dll`) is ever committed or shipped.

## Parts

| Part | What it does |
|---|---|
| `mapkit/zonekit` | Fastfile library: container, `.fd` patches, xfile stream, asset list, and the per-type readers and writers for the assets a map needs |
| `mapkit/ffinfo` | CLI: inspect, unpack and repack zones; `--scan` checks a whole install |
| client loader (cw-mod DLL) | Loads zones from `cw-mod/maps/<map>/`, skips signature checks for those only, launches by name |
| `cwlink` | Map source → the map's own zone (`<id>.ff`, since 2026-09-29). `--overlay` writes the older form this file describes: override zones for a base map (`ww_1080_`/`ww_4k_<map>`) |
| `mapkit/godot` | The level builder: a Godot 4 plugin. Block-out, Zombies objects, checks, walk-through, export to `.mkmap` ([format](../mapkit/mkmap-format.md)); Build runs `cwlink build` |

**Does the editor need the game running?** No. The editor and linker are standalone and read the
game install only to browse assets. The game is needed to **test** a map: the cw-mod client loads
it, and later hot-reloads it.

## What is known (IDB names; all renamed and commented)

**Container (`.ff`).** `DB_ReadFastfileHeaderTLV` 0x7FF7276A1C30. `TAFF` + section count + 9
tagged sections that fill a 1392-byte header struct: version (100), flags (server, block codec,
checksum mode, encrypted), build info with the 16-byte build checksum the exe checks, stream size,
13 XBlock sizes, and zone name + RSA-2048 signature. Then 16-byte block headers
`{compSize, rawSize, storedSize (compSize aligned to 4), fileOffset}` and Oodle Kraken data;
blocks are `0xFFFA0` raw bytes; a zero header ends the chain; files are padded to 64.
`DB_FillCompressedBlocks` 0x7FF7276A2050, `DB_DecompressBlockJob` 0x7FF72769F6E0.

**Patches (`.fd`).** Every shipped `.ff` is the launch build and its `.fd` upgrades it. 40-byte patch
header, the current zone's header, the `.ff`'s header (must match), then a zlib stream of an
RFC 3284 VCDIFF delta (default code table). Each window ends with an XXH32 of its output.
`DB_ReadZoneFileHeader` 0x7FF727EC1510, `DB_Patch_VcdiffDecodeWindow` 0x7FF72960AAD0.
**Custom zones need no `.fd`.**

**Stream.** `XAssetList` (40 B: script strings + asset table), then asset bodies in load order.
Pointers are `0` null, `-1` data follows inline, `-2` inline + insert, anything else a reference
`((block << 60) | offset) + 1` into data loaded earlier (`DB_ConvertOffsetToPointer` 0x7FF729668D00).
13 XBlocks: 0,1,4,5,6,10,12 are read from the file; 2,3 are zero-filled at runtime; 7,8,9,11 are
not stored (`DB_LoadXFileData` 0x7FF729668D60). Stream positions use `DB_PushStreamPos`,
`DB_PopStreamPos`, `DB_AllocStreamPos`.

**Assets.** `Load_XAssetHeader` 0x7FF71E7F75A0 dispatches to one loader per type; 211 of the 221
types have one, all renamed `Load_<Type>Asset`. Asset names are FNV-1a 64 of the lowercased name,
top bit cleared. The client world is `clip_map`, `com_map`, `game_map`, `gfx_map` (`col_map` has no
client loader), plus `streamerworld`, `lighting`, `navmesh`/`navvolume` and friends.

**Map entities.** The `entitylist` (0x8E) and `triggerlist` (0x80) assets hold a level's entities in
one shared array format:
- `Load_EntitylistAsset` 0x7FF71E7D50F0, `Load_TriggerList` 0x7FF71E7EEEE0 and `Load_MapEntityArray`
  0x7FF71E7DE900; layouts in `zonekit/map_entities.hpp`.
- An entity is 48 bytes (id, origin, angles, key count) plus a list of 32-byte key/value pairs.
- Value types: 2 string, 3 vec3, 4 name hash (models), 5 float, 6 int.
- Both assets' root structs start with the hash of `maps/<p>/<map>.d3dbsp`, so `ffinfo --entities`
  finds and decodes them without walking the rest of the zone.
- Decoded on all four round-based maps: zm_silver 2707 entities, zm_gold 3443, zm_platinum 3705,
  zm_tungsten 4205.

How the four round-based maps express Zombies gameplay (what cwlink will write for each mapkit class):

| mapkit class | Game entities |
|---|---|
| `player_spawn` | `script_struct`, targetname `initial_spawn_points`, script_noteworthy `player_<n>`, script_int `<n>`; per-zone respawns are `player_respawn_point` / `<zone>_respawn_points` |
| `zone` | `info_volume` (trigger list), targetname = the zone name (`zone_*`), script_noteworthy `player_volume`; a zone may be several volumes |
| `zombie_spawner` | `script_struct`, targetname `<zone>_spawns`, script_noteworthy `riser_location` / `spawn_location` / `dog_location` / `wait_location`, script_string `find_flesh`. The AI types are separate `actor_spawner_zm_*` entities (script_noteworthy `zombie_spawner`) |
| `barrier` | `script_struct` `exterior_goal` (script_string = the zone, target = the barrier), `script_struct` `barrier_align`, and a `zbarrier_<type>` entity (board models and animations as keys) |
| `door` | `trigger_use_touch`, targetname `zombie_door`, `zombie_cost`, script_flag = the zone connection it opens, target = the blocker `script_model` |
| `perk_machine` | `script_struct`, targetname `zm_perk_machine`, objectid `perk_spawn_struct`, script_noteworthy `talent_<perk>`: juggernog, speedcola, quickrevive, staminup, deadshot, elemental_pop, tombstone, mulekick, phdslider (PhD Flopper), deathperception |
| `mystery_box` | `script_struct` per location, script_noteworthy `<area>_chest`; plus one `magicbox_location` / `magicbox_instance` pair |
| `pack_a_punch` | `script_struct` `pap_machine_pos` (+ `pap_prompt_pos` on zm_gold); zm_silver's is quest-driven |
| `power_switch` | `trigger_use` `use_elec_switch`, `script_struct` `power_switch` (script_noteworthy `elec_switch_fx`), `script_model` `elec_switch` |
| `crafting_table` | `script_struct` `crafting_table` / `crafting_table_location`, `trigger_box` `crafting_trigger` |
| `wall_buy`, `arsenal` | not found yet: each map has a single `wallbuy_location` / `wallbuy_instance` pair, so wall buys are placed another way |

**World geometry (2026-09-25).** A retail map's world is not BSP geometry in the `.ff`:
- `gfx_map` (`Load_GfxWorld`, 7976-B root) holds layout, lighting and material tables. Its `GfxWorldDraw`
  vertex/index buffers are block 2 (runtime, zero-filled) and tiny on zm_silver (38 KB and 3 KB). It has no static
  models (count 0).
- The world is `streamerworld` (0xAC, `Load_StreamerWorld`): an xmodel list plus instance tables. zm_silver
  carries 14,123 xmodels and 35,364 xmodelmeshes.
- Mesh data (`Load_XModelMeshSurfaceData`, 464 B) is **streamed** when `flags & 1` (block 7, from `.xpak`/`.xsub`,
  nothing in the `.ff`). Otherwise it is **resident** in block 12, inside the `.ff`. So a custom map's meshes need
  no `.xpak` of their own.
- Image pixels: block 6 (in the `.ff`) unless the image is streamed (flag 0x10 → block 7).
- zm_silver declares 15 GB in block 7. Viewing a retail map 1:1 therefore needs the KAPI package reader
  (`.xpak` index + `.xsub` data; header magic `KAPI`, version 4, type 2 = xpak, 3 = xsub; not reversed yet).

**Zone trace.** A level zone mixes ~90 asset types, and its stream holds no sizes. So an offline walk stops at
the first type without a loader (zm_silver: asset 2 of 182,002). With cw-mod.json `"mapkit_trace": ["<zone>"]`,
the client records where each asset starts while the game loads the zone. That is the stream offset (the byte
count of `DB_ReadXFile` + `DB_ReadXFileString`) and all 13 block positions, captured at `Load_XAsset`. The
result goes to `<game>/cw-mod/mapkit/trace/<zone>.mktrace` (`client/game/mapkit_trace.*`).
`ffinfo <zone> --trace <file>` then:
- checks the trace against the zone;
- compares the engine's block positions with mapkit's model at the first asset;
- decodes every asset mapkit has a loader for from its recorded start, and checks that each one ends exactly
  where the next begins (stream offset and every block).

That proves loaders on level zones without walking them, and it lets the world-set readers jump straight to
`gfx_map`/`streamerworld`/`xmodel`.

**Signatures.** `DB_LoadXFile_Finish` 0x7FF7276A0C30 calls `DB_Signature_VerifyZone` 0x7FF7292AD410
after the zone is linked: RSA-PSS over a 25600-byte accumulator seeded with the load name and
XOR-folded with every block's SHA-256. Failure trips `DB_TamperResponse_CorruptEntryFreeList`
(the "Uniform 58 Guerrilla Boa" crash one zone later). `DB_Signature_VerifyHeader` runs only for
`.fd`-patched zones.

## Milestones

**M1: container. DONE (2026-09-25).** zonekit reads `.ff`, applies `.fd`, and writes `.ff`.
Pass: `ffinfo --scan` decodes all 948 shipped zones; `ffinfo zm_silver --repack` writes a
standalone `.ff` whose stream reads back byte-identical.

**M2: asset layer. CLOSED 2026-09-26:** the loaders and writers continued as part of each P milestone
([mapkit-plan.md](mapkit-plan.md)). Done 2026-09-25:
- `zonekit/xstream`: the engine's stream state replayed offline. Push/Pop/Alloc/Load/LoadString/Insert
  mirror `DB_PushStreamPos` and friends one for one.
- `zonekit/asset_walk` + `asset_loaders`: loaders transcribed from `Load_<Type>Asset`.
- `ffinfo --walk` / `--walk-scan`.
- Loaders: image, keyvaluepairs, techset, sound_bank (with the sound_duck and sound_alias_modifier
  sub-assets), sound_asset, sound_acoustics, streamkey, localizeentry, rawfile.
- **677 of 948 zones walk completely, 0 errors.** The stream has no sizes, so three checks prove
  a loader:
  - the walk ends on the stream's last byte;
  - every back-reference lands in data already loaded;
  - every XBlock size in the header matches the walk. The rules:
    - block 0 = temp high-water + 40;
    - block 1 = 208 + each inline asset root, 16-aligned (+208 per techset);
    - block 11 is not modelled yet;
    - every other block = its high-water mark.
- Next blockers by zones stopped: material 86, xskeleton 64, xanim 49, sound_alias_modifier 28,
  ttf 21, then a tail.
- 2026-09-25: `entitylist` and `triggerlist` loaders (above, "Map entities"). They also decode in
  place, and `cwlink replace <map> --move <key>=<value>@x,y,z[,yaw]` edits an entity in a retail zone.
  That is test C: Speed Cola moved to Die Maschine's start area, to prove the game plays the entity data
  we write.

The original plan: turn each needed `Load_<Type>Asset` into a schema (field offsets, pointer
fields, inline arrays, block pushes) and build a generic stream walker that reads and writes by
schema. Pass: decode → re-encode of a real zone's stream is byte-identical. Order: small types
first (`keyvaluepairs`, `rawfile`, `stringtable`, `scriptparsetree`, `localizeentry`), then the
map set (`com_map`, `game_map`, `clip_map`, `gfx_map`, `streamerworld`, `xmodel`, `material`,
`image`). Open question: how a zone references an asset that lives in another zone (needed to
use game materials and models without embedding them).

**M3: client loader.** In the cw-mod DLL: add `cw-mod/maps/<map>/` as a zone search path, skip
`DB_Signature_VerifyZone` only for zones loaded from there, find out whether missing variant zones
(`techset_`, `en_`, `ww_`, `1080_`) are skipped or fatal, and launch a map by name through the
existing host-launch path. Pass: a repacked retail map, renamed, loads and plays from
`cw-mod/maps/`. **DONE 2026-09-25**. Test A replaced zm_silver; Test B redirected Die Maschine to the
clone zm_mapkit. A lobby launch loads the level zone twice, and both loads need the redirect:
- the lobby preload (`MapPreload_StartZoneRead` 0x7FF727B32E60, into its own stream buffer);
- the client's own load at launch (`Com_LoadLevelFastFiles` → `DB_LoadXAssets`, which unloads the
  preload when the names differ).

`SV_StartMap` only renames the map. So a redirect must be set before the lobby opens.

**M4: first custom map. PASSED 2026-09-25 (test E1, below). Route changed 2026-09-26: see [mapkit-plan.md](mapkit-plan.md)** (Die Maschine
stays only as the asset library; the level script, entity lists and world assets become mapkit's own). `cwlink` builds a zone set from a map source: a box room using
game materials by reference, spawn points, and our GSC. Pass: it loads and a player can walk in it. The first
target (2026-09-25) is a plane map that replaces Die Maschine. Placements of retail map models are skipped (the
user's call): a new map doesn't need them.

*How a custom map gets into the game: override zones.* No retail zone is modified. mapkit writes small zones of
its own that the engine loads alongside the base map, and whose assets win over the map's own:
- `DB_ExpandZoneVariants` 0x7FF7295F2490 loads a level zone as `en_<map>`, `ww_<map>`, `1080_`/`4k_<map>`,
  `ww_1080_`/`ww_4k_<map>`, `techset_<map>`, then `<map>` itself.
- `DB_LinkXAssetEntry` 0x7FF727EC3AA0: a second asset with a name already linked is an **override**. It is chained
  behind the first, sorted by `DB_GetZonePriority` 0x7FF727EC2F50 of the zone flags, and
  `DB_PostLoadFrame_ApplyOverrides` 0x7FF727EC4A30 swaps the header contents (`DB_SwapXAssetHeaders` 0x7FF727EC4F80)
  so the entry everything looks up holds the winner. A level zone is priority 7; the region variants
  (`ww_`, flag 0x1000000) are 27.
- So `cw-mod/maps/<map>/ww_1080_<map>.ff` and `ww_4k_<map>.ff` (the game loads one, by texture setting) replace any
  asset of the map by name. The retail `ww_1080_zm_silver` holds one keyvaluepairs asset; mapkit carries it over.
- A name hash with the top bit set is a **reference** to an asset in another zone: it links to the existing
  one (or `DB_LinkMissingReference` 0x7FF727EC1860 makes a stub). That is how a custom zone uses retail materials
  and models without containing them. The stub is a copy of the type's **default** asset, replaced when the real
  one links later. A type with no default (lighting, clip_map) is a drop instead.
- **Link order.** Zones link in the order above, so the map's own zone links **last**, and the lobby preload
  links its zones in that order at launch (`DB_ProcessLoadQueue` 0x7FF727EC3E50). An override zone therefore
  linked before the map, and could only reference assets of zones linked earlier. The client's
  `DB_ExpandZoneVariants` detour moves `ww_1080_`/`ww_4k_<map>` just after `<map>`. It does this only for a folder
  that has no `<map>.ff` of its own and whose map is retail; a replaced or cloned map keeps the engine's order. Priority,
  not order, still decides the winner. It is safe only if the map never references the retail override zone's
  assets: true for zm_silver (its one kvp, 0x3A3D083D3CE2FEB9, is referenced nowhere), unchecked for other maps.
- The world pools (clip_map, com_map, game_map, gfx_map, streamerworld, lighting, entitylist, triggerlist)
  have 2 items: the frontend's and one level's. The lobby preload (`Load_XAsset_Preload`) links later, at launch.
  `g_clipMap` 0x7FF7363D4060 is the clip_map pool's item 0.

*Writer (built 2026-09-25).* `zonekit/xwriter` is XStream's writing side: an encoder calls the loader's own
primitives in the loader's order, and only stored blocks put bytes in the stream. `zonekit/zone_writer` writes
the XAssetList (script strings, XAsset array) and the assets, **walks the result with mapkit's own loaders** (the
walk must be complete) and takes the header's XBlock sizes from that walk. Block 11 (the preload's pointer
fixups, 8 B per pointer) gets an upper bound. `cwlink patch <map> --trace <file> [--move ...]` builds the override
zones; today the replacement is the map's entity list, re-encoded from the traced retail one
(`ReadMapEntitiesTraced` resolves every string, including the ones into other assets, by replaying those assets:
the vehicle loader was added for that).

**Test D1 PASSED in-game (2026-09-25):** `cw-mod/maps/zm_silver/ww_1080_zm_silver.ff` + `ww_4k_...`: the retail
kvp + the entity list with Juggernog moved to the player-4 spawn (1024, 61, 79) and Speed Cola to the player-5
spawn (1080, 11, 80), both yaw 253. zm_silver.ff itself is retail. The user saw both machines at the start: a zone
mapkit writes from scratch loads, and its world asset (a 2-item pool) wins over the map's own.

*The clip map, decoded (2026-09-25, exact on zm_silver).* `ReadClipMap` / `ffinfo --clipmap`:
- +48 **collision trees** (u32 @436, 214 on zm_silver) = cmodels. Tree 0 = the world bounds, trees 1.. = brush
  models (`*1`..). Each is a set of convex hulls: AABB, vertices, extra planes, per-piece counts and contents. Tiny
  and self-contained: a Godot `MkBrush` compiles straight to one.
- +160 **terrain** collision: a grid of heightfield tiles (zm_silver: 32x32 tiles of 512 units, 33x33 u16
  heights each). Height = u16 x scale + offset, both per entry (+148 / +112; zm_silver 0.12498665 and -122,
  checked against every sloped tile's bounds). A tile holds its bounds, grid origin, heights, and its surfaces:
  u16 @68 = 1 means one surface (index u16 @70 into the entry's list of 44 {name hash, ...}), more means a blob
  of packed per-quad indices. Retail tiles share identical heights or blobs by pointing at earlier ones.
- +24 136 structs of 992 B (xmodel lists), +64 1373 xcollisions, +96 518 dynent defs, block-2 runtime arrays.
  +72 (19 u32, 0..26) and +80 (375 u16, 0..17) look dynent-related (18 = u16 @90 = u16 @460).
- The buildings are not in the clip map: static model collision comes from streamed model instances.

**Test D2, the plane: PASSED in-game 18:08 (built 17:09; the 17:12, 17:25, 17:40, 17:48 and 17:57 runs crashed, each fix below):** `cwlink plane zm_silver --trace <file>` (defaults: floor z 0,
7 x 7 tiles). The same two override zones, now holding four assets:
- the retail kvp, carried over;
- an **empty streamerworld** (`EncodeEmptyStreamerWorld`): no cells, models or grid, terraingfx null, the lighting
  linked by name (a 472-B root holding only the name with its top bit set; `Load_Lighting` reads nothing past a
  zero root, and the walk accepts such roots through `LoadAsset`'s by-name reference path);
- a **plane clip map** (`MakePlaneClipMap` + `EncodeClipMap`): the 214 retail trees (the triggers use them), the
  cell and tree indexes, the runtime block, and the terrain grid with every tile flat at z -0.013 (u16 976) and one
  surface. No 992-B structs, xcollisions, dynents or +72/+80;
- the entity list, retail plus 49 `script_model`s of `*zm_silver_floor.map_..._16` (476 x 476 x 1.5, a retail
  floor piece found by its bounds) in a 7 x 7 grid around the player spawns, tops at the floor height. Each copies
  a retail script_model's keys, trimmed to classname/model/origin/angles.

Expected: Die Maschine's buildings and terrain gone, a square floor at the spawn, flat ground under the whole map
(invisible past the tiles), and the map's machines and props floating where they were (its entities and scripts
stay). Zombies still use the retail navmesh. What this test can reveal: the streamer, lighting or gfx_map
refusing an empty world (a crash names the code), or static collision that lives somewhere other than the
clip map.

The 17:12 run dropped at launch with ERR_DROP "Uniform 99 Divebomb Karma", raised by `DB_LinkMissingReference`. The
override zone linked before zm_silver, so the streamerworld's lighting reference found nothing, and lighting has no
default asset. The name itself was right: zm_silver's lighting is asset 165897, `maps/zm/zm_silver.d3dbsp`. The
fix is the client's link-order detour above; the zones did not change.

The 17:25 run (17:21 client) linked in the new order (`zone order:` logged at preload and launch), then crashed:
an access violation reading 0xF00 in `DB_ConvertZoneScriptString` 0x7FF72966F1E0, called from `Finish_ClipMap`
0x7FF71E83C3E0 (the preload's link pass). A clip map stores 64 **script strings** at +168 as indices into its own
zone's table (cells and dynents hold more). The plane clip map kept zm_silver's indices (the first one is 480), but the
override zone had no table, so the lookup read null + 8 x 480. The 17:12 run never reached this: the lighting drop
came first. Fix (zones rebuilt 17:37, the client did not change): `ResolveClipMapStrings` turns the indices into text
through the map zone's table, the asset declares them (`ZoneAsset::scriptStrings`), and `BuildZoneStream` adds them to
the zone's table, keeping entry 0 null as retail does. `EncodeClipMap` writes the new indices (1..37, then 0). All 37
are stored encoded (a prefix byte, decoded by SL_GetString), so they are copied as bytes. **Rule: any zone-local
index (script strings) must be remapped into the writing zone's own table.**

The 17:40 and 17:42 runs (17:37 zones) got past linking: the level started loading, and the GSC loader served its
scripts. Both then crashed on a null read in `CM_RegisterDynEntSettingsAssets` 0x7FF7290210E0, a level-load pass
(`LevelLoad_RegisterStreamGroups_cand` 0x7FF725FC5F90) that walks the clip map's dynent records at +96 and
registers the assets in each def's settings tree. It counts them with a **third** dynent count, u16 @460, which
the loader never reads (zm_silver: @88/@90 = 518/18, @460 = 18). `EncodeClipMap` zeroed +88/+90 and nulled +96 but
kept @460. Fix (zones rebuilt 17:45, the client did not change): @460 is zeroed too, checked in the written stream.
The cell count @462 stays 12: zm_silver's own +8 is null too, with only the index stored.

The 17:48 (GSC on) and 17:50 (GSC off) runs (17:45 zones) got further, about 4 s into the level, and crashed the
same way: a read of 0x1E in `CM_TerrainTile_TraceQuad` 0x7FF7287C2270, the first collision trace against the
terrain. A tile's u16 @38 says which of its quads exist: 0 none, 1 all, otherwise one bit per quad in the +48 mask.
In zm_silver, 1003 tiles are 1 and 21 are 2 (terrain with holes, masks inline). `MakePlaneClipMap` dropped the masks
but kept @38, so those 21 tiles pointed the trace at a null mask. Fix (zones rebuilt 17:53): every plane tile gets
@38 = 1. Checked in the written stream: 1024 tiles, @38 1, +48 null, z -0.013, one surface. The quad's surface
comes from the entry's +152 list when u16 @68 <= 1 (`CM_TerrainTile_GetQuadSurface`), so the null +56 blob is fine.

The 17:57 run (17:53 zones) got further again: the server reached status 4 and the lobby census ran. Then it read
0x150 in `CM_CellRecord_GetClipModel_cand` 0x7FF7291DF040: a clip map cell record's u16 @72 (42) indexes clip map
+24, which the plane drops. The cells are not in the clip map (zm_silver's +8 is null). They are streamed: the
streamer's cells are the **districts** asset (0xAB, `Load_Districts` 0x7FF71E7D4440), which `Streamer_InitDistricts`
0x7FF7289394E0 finds by name at world load. Each of zm_silver's 24 districts has ten typed streamkeys, and slot 9
is its clip map cell: the buildings' collision. `CM_ForEachStreamedCell_cand` 0x7FF71E628130 walks every loaded one,
whatever the clip map's cell count says. Fix (zones rebuilt 18:06): the override zones also hold an **empty
districts** (`EncodeEmptyDistricts`: no districts, sets or pointer table, gfx_map null; no streamer code reads +8),
so nothing of the old world streams in. zonekit now loads districts (`LoadDistrictsAsset`, replays zm_silver's
exactly). **Rule: a world replaced with an empty one needs its districts emptied too; the streamerworld is not
what the streamer streams.**

The 18:08 run (18:06 zones) PASSED: server status 4 at 18:09:15, a full match to 18:10:34, and the post-match
re-preload; the 18:11 crash is the known exit crash (telescope.dll / exe+0xD59FED6). The old buildings still **draw**:
gfx_map +7456 aliases the retail streamerworld, so the renderer keeps the old world. Their collision is gone. The user
set that aside: the priority is the Godot → `.mkmap` → `cwlink build` → in-game pipeline.

After D2: own geometry (Godot brushes to collision trees and resident meshes), then `cwlink build`.

*World collision, reversed 2026-09-25 (IDB renamed).* Every world trace goes through clip map **tree 0**
(`CM_World_Trace_cand` 0x7FF7293E3270 with the callback `CM_Tree_TraceCallback` 0x7FF72965C330;
`CM_World_PointContents_cand` 0x7FF7293EAB90), then the streamed cells, then the terrain. zm_silver's tree 0
is empty: bounds only, masks 0. A tree's hulls ("kind 1", gated by the contents mask at +36):
- +32 hull count, +24 extra-plane count, +28 vertex count;
- +128: 28 B per hull, `{mins, maxs, planeCount << 26 | contents}`;
- +104: first plane per hull; +64 planes, 16 B `{normal, d}`, inside when `n·p − d ≤ 0`;
- +112: `vertexCount << 24 | firstVertex`; +72 vertices;
- +120: first face per hull. +80 packs the face surfaces, 10 bits each and 6 per u64. Faces 0-5 are the AABB's,
  6.. the extra planes. They are local indices into clip map +56 (the collision surface table), remapped at load
  into `g_cmSurfaceTable` by `CM_Tree_RemapFaceSurfaces` 0x7FF7292AC0F0;
- +88: an optional BVH. When it is null, the hulls are walked linearly.

The sweep, `CM_Hull_TraceCapsule` 0x7FF7295F32C0, is Quake 3 style: the AABB as 6 axial planes, then the extra
planes pushed out by the capsule. It never reads the vertices. So a brush is one hull: its AABB, a plane for
each face that is not axis-aligned, 8 corners, contents 1. For a box rotated about Z, the AABB planes are exactly
the bevels it needs.

**`cwlink build <map> <source.mkmap> --trace <f> [--at x,y,z] [--floor z]` (built 2026-09-25):** the plane
world (D2), plus the source:
- each solid brush becomes a hull in tree 0 (`BoxHull` + `AppendHulls` in world_writer), with surface 0;
- the terrain goes just under the lowest brush;
- `<map>`'s `initial_spawn_points` move to the source's player spawns (`player_<n>` takes player n, else the
  source's spawns in turn), and each `perk_machine` moves `<map>`'s `zm_perk_machine` struct for that perk;
- the source's origin lands at `--at`, by default the center of `<map>`'s initial spawns (zm_silver: 1043, -67, 65),
  so the map sits in the start zone and Die Maschine's zone and spawner logic keeps running around it.

Brushes are not drawn yet (meshes are next), and the other entity classes are not built. The Godot dock's
**Build** exports and then runs it for zm_silver (the same call, byte-identical output).

**Test E1 PASSED in-game 2026-09-25 19:01 (18:53 zones):** the Godot template, exported by the plugin and built by
`cwlink build`. That is 7 solid brushes: a floor, 4 walls and a divider with a doorway, 1024 x 472 units. Also
4 player spawns and Juggernog. In the game, expect:
- spawning in the invisible first room (Die Maschine's buildings still draw around it);
- invisible walls you can't walk through, and a floor you stand on at the spawns' height;
- the doorway in the divider open (doors are not built);
- Juggernog against the room's east wall.

The D2 zones are archived in `old_tests/d2_1806_passed`.

The user saw Juggernog and was blocked by the invisible walls. The log shows the launch at 19:01:58, a full match
to the after-action report at 19:08, and the post-match re-preload. The crash at 19:11 is the known exit crash. The
old buildings still draw, as expected. So the whole pipeline (Godot export, `cwlink build`, the game) works.

*The drawn world: gfx_map (2026-09-25 evening, IDB renamed).* The user asked for mapkit's own drawn world instead of
Die Maschine's. `Load_GfxWorld` and all its sub-loaders (about 30) are transcribed in `zonekit/gfx_world.cpp`. zm_silver's
gfx_map (3.06 MB) replays byte-exact from the trace, and so do its 11 grouplodmodels and 10 sanims (new loaders too).
`ffinfo <zone> --trace <f> --gfx` prints what it links to and its sub-arrays. What it holds:
- **No world vertex data.** The draw struct's vertex/index buffers are block 2 (runtime).
- **Draw +536 = the placed static models**, which are the buildings and props: 158 model groups (the 992-B
  `Load_ClipMapStruct992`, each with its instances' 64-B transforms) and 1,713 xmodels. **136 of the 158 are pointers
  into the clip map's own +24 structs.** That is why the old buildings still drew after D2: the retail gfx_map keeps
  drawing the retail clip map's groups, which stay in memory under our clip map override. It is also why the
  streamerworld never showed placements.
- Draw +328: per-instance data for 38,334 instances (index lists 1,950 + 36,384 at draw +488/+496). Draw +688: the
  terrain's drawing. Draw +696/+712: 11 group LOD models. Root +1408: 3,061 216-B entries with two materials each.
- The links: 3,288 materials, 1,803 xmodels, 57 sanims, 22 grouplodmodels, 6 images, lighting, streamerworld (235
  distinct assets). 328 pointers point inside the gfx_map (the terrain's material sets), 136 into other assets' data
  (the clip map's model groups).

**The override swap is a content swap.** `DB_ApplyOverride` → `DB_SwapXAssetHeaders` exchanges the two pool items'
contents, so a pointer to the head entry sees the winner. So the copy links every asset **by name** (a top-bit
reference), which lands on the head, never on the zone's own override entry, which ends up holding the loser.

**The splice writer** (`EncodeGfxWorld`) copies the retail stream bytes and patches only what cannot travel:
- asset links go to by-name reference entries in the writing zone's XAsset array (`XWriter::AssetEntry`,
  `ZoneAsset::linkName`), so the data layout matches retail;
- script strings are remapped into the zone's table;
- pointers into the asset itself follow their data;
- chosen sub-arrays are cut (pointer nulled, count fields zeroed);
- `relinks` point every link in a sub-array at another asset (counts and indices unchanged).

sanims are named by an inline string, not a hash, so a by-name reference cannot link them. The retail ones travel
whole instead (`CopiedAsset`, which now advances the writer's positions).

**Test G1 run 1 CRASHED 20:16 (20:09 zones), at match load.** The AV was a read of 0xA in `I_strncpyz_Complete`
(0x7FF71E64C8C0), under `DecryptStringToBuffer` and `SL_GetString_Guarded(str, 4, 9, decrypt=1)`. That is the call
of the `DB_ResolveScriptStringIndex` family, which turns a zone-local index into an SL id through the loading zone's
table with no bounds check.
- **Cause:** the 9 sanims were copied whole, and each still held zm_silver's script-string indices. `Load_SAnim`
  converts them at root +92, +8 of each +24 entry, and +16/+20/+24 of each +32 entry. That is 30 indices, up to
  4449, in a table of 55, so one read the pointer 0xA from past the table.
- **Fix:** zonekit `SAnimScriptStrings` finds those fields, and `CopiedAsset` now takes `CopiedString` patches.
  cwlink re-points them into mapkit's table.
- **New check:** `ffinfo --walk` now checks every gfx_map and sanim index against the zone's table. The 20:09 zone
  shows 30 BAD, the fixed one is clean.

Crashed zones: `old_tests/g1_2009_sanim_strings_crash`. **Fixed zones deployed 20:28** (72 strings, walk complete, no
DLL change). The Godot dock uses the same rebuilt cwlink.

**Test G1 run 2 (20:28 zones): the map LOADS and plays.** Die Maschine's buildings and walls are gone. Three
findings:
- **Crash on killing a zombie** (3 runs, 20:32/20:36/20:42, identical): a write to 0x0 at exe+0x74B29C4 in
  `DynEnt_Create_cand` (0x7FF72406D7A0, called from the CSC builtin `CScr_CreateDynEntAndLaunch_cand` for the
  gibs). Runtime dynents go into clip map slot `@460 + (a free one of 500)` and the code writes that slot's +96
  (96 B), +104 (248 B, `CM_GetDynEntPose`), +112 (280 B) and +128 (44 B) entries. Since D2 the plane clip map had
  none of these arrays. Retail carries its own dynents plus 500 empty slots (zm_silver @88 518 = 18 + 500). Each
  empty slot is zero except +36/+40 = 0x04B004B0. **Fix:** `EncodeClipMap` writes `kDynEntDebrisSlots` (500)
  empty records with @88 = 500 and @90 = @460 = 0, plus their block-2 arrays. `ffinfo --clipmap` now summarises
  the +96 records. Zones rebuilt 20:54: block 2 is +0x45D30 (= 500 x 572) and the stream +48,000 B, walk complete.
  The 20:28 zones are in `old_tests/g1_2028_dynent_crash`. E1/D2 had the same hole; nobody gibbed a zombie then.
- **Die Maschine's wall buys, box and perk machines still stand:** `cwlink build` keeps zm_silver's entity list
  and moves only what the .mkmap places (spawns, Juggernog). Next: drop the retail ZM objects the .mkmap doesn't
  place (the risk is level GSC that looks one up by name).
- **The icy terrain still draws**, although our gfx_map has no terrain (+1120 null), the streamerworld is empty
  (terraingfx null), and no code looks up terraingfx (0xB1) by type. The source is not found yet. Suspect: the
  gfx_map +1408 entries (3061 x 216 B, two materials each), which G1 keeps. Collision is flat at z 45.7, so the
  visual ground and the floor disagree.

**Test G1 (first deployed 20:11):** `cwlink build` / `plane` now also write mapkit's own gfx_map:
- zm_silver's, without the draw model groups, the group LODs and the terrain (0.26 MB cut);
- the remaining 1,712 draw xmodels pointed at `tag_origin` (Die Maschine links it by name from the common zones);
- 243 by-name links, 9 sanim copies and 54 script strings; 1.79 MB per override zone, walk complete;
- `--keep-terrain` keeps the terrain; `--retail-gfx` writes no gfx_map.

It was built from the user's `zm_test.mkmap`; their 19:34 zones are archived in `old_tests/e1_1934_zm_test_userbuild`.
The client now logs `world start: gfx_map '...' (mapkit's own | the map's own)` from an `R_InitWorld_cand` detour
(0x7FF727F64E50, log only; 17:21 DLL archived). Expect: no Die Maschine buildings or terrain, the sky and lighting
unchanged, and the invisible collision as in E1. Possible crashes: a consumer of the model groups, the instances or the
terrain.

**Retail map viewer (Godot).** Shares step 2. It then adds the KAPI `.xpak`/`.xsub` reader for streamed meshes
and texture mips, vertex/BCn decode, and a Godot importer. The imported geometry is a local cache: it is
generated from the player's own install, and it is never committed or put in a shipped map.

**M5: level builder. v0 DONE 2026-09-25, as a Godot plugin.** Instead of a custom C++ editor, the
editor is Godot 4 (MIT license, no install, its own viewport, gizmos, undo and glTF import). The
engine choice only replaces the editor; the compiler (cwlink) and the asset writers are the same
work either way. The plugin (`mapkit/godot`) has:
- **node types:** `MkMap` (root), `MkBrush` (box geometry; plain `CSGBox3D` works too), and the
  Zombies objects: player spawn, zombie spawner, zone, door, barrier, wall buy, perk machine,
  Mystery Box, Pack-a-Punch, power switch, Arsenal, Crafting Table, prop, light;
- **gizmos:** each object's game-size box and facing arrow, and resize handles on volumes;
- **dock:** New map (from the template), Check (click a problem to select its node), Export to
  `.mkmap`, Build (export + `cwlink build` for zm_silver, E1 PASSED);
- **F6 walk-through:** at the game's speeds and scale.

Tested headless on 4.7.2: every script loads in the editor, and the template passes Check. Export
and the space conversion match hand-computed values. The handles move one face (the opposite face
stays put) and undo cleanly. The walker stands on the brushes. Godot is Y-up and in meters; export
converts (`game = (-z, -x, y) * 39.37`).

Next for the builder (as planned on 2026-09-25; all done since, in P1 to P5): meshes (glTF from Blender)
in the source, `cwlink build` behind the Build button, and a way to start the map (the CUSTOM MAPS tab).

**Later** (as planned on 2026-09-25): baked lighting, navmesh generation for zombie pathing (done in P5,
2026-09-29), streamed textures (`.xpak`), sound, hot reload. What is still open is tracked in
[ROADMAP.md](ROADMAP.md#open-and-parked).
