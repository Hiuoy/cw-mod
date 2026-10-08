#pragma once
#include "common.hpp"
#include "hooks/hook_types.hpp"
#include "memory/iat.hpp"
#include "memory/minhook.hpp"
#include "game/function_types.hpp"

namespace Client {
	namespace Hook {
		// LuaGate_Probe.cpp: the BeginLivePlay / title-screen natives, logged as (LuaGate) lines.
		// Returns how many detours are live.
		int InstallStartGateProbe();

		// DecryptString_UiText.cpp: builds the "ui_text" lookup from cw-mod.json. Must run before the
		// DecryptString detour is enabled; the table is read without a lock afterwards.
		void BuildUiTextTable();

		class Hooks {
		public:
			// IAT
			using HK_SetUnhandledExceptionFilter = HookPlate::StdcallHook<"kernel32/SetUnhandledExceptionFilter", LPTOP_LEVEL_EXCEPTION_FILTER,
				LPTOP_LEVEL_EXCEPTION_FILTER>;
			Memory::IAT* m_SetUnhandledExceptionFilterHK;

			// Bypasser hook
			using HK_RtlDispatchException = HookPlate::FastcallHook<"RtlDispatchException", bool,
				PEXCEPTION_RECORD, PCONTEXT>;
			Memory::MinHook<Game::Functions::RtlDispatchExceptionT>* m_RtlDispatchExceptionHK;

			// Regular game hooks
			using HK_BB_Alert = HookPlate::FastcallHook<"BB_Alert", void,
				const char*, const char*>;
			Memory::MinHook<Game::Functions::BB_AlertT>* m_BB_AlertHK;

			// Local Demonware backend: force the bdLogin flow selector to 9 (studio auth) so login
			// skips the Battle.net first-party token step and goes straight to our Demonware auth.
			// Installed only when DwBackend is enabled. See dump_anchors.hpp / dw_backend.
			using HK_Dw_GetLoginFlow = HookPlate::FastcallHook<"Dw_GetLoginFlow", std::uint64_t,
				void*>;
			Memory::MinHook<Game::Functions::Dw_GetLoginFlowT>* m_Dw_GetLoginFlowHK{};

			// Login-status mirror: read-only diagnostics that echo the login state-machine's status
			// strings into our console so we can see exactly how far Demonware login progresses.
			// Installed alongside the flow hook when DwBackend is enabled.
			using HK_Login_SetStatus = HookPlate::FastcallHook<"Login_SetStatus", std::uint64_t,
				void*, const char*, std::uint32_t>;
			Memory::MinHook<Game::Functions::Login_SetStatusT>* m_Login_SetStatusHK{};

			// The first-party presence gate. True for the login driver always (else it parks in state 1);
			// true for every other caller once DW login is complete and Bnet-error suppression is on
			// (the online-ready gates that start post-login traffic), except identity setup, which would
			// read the gamertag from the unsigned first-party manager. Each overridden call site is
			// logged once. DwBackend only.
			using HK_LiveUser_FirstPartyPresenceOk = HookPlate::FastcallHook<"LiveUser_FirstPartyPresenceOk", bool>;
			Memory::MinHook<Game::Functions::LiveUser_FirstPartyPresenceOkT>* m_LiveUser_FirstPartyPresenceOkHK{};

			// The null first-party SESSION census. FirstParty_GetSession_MayBeNull returns *(mgr+24),
			// which studio auth never populates. The census ran on 2026-08-02 and answered its
			// question: exactly ONE call site asks while it is null, and it wants one field — the
			// local user index — so HK_FirstParty_GetLocalUserIndex below is the actual fix and this
			// detour is now a pure WATCHMAN. It logs any call site that asks and hands back null
			// exactly as the original would, so it changes nothing; if a future boot reaches new code
			// that also wants a session, the log line lands BEFORE the crash and names the site.
			// ("fpsession_standin": true in cw-mod.json re-arms the zeroed stand-in to survive such a site and census
			// the rest of them, the way the first run did.)
			// Installed only when DwBackend is enabled, so a normal boot is byte-identical.
			using HK_FirstParty_GetSession = HookPlate::FastcallHook<"FirstParty_GetSession", void*,
				void*>;
			Memory::MinHook<Game::Functions::FirstParty_GetSessionT>* m_FirstParty_GetSessionHK{};

			// THE FIX the census pointed at. FirstParty_GetLocalUserIndex_ViaSession() dereferences
			// the null session to read +0xD0, the local user / controller index, and that read is the
			// crash one second after "[status 27] Login Complete". The detour returns 0 when the
			// session is null and calls through untouched when it is not. 0 is the right answer, not
			// a placeholder: the only consumer passes it on as the user index, and this is a
			// single-local-user PC — controller 0.
			using HK_FirstParty_GetLocalUserIndex =
				HookPlate::FastcallHook<"FirstParty_GetLocalUserIndex", std::uint32_t>;
			Memory::MinHook<Game::Functions::FirstParty_GetLocalUserIndexT>* m_FirstParty_GetLocalUserIndexHK{};

			// The ERR_DROP that ends an online boot ~1s after "[status 27] Login Complete", shown as
			// the word-code "An error occurred: Boy 501 Gothic Missile". That is not an error NAME —
			// it is Com_FormatHash64 printing an error hash with no localized string behind it.
			//
			// It is NOT a sign-out (B1, 2026-09-16). The target (IDB: LiveUser_PromoteSigninOnline_MaybeDrop)
			// runs on DW connect and PROMOTES signinState 1 -> 2; it additionally asks for a drop to
			// offline when the prior state was 1 in online session mode. The detour lets the promotion
			// run untouched and returns 0, skipping only the forced-offline + ERR_DROP — the verdict an
			// offline boot gets. Logs every call. Installed only when DwBackend is enabled.
			using HK_LiveUser_SignOutDropGate =
				HookPlate::FastcallHook<"LiveUser_SignOutDropGate", char, std::uint32_t, const char**>;
			Memory::MinHook<Game::Functions::LiveUser_SignOutBuildDropMessageT>* m_LiveUser_SignOutDropGateHK{};

