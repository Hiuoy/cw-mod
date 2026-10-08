# Backend roadmap: a local Demonware for BOCW

> **Status of every track, this one included: [ROADMAP.md](ROADMAP.md#3-local-demonware-backend).** This file
> holds the backend milestones in detail. Where they stand on 2026-09-30: B0, B1, B3 and B6 to B8 passed (an
> online match with two PCs, 2026-09-29); B2 and B5 are part done; B4 is done locally.

> **Read [fork-findings.md](fork-findings.md) first (2026-09-22).** The two forks traced padlocks
> (B5), player storage (B4) and the no-XP problem to local causes, not missing server replies. Before
> starting B3–B6, check whether a question here is already answered there.

**Goal:** normal online content (playlists, progression, entitlements, online lobbies) served by a
local Demonware emulator we run ourselves, instead of forcing the UI into states it never reaches on
its own. Build is pinned to **1.34.0.15931218**. Everything runs on the player's own machine for a
game they own; retail Activision services are never a target.

**Split of responsibilities:**
- **Demonware (this roadmap):** identity, publisher data, player storage, stats, entitlements.
- **LAN transport (already proven):** peer connectivity. The two-PC join works with the
  hand-carried host descriptor. Public matchmaking is out of scope.

## How every milestone is done

1. Start the backend with `python tools/dwserver/run.py`, boot, then read it back with
   `python tools/bootlog.py` and the server journals (`tools/dwserver/material/*.jsonl`).
2. The failing login status or error names a **stage**. Decompile that stage's handler in IDA and
   read the contract: the fields it parses, their order, and each error string.
3. Build the smallest server change that satisfies the contract.
4. Pass criterion: the in-game log line named in the milestone. A passing offline server test is
   never enough on its own.

Rename and comment everything you identify in the IDB (`E:\tools\Reverseing\bocw_fixed_renamed.i64`),
in the same sitting.

## Where things stand

| Stage | Status |
|---|---|
| Winsock redirect (`demonware.net` → 127.0.0.1) | proven in-game |
| Local CA + CRL, TLS through Schannel | proven in-game |
| Auth (`/auth/`, PSS-signed reply, plaintext ticket) | proven in-game |
| Umbrella legacy LSG token | proven in-game |
| LSG handshake + 0x85 record layer | proven in-game |
| Lobby reply (tag 1), `[status 27] Login Complete` | proven in-game 2026-08-02 |
| Online boot + backend together | proven in-game 2026-09-16 17:33 (`run.py` up; the earlier HTTP 0 boots had no server running) |
| Post-login | proven in-game 2026-09-16 17:52: no sign-out (it is a promotion to signed-in-online), and with the first-party presence gate answered the client sends its first post-login lobby requests. All get default replies for now. See B1. |
| Online party, playlists, Zombies lobby, match launch | proven in-game 2026-09-24 (B3, B6, B7, B8) |
| Two PCs in one online match, each with its own backend | proven in-game 2026-09-29 22:18 (B8) |

Protocol details live in [re/demonware-login.md](re/demonware-login.md) and `tools/dwserver/README.md`.

## Milestones

### B0: restore the baseline (DONE 2026-09-16)
An online + backend boot reaches `[status 27] Login Complete` again. `HTTP code [0]` means TLS died
or nothing was listening. `run.py` checks every known cause before it starts (CA in
`LocalMachine\Root`, CRL valid and served on :80, deployed keys match `material/`, no stale server on
the ports), and `bootlog.py` shows whether a server session was up during the boot. Preflight passed
on 2026-09-16, so the next boot with `run.py` up should settle it.

Then confirm the (still unverified) `LiveUser_SignOutDropGate` fires.

### B1: a stable online boot after login (DONE 2026-09-16)
Kill the post-login err_drops (`Sail 630 Nuclear Bug` / `Boy 501 Gothic Missile`). Name the drop path
from the BB_Alert caller RVAs. Keep a fix only if it supplies what the engine actually asks for, not
one that just suppresses the error. Then confirm the belt-and-braces Battle.net error detours never
fire, and delete them.

**RE result (static, 2026-09-16; names are in the IDB):**
- Login driver state `liveUser+5768`: `LiveUser_OnDwLoginComplete_SetState4` (0x7FF727F38F30) sets
  **4 = connected**, and 0 = connection lost/retry. `LiveUser_UpdateSigninState` calls
  `LiveUser_OnDwConnected` (0x7FF729722670) on the transition into 4.
- signinState `liveUser+5764`: 0 none, 1 local/signed-out, 2 signed in online.
  `LiveUser_OnDwConnected` → `LiveUser_PromoteSigninOnline_MaybeDrop` (0x7FF729753740, previously
  misnamed `LiveUser_SignOut_BuildDropMessage`) **sets it to 2** and asks for a drop only if it was 1
  while `Com_SessionMode_IsOnline()`. The drop path then forces session mode offline (`|0x6030`)
  and raises ERR_DROP. So "the sign-out" is a promotion with an unwanted offline demotion. None of
  its inputs (prior signinState, session mode, a UI string dvar) comes from the server.
  `SignOutDropGate` returning 0 is the correct outcome, not a mask.
- **The check that actually blocks post-login traffic:** `LiveUser_IsOnlineReady` (0x7FF7262ACD00) =
  `LiveUser_FirstPartyPresenceOk() && signinState==2 && loginState==4 && Dw_GetConnectionState==2`,
  and `LiveUser_PresenceOkAndSignedInOnline` (0x7FF7274B3540). These gate every per-user online
  subsystem tick in `LiveUser_UpdateSigninState` (from 0x7FF7297204B7), and 59 other call sites.
  On studio auth every term passes except `FirstPartyPresenceOk`, which reads Battle.net state.
  Our hook forces it true only for the login driver. **The server cannot supply this. The fix is
  client-side.**
- Risk: `FirstPartyPresenceOk` has 30 callers, including `LiveFirstParty_Frame`, the BLZBNTBGS
  watchdog. Widening it blindly can re-arm that watchdog.
- **Built 2026-09-16, not yet booted:** the presence detour now returns true to all callers once DW
  login is complete (controller 0: loginState 4, signinState 2), only while Bnet-error suppression is
  on (the `ForceSignOutAndFatal` detour neutralises the watchdog), except
  `LiveUser_OnSigninStateChange_SetupIdentity`, which would read the gamertag from the unsigned
  first-party manager. Each overridden call site logs once (`(Presence)` lines in `client.log`).
  The drop gate's log now says what it is: a promotion. **Pass:** `lobby_requests.jsonl` gains entries
  after `reportExtendedAuthInfo`, with no BLZBNTBGS and no new crash. If it crashes, the last new
  `(Presence)` site logged before the crash is the suspect.
- **PASSED in-game 2026-09-16 17:52.** 12 presence sites answered true within the Login Complete
  second (none denied, no watchdog, no crash), the promotion log fired, and the client sent its
  first post-login requests: `service 10 / task 1` (x2, ~430-byte blob), `service 3` msgType 95 (a
  StructData with the strings `client`, `bnet`, `client_tu34`, `bnet_tu34`; retried every 2s x4 on our
  default reply, then dropped), `service 6` msgType 12, `service 3` msgType 27. All got the default
  reply. The `[status 25]` at 17:54:02 is our own Ctrl+C on `run.py`, not the client. These are the
  first inputs for B2. `lobby_router` cannot walk type 23 (StructData) yet.

### B2: the service map (static RE, runs alongside B1) (PART DONE)
Enumerate every caller of `BdLobbyMsg_Ctor` (0x7FF729DD2300). For each, record:
- the serviceId and taskId
- the `bdX::method()` string naming it
- the request fields, in serializer order
- the result deserializer (vtable slot 5, behind Arxan return-gadget thunks, so script the chain-follow)

Do the same for every HTTPS path template (objectstore / umbrella / uno) and its reply parser.

Output: `docs/re/dw-service-map.md`, plus named stubs in `tools/dwserver/lobby_router.py`.

**Started 2026-09-16 with service 3 / msgType 95** (the request that retried x4):
- It is the **StructData request shape**: `[5f][03 03][17 08 <u32 len>][protobuf][00]`, no task id,
  so StructData requests are keyed (service, msgType). The body is a 40-byte protobuf
  `{1: "", 2: [client, bnet, client_tu34, bnet_tu34]}`.
- Builder `BdLobbyMsg_WriteStructDataPayload` (0x7FF729DCCFB0) has one caller, a rel32 call at
  0x7FF729D9A027 inside Arxan-flattened code (IDA shows no xref; found by scanning the dump). Walking
  that chain back to the named feature is still open.
- **Reply contract (read, not guessed):** `BdStructTask_ReadStructDataReply` (0x7FF729DCCE20) needs,
  after the normal handle/error/flag envelope, a StructData `17 08 <len>` whose bytes go to the result's
  protobuf parser. Our generic `08 00000000` result fails that tag check, which explains the retries.
- `lobby_router` now walks type 23 (with a schema-less protobuf decode in the journal) and answers
  StructData requests with an empty StructData (len 0 = a valid all-defaults message). `routertest.py`
  pins the capture and the reply. **Pass:** msgType 95 is sent once, not four times. If it still
  retries, the result message needs content and its schema is the next RE target.
- **PASSED in-game 2026-09-16 (20:19 and 21:09 boots):** msgType 95 was sent once per boot.
- **LSG keepalive (the other stability gap, found on the 20:19 boot):** the client closed the LSG exactly
  120s after the last bytes the server sent, then logged `[status 25]` and did a full re-login, 8 times in
  30 min. Its own 40s keepalives don't reset that timer. `Lsg_PumpRecv` (0x7FF729DE8470) restarts
  its last-receive stopwatch (conn+320) on any read of more than 0 bytes, so the server now sends back a
  zero-length frame for each keepalive (`ECHO_KEEPALIVE`). **PASSED in-game 2026-09-16 21:09:** one
  LSG connection for 4.5 min, 6 echoes, no `[status 25]`.
- Shutdown noise, not a bug: closing the game logs a null-session read from `+0xCC19EA1` and two AVs
  (`telescope.dll+0x1AAC2C7`, `BlackOpsColdWar.exe+0xD59FED6` reading 0x18), plus WinError 10054 on
  the server. Seen at the same addresses on both exits.
- Also seen: `BdRemoteHttp_SendLobbyRequest_Svc10` (0x7FF729DD17C0) builds service 10 / msgType 255,
  an HTTP-style request tunnelled through the lobby (objectstore `UserObjectsResource`,
  `PublisherObjectMetadataResource`).

Then run the **census boot**: online + backend, sit in the menu, click the mode tiles. Diff
`lobby_requests.jsonl` / `requests.jsonl` against the map. What the client actually asks for sets the
order of B3–B5.

**Status 2026-09-30.** The census ran: the 2026-09-24 06:24 boot sent 19 lobby requests with no retries, and
`material/lobby_requests.jsonl` holds every request since. The router's labels were corrected on 2026-09-24: what
it called "msgType" is the DW **service** id and its "serviceId" the **task** id, as CodRevamped's IW8 router decodes
them ([codrevamped-notes.md](codrevamped-notes.md)). Still open:
- `docs/re/dw-service-map.md` is not written.
- The service names taken from the IW8/T8 tables carry a `?` in `lobby_router.py` until a T9 boot or the IDB
  confirms them.

### B3: publisher data (objectstore, HTTPS) (DONE 2026-09-24)
Serve playlists, playlistschedule, ffotd and motd from the player's own local LPC files (never
committed, see `.gitignore`).

