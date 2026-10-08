# Contributing to cw-mod

This project is **community-owned**. Whether you write C++, script in GSC, run servers, make
art, or just test and give feedback — there's a place for you.

## Ground rules

1. **You must own the game.** No piracy, no distributing game files or copyrighted assets.
2. **Everything open source** and client-side on your own copy.
3. Be decent to each other. This is a community, not a leaderboard.

## Where help is needed (by skill)

- **C/C++ & reverse engineering** — the injector, engine hooks, netcode. The scarce, critical skill.
- **GSC/CSC scripting** — custom Zombies content: rounds, guns, modes, maps.
- **Maps** — building Zombies maps in Godot and Blender with mapkit, and testing them.
- **Backend/web** — the future master server, launcher, and site.
- **Testing & docs** — reproduce builds, file issues, keep `docs/` accurate.

## Source layout

Premake globs `client/**` and `common/**`, so adding a file needs no project edit — but you **must**
re-run `scripts\bootstrap.ps1` afterwards, or you'll get `LNK2019` for the new symbols. One concern per translation unit; the header comment at the top of each file says which.

```
client/
  arxan/          Arxan defeat. arxan_utility.cpp = public entrypoints, arxan_stubs.cpp = the two
                  JIT stub builders, arxan_internal.* = the checksum bookkeeping both share.
                  sys_hooks/ = one file per hooked syscall.
  engine/t9/      Plain engine type definitions. No logic.
  game/           Everything that reaches into the game. `Pointers` is declared once in game.hpp and
                  implemented across these, one domain each:
                    game.cpp        ctor (signature scan), PatchAuth/SetMode, com_maxclients
                    anchors.cpp     every build-locked RVA -> live pointer (incl. the Lua C API)
                    dump_anchors.hpp  the dump-absolute address table those anchors resolve from
                    dvars.cpp       the name hash, the dvar registry, runtime name recovery
                    lui_menu.cpp    opening a LUI menu by name or hash
                    lua_state.cpp   the read-only walker over the live Lua 5.1 state
                    coop.cpp        lever-1, the loopback 2nd-player seat
                    session.cpp     lever-3, the LAN net-session inspector
                    join.cpp        lever-4, the client join + host descriptor
                    netmsg.cpp      lever-5, the netmsg transcript detours
                    join_log.*      the transcript buffer join.cpp and netmsg.cpp share
                    game_internal.* SafeRead/SafeCopy and friends — internal to client/game/
                  The other files there own one feature each:
                    settings.*      <game>/cw-mod/cw-mod.json, read once at boot
                    boot_profile.*  what kind of boot this is (offline, LAN, online + backend)
                    arxan_call.*    calling an Arxan-guarded engine function from our module
                    dw_backend.*    the local Demonware backend, client side
                    dw_net.*        the Demonware network choke point (winsock redirect)
                    local_lpc.*     playlists from the local LPC files, no objectstore needed
                    lan_browser.cpp the Server Browser beacon: hosts broadcast, clients list
                    zm_progression.* ZM XP, weapon levels and saves in LAN and offline matches
                    ui_scripts*     our own menu Lua (cw-mod/ui_scripts), incl. the CUSTOM MAPS tab
                    mapkit_*        custom maps: the loader, the zone trace, the live-state dump
  hooks/          Engine + IAT detours, one file per hooked function under impl/.
  memory/         Signature scanning, MinHook wrapper, IAT walking.
  overlay/        D3D12 hook + the ImGui menu. menu.cpp owns only the window and the tab bar;
                  tabs/ has one translation unit per tab.
  scripting/      GSC/CSC lazy-link loader.
common/           Logging, string/IO/NT utilities. No game knowledge.
gsc/              Sample and tool GSC scripts (trainer, dev tools).
mapkit/           The custom-map tools: the Godot builder, cwlink, zonekit, ffinfo, mkasset.
                  See mapkit/README.md.
tools/            Offline Python tooling, incl. dwserver/ (the local Demonware backend);
                  wordlists/ holds the name lists it consumes.
docs/             The roadmap (docs/ROADMAP.md: status, what's next) and the reverse-engineering
                  notes. Update these when you learn something.
```

Two rules that are load-bearing rather than stylistic:

- **Every deref on a reverse-engineered struct layout goes through `SafeRead`/`SafeCopy`.** A wrong
  offset must produce a diagnostic line, not an access violation.
- **The menu never calls game code directly.** It draws on the render thread; button handlers
  `Enqueue()` a closure that runs on the game thread. See `client/overlay/menu.hpp`.

## Dev setup

- Windows + **Visual Studio 2022** or newer, with the "Desktop development with C++" workload
- Clone the repo. Every third-party library is vendored under `vendor/`; there are no submodules.
- Generate the solution: `scripts\bootstrap.ps1`. The first run downloads Premake 5.0.0-beta2 into
  `tools\premake` (git-ignored).
- Build: `scripts\build.ps1`, or open `t9_vs2022.sln`.

> ⚠️ Match your **game build** to the project's pinned target, **1.34.0.15931218**. Mods are
> version-locked — a mismatched patch will crash or refuse to load.

## Workflow

- Branch per change; open a PR. Keep PRs focused.
- Update `docs/` when you change behavior or learn something new about the engine.
- No secrets, no game binaries, no copyrighted assets in commits.
