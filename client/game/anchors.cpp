// Resolution of every build-locked engine anchor: the dump-absolute addresses in
// dump_anchors.hpp turned into live pointers on the Pointers instance.
#include "common.hpp"
#include "game/game.hpp"
#include "game/arxan_call.hpp"
#include "game/dump_anchors.hpp"
#include "game/game_internal.hpp"
#include <utility/nt.hpp>
#include <cstring>

namespace Client::Game {
	void Pointers::ResolveCoopAnchors(std::uintptr_t moduleBase) {
		const Common::Utility::NT::Library game;
		const std::uintptr_t size = game.GetOptionalHeader()->SizeOfImage;
		auto resolve = [&](std::uintptr_t dumpAbs) -> void* {
			const std::uintptr_t a = moduleBase + (dumpAbs - kDumpImagebase);
			return (a >= moduleBase && a < moduleBase + size) ? reinterpret_cast<void*>(a) : nullptr;
		};

		this->m_SV_DirectConnect                   = resolve(kDump_SV_DirectConnect);
		this->m_SV_Migration_ReseatClient_Loopback = resolve(kDump_SV_ReseatClientLoopback);
		this->m_g_svClients                        = reinterpret_cast<void**>(resolve(kDump_g_svClients));
		this->m_sv_migrationInProgress             = reinterpret_cast<bool*>(resolve(kDump_sv_migrationInProgress));
		this->m_SV_StageConnectMessage             = resolve(kDump_SV_StageConnectMessage);
		this->m_SV_UnstageConnectMessage           = resolve(kDump_SV_UnstageConnectMessage);
		this->m_Com_SessionMode_GetString          = resolve(kDump_Com_SessionMode_GetStr);
		this->m_g_netFieldChecksum                 = reinterpret_cast<int*>(resolve(kDump_g_netFieldChecksum));
		this->m_qportCounter                       = reinterpret_cast<std::uint16_t*>(resolve(kDump_qportCounter));

		// Lever-3 host-launch capture anchors — independent read-only diagnostics, NOT part of the
		// seat-path gate (CoopAnchorsResolved). Used only by DumpHostLaunchBlock.
		this->m_hostLaunchBlock                    = reinterpret_cast<std::uint8_t*>(resolve(kDump_HostLaunchBlock));
		this->m_g_hostLaunchPhase                  = reinterpret_cast<int*>(resolve(kDump_g_hostLaunchPhase));
		this->m_g_localPlayerCount                 = reinterpret_cast<int*>(resolve(kDump_g_localPlayerCount));
		this->m_GScr_LaunchP2P                     = resolve(kDump_GScr_LaunchP2P);
		this->m_GScr_LaunchP2P_Named               = resolve(kDump_GScr_LaunchP2P_Named);
		this->m_GScr_LaunchP2P_Full                = resolve(kDump_GScr_LaunchP2P_Full);
		this->m_LobbyHost_LaunchP2P_Plain          = resolve(kDump_LobbyHost_LaunchP2P_Plain);
		this->m_g_netSessionManager                = reinterpret_cast<void**>(resolve(kDump_g_netSessionManager));
		this->m_g_netSessionLaunchState            = reinterpret_cast<int*>(resolve(kDump_g_netSessionLaunchState));

		// Lever-4 client-join anchors. Independent of the seat-path gate; dormant until the menu acts.
		this->m_ClientSession_JoinKnownHost        = resolve(kDump_ClientSession_JoinKnownHost);
		this->m_ClientSession_StartJoin            = resolve(kDump_ClientSession_StartJoin);
		this->m_JoinCtx_AddHostCandidate           = resolve(kDump_JoinCtx_AddHostCandidate);
		this->m_ClientSession_JoinKick             = resolve(kDump_ClientSession_JoinKick);
		this->m_g_clientJoinCtx                    = reinterpret_cast<std::uint8_t*>(resolve(kDump_g_clientJoinCtx));
		this->m_cl_lobbyLaunchState                = reinterpret_cast<int*>(resolve(kDump_cl_lobbyLaunchState));

		// Lever-4 native path (XUID probe -> InfoResponse -> JoinPendingTarget).
		this->m_ClientSession_QueryHostByXuid      = resolve(kDump_ClientSession_QueryHostByXuid);
		this->m_ClientSession_JoinPendingTarget    = resolve(kDump_ClientSession_JoinPendingTarget);
		this->m_Session_GetSessionObject           = resolve(kDump_Session_GetSessionObject);

		this->m_pendingJoin_awaiting               = reinterpret_cast<bool*>(resolve(kDump_pendingJoin_awaiting));
		this->m_pendingJoin_nonce                  = reinterpret_cast<int*>(resolve(kDump_pendingJoin_nonce));
		this->m_pendingJoin_xuid                   = reinterpret_cast<std::int64_t*>(resolve(kDump_pendingJoin_xuid));
		this->m_pendingJoin_valid                  = reinterpret_cast<bool*>(resolve(kDump_pendingJoin_valid));
		this->m_pendingJoin_sessionId              = reinterpret_cast<std::int64_t*>(resolve(kDump_pendingJoin_sessionId));
		this->m_pendingJoin_hostName               = reinterpret_cast<std::uint8_t*>(resolve(kDump_pendingJoin_hostName));
		this->m_pendingJoin_slot                   = reinterpret_cast<int*>(resolve(kDump_pendingJoin_slot));
		this->m_pendingJoin_secId                  = reinterpret_cast<std::uint8_t*>(resolve(kDump_pendingJoin_secId));
		this->m_pendingJoin_secKey                 = reinterpret_cast<std::uint8_t*>(resolve(kDump_pendingJoin_secKey));
		this->m_pendingJoin_serializedAdr          = reinterpret_cast<std::uint8_t*>(resolve(kDump_pendingJoin_serializedAdr));

		// Lever-5 netmsg transcript anchors. Dormant until the menu installs the detours.
		this->m_NetMsg_Dispatch                    = resolve(kDump_NetMsg_Dispatch);
		this->m_NetMsg_SendJoinResponse            = resolve(kDump_NetMsg_SendJoinResponse);
		this->m_Session_ParseJoinLobbyRequest      = resolve(kDump_Session_ParseJoinLobbyRequest);
		this->m_g_netMsgNames                      = reinterpret_cast<void**>(resolve(kDump_g_netMsgNames));
		this->m_g_netMsgHandlers                   = resolve(kDump_g_netMsgHandlers);
		this->m_g_joinResponseCode                 = reinterpret_cast<int*>(resolve(kDump_g_joinResponseCode));
		this->m_g_expectedHostAdr                  = reinterpret_cast<std::uint8_t*>(resolve(kDump_g_expectedHostAdr));

		// PlayerData anchors for the TEMPORARY dwuser -> hdd redirect hooks (delete with them at B4).
		this->m_g_playerDataDefsById               = reinterpret_cast<void**>(resolve(kDump_g_playerDataDefsById));
		this->m_g_playerDataStore                  = reinterpret_cast<std::uint8_t*>(resolve(kDump_g_playerDataStore));
		this->m_g_playerDataMapCallbacks           = reinterpret_cast<void**>(resolve(kDump_g_playerDataMapCallbacks));
		this->m_PlayerData_OnStorageOpComplete =
			resolve(kDump_PlayerData_OnStorageOpComplete);
		this->m_PlayerData_ControllerStorageTick =
			resolve(kDump_PlayerData_ControllerStorageTick);
		this->m_g_sessionModePacked                = reinterpret_cast<std::uint32_t*>(resolve(kDump_g_sessionModePacked));
			// Local Demonware backend: MinHook target for the login-flow selector (forced to 9 =
			// studio auth in PostArxanDetectionHooks when DwBackend is enabled). Plain address; the
			// detour is installed after Arxan settles, same as BB_Alert.
			this->m_Dw_GetLoginFlow = resolve(kDump_Dw_GetLoginFlow);
			this->m_Login_SetStatus = resolve(kDump_Login_SetStatus);
			this->m_DwLogin_BuildStudioToken = resolve(kDump_DwLogin_BuildStudioToken);
			// Fatal-LUI-error handler: logs the Lua message; suppresses the termination on online boots.
			this->m_LuiError_ReportFatal = resolve(kDump_LuiError_ReportFatal);
			// LUI chunk runner: skips ui/ffotd_tu<N>.lua when no ffotd zone put it in the luafile pool.
			this->m_LUI_RunFile = resolve(kDump_LUI_RunFile);
			// Every localized string passes through it: "ui_text" in cw-mod.json replaces them here.
			this->m_DecryptString = resolve(kDump_DecryptString);
			// iprintln / iprintlnbold payloads: mirrored into client.log and the overlay.
			this->m_Scr_ConstructMessageString = resolve(kDump_Scr_ConstructMessageString);
			// The openmenu replay (lui_menu.cpp).
			this->m_g_luiCtx = reinterpret_cast<void**>(resolve(kDump_g_luiCtx));
			this->m_LUI_DispatchAddMenuEvent = reinterpret_cast<Functions::LUI_DispatchAddMenuEventT*>(
				resolve(kDump_LUI_DispatchAddMenuEvent));
			this->m_LUI_GetRootName = reinterpret_cast<Functions::LUI_GetRootNameT*>(
				resolve(kDump_LUI_GetRootName));
			this->m_CL_LocalClientToController = reinterpret_cast<Functions::CL_LocalClientToControllerT*>(
				resolve(kDump_CL_LocalClientToController));
			this->m_UI_SetUiActive = reinterpret_cast<Functions::UI_SetUiActiveT*>(
				resolve(kDump_UI_SetUiActive));
			this->m_LUI_ProtectedCall = reinterpret_cast<Functions::LUI_ProtectedCallT*>(
				resolve(kDump_LUI_ProtectedCall));
			this->m_g_liveUserSystemActive =
				reinterpret_cast<std::uint8_t*>(resolve(kDump_g_liveUserSystemActive));

			// Login-driver anchors: read by DumpDwLoginState, plus the RA-guarded presence hook.
			this->m_Dvar_LiveConnectMode =
				reinterpret_cast<std::uintptr_t**>(resolve(kDump_dvar_liveConnectMode));
			this->m_g_liveUserLoginAllowed =
				reinterpret_cast<std::uint8_t*>(resolve(kDump_g_liveUserLoginAllowed));
			this->m_LiveUser_FirstPartyPresenceOk = resolve(kDump_LiveUser_FirstPartyPresenceOk);
			this->m_LiveUser_LoginDriver_TickBase =
				reinterpret_cast<std::uintptr_t>(resolve(kDump_LiveUser_LoginDriver_Tick));
			this->m_LiveUser_SetupIdentityBase =
				reinterpret_cast<std::uintptr_t>(resolve(kDump_LiveUser_SetupIdentity));
			this->m_LiveUser_AccountIdPickersBase =
				reinterpret_cast<std::uintptr_t>(resolve(kDump_LiveUser_AccountIdPickers));
			this->m_bdCommonAddr_Ctor = resolve(kDump_bdCommonAddr_Ctor);
			this->m_g_bdAddrEmpty = resolve(kDump_g_bdAddrEmpty);
			this->m_g_firstPartyManager =
				reinterpret_cast<std::uintptr_t**>(resolve(kDump_g_firstPartyManager));
			this->m_FirstParty_GetSession = resolve(kDump_FirstParty_GetSession);
			this->m_FirstParty_GetLocalUserIndex = resolve(kDump_FirstParty_GetLocalUserIndex);
			this->m_g_firstPartyManagerObj = resolve(kDump_g_firstPartyManagerObj);
			this->m_LiveUser_SignOutBuildDropMessage =
				resolve(kDump_LiveUser_SignOutBuildDropMessage);
			this->m_BdRemoteHttpTask_FinishRow = resolve(kDump_BdRemoteHttpTask_FinishRow);
			this->m_PublisherObjectsResource_Parse = resolve(kDump_PublisherObjectsResource_Parse);
			this->m_ObjectMetadata_ParseJson = resolve(kDump_ObjectMetadata_ParseJson);
			this->m_Lpc_WriteManifest = resolve(kDump_Lpc_WriteManifest);
			this->m_Lpc_OnListFailed = resolve(kDump_Lpc_OnListFailed);
			this->m_BdLobbyMsg_WriteHeader = resolve(kDump_BdLobbyMsg_WriteHeader);
			this->m_MtxSync_ShouldStart = resolve(kDump_MtxSync_ShouldStart);
			this->m_MtxSync_RequestBnetTokenZEUS = resolve(kDump_MtxSync_RequestBnetTokenZEUS);
			this->m_MtxSync_OnBnetToken = resolve(kDump_MtxSync_OnBnetToken);
			this->m_MtxSync_IsDoneOrInFlight = resolve(kDump_MtxSync_IsDoneOrInFlight);
			this->m_g_mtxSyncBackoff = resolve(kDump_g_mtxSyncBackoff);
			this->m_g_mtxSyncState = resolve(kDump_g_mtxSyncState);
			this->m_DwFetch_GetStatus = resolve(kDump_DwFetch_GetStatus);
			this->m_DwFetch_IsDone = resolve(kDump_DwFetch_IsDone);
			this->m_Lua_IsDemonwareFetchingDone_ImplBase =
				reinterpret_cast<std::uintptr_t>(resolve(kDump_Lua_IsDemonwareFetchingDone_Impl));
			this->m_LiveUser_IsTrial = resolve(kDump_LiveUser_IsTrial);
			this->m_LuaNative_PrintInfo = resolve(kDump_LuaNative_PrintInfo);
			this->m_LuaNative_PrintWarning = resolve(kDump_LuaNative_PrintWarning);
			this->m_LuaNative_PrintError = resolve(kDump_LuaNative_PrintError);
			this->m_LuaNative_ContentIsFullyInstalled = resolve(kDump_LuaNative_ContentIsFullyInstalled);
			this->m_LuaNative_ContentIsPlayable = resolve(kDump_LuaNative_ContentIsPlayable);
			this->m_LuaNative_ContentIsInstalling = resolve(kDump_LuaNative_ContentIsInstalling);
			this->m_LuaNative_GetDvarInt = resolve(kDump_LuaNative_GetDvarInt);
			this->m_LuaNative_IsKoreaMinor = resolve(kDump_LuaNative_IsKoreaMinor);
			this->m_LuaNative_IsLobbySlotLive = resolve(kDump_LuaNative_IsLobbySlotLive);

			// Battle.net fatal-error reporters. Resolved always; hooked only on an online-mode boot.
			this->m_BnetError_ReportFatalUnguarded = resolve(kDump_BnetError_ReportFatalUnguarded);
			this->m_BnetError_ReportFatalIfSignedIn = resolve(kDump_BnetError_ReportFatalIfSignedIn);
			this->m_g_bnetEverSignedInLatch =
				reinterpret_cast<std::uint8_t*>(resolve(kDump_g_bnetEverSignedInLatch));
			this->m_FirstParty_SetError = resolve(kDump_FirstParty_SetError);
			this->m_FirstParty_SetErrorState = resolve(kDump_FirstParty_SetErrorState);
			this->m_FirstParty_OnBgsDisconnected = resolve(kDump_FirstParty_OnBgsDisconnected);
			this->m_ErrorQueue_Push = resolve(kDump_ErrorQueue_Push);
			this->m_LiveUser_ForceSignOutAndFatal = resolve(kDump_LiveUser_ForceSignOutAndFatal);

		LOG("Pointers", INFO, "playerdata anchors: defs={} store={} callbacks={}",
			static_cast<void*>(this->m_g_playerDataDefsById), static_cast<void*>(this->m_g_playerDataStore),
			static_cast<void*>(this->m_g_playerDataMapCallbacks));

		LOG("Pointers", INFO, "coop anchors: SV_DirectConnect={} stage={} unstage={} sessionmode={} nfc={} qport={} -> {}",
			this->m_SV_DirectConnect, this->m_SV_StageConnectMessage, this->m_SV_UnstageConnectMessage,
			this->m_Com_SessionMode_GetString, static_cast<void*>(this->m_g_netFieldChecksum),
			static_cast<void*>(this->m_qportCounter),
			this->CoopAnchorsResolved() ? "all resolved" : "INCOMPLETE (build mismatch?)");
		LOG("Pointers", INFO, "host-launch capture anchors: block={} phase={} players={}",
			static_cast<void*>(this->m_hostLaunchBlock), static_cast<void*>(this->m_g_hostLaunchPhase),
			static_cast<void*>(this->m_g_localPlayerCount));
		LOG("Pointers", INFO, "client-join anchors: JoinKnownHost={} StartJoin={} AddHostCandidate={} ctx={}",
			this->m_ClientSession_JoinKnownHost, this->m_ClientSession_StartJoin,
			this->m_JoinCtx_AddHostCandidate, static_cast<void*>(this->m_g_clientJoinCtx));
		LOG("Pointers", INFO, "client-join native path: QueryHostByXuid={} JoinPendingTarget={} GetSessionObject={} pendingValid={}",
			this->m_ClientSession_QueryHostByXuid, this->m_ClientSession_JoinPendingTarget,
			this->m_Session_GetSessionObject, static_cast<void*>(this->m_pendingJoin_valid));
		LOG("Pointers", INFO, "netmsg transcript anchors: Dispatch={} SendJoinResponse={} names={} handlers={} respCode={}",
			this->m_NetMsg_Dispatch, this->m_NetMsg_SendJoinResponse,
			static_cast<void*>(this->m_g_netMsgNames), this->m_g_netMsgHandlers,
			static_cast<void*>(this->m_g_joinResponseCode));
	}