**2026-09-16: the LPC list is served (awaiting boot).** The objectstore requests do not arrive over
HTTPS; they come through the lobby as service 10 / task 1 (msgType 255). Reply contract, read from IDA:
- rows: `UInt32 count, UInt32 total`, then `BdRemoteHttp_ReadResponse` (0x7FF729DC8C50):
  `UInt32 1, Blob(header protobuf), Bool hasBody, Blob(JSON)`.
- header protobuf (forward-only, ascending fields): `1` status (2xx), `203` double, `302` content type
  (1 = JSON, required or the body is never parsed), `400` bool.
- JSON `{"objects":[{"metadata":{...}}]}`, required keys and buffer sizes in `lobby_router.py`.
- `Lpc_SyncFrame` writes `.manifest` from that list itself, then MD5-checks each local file. All
  match → `Lpc_IsManifestLoaded`. So the handler lists `*_tu34_100_<buildId>.ff` from the ProgramData
  LPC folder; the TU35 files were copied locally to tu34 names (build checksums in the headers match).
- **Pass:** `[router] objectstore LPC list ... 4 file(s)` on the server, then contentSlots 2,2 in the
  `(Entitlements)` gate log. Not served yet: `contentURL` downloads (needed only if an MD5 mismatches).

**Result 2026-09-24.** On the first online boot that built a party (B6), the backend listed no LPC objects, so
there was no playlist ("No preferred playlist is loaded!"). The client's `local_lpc` now also runs on online +
backend boots and loads the player's own playlist zones directly. **PASSED 21:47 and 22:01:** `g_playlistValid 1`,
"Loaded 177 as preferred playlist", and the modes and maps show in the online menu.