			// B3 LPC list transcript. Read-only: each detour calls the original and logs what it
			// returned, so the log names which step rejects our objectstore reply. DwBackend only.
			using HK_BdRemoteHttpTask_FinishRow = HookPlate::FastcallHook<"BdRemoteHttpTask_FinishRow",
				std::uint64_t, void**, void*>;
			Memory::MinHook<Game::Functions::BdRemoteHttpTask_FinishRowT>* m_BdRemoteHttpTask_FinishRowHK{};
			using HK_PublisherObjectsResource_Parse = HookPlate::FastcallHook<"PublisherObjectsResource_Parse",
				std::uint8_t, void*, void*>;
			Memory::MinHook<Game::Functions::PublisherObjectsResource_ParseT>* m_PublisherObjectsResource_ParseHK{};
			using HK_ObjectMetadata_ParseJson = HookPlate::FastcallHook<"ObjectMetadata_ParseJson",
				std::uint8_t, void*, void*, std::uint32_t>;
			Memory::MinHook<Game::Functions::ObjectMetadata_ParseJsonT>* m_ObjectMetadata_ParseJsonHK{};
			using HK_Lpc_WriteManifest = HookPlate::FastcallHook<"Lpc_WriteManifest", std::uint64_t, std::uint32_t>;
			Memory::MinHook<Game::Functions::Lpc_ListCallbackT>* m_Lpc_WriteManifestHK{};
			using HK_Lpc_OnListFailed = HookPlate::FastcallHook<"Lpc_OnListFailed", std::uint64_t, std::uint32_t>;
			Memory::MinHook<Game::Functions::Lpc_ListCallbackT>* m_Lpc_OnListFailedHK{};
			// Lobby request census: first sighting of each (msgType, service) with its callers, plus a
			// running count for repeats. Read-only. DwBackend only.
			using HK_BdLobbyMsg_WriteHeader = HookPlate::FastcallHook<"BdLobbyMsg_WriteHeader", std::uint64_t,
				void**, std::uint8_t, std::uint8_t>;
			Memory::MinHook<Game::Functions::BdLobbyMsg_WriteHeaderT>* m_BdLobbyMsg_WriteHeaderHK{};
			// B5 MtxSync transcript: which gate keeps the marketplace sync at state 0. Read-only.
			using HK_MtxSync_ShouldStart = HookPlate::FastcallHook<"MtxSync_ShouldStart", std::uint8_t, std::int32_t>;
			Memory::MinHook<Game::Functions::MtxSync_ControllerGateT>* m_MtxSync_ShouldStartHK{};
			using HK_MtxSync_RequestBnetTokenZEUS = HookPlate::FastcallHook<"MtxSync_RequestBnetTokenZEUS",
				std::uint8_t, std::int32_t>;
			Memory::MinHook<Game::Functions::MtxSync_ControllerGateT>* m_MtxSync_RequestBnetTokenZEUSHK{};
			using HK_MtxSync_OnBnetToken = HookPlate::FastcallHook<"MtxSync_OnBnetToken", std::uint8_t,
				std::int32_t, std::int64_t*>;
			Memory::MinHook<Game::Functions::MtxSync_OnBnetTokenT>* m_MtxSync_OnBnetTokenHK{};
			using HK_MtxSync_IsDoneOrInFlight = HookPlate::FastcallHook<"MtxSync_IsDoneOrInFlight", std::uint8_t,
				std::int32_t>;
			Memory::MinHook<Game::Functions::MtxSync_ControllerGateT>* m_MtxSync_IsDoneOrInFlightHK{};
			// B6 lobby gate transcript: which Demonware fetch bits keep IsDemonwareFetchingDone false, so
			// the online frontend never builds its lobby. Read-only. DwBackend only.
			using HK_DwFetch_GetStatus = HookPlate::FastcallHook<"DwFetch_GetStatus", std::uint8_t,
				std::uint32_t, std::uint32_t*, void*>;
			Memory::MinHook<Game::Functions::DwFetch_GetStatusT>* m_DwFetch_GetStatusHK{};
			// B6 lobby waiver: Lua's IsDemonwareFetchingDone answers true when only the five public-
			// matchmaking/commerce bits are missing. Return-address guarded. DwBackend only.
			using HK_DwFetch_IsDone = HookPlate::FastcallHook<"DwFetch_IsDone", std::uint8_t, std::uint32_t>;
			Memory::MinHook<Game::Functions::DwFetch_IsDoneT>* m_DwFetch_IsDoneHK{};
			// "You don't own the game": answers not-trial on a backend boot, as nodw does offline.
			using HK_LiveUser_IsTrial = HookPlate::FastcallHook<"LiveUser_IsTrial", std::uint8_t>;
			Memory::MinHook<Game::Functions::LiveUser_IsTrialT>* m_LiveUser_IsTrialHK{};
			// This PC's own peer address, built the LAN way on a backend boot (no public address, NAT
			// open), so two PCs connect directly instead of through Demonware NAT traversal. DwBackend only.
			using HK_bdCommonAddr_Ctor = HookPlate::FastcallHook<"bdCommonAddr_Ctor", void*,
				void*, void*, const void*, std::uint32_t, std::int32_t>;
			Memory::MinHook<Game::Functions::bdCommonAddr_CtorT>* m_bdCommonAddr_CtorHK{};
			// Lua print transcript: Engine.PrintInfo/Warning/Error are compiled out of retail; the
			// wrappers are hooked so the lobby Lua's own narration reaches client.log. Read-only.
			using HK_LuaNative_PrintInfo = HookPlate::FastcallHook<"LuaNative_PrintInfo", std::int32_t, void*>;
			Memory::MinHook<Game::Functions::LuaNativeT>* m_LuaNative_PrintInfoHK{};
			using HK_LuaNative_PrintWarning = HookPlate::FastcallHook<"LuaNative_PrintWarning", std::int32_t, void*>;
			Memory::MinHook<Game::Functions::LuaNativeT>* m_LuaNative_PrintWarningHK{};
			using HK_LuaNative_PrintError = HookPlate::FastcallHook<"LuaNative_PrintError", std::int32_t, void*>;
			Memory::MinHook<Game::Functions::LuaNativeT>* m_LuaNative_PrintErrorHK{};
			// The mode-tile lock predicates, logged per (argument -> answer). LuaGate_Probe.cpp.
			using HK_LuaNative_ContentIsFullyInstalled = HookPlate::FastcallHook<"LuaNative_ContentIsFullyInstalled", std::int32_t, void*>;
			Memory::MinHook<Game::Functions::LuaNativeT>* m_LuaNative_ContentIsFullyInstalledHK{};
			using HK_LuaNative_ContentIsPlayable = HookPlate::FastcallHook<"LuaNative_ContentIsPlayable", std::int32_t, void*>;
			Memory::MinHook<Game::Functions::LuaNativeT>* m_LuaNative_ContentIsPlayableHK{};
			using HK_LuaNative_ContentIsInstalling = HookPlate::FastcallHook<"LuaNative_ContentIsInstalling", std::int32_t, void*>;
			Memory::MinHook<Game::Functions::LuaNativeT>* m_LuaNative_ContentIsInstallingHK{};
			using HK_LuaNative_GetDvarInt = HookPlate::FastcallHook<"LuaNative_GetDvarInt", std::int32_t, void*>;
			Memory::MinHook<Game::Functions::LuaNativeT>* m_LuaNative_GetDvarIntHK{};
			using HK_LuaNative_IsKoreaMinor = HookPlate::FastcallHook<"LuaNative_IsKoreaMinor", std::int32_t, void*>;
			Memory::MinHook<Game::Functions::LuaNativeT>* m_LuaNative_IsKoreaMinorHK{};
			using HK_LuaNative_IsLobbySlotLive = HookPlate::FastcallHook<"LuaNative_IsLobbySlotLive", std::int32_t, void*>;
			Memory::MinHook<Game::Functions::LuaNativeT>* m_LuaNative_IsLobbySlotLiveHK{};

