# Architecture

The client is one DLL, `discord_game_sdk.dll`, dropped next to `BlackOpsColdWar.exe`. The game
loads it as its Discord SDK. The DLL is pinned to build **1.34.0.15931218**: engine addresses are
dump-absolute constants in `client/game/dump_anchors.hpp`, rebased at runtime.

Alongside it, `tools/dwserver/` is a local Demonware emulator in Python. Its milestones are in
[backend-roadmap.md](backend-roadmap.md); the whole project's status is in [ROADMAP.md](ROADMAP.md).

## Boot sequence

| Step | Where | What |
|---|---|---|
| 1 | `main.cpp` `DllMain` | Identifies the build by checksum and starts the main thread. |
| 2 | main thread | Log service, crash logger (appended VEH), ntdll debug-function restore, MinHook, `Arxan::SysHooks`. |
| 3 | `DiscordCreate` export → `MainEntryPoint` | The game calls into the "SDK". This is the first safe point to touch the engine. |
| 4 | `Pointers` ctor (`game/game.cpp`) | Resolves anchors and signatures. `DwBackend::PatchEmbeddedKeys` swaps in our RSA keys if the key files exist. `Boot::Resolve` + `Boot::ApplyEarly` pick the boot profile and set the network mode **before the frontend is built**. |
| 5 | `Hooks` ctor (`hooks/hook.cpp`) | System, input and winsock hooks. `DwNet::Init` arms the Demonware redirect. |
| 6 | first exception through `RtlDispatchException` | `PostArxanDetectionHooks`: every engine detour, then the ImGui D3D12 overlay. |
| 7 | `Scripting::Initialize` | Resolves GSC signatures (read-only; the loader patches on demand from the Scripts tab). |
| 8 | thread waiting on `Scr_Initialized` | `Boot::ApplyAfterScriptInit`: the second half of the `nodw` sequence (below). |

## Boot profile

The frontends are built from the session network mode during boot, so the mode is chosen in
`<game>/cw-mod/cw-mod.json` and read once (`game/settings.cpp`, used by `game/boot_profile.cpp`).
That one file holds every cw-mod setting; nothing reads the old marker files (`online`,
`no-scripts`, ...) or `cw_mod_name.txt` any more. A boot with no `cw-mod.json` writes one from
whatever markers are there, and later boots log any markers still lying around.

```json
{
  "name": "Player",
  "mode": "online",
  "backend": true,
  "start_screen": true,
  "scripts": true,
  "progression": true,
  "live_menus": true,
  "local_playlists": true,
  "lobby_waiver": true,
  "lua_print": true,
  "fpsession_standin": false,
  "ui_text_log": false,
  "ui_text": {
    "Connecting to Call of Duty™ Online Services.": "Connecting to cw-mod."
  }
}
```

| Key | Effect |
|---|---|
| `name` | In-game name (at most 32 bytes). Empty: the Windows account name. The `CW_MOD_NAME` environment variable overrides it for one process. With the backend, the studio token carries it and the auth server bakes it into the ticket, so it is also the name after login. |
| `mode` | `offline` (default), `lan`, `lanlobby` (online nibble, lobby LAN) or `online` (lobby LIVE). |
| `backend` | Use `dwserver/auth_pub.der` + `lsg_pub.der` when present: our keys are patched in, login dials 127.0.0.1, and the winsock redirect is armed. `false` ignores the folder. |
| `start_screen` | Online + backend: open on the title screen so pressing start creates the party. `false` jumps straight to the director. |
| `scripts` | GSC loader on at boot (the Scripts tab can still toggle it). |
| `progression` | ZM progression kept on this PC, on every boot profile (`game/zm_progression.hpp`). |
| `unlock_all` | Off by default. `true` answers every lock and ownership question as unlocked or owned: level and weapon-level locks, camo challenges, the store inventory (blueprints, bundles, battle pass rewards) and the battle pass. Answers only, nothing is written to a save (`game/unlock_all.hpp`). |
| `live_menus` | Tell the menus a LAN lobby is LIVE (locks and gun levels show). |
| `local_playlists` | Load the playlists in `cw-mod/lpc`. |
| `lobby_waiver` | Backend: let Lua's `IsDemonwareFetchingDone` ignore the matchmaking/commerce bits. |
| `lua_print` | Lobby Lua print transcript in `client.log`. |
| `fpsession_standin` | Diagnostic: substitute a zeroed first-party session instead of watching for new null-session callers. |
| `ui_text` | Replace UI text: `{ "text the game shows": "new text" }`. The whole string must match; ASCII case is ignored, so a title shown in capitals still matches. Keep any `&&1`-style placeholders. Works for every localized string, Lua or native (`hooks/impl/game/DecryptString_UiText.cpp`). |
| `ui_text_log` | List every UI string once as a `(UiText)` line in `client.log`, to find the exact text to put in `ui_text`. |

