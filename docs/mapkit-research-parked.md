# Parked research: P2, P4, P5 and level effects (2026-09-27)

Four research threads were started in parallel on 2026-09-27 and stopped early (the session hit its usage limit).
This file keeps what each had found, so the work can resume where it stopped. The milestones they belong to
are in [mapkit-plan.md](mapkit-plan.md), and the map inventory in [mapkit-map-anatomy.md](mapkit-map-anatomy.md).

**Where the work is:** `<game>\cw-mod\mapkit\research\<topic>\`, on this PC only. It holds:
- each thread's Python scripts;
- `digest.txt` (the IDA renames and comments it made);
- `digest_full.txt` (every command it ran with its output);
- small outputs.

They contain bytes decoded from the game, so they stay out of the repo. The large stream dumps they read were
left out; `ffinfo <zone> --stream <file>` writes them again (`zm_silver`, `core_common`, `core_frontend`,
`zm_gold`).

The IDA renames and comments of the lighting and navmesh threads were saved to the IDB by the threads
themselves (`idb_save` succeeded). The other two renamed nothing.

Facts are marked **verified** (proved on data or in code) or **guess**.

## P2 placement: where static models are placed

**Verified: the placements are in a districts stream key, not in the gfx_map model groups.**
- zm_silver asset 169131 is a `streamkey` whose payload is stored in the zone (flags 0x3). It is at stream
  +0x9D42A28 and holds 796,680 bytes.
- The payload holds 3,318 entries of 64 bytes, starting at +0x9D42BA0: +0 the xmodel name hash, +8 a u32 (unique
  per entry), then fields that are mostly -1 or 0.
- After them come 3,318 records of 64 bytes, starting at +0x9D76920:

  | Offset | Field |
  |---|---|
  | +0 | origin (vec3, inches) |
  | +12 | scale |
  | +16 | world mins (vec3) |
  | +28 | world maxs (vec3) |
  | +40 | id (== the record's index, all 3,318) |
  | +44 | radius |
  | +48 | rotation quaternion (x, y, z, w; unit length) |

- 230 records match exactly: their stored bounds equal the model's bounds turned by the quaternion, scaled and
  moved to the origin. The others are large or odd models (terrain vistas, `*..._16` merged brush models).
- They hold 167 distinct models (for example `p9_zm_ndu_dark_aether_bio_02`, crystals, hanging lights), with
  origins from x -39,086 to 25,723.
- Scripts: `p2_model_groups\district.py`, `t11.py`, `t12.py`, `t14.py`.

**What this means for P2 (guess, next step):** a mapkit prop becomes an entry plus a record in a districts
stream key that mapkit writes, instead of a 992-byte model group. mapkit writes an empty districts asset today.

**Still open:**
- how the districts asset (0xAB, `Load_Districts` 0x7FF71E7D4440) points at this stream key, and in which
  district slot (the 10 stream keys per district; slot 9 is the collision cell);
- whether a payload stored in the zone (flags & 2, "inline") is enough, or the streamer wants a package key;
- which runtime function reads the records;
- the entry's other fields: +12 is 0x80000000..FFFFFFFF, +20 is 16 bits wide, +32 and +40 are small counts.

**The 992-byte model groups are something else (verified on data):**
- they are posed multi-bone objects (barbed wire, hanging foliage);
- tail +0 holds 32-byte bone records {quaternion, translation, w = 2/|q|²} (2,191 bones checked);
- tail +8 holds 64-byte 3×3-plus records;
- `Load_ClipMapStruct992` 0x7FF71E7D3840 loads them;
- readers found by the u8 @986 model count: 0x7FF7288E7FB0 (model by index), 0x7FF72904D560, 0x7FF72904FA50.

The streamerworld's 52-byte records are not placements (0 of 2,237 have unit axes).

## P5 navmesh: Havok AI tagfiles (verified)

**Format and location:**
- The `navmesh` asset (0x75) is a 104-byte root in the zone. Its data is **Havok 2019.2 (`SDKV 20190200`) TAG0
  tagfiles**, fetched through stream keys from the packages.
- Each payload: +0x100 cache slot, +0x108 u32 tagfile size, a u32 checksum, then the TAG0 tagfile at +0x120.
- Units are meters with Z up. The engine converts inches ×0.0254 in and ×39.370079 out.

**NavMeshData root (`Load_NavMeshData` 0x7FF71E7E1EE0):**

| Offset | Field |
|---|---|
| +0 | name hash |
| +8 | StreamKey*: the shared tagfile (`Runtime_NavMeshAssetShared`) |
| +32 | i32 cell count |
| +40 | cells, 64 bytes each: +0 u32 section uid, +8..+36 cell AABB (inches), +36 u32 count, +40 StreamKey* `Runtime_NavMeshCell` |
| +48 | bounds |
| +72 | StreamKey*: debug data |

**NavVolumeData (`Load_NavVolumeData` 0x7FF71E7E2070):** 24 bytes. Only the layer named `default` is used. Its
`Runtime_NavVolumeEntry` tagfile holds two `hkaiNavVolume` layers (small and big flyers). A missing navvolume is
not an error.

**The loading path:**
- `DB_LinkNavmeshAsset` 0x7FF728A3E1F0 queues the keys.
- `AI_InitNavWorld_cand` 0x7FF723BAD970 (called from `G_InitGame`) loads everything.
- A missing navmesh is a drop: `Com_ErrorDrop` 0x44853591 in 0x7FF723B9D5E0.
- `AI_AddNavMeshCellsToWorld_cand` 0x7FF723B9D100 makes an `hkaiNavMeshInstance` and an `hkaiDirectedGraphInstance`
  per cell, then adds the shared user-edge pairs (mantles and traversals).
- Globals: `g_aiNav_cand` 0x7FF72D791D50 and `g_navFaceRegions_cand`. Face regions are flood-filled at runtime,
  not stored.

**Die Maschine's main cell (decoded):**
- 3,919 faces, 17,394 edges, 8,051 vertices; a cluster graph with 395 nodes;
- a `TacticalGraphCell` with 3,429 tactical points that link to path nodes (257 distinct);
- also mantle edges and traversal settings;
- exported as geometry to `p5_navmesh\dm_navmesh_local.obj` (game data: keep local).
- Scripts: `hktag.py` (tagfile reader), `navcell.py`, `verify_mesh.py`, `ptch.py`.

**Script side:** `getclosestpointonnavmesh` 0x7FF720866320, `ispointonnavmesh` 0x7FF72086B4C0.

**What it means:** mapkit's own navmesh means writing a Havok 2019.2 `hkaiNavMesh` tagfile (faces, edges,
vertices, cluster graph, tactical graph) and the navmesh asset that points at it.

**Next steps:**
1. Rebuild Die Maschine's tagfile byte for byte from `hktag.py`'s parse (proves the writer).
2. Write a mesh from the level's brushes: the floor tops as faces.
3. Check the checksum at +0x10C (it is not CRC32 or Adler32 of the tagfile; tested).

**Picked up 2026-09-29 (mapkit-plan.md, "P5 step 1"):**
1. Done in C++ (zonekit `havok_tagfile`): all five of Die Maschine's navmesh and navvolume tagfiles write back byte
   for byte.
2. Done with Recast over the collision hulls (zonekit `GenerateNavPolygons`).
3. Not needed: the engine never reads the checksum.

## P4 lighting: the asset is decoded (verified)

- `p4_lighting\lighting_walk.py` walks the lighting asset (0xAA, 472-byte root) and ends exactly on the next
  asset's start, with every block position matching, on zm_silver, core_frontend and zm_gold.
- The field table is in the IDA comment at 0x7FF71E7DDEA5.

**Main parts of the lighting asset:**

| Where | What |
|---|---|
| +8/+16 | lights (688 bytes each; the cookie image at +672) |
| +56/+64 | lighting volumes (9,888 bytes each, `Load_LightingVolumeArray` 0x7FF71E7DA940) and their planes |
| +440 | wind volumes |
| +360 | image sets per state |

**One lighting volume** holds 4 **lighting states**, 2,084 bytes each from +76:
- sun direction (yaw and pitch), exposure range and fog;
- sun shadow and sun cookie per state;
- reflection probes and GI images per state;
- **the sky per state** at +9440 (40 bytes each): skybox xmodel, sky image, klf.

**Die Maschine's four states:**

| State | Sky | Sun | Exposure |
|---|---|---|---|
| 0 day | `skybox_zm_silver_override` | yaw 120, pitch 25 | 9.1..14 |
| 1 Dark Aether | `..._dark_override` | pitch 68.3 | 8.8..12 |
| 2 lightning | `..._dark_override_lightning` | — | 6..7 |
| 3 | `skybox_default_black` | — | — |

Each state also has its own fog values.

**Script-level switches (renamed in IDA), with no asset writing needed:**
- `setlightingstate(n)`, n < 4 (0x7FF72073DB50, and the player method 0x7FF721960F80);
- CSC `setpbgactivebank` 0x7FF7243EAAF0, `setexposureactivebank` 0x7FF7243EA9D0, `setworldfogactivebank`
  0x7FF7243EAE80;
- `setvolfog` 0x7FF7208B8F00, `setexpfog` 0x7FF7208B8B80, `visionsetnaked` 0x7FF7208B9570.

So a mapkit map should be able to pick any of Die Maschine's four looks (day, Dark Aether, and so on) from its
level script with `setlightingstate`, and set its own fog with `setexpfog`/`setvolfog`. This is not tested: the
state applies inside Die Maschine's lighting volumes, which a map at Die Maschine's start area is in.

**Still open:**
- what the klf asset (0x35, `Load_Klf` 0x7FF71E7D75A0) does.

**Picked up 2026-09-29 (mapkit-plan.md, "P4 sky and lighting, step 2"):**
- The walker is ported to zonekit (`LoadLightingAsset`, `CopyLighting`).
- The state's fields (sun, fog, exposure) are named in `world_assets.hpp`.
- `setlightingstate` remains the way to switch states (not tried in the game yet). `setexpfog` and `setvolfog` change
  nothing: the frame fog reads the lighting state and the gfx_map's override volumes, never configstring 10.

## Level effects, the minimap, and script bundles

**Static level effects (verified on data):**
- zm_silver's `staticlevelfxlist` (asset 169163, stream +0xA115007) holds 2,482 placed effects of 139 kinds.
- Its loader: `Load_StaticlevelfxlistAsset` 0x7FF71E7EBF40 → 0x7FF71E7EBE60 (a 40-byte root) → 0x7FF71E7EBC70.
- Each entry is 80 bytes:

  | Offset | Field |
  |---|---|
  | +0 | the fx (a link) |
  | +8 | origin |
  | +20 | angles |
  | +56 | count |
  | +64 | a name string |
  | +72 | an array of 32-byte sub-entries |

- mapkit leaves the list alone, so **Die Maschine's ambient effects most likely still play at Die Maschine's
  positions in a custom map** (not checked in the game).
- Next: an empty list of the same name in the override zone turns them off. mapkit's own list comes later;
  until then, the level script can place effects with `playfx`.

**`fxlibraryvolume` (verified shape, meaning a guess):**
- zm_silver's (asset 182001) is named `zm_silver`, holds 2.1 MB and starts with eight counted arrays: 670 fx
  links, 346, 681, 670, 6,115, 991, 613 and 394 entries.
- It also holds 681 materials.
- It looks like the list of effects and materials the level preloads.

**Minimap (verified):**
- The map's `maptableentry` is in **core_common**, not zm_silver: asset `zm_silver`, root 456 bytes at stream
  +0xD091869 of core_common.
- It holds two 64-byte minimap records (image links, then UV floats 0,0 → 1,1), the loading movie name
  (`zm_silver_loadingmovie`), and a settings tree with 45 entries.
- The minimap images are image links (#9D0A4042AFDF9D83, #ECB77D80BA53518D).
- A custom minimap therefore needs an image asset of the same name in the override zone, which needs the image
  writer (P4).

**Script bundles (not reached):**
- The loader was located, `Load_ScriptbundleAsset` 0x7FF71E7E82D0, with its settings tree in
  `Load_SettingsTree` 0x7FF71E7E7E80.
- The settings tree's shape is known (32 bytes: entry count, entries of 32 bytes each, group count, groups; entry
  +20 is a script string) and is written down in `level_fx_minimap_bundles\settings.py`.
- The value encoding and a writer were not reached. The exfil waves and crafting blueprints still need this.
