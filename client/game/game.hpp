#pragma once
#include "common.hpp"
#include "game/function_types.hpp"
#include "memory/scanned_result.hpp"

#include <atomic>

namespace Client {
	namespace Game {
		class Pointers {
		public:
			explicit Pointers();

			Functions::BB_AlertT* m_BB_Alert{};
			Functions::CL_DisconnectT* m_CL_Disconnect{};
			Functions::CL_DrawTextPhysicalT* m_CL_DrawTextPhysical{};
			Functions::Com_SessionMode_SetNetworkModeT* m_Com_SessionMode_SetNetworkMode{};
			Functions::Dvar_SetBoolFromSourceT* m_Dvar_SetBoolFromSource{};
			Functions::Dvar_SetIntFromSourceT* m_Dvar_SetIntFromSource{};
			Functions::Dvar_StringToValueT* m_Dvar_StringToValue{};           // ArxanCall thunk (ResolveLuaApi)
			Functions::Dvar_ApplyValueInternalT* m_Dvar_ApplyValueInternal{}; // ArxanCall thunk (ResolveLuaApi)
			Functions::Dvar_ShowOverStackT* m_Dvar_ShowOverStack{};
			void* m_Dw_GetLoginFlow{};                            // dump 0x7FF729DEBF90 (MinHook target)
			void* m_Login_SetStatus{};                            // dump 0x7FF729DEC4D0 (MinHook target, diagnostics)
			void* m_DwLogin_BuildStudioToken{};                   // dump 0x7FF729DECA90 (MinHook target, studio-JWT diagnostics)
			void* m_LuiError_ReportFatal{};                       // dump 0x7FF7222C2340 (MinHook target, keeps a raising menu from killing the game)
			void* m_LUI_RunFile{};                                // dump 0x7FF727B8CCD0 (MinHook target, skips a missing ui/ffotd chunk)
			void* m_DecryptString{};                              // dump 0x7FF729550AE0 (MinHook target, "ui_text" replacements)
			void* m_Scr_ConstructMessageString{};                 // dump 0x7FF7284000B0 (MinHook target, script print mirror)
			// The openmenu replay, for the overlay's LUI menus tab. See kDump_g_luiCtx.
			void** m_g_luiCtx{};                                  // dump 0x7FF7305B2F38 - *m_g_luiCtx is the lua_State
			Functions::LUI_DispatchAddMenuEventT* m_LUI_DispatchAddMenuEvent{};
			Functions::LUI_GetRootNameT* m_LUI_GetRootName{};
			Functions::CL_LocalClientToControllerT* m_CL_LocalClientToController{};
			Functions::UI_SetUiActiveT* m_UI_SetUiActive{};
			// Opening with params (OpenLuiOverlay). The two lapi entries are ArxanCall thunks; the
			// protected-call wrapper is the raw engine function (it has no caller guard).
			Functions::lua_createtableT* m_lua_createtable{};
			Functions::lua_setfieldT* m_lua_setfield{};
			Functions::LUI_ProtectedCallT* m_LUI_ProtectedCall{};
			// cw-mod/ui_scripts (game/ui_scripts.cpp). ArxanCall thunk.
			Functions::lua_loadT* m_lua_load{};
			Functions::LiveUser_GetUserDataForControllerT* m_LiveUser_GetUserDataForController{};
			Functions::LobbyBase_SetNetworkModeT* m_LobbyBase_SetNetworkMode{};
			Functions::LobbyUI_SetTargetMenuAndNotifyT* m_LobbyUI_SetTargetMenuAndNotify{};
			Functions::Unk_SetUsernameT* m_Unk_SetUsername{};

			// --- Debug tab: dvars by NAME (no dev console) -----------------------------------------
			// All AOB-resolved, so these survive a build bump. See the Debug tab in menu.cpp.
			Functions::Dvar_FindVarT* m_Dvar_FindVar{};
			// --- Lua 5.1 C API (ALL of these point at ArxanCall thunks, never at the engine) ------
			// Resolved by RVA in ResolveLuaApi, then wrapped. Calling the raw addresses from our
			// module makes the table accessors silently no-op (see arxan_call.hpp), which would look
			// like "the registry is empty" rather than like a failure - so there is deliberately no
			// stored pointer to the unwrapped function.
			Functions::lua_getfieldT*  m_lua_getfield{};
			Functions::lua_gettableT*  m_lua_gettable{};
			Functions::lua_rawgetiT*   m_lua_rawgeti{};
			Functions::lua_nextT*      m_lua_next{};
			Functions::lua_pushvalueT* m_lua_pushvalue{};
			Functions::lua_typeT*      m_lua_type{};
			Functions::lua_tolstringT* m_lua_tolstring{};
			Functions::lua_tonumberT*  m_lua_tonumber{};
			Functions::lua_getmetatableT* m_lua_getmetatable{};
			// The debug half, used only by the luaL_traceback detour to describe an erroring call
			// stack. Thunked like the rest; null on a build mismatch, and the detour then reports
			// that instead of printing an empty frame list.
			Functions::lua_getstackT*  m_lua_getstack{};
			Functions::lua_getinfoT*   m_lua_getinfo{};
			// The MinHook target, NOT a thunk - hooking wants the real entry point. The detour makes
			// its own thunk for the trampoline when it needs to call through.
			void* m_luaL_traceback{};                             // dump 0x7FF729E48320

			// Also a MinHook target, and also unwrapped. Hooked to repair the ar.name bug that kills
			// the process on any hashed-name Lua error - see dump_anchors.hpp for the full chain.
			void* m_luaG_getobjname{};                            // dump 0x7FF729E476D0
			// The exact .rdata literal luaG_getobjname returns on the broken path. Comparing against
			// this pointer identifies the case precisely; matching on the string's TEXT would also
			// catch the sibling "xhashfunc" return, which writes nameOut correctly and must be left
			// alone. Null if it did not resolve, and the detour then does nothing.
			const char* m_luaG_getobjname_NoNameResult{};

			// The dvar registry itself: 1024 buckets, bucket index = hash & 0x3FF, each a singly
			// linked list threaded through Dvar+8. Walking it enumerates every registered dvar on
			// THIS build — which is the only trustworthy source of dvar hashes (see DumpAllDvars).
			std::uintptr_t* m_g_dvarHashTable{};

			std::uintptr_t** m_Dvar_NoDW{};
			// The latch that makes a flag-0x400 (server-authoritative) dvar writable from the main
			// thread. Without it Dvar_ApplyValueInternal drops the write silently. WriteDvarBool/Int
			// hold it across the set and restore the previous byte. See kDump_g_dvarAllowServerFlaggedWrites.
			std::uint8_t* m_g_dvarAllowServerFlaggedWrites{};
			std::uint32_t* m_g_sessionModePacked{};              // dump 0x7FF73561C7F8 — packed session-mode word
			// LiveUser_GetObject is one load out of this array; a null entry means no signed-in user.
			std::uintptr_t* m_g_liveUserObjects{};               // dump 0x7FF72ADC3390
			// Read-only diagnostic: master gate for the LiveUser update loop. If 0, LiveUser_LoginDriver_Tick
			// never runs, so Demonware login never even begins. See DumpDwLoginState. dump 0x7FF72AAE018D.
			std::uint8_t* m_g_liveUserSystemActive{};
			// Login-driver state-1 gates, read by DumpDwLoginState, plus the presence hook's target and
			// its return-address window. See dump_anchors.hpp.
			std::uintptr_t** m_Dvar_LiveConnectMode{};           // dump 0x7FF733D20100 — dvar_t* slot, gate B
			std::uint8_t* m_g_liveUserLoginAllowed{};            // dump 0x7FF7371D75D7 — LiveUser_GetLoginAllowedFlag byte
			void* m_LiveUser_FirstPartyPresenceOk{};             // dump 0x7FF7296B79B0 — MinHook target (RA-guarded)
			std::uintptr_t m_LiveUser_LoginDriver_TickBase{};    // dump 0x7FF727F39160 rebased — RA-guard window base
			std::uintptr_t m_LiveUser_SetupIdentityBase{};       // dump 0x7FF729753500 rebased — presence deny window
			std::uintptr_t m_LiveUser_AccountIdPickersBase{};    // dump 0x7FF7296AB2F0 rebased — presence deny window (Bnet account id)
			void* m_bdCommonAddr_Ctor{};                         // dump 0x7FF729D373D0 (MinHook target, LAN-shape own address on LIVE)
			const void* m_g_bdAddrEmpty{};                       // dump 0x7FF73753F5E0 - the engine's empty bdAddr
			std::uintptr_t** m_g_firstPartyManager{};            // dump 0x7FF7371BF208 — mgr ptr slot (probe read-only)
			void* m_FirstParty_GetSession{};                     // dump 0x7FF7297DF050 — MinHook target (null-session census)
			void* m_FirstParty_GetLocalUserIndex{};              // dump 0x7FF7297D9EE0 — MinHook target (returns 0 = controller 0)
			void* m_g_firstPartyManagerObj{};                    // dump 0x7FF7371DA520 — the manager object; +24 is the session
			void* m_LiveUser_SignOutBuildDropMessage{};          // dump 0x7FF729753740 — MinHook target (the ERR_DROP gate)
			// B3 LPC list diagnostics (read-only MinHook targets, DwBackend only). See dump_anchors.hpp.
			void* m_BdRemoteHttpTask_FinishRow{};
			void* m_PublisherObjectsResource_Parse{};
			void* m_ObjectMetadata_ParseJson{};
			void* m_Lpc_WriteManifest{};
			void* m_Lpc_OnListFailed{};
			void* m_BdLobbyMsg_WriteHeader{};                    // dump 0x7FF729D98EC0 — lobby request census
			// B5 MtxSync transcript (read-only MinHook targets, DwBackend only). See dump_anchors.hpp.
			void* m_MtxSync_ShouldStart{};
			void* m_MtxSync_RequestBnetTokenZEUS{};
			void* m_MtxSync_OnBnetToken{};
			void* m_MtxSync_IsDoneOrInFlight{};
			const void* m_g_mtxSyncBackoff{};
			const void* m_g_mtxSyncState{};
			void* m_DwFetch_GetStatus{};                         // dump 0x7FF7262A4BA0 — B6 lobby gate transcript
			void* m_DwFetch_IsDone{};                            // dump 0x7FF7262ACBE0 — MinHook target (lobby waiver, RA-guarded)
			std::uintptr_t m_Lua_IsDemonwareFetchingDone_ImplBase{}; // dump 0x7FF72534AE20 rebased — the waiver's RA window
			void* m_LiveUser_IsTrial{};                          // dump 0x7FF728151A00 — MinHook target (full-game answer)
			// Engine.PrintInfo/Warning/Error wrappers — MinHook targets (Lua print transcript).
			void* m_LuaNative_PrintInfo{};
			void* m_LuaNative_PrintWarning{};
			void* m_LuaNative_PrintError{};
			void* m_LuaNative_ContentIsFullyInstalled{};
			void* m_LuaNative_ContentIsPlayable{};
			void* m_LuaNative_ContentIsInstalling{};
			void* m_LuaNative_GetDvarInt{};
			void* m_LuaNative_IsKoreaMinor{};
			void* m_LuaNative_IsLobbySlotLive{};
			// Battle.net fatal-error reporters. MinHook targets, live only during an online-mode test
			// boot — see dump_anchors.hpp for why level-1024 Com_Error is unsurvivable.
			void* m_BnetError_ReportFatalUnguarded{};            // dump 0x7FF7296B7C70 — the one that actually fires
			void* m_BnetError_ReportFatalIfSignedIn{};           // dump 0x7FF7296B7CF0 — latch-guarded sibling
			std::uint8_t* m_g_bnetEverSignedInLatch{};           // dump 0x7FF7371BF272 — sticky, read-only diagnostic
			// The two writers of first-party STATE_ERROR. Hooking these is what actually stops the
			// BLZBNTBGS popup — the Com_Error reporters above are only one of its consumers.
			void* m_FirstParty_SetError{};                       // dump 0x7FF7297DF340 — (obj, code)
			void* m_FirstParty_SetErrorState{};                  // dump 0x7FF7297DE380 — (obj), codeless
			void* m_FirstParty_OnBgsDisconnected{};              // dump 0x7FF7297DF2E0 — the writer that actually fires
			// ...and the one that actually matters. The popup has three producers, only one of which
			// is a BnetError reporter; LiveUser_HandleSignOut reaches it without ever touching the two
			// above. They all funnel through this queue, so this is where the dialog really dies.
			void* m_ErrorQueue_Push{};                           // dump 0x7FF722349D60 — (level, msg, flag)
			// ...nor was that it. The producer is a sign-in watchdog in LiveFirstParty_Frame that
			// calls Com_Error(1024) directly, bypassing the queue AND the reporters.
			void* m_LiveUser_ForceSignOutAndFatal{};             // dump 0x7FF727F3A320 — no args
			std::uintptr_t** m_dvar_com_maxclients{};
			int* m_g_svMaxClients{};