### B4: player storage (bdStorage) (DONE LOCALLY)
Per-user files on disk. When dwuser storage becomes available for real, **delete** the temporary
`PlayerData_ControllerStorageTick` / `PlayerData_OnStorageOpComplete` redirect hooks.

**Answered in the forks:** local storage needs no server and no hook. The redirect is the one int
`def+64`, and `PlayerData_Init` already writes it under its `forceLocalStorage` dvar. See
[fork-findings.md](fork-findings.md) §3.

**Status 2026-09-30.** Saves work on local storage through the client's redirect of the dwuser-backed data maps.
On 2026-09-24 the redirect was found to pull maps 32 and 33 (20 and 10 versions) into `AreLocalFilesReady`, which
blocked the online menu (B6), so every version of each redirected map is now seeded. A server-side copy
(bdStorage) is not planned, so the redirect hooks stay.

### B5: entitlements, stats, inventory (PART DONE)
Whatever the census shows drives `package_owned` and the mode-tile padlocks. The auth reply's
`extended_data` field is the first lead.

**Answered in the forks:** the padlocks are not an entitlement check. They are a missing type-1
lobby, and online boots never create one. The inventory cannot fill on studio auth because
MtxSync waits on a Battle.net `ZEUS` token. Match stats are gated by
`LiveStorage_AreMatchStatsEnabled`, which excludes private matches even online. See
[fork-findings.md](fork-findings.md) §1, §2, §4.

