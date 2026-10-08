# Mapkit plan v2: own the map, borrow only assets (2026-09-26)

> Status and milestones: [ROADMAP.md](ROADMAP.md#5-custom-maps-mapkit). This file holds each step's findings and
> test results.

The goal is unchanged ([roadmap](mapkit-roadmap.md)). A creator lays out a Zombies map in Godot: its geometry,
its models and where they go, and its gameplay objects. `cwlink build` turns that into zones, and the game
plays exactly that.

This plan changes the **route**. Up to now each M4 test edited Die Maschine: its entity list, clip map, gfx_map,
districts and terraingfx. DM's level script kept running on top of our changes. Every crash was a DM leftover
that no mapkit code owned. Every gameplay object had to fit DM's design (the Pack-a-Punch quest, its zones, the
out-of-area kill).

## Die Maschine has two roles; keep one

| Role | What it is | From now on |
|---|---|---|
| **Asset library** | The zone set loaded under the name `zm_silver`: AI types, zombie models, weapons, FX, sounds, materials, `techset_zm_silver` | **Kept.** A map references these by name. |
| **Level design** | Level GSC/CSC, entity list, trigger list, clip map, gfx_map, streamerworld, districts, lighting, navmesh | **All taken over.** Generated from the `.mkmap`. No DM bytes are copied unless every field is explained. |

The zones still load under DM's name, because that is what makes its asset library load. Nothing else of DM
survives.

## Why DM's level script runs, and replacing it

The level script is `scripts/zm/zm_silver.gsc` (the `level_init` event handler, `main`). It runs because the map
loads under DM's name; mapkit never chose it. GSC loader v2 already replaces any script by name, so `cwlink`
can generate one.

DM's `main` is about 100 lines (decompiled source: `cod-gscs/t9-gsc/bocw-source/scripts/zm/zm_silver.gsc`). A
mapkit map needs only this part of it:
- `zm::init_fx()`;
- `level.default_start_location` and `level.default_game_mode`;
- `load::main()`;
- `level.zones = []`, then `level.zone_manager_init_func` (our zone graph from the `.mkmap`) and
  `zm_zonemgr::manage_zones(<start zone>)`;
- the perk setup.

Everything else goes: the main, PaP and wonder-weapon quests, sound, VO, exfil, dog rounds, intel.

### Which scripts a map links: `script_using` (found 2026-09-26)

A zone names the scripts it links on their own (not through an include) in `script_using` assets (type 0x46,
and `_zm` 0x4A etc. with the same shape): 24 B `{u64 name, u64 gsc, u64 csc}`, 0 = none (`Load_ScriptUsingAsset`,
commented in the IDB; read from the zm_silver stream with `ffinfo --dump`). zm_silver's 17 entries:

| Script | Role | mapkit |
|---|---|---|
| `scripts/zm/zm_silver.gsc`, `.csc` | the level script | **replaced** (ours) |
| `scripts/zm/zm_silver_ffotd.gsc`, `.csc` | a patch: 21 `spawncollision` clip boxes at DM coordinates | **replaced** (empty) |
| `scripts/zm/zm_silver_zones.gsc` | its autoexec: DM's zone lists, a spawn callback, the zone challenges | **replaced** (empty) |
| `scripts/zm/zm_silver_fixup.gsc`, `.csc` | Mystery Box weapon-list fixups | kept |
| AI (`zombie`, `archetype_catalyst`, `zm_ai_catalyst`), a bgb, two vehicle scripts | asset library | kept |

Everything else of DM's (`_main_quest`, `_pap_quest`, `_ww_quest`, `_util`, `_sound`, `_vo`, the side quest, fast
travel, achievements, exfil) is linked only because `zm_silver.gsc` includes it, so our level script drops it by
not including it.

**P1 step 1: PASSED 2026-09-26 21:22** (built 19:10; two fixes below):
- `mapkit/level/zm_silver/`: our `zm_silver.gsc` + `.csc` and the three empty stand-ins. The level script keeps
  the asset-library setup of DM's `main` (clearance ceiling, FX, the weapon table `level.var_d0ab70a2`, the crawl
  entry, the announcer), boots ZM (`load::main`, the minimap, the zone manager), and powers every perk at start
  (a mapkit map has no power switch yet). Its zones are still DM's start zones, because DM's entity list is still
  loaded. It prints `mapkit: level script running` through the script print mirror.
- GSC and CSC register the same clientfields (only `player_lives`), and every system DM included on both sides
  (steiner, flashlight, fast travel, intel, dogs) is included on both sides or on neither.
- `cwlink build` compiles `mapkit/level/<map>/*.gsc|csc` with ACTS under the stock names into
  `cw-mod/maps/<map>/scripts/` (`--no-level-scripts` = DM's run, for bisecting).
- GSC loader: loads `.cscc` (ACTS `--name-client`), and each map folder's `scripts/` while `custom_maps` is on.
  Both are replace-only: a map's level script must never be injected into another map.

What the test boot should show: `Serving 'maps/zm_silver/scripts/zm_silver.gscc'` and the `.cscc` in client.log,
the banner line, no DM intro scene, VO or quest objects, zombies rising in DM's start zone, and Juggernog powered.

What the boots found:
- **19:24, dropped with "November 406 Cut Rain"**: `clientfield::register_clientuimodel(..., "int")` was refused by
  `ClientField_TypeFromString`, which compares against the engine's `scr_const_int`. Our string ids had gone stale.
  After a served script is handed over, the engine writes SL id 0x2 (the empty string) into every GetString
  operand. The writer is unknown; it is not a decrypt problem, because ACTS's `0x8B` header is the identity
  cipher in `DecryptStringToBuffer`. Fix: every scriptparsetree lookup re-checks our served scripts and
  re-interns any id a lookup of its text would no longer return. That runs before the scripts do.
- **20:10, kicked with "Clientfield Mismatch"**: both VMs must register the same clientfields. The engine prints
  the lists only to a TTY we do not have, so the loader now dumps both lists at `ClientField_Shutdown` and
  logs the difference (`Clientfield diff` in client.log). A mismatch also writes `cw-mod/clientfields_server.txt`
  and `clientfields_client.txt`. The server had 17 fields more:
  - 16 from `zm_dac_challenges.gsc`, which `zm_fasttravel.gsc` includes, while `zm_fasttravel.csc` does not
    include its client half.
  - 1 AI actor field.

  The CSC now includes the first and registers the second. What the diff means is in the header of
  `mapkit/level/zm_silver/zm_silver.csc`. Differences in version only are stock (e.g. `zm_aat` v6000 vs v1)
  and harmless.
- **21:22**: the full match runs our level script (`mapkit: level script running, start zone zone_proto_start`),
  and cwmod_devtools noclip works.

Risks to check in P1:
- **The CSC half.** `zm_silver.csc` registers the client side of the clientfields. GSC and CSC registrations
  must match, so cwlink generates both. The loader serves `.cscc` (PASSED 21:22). The two lists must be equal
  over everything linked, not only the level script's includes (see the 20:10 finding above).
- **Pack-a-Punch.** zm_common's `zm_pack_a_punch` wants a zbarrier entity with targetname `zm_pack_a_punch`.
  Neither zm_silver nor zm_gold has one in its entity lists (checked 2026-09-26, `ffinfo --entities-json`).
  So T9 brings the machine in another way (a quest or the content system), and that path has to be traced.
  Once the level script is ours, the DM quest is no longer in the way. **Traced 2026-09-27: it is the content
  system** (P1 step 3 below).

### P1 step 2: the level's own entity and trigger lists (PASSED 2026-09-27)

`cwlink build` writes both lists from scratch (`ComposeLevelEntities`), and the zone graph as GSC.

**Zones are volume zones.** DM's zones are *node* zones: path nodes named after the zone, whose first node's
`target` names the zone's zombie spawn structs. Nodes cannot be authored:
- the node array (176-byte nodes: type @40, origin @20) is compiled into the `game_map` asset and bound to the
  navmesh (each node's navmesh face is looked up at load);
- a `node_*` entity in the entity list only sets the script fields of the *next* node of that array, in order
  (`G_SpawnMapEntity_cand` → `sub_7FF7230C6FC0` + `sub_7FF7230D13A0`; `getnodearray` = `sub_7FF7230F28F0`).

zm_zonemgr supports volume zones too (the two `zm_utility` switches `function_21f4ac36` nodes /
`function_c85ebbbc` volumes both default to on, and every lookup tries volumes first or falls back to them):
- a zone's volumes: `info_volume`s in the trigger list with targetname = the zone;
- its spawn structs: the first volume's `target`;
- the play area: every volume with script_noteworthy `player_volume`.

With no node named after a mapkit zone, zm_common uses the volumes throughout. DM's `node_*` entities are left
out, so DM's `player_region` nodes no longer count as play area.

**The trigger list's shapes** (the three arrays mapkit had not decoded; same layout as Black Ops 3's
MapTriggers, `map_entities.hpp`):
- models (8 B: contents, hull count, first hull), hulls (32 B: mid point, half size, contents, slab count, first
  slab), slabs (20 B: a plane direction, mid point, half size);
- trigger-list entity *i* uses model *i* (`G_SpawnTriggers` 0x7FF723F2C3B0 sets ent+656 = i);
- a hull is relative to its entity's origin (all 357 of DM's have their mid point near 0);
- a turned box keeps angles 0 and gets two slabs.

**What the lists hold:**
- entity list, kept from DM: worldspawn, the 5 AI spawner templates (`actor_spawner_*`), the initial spawns
  (moved), and the perk, box, wall-buy and Pack-a-Punch structs the source places (moved, as in P1
  step 1), with their *content parents*. The box, wall buys and Pack-a-Punch are content structs, and
  `content_manager.gsc` reaches them only from the top: `content_destination` ← `content_location` ←
  `content_instance` (e.g. content_script_name `magicbox`) ← `content_struct` (content_key `magicbox_zbarrier`),
  each linked by `target` = the parent's targetname (the key is `variantName`). A struct without its parents
  spawns nothing. That was the first test's missing box (2026-09-27). Both of DM's destinations
  (`zm_silver_destination`, `dest_dp`) stay too, since the map's fields name destinations. DM's other content
  objects (ammo cache, armor machine, Wunderfizz choice, crafting table, exfil) are left out with their chains;
- entity list, generated: a `player_respawn_point` per zone (target `initial_spawn_points`), a `riser_location`
  struct per zombie spawner (targetname `<zone>_spawns`, script_string `find_flesh`: straight for the players,
  no barrier to tear down), and two minimap corners around the zones;
- trigger list: one `info_volume` per zone (player_volume, contents 0x80000001, as DM's);
- about 2,700 of DM's entities and all 327 of its triggers are left out: its level script no longer runs, so
  nothing looks them up.

**The zone graph** is generated GSC: `scripts/zm/zm_silver_zones.gsc` (namespace `mapkit_zones`,
`start_zones()` and `init()`), compiled in place of the stand-in and kept readable in
`<map folder>/generated/`. Start zones connect to each other always. A zone a door opens connects to the
zones the door's box touches (else the start zones) under the flag `mapkit_door_<n>`, which nothing sets
until doors are built.

**Left out for now:** barriers (a barrier spawner rises from the ground instead), the power switch (perks
powered at start). Doors were added on 2026-09-27 (below), props too (P2 step 1).

**Doors (PASSED 2026-09-27).** zm_silver's 25 doors are a `trigger_use_touch` in the trigger
list (targetname `zombie_door`, `script_flag` = the zone flags it sets, `zombie_cost`, contents `0x82000000`)
whose `target` names its pieces: script_models whose model's own collision blocks (`script_noteworthy
"model_clip"`) and that `zm_blockers` moves by `script_vector` when bought (`script_string` `slide_apart`,
`move`, `rotate`, `dynamite`). cwlink's `AddDoors` writes the same for each MkDoor:
- the trigger over the door's box, 40 units deeper on each side, `script_flag` = `mapkit_door_<n>` (the flag the
  generated zones script already connects the door's zones on);
- one piece: the MkDoor's model (default `p9_zm_ndu_door_metal_gray_rusted`, a door zm_silver slides open) at the
  node, `model_clip`, `move`, `script_vector` straight down by the box's height plus 8, so it sinks into the
  floor;
- the model in mapkit's bgcache (`BG_Cache_Register_cand` skips names already listed, checked in IDA).

The model is what blocks, so the box itself does not: a door model narrower than the opening leaves a gap.
`core_common` and `zm_common` ship invisible collision models (`collision_clip_wall_64x64x10`,
`collision_wall_*_standard`, `collision_geo_*`), which could fill the box; their collision is streamed and its
shape and pivot are not decoded yet.

What the test boot should show: the banner `mapkit: level script running, start zone start_zone, 2 zones`
(the template), zombies rising only at the start zone's spawners, the box and perks where placed, and no
error from the zone manager.

### Devices: ammo cache, Armor Station, Wunderfizz, crafting table, exfil (PASSED 2026-09-27, the exfil untested)

**What they have in common.** All five are `zm_common` objects: their models (`p9_usa_large_ammo_crate_01_sur`,
`p9_fxanim_zm_gp_armor_station_xmodel`, `p9_fxanim_zm_gp_wunderfizz_on_xmodel`, `p9_fxanim_zm_gp_crafting_xmodel`, the
radio `p9_zm_radio_pack_01b_surface`) and their scripts are in `zm_common.ff`, which every Zombies map loads, and the
scripts are always linked (`zm.gsc` includes the ammo cache, Wunderfizz and armor scripts, the perk scripts the crafting
one, the round-based game mode `zclassic` the exfil one). A map only supplies *where*: content structs under the
content chain (destination ← location ← instance ← struct, see P1 step 2). At `start_zombie_round_logic` each device
script walks the **first** `content_destination`'s locations for its instance and spawns one device per struct:

| device | script | instance / struct content_key | struct keys read |
|---|---|---|---|
| ammo cache | script_5a525a75a8f1f7e4 | `ammo_cache` / `ammo_cache_spawn` | `radius`, `height` (use trigger, default 80 / 48) |
| Armor Station | script_7a5293d92c61c788 | `armor_machine` / `armor_machine` | `script_noteworthy "power"` + `script_int` (needs power) |
| Wunderfizz | script_25be5471a9c31833 | `perk_machine_choice` / `perk_machine_choice` | none (needs power only if `level.var_9d96d174`) |
| crafting table | script_4ccfb58a9443a60b | `crafting_table` / `crafting_table` | none |
| exfil | script_7b1cd3908a825fdd | `exfil` / heli_spawn, exfil_loc, landing_zone, smoke, the path starts | level fields (below) |

Each device's front is its struct's +X (the use prompt goes 24 units along `anglestoforward`). cwlink moves
zm_silver's structs to the source's nodes and copies them past zm_silver's count (7 ammo caches, 1 station, 1
Wunderfizz, 2 tables), so any number works. The Armor Station's `"power"` key is dropped until the power switch exists.

**The crafting table** is a salvage shop, not a recipe table: the menu (`sr_crafting_table_menu`) sells a fixed list at
the game's prices. 19 of its items are switched by game settings the menu checks (`zmenablefraggrenade`,
`zmenablec4`, `zmenablescorestreakchoppergunner`, ...; names recovered from their hashes, list in cwlink
`kCraftingItems`), and a level script can set those (`setgametypesetting`, as MP maps do). The settings are
map-wide, so several tables share one list. "Parts x, y, z make A" is a different system: zm_crafting's blueprints
(`craftblueprint` bundles: `result` weapon, `component01..N` part items, `postcraft`; a `craftfoundry` bundle per
table, found through a `crafting_trigger` trigger whose field names the foundry). The shipped blueprints are the
quest ones (Aetherscope, ...). Custom recipes would need new bundle assets, or (planned, simpler) mapkit's own build
table in generated GSC: part pickups (any game model) and a table that takes them and gives a weapon, a perk, or
opens something.

**The exfil.** Round-based: `zclassic` makes the radio (`exfil_radio` struct, content_key `beacon`, found by
targetname) live for 120 s after rounds 10, 15, 20, ...; using it starts the exfil instance. The helicopter spawns at
heli_spawn and flies three vehicle paths found with `getvehiclenode(<struct targetname>, "target")`: in (20 nodes),
a loop round the landing point (11), the landing (2). Players must reach the zones in `level.var_ad5e81fe`; the
timer is `level.var_aaf7505f` (zm_silver 90 s); zombies come from the `survivalailist` bundle
`level.var_dafeed10 + clamp(floor(round/5), 1, 4)` (zm_silver's `exfil_silver_`: 38 / 45 / 56 at rounds 10 / 15 /
20+, plus 30% per extra player) within `level.var_26ed6a07` (600) and `level.var_c86f12d4` (200) of the landing
point. **Vehicle nodes are built from the entity list at load** (IDA 2026-09-27: `G_SpawnMapEntity_cand` calls
`VehNode_SpawnFromMapEnt_cand` 0x7FF723D0A600 for `info_vehicle_node[_rotate]`: a 0x5C-byte node per entity, up to
2000, every key parsed by `VehNode_ParseMapEntKeys_cand`), unlike path nodes, so paths can be moved or authored.
cwlink moves and turns zm_silver's exfil instance with its three paths so the helicopter lands at MkExfil facing its
arrow (zm_silver's lands facing exfil_loc's yaw), and writes the level fields into `mapkit_zones::settings()` in the
generated zones script. The level also keeps every vehicle node: P1 step 2 had dropped them, which left the chopper
gunner and napalm strike without their paths.

**Not configurable yet:** device prices (the game's own), the exfil's zombie count and mix (only zm_silver's waves are
loaded; a custom count needs our own `survivalailist` bundle, the same writer custom blueprints would need), the round
exfil opens (zclassic's schedule), power (comes with the power switch), more than one exfil point (the game picks one
at random among several instances; mapkit places one).

### P1 step 3: Pack-a-Punch (PASSED 2026-09-27)

**Cold War's Pack-a-Punch is a content object, not the zbarrier machine.** `zm_pack_a_punch.gsc` (the one that
wants a `zm_pack_a_punch` zbarrier) is carried over from Black Ops 4, and no T9 zone ships a Pack-a-Punch
zbarrier: zm_silver's and zm_common's zbarriers are the box and the window barriers only. The machine comes
from the `weapon_machine` content instance:
- zm_common `script_6fc2be37feeb317b` (namespace_4b9fccd8) spawns, per `weapon_machine_spawn` struct of the
  first `content_destination`, the machine `p9_fxanim_zm_gp_pap_xmodel` (`content_manager::spawn_script_model`,
  turned as the struct is, front +X, use prompt 24 units forward and 50 up);
- its menu, `sr_weapon_upgrade_menu`, sells the tiers (`level.var_af0de66` = 0, 5000, 15000, 30000) and the ammo
  mods (napalm burst, cryofreeze, brain rot, dead wire);
- it spawns at `start_zombie_round_logic`, but first waits for `level waittill(level.var_ce45839f)` if that is
  defined. zm_silver's quest sets it to `#"pap_quest_completed"`, so in Die Maschine the machine appears only
  after the quest. mapkit's level script does not set it, so **the machine is there from the first round**;
- the flag `disable_weapon_machine` hides it (Die Maschine sets it in the Dark Aether); nothing sets it in a
  mapkit map. `getgametypesetting(#"zmpapenabled")` must be on (it is in the normal mode).

Die Maschine's quest shows a stand-in at the `zm_go_mac` struct (`p9_fxanim_zm_sur_machine_weapon_mod`) and
deletes it when the quest ends; that is not the machine.

**The Arsenal is the Armor Station.** The `armor_machine` content object (`script_7a5293d92c61c788`) opens
`sr_armor_menu`, which handles `armor_purchase` and `tier_upgrade` (weapon rarity, white to orange, for
salvage). mapkit's `MkArsenal` had been mapped to `weapon_machine_spawn`, so it was placing the Pack-a-Punch.

**What cwlink does now:** `pack_a_punch` is a device like the ammo crate (`weapon_machine_spawn`: zm_silver's one
struct moved, copies past it, yaw = the node's facing). `arsenal` builds the same thing as `armor_station`
(`armor_machine`). The old marker (a `zm_go_mac` copy that a devtools script was to build a machine at) is gone.
Dry run on the device test map: `pack-a-punch: 1 placed`, `arsenal (armor station): 4 placed (1 moved, 3 copies)`,
and the struct keeps its chain (struct → `weapon_machine` instance → `weapon_machine_location` →
`zm_silver_destination`).

What the test should show: the Pack-a-Punch machine at the MkPackAPunch node in round 1, its prompt on the side
the node's arrow points to, and the tier upgrade for 5000 points.

### P2 step 1: props as script_models (PASSED 2026-09-28: drawn and solid)

Each `MkProp` becomes a `script_model` in the level's entity list (`AddProps`, cwlink):
- its model by hash, at the node's origin, turned by all three of the node's angles;
- `modelscale` (a float key, type 5) from the node's scale. Godot exports the mean of the node's three scales as
  `props.scale`, and Check warns about an uneven scale, since the game scales a model evenly;
- a copy of one of zm_silver's script_models: `ServerSide`, with `modelscale`, `spawnflags 1` and `DYNAMICPATH 1`
  (87 of its 140 script_models carry `modelscale`);
- drawn through mapkit's bgcache, as the doors are.

**Solid, first test (2026-09-27): all 11 props drew, but the sandbags, the debris piece and the ammo crate were
walk-through** (the others were not checked). Traced in IDA on 2026-09-28:
- a `script_model` collides only through its models' own xcollisions. `SP_ScriptModel_cand` 0x7FF72848ECA0 sets
  contents 0x2080, then ORs `XCollision_GetContents` 0x7FF7293EFBD0 over the entity's models (its DObj,
  `g_serverObjMap_cand`);
- the xcollision's contents are `(q | q >> 26) & 0x3FFFFFF`, with q the u64 at xcollision +88. All nine test models
  have an xcollision of their own, and all are streamed (+16 stream key, nothing at +24);
- the three that were walk-through have contents `0x1` only. The door and terminal have `0x131651` (both are entity
  models on zm_silver), the locker and pallet `0x131641`, the table and wood crate `0x130601`;
- zm_silver collides with its scenery through its streamed world cells, which mapkit empties.

**The fix (PASSED 2026-09-28: every prop blocks except the one with solid off):** each solid prop also gets a hull in the world collision tree, as a brush
does (`PropHulls` in cwlink, `MeshHull` in zonekit):
- the hull is the 26-DOP of the model's full-detail mesh: a plane across each of the model's 3 axes, 12 edge and 8
  corner directions. It is made in the model's own axes, then turned, scaled and moved as the prop. A boxy model
  gets a tight box; a concave one (a table) is filled in;
- the models come from zm_silver and zm_common (`ModelLibrary`, both traces; the build takes about 4 s);
- the model's bounds stand in when its mesh does not decode;
- dry run: all 10 hulls sit on their props, with 20 to 24 extra planes each.

Solid props also keep `DYNAMICPATH` (`spawnflags 1`). On zm_silver, the 39 script_models that carry it are all
debris pieces. **Guess:** it makes the entity cut the navmesh, so zombies path around it. Not checked yet.

**Not solid:** targetname `mapkit_prop_nonsolid`, and the generated zones script's `settings()` calls
`notsolid()` on each.

**Only models from loaded zones draw** (zm_silver, zm_common, core). cwlink warns about a model that is not in
zm_silver's list.

**Why script_models first:** they use only what has already passed in-game (entity models plus bgcache). The
retail way to place static props is a districts stream key, not an entity (see
[mapkit-research-parked.md](mapkit-research-parked.md)). Every script_model is a networked entity, and zm_silver
itself places 140 of them. A map with hundreds of props will need the static path (step 2); the entity budget is
not measured yet.

**The test map:** `zm_proptest`, built into CUSTOM MAPS as 'Prop test'. It is zm_latest plus 10 props in room_2:
- solid, standing: a rusty barrel, sandbags turned 90°, a wood crate at yaw 30 and scale 1.5, a locker, a table,
  and an ammo crate at scale 2;
- models zm_silver places as entities: the trial terminal and an explosive-door debris piece;
- a red barrel lying on its side (roll 90) that is **not solid**;
- a pallet tilted 20° in pitch.

The test answers:
1. whether models zm_silver places only statically also collide as a script_model. **No, for the ones whose
   xcollision says only 0x1** (above);
2. whether `modelscale` scales the collision too. The hulls are scaled by cwlink, so this no longer matters for
   solid props;
3. whether zombies path around a solid prop (`DYNAMICPATH`);
4. whether the two models that are not in zm_silver's zone (`barrel_rust_01_green`, and zm_latest's own prop
   `c_t9_usa_chopper_pilot_02_body`) draw from zm_common. **Yes, both drew.**