			// Studio-auth JWT builder mirror. Read-only diagnostics: logs the provider index and the
			// original return so we can see WHY the studio token fails to build (provider>3 early-out vs
			// asymmetric-sign failure from a missing studio key). Installed when DwBackend is enabled.
			using HK_DwLogin_BuildStudioToken = HookPlate::FastcallHook<"DwLogin_BuildStudioToken", std::uint8_t,
				void*, void*, void*>;
			Memory::MinHook<Game::Functions::DwLogin_BuildStudioTokenT>* m_DwLogin_BuildStudioTokenHK{};

			// Fatal LUI Lua-error handler. The engine pcalls the menu builder, so a builder that
			// raises is caught - and then deliberately terminates the process from here. Detoured so
			// the error text reaches our log, and on an online boot so one raise does not end it.
			// That suppression is a crutch: remove it once an online+backend boot builds clean.
			using HK_LuiError_ReportFatal = HookPlate::FastcallHook<"LuiError_ReportFatal", std::uint64_t,
				const char*, void*>;
			Memory::MinHook<Game::Functions::LuiError_ReportFatalT>* m_LuiError_ReportFatalHK{};

			// The LUI chunk runner. A chunk missing from the luafile pool is a sys_error inside the asset
			// lookup, before any Lua error exists for the handler above to suppress. The detour skips
			// ui/ffotd_tu<N>.lua when it is not loaded (LIVE + backend: the slots read loaded, the ffotd
			// zone never is) and passes every other chunk through untouched. Installed always; each
			// chunk name is logged once with the verdict. See dump_anchors.hpp. Around ui/main.lua it also
			// sets the season publisher variable the menus need (before) and runs cw-mod/ui_scripts (after).
			using HK_LUI_RunFile = HookPlate::FastcallHook<"LUI_RunFile", std::uint8_t, void*, const char*>;
			Memory::MinHook<Game::Functions::LUI_RunFileT>* m_LUI_RunFileHK{};

			// Every localized string the UI shows is decrypted here on its way out (Lua and native
			// alike), so this is where "ui_text" in cw-mod.json swaps one for another, and where
			// "ui_text_log" lists them. Installed only when one of the two is set.
			using HK_DecryptString = HookPlate::FastcallHook<"DecryptString", char*, char*>;
			Memory::MinHook<Game::Functions::DecryptStringT>* m_DecryptStringHK{};

			// Builds every iprintln / iprintlnbold payload. Read-only: the detour calls the original,
			// then mirrors the text into client.log as (Script) lines and onto the overlay, because
			// the game draws a script's plain-text print as an empty box. Installed always.
			using HK_Scr_ConstructMessageString = HookPlate::FastcallHook<"Scr_ConstructMessageString", void,
				int, std::uint32_t*, std::uint32_t, int>;
			Memory::MinHook<Game::Functions::Scr_ConstructMessageStringT>* m_Scr_ConstructMessageStringHK{};

