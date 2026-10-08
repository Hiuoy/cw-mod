# Findings imported from the two forks

Read 2026-09-22 from `cw-mod-VC2` (this repo's working tree, continued 2026-09-17/18, see its
`docs/plan.md`) and `cw-mod-final` (a from-scratch rewrite, work up to 2026-09-22, see its
`docs/FINDINGS.md` §1–§20). **No code has been ported.** This file is the knowledge only, so this
repo stops re-asking questions the forks already settled.

Status tags are the forks' own: `in-game` = seen on screen, `measured` = log/file evidence,
`static` = read from the IDB, `unproven`/`unlocated` = open. Addresses are for 1.34.0.15931218 at
the dump base (the same IDB naming as here).

## The headline

**Padlocks, storage and XP are not server questions.** Each one was traced to a local cause. That
removes most of B4 and B5 from [backend-roadmap.md](backend-roadmap.md). What is still open after
that is below, under "Still open".

## 1. Padlocks (B5): a missing lobby, not ownership

| Claim | Status | Source |
|---|---|---|
| With an unlock-all detour armed, `Entitlement_IsOwned` (0x7FF728513340) was asked **nothing** during a boot with locked tiles | `in-game` | VC2 |
| Tiles unlock when `lobby_leader_activity_changed` fires, ~2 s after `addmenu`, offline | `in-game` | VC2 (recovered from the deleted `lobby_tiles.cpp`, commit 79f5102) |
| Its only writer is `LobbyUI_RaiseLeaderActivityChanged` (0x7FF727DAF230), only caller `LobbyClient_ApplyLobbyStateUpdate` (0x7FF727250060); the raise is unconditional | `static` | VC2 |
| Online boots never fire it: 25 s idle, zero LUI events, `Session_IsSlotActive(1, 0..2)` false | `in-game` | VC2 |
| Offline, the host-launch FSM starts a type-0 session, and 314 ms later Lua's `LobbyVM_Action_StartSession` starts the type-1 lobby the tiles bind to. Online, the script never calls it | `measured` | VC2 |
| **Gate 0 passed:** a pure offline boot (`nodw = true`, `CL_Disconnect`) unlocks the tiles and loads a Zombies match | `in-game` | VC2 |
| A boot with our backend present is *not* that boot: it sets `nodw = false`, skips the `CL_Disconnect`, and no lobby is created | `static` | VC2 |
| Whether a **complete** backend would let the online frontend build its lobby | **unproven** | both. This is B6's real risk |

Lobby network mode is its own enum (0 UNKNOWN, 1 LAN/LOCAL, 2 LIVE), mapped to the session's
0/1/2/3 by `LobbyBase_NetworkModeToSessionNetworkMode` (0x7FF7281666F0). `0` leaves the lobby at
UNKNOWN and keeps the tiles padlocked. This repo already passes 1/2 ([game.cpp](../client/game/game.cpp)),
so this bug was cw-mod-final's own. Also, LOCAL (1) hides online-only UI such as Challenges on
purpose: feature predicates test `LobbyBase_GetNetworkMode() != 1`, and some are OR'd with a dvar
override (e.g. 0x7FF727C901B0).

## 2. Entitlement inventory (B5)

- `Entitlement_IsOwned` = owned iff one of the asset's product ids has nonzero quantity in the
  per-controller marketplace inventory. `static`
- The inventory is fetched by `Inventory_Frame` → `Inventory_RequestPage` (bdMarketplace), which
  waits on `MtxSync_IsDoneOrInFlight`. `static`, VC2
- **MtxSync can never finish on studio auth.** `MtxSync_BeginBnetTokenRequest` (0x7FF71E750420)
  asks the first-party layer for a Battle.net `ZEUS` token (0x5A455553). No server reply can
  supply that. `static`
- VC2 **built but never booted** the fix: set `dvar_mtxSyncEnabled` (0x7FF7371D6B18, 5 readers, all
  MtxSync) to false in `ApplyAfterScriptInit`. Then `IsDoneOrInFlight` returns 1 and `ShouldStart`
  returns 0. Expected: the first `Inventory_RequestPage` shows up in the census.

## 3. Player storage (B4): solved locally

| Claim | Status | Source |
|---|---|---|
| Store names are a flat `char*` table by data map id at 0x7FF72AF56450 (ids 0..45) | `static` | VC2 |
| Offline, maps 5 `common_progression`, 7 `mp_stats_online`, 18 `zm_stats_online`, 19 `zm_progression_online` are **never loaded**: they are dwuser-backed and dwuser is unavailable under `nodw` | `measured` | VC2 |
| Rewriting the defs dwuser → hdd makes all four load (`result=0`) | `measured` | VC2 |
| No hook is needed: the redirect is one int, `def+64` (1 dwuser → 0 hdd). `PlayerData_Init` (0x7FF7275C17C0) does that write itself when its `forceLocalStorage` dvar (off_7FF730351140) is set | `static` | final §19 |
| A late rewrite still takes: zero `store+0` (user key), and the tick runs `PlayerData_ResetControllerStore` (0x7FF7275BF690) and resubmits every location on the main thread | `static` | final §19 |
| Store layout per controller (stride 86040): `+0` user key, `+8` submitted[5], `+16` entries (336 B: `+0` mapId, `+8` def, `+32` version, `+120` ready), `+86032` count, `+86036` reset pending | `static` | final §19 |

The local `zm_*_online.cgp` files dated Jul 28–29 are retail files from real play. They are **not**
written by this project.

## 4. Why a Zombies match awards no XP