			// --- PlayerData anchors: TEMPORARY, for the dwuser -> hdd redirect hooks ----------------
			// Delete with those hooks once the local backend serves bdStorage (backend roadmap B4).
			// Why these exist: ZM upgrades are NOT locked by the session networkMode, they are locked
			// by the PlayerData storage backend. Each data map's def carries a storageLocation, and
			// location 1 is "dwuser" — Demonware user storage, whose availability predicate
			// (PlayerDataStorage_DwUser_IsAvailable, dump 0x7FF7283FFD20) requires a signed-in Live
			// user, a connected Demonware, privileges AND networkMode==2. Offline it fails, so
			// PlayerData_ResetBufferToDefaults never sets entry+120 = 1 (LOADED),
			// PlayerData_IsBufferReady returns false, and every progression read comes back empty.
			// PROVISIONAL RVA binding like the lever anchors below — build-locked to 1.34.0.15931218.
			void** m_g_playerDataDefsById{};        // dump 0x7FF73037BA60 — def ptr per dataMapId, valid ids 1..0x2D
			std::uint8_t* m_g_playerDataStore{};    // dump 0x7FF730351A10 — per-controller buffer store, 86040/controller
			// Per-data-map callback table, 32 bytes per dataMapId, four slots:
			//   [0] read-complete hook   [1] second read-complete hook
			//   [2] write-complete hook  [3] userdata getter
			// Slots [0] and [1] are the VETO: PlayerData_OnStorageOpComplete calls each with the
			// result code and, if either returns false, puts entry+120 back to 0 and arms a retry.
			// Read-only here, purely so a vetoed completion can name the function that vetoed it.
			void** m_g_playerDataMapCallbacks{};    // dump 0x7FF73037BBD0
			Functions::PlayerData_ResetBufferToDefaultsT* m_PlayerData_ResetBufferToDefaults{};
			// The async storage-read completion — the actual writer of entry+120 on the load path,
			// and therefore what decides when a data map becomes readable. MinHook target only.
			void* m_PlayerData_OnStorageOpComplete{};
			// The seam that actually fires. See dump_anchors.hpp: the init one is installed too late
			// on this build, the tick is self-timing. MinHook target only.
			void* m_PlayerData_ControllerStorageTick{};
			// --- Phase 3 lever-1 (loopback co-op) anchors ---------------------------------------
			// PROVISIONAL binding: resolved by module-base + RVA for the pinned build
			// 1.34.0.15931218 (dump imagebase 0x7FF71CBC0000; see docs/phase3_netcode.md). These are
			// NOT AOB signatures yet — the IDB was unavailable when they were wired, so the byte
			// patterns could not be read. TODO: convert every RVA below to a robust AOB signature
			// once the IDB is back, then delete the RVA path. Until then these are build-locked.
			//
			// SV_DirectConnect and the loopback reseat are stored as raw void* on purpose: their exact
			// call ABIs are not reversed yet, so there is deliberately no callable typedef to invoke
			// them by accident. Seating a 2nd player calls into these and stays gated (see Coop* below).
			void* m_SV_DirectConnect{};                    // dump 0x7FF723D4AB60 — OOB connect handler / join path
			void* m_SV_Migration_ReseatClient_Loopback{};  // dump 0x7FF71FD49410 — in-process seat (host migration)
			void** m_g_svClients{};                        // dump 0x7FF72DA75B00 — global holding the slot-array base ptr
			bool* m_sv_migrationInProgress{};              // dump 0x7FF72D672950 — host-migration-in-progress flag

			// Loopback-seat call path (Option B, surgical). All resolved by dump-abs - imagebase.
			void* m_SV_StageConnectMessage{};              // dump 0x7FF72669E1F0 — push connect userinfo onto OOB redirect stack
			void* m_SV_UnstageConnectMessage{};            // dump 0x7FF72669E1A0 — pop it
			void* m_Com_SessionMode_GetString{};           // dump 0x7FF728D7BD60 — current sessionmode string (for netfield/sessionmode userinfo)
			int* m_g_netFieldChecksum{};                   // dump 0x7FF736EE2EA4 — netfieldchk value SV_DirectConnect requires (plain global)
			std::uint16_t* m_qportCounter{};               // dump 0x7FF72D7C0AC4 — engine qport counter (read+inc for a unique qport)

			// --- Phase 3 lever-3 (host-launch capture) — read-only diagnostic anchors -------------
			// Latched by HostSession_StartLaunch (dump 0x7FF727B2A790) when a stock private match starts.
			// We read them back to clone the launch args (see docs/phase3_netcode.md, the GSC/LobbyVM
			// trigger section). Pure reads on a persistent global — no engine calls, safe from any thread.
			std::uint8_t* m_hostLaunchBlock{};             // dump 0x7FF73254EAC0 — latched StartLaunch param block (0x140 bytes)
			int* m_g_hostLaunchPhase{};                    // dump 0x7FF73254F5E8 — host-launch FSM phase (0 idle,1 kicked,40 done,41 err)
			int* m_g_localPlayerCount{};                   // dump 0x7FF7323AF2A8 — local player count (1 solo / 2 splitscreen)

		// LAN announce/search state anchors (read-only). Lets us confirm on ONE PC whether the host lobby
		// created by TriggerSystemlinkHostLaunch is actually advertising over LAN. See DumpSessionState.
		void** m_g_netSessionManager{};                // dump 0x7FF73753F5C8 — global holding the NetSession manager ptr (null until a session exists)
		int* m_g_netSessionLaunchState{};              // dump 0x7FF72ADC7CA0 — launch-state enum the pump drives (5->6, 7->8)

			// GSC-facing launch wrappers (top of the native chain). We hook these to capture the clonable
			// wrapper-level args of a stock private-match start — the StartLaunch config blob itself embeds
			// live pointers and can't be replayed, so we clone the inputs and let the game rebuild it.
			void* m_GScr_LaunchP2P{};                      // dump 0x7FF727A99000 — GScr_LobbyHost_LaunchPrivateP2P
			void* m_GScr_LaunchP2P_Named{};                // dump 0x7FF727A990F0 — ..._Named
			void* m_GScr_LaunchP2P_Full{};                 // dump 0x7FF727A99190 — ..._Full

			// The native launch entrypoint we REPLAY through (rebuilds config + istrings internally, so we
			// only feed captured scalars + two persistent config-string ptrs). See TriggerSystemlinkHostLaunch.
			void* m_LobbyHost_LaunchP2P_Plain{};           // dump 0x7FF72614EE00 — LobbyHost_LaunchPrivateP2P_Plain

			// --- Phase 3 lever-4 (CLIENT/PC2 join) ------------------------------------------------
			// The mirror of the host chain. ClientSession_JoinKnownHost is a one-shot "join THIS host":
			// StartJoin + AddHostCandidate + JoinKick in a single call, and it takes the host address
			// directly, so it needs no Demonware search result. See docs/phase3_netcode.md.
			//
			// IMPORTANT (from the decompile): AddHostCandidate dereferences three of its arguments with
			// NO null checks — xuid (__int64*, 8 bytes), netadr (_OWORD*, 16 bytes) and an 85-byte blob
			// read at a5+1..a5+85. They cannot be hand-fabricated blind; we capture a real join first and
			// replay the captured buffers, exactly as we did for the host launch.
			void* m_ClientSession_JoinKnownHost{};         // dump 0x7FF727946AD0 — one-shot join-this-host (lever-4 trigger)
			void* m_ClientSession_StartJoin{};             // dump 0x7FF727946640 — mirror of HostSession_StartLaunch
			void* m_JoinCtx_AddHostCandidate{};            // dump 0x7FF727946520 — the netadr injection point
			void* m_ClientSession_JoinKick{};              // dump 0x7FF727FC0470 — zero-arg thunk that pumps the FSM
			std::uint8_t* m_g_clientJoinCtx{};             // dump 0x7FF730430DA0 — join FSM context (state@+0)
			int* m_cl_lobbyLaunchState{};                  // dump 0x7FF73050A740 — client lobby->connect FSM state