			// The Lua errfunc LUI wraps its pcalls in - what appends "stack traceback:" to a caught
			// error. Hooked purely to read: an errfunc runs BEFORE luaD_throw unwinds, so this is the
			// only point at which the erroring call chain, and every frame's arguments, still exist.
			// LuiError_ReportFatal sees only the finished string, which on a build with stripped
			// chunk debug info says little more than "something was nil somewhere".
			//
			// Read-only and always installed, but it only LOGS while OnlineMode::g_SuppressBnetErrors
			// is set, so a normal offline session pays nothing but a branch.
			using HK_luaL_traceback = HookPlate::FastcallHook<"luaL_traceback", std::uint64_t,
				void*, void*, const char*, int>;
			Memory::MinHook<Game::Functions::luaL_tracebackT>* m_luaL_tracebackHK{};

			// A one-line repair of an engine bug, not an instrument. luaG_getobjname's hashed-name
			// branch returns a namewhat WITHOUT writing the name, and luaL_traceback then runs %s
			// over the caller's uninitialised stack slot - which killed the process on the online
			// frontend's very first Lua error. The detour writes a valid pointer on exactly that
			// return and passes everything else through untouched. Installed always: the bug is not
			// specific to online mode, it just needs a Lua error to fire, and the cost off that path
			// is one pointer compare. See dump_anchors.hpp for the decompiled chain.
			using HK_luaG_getobjname = HookPlate::FastcallHook<"luaG_getobjname", const char*,
				void*, void*, void*, unsigned int, const char**>;
			Memory::MinHook<Game::Functions::luaG_getobjnameT>* m_luaG_getobjnameHK{};

			// --- TEMPORARY: the dwuser -> hdd playerdata redirect (online boots only) --------------
			// These two exist because no backend serves Demonware user storage yet, so dwuser-backed
			// data maps never load and the online frontend raises. DELETE BOTH once the local backend
			// serves bdStorage (backend roadmap B4) and dwuser becomes available for real.
			//
			// The async storage-read completion — where entry+120 is written on the load path. When a
			// redirected map's read fails (no local file yet) and a per-map callback vetoes it, the
			// detour keeps the defaults loaded instead of letting the engine unload them.
			//
			// RUNS ON THE STORAGE JOB THREAD as well as the main thread. The detour must stay
			// thread-safe and must never call into Lua.
			using HK_PlayerData_OnStorageOpComplete =
				HookPlate::FastcallHook<"PlayerData_OnStorageOpComplete", void*,
					unsigned int, int, unsigned int, unsigned int*>;
			Memory::MinHook<Game::Functions::PlayerData_OnStorageOpCompleteT>*
				m_PlayerData_OnStorageOpCompleteHK{};

			// THE FIX for the director-menu nil, not an instrument. On its first call the detour
			// rewrites every dwuser(1) def to hdd(0) — the same 4-byte store the engine's own
			// dvar_playerData_forceLocalStorage path performs in PlayerData_Init, just applied at the
			// last moment where it still has an effect.
			//
			// Why the tick and not PlayerData_Init's own window: that was tried and MEASURED not to
			// fire. Init runs before PostArxanDetectionHooks, so the detour was installed on a
			// function that had already been called. The tick is self-timing — it IS the thing that
			// submits each location, so its first call necessarily precedes location 0 going out.
			//
			// Why not just set the dvar: the dump's slot for it resolves to a dvar_t whose hash is
			// r_norefresh, so its runtime identity is unproven. This needs no dvar.
			//
			// Anything later is permanently too late: the tick latches a per-location "submitted"
			// flag, so once location 0 has gone out a def rewritten afterwards is never revisited.
			//
			// Online boots, plus offline/LAN boots while ZM progression is on (game/zm_progression.hpp):
			// the progression maps 5/18/19 are dwuser too. The detour logs the pass-through as well, so
			// a boot with both off still shows it ran and chose not to act.
			using HK_PlayerData_ControllerStorageTick =
				HookPlate::FastcallHook<"PlayerData_ControllerStorageTick", char, unsigned int>;
			Memory::MinHook<Game::Functions::PlayerData_ControllerStorageTickT>*
				m_PlayerData_ControllerStorageTickHK{};

