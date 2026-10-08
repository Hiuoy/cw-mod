# CodRevamped: what it means for us

Read 2026-09-24 from `E:\Games\Games\Cods\tools\CodRevamped-master` (newly released, MIT). **No code
has been ported.** These are notes only, like [fork-findings.md](fork-findings.md).

It is a multi-game "offline/LAN revival" suite: BO4 (T8), Cold War (T9), MW2019 (IW8), and stubs for
later titles. It works the opposite way from us. It **blocks** Demonware and public networking and
forces the game into offline/LAN. We **emulate** Demonware. The Cold War part is a 36k-line
monolith (`src/shared/core/CoreRuntime.cpp`, "v47.95"), mostly INT3/VEH research tracing.

## The one finding that matters: our DW router has service and task swapped

Their IW8 backend (`src/backend/iw8/Demonware/DemonwareTaskRouter.cpp`) decodes a task request as

    [u32 len][0x86][serviceId, RAW byte][03 taskId, typed U8][params...]

That is the same position as our `[raw msgType][03 serviceId]` ([lobby_router.py](../tools/dwserver/lobby_router.py)).
So our **"msgType" is the DW service id**, our **"serviceId" is the task id**, and the UInt32 we
held "on probation" as the task id is really the first parameter.

Evidence: re-read as (service, task), our census (`material/lobby_requests.jsonl`, 9017 rows)
matches the IW8 table and the T8 table (shield) pair for pair:

| our (msg_type, service_id) | rows | name from IW8 / T8 | our reply today |
|---|---|---|---|
| (38, 8) | 32 | bdAntiCheat (38 in both). Task 8 is T9's own; IW8 uses 6/7 for reportExtendedAuthInfo/BNetSessionToken | generic |
| (8, 1) | **8781** | T8 bdProfiles.getPublicInfos, body = UInt64 1 | errorCode 800 since 09-17 19:19, which **ended** the storm (8781 rows came from the boots before that fix) |
| (8, 3) | 8 | T8 bdProfiles.setPublicInfo (carries a blob) | generic |
| (12, 6) | 19 | bdTitleUtilities.getServerTime (both) | generic |
| (27, 3) | 19 | bdDML.getUserHierarchicalData (T8) | generic |
| (95, 3) | 22 | bdPublisherVariables.retrievePublisherVariables (IW8) | generic, was the "retried four times" request |
| (104, 6) | 12 | bdMarketingComms.getMessages (both) | empty StructData, same as IW8 |
| (125, 9) | 6 | **bdAchievementsEngineService.getUserState** (IW8; T8 lists 125 as `bdUNK125`) | empty StructData |
| (125, 3) | 12 | AE service, task unknown | empty StructData |
| (196, 100) | 6 | unknown (IW8 has 197 = bdMW4Service, 198 = bdMessaging) | empty StructData |
| (255, 10) | 100 | bdRESTLegacy.request (IW8) | generic |

Six exact pairs is not a coincidence. `SERVICE_NAMES = {8: "bdAntiCheat"}` in our router is
therefore wrong: 8 is the task id of that one request, and service 8 is a different service.
**Fixed 2026-09-24** in `lobby_router.py`: bdAntiCheat is service 38, and names taken from the IW8/T8 tables
carry a `?` until T9 confirms them.

Do not import the T8/IW8 tables wholesale. Task numbers drift between titles (IW8 bdAntiCheat has no
task 8). Use them to name census rows, then confirm each one.

### Reply shapes we can copy (IW8, all `status: from their code, untested on T9`)

Envelope: `[u32 len][0x01][U64 transactionId][U32 error=0][U8 taskId][result...]`, padded to 16.
**They echo the task id in the U8.** We send 0 there, labelled "flag" from
`BdStructTask_ReadStructDataReply`. Every reply with 0 there has been accepted, so it stays 0
until a boot shows it matters.

**Applied 2026-09-24:** `lobby_router.py` now uses (service, task) labels (census rows carry
`"schema": 2`), answers getServerTime with the IW8 shape, and gives the user and publisher object
lists an honest empty list. `routertest.py` still pins the login reply byte for byte.
StructData is `17 08 <u32 len> <protobuf> 00`, the same as ours.

- **getServerTime (12, 6):** `U32 1, U32 1, U32 unixTime`.
- **retrievePublisherVariables (95, 3):** the request's pb tag 2 strings are the namespaces. Ours asks
  for `client`, `bnet`, `client_tu34`, `bnet_tu34`. Reply with StructData, one tag-1 object per
  namespace: `{1: version=1, 2: 0, 3: name, 4: "{}"}`.
- **AE getUserState (125, 9):** the request pb is tag 1 = context (ours: `"5836"`), repeated tag 2 =
  keys (ours: `/t9/battlepass`, `/t9/doublexp`, `/t9/progression…`). Reply with StructData whose
  pb tag 1 is **one JSON string**, `{"<key>": <value>, ...}`, one entry per requested key.
  **This is the native path to the AE block** that [zm_progression.cpp](../client/game/zm_progression.cpp)
  now stands in for locally (LEVEL/AAR/gun XP read `ae_sync`). The value schema per key is ours to
  find; IW8 answers fresh-account zeros/false.