			// g_clientJoinCtx layout, all verified against the StartJoin / AddHostCandidate decompiles.
			// The 12400-byte memset at ctx+40 in StartJoin is exactly 50 * 248, which pins both the
			// candidate-array base and the stride.
			static constexpr std::size_t kJoinCtx_State        = 0;      // int: 0 idle,1 pick,2 send,3 await resp,4 agree,5 next,6 done,7 teardown
			static constexpr std::size_t kJoinCtx_ActionId     = 4;      // int
			static constexpr std::size_t kJoinCtx_ControllerIdx= 16;     // int
			static constexpr std::size_t kJoinCtx_SrcLobby     = 20;     // int
			static constexpr std::size_t kJoinCtx_DstLobby     = 24;     // int
			static constexpr std::size_t kJoinCtx_Candidates   = 40;     // host candidate array base
			static constexpr std::size_t kJoinCtx_HostCount    = 12440;  // int, CAP 50
			static constexpr std::size_t kJoinCtx_HostIdx      = 12444;  // int: which candidate the FSM is on
			static constexpr std::size_t kJoinCtx_CurrentHost  = 12456;  // 248-byte copy of the candidate being tried
			static constexpr std::size_t kJoinHost_Stride      = 248;
			static constexpr std::size_t kJoinHost_MaxCount    = 50;
			// Offsets WITHIN one 248-byte host-candidate entry. Field IDENTITIES come from the matched
			// serializer/deserializer pair Session_ParseJoinDescriptor (0x7FF726BFADE0) /
			// Session_WriteJoinRequestMsg (0x7FF726BF8460), and are corroborated by
			// ClientSession_JoinPendingTarget passing &g_pendingJoinTarget_{secid,seckey,serializedadr}
			// into these same three slots. Earlier labels here (blob/xuid/netadr) were guesses and wrong.
			static constexpr std::size_t kJoinHost_SessionId     = 0;      // __int64
			static constexpr std::size_t kJoinHost_Name          = 8;      // char[36]
			static constexpr std::size_t kJoinHost_HostType      = 44;     // char[36] "dedicated"/"listen"
			static constexpr std::size_t kJoinHost_SerializedAdr = 97;     // 84 bytes — the PORTABLE host address
			static constexpr std::size_t kJoinHost_SecId         = 181;    // 8 bytes  (was mislabelled xuid)
			static constexpr std::size_t kJoinHost_SecKey        = 189;    // 16 bytes (was mislabelled netadr)

			// --- Phase 3 lever-4, THE NATIVE PATH (no capture) ------------------------------------
			// The client join takes exactly ONE input: the host's XUID. ClientSession_QueryHostByXuid
			// sends netmsg 0 (InfoRequest) addressed by XUID; the host answers netmsg 1 (InfoResponse)
			// carrying the whole join descriptor, which the engine parses into g_pendingJoinTarget_*;
			// ClientSession_JoinPendingTarget then does StartJoin + AddHostCandidate + JoinKick off
			// those globals. Nothing is fabricated and nothing needs capturing — the address arrives
			// from the host. Arg shapes are from the Lua natives that drive both (0x7FF71E937820 and
			// LuiNative_JoinPendingTarget 0x7FF71E933F10). See docs/phase3_netcode.md.
			void* m_ClientSession_QueryHostByXuid{};       // dump 0x7FF727947190 — (joinCtxId, controllerIdx, hostXuid)
			// arg4 is jointype, NOT a lobby type — it is forwarded to JoinCtx_AddHostCandidate arg6,
			// stored at dword_7FF730437700 (= joinCtx+26976), read by the FSM in state 2 and written to
			// the JoinLobby request at +8. The host refuses anything but 1 or 4 with verdict 36.
			void* m_ClientSession_JoinPendingTarget{};     // dump 0x7FF727946A30 — (joinCtxId, controllerIdx, unused, jointype)
			void* m_Session_GetSessionObject{};            // dump 0x7FF726F82D20 — (type, idx<3) -> live session object

			// g_pendingJoinTarget_* — normally filled by ClientSession_SetPendingJoinTarget_FromDescriptor
			// when the InfoResponse lands. ClientSession_JoinPendingTarget reads EXACTLY the eight marked
			// [J] below and nothing else, so the whole struct can also be filled by hand from what PC1
			// prints — which is what the descriptor path does when the XUID probe goes unanswered.
			bool* m_pendingJoin_awaiting{};                // dump 0x7FF730437720 — set while an InfoRequest is outstanding
			int*  m_pendingJoin_nonce{};                   // dump 0x7FF730437724 — must match in the reply
			std::int64_t* m_pendingJoin_xuid{};            // dump 0x7FF730437728 — [J] host XUID
			bool* m_pendingJoin_valid{};                   // dump 0x7FF730437738 — [J] gate: 1 once the descriptor is in
			std::int64_t* m_pendingJoin_sessionId{};       // dump 0x7FF730437748 — [J] entry+8, which the host fills
			                                               //     from sessionObj+168 — i.e. the host XUID again
			std::uint8_t* m_pendingJoin_hostName{};        // dump 0x7FF730437750 — [J] char[36]
			int*  m_pendingJoin_slot{};                    // dump 0x7FF730437774 — [J] lobby slot index 0..2
			std::uint8_t* m_pendingJoin_secId{};           // dump 0x7FF73043778C — [J] 8 bytes
			std::uint8_t* m_pendingJoin_secKey{};          // dump 0x7FF730437794 — [J] 16 bytes
			std::uint8_t* m_pendingJoin_serializedAdr{};   // dump 0x7FF7304377A5 — [J] 84 bytes

			// Live session-object fields, from Session_BuildSendInfoResponse: it copies these straight
			// into the descriptor it puts on the wire. This is where the host reads its own identity.
			static constexpr std::size_t kSessObj_MemberCount   = 68;   // int, > 0 means the slot is live
			static constexpr std::size_t kSessObj_HostXuid      = 168;  // __int64 — THE ONE INPUT PC2 NEEDS
			static constexpr std::size_t kSessObj_HostName      = 176;  // char[36]
			static constexpr std::size_t kSessObj_SerializedAdr = 265;  // 84 bytes
			static constexpr std::size_t kSessObj_SecId         = 349;  // 8 bytes
			static constexpr std::size_t kSessObj_SecKey        = 357;  // 16 bytes
			// Lobby details, from Session_BuildLobbyDescriptor (0x7FF727F1EB70) and
			// LobbyUI_PushGameSettings (0x7FF727B26C50). Read-only, for the LAN browser.
			static constexpr std::size_t kSessObj_MaxClients    = 376;  // int; 127 while the slot is idle
			static constexpr std::size_t kSessObj_ClientSlots   = 400;  // [127], stride 64 (Session_GetClientSlot)
			static constexpr std::size_t kSessObj_ClientStride  = 64;
			static constexpr int         kSessObj_ClientCount   = 127;
			static constexpr std::size_t kClientSlot_Present    = 16;   // u64, non-zero = occupied
			static constexpr std::size_t kClientSlot_Client     = 24;   // LobbyClient*
			static constexpr std::size_t kLobbyClient_Gamertag  = 0x441; // char[64] (LobbyClient_GetGamertag)
			static constexpr std::size_t kSessObj_Gametype      = 9129; // char[], game settings block +33
			static constexpr std::size_t kSessObj_MapName       = 9176; // char[], game settings block +80

			// --- Phase 3 lever-5: THE NETMSG TRANSCRIPT -------------------------------------------
			// Every lobby netmsg is dispatched by ONE function. NetMsg_Dispatch linearly scans a
			// 14-entry {u64 msgId, u64 handler} table and tail-calls the match:
			//
			//     v4 = &g_netMsgHandlers;                    // 0x7FF72A392110
			//     while (*(int*)(msg + 64) != *v4) {         // msg+64 IS the msgId
			//         ++i; v4 += 4;                          // _DWORD* += 4 == one 16-byte entry
			//         if (i >= 0xE) return 0;                // 14 entries, then drop
			//     }
			//     handlers[2 * i](localClient, &netadrCopy, a3, msg);
			//
			// Hooking it yields the complete inbound message log on BOTH machines, and — crucially —
			// it reads the payload AFTER transport decryption, which the packet capture cannot do:
			// frames 7/8 of the handshake are ciphertext on the wire. msg+64 is confirmed by
			// NetMsg_WriteHeader, which writes *(int*)(msg + 64) = msgId on the send side.
			void* m_NetMsg_Dispatch{};                     // dump 0x7FF726BF9370 — (localClient, netadr*, a3, msg)
			void** m_g_netMsgNames{};                      // dump 0x7FF72ADEF4E0 — 35 msgId -> const char*
			void* m_g_netMsgHandlers{};                    // dump 0x7FF72A392110 — 14 * {u64 msgId, u64 handler}
			static constexpr std::size_t kNetMsg_MsgIdOffset = 64;   // int, within the msg object
			static constexpr std::size_t kNetMsg_NameCount   = 35;
			static constexpr std::size_t kNetMsg_HandlerCount= 14;

			// The host's admission decision. NetMsg_Handle_JoinLobby (msgId 16, table entry 0) builds a
			// 96-byte JoinResponse struct whose FIRST DWORD is the verdict, then hands it to
			// NetMsg_SendJoinResponse (msgId 17) as the 6th argument. 1 == accepted; every other value is
			// a specific refusal. Hooking the SENDER is the cheapest read: one argument, one dword, and it
			// fires exactly once per join attempt. See NetMsgJoinResponseLabel.
			void* m_NetMsg_SendJoinResponse{};             // dump 0x7FF726BF82C0 — (a1,a2,a3,netadr*,a5, respStruct)