			// --- ZM progression (game/zm_progression.hpp) -------------------------------------------
			// Installed only when ZmProgression::Enabled() ("progression" on, any boot profile).
			// The two gates answer true in a Zombies session; the engine then records XP and stats
			// itself. Every original is called through an Arxan return thunk.
			using HK_LiveStorage_AreMatchStatsEnabled = HookPlate::FastcallHook<"LiveStorage_AreMatchStatsEnabled",
				bool, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>;
			Memory::MinHook<Game::Functions::StatsGateT>* m_LiveStorage_AreMatchStatsEnabledHK{};
			using HK_GScr_AreStatWritesEnabled = HookPlate::FastcallHook<"GScr_AreStatWritesEnabled",
				bool, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>;
			Memory::MinHook<Game::Functions::StatsGateT>* m_GScr_AreStatWritesEnabledHK{};
			// Connect-time prematch copy. Stamps player_xuid, predicts the copy, and if the forced copy
			// still fails, reruns it unforced so the match loads, and taints the controller so its
			// commit cannot write a blank-based result over the real save.
			using HK_LiveStorage_BeginStatsTransfer = HookPlate::FastcallHook<"LiveStorage_BeginStatsTransfer",
				char, int>;
			Memory::MinHook<Game::Functions::LiveStorage_BeginStatsTransferT>* m_LiveStorage_BeginStatsTransferHK{};
			// Match-end commit: logs every precondition, re-stamps player_xuid, marks the DW signature
			// in, and skips a tainted controller.
			using HK_LiveStorage_CommitStatsTransfer = HookPlate::FastcallHook<"LiveStorage_CommitStatsTransfer",
				std::uint64_t, int, int, unsigned int, char>;
			Memory::MinHook<Game::Functions::LiveStorage_CommitStatsTransferT>* m_LiveStorage_CommitStatsTransferHK{};
			// Read-only XP transcript: server rankxp before/after each award.
			using HK_G_AddPlayerRankXp = HookPlate::FastcallHook<"G_AddPlayerRankXp",
				std::uint64_t, std::int16_t, std::int64_t, std::int64_t, std::uint64_t>;
			Memory::MinHook<Game::Functions::G_AddPlayerRankXpT>* m_G_AddPlayerRankXpHK{};
			// The AE block (account XP, level, AAR): when the engine has no Demonware block, these
			// three hand out a local one whose base_xp mirrors the saved rankxp. When it has one (an
			// online boot), they first fill it from the same local save.
			using HK_LiveStats_GetLiveUserStatsInstance = HookPlate::FastcallHook<"LiveStats_GetLiveUserStatsInstance",
				void*, int>;
			Memory::MinHook<Game::Functions::LiveStats_GetLiveUserStatsInstanceT>* m_LiveStats_GetLiveUserStatsInstanceHK{};
			using HK_LiveStats_GetAeRootStateSlot = HookPlate::FastcallHook<"LiveStats_GetAeRootStateSlot",
				void*, int>;
			Memory::MinHook<Game::Functions::LiveStats_GetAeRootStateSlotT>* m_LiveStats_GetAeRootStateSlotHK{};
			using HK_Lua_PushAeSyncBuffer = HookPlate::FastcallHook<"Lua_PushAeSyncBuffer",
				std::uint64_t, void*, int>;
			Memory::MinHook<Game::Functions::Lua_PushAeSyncBufferT>* m_Lua_PushAeSyncBufferHK{};
			// Gun XP and camo challenges: fills the server's AE slot for the local player from the
			// saved local ae_sync block, right before the engine reads it (dump_anchors.hpp).
			using HK_SV_ClientStatsReady = HookPlate::FastcallHook<"SV_ClientStatsReady", char, std::uint8_t*>;
			Memory::MinHook<Game::Functions::SV_ClientStatsReadyT>* m_SV_ClientStatsReadyHK{};
			// The menus' copy of the lobby network mode: LAN is shown to the Lua as LIVE, so the
			// progression UI (locks, gun levels, hidden camos) runs. The engine stays LAN.
			using HK_LobbyRoot_SetNetworkModeModel = HookPlate::FastcallHook<"LobbyRoot_SetNetworkModeModel",
				std::uint64_t, int>;
			Memory::MinHook<Game::Functions::LobbyRoot_SetNetworkModeModelT>* m_LobbyRoot_SetNetworkModeModelHK{};
			// Engine.GameModeIsMode: the two CACUtility progression gates are told the LAN lobby is not
			// a custom game, so Create-a-Class shows gun levels and asks the engine about locks.
			using HK_Lua_GameModeIsMode_Impl = HookPlate::FastcallHook<"Lua_GameModeIsMode_Impl",
				std::uint64_t, void*, int>;
			Memory::MinHook<Game::Functions::Lua_GameModeIsMode_ImplT>* m_Lua_GameModeIsMode_ImplHK{};
			// Engine.GetLobbyNetworkMode: read-only for now, logs which Lua asks (step 2 recon).
			using HK_Lua_Engine_GetLobbyNetworkMode = HookPlate::FastcallHook<"Lua_Engine_GetLobbyNetworkMode",
				std::uint64_t, void*>;
			Memory::MinHook<Game::Functions::Lua_Engine_GetLobbyNetworkModeT>* m_Lua_Engine_GetLobbyNetworkModeHK{};