- **REST (255, 10):** `U32 1, U32 1, U32 1, Blob(pb metadata {1:200, 302:1, 400:false,
  1000:{101:txn, 102:false}}), Bool true, Blob(json + NUL)`.
- **DML (27, 2 in IW8, not our 27/3):** `U32 1, U32 1, "US", "United States", "", "", F32 0, F32 0,
  U32 0, "UTC"`. The shape may differ for task 3.

## Cold War client: our build, partly stale table

- Their "Retail" Cold War table is for **our exe, 1.34.0.15931218**. It carries a version marker at RVA
  0x11017B0 (= 0x8348B74E, checked against our dump), and six entries land exactly on functions we
  know. Their PE fingerprint (timestamp 0x6A038464, May 2026) is **not** ours (0x64FBD81C), so their
  exact-fingerprint gates would refuse our exe.
- The frontend/lobby entries are **stale**. `LobbyBase_SetNetworkMode` (0xAF20CB0) and
  `Com_SessionMode_SetNetworkMode` (0xC175190) land mid-function in Arxan junk. Ours are 0x7FF727B19B60
  and 0x7FF728D7C630. Their own code says they rediscover those at runtime.
- Names taken into the IDB (shape-checked, not run): `Cmd_AddCommandInternal` 0x7FF72668BAD0
  (+ list head `g_cmdFunctions`), `Scr_AddEntity` 0x7FF723F3F490, `Dvar_RegisterInt` 0x7FF728C87360.
  They second our `Dvar_SetInt_cand`, and call our `Dvar_ApplyValueInternal` `Dvar_SetVariant`.
- **Name conflict, left as a comment:** they call our `SV_Migration_ReseatClient_Loopback`
  (0x7FF723D49410) `SV_AddTestClient` and use it with `Scr_AddEntity` to spawn bots. The body fits
  that (default name, loopback connect, flag +53654, stats DDL init, enter world). Test in-game
  before renaming.
- `g_auth_manager` (RVA 0x17A63C38) hack from the older "T9 Offline / codUPLOADER" line: for
  1.34, walk `count @+0x8` entries of `@+0x10`, stride 0xD0, set `+0x14=0, +0x48=2, +0x4C=3` each
  frame. It forces the auth tasks to "done". It isn't needed with our backend. Noted only in case an
  offline-only boot ever wants it.

### Our code is in there

`src/shared/runtime/network/UniversalLan.cpp` ("cw-mod-v2 bridge") is a port of our LAN
join/descriptor work. All 10 of its anchors (JoinPendingTarget, pending-target globals, session
object offsets, netmsg dispatch/JoinResponse/ParseJoinLobbyRequest, `CWJOIN1.` blob) appear in this
repo and in cw-mod-VC2. The only credit is the name in code comments. Our LICENSE is MIT with
"cw-mod contributors". Whether to ask for the notice is the user's call.

## Lower value, recorded so nobody re-reads it

- **LAN Zombies menu** (`CoreRuntime.cpp` ~19387, `assets/CodRevamped/lua`): Lua that adds "LAN
  MULTIPLAYER / LAN ZOMBIES" cards by setting `lobbyRoot.mainMode` and navigating to
  `LuaUtils.GetLanSelectMenu` via `Lobby.ProcessNavigate`. It is guesswork (enum fallbacks, several
  call signatures under pcall), and their own readme says the card file is "not executed". The route
  is a cheap test with our Lua exec, but it leads to the same custom-game LAN lobby we already
  reach, not to playlist tiles (the open "step 2" of the LAN-as-LIVE menu work).
- **signedInDW / Weapons menu:** they fought the same CAC/Weapons Lua-nil wall and fix it by forcing
  the `signedInDW` UI model with INT3 probes. We solved it more cleanly (DDL-instance gate, network-mode
  model detour, GameModeIsMode detour).
- **Frontend dvars** (`ColdWarFrontendDvars.cpp`): a hand list (`onlinegame`, `xblive_privatematch`,
  `lobby_open`…) plus a few obfuscated 10-letter names paired with meanings, e.g. `MNMLRKRSSL` =
  enable_cod_account. Not a hash source.
- **BO4 client** = the public shield project (`shield-crash-*.zip`, `shield_devblock_opcode`). It's already
  in our prior-art notes. Its bdMarketplace (service 80: 204 getInventoryPaginated, 245 getBalancesV3)
  is a stub, so it doesn't help B5.
- Custom camo system (D3D12 BC7 upload), ImGui overlay, Win11 one-shot patches for Alpha/Beta/S2,
  multi-instance mutex hooks, INI online/offline selector: not relevant to our build or goals.