**Status 2026-09-30.** Done on the client: the padlocks (the missing party, fixed in B6) and the "you don't own
the game" upsell (`LiveUser_IsTrial.cpp`, 2026-09-24). Still empty: the marketplace inventory and the in-game
store, which need the Battle.net token; neither is planned. Match stats on online boots are not measured yet.

**What the battle pass and the store read (2026-10-06, static; nothing built).** The player asked why the battle
pass does not move and why the store lists only a few bundles.
- **Battle pass.** Tier and XP sit in the loot block (`LiveUser` user data +40024), 8 bytes per season: owned at
  +0x9AE34, tier at +0x9AE35, XP at +0x9AE38. The getters (`Loot_GetBattlePassRank` 0x7FF7279014F0 and its
  neighbours) return 0 unless `DwFetch_IsInventoryReady` holds, and on our backend the inventory never loads (the
  log's `(Entitlements)` heartbeat: `mtxSync=1`, `inventory state=1`, `items=0`). The only writer of tier and XP is
  a Demonware Achievement Engine event, kind 0x5D1491F2FFF95A42, handled in `LiveStats_OnAeEvent_cand`
  0x7FF726A7FA20: the server works out battle pass XP and pushes it. Player level does not feed it. The XP each tier
  needs is client data (`gamedata/mtx/seasons/mtx_season_<n>_xp.csv`).
- **Store bundles.** The item shop is built in Lua (the BattlePass/MTX utility, Lua asset `3c50bd229ba4cd94`, with
  its tables in `1b4479f96debe738`) from two string tables in the game's own zones. The item table (60-bit hash
  0xB4D7577727AEA46) has every item and bundle: id, name, category, price. The shop schedule (0x6F2E5FCE34E0591) has
  a start and end time, a segment, a category and up to 100 items per row. A row shows while
  `Engine.GetCurrentUTCTimeStr` is inside its window, and an item the player owns is left out. Which rows are active
  today was not read. `owned` is the item's quantity in the marketplace inventory (`Engine[0x352DC095BBB2A45]`).