			// --- "unlock_all" (game/unlock_all.hpp) ---------------------------------------------------
			// Installed only when UnlockAll::Enabled() ("unlock_all": true in cw-mod.json). Each asks the
			// engine first and changes only a "locked" / "not owned" answer; dump_anchors.hpp says what
			// each one decides. Nothing is written to a save.
			// Level, gun-level and challenge locks:
			using HK_Progression_IsItemLocked = HookPlate::FastcallHook<"Progression_IsItemLocked",
				bool, int, unsigned int, int>;
			Memory::MinHook<Game::Functions::Progression_IsItemLockedT>* m_Progression_IsItemLockedHK{};
			using HK_Progression_IsAttachmentLockedInBlock = HookPlate::FastcallHook<"Progression_IsAttachmentLockedInBlock",
				char, int, void*, unsigned int, int, char>;
			Memory::MinHook<Game::Functions::Progression_IsAttachmentLockedInBlockT>* m_Progression_IsAttachmentLockedInBlockHK{};
			using HK_Progression_IsAttachmentSlotLocked = HookPlate::FastcallHook<"Progression_IsAttachmentSlotLocked",
				bool, unsigned int, unsigned int, unsigned int, int>;
			Memory::MinHook<Game::Functions::Progression_IsAttachmentSlotLockedT>* m_Progression_IsAttachmentSlotLockedHK{};
			using HK_Progression_IsItemOptionLockedCore = HookPlate::FastcallHook<"Progression_IsItemOptionLockedCore",
				char, unsigned int, unsigned int, unsigned int, unsigned int, char>;
			Memory::MinHook<Game::Functions::Progression_IsItemOptionLockedCoreT>* m_Progression_IsItemOptionLockedCoreHK{};
			// The engine's "progression rules are off" test, answered yes to the unlockables module only.
			using HK_Com_SessionMode_IsProgressionExemptContext =
				HookPlate::FastcallHook<"Com_SessionMode_IsProgressionExemptContext", bool>;
			Memory::MinHook<Game::Functions::Com_SessionMode_IsProgressionExemptContextT>*
				m_Com_SessionMode_IsProgressionExemptContextHK{};
			// Ownership: the marketplace inventory, which never loads on the local backend.
			using HK_Inventory_GetItemQuantity = HookPlate::FastcallHook<"Inventory_GetItemQuantity",
				std::uint64_t, int, unsigned int>;
			Memory::MinHook<Game::Functions::Inventory_GetItemQuantityT>* m_Inventory_GetItemQuantityHK{};
			using HK_Loot_GetItemQuantity = HookPlate::FastcallHook<"Loot_GetItemQuantity",
				std::uint64_t, unsigned int, std::uint64_t, std::uint64_t, std::uint64_t>;
			Memory::MinHook<Game::Functions::Loot_GetItemQuantityT>* m_Loot_GetItemQuantityHK{};
			using HK_Entitlement_IsOwned = HookPlate::FastcallHook<"Entitlement_IsOwned", char, int, std::uint64_t>;
			Memory::MinHook<Game::Functions::Entitlement_IsOwnedT>* m_Entitlement_IsOwnedHK{};
			using HK_DwFetch_IsInventoryReady = HookPlate::FastcallHook<"DwFetch_IsInventoryReady", bool, unsigned int>;
			Memory::MinHook<Game::Functions::DwFetch_IsInventoryReadyT>* m_DwFetch_IsInventoryReadyHK{};
			// The battle pass: owned, top tier.
			using HK_Loot_GetBattlePassOwned = HookPlate::FastcallHook<"Loot_GetBattlePassOwned", char, unsigned int, int>;
			Memory::MinHook<Game::Functions::Loot_GetBattlePassOwnedT>* m_Loot_GetBattlePassOwnedHK{};
			using HK_Loot_GetBattlePassRank = HookPlate::FastcallHook<"Loot_GetBattlePassRank",
				std::uint64_t, unsigned int, int>;
			Memory::MinHook<Game::Functions::Loot_GetBattlePassRankT>* m_Loot_GetBattlePassRankHK{};

			// --- Local LPC playlists (game/local_lpc.hpp) --------------------------------------------
			// Installed only when LocalLpc::Enabled(). A pass-through except while our playlists zones
			// load: then it counts allocations per asset type and logs the check that is about to
			// ERR_DROP, naming a failed zone signature when the engine's tamper response fired.
			using HK_DB_AllocXAssetEntry = HookPlate::FastcallHook<"DB_AllocXAssetEntry",
				void*, std::uint64_t, std::uint64_t>;
			Memory::MinHook<Game::Functions::DB_AllocXAssetEntryT>* m_DB_AllocXAssetEntryHK{};

			// --- mapkit custom maps (game/mapkit_loader.hpp) -----------------------------------------
			// Installed only when cw-mod/maps holds a map. Each is a pass-through except for OUR zones:
			// no signature check, no .fd, and the optional map-start redirect set in the Maps tab.
			using HK_DB_Signature_VerifyZone = HookPlate::FastcallHook<"DB_Signature_VerifyZone", void>;
			Memory::MinHook<Game::Functions::DB_Signature_VerifyZoneT>* m_DB_Signature_VerifyZoneHK{};
			using HK_FS_OpenFileRead = HookPlate::FastcallHook<"FS_OpenFileRead", void*, const char*, int, int>;
			Memory::MinHook<Game::Functions::FS_OpenFileReadT>* m_FS_OpenFileReadHK{};
			using HK_SV_StartMap = HookPlate::FastcallHook<"SV_StartMap", std::uint64_t,
				std::uint32_t, const char*, std::uint32_t, std::uint8_t, std::uint32_t>;
			Memory::MinHook<Game::Functions::SV_StartMapT>* m_SV_StartMapHK{};
			using HK_MapPreload_StartZoneRead = HookPlate::FastcallHook<"MapPreload_StartZoneRead", std::uint64_t,
				const char*, std::uint64_t, std::uint64_t, std::uint64_t>;
			Memory::MinHook<Game::Functions::MapPreload_StartZoneReadT>* m_MapPreload_StartZoneReadHK{};
			using HK_DB_LoadXAssets = HookPlate::FastcallHook<"DB_LoadXAssets", void,
				Game::Functions::XZoneInfo*, std::uint32_t, int>;
			Memory::MinHook<Game::Functions::DB_LoadXAssetsT>* m_DB_LoadXAssetsHK{};
			using HK_DB_ExpandZoneVariants = HookPlate::FastcallHook<"DB_ExpandZoneVariants", int,
				Game::Functions::XZoneInfo*, int, char*, Game::Functions::XZoneInfo*, int>;
			Memory::MinHook<Game::Functions::DB_ExpandZoneVariantsT>* m_DB_ExpandZoneVariantsHK{};
			// Logs which gfx_map the renderer starts the level with (mapkit's own, test G1, or the map's).
			using HK_R_InitWorld = HookPlate::FastcallHook<"R_InitWorld", void>;
			Memory::MinHook<Game::Functions::R_InitWorldT>* m_R_InitWorldHK{};
			// A map of its own: the host's lobby map becomes the picked map's id, and the maptable finds a map of its own
			// under its asset library's entry (getmapfields).
			using HK_Session_SetMapName = HookPlate::FastcallHook<"Session_SetMapName", char, std::uint32_t, const char*>;
			Memory::MinHook<Game::Functions::Session_SetMapNameT>* m_Session_SetMapNameHK{};
			using HK_MapTable_FindEntryByHash = HookPlate::FastcallHook<"MapTable_FindEntryByHash", void*, std::uint64_t>;
			Memory::MinHook<Game::Functions::MapTable_FindEntryByHashT>* m_MapTable_FindEntryByHashHK{};
			using HK_MapTable_GetMapFlags = HookPlate::FastcallHook<"MapTable_GetMapFlags", std::uint32_t, const char*>;
			Memory::MinHook<Game::Functions::MapTable_GetMapFlagsT>* m_MapTable_GetMapFlagsHK{};