**Step 2 (next): static placement.** Write placement records into a districts stream key the way retail does:
an entry plus a 64-byte record (origin, scale, bounds, quaternion) per prop. That needs the open points listed
in the parked research: how districts reach the key, and whether an inline payload is enough.

### P3 step 1: the creator's own meshes (PASSED 2026-09-28: drawn and solid)

A model made in Blender (exported as `.glb`, imported by Godot) goes under an `MkMesh` node:
- **Export:** `MkExport` writes every mesh under the node as a part of triangle surfaces, in map space and game
  units, with its normals, UVs and material name (`meshes` in the `.mkmap`; format in mkmap-format.md). Godot's
  front faces are clockwise, as the game's are, and the axis swap keeps handedness, so the triangles go as they are.
  A mirroring transform swaps them back.
- **Drawing:** cwlink adds the surfaces to the level model mapkit already draws its brushes with (the resident
  xmodel placed by one script_model; `AddBrushModel`), split at 65535 vertices, with tangents from the UVs.
  - A surface whose editor material is named after a game material zm_silver stores draws with it (Blender
    material "mc/mtl_p7_barrier_block_concrete_rusty").
  - Any other draws with the node's `game_material`, else the default.
  - If a surface's normals say most triangles face the other way, cwlink turns the surface round and says so
    (`FrontClockwise`).
- **Collision** (`collision`), in the world tree like the brushes:
  - `faces`: one `TriangleHull` per triangle, a slab 4 units deep behind the front face. Exact for concave shapes
    (a room, a ramp).
  - `hull`: one `MeshHull` (26-DOP) per part.
  - `none`: walk-through.
  - The tree has no BVH, so the engine walks every hull on each trace. Check warns past 4000 faces.
- **Walking in Godot (F6):** `MkMesh` adds trimesh (faces) or convex (hull) collision.

**The test map:** 'Mesh test' (`zm_meshtest`, built into CUSTOM MAPS). It is zm_latest plus a room made as one
concave mesh, from a `.glb` written the way Blender writes them (scratchpad `make_room_glb.py`; glTF front
faces are counter-clockwise, Godot's import turns them round). The room sits just east of room_2 (x 768 to 1168 of
the map, entered through a doorway in its west wall), and a zone `mesh_room` covers it:
- floor 400 × 416 in grey tiles; 160-high walls in rusty concrete block;
- a pillar, and a ramp up to a 48-high platform, drawn with "Blender_Default", which falls back to the node's
  `game_material` (concrete pillar);
- dry run: 116 triangles, 116 hulls (122 extra planes), no surface turned round.

What the test should show:
- the room drawn with the three materials, seen from inside and out (it is not see-through);
- walls, pillar and platform block;
- the ramp can be walked up;
- players can't fall through the floor.