**Gate 1 is `LiveStorage_AreMatchStatsEnabled` (0x7FF7275C3DA0)**, which was misnamed
`LiveStorage_ShouldUseRawStatsBlockCopy` in this repo's memory:

```c
if (Dvar_GetBool(dvar @ 0x7FF72B4C3060)) return 1;                        // force-on override
if (IsGameMode(1) || IsGameMode(3)) return IsStatsEligibleOnlineMatch();  // mp / wz
return IsGameMode(0)                        // ZM
    && Com_SessionMode_IsOnline()           // networkMode == 2
    && !Com_SessionMode_TopNibbleIs(1);     // matchType != 1 (custom/private)
```

- Measured live packed word in Die Maschine: `0x00001010`. Both ZM conjuncts fail. `static` + `measured`
- **Online would fail too** while matchType stays 1 (private). Demonware would not fix this.
- `LiveStats_RecordMatchStatsIfEnabled` (0x7FF723BEF590) is wrapped entirely in it. It also gates
  `LiveStorage_CommitStatsTransferAndRecap`, `_CommitStatsTransfer`, `_BuildStatsTransferSlot`.
- The force-on dvar's name is unrecovered: hash `0x6AC2D9D017C1927F`. It **is** writable despite
  flag 0x400 if `g_dvarAllowServerFlaggedWrites` is held across the write (final §1).
- `Com_SessionMode_IsProgressionExemptContext` is **refuted** as the gate.

**It is not the only gate** (final §20, `in-game` 2026-09-22): redirect + forced gate plays two full
Zombies matches with no drop, but there are no XP popups and no `zm_*_online` file is written. On
entering Zombies the frontend switches the session to LOCAL (`packed 0x1000 → 0x1010`), and the match
writes `zm_loadouts_offline` instead. **Guess, not proven:** LOCAL selects the `_offline` data-map
set. Where that set is chosen is unlocated.

Forcing the gate *without* the redirect drops the match mid-load (`West 683 Winning Clover`,
`connState=7`, the stats handshake), because the stats-transfer slot copies dwuser maps that never
loaded. `in-game`, final §17.

## 5. Identity on an offline boot

- Every per-user subsystem (inventory, loadouts, storage, presence, identity) is ticked inside
  `LiveUser_UpdateSigninState` (0x7FF729720390) behind `LiveUser_IsOnlineReady`, the same gate as
  our B1. It needs `Dw_GetConnectionState == 2`, so offline it never runs. `static`
- Result: no player name and first-run EULA/brightness prompts on every boot. `in-game`
- Offline-name path: `LiveUser_SetOfflineLocalName` (0x7FF729753920) → `Player1`, reached only from
  `LiveUser_OnSigninStateChange_SetupIdentity` (0x7FF729753500). final reports the name fixed (§16)
  and that it was **not** the drop's cause.

## 6. Joining

- The engine has a server-browser join: `LobbyClient_JoinServerEntry` (0x7FF7280ED250), called from
  Lua via `LuiNative_JoinServerEntry` (0x7FF71E933E50, native hash `0x424003824075FD13`). Entry
  fields: name `+8`, address `+96` (our join writes the adr at `+97`; `+96` is probably a length
  byte, unchecked), secid `+181`, seckey `+189`. That is exactly what a `CWJOIN1` blob carries. `static`
- InfoResponse does **not** fill a list; it sets one pending target. `static`
- **Who fills the browser list is unlocated.** Candidate: DW matchmaking results. A guess.
- **There is no native LAN discovery to revive** (2026-09-22, `static`). `g_netMsgHandlers`
  (0x7FF72A392110) maps msgId 22 `ServerlistInfo` to a thunk into `mov al,1; ret` (0x7FF727B27BB0);
  nothing writes msgId 22; there is no `bdLANDiscovery` in the image. msgId 26 `AnnounceHost`
  (`NetMsg_SendAnnounceHost`, 0x7FF7275C7430) is host migration (`lobbytype`, `timeout`), not an advert.
  So cw-mod broadcasts the CWJOIN1 descriptor itself: `client/game/lan_browser.cpp`, UDP 28970, listed
  and joined from the Session tab. Built, not yet run on two PCs.
- ~~`JoinPendingTarget` passes `&...7A4` as the address; our join writes `7A5`.~~ Settled: the engine's
  own `ClientSession_SetPendingJoinTarget_FromDescriptor` writes addrbuff at `0x7FF7304377A5`, and
  `AddHostCandidate` reads `a5+1`. `7A5` is right. `static`

## 7. Rules the forks learned the hard way

- Never call `Dvar_GetBool`/`Dvar_GetInt` from the injected module. They are Arxan-flattened thunks
  that dispatch on the return address, and it was a boot crash. Write dvars, track what you wrote.
- Without an Arxan bypass, any engine detour crashes the boot (final §15b). This repo has the bypass;
  cw-mod-final does not.
- `West 683` / `Sail 630` are generated codenames, not static strings. Hashing them recovers
  nothing, and `ErrorQueue_Push` stays silent for these drops (final §12).

## Still open

1. Where the offline vs online data-map set is chosen per session mode (final's next step). Then:
   does session network mode 2 with the lobby left at LOCAL satisfy the stats gate natively?
2. Whether a complete backend builds a lobby in online mode (B6).
3. Who fills the server-browser list.
4. `Sail 630 Nuclear Bug`'s raiser.
5. Carried over unchanged from this repo: B2's service map / census, B3's LPC-list boot, the GSC
   string-literal fixup, and the force-on dvar's name.