			// The JoinLobby request parser, `bool(block, msg)`. Field names below are the literal wire
			// keys it reads; the offsets are its own writes. Every gate in NetMsg_Handle_JoinLobby is a
			// comparison against one of these, so logging the block after the parse turns "verdict 36"
			// into "networkmode was N, and the host wanted 1".
			void* m_Session_ParseJoinLobbyRequest{};       // dump 0x7FF726BFB070
			static constexpr std::size_t kJoinReq_TargetLobby     = 0;      // int
			static constexpr std::size_t kJoinReq_SourceLobby     = 4;      // int
			static constexpr std::size_t kJoinReq_JoinType        = 8;      // int   — must be 1 or 4, else verdict 36
			static constexpr std::size_t kJoinReq_MemberCount     = 44;     // int
			static constexpr std::size_t kJoinReq_SplitScreen     = 4112;   // int
			static constexpr std::size_t kJoinReq_PlaylistId      = 4116;   // int
			static constexpr std::size_t kJoinReq_PlaylistVer     = 4120;   // int   — verdict 38/39
			static constexpr std::size_t kJoinReq_PlaylistChecksum= 4124;   // int   — verdict 40
			static constexpr std::size_t kJoinReq_TuVersion       = 4128;   // int
			static constexpr std::size_t kJoinReq_FfotdVer        = 4132;   // int   — verdict 43/44
			static constexpr std::size_t kJoinReq_NetworkMode     = 4136;   // int   — must be 1, else verdict 36
			static constexpr std::size_t kJoinReq_NetChecksum     = 4140;   // int   — verdict 42
			static constexpr std::size_t kJoinReq_Protocol        = 4144;   // int
			static constexpr std::size_t kJoinReq_Changelist      = 4148;   // int
			static constexpr std::size_t kJoinReq_JoinNonce       = 4176;   // __int64
			static constexpr std::size_t kJoinReq_FeatureChecksum = 4200;   // int   — verdict 45
			static constexpr std::size_t kJoinResp_Code    = 0;    // int   — THE VERDICT
			static constexpr std::size_t kJoinResp_Name    = 4;    // char[36]
			static constexpr std::size_t kJoinResp_Location= 40;   // char[36]
			static constexpr std::size_t kJoinResp_LobbyType=76;   // int
			static constexpr std::size_t kJoinResp_NetworkMode=80; // int
			static constexpr std::size_t kJoinResp_MainMode= 84;   // int
			static constexpr std::size_t kJoinResp_ReservationKey=88; // __int64

			// Client-side landing spot for the verdict. NetMsg_Handle_JoinResponse stores the received
			// code here before switching on it (case 1 -> accept, case 47 -> retry, default -> state 5).
			int* m_g_joinResponseCode{};                   // dump 0x7FF730437710
			// The netadr the client compares the JoinResponse SOURCE against before accepting it. If this
			// comparison fails the handler returns without ever touching the FSM state — which is the one
			// path that explains leaving state 3 without landing on 5. See docs/phase3_test_plan.md.
			std::uint8_t* m_g_expectedHostAdr{};           // dump 0x7FF730433E98 — 16-byte netadr

			bool* m_Scr_Initialized{};
			T9::ContentManager** m_unk_ContentManager{};
			char* m_unk_Config_1{};
			char* m_unk_Config_2{};
			T9::Font_s** m_unk_WatermarkFont{};

			Functions::RtlDispatchExceptionT* m_RtlDispatchException{};

			void PatchAuth();
			// `lobbyLive` overrides the lobby-enum choice that `mode` would otherwise imply: -1 derives
			// it (online -> LIVE, everything else -> LAN), 0 forces LAN, 1 forces LIVE. The override
			// exists so the boot marker can request networkMode 2 with a LAN lobby, which is a much
			// smaller slice of the live stack than full LIVE. See Boot::Profile (boot_profile.hpp).
			// `menu` < 0 leaves the lobby UI target alone, so the frontend opens on the title screen.
			void SetMode(int mode, int menu, int lobbyLive = -1);

			// --- Name hashing, dvars and LUI menus (the "no dev console" toolkit) ----------------
			//
			// T9 stores no dvar or menu name strings — everything is keyed by a 64-bit hash. This is
			// THE hash function (Com_HashString, RVA 0x1D56710): FNV-1a-64 over the name with ASCII
			// A-Z lowercased, masked to 63 bits. Verified two ways against build 1.34.0.15931218:
			//   * H("lobby") == 0x1CCAE3B6EF72F533, the literal the engine passes as the "menu"
			//     parameter of its own addmenu event;
			//   * H("r_fog"), H("cg_fov"), H("r_mode"), H("com_maxclients") all appear as the
			//     `mov rcx, <imm64>` immediately before their Dvar_Register call sites.
			// Reimplemented here rather than called: it is a leaf with no state, and calling into
			// the module risks an Arxan split thunk for no benefit. Same algorithm the connect
			// userinfo keys already use (InfoKeyHash in game.cpp).
			//
			// NOTE: tools/wordlists/massive_dvar_dump.txt is NOT for this game. Its hashes are
			// FNV-1a with multiplier 0x10000000233 (an IW-engine prime that appears nowhere in the
			// BOCW image). Use it as a NAME wordlist only, and re-hash every name through here.
			static std::uint64_t HashString(const char* name);

			// Dvar registry. Engine calls -> game thread only.
			// FindDvar returns null when nothing is registered under that name on this build.
			std::uintptr_t* FindDvar(const char* name) const;
			// Human-readable report for one dvar: whether it exists, its type, its flags decoded,
			// and the raw first dword/float of its value block. Never calls a setter.
			std::string DescribeDvar(const char* name) const;
			// Set by name. Both route through the engine's own type-dispatching setters, so a bool
			// dvar can be driven by SetDvarBool and an int/enum/float one by SetDvarInt. Returns a
			// status line (empty name, not found, read-only, ...) rather than a bare bool so the
			// menu can show why nothing happened.
			std::string SetDvarInt(const char* name, int value);
			std::string SetDvarBool(const char* name, bool value);
			// EVERY dvar write in this codebase must go through these two, never through
			// m_Dvar_Set*FromSource directly. A dvar carrying flag 0x400 (server-authoritative) has its
			// write DROPPED by Dvar_ApplyValueInternal when it comes from the main thread with the
			// g_dvarAllowServerFlaggedWrites latch clear — silently, returning void, so the call site
			// cannot tell. These hold the latch across the call and restore it. Measured on the
			// force-raw-block-copy dvar: without the bracket it was a no-op for two full test runs.
			// See kDump_g_dvarAllowServerFlaggedWrites.
			void WriteDvarBool(std::uintptr_t* dvar, bool value);
			void WriteDvarInt(std::uintptr_t* dvar, int value);
			// Same bracket, for a type-8 (string) dvar: StringToValue + ApplyValueInternal, as the
			// engine applies a PubVar. False if the setters are unresolved or the dvar is not a string.
			bool WriteDvarString(std::uintptr_t* dvar, const char* value);
			// The current string value ("" for none/unreadable). Read-only.
			std::string ReadDvarString(const std::uintptr_t* dvar) const;
			// Walk all 1024 buckets and write every registered dvar (hash, type, flags, address) to
			// cw-mod/dvars.txt in the game folder. This is the build-accurate hash list to match a
			// name wordlist against offline. Read-only pointer chase, SEH-guarded, chain-capped.
			std::string DumpAllDvars() const;

			// Un-hash names by scanning this process's own memory. T9 keeps only 63-bit FNV-1a
			// hashes and the retail engine has NO reverse table — Com_FormatHash64 (0x7FF72904A3F0)
			// can only print the hex digits — so a name is recoverable only where the original
			// string still exists somewhere. Hashing every name-shaped run in the decrypted exe
			// dump offline names 21 of 276 LUI menus but 0 of 2294 unknown dvars, because dvar
			// names are baked to hashes at build time and appear nowhere in the image.
			//
			// Scanning the LIVE process reaches what the exe cannot: decompressed fastfile zones,
			// the Lua heap, asset name pools. Targets are the live dvar registry's hashes (free,
			// build-accurate) plus any "0x..." lines in cw-mod/hashes_wanted.txt, so a menu hash
			// list can be dropped in unedited. Writes cw-mod/names_recovered.txt.
			//
			// Pure memory reads — it calls no engine function and takes no engine lock, so unlike
			// everything else here it does NOT need the game thread, and should be run off it: the
			// scan takes seconds and would visibly hitch the game.
			std::string HarvestNamesFromMemory() const;

			// Resolve the Lua C API and wrap every entry in an ArxanCall thunk. Called once from the
			// ctor, after ArxanCall::Init. Leaves the pointers null on a build mismatch.
			void ResolveLuaApi(std::uintptr_t moduleBase);
			bool LuaApiResolved() const;

			// Argument `idx` (1-based) of the C function running on L, as text: a string's bytes (up to
			// 1024), a number formatted, true/false/nil, a hashed name as #<hash>, anything else
			// "<tag N>". Memory reads only, so no Arxan caller check and no __tostring can run. False
			// when idx is past L->top or the slot is unreadable.
			static bool LuaArgText(void* L, int idx, std::string& out);
			// The value a native just pushed (L->top - 1), same formatting. Only meaningful inside a
			// detour, after the original returned and before control goes back to the VM.
			static bool LuaResultText(void* L, std::string& out);

			// --- Opening LUI menus (overlay "LUI menus" tab, lui_menu.cpp) ------------------------
			//
			// Every key of _G.LUI.createMenu - the menus this UI state can build - as 63-bit name
			// hashes, sorted. Read with plain memory reads (_G is a raw Table* at L+48, see
			// lua_index2adr), so no Lua runs and nothing can raise. "" on success, else why not.
			// Game thread only: the tables belong to the game thread's Lua state.
			std::string LuaCollectMenuHashes(std::vector<std::uint64_t>& out) const;

			// Open a menu the way the `openmenu` console command does. `nameOrHash` is a menu name
			// ("ZMUpgrades" - hashed for you, case-insensitive) or a hash: "0x" + up to 16 hex digits,
			// or a bare 15/16-digit hex word. A hash that is not a registry key but matches exactly
			// one key on its low 60 bits (the form the Lua decompiler prints) is widened to that key.
			// Refused when the registry is readable and the hash is not in it, unless `force` - a few
			// real menus ("lobby") are not in LUI.createMenu. A builder that raises does NOT kill the
			// game: the fatal is suppressed for this dispatch and its message comes back in the
			// status line. Game thread only.
			std::string OpenLuiMenu(const char* nameOrHash, bool force = false, int localClient = 0);