A key that is missing is added with its default; a file that does not parse boots with every
setting at its default (offline) and says so in the log and on the overlay's Home tab.

The `nodw` dvar gates both the Battle.net sign-in watchdog and the Demonware login driver, so it
is set in two steps:

| Profile | In the ctor | After `Scr_Initialized` |
|---|---|---|
| offline / lan, no backend | set mode | `nodw = true` + `CL_Disconnect` |
| offline / lan + backend | set mode | `nodw = false`, login dials the local server |
| online, no backend | set mode, `nodw = true` | nothing |
| online + backend | set mode, `nodw = true` | `nodw = false` (the watchdog's window has passed) |

Read a boot back with `python tools/bootlog.py`.

## Hooks

**System and Arxan** (`hooks/impl/patched`, `hooks/impl/iat`)
- `SetUnhandledExceptionFilter`, `RtlDispatchException`: keep Arxan's exception games working. The first dispatch triggers the post-Arxan hooks.
- `CreateMutexExA`, `GetThreadContext`, `NtQueryInformationProcess`: anti-debug and single-instance checks.
- `GetRawInputBuffer`, `GetAsyncKeyState`: keep input away from the game while the overlay is open.
- `getaddrinfo`, `gethostbyname`, `connect`: the Demonware choke point. `*.demonware.net` resolves to loopback, and resolves and connects are journalled to `cw-mod/dw_journal.txt`.

**Engine fixes** (`hooks/impl/game`)
- `luaL_traceback` + `luaG_getobjname`: the engine leaves `ar.name` uninitialised and crashes on any Lua error.
- `LuiError_ReportFatal`: logs fatal LUI errors. On an online boot it suppresses them; this is a crutch to remove once the backend boots clean.
- `BB_Alert`: logs every err_drop with caller RVAs.

**Demonware login** (installed when the backend is enabled)
- `Dw_GetLoginFlow`: forces studio-auth flow 9.
- `DwLogin_BuildStudioToken`: supplies a local studio token.
- `Login_SetStatus`: mirrors the login status transcript into the log.
- `LiveUser_FirstPartyPresenceOk`: presence gate that login state 1 needs (RA-guarded).
- `FirstParty_GetSession` (watchman) + `FirstParty_GetLocalUserIndex`: serve the one consumer of the null first-party session.
- `LiveUser_SignOutDropGate`: reports a sign-out the way an offline boot does. Not yet verified in-game.

**Battle.net errors** (always installed, inert unless online)
- `LiveUser_ForceSignOutAndFatal`: the BLZBNTBGS watchdog; this one is the real fix.
- `BnetError_ReportFatal*`, `FirstParty_SetError*`, `ErrorQueue_Push`: belt and braces. Delete them once roadmap B1 confirms they never fire.

**Temporary** (delete at roadmap B4, when bdStorage is served)
- `PlayerData_ControllerStorageTick` + `PlayerData_OnStorageOpComplete`: redirect dwuser player data to hdd so the online frontend can read it.

## Overlay

ImGui over D3D12 (`client/overlay`). Tabs: Home, Session (LAN host/join, boot profile), Demonware
(key patch, redirect journal, login state), Scripts (GSC loader), Debug (name hashing, dvars).

## Backend

`python tools/dwserver/run.py` checks the setup, then runs auth (443 + CRL on 80) and LSG (3074) in
one console with a combined log in `material/server.log`. `--test` runs the offline tests. Protocol
notes: [re/demonware-login.md](re/demonware-login.md) and `tools/dwserver/README.md`.