			// --- mapkit asset-usage census (game/mapkit_usage.hpp) -----------------------------------
			// Installed only when cw-mod.json "mapkit_usage" is true. A pass-through that records which
			// bgcache-listed asset scripts and map entities look up (the DB_FindXAssetHeader half rides the
			// script loader's detour).
			using HK_BG_Cache_FindIndex = HookPlate::FastcallHook<"BG_Cache_FindIndex", std::int64_t, std::uint8_t,
				std::uint64_t>;
			Memory::MinHook<Game::Functions::BG_Cache_FindIndexT>* m_BG_Cache_FindIndexHK{};

			// --- mapkit zone trace (game/mapkit_trace.hpp) -------------------------------------------
			// Installed only when cw-mod.json "mapkit_trace" names a zone. Pure pass-throughs that record
			// where each asset of a traced zone starts: the zone bracket, the per-asset call, and the two
			// stream readers (their byte count is the stream position).
			using HK_DB_LoadXFile_Internal = HookPlate::FastcallHook<"DB_LoadXFile_Internal", std::int64_t,
				std::int64_t, std::int64_t, const char*, int, void*, void*, std::uint64_t, int, int>;
			Memory::MinHook<Game::Functions::DB_LoadXFile_InternalT>* m_DB_LoadXFile_InternalHK{};
			using HK_Load_XAsset = HookPlate::FastcallHook<"Load_XAsset", std::int64_t, char, std::uint8_t*>;
			Memory::MinHook<Game::Functions::Load_XAssetT>* m_Load_XAssetHK{};
			using HK_Load_XAsset_Preload = HookPlate::FastcallHook<"Load_XAsset_Preload", std::int64_t, char,
				std::uint8_t*>;
			Memory::MinHook<Game::Functions::Load_XAssetT>* m_Load_XAsset_PreloadHK{};
			using HK_DB_ReadXFile = HookPlate::FastcallHook<"DB_ReadXFile", void, void*, int>;
			Memory::MinHook<Game::Functions::DB_ReadXFileT>* m_DB_ReadXFileHK{};
			using HK_DB_ReadXFileString = HookPlate::FastcallHook<"DB_ReadXFileString", void, std::uint8_t*,
				std::uint32_t*>;
			Memory::MinHook<Game::Functions::DB_ReadXFileStringT>* m_DB_ReadXFileStringHK{};

			// Battle.net fatal-error reporters. Booting in online session mode brings the BGS layer up
			// for real; with no Battle.net reachable it answers with an error that goes to Com_Error at
			// level 1024 — undismissable dialog, exit to desktop (BLZBNTBGS000003EA). Detoured to
			// log-and-return so the frontend we actually want to look at gets a chance to build.
			// Installed ONLY when cw-mod.json's mode was not offline at boot: on a normal offline launch a
			// genuine Battle.net fatal stays fatal. See dump_anchors.hpp.
			using HK_BnetError_ReportFatalUnguarded =
				HookPlate::FastcallHook<"BnetError_ReportFatalUnguarded", std::uint64_t, std::uint32_t>;
			Memory::MinHook<Game::Functions::BnetError_ReportFatalUnguardedT>*
				m_BnetError_ReportFatalUnguardedHK{};

			using HK_BnetError_ReportFatalIfSignedIn =
				HookPlate::FastcallHook<"BnetError_ReportFatalIfSignedIn", void, std::uintptr_t, std::uint32_t*>;
			Memory::MinHook<Game::Functions::BnetError_ReportFatalIfSignedInT>*
				m_BnetError_ReportFatalIfSignedInHK{};

			// The real fix for the BLZBNTBGS wall. The dialog is a LUI popup that QUERIES the stored
			// first-party error (see dump_anchors.hpp), which is why suppressing Com_Error left it on
			// screen and why it re-raises on every menu change. These two functions are the only ways
			// into STATE_ERROR, so no-oping them means no error is ever stored to query. Installed
			// only on an online-mode test boot.
			using HK_FirstParty_SetError =
				HookPlate::FastcallHook<"FirstParty_SetError", std::uint64_t, std::uintptr_t, int>;
			Memory::MinHook<Game::Functions::FirstParty_SetErrorT>* m_FirstParty_SetErrorHK{};

			using HK_FirstParty_SetErrorState =
				HookPlate::FastcallHook<"FirstParty_SetErrorState", std::uint64_t, std::uintptr_t>;
			Memory::MinHook<Game::Functions::FirstParty_SetErrorStateT>* m_FirstParty_SetErrorStateHK{};

			// The third writer, and the one an online + backend boot actually hits: the BGS
			// "disconnected" callback. Runs the original, then clears the stored error so the title
			// screen has nothing to show (see FirstParty_SetError.cpp).
			using HK_FirstParty_OnBgsDisconnected = HookPlate::FastcallHook<"FirstParty_OnBgsDisconnected",
				std::uint64_t, std::uintptr_t, std::uintptr_t, const std::uint32_t*>;
			Memory::MinHook<Game::Functions::FirstParty_OnBgsDisconnectedT>* m_FirstParty_OnBgsDisconnectedHK{};