			// Open a menu the way the game's own buttons do:
			//     CoD.BaseUtility.OpenOverlay(root, menu, controller, { _sessionMode = Enum.eModes[mode] })
			// run through LUI_ProtectedCall. OpenLuiMenu's addmenu builds the menu with the controller
			// ONLY, so a menu that reads self:getSessionMode() raises (ZMUpgrades: "table index is nil").
			// mode: -1 = empty params table, 0 = MODE_ZOMBIES, 1 = MODE_MULTIPLAYER. The menu has to be
			// a LUI.createMenu key in this UI state (its key object is what gets passed). A raise comes
			// back as the status line. Game thread only.
			std::string OpenLuiOverlay(const char* nameOrHash, int mode, int localClient = 0);

			// "0x" + 1..16 hex digits, or a bare 15/16-digit hex word -> true and the hash. Anything
			// else is a name (false); callers hash it with HashString.
			static bool ParseMenuHash(const char* in, std::uint64_t& out);

			// Describe the Lua call stack of `L` as it stands RIGHT NOW, one line per frame plus the
			// raw argument slots of each. Written for the luaL_traceback detour, which is the one
			// place we get control while an erroring stack is still assembled.
			//
			// Why this exists at all: T9 ships its LUI chunks with local and upvalue names stripped,
			// so Lua's own diagnostics degrade to "attempt to index a nil value" with no parenthetical
			// naming the variable, from a chunk whose name is a hash. The engine's message is
			// therefore near-useless on its own, and the missing half - which menu, built from what
			// arguments - is sitting in the frame slots that lua_getstack hands us.
			//
			// Read-only and engine-call-light: lua_getstack and lua_getinfo("nSl") fill a caller-owned
			// lua_Debug and push NOTHING (the pushing selectors 'f' and 'L' are deliberately not
			// asked for), and the argument slots are read straight out of the value stack with
			// SafeRead. Nothing here can run a metamethod or raise, which matters more than usual on
			// a path that is already handling an error. Game thread only.
			std::string LuaDescribeErrorFrames(void* L, int maxLevels = 12,
				int maxSlotsPerFrame = 24) const;

			// The function running at `level` of L's call stack, as its closure object pointer; 0 when
			// there is no such level or it does not read as a function. Same frame locator as
			// LuaDescribeErrorFrames (the running closure sits at frame[-1]). Pushes nothing, so it is
			// safe from inside a native. Game thread only.
			std::uint64_t LuaFrameFunction(void* L, int level) const;

			// One line for the frame at `level`: "x64:<hash>.lua:<line> <name it was called under>
			// [defined at N]", or "-" when there is no such level. The first line of each
			// LuaDescribeErrorFrames entry, without the slots. Pushes nothing. Game thread only.
			std::string LuaFrameBrief(void* L, int level) const;

			// CoD[tableHash][fnHash] (hashes as the decompiled Lua prints them) as a closure object
			// pointer, 0 when absent or not a function. Plain memory reads. Game thread only.
			std::uint64_t LuaCoDFunction(void* L, std::uint64_t tableHash, std::uint64_t fnHash) const;

			// LiveUser object layout, from LiveStats_GetLiveUserStatsInstance (0x7FF726A7D380) and
			// LiveStats_CreateLiveUserStatsInstance (0x7FF726A7C600). Read by DumpDwLoginState.
			static constexpr std::size_t kLiveUser_Flag5761     = 5761;  // u8, blocks the stats-alloc call site
			static constexpr std::size_t kLiveUser_SigninState  = 5764;  // int, == 2 means signed in
			static constexpr std::size_t kLiveUser_OnlineState  = 5768;  // int, == 4 means online enabled
			static constexpr std::size_t kLiveUser_StatsContainer = 40024; // -> the stats container, or 0

			// Dvar object layout, from Dvar_CanSetValue (0x7FF728C6CBB0), the Lua Dvar metatable
			// natives (0x7FF71E9031E0) and Dvar_SetInt's type switch (0x7FF728C77B90).
			static constexpr std::size_t kDvar_Hash       = 0;   // u64, compared masked to 63 bits
			static constexpr std::size_t kDvar_Next       = 8;   // Dvar*, hash-bucket chain
			static constexpr std::size_t kDvar_Values     = 16;  // -> value block, 96 bytes/gamemode
			static constexpr std::size_t kDvar_Type       = 24;  // int, see kDvarType_*
			static constexpr std::size_t kDvar_Flags      = 28;  // int, see kDvarFlag_*
			static constexpr std::size_t kDvar_Domain     = 32;  // 16 B limits (Dvar_StringToValue arg 3)
			static constexpr std::size_t kDvarValueStride = 96;  // per-gamemode stride within Values
			static constexpr std::size_t kDvarBucketCount = 1024; // index = hash & 0x3FF

			static constexpr int kDvarType_Bool   = 1;
			static constexpr int kDvarType_Float  = 2;
			static constexpr int kDvarType_Int    = 6;
			static constexpr int kDvarType_Enum   = 7;
			static constexpr int kDvarType_String = 8;
			static constexpr int kDvarType_Int64  = 10;
			static constexpr int kDvarType_UInt64 = 11;

			static constexpr int kDvarFlag_ReadOnly       = 0x10;
			static constexpr int kDvarFlag_WriteProtected = 0x40;
			static constexpr int kDvarFlag_CheatProtected = 0x80;
			static constexpr int kDvarFlag_PerGameMode    = 0x800;
			static constexpr int kDvarFlag_Undefined      = 0x4000;

			// --- Walking the Lua state ------------------------------------------------------------
			//
			// The lua_State is not something we have to go find: LUI_BeginEvent is
			//     *(void**)ev = luiCtx;
			// i.e. the "LUI context" global IS the lua_State pointer, and every LUI_SetEvent* call
			// hands that same pointer to lua_getfield. So *m_g_luiCtx is L, with no event to begin,
			// fill or dispatch, and therefore no side effects on the UI.
			//
			// What the engine actually does for a menu open (LUI_DispatchAddMenuEvent, 0x7FF727997770):
			//     lua_getfield(L, LUA_GLOBALSINDEX, "LUI")
			//     lua_getfield(L, -1, "roots")
			//     lua_getfield(L, -1, <"UIRoot0"/"UIRootFull">)
			//     lua_getfield(L, -1, "processEvent")
			//     lua_pushvalue(L, -2); lua_call(L, 0, 0)
			// so a menu name is resolved inside Lua, by _G.LUI.roots[root]:processEvent. There is no
			// native table of menu names to dump - the registry has to be read out of the Lua state.
			//
			// Values are NaN-boxed: tag = (int64)v >> 47, payload = v & 0x7FFFFFFFFFFF. Tags are
			// negative; the two confirmed from the decompiles are string (lua_getfield ORs in
			// 0xFFFD800000000000) and table (lua_index2adr ORs in 0xFFF9800000000000 for the globals
			// table). Anything outside the tag range is a double.
			static constexpr std::size_t kLuaState_Top    = 56;   // L->top, a TValue* into the stack
			static constexpr int         kLuaTagShift     = 47;
			static constexpr std::uint64_t kLuaPayloadMask = 0x7FFFFFFFFFFFULL;
			static constexpr int         kLuaTag_Nil      = -1;
			static constexpr int         kLuaTag_String   = -5;
			static constexpr int         kLuaTag_Table    = -13;
			// Closures. Read straight off lua_getinfo's 'f' branch, which pushes the running
			// function as `closure | 0xFFFB000000000000` — and (int64)0xFFFB000000000000 >> 47 is
			// exactly -10. Confirmed against a live frame dump, where the callbacks passed to
			// processEvent / ProcessEventNow all came back as tag -10.
			static constexpr int         kLuaTag_Function = -10;
			// The engine's interned hashed-identifier type - what a name compiles down to. The
			// LUI.createMenu keys are these: +0 often null, +8 a shared class pointer, +16 the
			// 63-bit name hash, +24 a small size. Only +16 is of any use to us.
			static constexpr int         kLuaTag_HashedName = -6;
			static constexpr std::size_t kLuaHashedName_Hash = 16;
			// A nil TValue: tag -1 in the top 17 bits, zero payload. Pushing and popping is done by
			// writing L->top directly rather than through lua_push*/lua_settop, which sidesteps the
			// Arxan caller check entirely for the stack operations - only the table accessors need
			// the thunk.
			static constexpr std::uint64_t kLuaNil = 0xFFFF800000000000ULL;
			// TString: the characters live past the header, the length sits in it.
			static constexpr std::size_t kLuaTString_Len  = 16;
			static constexpr std::size_t kLuaTString_Data = 24;
			// TString chain pointer. Every interned string is a node in the global string table's
			// bucket list; luaS_new links a new one with `*(qword*)ts = buckets[i]; buckets[i] = ts`.
			static constexpr std::size_t kLuaTString_Next = 0;

