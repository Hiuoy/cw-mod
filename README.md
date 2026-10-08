# cw-mod — Call of Duty: Black Ops Cold War (T9) Community Revival

---

> **AI disclaimer:** yes this is Ai slop.

## Docs

Full documentation: **[hiuoy.github.io/cw-docs](https://hiuoy.github.io/cw-docs/#/)**

## What this is

`cw-mod` is a client-side modification for a game **you legally own**. It injects into the
Black Ops Cold War process, loads a scripting layer (GSC/CSC), and enables custom Zombies
experiences offline and over LAN — with a longer-term goal of community-hosted online lobbies.

We do **not** distribute game files. We do not support piracy. You must own the game.

## Status

Target build **1.34.0.15931218**. "Working" below means seen working in the game, not just built.

### Working today

**Client**
- Arxan bypass and a stable boot, with a crash logger and `cw-mod/client.log`.
- In-game overlay with Home, Maps, Server Browser, Session, Scripts, Demonware, LUI Menus and Debug tabs.
- GSC/CSC script loader: add or replace the game's scripts from `cw-mod/scripts`.
- Trainer script (Pack-a-Punch, crystals).
- All settings in one file, `cw-mod/cw-mod.json`, including UI text replacement.
- Fix for the engine's crash on any Lua error.

**Playing together**
- LAN mode: host a match and join it from a second PC.
- Online mode: the game's online menus and party, against a local Demonware backend that each player
  runs on their own PC. Two PCs have played a custom map together on one home network.
- Server Browser tab: finds hosts on the network.

**Local Demonware backend** (`tools/dwserver`)
- Login, online party and mode tiles, playlists from local files, local player storage, and starting
  and playing a Zombies match.

**Zombies progression**
- XP, rank, weapon XP, weapon levels, attachment unlocks and saves, kept on your PC, offline and in LAN mode.
- Online mode: level and weapon levels stay across restarts.

**Custom maps (mapkit)**, built in Godot and started from the CUSTOM MAPS tab:
- Map gameplay: level scripts, zones, spawns, doors, Mystery Box, Pack-a-Punch, perks, ammo cache, Armor
  Station, Wunderfizz, crafting table, power switch and exfil.
- Your own meshes (`.glb`), game models as props, your own textures, sky, sun and fog.
- Your own navmesh, so zombies chase the player through your layout.
- A map of its own name and zone, played by a host and a second PC with no kick.
- A map that loads only `techset_zm_silver` of Die Maschine's zones, with none of its lamps or baked shadows.
- Server Browser as a game menu: a SERVER BROWSER button on the Zombies main screen.
- Online mode: the match-end save of the progression files.
- Unlock-all switch (`"unlock_all": true` in `cw-mod.json`).


What's done, what's next and what's parked: **[docs/ROADMAP.md](docs/ROADMAP.md)**. How the client is
put together: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
powershell -ExecutionPolicy Bypass -File scripts\launch.ps1 -GamePath "<your BOCW folder>"
```

## What's next

Custom maps that load no other map's zone (mapkit P6). The full list, with what's open and parked, is
in [docs/ROADMAP.md](docs/ROADMAP.md#next-up).

## Prior art we build on (credit where due)

- **[shiversoftdev/t9-src](https://github.com/shiversoftdev/t9-src)** — decompiled T9 GSC/CSC scripts. Our reference for game internals.
- **[ProjectDonetsk/T9](https://github.com/ProjectDonetsk/T9)** — "Cold War Reimagined" C++ mod platform (archived).
- **[TheBlaster041/t9-mod](https://github.com/TheBlaster041/t9-mod)** — public t6-mod→Defcon→t9-mod lineage base.
- **[ProjectDonetsk/IW8-Master-Server](https://github.com/ProjectDonetsk/IW8-Master-Server)** — blueprint for online revival.
- **[Plutonium](https://plutonium.pw/)** — the gold-standard model (client + master server + dedicated servers) for a dead-server revival.

## Getting started

See [CONTRIBUTING.md](CONTRIBUTING.md). Build toolchain: Premake5 + Visual Studio 2022 (matching the t9-mod lineage).

## Legal

For preservation and modding of a game you own. No proprietary game assets are hosted here.
Licensed MIT — see [LICENSE](LICENSE).
