# cw-mod — Call of Duty: Black Ops Cold War (T9) Community Revival

> **Vision:** keep T9 Zombies alive. A community-owned, open-source mod that lets people
> load custom Zombies content, play together , and
> preserve this version of Zombies for the long run.

**Community-owned.** Nobody owns this but the people who play it. Contributions welcome.

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

Target build **1.34.0.15931218**. Working today:
- the Arxan bypass, an in-game overlay, and a GSC/CSC script loader;
- Zombies progression (XP, weapon XP, saves) offline and in LAN mode;
- two PCs in one match, in LAN mode or in online mode against a local Demonware backend that each
  player runs on their own PC;
- custom Zombies maps, built in Godot with mapkit and started from a CUSTOM MAPS tab.

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