			// The global_State, and inside it Lua 5.1's `stringtable strt` - read straight off
			// luaS_new (0x7FF729E4BD20), which is where every string in the runtime is interned:
			//     G          = *(L + 24)
			//     buckets    = *(TString***)(G + 0)
			//     bucket i   = buckets[hash & *(u32*)(G + 8)]        <- +8 is a MASK, not the size
			//     nuse       = *(u32*)(G + 12)                       <- incremented per new string
			// The mask reading is not an assumption: the rehash path sets `*(u32*)(G+8) = 2*old+1`
			// and allocates `8 * (2*old + 2)` bytes, so the bucket count is mask + 1.
			// Closure / Proto / bytecode layout, read off luaG_getobjname (0x7FF729E476D0) and
			// luaG_currentpc (0x7FF729E48740). This is what lets us name the field an error was
			// indexing when Lua itself refuses to (see LuaDescribeErrorFrames).
			static constexpr std::size_t kLuaClosure_IsC  = 11;   // u8, non-zero for a C function
			static constexpr std::size_t kLuaClosure_Code = 32;   // Instruction* (u32 array)
			// The Proto header sits BELOW the code array; proto == code - 104.
			static constexpr std::ptrdiff_t kLuaCode_ToKEnd       = -72;  // TValue* END of constants
			static constexpr std::ptrdiff_t kLuaCode_ToLineDefined = -28; // u32
			// The rest of the Proto header, read straight off luaG_currentline (0x7FF729E486A0).
			// T9 compresses lineinfo: every entry is a DELTA from linedefined, and the entry WIDTH is
			// chosen once per function from the function's total line span - 1 byte under 256 lines,
			// 2 bytes under 65536, 4 bytes above. sizelineinfo doubles as the instruction count, which
			// is the bound that makes a decoded pc trustworthy instead of merely plausible.
			static constexpr std::ptrdiff_t kLuaCode_ToSizeCode   = -80;  // u32, == sizelineinfo
			static constexpr std::ptrdiff_t kLuaCode_ToLineSpan   = -32;  // u32, lastlinedefined - linedefined
			static constexpr std::ptrdiff_t kLuaCode_ToLineInfo   = -16;  // packed delta array
			// LOCAL AND UPVALUE NAMES ARE RETAINED. Read off luaF_getlocalname (0x7FF729E48970), which
			// luaG_getobjname calls first and which returns "local" when it hits. T9 packs them instead
			// of using Lua 5.1's LocVar array:
			//   locvars blob: repeated { name, uleb startpc-delta, uleb (endpc - startpc) }, ending at
			//   a 0 byte. A leading byte >= 7 means the name is inline NUL-terminated text starting at
			//   that byte; 1..6 select a compiler-internal name from the packed list at 0x7FF72AFAD930
			//   ("(for index)", "(for limit)", "(for step)", "(for generator)", "(for state)",
			//   "(for control)"). Upvalue names are a plainer blob: just packed NUL-terminated strings.
			static constexpr std::ptrdiff_t kLuaCode_ToUpvalNames = -24;  // Proto+80
			static constexpr std::ptrdiff_t kLuaCode_ToLocVars    = -8;   // Proto+96
			// Instructions are 8/8/8/8 on this build, not stock Lua's 6/8/9/9.
			// Operand roles, recovered by decoding lines whose source is independently known (a
			// `LUI.CoDRoot:ProcessEventNow(...)` line decodes to exactly GETGLOBAL/GETFIELD/GETFIELD,
			// which pins the layout):
			//     op 40 LOADK      R[A] = k[B]
			//     op 55 GETGLOBAL  R[A] = _G[k[B]]
			//     op 57 GETTABLE   R[A] = R[C][R[B]]   (key in a register - T9's hashed-name lookup)
			//     op 58 GETFIELD   R[A] = R[C][k[B]]
			//     op 67 CALL       B/C are counts, NOT constant indices
			//     op 18 MOVE       R[A] = R[B]
			// Only LOADK/GETGLOBAL/GETFIELD/SETGLOBAL have a constant in B. Naming B for anything
			// else prints real constants at meaningless indices, which reads as a confident answer.
			static constexpr std::uint32_t kLuaOp_Move      = 18;
			static constexpr std::uint32_t kLuaOp_LoadK     = 40;
			static constexpr std::uint32_t kLuaOp_GetGlobal = 55;
			static constexpr std::uint32_t kLuaOp_SetGlobal = 56;
			static constexpr std::uint32_t kLuaOp_HashCall  = 57;
			static constexpr std::uint32_t kLuaOp_GetField  = 58;
			static constexpr std::uint32_t kLuaOp_Call      = 67;
			// THE REGISTER FILE IS THE FRAME: R[n] == frame[+1+n]. Proven live - a
			// `LOADK R6, k[38]` whose constant is the hashed name 0x749269C31A792677 was matched by
			// frame slot [+7] holding exactly that hash. This is what makes a decoded instruction
			// checkable against real values, and it is how the faulting instruction gets identified:
			// the last instruction whose destination register still holds its computed value has
			// executed; the one after it is the one that raised.
			static constexpr std::ptrdiff_t kLuaRegisterBase = 1;
			// Constants are RAW OBJECT POINTERS indexed backwards from the end pointer, not
			// NaN-boxed values: k[i] = *(void**)(kEnd - 8*(i+1)).
			static constexpr std::size_t kLuaObj_TypeByte = 8;
			static constexpr std::uint8_t kLuaObjType_HashedName = 5;

			// Lua Table layout, read straight off luaH_next (0x7FF729E4CE80). Enough to walk any
			// table with plain memory reads — no lua_next, so no metamethod can run and nothing can
			// raise, which is what makes it safe to use from inside an error handler.
			static constexpr std::size_t kLuaTable_Node      = 24;  // Node*, hash part
			static constexpr std::size_t kLuaTable_Array     = 40;  // TValue*, 8 bytes per entry
			static constexpr std::size_t kLuaTable_LastNode  = 48;  // u32, sizenode - 1 (inclusive)
			static constexpr std::size_t kLuaTable_SizeArray = 52;  // u32
			static constexpr std::size_t kLuaNodeStride      = 24;
			static constexpr std::size_t kLuaNode_Value      = 0;
			static constexpr std::size_t kLuaNode_Key        = 8;
			// An empty array slot / empty node value is all-ones, not a zero-payload nil. That
			// decodes as tag -1 with payload 0x7FFFFFFFFFFF, which is also the engine's canonical
			// nil — see LuaCanonicalNil.
			static constexpr std::uint64_t kLuaEmptySlot     = 0xFFFFFFFFFFFFFFFFULL;

			static constexpr std::size_t kLuaState_GlobalState = 24;
			static constexpr std::size_t kLuaStrt_Buckets      = 0;
			static constexpr std::size_t kLuaStrt_SizeMask     = 8;
			static constexpr std::size_t kLuaStrt_NUse         = 12;

			// com_maxclients — the player-cap dvar SV_SpawnServer reads (see docs/phase3_netcode.md).
			// We never read the dvar value back through the engine: Dvar_GetInt is an Arxan split-
			// thunk that faults when called from our module, and the dvar struct is obfuscated so it
			// can't be read at a fixed offset. SetMaxClients pushes a value via the engine setter (a
			// normal, safe function entry) and caches it; LastSetMaxClients returns that cache.
			// ServerMaxClients reads g_svMaxClients — a plain global int, safe from any thread — which
			// is the effective cap set at map spawn and the definitive proof the change took effect.
			int ServerMaxClients();
			int LastSetMaxClients() const;
			bool SetMaxClients(int value);

			// --- Phase 3 lever-1 (loopback co-op) --------------------------------------------------
			// Resolve the co-op anchors above from the game module base (called once in the ctor).
			void ResolveCoopAnchors(std::uintptr_t moduleBase);
			// True once all four anchors resolved to in-range addresses.
			bool CoopAnchorsResolved() const;

			// The client-slot array base (*g_svClients). NULL when no server is running (frontend idle
			// often has one though — core_frontend spawns a small server). Plain pointer read, safe.
			void* ClientSlotArrayBase() const;
			static constexpr std::size_t kClientSlotStride = 0x114D0; // 70864 bytes/slot (recon)

			// Read-only diagnostic: for each slot in [0, cap) dump a few candidate DWORDs so we can
			// find the "connected" state field empirically (recon: SV_DirectConnect sets state = 3).
			// Pure memory reads on the game thread; never calls engine code. Returns a human log blob.
			std::string DumpClientSlots() const;

			// Seat a 2nd local client in-process (lever 1, Option B — surgical). Builds a fresh-connect
			// userinfo with FNV-1a-64 hashed keys (protocol=2426, live netfieldchk, current sessionmode,
			// migrating=0, xuid=0, unique qport, name), stages it, builds a loopback netadr (type 0 at
			// +8, qport at +4), calls SV_DirectConnect, then unstages. MUST run on the game thread with a
			// server running (a ZM map loaded). Returns false if prerequisites are unmet.
			// See docs/phase3_netcode.md, Step 7. Verify success via the slot dumper (a new slot -> state 3).
			bool SeatSecondPlayerLoopback();

			// --- Phase 3 lever-3 (host-launch capture) ------------------------------------------
			// Read-only diagnostic: dump the latched HostSession_StartLaunch parameter block —
			// controllerIdx, sessionId, sessionType, the 208-byte session-config blob, flags, and the
			// map/gametype strings — plus the launch phase and local-player count. Start a stock private
			// match FIRST, then dump: the block is a persistent global (only overwritten on the next
			// launch), so timing is forgiving. Also writes the raw 0x140 bytes to host_launch_block.bin
			// in the game folder for byte-exact sharing. Pure memory reads; never calls engine code.
			// See docs/phase3_netcode.md.

			// Install/remove a passthrough capture hook on the three GScr_LobbyHost_LaunchPrivateP2P wrappers.
			// While installed, starting a stock private match logs which variant fired and all its args (the
			// clonable inputs we replay to trigger our own systemlink co-op host). Appends to host_launch_args.txt

			// Replay a real private-match host launch as a LAN/systemlink session. Sets networkMode=1 then
			// calls LobbyHost_LaunchPrivateP2P_Plain with the arg vector captured by InstallHostLaunchCapture
			// (which rebuilds the config blob + GSC istrings itself). MUST run on the game thread from the
			// frontend, and requires that one stock private match was captured first. The ~1Hz launch pump
			// then carries g_hostLaunchPhase 1->40 (server up). See docs/phase3_netcode.md.

		// Arm a background sampler that polls g_hostLaunchPhase every 20ms for 15s and logs every
		// transition (with elapsed-ms stamps) into the capture log — so one launch yields a complete
		// trajectory instead of a value that resets faster than the dumper can be clicked. Auto-armed by
		// TriggerSystemlinkHostLaunch; also exposed for watching a native Create-Match launch. Detached,
		// guarded against double-arm, and self-terminating. Reads a plain global; safe from any thread.

		// Read-only LAN-session inspector. Chases g_netSessionManager -> session(+672) -> state(+680) and
		// the announce(+688)/search(+696) handler states, so on a SINGLE PC we can tell whether the host
		// lobby is actually advertising over LAN (session state 1=negotiating/advertising, 2=published/
		// discoverable, -2=err) before setting up a 2nd machine. Pure pointer-chase reads with null guards;
		// never calls engine code. Run on the game thread (same thread as the pump) so nothing mutates mid-read.
		std::string DumpSessionState() const;

		// Read-only diagnostic for WHY Demonware login never starts. Reads the LiveUser system master gate,
		// the nodw dvar, and per-controller: sign-in state (+5764), login-driver state (+5768), the DW login
		// controller ptr (+24, non-null => it would be pumped and emit status), and the stats container
		// (+40024). Distinguishes "no user signed in offline" from "signed in but a state gate blocks
		// connect". Pure SEH-guarded reads; never calls engine code. See client/game/session.cpp.
		std::string DumpDwLoginState() const;