			// ...and the one that finally works. Measured: with the four above installed, the latched
			// reporter WAS suppressed, the two state writers never fired at all, and the popup still
			// appeared — because LiveUser_HandleSignOut raises it directly. Offline that same sign-out
			// is SILENT (LiveUser_BuildSignOutErrorMessage returns "no message" for signinState 2
			// unless Com_SessionMode_IsOnline), which is precisely why going online produced a wall.
			// Every producer funnels through this queue, so drop level 1024 here and return 1 so no
			// caller falls through to Com_Error. Installed always, inert unless
			// OnlineMode::g_SuppressBnetErrors. See dump_anchors.hpp.
			using HK_ErrorQueue_Push =
				HookPlate::FastcallHook<"ErrorQueue_Push", char, int, const char*, char>;
			Memory::MinHook<Game::Functions::ErrorQueue_PushT>* m_ErrorQueue_PushHK{};

			// ...and THIS is the one that produces the dialog. Measured: with the queue hook armed it
			// never fired once and the popup appeared anyway, because LiveUser_ForceSignOutAndFatal
			// calls Com_Error(1024) directly. It is a "you must stay signed in" watchdog hanging off
			// LiveFirstParty_Frame, and reaching it is already unrecoverable — it signs both
			// controllers out on the way. No-oped while suppression is on. See dump_anchors.hpp.
			using HK_LiveUser_ForceSignOutAndFatal =
				HookPlate::FastcallHook<"LiveUser_ForceSignOutAndFatal", char*>;
			Memory::MinHook<Game::Functions::LiveUser_ForceSignOutAndFatalT>*
				m_LiveUser_ForceSignOutAndFatalHK{};

			// ?????? hooks
			using HK_CreateMutexExA = HookPlate::StdcallHook<"CreateMutexExA", HANDLE,
				const LPSECURITY_ATTRIBUTES, const LPCSTR, const DWORD, const DWORD>;
			Memory::MinHook<decltype(CreateMutexExA)>* m_CreateMutexExAHK;

			// Keyboard input while the overlay is open. Swallowing WM_KEY* in our WndProc is not
			// enough: T9 imports GetRawInputBuffer (USER32) and drains the raw-input queue itself,
			// which never goes near the window procedure - so the mouse was correctly blocked and the
			// keyboard was not. This hook still calls the original (so the queue is drained and no
			// backlog is waiting when the overlay closes) but reports zero events to the game.
			using HK_GetRawInputBuffer = HookPlate::StdcallHook<"GetRawInputBuffer", UINT,
				const PRAWINPUT, const PUINT, const UINT>;
			Memory::MinHook<decltype(GetRawInputBuffer)>* m_GetRawInputBufferHK{};

			// The other half of the keyboard leak. Draining the raw-input queue was not enough: T9
			// also POLLS key state with GetAsyncKeyState, which asks the OS directly and has no queue
			// to starve. Answered as "not pressed" while the overlay is open, but only for callers
			// inside the game module so ImGui's own modifier handling is untouched.
			using HK_GetAsyncKeyState = HookPlate::StdcallHook<"GetAsyncKeyState", SHORT, const int>;
			Memory::MinHook<decltype(GetAsyncKeyState)>* m_GetAsyncKeyStateHK{};

			// --- The Demonware network choke point (see game/dw_net.hpp) --------------------------
			// IAT hooks rather than MinHook: T9 imports all three statically from WS2_32, so the
			// import table is the complete surface and patching it needs no code modification at all.
			// Installed unconditionally and inert by default — the detours consult DwNet's atomics,
			// which a normal offline launch leaves off.
			using HK_getaddrinfo = HookPlate::StdcallHook<"WS2_32/getaddrinfo", int,
				const char*, const char*, const addrinfo*, addrinfo**>;
			Memory::IAT* m_getaddrinfoHK{};

			using HK_gethostbyname = HookPlate::StdcallHook<"WS2_32/gethostbyname", hostent*,
				const char*>;
			Memory::IAT* m_gethostbynameHK{};

			using HK_connect = HookPlate::StdcallHook<"WS2_32/connect", int,
				SOCKET, const sockaddr*, int>;
			Memory::IAT* m_connectHK{};

			using HK_GetThreadContext = HookPlate::StdcallHook<"GetThreadContext", BOOL,
				const HANDLE, const LPCONTEXT>;
			Memory::MinHook<>* m_GetThreadContextHK;

			using HK_NtQueryInformationProcess = HookPlate::StdcallHook<"NtQueryInformationProcess", NTSTATUS,
				const HANDLE, const PROCESSINFOCLASS, const PVOID, const ULONG, const PULONG>;
			Memory::MinHook<>* m_NtQueryInformationProcessHK;

			// Event handlers
			HookPlate::EventHandlerStore m_EventHandlerStore{};
			static void OnShowOverStack();

			explicit Hooks();
			~Hooks();

			void PostArxanDetectionHooks();

			template <typename T>
			void DeleteHook(Memory::MinHook<T>** hook, std::vector<int> indexes = {}) {
				if (!hook || !*hook) {
					return;
				}

				if (indexes.empty()) {
					(*hook)->Unhook();
				}
				else {
					for (int index : indexes) {
						(*hook)->Unhook(index);
					}
				}

				delete* hook;
				*hook = nullptr;
			}

			void DeleteHook(Memory::IAT** hook) {
				if (!hook || !*hook) {
					return;
				}

				(*hook)->Unhook();

				delete* hook;
				*hook = nullptr;
			}
		};
	}

	inline std::unique_ptr<Hook::Hooks> g_Hooks{};
}