- Both need a marketplace inventory, and the battle pass also needs an Achievement Engine reply that carries XP and
  tier. A local inventory cannot tell a bought item from one that was not: it would own whatever its file lists.

### B6: online lobby over LAN transport (DONE 2026-09-24)
An online-mode private lobby whose peers connect over the proven LAN path.

**The gate is named (static, 2026-09-24).** It is a Demonware checklist, not an unexplained Lua
choice. `Lobby.ProcessNavigate.PressStart` → `ShouldBeginLAN` false → `BeginLivePlay`, which
navigates to the online menu (and so runs `LobbyHostStart`, the lobby the tiles bind to) only when
`Engine.IsDemonwareFetchingDone && Engine.AreLocalFilesReady`. Otherwise it shows the "connecting to
online services" overlay and waits. `AreLocalFilesReady` is the playerdata buffers, which our
dwuser→hdd redirect already satisfies. `IsDemonwareFetchingDone` ends in `DwFetch_GetStatus`
(0x7FF7262A4BA0). It ORs one bit per ready subsystem and needs all of `0x17337FA`:

| bit | letter | ready when | likely owner |
|---|---|---|---|
| 0x2 | B | presence ok + signed in online | B1 (done) |
| 0x8 | D | `g_netSessionLaunchState == 8` | net session launch, unknown |
| 0x10 | E | `LiveUser_IsOnlineReady` | B1 (done) |
| 0x20 | F | LPC manifest loaded | B3 (done) |
| 0x40 | G | a content slot installed | LPC content |
| 0x80, 0x200, 0x800000, 0x1000000 | H, J, X, Y | content slots 0, 1, 9, 10 == 2 | LPC content |
| 0x100 | I | `g_pubVarsState == 4` | service 95 (was 4 on 09-17) |
| 0x400 | K | online stats maps + 10 data maps ready | storage redirect |
| 0x1000, 0x2000, 0x400000 | M, N, W | three state globals (in the IDB comment) | unknown |
| 0x10000 | Q | `LiveUser+5776 != 0` | unknown |
| 0x20000 | R | per-controller state == 3 (waived by a dvar) | unknown |
| 0x100000, 0x200000 | U, V | dvar-gated checks | unknown |

MtxSync cannot block it: while MtxSync is not done, the required mask adds bit 0x80000, which is
"MtxSync not done". The engine draws the same mask as 25 letters (`A.B.-.D…`, `-` = missing) in
its connection-info string (0x7FF7262AD670).

**Next boot (built 2026-09-24):** `DwFetch_Transcript.cpp` logs the mask, the missing bits and the
raw state behind them each time it changes (`DwFetch` lines in `client.log`). Boot online + backend,
press start, then wait on the overlay if it appears. The missing bits are the B6 to-do list. Each one
is either a server reply we owe or a client-side gate.

**First boot (2026-09-24 06:24), measured.** Login Complete in 5 s, 19 lobby requests, no retries. The
mask settled at `0x3D1FBF`: every required bit set except five. Read from IDA the same day, all five
are public matchmaking or commerce, not anything a private lobby uses:

| bit | what it waits for | why we can't serve it |
|---|---|---|
| G 0x40 | slot 1 `core_ffotd` listed in the LPC manifest | our TU35 files ERR_DROP in the TU34 exe (B3) |
| Y 0x1000000 | slot 10 `ingamestore_<lang>.json` fetched and parsed | the store; only fetched after G |
| R 0x20000 | marketplace inventory loaded | needs a Battle.net ZEUS token (B5); waived by the engine when a dvar is off |
| N 0x2000 | dedicated-server QoS done (`OnDediQosReady`) | fed by Lua `Lobby.MatchmakingAsync` from matchmaking-service events |
| W 0x400000 | SocketRouter relay bound | same pipeline: relay tokens, then `SocketRouterBindToRelay` |

The same boot showed the player the "you don't own the game" upsell on the tiles. The presence blob
said `"trial":true`. `LuaUtils.IsTrial` → `LiveUser_IsTrial` (0x7FF728151A00): `nodw` returns
not-trial (why offline never upsells). Otherwise it is a trial unless the Battle.net license list holds
product `0xCDE9`, and studio auth has no license list.