			// --- Phase 3 lever-4 (client join) ---------------------------------------------------
			// Install/remove passthrough detours on ClientSession_StartJoin, JoinCtx_AddHostCandidate and
			// ClientSession_JoinKnownHost. With them active, joining a game normally (any path — invite,
			// server browser, LAN) logs the full argument set AND hex-dumps the three buffers the engine
			// dereferences: the 8-byte xuid, the 16-byte netadr handle, and the 85-byte blob. This is how
			// we learn what a real host address actually looks like — the one thing static RE could not
			// tell us. Pure passthrough: reads/logs, then calls the original. Appends to client_join_args.txt.
			std::string ClientJoinCaptureLog() const;

			// Read-only inspector for g_clientJoinCtx: FSM state, candidate count/index, and a decode +
			// hex dump of the host candidate currently being tried (name, host type, xuid, netadr). Use it
			// on PC2 to watch a join march 1->2->3->...->6, and to see whether a host was discovered at all.
			// Pure memory reads with SEH guards; never calls engine code.
			std::string DumpClientJoinState() const;

			// Lever-5 netmsg transcript. Two MinHook detours, both default-off and fully removable:
			// NetMsg_Dispatch (every inbound message, post-decryption) and NetMsg_SendJoinResponse (the
			// host's verdict). Install on BOTH machines, run the join, diff the two transcripts.
			std::string InstallNetMsgTranscript();
			std::string RemoveNetMsgTranscript();
			bool NetMsgTranscriptInstalled() const;

			// Lever 4. Sets networkMode=1 (systemlink) then replays the captured host candidate through
			// ClientSession_JoinKnownHost — a one-shot "join this host" that needs no search result.
			// Requires a prior capture (see InstallClientJoinCapture); refuses to fire otherwise rather
			// than passing null pointers into a function that dereferences them unchecked. MUST run on the
			// game thread from the frontend. Auto-arms the join watcher.

			// Poll the join FSM state every 20ms for 15s, logging every transition with elapsed-ms stamps
			// into the client-join capture log — the join marches faster than the dumper can be clicked.
			// Detached, guarded against double-arm, self-terminating. Read-only on a plain global.
			void StartJoinWatch();

			// --- Lever 4, the native path. These two are the whole 2-PC join. --------------------
			//
			// PC1 (host). Read-only sweep of the 6 session objects (2 types x 3 slots) reporting the
			// live ones, and for each its host XUID / name / secid / seckey / serialized address. The
			// XUID it prints is the ONLY thing PC2 needs. Never calls engine code beyond the getter.
			std::string DumpMyHostIdentity() const;

			// PC2 (client). Forces networkMode=1, then ClientSession_QueryHostByXuid(joinCtxId,
			// controllerIdx, hostXuid) — one InfoRequest addressed by XUID. The engine does the rest:
			// the host's InfoResponse populates g_pendingJoinTarget_*, and once it is valid we call
			// ClientSession_JoinPendingTarget. Because the probe is asynchronous, this arms a poller
			// that waits for the pending target to go valid (up to timeoutMs) and then fires the join
			// on a worker thread; the initial probe itself must run on the game thread.
			// No capture, no fabricated pointers — the address arrives from the host.

			// Read-only: how far the XUID probe got (awaiting flag, nonce, valid flag, and once the
			// descriptor lands, the host name and address the host sent us).
			std::string DumpPendingJoinTarget() const;

			// PC2, the probe-less path. The XUID probe only exists to fill g_pendingJoinTarget_*, and
			// ClientSession_JoinPendingTarget reads just eight of those fields — all of which PC1 can
			// print directly out of its own live session object. So if the probe goes unanswered (the
			// XUID has no address binding on this machine), we can carry the descriptor by hand: PC1's
			// "Show my host XUID" emits a CWJOIN1 blob, PC2 pastes it here, we write the eight fields,
			// set valid=1, and call ClientSession_JoinPendingTarget directly.
			//
			// This writes engine globals, so it MUST run on the game thread, and only while the join
			// context is idle. Fails closed on any malformed blob rather than half-filling the struct.
			std::string JoinHostByDescriptor(const std::string& blob, int joinCtxId, int controllerIdx,
				int joinType);

			// Builds the CWJOIN1 blob for a given live session object. Used by DumpMyHostIdentity.
			static std::string BuildJoinDescriptorBlob(std::uint64_t xuid, int slot, const char* name,
				const std::uint8_t* secId, const std::uint8_t* secKey, const std::uint8_t* adr);

			// One host's join target: exactly the fields a CWJOIN1 blob carries.
			struct JoinDescriptor {
				std::uint64_t xuid{};
				int           slot{};
				char          name[37]{};   // 36 from the engine, plus a terminator
				std::uint8_t  secId[8]{};
				std::uint8_t  secKey[16]{};
				std::uint8_t  adr[84]{};
			};

			// Parses a CWJOIN1 blob. Whitespace and the prefix are optional. Returns an empty string on
			// success, else why it was rejected; `d` is written only on success.
			static std::string DecodeJoinDescriptorBlob(const std::string& blob, JoinDescriptor& d);

			// The one session a joining client would end up targeting if it asked this host: per slot
			// the type-0 object if live, else type-1 (Session_BuildSendInfoResponse), and of those the
			// HIGHEST live slot (ClientSession_SetPendingJoinTarget_FromDescriptor scans 2 -> 0).
			// Game thread only (calls Session_GetSessionObject). False when nothing is live. `obj`, if
			// given, receives the session object the descriptor was read from.
			bool ReadAdvertisedSession(JoinDescriptor& d, int& members, const std::uint8_t** obj = nullptr) const;

			// Everything else a browser row shows about this host's lobby. Map and gametype are read from
			// `obj` first, then from the other live session objects, since which of the two session types
			// carries the game settings has not been pinned down. Game thread only.
			struct LobbyDetails {
				std::string map;
				std::string gametype;
				int maxClients{};                // 0 when the slot reports its idle value (127)
				std::vector<std::string> players;
			};
			LobbyDetails ReadLobbyDetails(const std::uint8_t* obj) const;

			// --- Progression / PlayerData (the ZM upgrade gate) -----------------------------------
			// PlayerData def layout, from PlayerData_Init / IsValidForController / FlushLocation /
			// ResetBufferToDefaults (dump 0x7FF7275C17C0 / C1760 / BFFE0 / C2250).
			static constexpr std::size_t kPdDef_DdlNameHash   = 16;  // u64, DDL asset name hash
			static constexpr std::size_t kPdDef_DefaultVer    = 48;  // int, used when version == -1
			static constexpr std::size_t kPdDef_DataMapId     = 56;  // int, index into g_playerDataDefsById
			static constexpr std::size_t kPdDef_Scope         = 60;  // int: 1 => controller 0 only, 2/3 => controller <= 1
			static constexpr std::size_t kPdDef_StorageLoc    = 64;  // int, index into g_playerDataStorageBackends
			// +68 is the flag PlayerData_FlushLocation requires before it will submit an entry for
			// writing — a def with +68 == 0 is never persisted, no matter what its storageLocation says.
			// +69 is checked by PlayerData_SubmitWrite only for writeKind 1. Both matter for saving, so
			// the probe prints them.
			static constexpr std::size_t kPdDef_Flag          = 68;  // bool, "persist this map"
			static constexpr std::size_t kPdDef_Flag69        = 69;  // bool, blocks writeKind 1

			// Per-controller store block, and the 336-byte entries inside it. entryBase(k) =
			// store + kPdStoreStride*controller + 336*k + 16 — the +16 is the engine's own bias
			// (PlayerData_GetBuffer computes &block[336*idx + 16]).
			static constexpr std::size_t kPdStoreStride    = 86040; // bytes per controller
			static constexpr std::size_t kPdStoreCountOff  = 86032; // int, live entry count for that controller
			static constexpr std::size_t kPdEntryStride    = 336;
			static constexpr std::size_t kPdEntryBias      = 16;
			static constexpr std::size_t kPdEntryCapacity  = 256;   // (86032 - 16) / 336
			static constexpr std::size_t kPdEntry_DataMapId = 0;    // int
			static constexpr std::size_t kPdEntry_Def       = 8;    // def*
			static constexpr std::size_t kPdEntry_Size      = 28;   // int, buffer size
			static constexpr std::size_t kPdEntry_Version   = 32;   // int
			static constexpr std::size_t kPdEntry_Buffer    = 40;   // void*, the DDL byte buffer
			static constexpr std::size_t kPdEntry_Loaded    = 120;  // int, ==1 means READY. THE gate.
			// What PlayerData_GetBuffer actually returns: entryBase+16+56, i.e. a DDL INSTANCE
			// ({buffer, size, rootDef}) embedded in the entry, not the raw bytes. That is why a
			// playerdata map can be used directly as the SOURCE of Ddl_CopyInstanceToInstance.
			static constexpr std::size_t kPdEntry_DdlInstance = 56;

			// Retry state, armed by PlayerData_OnStorageOpComplete when a per-map callback vetoes a
			// completion. The engine indexes the entry as unsigned int*, so a4[2*opKind + 33] is the
			// attempt counter and a4[2*opKind + 34] the next-fire time:
			//     nextFire = 1000 * min(attempt, dvarMaxBackoff) * dvarInterval + now
			// READ (opKind 0) is entry+132/136; WRITE would be entry+140/144. Only READ is named here
			// because only the read path is ours to settle — see PlayerData_OnStorageOpComplete.cpp.
			// Clearing both is what stops a redirected map from being re-read, re-failed and
			// re-vetoed every few seconds after we have filled it with defaults.
			static constexpr std::size_t kPdEntry_ReadRetryAttempt  = 132;
			static constexpr std::size_t kPdEntry_ReadRetryNextTime = 136;

			// Storage backend table: {+0 id, +8 name, +16 maxPending, +24 Write, +32 ?, +40 IsAvailable}.
			static constexpr std::size_t kPdBackendStride     = 8272;
			static constexpr std::size_t kPdBackend_Name      = 8;
			static constexpr std::size_t kPdBackend_MaxPending = 16;
			static constexpr std::size_t kPdBackend_Write     = 24;
			static constexpr std::size_t kPdBackend_IsAvail   = 40;
			static constexpr int kPdBackendCount   = 5;