	bool Pointers::CoopAnchorsResolved() const {
		return this->m_SV_DirectConnect
			&& this->m_SV_Migration_ReseatClient_Loopback
			&& this->m_g_svClients
			&& this->m_sv_migrationInProgress
			&& this->m_SV_StageConnectMessage
			&& this->m_SV_UnstageConnectMessage
			&& this->m_Com_SessionMode_GetString
			&& this->m_g_netFieldChecksum
			&& this->m_qportCounter;
	}

	void Pointers::ResolveLuaApi(std::uintptr_t moduleBase) {
		// Anchor addresses live in dump_anchors.hpp, same convention as every other anchor.

		const Common::Utility::NT::Library game;
		const std::uintptr_t size = game.GetOptionalHeader()->SizeOfImage;

		if (!ArxanCall::Init(moduleBase, size)) {
			LOG("Pointers", ERROR, "Lua C API left unresolved: no verified Arxan return gadget. "
				"Calling these directly would silently do nothing, so they stay null.");
			return;
		}

		auto wrap = [&](std::uintptr_t dumpAbs) -> void* {
			const std::uintptr_t a = moduleBase + (dumpAbs - kDumpImagebase);
			if (a < moduleBase || a >= moduleBase + size) return nullptr;
			return ArxanCall::MakeThunk(reinterpret_cast<void*>(a));
		};

		this->m_lua_getfield  = reinterpret_cast<Functions::lua_getfieldT*>(wrap(kDump_lua_getfield));
		this->m_lua_gettable  = reinterpret_cast<Functions::lua_gettableT*>(wrap(kDump_lua_gettable));
		this->m_lua_rawgeti   = reinterpret_cast<Functions::lua_rawgetiT*>(wrap(kDump_lua_rawgeti));
		this->m_lua_next      = reinterpret_cast<Functions::lua_nextT*>(wrap(kDump_lua_next));
		this->m_lua_pushvalue = reinterpret_cast<Functions::lua_pushvalueT*>(wrap(kDump_lua_pushvalue));
		this->m_lua_type      = reinterpret_cast<Functions::lua_typeT*>(wrap(kDump_lua_type));
		this->m_lua_tolstring = reinterpret_cast<Functions::lua_tolstringT*>(wrap(kDump_lua_tolstring));
		this->m_lua_tonumber  = reinterpret_cast<Functions::lua_tonumberT*>(wrap(kDump_lua_tonumber));
		this->m_lua_getmetatable =
			reinterpret_cast<Functions::lua_getmetatableT*>(wrap(kDump_lua_getmetatable));
		// For OpenLuiOverlay's params table.
		this->m_lua_createtable = reinterpret_cast<Functions::lua_createtableT*>(wrap(kDump_lua_createtable));
		this->m_lua_setfield    = reinterpret_cast<Functions::lua_setfieldT*>(wrap(kDump_lua_setfield));
		// Our own menu Lua (game/ui_scripts.cpp).
		this->m_lua_load        = reinterpret_cast<Functions::lua_loadT*>(wrap(kDump_lua_load));
		// The debug pair, for describing an erroring Lua call stack from the luaL_traceback detour.
		// Thunked for the same reason as everything above: called raw they would return 0 having
		// done nothing, and "lua_getstack said there are no frames" is exactly the kind of confident
		// empty answer that sends you looking in the wrong place.
		this->m_lua_getstack  = reinterpret_cast<Functions::lua_getstackT*>(wrap(kDump_lua_getstack));
		this->m_lua_getinfo   = reinterpret_cast<Functions::lua_getinfoT*>(wrap(kDump_lua_getinfo));
		// The hook targets stay UNwrapped - MinHook has to patch the real entry point.
		{
			const std::uintptr_t a = moduleBase + (kDump_luaL_traceback - kDumpImagebase);
			if (a >= moduleBase && a < moduleBase + size) {
				this->m_luaL_traceback = reinterpret_cast<void*>(a);
			}
		}
		{
			const std::uintptr_t a = moduleBase + (kDump_luaG_getobjname - kDumpImagebase);
			const std::uintptr_t s = moduleBase + (kDump_aXhashfuncNoName - kDumpImagebase);
			// Both must land in-module, and the literal must still read as "xhashfunc". That last
			// check is the point: this anchor is used to decide whether to WRITE a pointer into an
			// engine struct, so a build whose .rdata has shifted must disable the repair rather
			// than misidentify some other return value and corrupt a name that was fine.
			if (a >= moduleBase && a < moduleBase + size && s >= moduleBase && s < moduleBase + size) {
				char probe[10]{};
				if (SafeCopy(probe, reinterpret_cast<const void*>(s), sizeof(probe))
					&& std::memcmp(probe, "xhashfunc", 10) == 0) {
					this->m_luaG_getobjname = reinterpret_cast<void*>(a);
					this->m_luaG_getobjname_NoNameResult = reinterpret_cast<const char*>(s);
				}
			}
		}

		// The one playerdata call we make, from the redirect hook (seeds defaults for redirected maps).
		// Thunked: it may carry the Arxan caller check, and a guarded no-op would look like success.
		this->m_PlayerData_ResetBufferToDefaults =
			reinterpret_cast<Functions::PlayerData_ResetBufferToDefaultsT*>(wrap(kDump_PlayerData_ResetBufferToDefaults));

		// String dvar writes (WriteDvarString). Thunked for the same reason.
		this->m_Dvar_StringToValue =
			reinterpret_cast<Functions::Dvar_StringToValueT*>(wrap(kDump_Dvar_StringToValue));
		this->m_Dvar_ApplyValueInternal =
			reinterpret_cast<Functions::Dvar_ApplyValueInternalT*>(wrap(kDump_Dvar_ApplyValueInternal));

		// Plain globals.
		auto global = [&](std::uintptr_t dumpAbs) -> void* {
			const std::uintptr_t a = moduleBase + (dumpAbs - kDumpImagebase);
			if (a < moduleBase || a >= moduleBase + size) return nullptr;
			return reinterpret_cast<void*>(a);
		};
		this->m_g_liveUserObjects =
			reinterpret_cast<std::uintptr_t*>(global(kDump_g_liveUserObjects));
		this->m_g_dvarAllowServerFlaggedWrites =
			reinterpret_cast<std::uint8_t*>(global(kDump_g_dvarAllowServerFlaggedWrites));

		LOG("Pointers", INFO, "lua api (via arxan thunks): getfield={} next={} type={} tolstring={} -> {}",
			reinterpret_cast<void*>(this->m_lua_getfield), reinterpret_cast<void*>(this->m_lua_next),
			reinterpret_cast<void*>(this->m_lua_type), reinterpret_cast<void*>(this->m_lua_tolstring),
			this->LuaApiResolved() ? "all resolved" : "INCOMPLETE (build mismatch?)");
	}
}