**Built 06:38 (not booted):**
- `DwFetch_LobbyWaiver.cpp` detours `DwFetch_IsDone`. It answers true to Lua's
  `IsDemonwareFetchingDone` only, and only when every missing bit is one of G/N/R/W/Y. The engine's own
  callers (store tick, dedicated matchmaking) keep the truth. Opt out: `"lobby_waiver": false` in `cw-mod.json`.
- `LiveUser_IsTrial.cpp` answers not-trial on backend boots (what `nodw` gives offline) and logs the
  real answer per call site (`Trial` lines).
- Pass: `DwFetch ... answering TRUE (lobby waiver ...)` after pressing start, then the online menu
  instead of the connecting overlay, and tiles without padlocks or the upsell. If the online menu opens
  but the tiles are still padlocked, the lobby needs something past this gate. That was B6's real
  risk from the start.

**Results, 2026-09-24:**
- 06:47: the waiver answered TRUE and the upsell was gone, but the tiles stayed padlocked, and a click only
  played a sound. **The padlock is the missing party.** The tile's lock state is `not privateClient.isHost`, and
  the party is created only by `Lobby.ProcessNavigate.CreatePrivateLobby`, on the step from the title screen to
  the online menu. Our boot skipped that step: it jumped straight to the menu through
  `LobbyUI_SetTargetMenuAndNotify` (0x7FF727DAF430, formerly `Unk_SetScreen(10)`), inherited from the original fork.
- Fix: online + backend boots keep the title screen (`"start_screen"` in `cw-mod.json`), so
  `PressStart → BeginLivePlay` runs. The Battle.net error dialog this exposed was fixed too (gone on the 20:16 boot).
- `BeginLivePlay` then waited on `AreLocalFilesReady`, which failed on maps 32 and 33 (see B4). Seeding every
  version fixed it.
- **PASSED 21:27:** `LobbyHostStart(privateLobby)` succeeded and `IsLobbySlotLive(0)` turned true. The party exists
  and the tiles unlock. The next wall was the missing playlist (B3, result above).

### B7: starting a Zombies match (DONE 2026-09-24)
Every ZM Start went to dedicated-server matchmaking (`director_online_public`: wait for DS pings, relay, async
search), timed out and showed "Failed to host lobby". The Start handler picks public instead of
`director_online_private` while the bool dvar `0x7720058BE2417E91` is true, and the exe registers it true.
`EntitlementGates::Tick` (`client/game/session.cpp`) forces it false on backend boots ("ZM Start routing" in
client.log). **PASSED 22:43:** Start opens a self-hosted game lobby, the countdown runs, and the game launches.

### B8: the match launch (DONE 2026-09-24; two PCs 2026-09-29)
Two crashes at launch, one after the other:
- `sys_error ... luafile`: `LUI_Init` runs `ui/ffotd_tu34.lua` once the content slots read as loaded, and no ffotd
  zone ever loads. A detour on `LUI_RunFile` (`LuiError_ReportFatal.cpp`) skips `ui/ffotd*` when the luafile is
  absent. Passed 22:57.
- A null read in the account-id picker (`LiveUser_GetAccountId_cand` 0x7FF7296AB340): with presence answered true,
  it asked the null Battle.net account. The presence detour now answers the real false inside the two account-id
  functions (`LiveUser_RefreshAccountId_cand` 0x7FF7296AB2F0 has the same branch), so both take the Demonware id
  ("DENIED — account-id picker" in client.log).

**PASSED 23:06:** the match loaded and ran until the return to the menu at 23:08 (the second ffotd skip in
client.log is the frontend reload).

**Two PCs, 2026-09-29 22:18:** a host and a second PC played one online match (a custom map) for about 3 minutes.
What it needed, from the failed tries of 2026-09-24:
- each PC its own identity: every PC's backend minted XUID 1, so `cw-mod.json` now holds a per-PC `"xuid"` that
  reaches the ticket through the studio token;
- each PC's own peer address in the LAN shape: a `bdCommonAddr_Ctor` detour (`BdCommonAddr_Ctor.cpp`) leaves out
  the public address and sets NAT open, so peers use each other's local addresses.

Both PCs were on one home network. Two PCs on different networks (over a VPN) are not tested since these fixes.