			// The pending-op queue, which is the ONLY honest witness that a storage op did anything.
			// PlayerDataStorage_EnqueueOp (0x7FF728173EB0) writes into
			//   backend + 2056*(writeKind + 2*controller), entries at +48 (256 x void*), count at +2096.
			// Four queues per backend: 48 header + 4*2056 = 8272 = kPdBackendStride, so they tile exactly.
			// FOUR queues, not two per controller by accident: kind 0 is the READ queue and kind 1 is the
			// WRITE queue, so the two directions never share a pending list.
			//
			// Why we read this by hand instead of trusting the return codes: BOTH halves of either pair
			// return a value that is indistinguishable from "did nothing".
			//   PlayerData_SubmitLocationLoad  starts its result at 1 and only clears it on a FAILED
			//                             submit, so "no entry matched" also returns 1.
			//   PlayerDataStorage_RunQueuedOps  reassigns result = writeKind + 2*controller before the
			//                             drain loop, so for controller 0 / kind 0 an EMPTY queue returns
			//                             0 — the same 0 the backend's op fn returns on failure.
			// Reading the count before/after collapses that ambiguity: submitted>0 with the count back to
			// 0 means the backend accepted them; a count that never rises means nothing was submitted; a
			// count that rises and stays means the backend refused.
			static constexpr std::size_t kPdQueueStride  = 2056;
			static constexpr std::size_t kPdQueueEntries = 48;
			static constexpr std::size_t kPdQueueCount   = 2096;
			static constexpr int kPdQueueCapacity = 256;   // EnqueueOp refuses at exactly 256
			// The direction, spelled out so no call site has to remember which way round it is.
			static constexpr int kPdKind_Read  = 0;   // disk -> buffer (PlayerDataFile_ReadBatch)
			static constexpr int kPdKind_Write = 1;   // buffer -> disk (PlayerDataFile_WriteBatchWithBackup)

			static constexpr int kPdLoc_Hdd         = 0;
			static constexpr int kPdLoc_DwUser      = 1;   // the one that fails offline
			static constexpr int kPdLoc_Fastfile    = 4;   // write-only; ResetBufferToDefaults skips it
			static constexpr int kPdControllerCount = 2;   // PlayerData_Init loops controllers [0,2)

		private:
			std::atomic<int> m_MaxClientsCache{ -1 };
		};

		// --- Online session mode (menu-shape testing) --------------------------------------------
		// The frontends the game builds differ by session network mode: offline/LAN hides the
		// matchmaking and online-progression menus entirely, so those code paths can never be
		// exercised from an offline boot. This is the opt-in switch that starts the client in
		// networkMode 2 (live) so the online frontends get built. It changes what the UI SHOWS; it
		// does not by itself make any Demonware service answer. See client/game/session.cpp.
		namespace OnlineMode {
			// True when the boot profile is online (Boot::ApplyEarly). Read by the per-frame pin below.
			inline std::atomic_bool g_Requested{ false };
			// Re-apply networkMode 2 whenever the engine resets it (lobby transitions do). Separate
			// toggle because pinning fights LAN discovery, which selects on networkMode == 1.
			inline std::atomic_bool g_Pin{ false };

			// Drop Battle.net / first-party errors instead of letting them latch. Read by the four
			// detours (the two Com_Error reporters and the two STATE_ERROR writers), which are now
			// installed ALWAYS and inert — gating installation on the boot marker meant flipping to
			// online from the overlay got no suppression at all, because the hooks were decided long
			// before the button existed. Set automatically whenever online mode is requested; also a
			// checkbox, so it can be turned off to see the stock failure. Default false = stock.
			inline std::atomic_bool g_SuppressBnetErrors{ false };

			// Called once per frame from OnShowOverStack (game thread). Cheap: one read of a plain
			// global; only calls the engine setter when the nibble has drifted off 2.
			void Tick();
		}

		// --- Opening LUI menus by hand (Pointers::OpenLuiMenu) ---------------------------------------
		namespace LuiMenus {
			// True only while OpenLuiMenu is inside LUI_DispatchAddMenuEvent. The LuiError_ReportFatal
			// detour suppresses the engine's deliberate termination during that window - a menu that
			// wants data this UI state never loaded raises from its builder, and that must cost a
			// status line, not the process. Everything outside the window keeps stock behaviour.
			inline std::atomic_bool g_InDispatch{ false };
			// The Lua error the suppressed fatal carried. Written and read on the game thread only.
			inline std::string g_LastError;
			// Sticky: set once the tab has sent any menu. A hand-opened menu that BUILT can still raise
			// later, from its own event handlers (measured 2026-09-23: BlueprintShopMenu's button-prompt
			// update, "attempt to compare number with nil", on the next input change), and that lands
			// outside g_InDispatch. From then on every fatal is suppressed and logged for the rest of the
			// session. Deliberately broad: the error carries nothing that ties it to the element.
			inline std::atomic_bool g_HandOpened{ false };
			inline std::atomic_int  g_LateSuppressed{ 0 };
		}

		// --- B5 diagnostics: the gates in front of the marketplace inventory fetch ----------------
		// Read-only. On an online boot, logs one "(Entitlements)" line whenever any gate value changes
		// (and every 30 s as a heartbeat), so one boot shows which gate the inventory is stuck behind.
		// Anchors and meanings are in dump_anchors.hpp (kDump_g_pubVarsState and below).
		namespace EntitlementGates {
			void Tick();
		}

		// --- B6: the Demonware fetch checklist (DwFetch_Transcript.cpp / DwFetch_LobbyWaiver.cpp) --
		namespace DwFetch {
			// The five bits that only public matchmaking and commerce fill: G ffotd listed, N dedicated
			// server QoS, R marketplace inventory, W relay bind, Y in-game store. See dump_anchors.hpp.
			inline constexpr std::uint32_t kRequired = 0x17337FA;
			inline constexpr std::uint32_t kWaivable = 0x40 | 0x2000 | 0x20000 | 0x400000 | 0x1000000;
			// The mask the transcript detour saw on THIS thread's most recent DwFetch_GetStatus call.
			// False when no call was seen since the last Reset (e.g. the transcript is not installed).
			void ResetLastGot();
			bool LastGot(std::uint32_t& got);
		}

		// --- LAN server browser --------------------------------------------------------------------
		// T9 has no working LAN discovery: the ServerlistInfo (netmsg 22) handler is compiled out to
		// `mov al,1; ret`, nothing sends msgId 22, and there is no bdLANDiscovery. So we carry it
		// ourselves. Every host broadcasts its join descriptor (the CWJOIN1 blob) over UDP once a
		// second, every client keeps a list of what it hears, and the Session tab joins an entry
		// through JoinHostByDescriptor. See client/game/lan_browser.cpp.
		namespace LanBrowser {
			inline constexpr std::uint16_t kPort = 28970;

			inline std::atomic_bool g_Advertise{ true };   // host side: broadcast our live lobby
			inline std::atomic_bool g_Listen{ true };      // client side: collect other hosts' beacons

			// What a host says about itself. Everything but the descriptor is display-only.
			struct Advert {
				std::string   blob;          // CWJOIN1 blob, fed straight to JoinHostByDescriptor
				std::uint64_t xuid{};
				std::string   name;
				int           slot{};
				int           members{};
				int           maxClients{};  // 0 = unknown
				int           networkMode{}; // 0 offline, 1 LAN, 2 online
				int           gameMode{};    // low nibble of g_sessionModePacked: 0 zm 1 mp 2 cp 3 wz
				bool          inMatch{};
				std::string   map;           // internal name, e.g. zm_silver
				std::string   gametype;
				std::vector<std::string> players;
				std::string   build;         // game version, e.g. 1.34.0.15931218
				std::string   mod;           // cw-mod git describe
			};

			struct Host : Advert {
				std::string   address;       // sender IPv4, from recvfrom
				std::uint64_t lastSeenMs{};  // GetTickCount64
				float         pingMs = -1.f; // round trip of our last answered ping; < 0 = none yet
			};

			struct Status {
				bool          socketUp{};
				std::string   socketError;
				bool          advertising{};
				Advert        advert;        // valid while advertising
				int           broadcastTargets{};
				std::uint64_t sent{};
				std::uint64_t received{};
				std::uint64_t rejected{};
			};

			// "Die Maschine" for "zm_silver"; empty if we do not know the map.
			const char* FriendlyMapName(const std::string& map);
			// "Zombies" for 0, etc.
			const char* GameModeName(int gameMode);

			// Game thread, once per frame (OnShowOverStack). Starts the network thread on the first
			// call, then refreshes our own advert once a second. Only reads engine memory.
			void Tick();

			std::vector<Host> Hosts();   // snapshot, sorted by name
			Status GetStatus();
		}

		// --- The dwuser -> hdd redirect, and the failed reads it produces -------------------------
		// Shared between the two hooks that make the online frontend's player data readable:
		//   PlayerData_ControllerStorageTick.cpp   rewrites the defs and records them here
		//   PlayerData_OnStorageOpComplete.cpp     reads this to decide whether a failed read is one
		//                                          of ours to settle, or a stock one to leave alone
		//
		// A bitmask, not a set: data map ids are 1..0x2D, so they fit in a single atomic word that
		// both the main thread (tick) and the storage job thread (completion) can touch without a
		// lock. Id 0 is not a valid map, so bit 0 is unused.
		namespace PlayerDataRedirect {
			inline std::atomic<std::uint64_t> g_RedirectedMaps{ 0 };

			inline void MarkRedirected(int dataMapId) {
				if (dataMapId > 0 && dataMapId < 64)
					g_RedirectedMaps.fetch_or(1ull << dataMapId, std::memory_order_relaxed);
			}
			inline bool IsRedirected(unsigned int dataMapId) {
				return dataMapId < 64
					&& (g_RedirectedMaps.load(std::memory_order_relaxed) & (1ull << dataMapId)) != 0;
			}
		}
	}
	inline std::unique_ptr<Game::Pointers> g_Pointers{};
}

#define MATERIAL_WHITE Client::g_Pointers->m_DB_FindXAssetHeader(Client::T9::XAssetType::ASSET_TYPE_MATERIAL, "white", true, -1).material