**Result (2026-09-28): all four passed.** The user then made a map in the Godot project with their own `.glb` under
an `MkMesh`: it was drawn and solid, and scaling the node scaled both the model and its collision in the game. The
`.glb`'s own materials don't show: a surface draws only with a game material zm_silver stores (named in Blender, or
the node's `game_material`). Drawing the model's own textures is P4.

**Not yet:** lighting for the mesh (it is lit like the brushes, by the level's probes); a BVH for big meshes; its
own textures (P4).

### P4 step 1: the creator's own textures (PASSED 2026-09-28)

A mesh surface whose editor material is no game material now draws with its own look: its albedo texture, or a
swatch of its albedo colour when it has no texture.

**How the game stores them (read 2026-09-28, IDB renamed and commented):**
- **Images** (208 B; Greyhound's BOCWGfxImage matches): +156 DXGI format, +160/+162 size, +184 mip record count.
  Die Maschine's world images are STREAMED (+144 & 0x10): the zone keeps only a 32 x 32 tail chain at +8, the
  larger levels come from the packages by the mip records' keys. About 60 images are RESIDENT (flags 0x20/0x22):
  every level back to back at +40 in block 6, no mip records. That is what mapkit writes.
- **The resident path:** `Load_GfxImage` calls `Image_PostLoad_cand` 0x7FF729114050, which builds the GPU texture
  at once for a resident image (`Image_CreateResident_cand` 0x7FF729113680: format, size, depth +164, type +181,
  levels +182; the levels are read back to back from level 0). `DB_Image_LinkCallback` 0x7FF7295ED880 does nothing
  for a resident image. Full field notes: `zonekit/material_writer.hpp`.
- **Materials:** the image table is 24 B per image, {image, u32 semantic, 1.0, 1.0, u32}; semantics A0AB1041 =
  colour, 59D30D0F = normal map (x, y in r, g; b varies like a gloss; a = 1), EB529B4D = a detail normal map tiled
  10x (the cinder block's). Texture sets (+64..) list each image's +148 id for streaming. Retail zones SHARE
  identical pieces between materials (the constant buffer, the +56 list, texture set data): a stored reference to
  data an earlier asset loaded. zonekit now resolves such references to bytes (`ResolveStoredData`: finds the asset
  that loaded the position from the trace and replays it with a load log).
- ffinfo: `--material <name|#hash>` (the whole material, each image's header, `--dds <dir>` writes the mips a zone
  keeps as .dds, game data: scratchpad only), `--images` (every image by format and residency),
  `--resident-materials`.

**What mapkit writes:**
- **The editor** (`MkExport.write`): per surface, `color_texture` = a .dds next to the .mkmap
  (`<map>_textures/<hash>.dds`, DX10 header). An imported texture already block-compressed with mipmaps (Godot's
  VRAM compression) goes as it is; any other is compressed to BC1/BC3 with mipmaps; a colour is a 4 x 4 RGBA8 swatch.
- **cwlink** (`OwnMaterials`): one resident image per texture (the sRGB form of its format, the .dds's levels; the
  GPU gets those while both sides are at least 4, at most 7, as Die Maschine's 2048 image uses 7), and one material
  copied from `mc/mtl_p7_concrete_pillar_damage` (techset B18667B013CD008A: colour, normal map, one shared global
  image) with every shared piece written inline, its colour map and normal map replaced (a flat 4 x 4 normal map
  mapkit writes, 128 128 118 255), and its texture sets pointed at the new images. The techset and the shared image
  stay by-name links.
- A surface's material: its name when that is a game material; else the MkMesh's `game_material` when set; else
  its own look; else the default.

**The test map:** 'Texture test' (`zm_texturetest`, built into CUSTOM MAPS 18:22). The mesh room again at x 768 to
1168, now made with its own materials (scratchpad `make_textured_room_glb.py`): brick-textured walls, a checker floor,
a plain blue pillar, a plain orange ramp and platform. Inside it, the user's own car model (a Sketchfab-style .glb,
22 parts, 5636 triangles, scale 0.4, `hull` collision): 8 plain colours and two decal textures (BC3, 1048 x 268 and
532 x 236). It was imported by Godot's editor path (headless `--import` of a scratchpad copy of the project), so the
textures went through Godot's own VRAM compression. 12 materials, 1.66 MB zones.

What the test should show:
- the walls brick, the floor a black and white checker, the pillar blue, the ramp orange;
- the car in its colours (blue body, black tyres), its plate and logo decals readable;
- nothing see-through or flickering; the map loads with no error.

**Result (2026-09-28): all four passed.** Resident images and copied materials draw: the textured room and the car
in their own colours, the decals readable.

**Not yet:** the material's normal and roughness maps (a flat normal map for now), transparency (a glass or decal's
alpha draws opaque), a texture's tint colour (the texture is drawn untinted), lighting.

### P4 step 2: normal and roughness maps (PASSED 2026-09-28)

A mesh surface's own look now includes its normal map and its roughness (a texture channel times the roughness
value, or the value alone).

**How the game stores them (read offline 2026-09-28, no IDA needed):**
- `ffinfo --semantics` counts every image semantic Die Maschine's materials use; ACTS names them
  (`acts -t lookup`): colorMap0 A0AB1041, normalMap0 59D30D0F, **perceptualRoughnessMap E9817F0D** (BC4, in 3427
  materials), specColorMap0 EC443804 (BC1 sRGB or BC4), detailMap0 EB529B4D, thermalHeatmap0 389DD40F (a shared
  image), tintMask0, emissiveMap0, alphaMaskMap0, the detail-layer maps and more. 07176BF2 (BC4, 3214 materials,
  most of them sharing one by-name image) is unnamed.
- **Normal maps' b channel is not gloss:** on Die Maschine's one full-chain resident normal map (2048, asset 141948)
  b's mean climbs level by level (0.14, 0.29, 0.39, 0.47, 0.54, 0.58, 0.61) while the normals' mean tilt shrinks.
  It is the roughness a level's spread of normals adds (specular anti-aliasing): parent b² ≈ children's b² + about
  2 × their normals' variance.
- **Green points down the image (DirectX):** `ffinfo --mesh` checks each retail mesh's stored tangent frames against
  its UVs. On 7 meshes (8800 triangles) the tangent runs along +dP/du and sign × cross(normal, tangent) along +dP/dv
  (v down the image), as mapkit writes them. A curl test on 19 textured retail normal maps agrees on 16. glTF's and
  Godot's normal maps point green up, so export flips it.
- Image header usage byte (+180): 7 on roughness maps (5 on a BC4 specColorMap0).
- **The template:** `mc/mtl_com_trash_props_iw6`, a world prop's material (techset F79F5E9BABE46117, which 301 of the
  roughness-map materials use): colour, normal and roughness maps plus two shared images (thermal and 07176BF2) that
  stay by-name links.

**What mapkit writes:**
- **The editor** (`MkExport`): per surface also `normal_texture` (the normal map converted: green flipped, scaled by
  `normal_scale`, its own mip chain with b = sqrt(2 × (1 − |mean normal|²)), BC7) and `roughness_texture` (the
  roughness texture's channel × `roughness`, BC4; a 4 x 4 swatch when there is no texture). Each conversion is cached
  for the editor session.
- **cwlink** (`OwnMaterials`): one resident image per texture (usage 2 / 4 / 7), one material per colour, normal and
  roughness combination copied from the template. No normal map: a flat one (128, 128, 0, 255); no roughness map:
  fully rough. Without the template in the base map, the concrete pillar again (no roughness).

**The test map:** 'Texture test' again (`zm_texturetest`, rebuilt 19:22; the 18:22 build is in the scratchpad),
from scratchpad `p4/make_textured_room2_glb.py`:
- walls: brick with a normal map (bevelled bricks, sunk mortar) and a roughness map (mortar rougher);
- floor: checker with a roughness checker (dark tiles 0.1, glossy; light tiles 0.9);
- pillar: blue, roughness 0.15 (shiny); ramp and platform: orange, roughness 1;
- panels **A** and **B** on the north wall (left when you come in): 4 x 4 studs; B's normal map has green inverted.
  The studs should look raised on A and sunk on B;
- the user's car as before.

What the test should show:
- the mortar looks recessed and the bricks raised;
- the dark floor tiles and the pillar are shiny, the light tiles and the ramp matte;
- panel A's studs look raised, panel B's sunk;
- nothing black, flickering or see-through; the map loads with no error.

**Result (2026-09-28): passed.** The mortar looks recessed and the bricks raised; the dark tiles and the pillar shine,
the light tiles and the ramp are matte; panel A's studs look raised and B's sunk, so the green flip is the right way
round (and b = the mip spread, 0 at level 0, draws without artefacts). Nothing went black, flickered or went
see-through, and the map loaded cleanly.

### P4 step 3: transparency, metal and tint (PASSED 2026-09-28)

A mesh surface's own look now also has its tint, its metallic and its transparency. The user then asked for these
three first and sky and lighting later.

**How the game does them (read offline 2026-09-28, no IDA):** `ffinfo --material-table` writes every Die Maschine
material as one line (name, techset, image slots, root, constants); ACTS named 19464 of the 20844 materials, and
`--model-materials` lists each xmodel's per-LOD material lists.
- **The root says the kind:** +330/+331 is the draw class (02 00 opaque, 06 01 decal, 1A 14 see-through, 09 13
  emissive); +324 02 marks a cut-out (barbed wire, grates, foliage: an alpha test on the colour map's alpha, +325
  03..0B); +36 0x10 glass; +32 >> 20 the surface type (5 concrete, 9 glass, 13 metal, 15 paper, 21 wood, 24 plastic).
  The techset picks the image slots. Copying a whole retail material of the right kind carries all of it.
- **Metal:** on the techsets that share mapkit's slots, specColorMap0 (EC443804) is a BC4 **metalness**: Die
  Maschine's paper and books average 0.05 (a few metal bits at 1), the Pack-a-Punch's green-painted panels about 0.1,
  its bare frame 0.33. Other techsets take an sRGB specular colour (BC1) there instead. glTF's metallic maps straight
  onto it.
- **The other slots are shared images** (ACTS names): 07176BF2 ambient occlusion (`$white_ao`, `$occlusion`),
  199A03D3 a reveal mask (`$white_reveal`, `$reveal`), 389DD40F thermal (`$gray_32_one_channel`); some materials
  hold their own AO map there. A template must link the neutral ones, or its occlusion would show on the creator's
  surface; swapping the link is not safe (texture sets record image ids of the other zone).
- **Cut-outs' `mcdp/` twins:** most cut-out materials have a `mcdp/<name>` twin (same images, a depth techset), in
  the xmodel material table's second list (+16). 96 of 326 retail cut-out surfaces have none there, so mapkit
  writes none.

**What mapkit writes:**
- **The editor** (`MkExport`): the colour texture times the albedo colour (tint baked in; sRGB bytes multiplied);
  `alpha` per surface from Godot's transparency (scissor and hash: `clip`, with alpha made 0 or 1 at the material's
  threshold so the game's own threshold cuts the same edge; alpha and depth pre-pass: `blend`); `metal_texture` (the
  metallic texture's channel times metallic, BC4, or a swatch; none when metallic is 0).
- **cwlink** (`OwnMaterials`): a template per kind, each checked to link only the neutral shared images, the first
  the base map can copy; a kind without one falls back to a simpler kind:
  | kind | template (fallback) | techset |
  |---|---|---|
  | opaque | `mc/mtl_com_trash_props_iw6` (`mc/mtl_p7_concrete_pillar_damage`) | F79F5E9BABE46117 |
  | metal | `mc/mtl_p7_debris_metal_scrap_04` (`mc/t7_metal_bare_iron_matte`) | 181EC03271DE722E |
  | cut-out | `mc/mtl_greece_classic_furniture_brown_destory` (`mc/mtl_trash_debris_paper_01_alt_forms_burnt`) | 2FE9627E1FFFE887 |
  | metal cut-out | `mc/mtl_p9_rus_machine_generator_02_strainer` (`mc/mtl_p7_corrugated_sheet_metal_rust_dmg`) | 20F0DC7D0A756A6D |
  | see-through | `mc/mtl_p9_zm_ndu_ieu_locker_glass` (`mc/mtl_wpn_t7_zmb_zod_rocket_shield_glass`) | 5CC72245358C6B13 |

  A see-through surface ignores a metal map. The surface type comes with the template (metal kinds are metal, the
  see-through one glass).

**The test map:** 'Texture test' again (`zm_texturetest`, rebuilt 21:12; the 19:22 build is in the scratchpad), from
scratchpad `p4/make_textured_room3_glb.py`: step 2's room plus
- a **glass** pane (x 280, south half, 80 wide): light blue, mostly clear, a dark frame and a frosted diagonal band;
- a **fence** pane beside it: a wire grid, the rest cut out;
- a steel **grate** on the south wall (x 200..264, 4 units off it): bricks through its holes; metal;
- a **tint** panel on the south wall (x 30..94): the floor's checker tinted green;
- a **gold** block near the east wall, and a **checker block** near the doorway: red paint squares (matte, not
  metal) and steel squares (shiny metal);
- the car: its glass, licence plate and window logo now see-through; its chrome and metal parts metal.

**Result (2026-09-28): all six passed, as described:** the glass is clear with its frame and frosted band, the fence and
the grate are cut out, the tint panel's light squares are green, the gold block, the steel squares and the car's
chrome are metal, the car's glass and decals are see-through; nothing black, flickering or missing.

**Not yet:** emission (glowing surfaces; done in the leftovers below), a see-through metal, surface types chosen by the
creator.

**Found afterwards (2026-09-28):** three of these templates link a replaced slot by name, not as their own image (the
metal template's colour map, the metal cut-out's roughness and metal maps, the glass's normal map). A texture set's
data names the images by their +148 id, and cwlink can only rewrite the ids of images the base map stores; a by-name
image's id is unknown there (retail ids are not the name's low 32 bits: 0 of 51443 in zm_silver, 0 of 53753 in
zm_common, `ffinfo --images`). The gold block still drew gold, so those ids do not pick the texture drawn.

### Leftovers: barriers, power switch, glowing surfaces, loose models, exfil test (built 2026-09-28; power switch and exfil PASSED 2026-09-29; the wood barrier works since P5; the concrete barrier and glow parked)

**Window barriers** (MkBarrier, `kind` wood or concrete). How zm_common runs them (`zm_blockers.gsc` blocker_init,
`zm_behavior.gsc` findnodesservice and the barricade mocomps): an `exterior_goal` struct targets a zbarrier entity
(the boards, their tear and repair animations; zbarrier asset from zm_silver) and shares a `script_string` with it. A
zombie spawn struct whose `script_string` is that string sends its zombies to the goal (the nearest navmesh point
within 128). They tear the boards down from there and climb in along the `barrier_align` struct in noclip, so the
climb passes through whatever the wall is. All 25 of zm_silver's barriers use one layout around the zbarrier, whose +X
points OUT:
- `exterior_goal`: 38 out, 16 up, facing in;
- `attack_spots`: 38 out, 31 up;
- `barrier_align`: 1 in (the concrete one has none);
- `trigger_location` (radius 36, height 64): 9 in, 23 up, where players rebuild;
- two path nodes, left out.

cwlink `AddBarriers` copies one such group per MkBarrier, moved and turned as one: the zbarrier on the node, facing
away from the node's arrow, and the group's targetname and script_string = `mapkit_barrier_<n>`. A barrier spawner
(MkZombieSpawner kind barrier) gets the nearest barrier of its zone as its struct's `script_string`, instead of
`find_flesh`.

**The power switch** (MkPowerSwitch). zm_power `electric_switch` wants a `trigger_use` "use_elec_switch" whose target
names an `elec_switch_fx` struct (the sparks). It turns an entity of that name with script_noteworthy `elec_switch`
through 90 degrees of roll, but zm_silver's console (script_model 41708, `p9_zm_ndu_power_on_switch`, front +X) is not
targeted, so nothing turns. Using the switch sets flag `power_on`, unpauses the perks and opens the doors whose trigger
has script_noteworthy `electric_door`. cwlink `AddPowerSwitch` places copies of the console and the sparks struct, and
a use trigger 48 deep in front of the console (64 wide, 80 high). The console blocks through a hull, as solid props
do. With a switch, the generated `settings()` sets `level.vending_machines_powered_on_at_start = 0`, so the perk
machines wait (zm_perks `get_perk_machine_start_state`; Quick Revive still works in a solo game). Two options follow:
- MkDoor `needs_power`: the trigger gets `electric_door`, and the door opens when the power comes on, with no price;
- MkArmorStation / MkArsenal `needs_power` (default on): the struct keeps zm_silver's script_noteworthy `power`, and the
  station shows "need power" until the flag is set.

Without a switch, both options are ignored.

**Glowing surfaces.** emissiveMap0 = `34614347` (named by ACTS). Draw class 09 13 is opaque with emission. The template
is `mc/mtl_p9_zm_ndu_sign_do_not_enter_light` (fallback `..._medical_lab_light`), Die Maschine's lit signs, on techset
2563079AB548D2FD. It holds colour, normal, roughness and emissive maps, plus the neutral AO and thermal images. Across
that techset's 13 materials:
- constant +292 goes with how bright each thing looks: signs 32, the Tempest lure lights 8, the antenna lights 64, a
  Russian computer 256, the mutated fungus 2. cwlink sets it to 32 × the editor's emission energy. This is a guess from
  the data, and the test checks it;
- +4..+12 hold the glow's tint (1, 1, 1 on the signs, red on the red lights). cwlink leaves them as the template's,
  because the editor bakes the colour into the emissive map.

The editor writes `emission_texture`: Godot's emission colour times its emission texture (Multiply, which glTF imports
use) or plus it (Add), else a swatch. It also writes `emission_energy`. A glowing surface is opaque and not metal. A
cut-out or see-through surface drops its glow, with a message.

**Loose models.** A model dropped into the map without an MkMesh now exports as one with the defaults (collision
faces, its own look): `MkExport.loose_meshes`, one mesh per node right under the map. MkCheck warns past 4000 faces,
and walking the map (F6) gives it face collision too.

**Exfil test.** MkExfil `radio_live_at_start`: the generated script waits for `rbz_exfil_allowed` (set once the exfil
script has spawned the radio) and sets `rbz_exfil_beacon_active`, the flag zclassic sets for 120 s after rounds 10, 15,
20... An early exfil is valid: its zombie waves are `exfil_silver_` + clamp(n, 1, 4), so round 1 uses `exfil_silver_1`.

**The test map:** 'Leftovers test' (`zm_leftovertest`, deployed 22:14), from scratchpad `lo/make_leftover_test.py`. It
is the user's zm_latest (17:46 export) with these changes:
- the start zone lowered to the floor (it sat 167 units up);
- a south wall along the start area with two openings: the wood barrier (x 47..111) and a concrete one (x -200..-136),
  each with a player clip in it, a yard outside, and a barrier spawner in the yard for each;
- the power switch in the start area against the south wall; the door to room_2 needs power, and the Armor Station
  needs power;
- the exfil landing in room_2, its radio live from the start;
- four glow panels (a loose glb, `lo/make_glow_glb.py`) at the west end, behind the spawns: green at strength 1, green
  at strength 4, orange stripes from a texture at strength 2, and plain white. Their strengths are 32, 128, 64 and
  none.

zm_latest's own MkMesh (the car at collision faces, 5636 hulls) is left out of the test copy.

**Result (2026-09-29):**
- **Power switch: passed.** The perks and the Armor Station waited for the power, and the switch turned it on.
- **Exfil: passed.** The radio worked from round 1.
- **Barriers: not working, parked.**
  - The zombies never broke the wood barrier, so rebuilding it could not be checked. Likely cause: the yard is off Die
    Maschine's navmesh, and a zombie reaches an exterior goal only within 128 units of the navmesh (P5).
    **Confirmed by P5 (2026-09-29):** with the map's own navmesh, the yard zombies attacked the wood barrier and
    climbed through it.
  - The concrete barrier said it needed power, and the power did not remove it. Not looked into yet. Next time,
    check what zm_blockers does with the concrete zbarrier class and with the parts copied from it.
- **Glow: not visible, parked.** Either there is no glow or it is too faint to notice; the panels themselves drew.
  Next time, check:
  - whether +292 really is the strength (try much larger values);
  - whether the emissive map reaches the draw (the texture set's image ids);
  - whether the level's exposure hides it.

### P4 sky and lighting, step 1: the map's own sky (PASSED 2026-09-29)

**How Die Maschine draws its sky (read offline 2026-09-29, no IDA):**
- The lighting asset's state 0 (day) is the one a mapkit map runs in: our level script never changes the state. It
  names the dome `skybox_zm_silver_override`, a resident mesh of 43 vertices (about 780 units across).
- The dome's one material is `792207D7B4B8B185`, on techset `C84D8285C9443CAB`. It has one image slot, colorMap0:
  `i_mtl_skybox_zm_silver`, an 8192 × 4096 BC6H_UF16 image, streamed, with one level and usage 0x13.
- That image is an ordinary equirectangular panorama:
  - the top row points straight up and the middle row is the horizon;
  - the lower half is black;
  - the sky is overcast, over snowy mountains round the horizon;
  - its brightest spot is 27° up, and the state's sun is 25° up.
- The dome's own UVs do not map the panorama (they are a flat layout), so the shader works the direction out from the
  view. Offline data cannot say which way round the panorama wraps (which column faces which yaw, and whether it runs
  clockwise). The test shows that.
- Constants +160 and +416 hold the brightness:
  - the day sky: 2^6.5 = 90.5;
  - the Dark Aether sky (`02D4D71BD150B8C6`, same techset): 2^0.5;
  - `skybox_default_black` (`5548D2EDCF7C7942`): 2^-10.

  The day panorama's upper half averages about 0.7 in linear values, and its sun reaches 8.5. States 1 and 2 use the
  dark domes, and state 3 `skybox_default_black`.
- New in ffinfo: `--material --dds` also fetches a streamed image's pixels from the packages (`_streamed.dds`, game
  data, kept local).

**What mapkit writes:**
- **The editor** (MkExport) takes the sky of a WorldEnvironment in the map:
  - a PanoramaSkyMaterial's panorama as it is;
  - any other sky material baked with `RenderingServer.sky_bake_panorama` (only in the editor, not headless).

  The panorama is resized to 2:1 and a power of two wide (at most 4096), with one level. It is turned to the game's
  layout (`_SKY_LEFT_EDGE_YAW`, `_SKY_CLOCKWISE`; Godot's own layout until the test says otherwise), then compressed:
  BC6H for HDR, else BC7 sRGB. `sky.energy` = the background energy × the panorama's energy multiplier.
- **cwlink** (`AddSky`) writes:
  - the panorama as a resident image, `mapkit_<map>_sky` (usage 0x13, one level: a wrapped direction lookup draws a
    seam at the wrap column when there are mip levels);
  - a copy of Die Maschine's day sky material under that material's own name and id, with the image replaced and the
    brightness set to 90.5 × energy. The override zone's copy wins the name, so the dome draws the creator's sky.
- **Found on the way:** an MkMesh made of Godot's primitive meshes (QuadMesh, BoxMesh and the like) failed to export,
  because `surface_get_primitive_type` exists only on ArrayMesh. Fixed with `MkExport.is_triangles`.

**Not yet:**
- the sun (direction, colour), fog and exposure;
- lighting the creator's meshes (Die Maschine's probes light them);
- the other lighting states;
- the sky's rotation;
- the sky image the lighting state names for itself (`i_mtl_skybox_zm_silver` stays; perhaps used for reflections).

**The test map:** 'Sky test' (`zm_skytest`, deployed 06:07), from scratchpad `sky/make_sky_test.py`. It is zm_latest
(start zone lowered) plus:
- the labelled sky (`sky/make_sky_test_images.py`), exported through MkExport (`sky/export_sky_test.gd`). Numbers 1 to
  8 go round the horizon at 12, 35 and 60 degrees up, each with an arrow pointing to the next. Number k is centred at
  u = (k - 1) / 8, so 1 straddles the wrap;
- a compass pillar at (40, 0, 0). Each face shows the direction you look when facing it: +X red, +Y green, -X blue,
  -Y yellow.

If the game wraps the panorama as Godot does, looking toward +X shows 1, -Y 3, -X 5 and +Y 7, and the text reads
normally.

**Result (2026-09-29): passed.**
- The dome drew the creator's sky, the numbers and arrows read normally (not mirrored), and the brightness looked
  right beside the rest of the level.
- Looking toward +X showed 4 (u = 3/8) and toward +Y 2 (u = 1/8). So the game's panorama runs clockwise seen from above,
  as Godot's does, and its left edge (u = 0) faces yaw 135°: u = (135° − yaw) / 360°.
- MkExport now turns the panorama by that (`_SKY_LEFT_EDGE_YAW` 135). In practice that is a shift by 3/8 of the width,
  checked headless.
- Die Maschine's sun, in these terms: its panorama's brightest spot (u 0.493, 27° up) faces yaw 317°. The lighting
  state's sun is yaw 120, pitch 25. If those angles are the direction the light travels, the sun sits at yaw 300, 25°
  up, 17° from the panorama's sun. That is for the sun step.

### P4 sky and lighting, step 2: the sun and the fog (PASSED 2026-09-29)

**Result (2026-09-29, the 07:13 rebuild):** "all the things 1-4 are working": the map loads, the sky is turned right (+X
face under 1, +Y under 7), the warm low sun lights the −X face with shadows pointing west, and the blue-white fog
thins with height.

**Script fog does nothing here (IDB 2026-09-29).** `setvolfog` (8 or 18 arguments) and `setexpfog` only send
configstring 10. The frame's fog (`R_SetupFrameFog_cand`) is read from the lighting state instead, blended with the
gfx_map's override volumes. So fog and sun both live in the lighting asset.

**The lighting state (2084 B, 4 per volume), from `LightingState_Copy` and Die Maschine's four states:**

| Offset | Field | Die Maschine's day state |
|---|---|---|
| +0 / +4 | yaw and pitch of the direction the sun's light travels; `LightingState_GetSunDir` puts the sun at −AngleVectors(pitch + [+1320], yaw + [+1324]) | yaw 120, pitch 25, so the sun sits at yaw 300, 25° up |
| +16..+24 | the sun's colour | 0.79 0.89 0.98 (Dark Aether: 0.07 0.17 1) |
| +28 | the sun's intensity | 340 (Dark Aether 1, lightning 24) |
| +40 | the sky's name as text | `skybox_zm_silver_override` |
| +124 / +128 | the exposure range in EV | 9.1..14 |
| +1336.. | a physical atmosphere | planet radius 6.36e6 m, Rayleigh / Mie heights 8000 / 1200 m, coefficients |
| +1768.. | 17 floats blended with override volumes of category 8 | volumetric scattering |
| +1936.. | the world fog (category 7) | start 250, base height −1700, halfway 5000, halfway height 770, 0, 800, opacity 0.9, colour 208 226 251 (in 0..255 display units; Dark Aether 7 17 100), on 1 |

**Override volumes (gfx_map +7608/+7616, 148 B each; Die Maschine has 299):**
- Each record holds a 40-byte runtime header, the 10 fog floats at +40 and the 17 category-8 floats at +80.
- Their shapes live in a per-category table at gfx_map +1856 (`R_OverrideVolumes_WeighCategory_cand`): spheres and
  boxes with a smoothstep falloff.
- Our gfx_map still carries Die Maschine's volumes. A map at its start area may be inside some of them; untested.

**What mapkit writes:**
- **zonekit:**
  - `LoadLightingAsset` is the C++ port of the parked walker. It replays zm_silver's (7.06 MB) and core_frontend's
    (1.5 MB) exactly.
  - `CopyLighting` copies the asset's bytes plus the 2317 links to other assets and the 47 pointers into its own data.
    Die Maschine's states share pieces: all 47 lead into the asset itself.
  - `CopiedAsset` now also re-points links (through the zone's entry for each asset) and places the pointers again
    (from where its read-back loaded each target byte).
- **cwlink** (`AddLighting`):
  - It writes the sun and the fog into state 0 of every volume.
  - It adds 2306 by-name references; klf and winddef became reference-root types.
  - The copy keeps Die Maschine's name, so it takes that asset's place.
  - The zones grow from 1.7 to 4.9 MB each, and walk complete.
- **The editor:**
  - The sun is the map's first DirectionalLight3D: the direction its light travels, its colour (linear) and its
    energy (1 = Die Maschine's 340).
  - The fog comes from the WorldEnvironment:
    - halfway = ln 2 / density;
    - halfway height = ln 2 / height density above the fog height;
    - colour = the fog light colour × its energy, stored × 255;
    - opacity 1;
    - fog off = none.

**The test map:** 'Lighting test' (`zm_lightingtest`, deployed 06:46, rebuilt 07:13), from scratchpad `sky/make_lighting_test.py` and
`sky/export_lighting_test.gd`. It has:
- the labelled sky, now turned to the game's layout;
- a warm sun (colour 1 0.52 0.26) 20° up toward +X, its light travelling toward −X;
- a light blue-white fog, half covered at 1500 units and thinning by half every 300 up from the floor;
- the compass pillar at (40, 0, 0).

**First try: a crash while loading (06:50).** The game faulted in `DB_ConvertOffsetToAlias`, called from the lighting
asset's light list (`Load_LightingLightArray_cand`), reading a non-canonical address.
- **Cause: the klf reference.** A klf keeps an XString at +0 and its name hash at +16. The engine's per-type name getters
  (table 0x7FF72A386B18) read +0 for every other type mapkit references; sanim reads +8.
- mapkit read the klf's "name" at +0 (Die Maschine's -1, "string follows inline") and wrote it back at +0. The engine
  took that -1 as an inline name string and read the next asset's bytes as it.
- That put the engine's stream a few bytes off ours from asset 2325 on. Every later root was garbage. The lighting
  asset read a light count that wrapped `DB_ReadXFile`'s int size negative, so nothing was read, and its light loop ran
  over unwritten memory: block 6 + 0xF0, still holding a sanim's floats.
- **How it was found:**
  - The blocks are laid out back to back, and the crash address sat at block 6 + 0xF0.
  - The value there was one of our sanim's floats.
  - The loop reading it meant the light list was never written, so the lighting root the engine read was not ours.
  - Byte-dumping every new reference type showed the klf root.
- **Fix:**
  - `XAssetNameOffset` (zonekit): klf +16, sanim +8, else +0.
  - `TracedAssetName` and `EncodeAssetReference` (which now takes the type) use it.
  - The walker's reference check wants the name there and every other byte zero. The crashing zone now fails its walk
    at asset 2325.
- **Instrumentation added:**
  - `ffinfo --assets --walk` prints each asset's stream range and block 4/6 positions.
  - A crash during a zone the trace covers now writes the partial `.mktrace` and logs the faulting asset, its root words
    and the block bases.
  - cw-mod.json traces `ww_4k_`/`ww_1080_zm_lightingtest`.

**Not yet:**
- exposure;
- the lighting state preset (Dark Aether);
- placed lights;
- the light meshes get from their surroundings (Die Maschine's probes and GI);
- Die Maschine's override volumes (kept).

### P5 step 1: the map's own navmesh (PASSED 2026-09-29; the rest parked)

Until now zombies walked Die Maschine's navmesh, so they only moved where Die Maschine has ground (the Leftovers yard
was off it: its wood barrier was never attacked). mapkit now writes the map's own. Research: the parked P5 notes
(mapkit-research-parked.md) plus the IDB reads below (renamed, commented, saved).

**How the engine loads a navmesh:**
- `AI_LoadNavmeshForMap` (0x7FF723B9D5E0) finds the navmesh asset (0x75) by the hash of `maps/zm/<map>.d3dbsp`
  (none = a drop). An override zone's asset of that name takes Die Maschine's place, like the gfx_map.
- The asset's root points at stream keys: the shared tagfile (+8), one per cell (+40 of each 64-B cell) and debug
  data (+72, never loaded by this build). A key whose flags (+55) have bit 2 holds its data inline in the zone
  (block 4); the loader then reads it in place (`StreamKey_IsReady_cand`). Die Maschine's inline keys use flags 3.
- The key's data is a 0x120-byte header, then a Havok 2019.2 tagfile (`Nav_TagfileBlobFromStreamBuffer_cand`). The
  header's u32 at +0x10C (the "checksum") is never read.
- Per cell, `AI_AddNavMeshCellsToWorld_cand` makes a Havok navmesh instance and a cluster-graph instance. A navmesh
  without a face search tree gets one built at load (`hkaiNavMesh_GetOrBuildFaceIterator_cand`).
- Face flags (faceData & 0x1FFFFF, `Nav_FaceMaterialFlags_cand`) are material flags: walkable 1, water 2, staircase
  8, disconnected 0x20, region_separator 0x80, nobot 0x400, ... then numbered bits. Die Maschine's faces carry
  walkable plus the numbered 18..21. An AI may use a face that shares a bit with its mask, unless it is nobot or
  disconnected (`Nav_FaceFilterAllows_cand`, `Nav_FaceExcluded_cand`).
- The tactical graph (cover points for human AI) is indexed per face with no bounds check (`Tac_ClosestPointNear_cand`),
  so a cell needs one entry per face. Its other readers check their bounds.
- Die Maschine's path nodes still load. Each binds to the navmesh by a 72-unit ray down (`Path_BindNodesToNavmesh_cand`);
  one that hits nothing just stays unbound.
- The exe also carries Havok's navmesh generator: moving entities get a navmesh at runtime from the shared file's
  generation settings (`Nav_BuildMovingPlatformNavMesh_cand`). So the shared file keeps them.

**The tagfile format (zonekit `havok_tagfile`):** sections SDKV, DATA, TYPE, INDX {ITEM, PTCH}. Items are numbered
breadth first from the root; arrays are laid out as soon as they are numbered, objects when their turn comes; an
array of records has its references numbered one field at a time. Objects are aligned as their type, arrays to 16.
mapkit keeps a file's TYPE section as it is. **All five of Die Maschine's navmesh and navvolume tagfiles read and
write back byte for byte** (`ffinfo --hk <file>`).

**What mapkit writes:**
- **zonekit `GenerateNavPolygons`:** Recast (vendored in `vendor/recastnavigation`, zlib license) over the same
  convex hulls the clip map gets (solid brushes, mesh collision, prop hulls). It uses Die Maschine's generation
  settings: a character 72 tall, steps of 18, slopes up to 46°, cells 4 across and 2 up, and no shrinking from walls
  (Havok keeps a character's radius off the edges itself). Polygons no seed reaches are dropped: wall tops, the tops of
  props. The seeds are where players and zombies start and the objects players use.
- **zonekit `BuildNavMeshCell`:** Die Maschine's cell with its ground replaced:
  - faces counter-clockwise from above;
  - edges paired with their opposites;
  - vertices in meters;
  - faceData 0x1E0001, so every AI size may use every face;
  - a cluster graph of about 10 faces per cluster;
  - no tactical points (one {-1, -1} tfaces entry per face);
  - no face tree.
  `CheckNavMeshCell` re-reads the written cell and checks it the way the engine walks it.
- **zonekit `BuildNavMeshShared`:** Die Maschine's shared file with the path nodes' and regions' tactical point keys
  emptied, since they index tactical points our cell does not have.
- **zonekit `EncodeNavMesh`:** the asset with one cell and both keys inline. The loader `LoadNavMesh` makes written
  zones walk complete.
- **cwlink `AddNavMesh`:** fetches Die Maschine's two tagfiles from its packages (a key's +8 is its package key),
  builds both files and writes `generated\navmesh.obj`. `--no-navmesh` keeps Die Maschine's.
- **client:** the live dump's first lines now say whose navmesh the AI uses and whether its files loaded
  ("navmesh: ... (inline: mapkit's), shared file loaded, 1 cell(s); cell 0: N faces ...").

**The test map:** 'Navmesh test' (`zm_navtest`, deployed 13:57), from scratchpad `nav/make_navmesh_test.py`. It is the
Leftovers test plus a deck 64 up in room_2 (x 400..560, y 150..236), with seven 8-unit steps down its south side and
an 18° ramp down its east side. The navmesh has 30 faces over the start zone, the doorway, room_2, the deck, the stairs,
the ramp and the yard (separate: the barriers close it off).

**Result (2026-09-29): PASSED.**
- The yard zombies attacked the wood barrier and climbed through it (the Leftovers test had never seen that).
- Zombies chased the player without getting stuck.
- They followed the player up the stairs and the ramp onto the deck.
- client.log (16:40) confirmed whose navmesh ran: `live navmesh: 6BF83815978FDE24 (inline: mapkit's), shared file
  loaded, 1 cell(s); cell 0: 30 faces, 121 edges, 59 vertices`.

On 2026-09-29 the same test map was the first map of its own ("A map of its own, stage 1" below), and played on two PCs.

**Not yet (parked 2026-09-29):**
- doors and props that move (the navmesh runs through doorways; closed doors rely on the engine);
- traversals (mantles, jumps): no user edges;
- tactical points;
- more than one cell (large maps);
- navmesh settings in the editor.

### The intro cinematic (found 2026-09-27)

Our level script drops DM's intro (the helicopter arrival), so a mapkit map starts without one. This is how it
works, so the editor and `cwlink` can offer one later.

**zm_common plays it, not DM.** `zclassic::intro_cinematic` (`scripts/zm_common/gametypes/zclassic.gsc`) waits for
the flag `initial_blackscreen_passed`. If `level.var_dfee7fc2` names a scene and dvar `hash_39af51993585a73e` (a
skip switch) is 0, it plays that scene with `scene::play`, then moves every player to `player.spectator_respawn`
(their initial spawn point). Each player's spawn streams the scene in first (`scene::init_streamer`).

**DM's part is two lines in `main`:**
- `level.var_dfee7fc2 = #"cin_zm_silver_intro";`
- `scene::function_497689f6( #"cin_zm_silver_intro", "helicopter", "tag_probe_attach", "prb_tn_zm_silver_heli_light_cabin" );`
  attaches a light probe to the helicopter.

`zm_silver_vo::match_start_vo` adds the "infil" voice line when the intro runs, and waits for `intro_scene_done`.

**The scene** is scriptbundle `cin_zm_silver_intro` (decompiled: `bocw-source/scriptbundle/scene/cin_zm_silver_intro.json`):
- skipping disabled, streamer hint `cin_zm_silver_intro`;
- `aligntarget` = `tag_align_intro_start`: every animation plays relative to it;
- 7 objects: `player1`..`player5` (a player anim per shot, `sh010` to `sh040`), `helicopter` (a vehicle, deleted
  when finished) and `helicopter_pilot` (a prop).

**The align target is an entity** in DM's entity list:
- id 5802, a `script_struct` with targetname `tag_align_intro_start`;
- `client_server` = `BothSides`;
- origin (1968, -1812, 471), angles 0 0 0.

**How a mapkit map gets an intro:**
1. **DM's helicopter intro, somewhere else.** The scene and its animations load with DM's asset library, so the
   map needs only:
   - the two lines above in its level script;
   - a `script_struct` `tag_align_intro_start` (BothSides) in its entity list, where the arrival should play.

   Editor: an `MkIntro` node (the align transform; none / DM helicopter). `cwlink`: writes the struct and the
   two lines.

   Caveat: the flight path and camera are DM's, relative to the align point, and pass through whatever is
   around it.

   While we still load DM's entity list (P1 step 1), its `tag_align_intro_start` is already there. Adding the
   two lines back to our `zm_silver.gsc` should bring DM's intro back as it is. Not tested.
2. **A new intro.** This needs a new scene scriptbundle plus player and vehicle xanims. That means a scriptbundle
   writer and xanim authoring. Not started; xanim is one of the M2 loader blockers.

## What deleting the entity list does

`G_SpawnMapEntities_cand` 0x7FF723F2BE30 spawns the level's entity list:
- It passes entity 0 to `SP_Worldspawn_cand` **without checking its classname**.
- It then loops `while (i != count - 1)` over the rest.

So with an **empty list**, worldspawn reads a null entity and the count underflows to 2^64 iterations. That is
a crash at level start.

The minimum list is a single worldspawn at index 0. Everything else is looked up by scripts, by targetname or
classname. T9's `script_struct`s live in the entity list too.

With our own level script, a from-scratch list holds:
- worldspawn;
- what the `.mkmap` places;
- what zm_common needs: `initial_spawn_points`, zone `info_volume`s (trigger list), `<zone>_spawns` structs,
  the `actor_spawner_zm_*` entities, and the perk, box and wall-buy structs;
- the `minimap_corner`s.

DM's other ~2,700 entities go.

## How we reverse from now on

1. **Every field written has an owner.** Before mapkit writes a struct, the struct gets a field table:
   - whether the loader reads the field;
   - which runtime code reads it (function and purpose);
   - what the field means;
   - the value mapkit writes.

   A nonzero field nobody can explain means the struct is not ready to ship. The table lives in the zonekit
   header, next to the encoder. Almost every M4 crash was a field the loader never sized, read by runtime code:
   - clip map @460 (dynent count);
   - terrain @38 (quad masks);
   - clip map +24 (read through districts);
   - the 500 dynent debris slots;
   - the sanim script-string indices;
   - gfx_map +1432/+1444 (decal ranges);
   - the terraingfx textures (SRV slot 105).

   Each would have been one line in such a table. The table is built by declaring the struct in IDA, applying
   it at the loader and at every consumer, and finding the consumers systematically: scan the dump for
   `[reg+disp]` accesses in the functions that fetch the asset from its pool or global, instead of waiting for
   a crash to name them.
2. **Use the smallest valid retail world as the reference, not DM.** `core_frontend`'s world is complete and
   tiny: 16 model groups, 209 models, 1 cell (client.log, 2026-09-26 00:39). Compare its gfx_map and clip map
   with zm_silver's, field by field (`ffinfo --gfx` / `--clipmap` on both traces):
   - a field with the same shape in both is **structural**, and its value goes into the template;
   - a field that differs is **content**, and comes from the `.mkmap`.
3. **Live-state dump (the one small tool).** After the world starts, a client action writes the live bytes
   (linked, after init) of a named asset and its sub-arrays, plus chosen engine globals. `ffinfo` then diffs
   file against live, and ours against retail. That shows every field the engine fills at runtime within one
   boot, before it turns into a crash. First use: our wall model against the retail plinth.
4. **Each test boot carries a matrix.** One boot tests several variants side by side, the way devtools'
   `mapkit_check` does. Example: our mesh with our material, our mesh with the plinth's material, and the
   plinth's mesh under our xmodel header.
5. **Script strings are evidence only when canonical.** An interned literal filed under the complement of its
   hash (the Arxan caller guard) never compares equal to the engine's own string. `getentarray` and `===` then
   silently find nothing. The loader now logs whether each literal is canonical. Until it says so, a devtools
   check that "found nothing" proves nothing.

## What the zone files hold that mapkit does not handle yet

The full inventory of a map (every asset type in Die Maschine's zones, which file holds it, and mapkit's status
on each) is in [mapkit-map-anatomy.md](mapkit-map-anatomy.md). Research started for P2, P4, P5 and the level
effects, and parked unfinished on 2026-09-27, is in [mapkit-research-parked.md](mapkit-research-parked.md): the
static model placements (a districts stream key), the navmesh (Havok 2019.2 tagfiles), and the lighting asset
(decoded, with script switches).

### Placement: how retail places static models (the key to "Godot places model X at T")

Retail static geometry is **never** a `script_model`. It is the gfx_map's draw +536 **model groups**: 992-B
structs (`Load_ClipMapStruct992`), each holding:
- an xmodel list at +64 (count u8 @986);
- per-instance 64-B records at tail +8 (`(u16 @976 + i16 @984) x 64 B`);
- a 912-B block at +24;
- a head at +32 with 264-B entries;
- 32-B entries at +80 (count u8 @989);
- a u16 list at +936.

136 of DM's 158 groups are **shared with the clip map's +24**, and that is where their collision comes from
(xcollisions, streamed through the districts' slot 9).

The loader is transcribed; the meaning of the fields is not. This is the format that mapkit's props (retail
models by name) and its own meshes should compile to. Still to find:
- the instance transform layout;
- which instance uses which model;
- what the 912-B block does (a culling BVH?);
- per-instance lighting;
- the LOD and cull distances.

### XModel / xmodelmesh

- **Resident loading is complete.** `DB_XModelMesh_LinkCallback` only marks the `.xpak` set dirty, and only for
  STREAMED meshes. Resident meshes get no registration at link time, and block 12 is ordinary memory
  (`g_xblockAllocKind` 7). So the invisible walls were not a load problem.
- **P0 result: the invisible walls (solved 2026-09-26, passed in-game).** Two causes, both outside the mesh:
  1. **No bgcache listed the model.** A map entity's `model` key goes through `G_SetModel_cand` →
     `BG_Cache_FindIndex_cand(2 "model", hash)`. Those tables are filled only at level load, by
     `BG_Cache_RegisterAll_cand`, from every loaded bgcache asset (0x6D). An unlisted model leaves the entity
     without a model: it spawns and never draws, with no error. Every earlier "success" was a same-name override or
     a model DM's own bgcache lists. `cwlink build` now writes `mapkit_<source>_bgcache` listing every xmodel it
     writes (`EncodeBgCache`, world_writer.hpp).
  2. **The borrowed skeleton's bone info.** xskeleton +64 holds per-bone bounds (mins, maxs, center, radius²), and
     each of DM's one-bone skeletons stores its own mesh's. mapkit's models borrowed the PaP plinth's (a flat
     21 × 22 box), so they vanished whenever that box at their origin left the view. Each model now gets its own
     skeleton (`EncodeStaticSkeleton`, model_writer.hpp: one bone, `tag_origin`, the model's bounds).
  - Ruled out by the draw test row (`--draw-test`): xmodel +208 flags, +221, xmodelmesh +62 and the LOD table.
    mapkit's copies of a retail door draw with any of them.
- Mesh info +256/+296/+336 are three 40-B records, each with 12 bytes of 0xFF in the file. They are probably GPU
  views filled on first use. The live dump tells whether ours get filled.
- The 96-B block `{0x80000000, 0...}` at xmodel +104 and surface +32: meaning unknown.
- A single LOD (+112 count, +116 distances): what does the draw path do with it?
- Per-model collision (xmodel +16 xcollision): mapkit links an empty one. Placed props with mesh collision need
  an xcollision writer, or brush hulls.

### gfx_map drawing ("surfaces")

- Draw buffers A, B and C (+72/+136/+200, four each, block 2, sized by u32 @64/@128/@192) are world vertex and
  index data built at **runtime**. What fills them, and from what?
- Draw materials: 96-B entries, the material at +72, count = root i32 @16.
- Decals (+1408), terrain (+688 plus terraingfx) and group LODs (+696/+712) are cut or kept today, never
  authored.
- The terraingfx +224 block (world bounds plus map-wide height textures) must stay valid even with no terrain.

### Material / image

- Materials are only referenced (retail, by name). `DB_Material_LinkCallback` only sets a "material set
  changed" flag. `Load_Material` is transcribed; there is no writer.
- Custom textures need three pieces:
  - an image writer (pixels resident in block 6);
  - a material writer (a retail material's techset and constant layout, with a new image table);
  - staying inside the shader permutations `techset_zm_silver` ships.

### Not started

- **Lighting** (`Load_Lighting` is not transcribed): sky, sun, color grade and model lighting are baked there.
- **Navmesh.** Zombies path on DM's navmesh. Our geometry inside DM's start zone only works by accident; any
  other layout needs navmesh authoring (Havok AI data). **This is the biggest unknown in the plan.**
- Minimap, sound and reverb volumes, streamed textures.

## Future: textures in the editor (planned 2026-09-27, not started)

The editor draws the game's models and their picker pictures in plain gray (`mkasset`, `MkAssets`, the model
picker). Textures are step 5 of that work, left for later on purpose. What it takes:
1. **Material → images.** `mkasset` already has each surface's material hash. Read the material (`Load_Material`
   is transcribed) for its image table, and pick the color map (the albedo slot); normal and roughness maps can
   come later.
2. **Image pixels.** Images are mostly streamed from the `.xpak` packages through KAPI, as BCn blocks (BC1/BC3/
   BC7). Read the smallest mip that is still sharp enough for the editor (256 px is plenty), and write it into
   the `.glb` as a PNG, or as a DDS the editor loads.
3. **Cache like the models.** Images go into the same local cache (`user://game_models/`), shared between
   models; the pictures are redrawn once textured.

The same image reader is the first half of P4 (own materials), so the work is not lost.

## A map of its own, stage 1: its own name (2026-09-29; PASSED on one PC 21:52 and on two PCs 22:18)

**Why.** A 2-PC test on 2026-09-29 kicked the joining client with "Server Disconnected - Clientfield Mismatch". A
mapkit map was an overlay laid over Die Maschine and live only on the PC that picked it in CUSTOM MAPS. The lobby
told the client "zm_silver", so the client loaded stock DM and ran DM's `zm_silver.csc` against the host's mapkit
GSC (client log: `gfx_map 'zm_silver' (the map's own): 158 model groups`, then the kick one second later). The user
also wanted the zm_silver scaffolding gone. Stage 1 gives the map its own name; stage 2 (P6) removes DM's zones.

**What the engine does with a map name** (IDB 2026-09-29, renamed and commented):
- The lobby map is a string. `Session_SetMapName` (0x7FF727B272A0) stores it in the session (+9176, hash at +9216).
  The lobby-state packers (`LobbyMsgRW_PackageLobbyStateA/B`) send it to every member as the field `"map"`
  (36 chars). The host's playlist apply (0x7FF726F4C700) sets it from the playlist entry; a member's
  `Lobby_ApplySessionSettings_cand` sets it from the host's state.
- The level script is found by the started map's name: 0x7FF71E76DFD0 builds `scripts/<p>/<map>.gsc` (`.csc` on the
  client). Every `script_using` asset of every loaded zone is linked as well.
- World assets are looked up by the started map's `.d3dbsp` name, through `DB_FindXAssetHeader`: clip_map
  (`CM_LoadMap`), com_map, navmesh, navvolume, the entity lists, and the client game's world
  (`World_LoadByMapName_cand` 0x7FF72904A850: gfx_map and cpu_occlusion_data).
- But the engine keeps **one** world loaded, and two of its parts do not follow that name:
  - `CM_LoadMap` drops the clip map it finds and uses `g_clipMap`, the clip_map pool's first slot.
  - The renderer loads the gfx_map named after a **zone**: `DB_PostLoadFrame_ApplyOverrides` (0x7FF727EC4A30) calls
    `R_BeginLoadWorld` (0x7FF727F64DB0) for the first loaded zone with flag 0x140 and none of 0x1278000 (no variant,
    no override flags). For a map of its own that is zm_silver, since the map's zone carries the override flags.
    Its districts then come by the gfx_map's name.
- `getmapfields` (used by content_manager, the box and the wall buys) returns undefined for an unknown map, and
  content_manager reads a field of the result without checking.

**The first run crashed (19:04).** The first build named the map's world after `<id>` (`maps/zm/<id>.d3dbsp`), so it
sat next to DM's world instead of replacing it. The client game took the map's gfx_map by the map name and sized the
streamer's per-cell bitmaps from its empty streamerworld (0 cells, `R_StreamerCellBits_Alloc_cand`), while the
renderer drew DM's gfx_map by the zone name and walked DM's 12 cells into that null array
(`R_StreamerCellBits_Set_cand`, a null read). The clip map would have been DM's too (the first pool slot).

**What mapkit does now:**
- `cwlink build` writes one zone, `<id>.ff`, holding only what the build makes. Its world assets keep zm_silver's
  names and replace DM's world in the override swap (priority 27), exactly as an overlay's do: renderer, client
  game, collision and streamer all see the same world. The level scripts are compiled as `scripts/zm/<id>.gsc/.csc`,
  with empty stand-ins under `zm_silver.gsc/.csc` because DM's zone still links those. map.json gains
  `"format": "standalone"`. `--overlay` writes the old form.
- The client (mapkit_loader.hpp 7.):
  - The CUSTOM MAPS pick makes the host's lobby map `<id>`: the `Session_SetMapName` detour, plus a direct set when
    the playlist does not change.
  - Wherever `<id>` loads (lobby preload or level load, host or client), `DB_ExpandZoneVariants` puts zm_silver's
    zones before it, drops `<id>`'s variants and gives it the override zones' flags (priority 27: its world and the
    sky material override win).
  - The loaded map, not the pick, decides which level scripts are served.
  - A world lookup by `<id>`'s `.d3dbsp` name looks up zm_silver's instead, which holds the map's world after the
    swap.
  - The maptable knows `<id>` by zm_silver's entry.
- A world named after the map itself comes with stage 2, once DM's zone (and its world) no longer loads.

**Test:** 'Navmesh test' was rebuilt this way (`cw-mod/maps/zm_navtest/zm_navtest.ff`; the zone's world names match
the proven overlay build's). One PC first, then two. In client.log look for:
- `lobby map 'zm_navtest' <- 'zm_silver'`;
- `loading 'zm_navtest', a map of its own`;
- `zone list for a map of its own: [...zm_silver 0x..., zm_navtest 0x1040200]`;
- `level scripts: maps/zm_navtest/scripts served`, then `Serving 'maps/zm_navtest/scripts/zm_navtest.gscc'`;
- `world start: gfx_map 'zm_silver' (mapkit's own): 0 model groups` (not DM's 158);
- `world asset 0x.. of 'zm_navtest' looked up under zm_silver's name`, one line per type.

On the second PC the same lines should appear, with the lobby map coming `from the host's settings`, and no kick.
The other test maps are still overlays until they are rebuilt (the Godot Build button now writes the new form).

**Results (2026-09-29, 21:23 DLL):**
- One PC PASSED (21:52). All the lines above appeared: 0 model groups, the map's own navmesh, and world lookups
  aliased for 0x18, 0x19, 0x75, 0x76, 0x1B and 0xA9.
- Second PC, run 1 (21:53): its `cw-mod/maps/zm_navtest` was still the old overlay build (ww_1080_/ww_4k_ zones, no
  `zm_navtest.ff`). The host's lobby map `zm_navtest` arrived, and at the launch `Com_LoadLevelFastFiles("zm_navtest")`
  dropped with 0x48342502, "Zed 453 Kinetic Devil" (the map's .ff header does not read). The maptable check before it
  passes for an unlisted name.
- Second PC, run 2 (22:05): `zm_navtest.ff` had been added next to the old overlay zones. The loader kept the variants
  a map's folder ships, so the level load listed `[..., zm_silver, ww_4k_zm_navtest, zm_navtest]`: two mapkit worlds
  under zm_silver's names, beside DM's. Linking a third copy of a world asset overflowed its pool:
  `DB_AllocXAssetEntry` 0xE554F200, "Spring 817 Wild Star". This happened on the DB thread, and that thread turns any
  drop into sys_error 0xE4BD8598 (`DB_Thread_RunLoadQueue_cand`), so the game quit with a Windows error.
- Fixes (22:11 DLL):
  - A map of its own never loads a variant of `<id>`: cwlink writes none, so one in its folder is left over from an
    older build. The loader logs such zones at boot ("delete them").
  - A member checks the host's lobby map when it arrives and logs why it cannot load it: not installed, or an older
    build with no `<id>.ff`.
- Two PCs PASSED (22:18), with the second PC's folder cleaned (`zm_navtest.ff`, `map.json`, `scripts/`). Host and
  client were both in Navmesh test for about 3 minutes, with no kick and no mismatch. The client's log matched the
  host's: the same zone list, `gfx_map 'zm_silver' (mapkit's own): 0 model groups`, `zm_navtest.cscc` served, world
  lookups aliased and the maptable alias. The navmesh loads only on the host, where the AI runs; the client logs
  `live navmesh: no nav world`.
- Log noise, not failures:
  - The `(Crash) Unhandled-looking exception` blocks next to each `live dump` line are the P0 live dump's own caught
    reads (memcpy from our DLL): 647 in the host log since before stage 1.
  - The one at quit (`telescope.dll`, then `BlackOpsColdWar.exe+0xD59FED6` reading 0x18) ends almost every session
    on both PCs.
- Clientfields: every match logs 5 fields whose registration version differs between the server and client VMs
  (bits and type equal). Stock DM does the same (2026-09-26 22:59, 842 fields), so this is not a mapkit mismatch.
- Not done: the lobby's error dialog still shows the engine's code name. It shows the `errorMsg` that
  `Com_NotifyLuiError` (0x7FF727A382A0) passes to Lua before `BB_Alert` runs, so a readable text would need a detour
  there.

## P6 step 1: count what a map borrows (done 2026-09-30)

**Why.** P6 copies everything a map uses from Die Maschine's zones into the map's own zone, so that DM stops loading.
Before writing any copier, the count says how much that is and which asset types it involves.

**The links inside zm_silver.ff, offline** (`zonekit/ref_graph.*`). The zone trace gives every asset's bytes and every
block's position at each asset's start. A stored pointer is `((block << 60) | position) + 1`. So any 8 bytes in an
asset that decode to a position an earlier asset loaded point into that asset:
- into the XAsset array (block 4, the entry's +8): a link to another asset of the zone, or a by-name link to another
  zone's asset;
- to the start of what that asset stored in a block (its root, or an insert slot): a link;
- into the middle of what it stored: an inner pointer, not followed.

No loader per type is needed, so all 92 of the zone's types are covered. zm_silver has 146,712 links between its
182,002 assets (182,359 through the XAsset array and 89,496 to a start, before duplicates are removed), plus 227,918
inner pointers. The scan takes 0.2 s.

**Inner pointers are shared data, not links.** The first version followed them, and they inflated every count:
- They are systematic: 37% of DM's materials point into another material, 21% of its skeletons into another skeleton,
  and 7,941 pointers go into one streamkey. So they are data stored once and shared, the way a fastfile shares any data
  it already wrote. A copy needs those bytes, but not the asset around them.
- Followed as links, they pulled in whole assets. DM's 5.3 MB resident image had 115 "parents": 44 images, 23 xanims,
  12 skeletons. The static count fell from 12,278 assets / 56.9 MB to 8,277 / 35.1 MB once they were left out.
- `ffinfo --needs-graph <file>` writes the graph, with inner pointers marked `~`, for questions the report does not
  answer.

**What a match uses, in the game** (`client/game/mapkit_usage.*`, `"mapkit_usage": true` in `cw-mod.json`, off by
default).
- Scripts and map entities reach most of a level's assets through the bgcache tables, with
  `BG_Cache_FindIndex_cand(table, name)` (0x7FF726212E90). There are 40 tables, among them model, aitype, weapon, fx,
  xanim, scriptbundle and soundalias; the full list is in `dump_anchors.hpp`.
- The census records those lookups and every `DB_FindXAssetHeader` lookup, each with its caller.
- It writes `cw-mod/mapkit/usage/<map>_<time>.mkuse` 45 s after the world starts, then every 2 minutes while new names
  come in. The Maps tab has a "Write asset usage now" button.
- The file also holds every loaded asset with its zone. A duplicate from another zone is chained behind the first entry
  (`DB_InsertOverrideEntry` 0x7FF727EC1210, entry +8 bits 24-47) and has no used bit of its own, so the census follows
  the chains.
- The level load's own lookups are left out: `BG_Cache_Register_cand` looks up every name that every bgcache lists, so
  those say "listed", not "used".
- The census counts from the game's start, so a match's file holds the frontend's lookups too. The frontend's own file
  (`level_<time>.mkuse`) is written first; `--usage-base` subtracts it.

**The renderer's stream-ins say "streamed", not "used".** The stream-in routine (0x7FF727F504C0) resolves each stream
request `{name, kind @36}` by name: an image, an xmodelmesh (kind 0x100) or a streamkey (0x200).
- It runs from the stream-in queue (0x7FF729284C70), which copies the data read from the packages into the asset. It
  also runs from a residency check over the streamerworld's +64/+72 records (0x7FF727F50610).
- In the zm_navtest match it streamed 8,526 of DM's assets. 6,720 of them (3,774 meshes, 2,501 images, 445 streamkeys)
  are reached only from the world the map replaces (DM's streamerworld, districts, gfx_map). Neither they nor their
  1,354 parent models are named anywhere in zm_navtest.ff.
- So DM's world streams in the background while zm_silver.ff is loaded. The map's own streamerworld is the head, and it
  is empty.
- The count leaves those out, keeps the 1,760 that something the map uses links to, and counts the 46 that nothing
  explains.

**The count.**
```
ffinfo zm_silver --trace <zm_silver.mktrace> --needs --usage <match.mkuse> --usage-base <level.mkuse>
       --map-zone <game>\cw-mod\maps\<id>\<id>.ff [--needs-list <file>] [--needs-graph <file>]
```
- It starts from what the match used and what the map's zone links by name, and follows every link.
- It stops at anything a zone that stays holds: zm_common, core, or the map's own zone, whose world replaces DM's
  under the same names.
- It prints what would be copied from zm_silver.ff, by type and size, and what only DM's other zones (`techset_`,
  `en_`, `ww_`, `1080_`/`4k_`) hold.
- Per source (a bgcache table, a lookup type, the map zone's links) it prints "only it": the assets no other source
  reaches.
- Without a census, `--keep zm_common:<trace>` stands in for the zones that stay, and only the static half runs.
- The map zone's links carry the type their loader read them as (`XStream::SetRootType`): a world asset's name is
  shared by some 15 types.

**Names are not always at +0.** `DB_GetXAssetName` jumps to `[0x7FF72A386B18 + 40 * type]`, one
`mov rax, [rcx+N]` getter per type.
- The 40-byte records are `{type name, size, alignment, GetName, SetName}`. Read from the getter, the name and size
  next to it belong to the next type, which is how this was missed before.
- 24 types keep their hash elsewhere. xanim is the big one: +112, since +0 of its 288-byte root is zero. The full list
  is in `XAssetNameOffset` and on the IDB's `DB_GetXAssetName`.
- With it, DM holds 2,203 xanims of its own (28.2 MB). The other 5,071 are 288-byte links by name. The anatomy
  table's "7,243 own" read +0.

**Static numbers (zm_latest, zm_common kept).**
- zm_latest.ff links 17 of DM's names. With what they link to, that is 2,331 assets and 14.3 MB: the lighting
  (7.06 MB) and its 2,300 images (7.2 MB).
- The ceiling, if a map used every name DM's bgcache lists, is 8,277 assets and 35.1 MB of zm_silver.ff. 500 xanims
  make up 23.7 MB of it.

**The census match (zm_navtest, 2026-09-30 19:11 to 19:18, 7 minutes).** The upper bound is **7,377 assets and 44.8 MB
of zm_silver.ff's 181 MB**, with DM's zones still loaded. By what pulls it in (the "only it" column):

| Source | Assets | Stream | What it is |
|---|---|---|---|
| xanim lookups | 270 | 23.0 MB | DM's animations, see below |
| the map zone's links | 2,328 | 14.3 MB | the lighting and its images, the sky's material and images |
| static level FX list lookup | 335 | 1.7 MB | DM's `staticlevelfxlist` and its FX |
| xmodel lookups | 1,557 | 1.6 MB | models scripts and entities asked for |
| terraingfx lookup | 1,491 | 1.6 MB | DM's terrain gfx and its images |
| weapon lookups | 666 | 1.3 MB | 16 DM items, see below |
| scriptbundle lookups | 183 | 0.6 MB | DM's script bundles |
| the rest | 157 | 0.3 MB | 9 of DM's bgcache models, 3 AI types, 9 scripts, 46 unexplained stream-ins |
| more than one source | 390 | 0.5 MB | |

What the names say (hashes resolved with `acts -t lookup`, or checked with the T9 FNV-1a hash):
- **All 500 xanims are listed only by DM's bgcache.** The level load's animation setup requested them (the anim-tree
  slot loader 0x7FF724957D90 and the state-machine loader 0x7FF728EBFB50). That setup requested 13,751 xanims in all,
  about half of every bgcache's list. 41 of DM's are over 100 KB (18.5 MB): the "echo" cutscenes (Orlov, Medved,
  Vogel, the last soldier), Easter-egg step 4, the jellyfish fxanim, the tank zombie. The map plays none of them.
- **The 16 weapons are DM's own items:** the rocket shield (`riotshield`, `zhield_dw/lh/turret`, parts 1 to 3), the
  Aetherscope and its parts, `essence_trap_zm`, `zombie_lure_zm`, `equip_gold_container_zm`, `equip_sprout_zm`. Each
  was looked up only by the registration and its weapon callback (36 calls each). Only three bgcache tables have a
  callback (vehicle, aitype, weapon, at table +16), and it can look up more names.
- **The AI types are DM's spawner templates,** which cwlink keeps from DM's entity list: `spawner_zm_zombie_ndu`, its
  `_wall_pull` variant and `spawner_bo5_zombie_zm_silver_armor_heavy`. zm_common holds `spawner_zm_zombie`, with the
  same 1,069-xanim set (1.05 MB), plus the Plaguehound, the Megaton (`spawner_zm_steiner`), the Abomination and the
  Mimic.
- **The scripts:** the map ships `zm_silver.gsc`, `zm_silver_zones.gsc` and `zm_silver_ffotd.gsc` as replacements, so
  the engine still looks DM's up by name. DM's `zm_silver_fixup.gsc/.csc` still loads.
- **Sounds are not counted.** Sound aliases are names in a sound bank, not assets; the match used 56 of DM's.

**So the map's real need is the world part, about 17.5 MB:** the lighting and its images (14.3 MB), the terrain gfx
(1.6 MB) and the static level FX list (1.7 MB). The rest (animations, items, script bundles, AI types) loads because DM's
bgcache, spawner templates and scripts load, and should go with them. The census cannot split load from play yet (it
keeps only the direct caller, not the stack), so step 2 has to confirm it.

**Next: P6 step 2, a map that loads without zm_silver.ff.**
- The world part as the map's own:
  - the lighting copied with its images (both readable);
  - an empty terrain gfx that keeps its +224 block images (the 2026-09-26 00:31 crash);
  - an empty static level FX list.
- Spawner templates from zm_common, not DM's.
- The map's scripts injected instead of replacing DM's.
- Then a census match without DM's zones: whatever is still missing shows as "held by no loaded zone".
- Optional: a census flag for lookups made while `BG_Cache_Register` is running, to make the load-versus-play split
  exact.

Step 2 is split in two: first the world part, while DM still loads (2a, below), then dropping DM's zones (2b).

## P6 step 2a: the level's world assets as the map's own (PASSED in the game 2026-09-30, on the second test)

**Why first, and alone.** Dropping DM's zones changes two things at once: which zone the world comes from, and which
assets exist at all. 2a does the first while DM still loads under the map. Every world asset the level finds is now
the map's own copy, under DM's names, so it takes the place of DM's in the override swap as the gfx_map and clip map
already do. A crash then points at a copy, not at something missing.

**What the level finds, and how** (the census's lookups and the IDB, 2026-09-30):

| Asset | Found by | Before 2a | Now |
|---|---|---|---|
| gfx_map, clip_map, streamerworld, districts, navmesh, entity and trigger lists | the map's `.d3dbsp` name | the map's own | the same |
| lighting | the gfx_map's +7448 link | DM's (copied only for a sun or fog) | copied always |
| terraingfx | its name | DM's | DM's, without its tiles |
| staticlevelfxlist | its name | DM's: 2,482 placed effects at DM's places | empty |
| glasses | its name | DM's (already empty) | copied |
| com_map (the primary lights) | its name (`Com_FindComWorld_cand` 0x7FF7295F04A0) | DM's, through the stage-1 alias | copied |
| cpu_occlusion_data | its name | DM's, through the alias | copied |
| navvolume | its name | DM's, through the alias | copied |
| game_map (the AI path nodes) | the one asset of its pool (never looked up) | DM's | copied |

**How a copy is made** (`zonekit/asset_record.hpp`). The gfx_map's copy machinery is now shared:
- a *recording reader* walks an asset the way its `Load_<Type>` does and notes where every link, pointer, script
  string and sub-array sits in the stream;
- the *splice writer* copies the asset's stream bytes with links re-pointed at the zone's entries, script strings
  re-indexed into the zone's table, pointers into the asset itself moved, and chosen sub-arrays cut out (their
  counts zeroed).

gfx_world.cpp now uses it; zm_latest rebuilds byte for byte as before (the regression check).

**The seven readers** (`zonekit/level_assets.hpp`) are transcribed from the IDB, from `Load_TerrainGfx`
0x7FF71E7EB650 down to its tile grid, `Load_ComWorld`, `Load_CpuOcclusionData_cand` 0x7FF71E7D16F0,
`Load_GameWorld` with `Load_GameWorldPath_cand` 0x7FF71E7E3B60, `Load_Glasses_cand`, `Load_StaticLevelFxList_cand`
and `Load_NavVolumeData`. All seven decode DM's assets exactly (the trace replay: each ends where the next asset starts).
`ffinfo zm_silver --trace <t> --level-assets` shows what each one links and whether it can be copied:

| Asset | Size | What the copy carries |
|---|---|---|
| terraingfx | 1.08 MB, 640 B without tiles | the +224 block: two map-wide images, a 96 x 2 texture, two sets of six images (11 image links, 7 of them DM's own) |
| staticlevelfxlist | 0.21 MB, 40 B empty | nothing |
| glasses | 80 B | nothing (DM's holds no glass) |
| com_map | 0.65 MB | 91 light-cookie image links (11 images) |
| cpu_occlusion_data | 0.19 MB | 2 script strings; no pointers at all (step 1's "inner pointers" in it were false) |
| game_map | 3.37 MB | 180 pointers into itself, 200 script strings |
| navvolume | 80 B | 2 streamkey links |

No field of any of them points into another asset's data, so no copy needs shared data inlined. A byte diff of the
built zone against DM's confirms that each copy differs only where a link, string or pointer was re-pointed.

**The terraingfx.** The 2026-09-26 empty one (every pointer null) crashed every render thread on texture slot 105.
The +224 block holds textures the renderer binds whatever the terrain, so 2a keeps it and cuts the tiles, the images
+56, +72 and the collision +152 (DM's +160..+216 are empty). `--keep-terrain` copies it whole.

The renderer's readers of the copy were checked in the IDB (2026-09-30). `g_terrainGfx_cand` 0x7FF7340D6350 is set by
`Terrain_InitFromWorld_cand`, and about 17 functions read it. Each one reaches the tiles, and the +56/+72 texture-id
cache, only through the tile count at +8, which the copy zeroes. `Terrain_UpdateFrame_cand` resets that cache with a
memset of `4 × +64` bytes, zero here. `Terrain_BindMapWideTextures_cand` binds the +224 block (slots 105 and 106).

**cwlink build** now adds all of it (`AddLevelWorldAssets`, `AddLighting`), with by-name references for what the
copies link. `--library-world` leaves them to DM, as before. zm_latest: 2.24 MB → 5.60 MB, 2,402 assets (2,354
by-name references, 2,306 of them the lighting's images).

**What the map still takes from zm_silver.ff** (static count on the new zm_latest): 2,340 assets, 7.30 MB, all of it
what the copies link. That is 2,308 images (the lighting's 2,296, mostly under 2 KB, plus the terrain's and lights'), 10
streamkeys, the four sky domes (xmodels with 6 meshes and 4 skeletons), 6 materials, a klf and a winddef. Everything
else they link is in zones that stay (core_bootstrap, core_common, zm_common and their techset zones).

**Test (one PC is enough):** rebuild a map with Build (or `cwlink build`), then play it. Die Maschine still loads under
it. Look for:
- in cwlink's report: `level world: copies of zm_silver's, under its names: ...` and `lighting: the base map's ...`;
- no crash while the level loads and draws (the terraingfx without tiles, the copies);
- Die Maschine's placed effects gone (smoke, sparks, embers around its start area), and its icy ground gone if it
  still showed;
- zombies still spawning and chasing (the game_map copy), lights and sky as before (com_map, lighting).

**Test 1 (2026-09-30 21:47): crashed while loading; fixed.** The level loaded, and the renderer crashed on an early
frame in `R_Image_MarkUsed_cand` 0x7FF726E88130, called from `R_SetupFrameLightingImageSet_cand` 0x7FF725886C80.
Each frame the renderer finds the lighting image set that contains the view (lighting +208/+216: 128-byte sets, each a
convex volume; `R_Lighting_FindImageSetAtPoint_cand` 0x7FF725864850). It then marks that set's six images as used by
their slot in the image pool, (image − pool) / 208.
- The sixth image (+120) was still Die Maschine's stored link. Mapkit's lighting walker loaded five images a set, but
  `Load_LightingImageSet128Array_cand` 0x7FF71E7DAFD0 loads six.
- A link loads no stream bytes, so the walker still read DM's zone exactly and the miss never showed. The copy kept
  DM's value, which in our zone leads to unrelated data, so the image pointer and its slot were garbage.
- The P4 lighting test presumably passed because its player never stood inside that set's volume.

The fix:
- the walker takes six images, so the copy links 2,318 assets, one more;
- every copy now fails the build if its bytes hold a link its reader did not record (`FindAssetLinks`,
  `zonekit/xasset_list.hpp`). That covers the lighting, the gfx_map and its sanims, and the seven level assets. All
  pass on zm_silver.

A byte check of both builds confirms that exactly that one field (lighting copy +0x70E6A4) changed from DM's value
0x40000000002C18D1 to our image entry. A 22:18 run crashed the same way because the game still had the 21:45 zone,
which was never rebuilt. The fixed build was installed at 22:19.

**Test 2 (2026-09-30 22:26): passed.** The user rebuilt zm_latest with Build at 22:25. The zone came out byte-identical
to the checked fixed build. The level loaded and ran until the game closed at 22:45, with no crash, and the user
reported it working. The match's census (`zm_latest_20260930_222645.mkuse`) settles which copies the game used. Each
of the 24 assets the zone holds under Die Maschine's names is the entry a lookup finds, with DM's own chained behind
it:
- the stage-1 world: gfx_map, clip map, streamerworld, districts, navmesh, entity and trigger lists, and the 9 sanims;
- the 2a copies: lighting, terraingfx, the empty staticlevelfxlist, glasses, com_map, cpu_occlusion_data, game_map
  and navvolume.

The log shows only faults earlier runs show too: the DLL's memcpy probes at world start (`VCRUNTIME140+0xCAA7` under
discord_game_sdk.dll) and a `telescope.dll` fault as the game closes.

**Still Die Maschine's, noted in test 1:** the map's picture in the Zombies menu, the loading screen's background video
and the loading screen's instructions image. They come from zm_silver's maptable entry, which stage 1 aliases for the
map. A map of its own needs its own entry for them (a later P6 step, after 2b).

**Next, step 2b,** in two halves: first copy those 2,340 dependencies while DM still loads (2b-1, below), then load the
map without DM's zones (2b-2). One ordering trap for 2b-2: a by-name reference resolves when it is linked, so each copy
must come before whatever links to it (the gfx_map's references to the lighting and streamerworld come first today).
Then the spawner templates from zm_common, the map's scripts injected, and a census match without DM.

## P6 step 2b-1: what the map's copies link, copied too (PASSED in the game 2026-10-01, on the second test)

**Why, and why alone.** 2a's world copies still link 2,340 of Die Maschine's assets by name, mostly the lighting's
images. 2b-1 copies those too while DM still loads, so a crash points at a copy, not at something missing. Dropping DM's
zones is 2b-2.

**The readers** (`zonekit/library_assets.hpp`): recording readers for the eight types the copies link: image, streamkey,
xskeleton, xmodelmesh, xmodel, material, klf and winddef. They are transcribed from the IDB. Three types keep zone-local
script strings, which a copy re-indexes:
- xskeleton: one per bone at +8, and +0 of each 8-B entry at +16 (`Load_XSkeleton` 0x7FF71E7F8EB0);
- a mesh's surface data: +436, each u32 at +440, and +0 of each 8-B entry at +448 (`Load_XModelMeshSurfaceData`
  0x7FF71E7F93B0);
- xmodel: +8 of each 40-B attached-model entry.

Materials, images and streamkeys hold none (no caller of the two resolve functions loads them). klf and winddef got plain
loaders as well; until now mapkit read them only as by-name references. A material with its own local techset cannot be
copied yet. None of those needed has one.

`ffinfo zm_silver --trace <t> --library-assets` reads every one of DM's own assets of those types. That is 11,736 images,
8,494 streamkeys, 3,254 skeletons, 14,717 meshes, 3,254 models, 8,066 materials, 42 klfs and 1 winddef, and each one
ends exactly where the next asset starts.

**Shared data.** A fastfile stores identical data once, so a later asset's pointer points into the data of the asset that
loaded it first. Almost every model, mesh, skeleton and material in DM does this (3,253 of 3,254 models, 8,009 of 8,066
materials), and so do 279 images. A copy of such an asset needs the asset whose data it shares copied first. Its pointer
then goes to where that copy's data landed (`RecordSplice::shared`, `RecordLanding`). That owner is always earlier in the
zone, and always of a type mapkit copies.

**The missed-link guard, corrected.** Run over all of DM's images, 2a's guard flagged 26 images and 3 streamkeys. In
texture data, a run of zero bytes and a 0x40 byte read like a stored link. A pointer field always sits at an 8-aligned
block position (the stream has no padding, so the stream offset can be anything), so a match at any other position is now
ignored. The case the guard was written for, a missed image field, is still caught.

**cwlink build** now copies them (`AddLibraryCopies`). For every by-name reference in the zone to an asset DM holds
itself, it copies that asset under DM's name, along with what it links and the owners of the data it shares,
recursively.
- The copies go at the front of the zone, each ahead of the references to it.
- Links still go through by-name references, because after the override swap the zone's own entry holds the loser.
- What the zone holds itself under a DM name (the creator's sky material) is never copied over.
- An asset that another asset's load writes into stays DM's, linked by name: the lighting's 8 streamed-texture keys
  (`LoadWritesInto`; test 1 below).
- `--link-library` leaves all of it to DM.

For zm_latest:
- 2,355 copies, 7.33 MB: 2,325 images, 8 materials, 7 meshes, 6 skeletons, 4 models (the sky domes), 2 klfs, 2
  streamkeys (the navvolume's) and 1 winddef. 2,320 of them are what the zone linked by name; the other 35 are what
  those link, or share data with. Only the lighting's 8 streamed-texture keys stay linked to DM.
- 1,613 of the images are streamed: the zone holds their roots and smallest levels, and the rest streams from DM's
  packages by key. Together they reserve 590 MB of block 7, which is never stored (DM's own zone reserves 15 GB there).
  This is the first mapkit zone with a block 7. Retail's own 4k_/1080_ variant zones override streamed images the same way.
- The zone grows to 6.57 MB, with 4,800 assets.
- The image pool is the tightest: 111,102 of its 118,784 items with DM's images loaded too.

**Checks, offline:**
- the zone walks completely with mapkit's loaders;
- `ffinfo zm_silver --trace <t> --check-copies <zm_latest.ff>` reads every copy back and compares it with the original:
  2,355 checked, 0 problems. The bytes match except where a pointer or string was re-pointed. 93 links name the same
  assets, 33 pointers into the copy itself and 58 into shared data land on the same piece of data, and 13 script strings
  read the same. Pointed at the wrong zone (zm_common), it reports differences, so the check is not vacuous;
- the static count against the new zone finds 8 assets left to take from zm_silver.ff, the lighting's keys;
- a build with `--link-library` is byte-identical to the 2a build that passed.

**Found on the way, for 2b-2:**
- zm_common holds 23 of the copies' names itself, all among the 35 extra copies. 10 of its versions are identical, and
  13 differ by a few bytes. For each of those 13, DM's version was already the entry lookups found in the 2a match. The
  copies are DM's, so nothing changes now, and once DM is gone the same versions stay in use. The 2,328 the zone linked
  by name were held by zm_silver alone in that match, so no 4k_/1080_ variant or zm_common version competes with them;
- the streamed copies stream from DM's packages; whether those still open without DM's zones is the next question;
- DM's materials link techsets that only `techset_zm_silver` holds (18 in the census count);
- the lighting's 8 streamed-texture keys are copied there: with no DM zone loaded nothing is swapped, so the owner the
  lighting writes stays on the copy (test 1).

**Test (one PC):** play zm_latest. The build is installed (19:42, the same as Build makes). Look for:
- no crash while it loads or plays (2,325 images copied over DM's in the override swap, 1,613 of them streamed);
- the lighting, the sky and its domes as before: same brightness, no missing, black or blurry textures;
- afterwards, the census shows the copies as the entries lookups find, as 2a's world assets were.

**Test 1 (2026-10-01 19:24, and again 19:26): crashed while loading; fixed.** Both runs crashed the same way, a moment
after the level looked up its world (gfx_map, com_map, navmesh): a read of null + 0x70 in `StreamKeyType2_Install_cand`
0x7FF729493880. The stream-in system (`StreamIn_InstallQueue_cand` → `StreamIn_ResolveRequest_cand` →
`StreamIn_InstallStreamKey_cand`) had streamed in one of the lighting's texture keys and called its type's install
callback (`g_streamKeyTypeCallbacks` 0x7FF72AF35800, by the key's u8 @54), which reads the key's owner at +40.
- **Who sets the owner.** A key of type 1 to 4 is installed through an owner that the asset linking it sets while it
  loads. The lighting's 8 keys are types 2 and 3 (all of DM's keys of those types). `Load_LightingVolumeArray` and
  `Load_Lighting` call `Lighting_LinkStreamedTextureA/B_cand` 0x7FF7294938A0/B0, which write key+40 = the volume or
  shadow-region entry, a pointer into the lighting's own data.
- **Why the copy's owner was null.** Overrides are deferred. `DB_PostLoadFrame_ApplyOverrides` 0x7FF727EC4A30 applies them
  a frame after the whole zone has loaded, and `DB_SwapXAssetHeaders` swaps the two assets' bytes. While the map's zone
  loads, the lighting copy's by-name reference still finds DM's key, so the owner went into DM's key. The swap then put
  the copy's bytes, owner null, in that key's place, where every lookup finds it. In 2a the keys were DM's alone:
  nothing was swapped, and the owner stayed.
- The engine handles this for type 4 only: a swap hook moves its owner (`StreamKey_PreSwap_MoveType4Owner_cand`
  0x7FF72495FB50). Types 0 and 5 (the navmesh and navvolume keys) have no callback.
- The IDB has no other such write. Every loader of the copied types, and of what links them (lighting, com_map,
  terraingfx, navvolume, gfx_map, material, xmodel, klf), was checked for writes into a linked asset. Their helpers
  touch only the asset's own data (`Material_CreateStaticConstantBuffer_cand`, `XModelMesh_PostLoadSurfaceData_cand`,
  `StableSort8_cand`).

The fix: `LoadWritesInto` (`zonekit/library_assets.hpp`) names such assets (streamkeys of types 1 to 4), and
`AddLibraryCopies` leaves them DM's, linked by name as in 2a. cwlink reports "not copied: 8 x streamkey". The rebuilt
zone has 2,355 copies and 4,800 assets. It walks completely, `--check-copies` finds 0 problems, and the static count
finds exactly the 8 keys left. The 8 key names occur in it only as by-name references; in the crashed build each was
also a copy. The `--link-library` build is still byte-identical to 2a's. Installed 19:42, byte-identical to the checked
build.

**Test 2 (2026-10-01 23:05): passed.** The level loaded and ran until the game closed at 23:08, with no crash, and the
user reported it loading perfectly. The log shows only faults earlier runs show too: the DLL's memcpy probes at world
start (`VCRUNTIME140+0xCAA7`), and as the game closes `telescope.dll` and `BlackOpsColdWar.exe+0xD59FED6` (logged at
every close since 2026-09-24) and the 755-field clientfield warning (every in-game shutdown since 2026-09-29). The
match's census (`zm_latest_20261001_230615.mkuse`) settles which copies the game used:
- every one of the 2,355 copies is the entry a lookup finds, with DM's own chained behind it: 2,325 images, 8
  materials, 7 meshes, 6 skeletons, 4 models, 2 klfs, 2 streamkeys and 1 winddef. So are 2a's world assets, as before.
  Not one asset the zone holds under a DM name lost the swap;
- the lighting's 8 streamed-texture keys are DM's alone, as built;
- the stream-in system asked for 1,576 of the copied images (3,586 requests), and each request found the copy, so their
  larger levels streamed from DM's packages into the copies.

## P6 step 2b-2: loading without Die Maschine's zones (built 2026-10-02, PASSED in the game 2026-10-03)

**What loads now.** A map of its own loads `zm_common`, core and its own zone, plus one zone of Die Maschine's:
`techset_zm_silver`. That zone holds only shaders (562 techsets, no other assets). It stays for one techset that
nothing else holds: `C84D8285C9443CAB`, which the sky material `792207D7B4B8B185` and the four sky domes draw with.
Copying it means a techset reader for techniques and shader passes, which is a step of its own (2b-3). `zm_silver.ff`,
`en_`, `ww_`, `4k_` and `ww_4k_zm_silver` no longer load.

**What the engine does without them** (IDB 2026-10-01, renamed and commented):
- **Packages.** Streamed data (a streamed image's larger levels, a streamed mesh's buffers) is read from packages:
  `zone\<name>.xpak` and its `.xsub` files. They are per mode, not per map: `zm`, `zm_postship`, `zm_sink` and `core`.
  - Once a zone has loaded, `DB_OpenZonePackages_cand` 0x7FF72928AB20 opens every package listed under
    `xpak_read` in the `keyvaluepairs` asset (0x4B) named after that zone.
  - `zm_silver`'s list has `zm`, `zm_postship`, `zm_sink`, `core` and their `_rt`/`_hd`/`_audio` packs.
    `zm_common`'s has only `zm_sink` and `core`.
  - So without DM's zone, the `zm` and `zm_postship` packages would never open, and the streamed copies would keep
    their smallest levels.
  - Lookup details: the registry is `g_buildKvTables_cand`, with 65 slots, filled by each keyvaluepairs asset's link
    callback. Keys are hashed by `BuildKv_KeyHash_cand` 0x7FF7295FC9B0: `xpak_read` = 0x1C4D6.
- **Missing assets.** A lookup or by-name reference that finds nothing does one of three things:
  - for an xmodel, image, material, AI type, weapon, script bundle or string table, it gets the type's default as a
    stub (`DB_LinkMissingReference` 0x7FF727EC1860);
  - script types, the navmesh and the navvolume return null;
  - a world type is fatal: `Sys_ErrorCode 0x3580ADA5` for gfx_map, clip_map, com_map, lighting, terraingfx and so on,
    and `Com_ErrorDrop` 0xDE8F2849 for a lighting reference.
- **The world.** The map's zone is now the level zone (flags 0x100, no override flags). So
  `DB_PostLoadFrame_ApplyOverrides` calls `R_BeginLoadWorld("zm_latest")`, and `R_LoadWorldFrame` looks up
  `maps/zm/zm_latest.d3dbsp` with `DB_FindXAssetHeader`. The client's existing alias turns that into zm_silver's
  name, which the map's world keeps. Giving the world its own names is left for later.

**What a map built without DM's zones changes** (cwlink, the default now; `--with-library` builds the 2b-1 form,
byte-identical to the build that passed).

The zone:
- **Package list.** A keyvaluepairs asset named after the map lists DM's 19 `xpak_read` packages. The one value DM
  stores by pointer is left out: it names DM's own world, and no package has that name.
- **Lighting keys.** The lighting's 8 streamed-texture keys are copied. Nothing is swapped now, so the owner the
  lighting writes stays on the copy (`LoadWritesInto` applies only while DM loads).
- **Order.** Without DM, a by-name reference finds only what has loaded before it:
  - the lighting goes before the empty streamerworld, which references it inline;
  - the creator's sky material goes before the copied sky domes that draw with it;
  - the library copies go after the zone's own assets they link.

  Two new guards fail the build otherwise. `XWriter` reports a link to an entry written later (a forward link, broken
  in any build). The written zone is also checked for a by-name reference that comes before the asset it names.
- **What zm_common lacks.** cwlink reads `zm_common`'s trace, which sits next to DM's, to learn what stays loaded:
  60,554 assets of its own.
  - **Spawners.** DM's spawner templates take zm_common's AI types (aitype tables of the GSC dump):
    - `spawner_zm_zombie_ndu` and its `_wall_pull` variant become the plain `spawner_zm_zombie`;
    - DM's armoured heavy becomes Outbreak's `spawner_bo5_zombie_sr_armor_heavy`, which has the same character, state
      machine, behavior tree and score type. `zm_common`'s bgcache registers both.
  - **Entity models.** Every model a script_model draws goes into the map's bgcache, since DM's, which listed them,
    no longer loads. DM's own `p9_zm_ndu_power_on_switch` is copied, with its xcollision: a new copy reader for that
    type. It is just a root, a skeleton and a streamkey.
    A model that physics can move also links a physpreset from its xcollision. Those are copied since 2026-10-03
    (see "The debug map").
- **Copy count.** 2,492 assets are copied, 7.51 MB, and none is left linked to DM.

Level scripts:
- no stand-ins for DM's level scripts (nothing links them now);
- the level script no longer names DM's weapon table (`level.var_d0ab70a2`, only in DM's zone): the generated
  `settings()` sets it when DM's zone loads;
- the exfil's waves are `exfil_realm_<n>` (zm_common's, Outbreak's) in place of DM's `exfil_silver_<n>`. The exfil
  script reads the bundle with no check that it exists.

`map.json`: `"library": [ "techset_zm_silver" ]`.

**The client** (mapkit_loader 7.):
- when `map.json` lists library zones, the zone list keeps only those of the base;
- the map's zone keeps level flags;
- a map without the list loads its whole library, as before.
- Example, from the last run's list: `[en_ 0x8100, ww_ 0x1000100, 4k_ 0x40100, ww_4k_ 0x1040100, techset_zm_silver
  0x200100, zm_silver 0x100, zm_latest 0x100]` becomes `[techset_zm_silver 0x200100, zm_latest 0x100]`.

**Found in the 2b-1 match's census, gone with DM's zones:**
- DM's world streaming in the background (6,948 assets);
- DM's bgcache: 500 xanims, 16 items, 180 script bundles;
- DM's own scripts;
- 111 intel models that a UI function asked for, through DM's intel bundles.

The two script builtins that look up bundles check that the bundle exists first, so a missing one returns undefined
(`Scr_GetScriptBundle_cand`, `Scr_GetItemBundleByIndex_cand`).

**Checks, offline:**
- the `--with-library` build is byte-identical to the 2b-1 build that passed;
- the zone walks completely: 5,114 assets, and the block sizes match;
- `--check-copies` checks 2,492 copies and finds 0 problems;
- the static count with `zm_common` kept finds 0 assets left to take from `zm_silver.ff`;
- of the zone's other 93 by-name links, the 2b-1 census shows each held by a zone that stays, except the sky's
  techset, which `techset_zm_silver` holds;
- the package list reads back: it is named after `zm_latest`, has 19 `xpak_read` pairs, and ends at the stream's end.

**Not settled offline:** one actor clientfield (`hash_c5d06ae18fde4c0`), which `zm_silver.csc` registers by hand. Its
server half is zm_common's `script_789f2367a00401d8`, pulled in by `zm_ai_dog`, `zm_ai_hulk` or Onslaught's scripts,
which may or may not still be linked. The "Clientfield diff" line in client.log says.

**Test (one PC):** play zm_latest (installed 00:15; the DLL 00:13). Look for:
- in client.log: `zone list for a map of its own: [techset_zm_silver 0x200100, zm_latest 0x100]`;
- in client.log: `world start: gfx_map 'zm_silver' (mapkit's own)`;
- no crash while it loads or plays;
- textures as sharp as before (streaming from the `zm` packages), and the sky drawn;
- zombies spawn: zm_common's plain zombies now, Outbreak's armoured heavies, and the plague dogs;
- the power switch drawn;
- no "Clientfield Mismatch";
- afterwards, the census without DM: new stubs against the 2b-1 baseline, which had about 1,680, mostly other maps'
  xanims, images and bundles that bgcaches list.

**Result (2026-10-03): passed.** zm_latest was played on this build in the ambient-room tests:
- client.log: `zone list for a map of its own: [techset_zm_silver 0x200100, zm_latest 0x100]` and
  `world start: gfx_map 'zm_silver' (mapkit's own)`;
- the clientfield diff: 754 fields on each side, none only on one side and none different, so the actor clientfield
  `zm_silver.csc` registers by hand still matches;
- no crash; zombies spawned, and the power switch looked right (the user's report);
- the sound bank copy worked too (see "Ambient rooms").

Not reported on their own: how sharp the textures are and the sky. The census without DM was not run.

## Ambient rooms: how a map sounds (built 2026-10-03; PASSED in the game 2026-10-03, on the second test; parked)

**The problem.** A map of its own sounded like one big echoing room everywhere, and oddly in places. That came from
P1 step 2 (`d4e71c4`, 2026-09-28), not from stage 1 the day after: until then `cwlink build` wrote no trigger list,
so Die Maschine's own stayed live, its room triggers included, and its rooms played where its rooms had been. Since
then the trigger list holds only the map's zone volumes, doors and power switch, so no room trigger is left.

**First test (2026-10-03): no change.** Two `MkAmbientRoom`s, `outdoors` and `indoors`, built without `--with-library`:
the map sounded the same in both and outside them. Two reasons, either enough: neither name is a room of any bank
(the build said so), and since P6 step 2b-2 no bank with rooms loads at all. Die Maschine's sound bank is in
`zm_silver.ff`, and the map's zone carried no sound asset of any kind (the library copied models, materials, images,
streamkeys, a klf and a winddef). That is what the bank copy below is for.

**Second test (2026-10-03): passed.** The default build (the bank copy, with its acoustics) and rooms named after Die
Maschine's. Outside every room the map sounds normal, with no echo (the default room). Each room sounds different
from the others. Inside `zm_nacht_bunker_hallways` the sound also changes from point to point. The IDB explains why
(below): a room sets how a whole area sounds, and on top of that Die Maschine's baked acoustics change the reverb and
occlusion with where the player and each sound are. The feature is parked here: custom room sounds come back later in
the project ("Later" at the end of this section).

**How the game picks a room** (IDB 2026-10-03, renamed):
- When the level loads, the client spawns the trigger list (`CG_SpawnMapTriggersAndEntities` 0x7FF727F0E890 ->
  `CG_SpawnClientTrigger` 0x7FF727EF58D0). Only triggers whose spawnflags have 0x880 set go that way.
- A `trigger_multiple` with targetname `ambient_package` is an ambient room. Its String key `script_ambientroom` names
  the room: the engine keeps the FNV-1a hash of the name, lowercased (0x7FF71E916710, the same hash as asset names),
  and the key must be a String, since the engine reads the pointer at +8. `script_ambientpriority` is read as an int
  (0x7FF7293EB9E0: a String is atoi'd, an Int or Float is used, a Hash is an error drop). It goes to
  `CG_RegisterAmbientRoomTrigger` 0x7FF724DC9370 (Arxan-obfuscated), a table of 1792 entries by entity number
  (`g_cgAmbientRoomTriggers`).
- `ambient_blend` (`triggerA`/`pointA`, `triggerB`/`pointB`) blends two rooms (`CG_RegisterAmbientBlendTrigger`).
  `audio_step_trigger`, `audio_bump_trigger` and `audio_material_trigger` are spawned the same way. mapkit writes
  none of these yet.
- Each frame (`CG_UpdateAmbientRooms_cand` 0x7FF724D89090) the listener takes the room of the trigger it stands in,
  and room 0 when it is in none: sound command 65, `SND_SetListenerRoom` 0x7FF729538940 (listener +112).
- The sound thread (`SND_UpdateAmbientRooms` 0x7FF729538BE0) looks the room up by its hash in **every loaded bank**
  (`SND_FindRoom` 0x7FF729221920): bank +80, 136-B records x u32 @72, the name hash at +0. Room 0, and a name no bank
  has, play the **default room**: the first room with `defaultRoom` set (`SND_FindDefaultRoom` 0x7FF729220F40,
  `SND_ApplyListenerRoom` 0x7FF729538990). With no bank that has rooms there is no room at all.
- A room is more than reverb. Its record (field names from the schema at 0x7FF72A3C47E0): +0 name, +8 `defaultRoom`,
  +9 `OverrideTriton`, +16/+24/+32 `reverb`/`nearVerb`/`farVerb` (hashes), +40 `ReverbDryLevel`, +44 `ReverbWetLevel`,
  +48 `RoomOcclusion`, +52 `RoomVerbAttenuation`, +56 `loop` (the room tone, played as a 2D loop:
  `SND_PlayRoomLoop` 0x7FF7295387D0), +64 `duck` (a mix), three entity contexts at +72 .. +112 and a global context at
  +120. Die Maschine's rooms use the entity context `ringoff_plr` = `indoor` or `outdoor`
  (`SND_RoomRingoffSide` 0x7FF7295386E0): which tails the weapons' shots ring off with. So with no rooms the map also
  loses its room tone and its indoor/outdoor gunfire.
- Other rooms the engine picks itself: a forced room (two hashes, set by sound commands), `underwater` when the
  listener is under water, and while a dvar (off_7FF72E407D18) is on, two fixed rooms by hash (0x31DA7AD856AE50A3,
  0x69A9D09F6A8C22ED; names not found) chosen by something sub_7FF728B7BA60 finds (`g_sndAutoRoomState` 1 or 2).

**The level's sound bank and acoustics** do not depend on the map's name, so stage 1 changed nothing here:
- `DB_LinkSoundBank` 0x7FF728A3E290 queues sound command 37, `SND_AddBank(bank, zone priority, zone flags & 0xF)`
  0x7FF7292202E0. Bank +0x18 is a key: `all` (0x66F09B190519DAA4) for the banks every level loads; otherwise the
  first bank sets `g_sndLevelBankKey`, and a bank with another key is skipped. Nothing compares it with the map's
  name, so a copy of Die Maschine's bank in the map's zone is the level's bank.
- Bank +0x68 is its sound_acoustics, loaded by `SND_Triton_LoadBankAcoustics` 0x7FF729992B50 only when it is not
  null. The engine runs Microsoft Project Acoustics (`TritonRuntime` 1.2.8, buses `bus_triton_indoor_small/medium/large`
  and `bus_triton_outdoor_medium/large`). Its probe data is baked from Die Maschine's geometry, so Die Maschine's walls
  shape the reverb and occlusion on a mapkit map wherever the bank is Die Maschine's.

**Why a room sounds different from point to point** (IDB 2026-10-03, renamed and commented):
- **A room's own settings are the same all over it.** They are:
  - its reverb (+16): a hash into the global table of reverb presets (48-B entries, `SND_FindReverb` 0x7FF7295FE620,
    `SND_GetRoomReverb` 0x7FF728818B70). An unknown hash falls back to 0x56A247DF5F4D2E8B.
  - its dry level (+40), which scales the direct signal of every sound (`SND_VoiceDryLevel` 0x7FF728818E20);
  - its wet level (+44), which scales the reverb signal of every sound (`SND_VoiceWetLevel` 0x7FF72881B2F0);
  - its loop, duck and contexts (above).
- **Triton adds the part that changes with position.** For each sound source, `SND_Triton_UpdateEmitter`
  0x7FF7299926B0 calls `SND_Triton_QueryEmitter` 0x7FF729992CB0, which calls `TritonRuntime_QueryAcoustics`
  0x7FF729D1AE60 with positions times 0.0254 (metres). The query returns 14 values for that source and the listener,
  interpolated from probes baked over the level:
  - the direct path's delay and loudness: occlusion through walls;
  - the direction the direct sound arrives from: around a corner, or through a doorway;
  - the reflections' delay and loudness, and their loudness from six directions;
  - the early decay time and the reverb time.

  It also reads the listener's outdoorness (`TritonRuntime_GetOutdoornessAtListener` 0x7FF729D1AAC0; 1 with no
  data). -1e10 means the query failed, for example outside the baked probes. It is then retried 12 units higher, and
  then at up to three points 12, 24 and 36 units away.
- **Triton mode** (`SND_Triton_ReverbActive` 0x7FF7299933B0) needs four things:
  - the acoustics are loaded;
  - the dvar at off_7FF72E407D40 is on;
  - the six reverb presets the acoustics asset names are found (+104/+112/+120/+200/+208/+216, probably the
    `bus_triton_*` ones above);
  - the current room does not set `OverrideTriton`.

  In Triton mode the mix runs six reverb buses with those presets, in place of the room's reverb
  (`SND_SetupReverbBuses` 0x7FF727B12BF0). Each sound's reverb is split over the six by curves of its Triton values
  (`SND_Triton_ReverbBusGains` 0x7FF7299921D0, `SND_SetVoiceSends` 0x7FF728813350). Its panning leans toward Triton's
  arrival direction (`SND_UpdateVoice3D` 0x7FF7288218E0). The room's dry and wet levels still scale the result.
- **`OverrideTriton`** (+9, `SND_CurrentRoomOverridesTriton` 0x7FF729538790) turns Triton mode off while that room is
  the current one. The room's one reverb then plays on one bus, the same at every point. A room without the flag
  changes from point to point, so `_bunker_hallways` probably has none (the `--no-acoustics` test below would tell).
  Which of Die Maschine's rooms set it is in their records; `ReadBankRooms` reads only the names and `defaultRoom` so
  far.
- **So a room has general properties** (reverb preset, levels, room tone, duck, indoor or outdoor), and the per-point
  acoustics come from Triton: how much echo, how long, from where, how muffled. On a mapkit map the per-point part
  comes from Die Maschine's probes at the same coordinates, so it follows Die Maschine's walls, not the map's.
- Not traced yet: `nearVerb` and `farVerb` (+24, +32), `RoomOcclusion` (+48) and `RoomVerbAttenuation` (+52).

**What mapkit does:**
- `MkAmbientRoom` (a volume: `room`, `priority`) exports as `ambient_room`. In Godot the room field suggests Die
  Maschine's six named rooms, and the map check warns on any other name.
- cwlink (`AddAmbientRooms`) writes a copy of one of Die Maschine's `ambient_package` triggers per room. It is cut to
  its classname, spawnflags and client_server, with targetname `ambient_package`, the room's `script_ambientroom` and
  `script_ambientpriority`, and a box trigger model (contents 0x82000000, as zm_silver's `trigger_multiple`s).
- The build reads the bank's rooms (`ReadBankRooms`): how many it has, which is the default, and for each placed room
  whether the bank has it, loudly when it does not. Die Maschine's bank names only six of its rooms by its triggers
  (`zm_nacht_bunker`, `_bunker_entrance`, `_bunker_hallways`, `_bunker_medium_room`, `_bunker_small_room`,
  `zm_nacht_interior`); the others are hashes with no name to go by.
- **The bank copy (P6, 2026-10-03):** without `--with-library`, `AddLibraryCopies` copies Die Maschine's sound bank
  into the map's zone, whole: aliases, rooms, ducks, its sound assets and alias modifiers, and its acoustics, each a
  nested asset recorded with the bank (`Ctx::Nested`, library_assets.cpp; a nested asset stored after a
  DB_InsertPointer slot is recorded with its slot, so a later link through the slot follows it into the copy). The
  audio itself streams from the packages by key, as before. A bank that cannot be copied is said in the build's
  output and leaves the map as it was; it does not fail the build. `--no-acoustics` leaves the Triton acoustics out
  (a null +0x68 loads none), so reverb and occlusion come from the rooms alone: the A/B test for Die Maschine's walls.
  With `--with-library` Die Maschine's own bank loads, and nothing is copied.
- `ffinfo zm_silver --trace <zm_silver.mktrace> --library-assets` reads the bank with its copy reader;
  `--check-copies <the map's .ff>` compares the copy with the original.

**Test:** done (the second test above). Still to try when this is picked up again: the same walk with
`--no-acoustics`. If the change from point to point goes away and each room sounds the same all over, it was Triton.

**Later (parked 2026-10-03; custom room sounds come back later in the project):**
- The build lists each room's settings: `OverrideTriton`, reverb, dry and wet levels, loop, indoor or outdoor. A
  creator can then pick a room by how it sounds, and knows which rooms change with position.
- Rooms of the map's own, set in Godot on `MkAmbientRoom`: a room record written into the copied bank, with a name, a
  reverb preset from the global table, dry and wet levels, a loop, a duck, and indoor or outdoor.
- Triton: Die Maschine's acoustics fit only Die Maschine's walls. The options:
  - leave them out (`--no-acoustics`);
  - set `OverrideTriton` on the map's rooms;
  - bake the map's own probes: Project Acoustics bakes them from the map's geometry, and the format is not
    researched.
- `ambient_blend` triggers, for a smooth change between two rooms.
- The Godot dock passes fixed cwlink arguments, so `--no-acoustics` is command line only.

## The debug map: one of everything (built 2026-10-03; five builds stopped, crashed or dropped, each fixed the same day; the sixth loaded and ran 2026-10-05)

`mapkit/godot/maps/zm_debug.tscn` places every object and setting the editor has, each labelled, so one build tests
them all. The README's "The debug map" lists what is where. Its textures (`maps/zm_debug/`) are procedural:
- a checker with coloured corners;
- a brick normal map;
- roughness stripes and a half-metal map;
- a cut-out grate and a glow pattern;
- a sky with the game's axes on its horizon.

It was made with a headless Godot 4.7.2 script, and the result was checked the same way:
- the dock's Check reports nothing;
- Export writes 61 brushes, 80 entities (20 classes), 7 meshes, 45 textures, the sky, and the sun and fog;
- rendered previews (Xvfb, Godot's Compatibility renderer) look as intended: the labels read the right way round,
  the checker's red corner is top left, and under a single light the normal map's bricks stand out.

The labels are TextMesh geometry: 110 of them, about 34,500 triangles. A label is a mesh surface like any other,
so text needs nothing new in cwlink.

**First build (2026-10-03): stopped at the copies.** Everything up to the library copies came out as expected:
- six perks moved, four skipped;
- twelve wall buys and three box locations;
- six ambient rooms;
- the shader-material sample drawn with the default.

Then:
- **The error.** `not copied: 1 x xcollision: no by-name reference layout for physpreset`. The user's cwlink was the
  current one (its output has the sound bank copy of `9c4ae7b`).
- **Why.** One of the props' models can be moved by physics. Its xcollision links a physpreset (+40), a type the
  copies had neither a reader nor a by-name layout for. A map without DM's zones cannot leave it linked to DM, so the
  build stopped.
- **The log hid the reason.** cwlink's stdout was buffered in the pipe and stderr was not, so the line saying why
  landed in the middle of another line.

**The fix** (same day, not yet rebuilt):
- **The copy.** A physpreset is copied like the other library assets: `library_assets.cpp`, from
  `Load_PhyspresetAsset` 0x7FF71E7E4340. It has a 112-B root and no strings. Under block 4 it links an fx (+88), an
  impactsfxtable (+96) and an impactsoundstable (+104).
- **By name.** These now have by-name layouts: physpreset 112 B, physconstraints 48 B, impactsfxtable 24 B and
  impactsoundstable 56 B. One that zm_common holds is linked by name.
- **What only DM's zone holds.** A link of a copy to an asset only DM's zone holds, of a type mapkit has no reader
  for, now fails that copy with the reason (`it links a ... only zm_silver's zone holds`). Before, it went by name to
  nothing. What the engine would do then (IDB, renamed and commented):
  - `DB_LinkMissingReference` 0x7FF727EC1860 stubs the reference with the type's default asset;
  - the default's name is in `g_defaultAssetNames` 0x7FF72A389480: `default` for physpreset, physconstraints,
    techset, impactsfxtable and impactsoundstable, `void` for xmodel and xanim;
  - if that default is not loaded, the game drops (`Com_ErrorDrop_Code` 0xDE8F2849).
- **Clearer output.** The copies that leave something linked are named on a `for:` line, by the source's model
  name, and the last line says to leave them out or build with `--with-library`. stdout is unbuffered, so the
  reason lands where it happens.
- **Checked offline.** zonekit and cwlink compile (gcc 13 and clang 18, with a stand-in `windows.h`). A test ran the
  new reader on a fake physpreset: 112 B read, and its three links resolve by type and name. An inline fx fails, with
  the reason.

**Second build (2026-10-03): stopped at the zone writer.** The copies worked:
- `library: 7166 of zm_silver's assets copied (... 1 physpreset, 17 xmodel, 13 xcollision ...)`, and no `not
  copied` line;
- the level scripts, `map.json` and the navmesh were written.

Then:
- **The error.** `zm_debug: 1 link(s) to an asset written after the one linking it ...: asset 1 (material (sky)) links
  techset 484D8285C9443CAB at asset 2289`. The zone was not written.
- **Why.** That techset is the sky's (`C84D8285C9443CAB` with the reference bit; only `techset_zm_silver` holds it).
  Build writes its by-name reference just ahead of the sky material. A material copied for one of zm_debug's models
  links the same techset, so the copies had a reference to it too. The zone's own references that the copies repeat
  were dropped, and the copies go after the sky material, since the sky domes' copies link that. So the sky material
  linked a techset written after it, which the engine cannot resolve.
- **The fix.** A reference of the zone's own that sits ahead of where the copies go stays there, if it is to an asset
  another zone holds, and the copies drop their own. A link finds the first entry of its name. A reference to a copy
  still goes, since it cannot sit ahead of the copy, and the writer's check reports any asset that links it. With
  `--with-library` (copies at the very front) and for zm_latest (no copy links the sky's techset) nothing changes.
- **Clearer output.** A failed build ends with `FAILED: the lines above say why; the map is not ready to play.`
  Before, it still printed where to find the map in the game.

**Third build (2026-10-03): built, and the game crashed loading it.**
- **The crash.** client.log, 19:02:28, right after `zone list for a map of its own: [techset_zm_silver 0x200100,
  zm_debug 0x100]`: an access violation in `DB_ConvertZoneScriptString` 0x7FF72966F1E0 (`+0xCAAF1FA`). Its callers
  (IDB, renamed): `DB_FinishPreloadedZone` 0x7FF72769F990 -> `Finish_XModelAsset` -> `Finish_XModel` ->
  `Finish_XModelMesh` -> `Finish_XModelMeshSurfaceData`. A model's LOD was walked as if stored inline, and the script
  string at +436 of its mesh info was garbage.
- **What that means.** The lobby preloads the map's zone (flags 0x200; at 19:02:12) with the loaders' twin family:
  - every asset root goes to block 1;
  - the stored value of every pointer field is saved in block 11.

  At launch `DB_FinishPreloadedZone` walks the zone again from those roots and saved values. No mapkit zone holds a
  model with an inline LOD (a copy's LODs are links, and so are the brush model's), so this second walk was already
  out of step: it read a value saved for another field.
- **The likely cause: block 1 too small** (IDB: `Load_TechsetAsset_Preload` 0x7FF71E852EB0,
  `DB_SaveStreamPositions_Preload` 0x7FF72966EF90):
  - every techset field the preload meets saves all 13 block positions, 104 B, in block 1. That is a material's
    techset (+40), even an empty or linked one, and every techset entry. The second walk restores them from there;
  - so a material takes 448 B of block 1 (its 344-B root and a save), and a techset entry 104 B (its root goes to the
    temp block);
  - cwlink sized block 1 by the rule measured on retail zones: 208, plus every root 16-aligned, plus 208 per techset.
    That gives a material 352 B and a techset entry 384 B, which is enough only while a zone has few materials per
    techset;
  - nothing checks a block's size. Block 2 follows block 1 and starts with the zone's script-string id cache, which
    the second walk zeroes first (`DB_SetZoneScriptStrings`). So roots the preload wrote past block 1's end are zeroed
    before they are walked. Their fields then read as empty, and every pointer after them takes the wrong saved value;
  - zm_latest had 54 materials, and its block 1 (`0x107450`) had 35 KB spare in its 64-KB-aligned allocation. The
    materials cost it about 5 KB. zm_debug has 128 (94 copied, 34 of its own), which cost more than 12 KB. The last
    assets in its zone are its own images and materials and the brush model, so the model that crashed is likely the
    brush model, the first one after them.

  Not proven: the old zone is not here to replay. The next build's output says (Test, below).
- **The fix.**
  - zonekit now replays block 1 the way the preload fills it (`XStream::PreloadRoot`, `PreloadFieldEnd`,
    `PreloadSave`):
    - each root at its alignment, then 16-aligned when its field ends;
    - 104 B for each techset or keyvaluepairs field, and for each inline material-local techset.
  - The zone writer gives block 1 the larger of that and the retail rule.
  - cwlink says when the replay is the larger, and whether the rule's block would have held it.
  - `ffinfo --walk` prints the preload's block 1 next to the header's, so a retail zone (`zm_silver.ff`) can check
    the replay against the engine's own sizes.
- **Checked offline.**
  - zonekit, cwlink and ffinfo compile (gcc 13, clang 18).
  - A test writes small zones of by-name references with cwlink's writer, walks them back, and gets the sizes worked
    out by hand from the preload loaders: a techset and ten materials need 4,592 B of block 1 where the rule gives
    4,112.

**Fourth build (2026-10-03): loaded, then dropped at launch.**
- **What happened.** client.log, 21:03:35: the lobby preloaded the zone (14,564 assets), and at 21:03:52 the level
  started (`world start: gfx_map 'zm_silver' (mapkit's own)`). The same second the game went back to the Zombies menu
  with `An error occurred: Uniform 99 Divebomb Karma`, an ERR_DROP (`BB_Alert ... err_drop`). The Discord SDK faults
  logged just before it are its own, and the telescope.dll fault at 21:04:24 is the known exit crash.
- **Block 1 fit.** The preload's trace (`zm_debug.mktrace`, flags 0x200) records every block's base address and
  position. Block 1 ended at 0x1D0588 (1.90 MB) with 0x1E0000 before block 2's base: the third build's overflow did
  not recur. Every other block ended inside its room too.
- **The drop** (IDB, renamed and commented). The callers: `SV_StartMap` -> `G_InitGame_cand` ->
  `G_InitGame_LevelSetup_cand` 0x7FF720913500 -> `G_RegisterZBarriers_cand` 0x7FF720912790 -> `DB_FindXAssetHeader`
  -> `DB_LinkMissingReference` -> `Com_ErrorDrop_Code` 0xDE8F2849.
  - When the level starts, the game walks the entity list for barriers. An entity whose classname is
    `zbarrier_<name>`, or a `content_struct` with a `zbarrier` key (the Mystery Box, when a content flag is set),
    names a **zbarrier** asset (0x53): a window barrier's boards, what they draw and how they animate.
  - It looks each one up by name. zbarrier has no default asset (`g_defaultAssetNames[0x53]` is 0), so a name that no
    loaded zone holds is this drop. The same code drops for a missing lighting.
  - zm_debug's concrete barrier is a `zbarrier_zmcore_basicwallbarrier_concrete_silver` entity (AddBarriers copies it
    from zm_silver). Only `zm_silver.ff` holds that zbarrier, and it no longer loads. The trace shows zm_debug's zone
    holding no zbarrier. The wood barrier's (`zmcore_t8_basicwoodbarrier`) is zm_common's, which is why zm_latest's
    two wood barriers started.
  - The library copies follow links inside assets. A zbarrier is named only by an entity's classname, so they never
    saw it.
- **What a zbarrier is** (`Load_ZbarrierAsset` 0x7FF71E7F99E0 -> `Load_ZBarrierDef` 0x7FF71E7F9910, a 944-B root):
  - under block 4, +120 an xmodel and six 136-B boards embedded at +128 (`Load_ZBarrierBoardArray` 0x7FF71E7F95C0);
  - each board has three xmodels (+0, +8, +16) and two fx (+56, +64), every one a nested asset;
  - a board's four animations are xanim names at +24 .. +48, not pointers. `ZBarrier_InitAnimTree_cand`
    0x7FF72090D3A0 looks them up by name for each board in use (i32 @76);
  - no script strings.
- **The fix.**
  - **A copy reader for zbarrier** (`library_assets.cpp`). The board models are read as nested assets recorded with
    the barrier, so one stored inline travels in the copy. An fx stored inline fails the copy, with the reason
    (mapkit reads an fx only as a by-name reference). zonekit now also walks a zbarrier (`kLoaders`), and it has a
    by-name layout (944 B).
  - **cwlink copies the zbarriers the entities name** (`AddEntityZBarriers`, for a map without DM's zones). Each name
    zm_common lacks and DM's zone holds gets a by-name reference, which the library copies turn into a copy with
    what it links. A name no zone cwlink reads holds is said: core's zones also stay loaded, and cwlink does not
    read them.
  - **The output** has a new line: `zbarriers the entities name: zm_common's ...; copied from zm_silver: <name> (<n>
    boards, <n> animations: <n> in zm_common ...)`. An animation only DM's zone holds plays the game's default
    animation: xanims have one (`void`), and mapkit cannot copy an xanim yet.
  - **An fx only DM's zone holds** no longer fails a copy, when zm_common holds the game's default fx
    (`g_defaultAssetNames[0x33]` = 0x4644FA5FD144D2D2). It stays linked by name, and the game stubs it with that
    default. The library line counts them (`linked by name though only zm_silver's zone holds them: <n> fx`). Any
    other type still fails the copy.
- **Checked offline.**
  - zonekit, cwlink, ffinfo and mkasset compile (gcc 13, clang 18).
  - A test reads a fake zbarrier with the new reader: 944 B, its two models and fx linked; again with a board model
    stored inline (recorded, 1,176 B). Each copy, written after by-name references with cwlink's writer, walks back
    complete. An inline fx fails, with the reason.
  - The physpreset and preload tests still pass.

**Fifth build (2026-10-03): the level started, then crashed two seconds in.** The zbarrier copy worked (no drop;
14,806 assets). Then a null read at `exe+0xB13AE3C` (`cmp [rdi], 0`) in the client's barrier piece update: the piece's
DObj came back null (`sub_7FF722F5E1E0` returns null for a null model). The client sets each board's models up by
name through the bgcache model table (`CG_ZBarrier_SetupPieces_cand` 0x7FF727CE62D0 -> 0x7FF727F0E830, "model %s
not precached."). The concrete barrier's board models were listed only by DM's bgcache, so the lookup failed, the
index stayed 0, and model 0 is null. **Fix:** `AddEntityZBarriers` lists each copied zbarrier's models (its own at
+120 and each board's three) in the map's bgcache; the output line says how many. Compiles (gcc 13, clang 18); not
yet rebuilt.

**Test:** Build zm_debug, play it, and check:
- **the build output:**
  - `zbarriers the entities name: zm_common's zmcore_t8_basicwoodbarrier ...; copied from zm_silver:
    zmcore_basicwallbarrier_concrete_silver (...)`, and no `in no zone cwlink reads` part;
  - the library line: `zbarrier` among the copies, and no `not copied` line. If it says `not copied: ... zbarrier:`,
    the reason follows (a board model or fx the copies cannot carry yet);
  - under the `zm_debug.ff` line, `block 1 sized for the lobby preload: ...` (the fourth build's trace showed the
    resized block 1 holding);
  - perks: six moved, and Tombstone, Mule Kick, PHD Flopper and Death Perception skipped;
  - twelve wall buys and three box locations moved;
  - six ambient rooms;
  - one surface drawn with the default (the shader-material sample);
  - the library line: no `not copied` line. A physpreset is among the copies when DM's zone holds the props' one,
    and is not when zm_common does.
- **sky:** standing in the yard, `+X` is in front of a player facing the map's +X (Godot's -Z), `+Y` on the left,
  and the letters are not mirrored. This checks the sky turn (`_SKY_LEFT_EDGE_YAW`).
- **gallery:**
  - the roughness and gold rows go from mirror-like to matte;
  - the checker's red corner shows top left;
  - the bricks of the normal map look raised;
  - the roughness map shows as stripes and the metal map half shiny;
  - the cut-outs have holes and the glass is see-through;
  - the named surface draws Die Maschine's concrete pillar;
  - the MkMesh `game_material` draws metal scrap;
  - the mirrored panel's checker is mirrored (red corner top right), and it is not inside out.
- **text:** every label reads the right way round, and the floating ZM_DEBUG title has depth.
- **collision:** you walk through the faces ring's hole, the hull ring blocks, and the none ring is walk-through.
- **brushes:**
  - the invisible wall blocks, and the walk-through wall does not;
  - the ramp and stairs reach the platform;
  - the turned brush is drawn and solid where Godot shows it.
- **gameplay:**
  - the level starts (no "Uniform 99 Divebomb Karma");
  - both barriers have boards, zombies tear them down, and you can rebuild them. If the concrete one's boards move
    oddly, the build output said its animations are only DM's;
  - the perks wait for the power, all but Quick Revive in a solo game;
  - the gallery's debris opens when the power comes on;
  - the Armor Station waits for the power, and the Arsenal does not;
  - the exfil can be called from round 1;
  - the yard door's own model blocks the doorway.
- **sound:** each named room sounds different, and the open yard plays the default room.
- **if it crashes while loading again:** add `"zm_debug"` to `"mapkit_trace"` in cw-mod.json and load it once more.
  The preload writes `cw-mod/mapkit/trace/zm_debug.mktrace` before the launch. It holds every block's position at
  each asset, block 1 included, so it shows where the preload and the second walk part.

**Sixth build (2026-10-03 21:59, played 2026-10-05): loaded and ran.** With the zbarrier models in the bgcache the
level starts and stays up. client.log of 2026-10-05:
- 19:04:48 `zone list for a map of its own: [techset_zm_silver 0x200200, zm_debug 0x200]`, the preload, then the load;
- 19:05:04 the clientfield diff: 179 fields on each side, none different;
- 19:05:05 `world start: gfx_map 'zm_silver' (mapkit's own)`, and only the DLL's own memcpy probes after it;
- the game ran until it was closed at 19:20.

The test list above was not gone through. What the user reported from that session are two lighting faults, which
the next section takes up.

## The map's own lighting, step 1: none of Die Maschine's lamps or baked shadows (built 2026-10-05, PASSED in the game the same day)

**The report (zm_debug, 2026-10-05).** Seen from above the map, shadows "sometimes get very pixelated": hard-edged
blocks, some of them where nothing seems to stand (the screenshot). And there are lights in the map with no lamp,
like the lights of the original map. The question was whether `techset_zm_silver` causes this. It does not: that zone holds compiled
shaders only, and the map uses one of them, for the sky. Both faults come from the lighting data the map copies from
Die Maschine (P6 step 2a).

**What was measured** (the zones' streams and the IDB, 2026-10-05; every name below is in the IDB):
- **The lights.** The lighting asset (+8/+16) and the com_map (+12/+16) each hold the same 948 lights of 688 B. They
  differ only in +564 and the cookie pointers. zm_debug's copies were Die Maschine's, byte for byte.
  - 482 of the 948 lie within 1,500 units of where a map lands (Die Maschine's player spawns), the nearest 258 units
    away and 35 above the floor.
  - The client game copies the com_map's list into its own array when a level starts
    (`Com_CopyPrimaryLightsToCG_cand` 0x7FF7295F0520, from `g_comWorld_cand`). So the com_map's list is the one the
    game lights with.
  - A lighting volume lists the lights that belong to it by index (+8960/+8968, with per-light data at +8976). Die
    Maschine's one volume lists all 948, its fallback volume none; each of core_frontend's scenes lists its own.
  - A light (`Light_Copy688_cand`): +64 type, +104 origin, +200 colour times intensity, +232 radius, +336 cone, +524
    origin again with bounds, +624 the colour's largest part, +644 the colour again, +672 cookie image.
- **The baked shadow.** The exe calls it SST (`DrawSST`, `SST Only`): the sun's shadow of the level's static geometry,
  baked into a tree, and drawn where the shadow maps the game renders each frame do not reach.
  - Each lighting state of a volume has a record (128 B at volume +8416): a 3x4 transform, an origin, the tree's size
    in tiles, the texel size, a depth range, and the yaw and pitch it was baked for. Die Maschine's day state: 61 x 34
    tiles of 2.79 units a texel, baked for yaw 120, pitch 25. A tile is 128 texels
    (`R_SunShadowTreeConstants_cand` 0x7FF729475900).
  - The record points at the tree (104 B): a stream key, the node count, and an image holder. The data streams from
    the packages under the key: 14.3 MB for the day state (3,560,614 nodes, then a 512 x 512 image that follows the
    nodes). `Lighting_StreamedTexture_Install_cand` 0x7FF7294939B0 makes the node buffer and gives the image its
    pixels. One image texel covers 16 x 16 tree texels, about 45 units. The blocks in the screenshot look about that
    big, which is a guess from one picture, not a measurement.
  - The renderer takes the record of the volume state the view is in (`R_SetupFrameLighting_cand`: frame +2792) and
    draws the tree while its key is resident (`Lighting_StreamedTexture_IsResident_cand`).
  - zm_debug sets its own sun (yaw -35, pitch 50), and `AddLighting` wrote that into the state. The tree stayed Die
    Maschine's: its buildings' shadows, for its sun.
  - **An empty tree** is how retail says "no baked shadow here": a record of 0 x 0 tiles, a tree of one node, a key of
    786,432 B. Every empty tree's key names the same data in the packages (`133595F7FC2C2E2E`). Die Maschine's
    fallback volume and its unused fourth state have one. So have core_frontend's volumes 0, 1 and 4, in the states
    their masks allow (24 of its 32 records lead to its one empty tree, whose key names that same data). Its baked
    trees are small: one node a tile (209 nodes for 11 x 19 tiles).
- **Shadow regions** (lighting +144/+152, 464 B: a box of 6 planes, then 4 entries, one per state). Each entry is a
  record like the sun shadow's, with its own tree (stream key type 3; a volume's is type 2). Die Maschine has one
  region, baked for yaw 75, pitch 50, which is none of its suns: 14 x 9 tiles of 5 units a texel, 5.5 MB, the same
  data in all four states. What it is for is not known (its direction and the snow suggest where snow falls). It
  holds Die Maschine's geometry either way.
- **The sun cookie** of the day state is `t9_gobo_smoke_color`, a soft smoke pattern. It is not the fault, and stays.

**What cwlink writes now** (the default; zonekit `LightingCopy::DarkenLights`, `EmptyShadowTrees`):
- **Lights black.** Every light's colour (+200 and +644) is zero, in the lighting copy and in the com_map copy
  (`RecordSplice::edits`, new). The lists keep their length and every other field, since the volumes and the
  com_map's own groups name lights by index. `--keep-lights` leaves them lit.
- **Trees empty.** Each baked tree becomes an empty one, modelled on an empty tree the asset already holds (a
  volume's for the sun shadows, a region's for the regions):
  - its records get the empty record;
  - the tree gets one node, and its image holder the empty one's used part (1/511) and depth range (393216);
  - the copy of its stream key names the empty tree's data and size (`AddLibraryCopies`, root edits at +8 and +48).

  The tree keeps its own key and image: a key has one owner, the record that linked it last, and every tree's image is
  512 x 512 of one format. Type 3's install is the same function with the tree at +96 of the entry.
  `--keep-baked-shadows` leaves the trees. With `--with-library` they always stay, because the keys are Die
  Maschine's then.
- A key edit that no copy took fails the build: the lighting would say an empty tree and the key a baked one.

**Checks, offline (zm_debug):**
- cwlink: `its 948 lights black (948 gave light), 6 of its 8 baked shadow trees emptied`, and `6 of the copied
  streamkeys name an empty shadow tree's data (4.5 MB)`. The zone streams 6.0 MB for its 8 trees, where it streamed
  50.7 MB.
- The zone walks completely: 14,806 assets, and the stream ends on its last byte.
- With `--keep-lights --keep-baked-shadows` the zone is byte-identical to the sixth build, which ran.
- Against that build, only three kinds of assets differ: the lighting (21,936 B), the com_map (21,442 B) and six
  stream keys (9 B each: +8 and the size).
- `ffinfo <id>.ff --level-assets` (new: the lighting's lights and trees, and no trace needed for a map's zone) shows
  0 of 948 lights with a colour in both lists, and 8 of 8 trees empty, each key naming `133595F7FC2C2E2E`.
- `--check-copies` counts the six keys apart from problems. It still reports the sky material as one: the map's own,
  under Die Maschine's name (it did so for the sixth build too).

**Test (one PC):** play zm_debug (installed 2026-10-05 19:48). Look for:
- no crash while it loads (the eight empty trees stream in right after the world starts);
- Die Maschine's lamps gone: no pools of light on the floor or the walls that no MkLight explains;
- from above and from far, no blocky shadows, and no shadows where nothing stands;
- near the player, the shadows of the map's own objects as before (the shadow maps of the frame);
- anything new and wrong: black or flickering squares would be the black lights, a dark or over-bright map the
  empty trees.

If the blocks stay, the baked tree was not their cause, and the frame's own far shadow map is next.

**Result (2026-10-05): passed.** The user's report: no crash while loading, the stray lights gone, no blocky shadows
from above or from far away, and the nearby shadows work. client.log of the 19:57 session agrees:
- 19:59:00 the zone installed at 19:48 loads (14,806 assets; its trace was written again);
- 19:59:16 the clientfield diff: 179 fields on each side, none different;
- 19:59:17 `world start: gfx_map 'zm_silver' (mapkit's own)`, followed only by the DLL's own memcpy probes
  (`VCRUNTIME140+0xCAA7`, 14 of them, as at every world start);
- the game was still running at 20:04.

The build changed the lights and the trees at once, so the test does not tell the two apart. But the blocks were
dark patches on a floor the sun lights, which a lamp cannot make: they were the baked tree's. Nothing new and wrong
was reported, so black lights and empty trees in the states a map runs in both hold in the game.

Not looked at: what far objects look like with no baked shadow behind the frame's shadow maps, and the other three
lighting states (a mapkit map stays in the day state).

**What this is not yet.** The copy still carries Die Maschine's level in four places, all baked from its geometry:
the reflection probes (+9024), the GI images (+9376), the +9600 grids and the image sets. A map's surfaces reflect
and take their ambient light from Die Maschine's yard. The steps of a lighting writer, in order:
1. the creator's own lights: MkLight nodes into the two lists, with the volume's index list and per-light data;
2. a baked shadow tree of the map's own geometry (the node format is not reversed), or the frame's shadow maps
   reaching further;
3. probes and ambient light from the map's sky in place of Die Maschine's bakes;
4. one volume of the map's own in place of Die Maschine's box.

## Collision for perk machines, the Mystery Box and barriers (built 2026-10-05, not yet tested in the game)

**The report (zm_debug, 2026-10-05).** A perk machine is walk-through until it has power. The Mystery Box is
walk-through whether it is there or not. The wood barrier is walk-through.

**What was measured** (the zones' streams, the game's scripts and the IDB, 2026-10-05; the names are in the IDB):
- **How a model blocks.** An xcollision's contents are `(q | q >> 26) & 0x3FFFFFF`, with q the u64 at +88 (P2 step
  1). The two halves of q are two kinds of collision:
  - the low 26 bits are the model's detailed shapes, what bullets hit: 0x1 or 0x11;
  - bits 26 to 51 are its clip: 0x10000 player, 0x20000 monster, 0x200 vehicle, 0x400 item, 0x40, 0x1000 and
    0x100000. The game's invisible collision models show which is which: `collision_player_wall_*` hold 0x50000 and
    nothing else, `collision_geo_*` and `collision_wall_*_standard` 0x130200.

  As an entity, a model with no clip does not stop a player. That is what P2's walk-through props had in common.
- **A player's movement mask is 0xA18011** (0x7FF723865EFA sets it in the pmove, 0x7FF724229EA2 as the entity's clip
  mask): 0x1, 0x10, 0x8000, 0x10000, 0x200000 and 0x800000.
- **A perk machine** is a script_model that `zm_perks.gsc` spawns with no clip of its own (`collision = undefined`).
  It gets the perk's `_off` model, and the powered model when the power comes on.

  | Model | Contents |
  |---|---|
  | the `_off` models of Juggernog, Speed Cola, Quick Revive, Stamin-Up, Deadshot and Elemental Pop | 0x11: no clip |
  | their powered models | 0x131651: 0x11 and the clip 0x131640 |
  | Tombstone, `_off` and powered | 0x131651 |
  | Mule Kick (one model for both) | 0x11 |

  So a machine blocks only once it has power.
- **The Mystery Box** is a zbarrier, `zmcore_magicbox`, spawned at each box location
  (`content_manager::spawn_zbarrier`). The asset has no collision model (+120 is null). Its pieces are
  `p9_zm_ndu_magic_box_fake` (shown while the box is away), `p7_zm_der_magic_box` twice (the box, 94 x 26 x 19) and
  `tag_origin` twice (the weapon). Their xcollisions say 0x1.
- **A barrier** is a zbarrier too. `SP_ZBarrier_cand` 0x7FF7287525D0 gives the entity the asset's collision model
  (+120, through `G_SetModel`) and contents 0x2080, or nothing at all when the asset has none. The wood barrier's
  model is `p8_zm_barricade_board_collision`: six boxes, one a board, 8 x 106 x 117 over all. The concrete one's is
  `p8_zm_esc_wall_barrier_collision_col`, a sheet 75 x 65 that starts 34 units up. Both say 0x1.
- **Die Maschine's entity list holds nothing that blocks at any of them:** no brush model, and no collision model
  near a perk struct, a box location or a barrier. A retail map blocks there with clip in its own world, the streamed
  cells, which a map of its own has none of.
- **The other machines carry their own clip:** the ammo crate 0x131641, Pack-a-Punch 0x10201, and the model Der
  Wunderfizz's struct names 0x131651. Not checked: the crafting table and the Armor Station, whose structs name a
  model that neither zone read here holds.

**What cwlink writes now** (`ModelHulls` and `BarrierClips`; `--no-object-clip` builds the old way):
- **A hull round each placed perk machine and box location**, in the world tree, as a solid prop has: the 26-DOP of
  the model the struct names (the powered machine, the box), turned as the game turns it. A box location has it
  whether the box is there or not.
- **A player clip at each barrier:** a box over the bounds of the barrier's collision model, in the zbarrier's own
  axes, at least 8 units deep, with contents 0x50000. A hull is traced only when the trace's mask and the hull's
  contents share a bit (`CM_TreeHull_Trace` 0x7FF7295F5DC0). So players stop, and bullets and grenades pass. Zombies
  climb in as before: their climb is in noclip.
- **The navmesh goes round them.** These hulls are obstacles, not floors (`ConvexHull::walkable` off). Recast gets
  them after its low-obstacle filter and merges them with a threshold of 0. A box is 19 units high and a step 18,
  so either would turn a box's top into floor on some grids and not on others. A perk machine's and a box's navmesh
  seed moved 48 units to its front, where a player stands: its origin is inside its hull now.
- **The editor's barrier box is the clip** (`MkBarrier.mk_bounds`), so an opening can be made to fit it.

**Checks, offline** (zm_debug's export of 2026-10-03, built into a scratch folder):
- cwlink: `model hulls: 3 of 3 box locations, 6 of 6 perk machines, 1 of 1 power switch, 11 of 11 props`, and a
  `player clip` line for each barrier. The world tree has 771 hulls and contents 0x50001 (760 and 0x1 before).
- Each hull sits on its model. Juggernog's is 34 x 40 x 108 with its long side across the node's facing, a box's
  26 x 95 x 20. The wood barrier's clip covers the 71-wide opening of its wall.
- The navmesh has 422 polygons, 293 of them kept (385 and 259 before), with 33 of 46 seeds on it as before. No
  polygon lies under or on top of a machine or a box, one lies in front of each, and none crosses the wood barrier's
  clip.
- The zone walks completely: 14,806 assets.
- With `--no-object-clip` the zone is byte-identical to the one that ran on 2026-10-05, so nothing else changed.

**Test:** press Build on zm_debug and play it. Look for:
- every perk machine stops you before the power is on, and after;
- the Mystery Box stops you where it is, and so do the two locations it is not at;
- the wood barrier stops you, you can shoot through it, zombies still tear it down and climb in, and you can
  rebuild it;
- zombies walk round the machines and the box, and reach you when you stand on the box;
- the build output has the `model hulls` and `player clip` lines above.

**Limits:**
- A box location blocks while the box is away, where only the bear shows. In a retail map the bear sits on a piece
  of the level. Put a base of your own there (a brush or a mesh).
- A barrier's clip has the barrier's size. An opening wider or higher than the editor's box leaves a gap.

## Future: a map with no base map (planned 2026-09-27)

*Updated 2026-09-30.* Since stage 1 (above), a mapkit map loads under its own name, from its own zone. Die
Maschine's zones still load first, as its asset library, and the map's world takes the place of DM's under DM's
names. That is why a model shows in the game only when a loaded zone holds it (zm_silver, zm_common or core): the
engine finds a model by hash among the loaded assets, and an entity whose model is not loaded spawns with nothing
drawn, and no error.

Leaving the base map behind takes two parts:
1. **Own the level design** (P1 to P5): level script, entity and trigger lists, zones, placement, meshes,
   lighting, navmesh. **Done 2026-09-29:** DM's zone is now used only as an asset library.
2. **Own the asset set** (P6): the map's zone holds everything it uses that zm_common and core don't, copied at
   build time from the creator's own install. The map then loads with only zm_common, core and its own zone
   (stage 1 already gives it its own name), and its world gets its own names. No other map's zone loads.

**Copying an asset into our zone.** A model from any zone (another map's included) can become ours:
- **Geometry: possible today.** `ModelLibrary` decodes any model, streamed ones included (it reads the
  packages), and `model_writer` writes a resident xmodel, its mesh, its own skeleton and a bgcache entry. That
  is how mapkit's walls are drawn already.
- **Materials: the blocker.** A copied model keeps its material names. A material from a loaded zone just
  works. One from another zone must be copied too, with its images (the texture reader above, plus an image
  writer) and its techset. A techset is a set of compiled shaders; if the loaded techsets lack the technique the
  material needs, it cannot draw. Many materials share techsets, so this needs a count first.
- **Collision:** mapkit links an empty xcollision, so a copied model is not solid until there is an xcollision
  writer (or brush hulls around it).
- **Animated models** (`fxanim`, skinned): they need their real skeleton and their animations copied, not the
  one-bone static skeleton.

**The rule that keeps it legal:** zones that contain copied game data are built on each player's machine from
their own install, and never shared. A shared map is its `.mkmap` source (names, hashes and placements) plus
our own content; each player's `cwlink build` copies the game assets from their own game. Nobody downloads
Activision data from us.

First measurement for P6: list the assets a mapkit map actually uses from zm_silver itself (not zm_common or
core). About 3,250 of zm_silver's 14,123 xmodels are its own; the rest are references to other zones. The AI
types, weapons, FX, sounds and techset_zm_silver are the parts to count.

## Milestones

The milestones (P0 to P6), their pass tests and their status are kept in one place,
[ROADMAP.md](ROADMAP.md#5-custom-maps-mapkit). The sections above hold each step's findings and results.

P1 came first because it ended the fights with DM's design and did not depend on rendering. P2 is what
"Godot maps out models and positions" means.
