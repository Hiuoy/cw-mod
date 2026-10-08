# cw-mod roadmap

cw-mod keeps Black Ops Cold War Zombies playable on a copy of the game you own (build **1.34.0.15931218**).
This file is the one place for the project's status: what works, what comes next, and what is parked. The
documents it links to hold the technical detail.

Last updated 2026-10-05. When a milestone passes in the game, update its row here in the same commit as its
result.

**Status words**
- **Done**: seen working in the game, on the date given. Code that builds, or an offline test that passes, is not
  enough.
- **Part done**: some of it works; the row says what is left.
- **Built**: written, not yet seen working in the game.
- **Open**: known work, not started.
- **Parked**: set aside on purpose; the linked notes say where it stopped.
- **Not planned**: out of scope for now.

## At a glance

| Track | Where it stands | Next |
|---|---|---|
| [1. The client](#1-the-client) | Done: the DLL boots the game with an overlay, a script loader and a settings file | Small open items |
| [2. Playing together](#2-playing-together) | Done: two PCs in one match, in LAN mode or in online mode | Two PCs on different networks |
| [3. Local Demonware backend](#3-local-demonware-backend) | Done up to a full online match: login, party, playlists, match launch | The written service map |
| [4. Zombies progression](#4-zombies-progression) | Done offline and in LAN mode. Online mode: level and weapon levels passed 2026-10-07; the match-end save of the progression file was refused, fix built the same day. Unlock-all switch built 2026-10-07. Neither run in the game yet | One online match with the new build |
| [5. Custom maps (mapkit)](#5-custom-maps-mapkit) | Done through P5 and stage 1: a map's own gameplay, meshes, textures, sky, lighting, navmesh and name | **P6: the map's own asset set** (steps 1 to 2b-2 passed: a map loads with only `techset_zm_silver` of Die Maschine's zones), and **the map's own lighting** (step 1 passed 2026-10-05: none of Die Maschine's lamps or baked shadows) |

## Next up

1. **The map's own lighting, the next steps.** Step 1 passed on 2026-10-05: a map no longer shows Die Maschine's
   lamps or its baked sun shadow
   ([mapkit-plan.md](mapkit-plan.md#the-maps-own-lighting-step-1-none-of-die-maschines-lamps-or-baked-shadows-built-2026-10-05-passed-in-the-game-the-same-day)).
   Still to do, in order: the creator's own lights (MkLight), a baked shadow of the map's own geometry, and probes
   and ambient light that are not Die Maschine's.
2. **P6 step 2b-3:** copy the sky's techset so `techset_zm_silver` can go too; give the world the map's own names.
3. **The debug map's test list** (zm_debug loads and runs; its checks have not been gone through).

Everything else below is open or parked, and not scheduled.

## 1. The client

One DLL that the game loads as `discord_game_sdk.dll`. How it is put together: [ARCHITECTURE.md](ARCHITECTURE.md).

| Part | Status |
|---|---|
| Arxan bypass and a stable boot | Done |
| Overlay: Home, Maps, Server Browser, Session, Scripts, Demonware, LUI Menus and Debug tabs | Done |
| Crash logger and `cw-mod/client.log` | Done |
| Settings in `cw-mod/cw-mod.json` (replaced the old marker files) | Done 2026-09-24 |
| GSC/CSC loader: add or replace scripts from `cw-mod/scripts`, and a map's own scripts from its folder | Done 2026-09-23; client scripts and map folders 2026-09-26 |
| Trainer script (`gsc/cwmod_trainer.gsc`: Pack-a-Punch, crystals) | Done 2026-09-23 |
| Replacing the game's UI text (`"ui_text"` in `cw-mod.json`) | Done 2026-09-25 |
| Fix for the engine's crash on any Lua error | Done |
| Dev tools script (`gsc/cwmod_devtools.gsc`: god mode, noclip, wall check) | Built 2026-09-26 |

Open: the lobby's error dialog shows the engine's code name (such as "Zed 453 Kinetic Devil") instead of a
readable message. `Com_NotifyLuiError` passes that text to the menu, so a detour there would fix it.

## 2. Playing together

In both modes the players' PCs connect to each other directly, and the game's Demonware traffic goes to each
player's own local backend, never to Activision's servers.

| Mode | Status |
|---|---|
| **LAN mode**: host a match and join it from a second PC | Done 2026-07-23 |
| **Online mode**, over the local backend ([section 3](#3-local-demonware-backend)): the game's online menus and party. Each PC runs its own backend and needs its own `"xuid"` in `cw-mod.json` (written on the first boot; don't copy `cw-mod.json` to another PC) | Done 2026-09-29: a host and a second PC played a custom map together, on one home network |
| **Server Browser** tab: finds hosts on the network by a UDP beacon (port 28970) | Done 2026-09-29: each PC saw the other |
| **Server Browser as a game menu**: a SERVER BROWSER button on the Zombies main screen (online mode), below SOLO, opening the same list with details and Join (`client/game/ui_scripts_server_browser.hpp`) | Built 2026-10-07. The first run (20:48) ended the process on the Zombies main screen: the button's name was plain text, which the game's localize call takes for a missing entry. Fixed build 21:08, not run yet |

Open: two PCs on **different** networks, through a VPN such as Radmin. The tries on 2026-09-24 failed on a shared
identity and on the peer address. Both were fixed that night, but every online test since has run on one network.

Not planned: public matchmaking and dedicated servers. A server that many players' games share (the README's
"hosted lobbies") has no design yet.

## 3. Local Demonware backend

Demonware is Activision's online backend: login, playlists, player data, the store. `tools/dwserver` is a
stand-in that each player runs on their own PC (`python tools/dwserver/run.py`), and the client sends the game's
Demonware traffic to it. Milestone detail: [backend-roadmap.md](backend-roadmap.md). The protocol:
[re/demonware-login.md](re/demonware-login.md).

| Milestone | Status | What it gives |
|---|---|---|
| Login chain: redirect, TLS, auth, umbrella, LSG, lobby reply | Done 2026-08-02 | `[status 27] Login Complete` against our own server |
| B0: baseline | Done 2026-09-16 | An online boot with the backend reaches Login Complete again |
| B1: stable after login | Done 2026-09-16 | No sign-out after login; the first post-login requests arrive |
| B2: service map | Part done | Replies the client accepts (StructData, keepalive echo): done 2026-09-16. A census of what the client asks, and the router's service and task labels corrected: 2026-09-24 ([codrevamped-notes.md](codrevamped-notes.md)). Left: the written map |
| B3: publisher data | Done 2026-09-24 | Playlists from the player's own local LPC files: modes and maps show on online boots |
| B4: player storage | Done, locally | Saves go to local storage through a client-side redirect. A server-side copy (bdStorage) is not planned |
| B5: entitlements and inventory | Part done | Padlocks and the "you don't own the game" upsell are fixed on the client (2026-09-24). The store and the marketplace inventory stay empty: they need a Battle.net token |
| B6: online party | Done 2026-09-24 | The online menus open with a party, and the mode tiles unlock |
| B7: starting a Zombies match | Done 2026-09-24 | Start opens a self-hosted game lobby instead of dedicated-server matchmaking, and the countdown runs |
| B8: the match launch | Done 2026-09-24 | The match loads, plays and returns to the menu. Two PCs in one online match: 2026-09-29 |

Open:
- B2: write the service map (`docs/re/dw-service-map.md`), and confirm on T9 the service names taken from the IW8
  and T8 tables (marked `?` in `lobby_router.py`).
- Online-mode progression ([section 4](#4-zombies-progression)).

Not planned: the in-game store and the marketplace (B5). Without them the battle pass stays at tier 0 and nothing in
the store is owned; what each one reads is in [backend-roadmap.md](backend-roadmap.md) under B5. The client-side
`unlock_all` switch ([section 4](#4-zombies-progression)) answers the ownership questions instead; it does not
bring the store or the marketplace back.

## 4. Zombies progression

The client opens the game's own XP and stats gates and keeps the save on the PC
(`client/game/zm_progression.cpp`; on by default, `"progression": false` in `cw-mod.json` turns it off).
Passed offline and in LAN mode. On an online boot the same code runs since 2026-10-07 (see "Open").

| Part | Status |
|---|---|
| XP, rank and saving | Done 2026-09-23 (LAN). Online: see "Open" |
| Weapon XP: weapon levels and attachment unlocks | Done 2026-09-23 (LAN). Online: passed on the log 2026-10-07 |
| Level and weapon levels kept across restarts on an online boot | Passed 2026-10-07 (one match, then a restart) |
| Unlock-all switch (`"unlock_all": true` in `cw-mod.json`, off by default) | Built 2026-10-07, not run in the game yet |
| After-action report in LAN mode | Done 2026-09-23 |
| After-action report on an online boot | Passed on the log 2026-10-06 (one `zm_debug` match): the report opened with no Lua error and the lobby's menus came back. The player has not yet confirmed the button prompts by eye. Before that, after four matches (2026-09-29 to 2026-10-03), it opened without its button prompts, and Esc left a frontend with no menus: the season publisher variable (`loot_season_stream`) was empty, which is what broke it in LAN mode before 2026-09-23. The client now sets it before every UI load, on every boot profile. The same empty variable dropped every challenge notification in a match (not seen again, but that match was two minutes long) |

Open:
- **Online mode: the match-end save of the progression file. Fix built 2026-10-07, not run in the game yet.**
  Until 2026-10-07 the feature stood aside on an online boot, and the player's level was back at 1 after every
  restart. Read from the engine's code:
  - The level and the weapon levels are kept in one block that only Demonware's achievement service fills and
    stores. Signing in to the local backend creates it empty, and the backend has no such service.
  - In an online match the server sends its XP and weapon-level changes straight into that block, so they show
    for the session and are gone at exit.

  What the client does on an online boot since then: it fills that block from the same local file LAN mode uses
  (`player/cwmod_ae_sync_0.bin`) and the saved XP, and saves it back when a match ends.

  The first run (2026-10-07 17:29, one match, then a restart), from `client.log`:
  - Passed: the block was filled at sign-in (level 154), the match's weapon XP reached the file, and the restart
    loaded it again. The player confirmed the level stays.
  - Measured: an online private Zombies match passes the game's own two stats gates (match type 0), so nothing
    is forced there. The fork findings said otherwise ([fork-findings.md](fork-findings.md) §4).
  - Failed, and not visible in the menus: the match-end copy into the two progression files (`zm_progression`,
    `common_progression`) was refused, so nothing in them changed (XP below the level cap, crystals). The game
    only copies when the save carries the player's id. This save was made under an older id (before `"xuid"`
    existed in `cw-mod.json`), and the client's write of the new id was refused: the game makes the progression
    files read-only once they have loaded. The write had worked on 2026-09-23, on a boot with no save file yet;
    by the same code a file that did not load stays writable, and a LAN boot with this save would fail the
    same way (both read from the code, not measured).

  The fix: the client opens the file for that one write the way the game does for its own (`STAMP FAILED` in
  the log was this). To check on the next run: `player_xuid ... (stamped; ...)` at match start, and
  `storage WRITE ... map=19` and `map=5` after the match.
- **The unlock-all switch, built 2026-10-07, not run in the game yet** (`client/game/unlock_all.cpp`). With
  `"unlock_all": true` the client answers the game's lock and ownership questions: weapons and attachments
  behind a level or a weapon level, camos and reticles behind their challenges, everything behind the store
  inventory (blueprints, bundles, operators, battle pass rewards), and the battle pass itself (owned, tier 100).
  It changes answers only: nothing is written to a save, and `false` brings every lock back.
  Not covered: seven Zombies rewards the menus read straight from the save. To check on the first run:
  `unlock_all detours armed (11/11 live)` in `client.log`, then the `(UnlockAll)` lines (at most one `so far`
  line a minute with what was asked and what was changed), and by eye: a locked weapon, a camo, a blueprint,
  the battle pass.
- The after-action report after a custom-map match. On an online boot it opens: the probe
  (`cw-mod/ui_scripts/aar_gates.lua` in the game folder) logged every gate open after four `zm_debug` matches,
  2026-09-29 to 2026-10-03. Not checked in LAN mode since the probe went in.

## 5. Custom maps (mapkit)

A creator lays out a Zombies map in Godot with the mapkit plugin and presses Build. `cwlink` writes the map's zone
and level scripts to `cw-mod/maps/<id>`, and the CUSTOM MAPS tab in Zombies Private starts it. Game assets are used
by name from each player's own install. Using the tools: [mapkit/README.md](../mapkit/README.md). Each step's
findings and test results: [mapkit-plan.md](mapkit-plan.md).

### Parts

| Part | What it does |
|---|---|
| `mapkit/godot` | The level builder: Zombies objects, meshes, lights, sky, checks, a walk-through, and the Build button |
| `mapkit/cwlink` | Builds a map from its `.mkmap` source |
| `mapkit/zonekit` | Reads and writes the game's zone files and the assets a map needs |
| `mapkit/ffinfo` | Inspects zones and assets from the command line |
| `mapkit/mkasset` | Reads the game's models into a local cache, for the builder's model picker |
| The client | Loads maps from `cw-mod/maps`, adds the CUSTOM MAPS tab, and serves each map's own scripts |

### Milestones

| | Milestone | Status | Passed when |
|---|---|---|---|
| M1–M5 | Groundwork: the zone format, asset loaders and writers, the client's map loader, the first `cwlink build`, the Godot builder | Done 2026-09-25 | A map made in Godot loads, and a player walks in it |
| P0 | Our models draw | Done 2026-09-26 | Every model the map writes draws, and stays drawn as the camera moves |
| P1 | The map's own gameplay: level scripts, entities, zones, spawns, doors, the box, Pack-a-Punch, perks, ammo cache, Armor Station, Wunderfizz, crafting table, power switch, exfil | Done 2026-09-27; power switch and exfil 2026-09-29 | A match that holds only our objects, all working |
| P2 | Game models placed as props | Step 1 done 2026-09-28; step 2 parked | Props drawn and solid where Godot puts them |
| P3 | The creator's own meshes (`.glb`) | Done 2026-09-28 | A room made in Blender, drawn and solid |
| P4 | The look: own textures, normal and roughness maps, transparency, metal, tint, sky, sun and fog | Done 2026-09-29; glow parked, minimap open | A textured room under its own sky |
| P5 | The map's own navmesh | Step 1 done 2026-09-29; the rest parked | Zombies chase the player through a layout outside Die Maschine's |
| Stage 1 | A map of its own name: its own zone (`<id>.ff`) and level scripts, and the lobby sends its name to every player | Done 2026-09-29, on two PCs | A host and a second PC play the map with no kick |
| **P6** | **The map's own asset set (stage 2)** | **Step 1 done 2026-09-30:** 7,377 assets / 44.8 MB with Die Maschine loaded; the real need is about 17.5 MB. **Step 2a passed 2026-09-30** (the level's world assets as the map's own): test 1 crashed on a missed lighting link, fixed; test 2 passed. **Step 2b-1 passed 2026-10-01** (what those copies link, copied too: 2,355 assets, 7.33 MB, each checked against its original offline): test 1 crashed on the lighting's streamed-texture keys, whose owner the override swap lost; they stay Die Maschine's until 2b-2; test 2 passed, and the census shows every copy in use. **Step 2b-2 passed 2026-10-03** (only `techset_zm_silver` of Die Maschine's zones loads; 2,492 copies; its own package list; zm_common's spawners): no crash, zombies and the power switch right, the clientfields match | A map that loads no other map's zone, with a model from another map in it |

### P6: the map's own asset set

A map of its own still loads Die Maschine's zones first, as its asset library: AI types, zombie models, weapons,
FX, sounds, materials and `techset_zm_silver`. P6 ends that.

1. **Count** the assets a map actually uses from Die Maschine's own zone, not zm_common or core. Done 2026-09-30
   (zm_navtest census match):
   - 7,377 assets and 44.8 MB of zm_silver.ff while Die Maschine is loaded;
   - of that, 500 xanims (23.7 MB), 16 items and 183 script bundles are listed only by Die Maschine's bgcache and
     were requested by the level load;
   - the renderer also streams Die Maschine's replaced world in the background (6,720 assets), which the count
     leaves out;
   - the map's real need is its world part, about 17.5 MB.
2. **Copy** what is still needed into the map's zone when it is built:
   - models: possible today (`ModelLibrary` and `model_writer`);
   - materials, images and techsets: the likely blocker. A techset is a set of compiled shaders, and a material
     draws only with a technique that a loaded techset has;
   - collision: needs an xcollision writer, or hulls around the model;
   - animated models: need their real skeletons and animations.
   Step 2a (built 2026-09-30): the level's world assets. The map's zone now holds copies of Die Maschine's
   lighting, terrain gfx (without its tiles), glass, primary lights, occlusion data, path nodes and navvolume, and
   an empty placed-effects list, spliced by `zonekit/asset_record.hpp` from readers transcribed from the IDB
   (`zonekit/level_assets.hpp`). They still link 2,340 of Die Maschine's assets (7.3 MB), which step 2b copies.
3. **Load** the map with only zm_common, core and its own zone, and give its world its own names (step 2b). Step
   2b-2 (built 2026-10-02) leaves out every zone of Die Maschine's but `techset_zm_silver`, which still holds the sky's
   techset; the world keeps Die Maschine's names behind the client's alias.

The legal rule: a zone that holds copied game data is built on each player's PC from their own install, and is
never shared. A shared map is its `.mkmap` source plus the creator's own content.

### Open and parked

| Item | Status | Where it stopped |
|---|---|---|
| Concrete barriers: they say they need power, and turning the power on does not clear that | Parked | [mapkit-plan.md](mapkit-plan.md), "Leftovers" |
| Glowing surfaces: the panels draw, the glow does not show | Parked | [mapkit-plan.md](mapkit-plan.md), "Leftovers" |
| Maps with hundreds of props: the retail way to place static models (districts) | Parked | [mapkit-research-parked.md](mapkit-research-parked.md), "P2 placement" |
| Navmesh: moving doors and props, traversals (mantles, jumps), tactical points, more than one cell, settings in the builder | Parked | [mapkit-plan.md](mapkit-plan.md), "P5 step 1" |
| Minimap picture | Open | [mapkit-research-parked.md](mapkit-research-parked.md), "Level effects, the minimap, and script bundles" |
| The map's own picture in the Zombies menu, loading-screen video and loading-screen instructions image (all still Die Maschine's, from its maptable entry) | Open, part of P6 | [mapkit-plan.md](mapkit-plan.md), "P6 step 2a" |
| Intro cinematic | Open: the method is known | [mapkit-plan.md](mapkit-plan.md), "The intro cinematic" |
| Game models' textures in the builder (they show gray) | Open | [mapkit-plan.md](mapkit-plan.md), "Future: textures in the editor" |
| Everything else still borrowed from Die Maschine (terrain drawing, sound banks, ...) | Open, part of P6 | [mapkit-map-anatomy.md](mapkit-map-anatomy.md) |
| Ambient rooms (`MkAmbientRoom`): each indoor space's reverb, room tone and gunfire tails. Without them a map sounds like Die Maschine's default room everywhere | Passed 2026-10-03 on the second test: no echo outside the rooms, and each room sounds different. Parked: rooms with the map's own settings come later | [mapkit-plan.md](mapkit-plan.md), "Ambient rooms" |
| Die Maschine's sound bank copied into the map's zone (without `--with-library`): its rooms, ambience and level sounds | Works (2026-10-03): the rooms play from the copy | [mapkit-plan.md](mapkit-plan.md), "Ambient rooms" |
| Die Maschine's baked acoustics (Triton) change the reverb and occlusion from point to point, following its own walls, in every room that does not set `OverrideTriton` (seen 2026-10-03 in `zm_nacht_bunker_hallways`) | Parked. `--no-acoustics` leaves them out of the copy (not yet tried) | [mapkit-plan.md](mapkit-plan.md), "Ambient rooms" |
| The debug map (`maps/zm_debug.tscn`): one of everything the editor can place, labelled, to test them all in one build | Part done: the sixth build loaded and ran for 15 minutes on 2026-10-05 (the clientfields match). Its test list has not been gone through. The five builds before it stopped, crashed or dropped: a prop's physpreset, the sky's techset reference, block 1 too small for the lobby preload, a zbarrier only Die Maschine's zone holds, and that zbarrier's models missing from the bgcache. Each was fixed on 2026-10-03 | [mapkit-plan.md](mapkit-plan.md), "The debug map" |
| The map's own lighting, step 1: Die Maschine's 948 lamps lit a custom map where Die Maschine has them, and its baked sun shadow (made from its own buildings, for its own sun) drew in blocks beyond the reach of the frame's shadow maps (seen on zm_debug, 2026-10-05) | Done 2026-10-05: cwlink writes every copied light black (the lighting's list and the com_map's) and every baked shadow tree as the engine's own empty one. In the game: no crash, the stray lights gone, no blocky shadows, nearby shadows as before. `--keep-lights` and `--keep-baked-shadows` build the old way | [mapkit-plan.md](mapkit-plan.md), "The map's own lighting, step 1" |
| The rest of Die Maschine's lighting on a custom map: reflection probes, GI images and image sets baked from its geometry; no lights or baked shadow of the map's own | Open | [mapkit-plan.md](mapkit-plan.md), "The map's own lighting, step 1" (its last part) |
| Collision for perk machines, the Mystery Box and barriers: a perk machine was walk-through until it had power, and the box and the wood barrier always (seen on zm_debug, 2026-10-05). A retail map blocks at them with clip in its own world, which a map of its own has none of | Built 2026-10-05, not yet tested in the game: cwlink writes a hull round each placed machine and box location, and a player clip at each barrier (players stop; bullets and zombies pass). The navmesh goes round them. `--no-object-clip` builds the old way | [mapkit-plan.md](mapkit-plan.md), "Collision for perk machines, the Mystery Box and barriers" |
| From the first plan, not started: baked lighting, streamed textures for large maps (`.xpak`), placed sound emitters, reloading a map while the game runs | Open | [mapkit-roadmap.md](mapkit-roadmap.md), "Later" |
| Not yet checked in the game: zombies walking around props, and the collision of scaled props | Open | [mapkit-plan.md](mapkit-plan.md), "P2 step 1" |
| P0's method leftovers: field tables for the clip map, the gfx_map and the entity lists, and a diff of core_frontend's world against Die Maschine's | Open | [mapkit-plan.md](mapkit-plan.md), "How we reverse from now on" |

Maps built before 2026-09-29 use the old overlay form (`ww_1080_<id>.ff` and `ww_4k_<id>.ff`). They work only on
the PC that picks them: the lobby tells the other PCs "zm_silver", so those load stock Die Maschine and are kicked
with "Clientfield Mismatch". Rebuild such a map with Build, and delete its old zones on every PC.

## Where the details are

| Document | What it holds |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the client DLL is put together: boot, hooks, overlay |
| [backend-roadmap.md](backend-roadmap.md) | The backend milestones B0 to B8 in detail |
| [re/demonware-login.md](re/demonware-login.md) | How the login protocol was reversed |
| [fork-findings.md](fork-findings.md) | Findings imported from the two forks: padlocks, storage, XP |
| [codrevamped-notes.md](codrevamped-notes.md) | What CodRevamped's backend tells us about ours |
| [mapkit-plan.md](mapkit-plan.md) | mapkit's plan (P0 to P6), with each step's findings and test results |
| [mapkit-roadmap.md](mapkit-roadmap.md) | mapkit's groundwork (M1 to M5) and the zone formats it reversed |
| [mapkit-map-anatomy.md](mapkit-map-anatomy.md) | Everything Die Maschine's map holds, and which parts mapkit writes |
| [mapkit-research-parked.md](mapkit-research-parked.md) | Research set aside: placement, navmesh, lighting, effects, minimap |
| [mapkit/README.md](../mapkit/README.md) | Using the mapkit tools |
| [tools/dwserver/README.md](../tools/dwserver/README.md) | Running the local backend |

## Terms

- **Die Maschine** (`zm_silver`): the retail Zombies map that mapkit maps load as their asset library until P6.
- **Zone** (`.ff`, a fastfile): the game's asset container. A map is a set of zones.
- **GSC / CSC**: the game's server and client scripts.
- **Demonware**: Activision's online backend. **LSG**: its lobby server connection. **LPC**: the publisher's content
  files, such as playlists.
- **Navmesh**: the surfaces zombies walk and find their path on.

Every part of this plan assumes that the player owns the game. Nothing read from the game is committed or
shipped ([README](../README.md#legal)).
