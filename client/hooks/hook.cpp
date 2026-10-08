#include "common.hpp"
#include "hooks/hook.hpp"
#include "memory/memory.hpp"
#include "game/game.hpp"
#include "game/arxan_call.hpp"
#include "game/dw_backend.hpp"
#include "game/dw_net.hpp"
#include "game/dump_anchors.hpp"
#include "game/local_lpc.hpp"
#include "game/mapkit_loader.hpp"
#include "game/mapkit_trace.hpp"
#include "game/mapkit_usage.hpp"
#include "game/settings.hpp"
#include "game/unlock_all.hpp"
#include "game/zm_progression.hpp"
#include "overlay/d3d12_hook.hpp"
#include "scripting/scripting.hpp"

#include <utility/memory.hpp>
#include <utility/nt.hpp>

#include <filesystem>

namespace Client::Hook {
	Hooks::Hooks() {
		this->m_SetUnhandledExceptionFilterHK = new Memory::IAT("kernel32.dll", "SetUnhandledExceptionFilter");
		this->m_SetUnhandledExceptionFilterHK->Hook<HK_SetUnhandledExceptionFilter>();

		this->m_RtlDispatchExceptionHK = new Memory::MinHook(g_Pointers->m_RtlDispatchException);
		this->m_RtlDispatchExceptionHK->Hook<HK_RtlDispatchException>();

		this->m_CreateMutexExAHK = new Memory::MinHook(CreateMutexExA);
		this->m_CreateMutexExAHK->Hook<HK_CreateMutexExA>();

		this->m_GetRawInputBufferHK = new Memory::MinHook(GetRawInputBuffer);
		this->m_GetRawInputBufferHK->Hook<HK_GetRawInputBuffer>();

		this->m_GetAsyncKeyStateHK = new Memory::MinHook(GetAsyncKeyState);
		this->m_GetAsyncKeyStateHK->Hook<HK_GetAsyncKeyState>();

		this->m_GetThreadContextHK = new Memory::MinHook("kernelbase.dll", "GetThreadContext");
		this->m_GetThreadContextHK->Hook<HK_GetThreadContext>();

		this->m_NtQueryInformationProcessHK = new Memory::MinHook("ntdll.dll", "NtQueryInformationProcess");
		this->m_NtQueryInformationProcessHK->Hook<HK_NtQueryInformationProcess>();

		// The Demonware choke point. Installed before anything can resolve a name, and inert until
		// DwNet::Init sets its switches — so on a normal offline launch these are three extra
		// predictable branches on a path the game barely uses. See game/dw_net.hpp for why this
		// replaces the hosts-file redirect but NOT the local CA.
		Game::DwNet::Init();

		this->m_getaddrinfoHK = new Memory::IAT("ws2_32.dll", "getaddrinfo");
		this->m_getaddrinfoHK->Hook<HK_getaddrinfo>();

		this->m_gethostbynameHK = new Memory::IAT("ws2_32.dll", "gethostbyname");
		this->m_gethostbynameHK->Hook<HK_gethostbyname>();

		this->m_connectHK = new Memory::IAT("ws2_32.dll", "connect");
		this->m_connectHK->Hook<HK_connect>();

		this->m_EventHandlerStore.AddHandler("OnShowOverStack", 0x0000000000000001, g_Pointers->m_Dvar_ShowOverStack, OnShowOverStack);
	}

	Hooks::~Hooks() {
		this->DeleteHook(&this->m_RtlDispatchExceptionHK);
		this->DeleteHook(&this->m_SetUnhandledExceptionFilterHK);
	}

	void Hooks::PostArxanDetectionHooks() {
		this->m_BB_AlertHK = new Memory::MinHook(g_Pointers->m_BB_Alert);
		this->m_BB_AlertHK->Hook<HK_BB_Alert>();

		// Logs fatal LUI errors. Installed always: the detour only diverges from the original on an
		// online boot (see hook.hpp), so normal LUI failures still report exactly as they did.
		if (g_Pointers->m_LuiError_ReportFatal) {
			this->m_LuiError_ReportFatalHK = new Memory::MinHook<Game::Functions::LuiError_ReportFatalT>(
				reinterpret_cast<Game::Functions::LuiError_ReportFatalT*>(g_Pointers->m_LuiError_ReportFatal));
			this->m_LuiError_ReportFatalHK->Hook<HK_LuiError_ReportFatal>();
		}

		// Skips a missing ui/ffotd chunk (a sys_error, not a Lua error). Pass-through for every other chunk.
		if (g_Pointers->m_LUI_RunFile) {
			this->m_LUI_RunFileHK = new Memory::MinHook<Game::Functions::LUI_RunFileT>(
				reinterpret_cast<Game::Functions::LUI_RunFileT*>(g_Pointers->m_LUI_RunFile));
			this->m_LUI_RunFileHK->Hook<HK_LUI_RunFile>();
			if (*static_cast<std::uint8_t*>(g_Pointers->m_LUI_RunFile) == 0xE9) {
				LOG("Hooks", INFO, "LUI_RunFile detour live: a missing ui/ffotd chunk is skipped instead of "
					"ending the process at map load.");
			}
			else {
				LOG("Hooks", ERROR, "LUI_RunFile detour NOT live: a missing ui/ffotd chunk will still end the process.");
			}
		}

		// UI text replacement ("ui_text" / "ui_text_log" in cw-mod.json). Every decrypted engine string
		// pays for the detour, so it only goes in when there is something for it to do.
		if (const auto& settings = Game::Settings::Get(); !settings.uiText.empty() || settings.uiTextLog) {
			if (!g_Pointers->m_DecryptString) {
				LOG("Hooks", ERROR, "DecryptString anchor did not resolve: \"ui_text\" does nothing this boot.");
			}
			else {
				BuildUiTextTable();
				this->m_DecryptStringHK = new Memory::MinHook<Game::Functions::DecryptStringT>(
					static_cast<Game::Functions::DecryptStringT*>(g_Pointers->m_DecryptString));
				this->m_DecryptStringHK->Hook<HK_DecryptString>();
				if (*static_cast<std::uint8_t*>(g_Pointers->m_DecryptString) == 0xE9) {
					LOG("Hooks", INFO, "UI text detour live: {} replacement(s){}.", settings.uiText.size(),
						settings.uiTextLog ? ", every UI string listed once as a (UiText) line" : "");
				}
				else {
					LOG("Hooks", ERROR, "UI text detour NOT live: \"ui_text\" does nothing this boot.");
				}
			}
		}

		// Script prints (iprintln / iprintlnbold) into client.log and the overlay. Read-only.
		if (g_Pointers->m_Scr_ConstructMessageString) {
			this->m_Scr_ConstructMessageStringHK = new Memory::MinHook<Game::Functions::Scr_ConstructMessageStringT>(
				static_cast<Game::Functions::Scr_ConstructMessageStringT*>(g_Pointers->m_Scr_ConstructMessageString));
			this->m_Scr_ConstructMessageStringHK->Hook<HK_Scr_ConstructMessageString>();
			if (*static_cast<std::uint8_t*>(g_Pointers->m_Scr_ConstructMessageString) == 0xE9) {
				LOG("Hooks", INFO, "Script print mirror live: iprintln/iprintlnbold text goes to (Script) lines and the overlay.");
			}
			else {
				LOG("Hooks", ERROR, "Script print mirror NOT live.");
			}
		}

		// The lobby Lua's own narration (Engine.PrintInfo/Warning/Error, compiled out of retail) into
		// client.log. Installed always: an offline boot's transcript is the baseline the online one is
		// diffed against. Read-only (each detour calls the original first); capped and de-duplicated
		// in the impl. Opt out: "lua_print": false in cw-mod.json. PrintInfo alone is also installed for
		// "ui_scripts": it carries our own menu Lua's commands (LuaPrint_Transcript.cpp).
		{
			const bool luaPrint = Game::Settings::Get().luaPrint;
			bool printInfoLive = false;
			if ((luaPrint || Game::Settings::Get().uiScripts) && g_Pointers->m_LuaNative_PrintInfo) {
				this->m_LuaNative_PrintInfoHK = new Memory::MinHook<Game::Functions::LuaNativeT>(
					static_cast<Game::Functions::LuaNativeT*>(g_Pointers->m_LuaNative_PrintInfo));
				this->m_LuaNative_PrintInfoHK->Hook<HK_LuaNative_PrintInfo>();
				printInfoLive = *static_cast<std::uint8_t*>(g_Pointers->m_LuaNative_PrintInfo) == 0xE9;
			}
			if (!luaPrint) {
				if (printInfoLive) {
					LOG("Hooks", INFO, "\"lua_print\" is off in cw-mod.json: no Lua print transcript. Engine.PrintInfo is "
						"detoured anyway, for the ui_scripts commands.");
				}
				else if (Game::Settings::Get().uiScripts) {
					LOG("Hooks", ERROR, "Engine.PrintInfo detour NOT live: ui_scripts commands (the CUSTOM MAPS pick) are lost.");
				}
				else {
					LOG("Hooks", INFO, "\"lua_print\" is off in cw-mod.json: Lua print transcript not installed.");
				}
			}
			else {
				int printArmed = printInfoLive ? 1 : 0;
				auto hookPrint = [&](void* target, auto*& member, auto install) {
					if (!target) return;
					install(target, member);
					printArmed += *static_cast<std::uint8_t*>(target) == 0xE9;
				};
				hookPrint(g_Pointers->m_LuaNative_PrintWarning, this->m_LuaNative_PrintWarningHK, [](void* t, auto*& m) {
					m = new Memory::MinHook<Game::Functions::LuaNativeT>(static_cast<Game::Functions::LuaNativeT*>(t));
					m->template Hook<HK_LuaNative_PrintWarning>();
				});
				hookPrint(g_Pointers->m_LuaNative_PrintError, this->m_LuaNative_PrintErrorHK, [](void* t, auto*& m) {
					m = new Memory::MinHook<Game::Functions::LuaNativeT>(static_cast<Game::Functions::LuaNativeT*>(t));
					m->template Hook<HK_LuaNative_PrintError>();
				});
				if (printArmed == 3) {
					LOG("Hooks", INFO, "Lua print transcript armed (Engine.PrintInfo/Warning/Error -> (LuaPrint) lines).");
				}
				else {
					LOG("Hooks", ERROR, "Lua print transcript only {}/3 detours live; missing (LuaPrint) lines prove nothing.",
						printArmed);
				}

				// The mode-tile lock predicates (B6 padlocks), same wrapper shape, same opt-out.
				int gateArmed = 0;
				auto hookGate = [&](void* target, auto*& member, auto install) {
					if (!target) return;
					install(target, member);
					gateArmed += *static_cast<std::uint8_t*>(target) == 0xE9;
				};
				hookGate(g_Pointers->m_LuaNative_ContentIsFullyInstalled, this->m_LuaNative_ContentIsFullyInstalledHK, [](void* t, auto*& m) {
					m = new Memory::MinHook<Game::Functions::LuaNativeT>(static_cast<Game::Functions::LuaNativeT*>(t));
					m->template Hook<HK_LuaNative_ContentIsFullyInstalled>();
				});
				hookGate(g_Pointers->m_LuaNative_ContentIsPlayable, this->m_LuaNative_ContentIsPlayableHK, [](void* t, auto*& m) {
					m = new Memory::MinHook<Game::Functions::LuaNativeT>(static_cast<Game::Functions::LuaNativeT*>(t));
					m->template Hook<HK_LuaNative_ContentIsPlayable>();
				});
				hookGate(g_Pointers->m_LuaNative_ContentIsInstalling, this->m_LuaNative_ContentIsInstallingHK, [](void* t, auto*& m) {
					m = new Memory::MinHook<Game::Functions::LuaNativeT>(static_cast<Game::Functions::LuaNativeT*>(t));
					m->template Hook<HK_LuaNative_ContentIsInstalling>();
				});
				hookGate(g_Pointers->m_LuaNative_GetDvarInt, this->m_LuaNative_GetDvarIntHK, [](void* t, auto*& m) {
					m = new Memory::MinHook<Game::Functions::LuaNativeT>(static_cast<Game::Functions::LuaNativeT*>(t));
					m->template Hook<HK_LuaNative_GetDvarInt>();
				});
				hookGate(g_Pointers->m_LuaNative_IsKoreaMinor, this->m_LuaNative_IsKoreaMinorHK, [](void* t, auto*& m) {
					m = new Memory::MinHook<Game::Functions::LuaNativeT>(static_cast<Game::Functions::LuaNativeT*>(t));
					m->template Hook<HK_LuaNative_IsKoreaMinor>();
				});
				hookGate(g_Pointers->m_LuaNative_IsLobbySlotLive, this->m_LuaNative_IsLobbySlotLiveHK, [](void* t, auto*& m) {
					m = new Memory::MinHook<Game::Functions::LuaNativeT>(static_cast<Game::Functions::LuaNativeT*>(t));
					m->template Hook<HK_LuaNative_IsLobbySlotLive>();
				});
				if (gateArmed == 6) {
					LOG("Hooks", INFO, "Mode-tile gate probe armed (content x3, mode dvars, Korea minor, lobby test -> (LuaGate) lines).");
				}
				else {
					LOG("Hooks", ERROR, "Mode-tile gate probe only {}/6 detours live; missing (LuaGate) lines prove nothing.",
						gateArmed);
				}
				InstallStartGateProbe();
			}
		}

		// Reads the erroring Lua call stack one instant before it is unwound. See hook.hpp; the
		// detour is a straight pass-through unless we are already in a boot we are instrumenting.
		if (g_Pointers->m_luaL_traceback) {
			this->m_luaL_tracebackHK = new Memory::MinHook<Game::Functions::luaL_tracebackT>(
				reinterpret_cast<Game::Functions::luaL_tracebackT*>(g_Pointers->m_luaL_traceback));
			this->m_luaL_tracebackHK->Hook<HK_luaL_traceback>();
			LOG("Hooks", INFO, "luaL_traceback hooked — Lua errors will report their live call stack.");
		}
		else {
			LOG("Hooks", WARN, "luaL_traceback did not resolve; Lua errors will report only the "
				"engine's own message.");
		}

		// Repairs the uninitialised ar.name that makes the engine's own traceback crash on any
		// hashed-name Lua error. Must be installed for the luaL_traceback detour above to be able to
		// call through to the original safely - if it is not, that detour skips the engine traceback
		// rather than walk into the fault, so this pair is deliberately ordered and both outcomes
		// are logged.
		if (g_Pointers->m_luaG_getobjname) {
			this->m_luaG_getobjnameHK = new Memory::MinHook<Game::Functions::luaG_getobjnameT>(
				reinterpret_cast<Game::Functions::luaG_getobjnameT*>(g_Pointers->m_luaG_getobjname));
			this->m_luaG_getobjnameHK->Hook<HK_luaG_getobjname>();
			LOG("Hooks", INFO, "luaG_getobjname hooked — the engine's hashed-name traceback path "
				"will no longer format an uninitialised pointer.");
		}
		else {
			LOG("Hooks", WARN, "luaG_getobjname did not resolve; the engine traceback will be "
				"SKIPPED on Lua errors, because calling it would crash the process.");
		}

		// Decide ZM progression before the redirect below can run its first tick: the redirect reads
		// ZmProgression::Enabled() to know whether an offline/LAN boot rewrites too.
		{
			const Common::Utility::NT::Library game;
			Game::ZmProgression::Init(reinterpret_cast<std::uintptr_t>(game.GetPtr()),
				game.GetOptionalHeader()->SizeOfImage);
			Game::UnlockAll::Init(reinterpret_cast<std::uintptr_t>(game.GetPtr()),
				game.GetOptionalHeader()->SizeOfImage);
			Game::LocalLpc::Init(reinterpret_cast<std::uintptr_t>(game.GetPtr()),
				game.GetOptionalHeader()->SizeOfImage);
			Game::MapKit::Init(reinterpret_cast<std::uintptr_t>(game.GetPtr()),
				game.GetOptionalHeader()->SizeOfImage);
			Game::MapKitTrace::Init(reinterpret_cast<std::uintptr_t>(game.GetPtr()),
				game.GetOptionalHeader()->SizeOfImage);
			Game::MapKitUsage::Init(reinterpret_cast<std::uintptr_t>(game.GetPtr()),
				game.GetOptionalHeader()->SizeOfImage);
		}

		// mapkit asset-usage census: one pass-through detour, only when cw-mod.json "mapkit_usage" is true.
		if (Game::MapKitUsage::Enabled()) {
			const Common::Utility::NT::Library game;
			void* const findIndex = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(game.GetPtr())
				+ (Game::kDump_BG_Cache_FindIndex_cand - Game::kDumpImagebase));
			this->m_BG_Cache_FindIndexHK = new Memory::MinHook<Game::Functions::BG_Cache_FindIndexT>(
				reinterpret_cast<Game::Functions::BG_Cache_FindIndexT*>(findIndex));
			this->m_BG_Cache_FindIndexHK->Hook<HK_BG_Cache_FindIndex>();
			if (*static_cast<std::uint8_t*>(findIndex) == 0xE9) {
				LOG("Hooks", INFO, "mapkit asset usage: BG_Cache_FindIndex detour live.");
			}
			else {
				LOG("Hooks", ERROR, "mapkit asset usage: BG_Cache_FindIndex detour NOT live: the census misses every "
					"bgcache lookup.");
			}
		}

		// mapkit zone trace: five pass-through detours, only when cw-mod.json "mapkit_trace" names a zone.
		if (Game::MapKitTrace::Enabled()) {
			const Common::Utility::NT::Library game;
			auto target = [&](std::uintptr_t dumpAbs) {
				return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(game.GetPtr())
					+ (dumpAbs - Game::kDumpImagebase));
			};
			void* const zone = target(Game::kDump_DB_LoadXFile_Internal);
			void* const asset = target(Game::kDump_Load_XAsset);
			void* const assetPreload = target(Game::kDump_Load_XAsset_Preload);
			void* const read = target(Game::kDump_DB_ReadXFile);
			void* const readString = target(Game::kDump_DB_ReadXFileString);
			this->m_DB_LoadXFile_InternalHK = new Memory::MinHook<Game::Functions::DB_LoadXFile_InternalT>(
				reinterpret_cast<Game::Functions::DB_LoadXFile_InternalT*>(zone));
			this->m_DB_LoadXFile_InternalHK->Hook<HK_DB_LoadXFile_Internal>();
			this->m_Load_XAssetHK = new Memory::MinHook<Game::Functions::Load_XAssetT>(
				reinterpret_cast<Game::Functions::Load_XAssetT*>(asset));
			this->m_Load_XAssetHK->Hook<HK_Load_XAsset>();
			this->m_Load_XAsset_PreloadHK = new Memory::MinHook<Game::Functions::Load_XAssetT>(
				reinterpret_cast<Game::Functions::Load_XAssetT*>(assetPreload));
			this->m_Load_XAsset_PreloadHK->Hook<HK_Load_XAsset_Preload>();
			this->m_DB_ReadXFileHK = new Memory::MinHook<Game::Functions::DB_ReadXFileT>(
				reinterpret_cast<Game::Functions::DB_ReadXFileT*>(read));
			this->m_DB_ReadXFileHK->Hook<HK_DB_ReadXFile>();
			this->m_DB_ReadXFileStringHK = new Memory::MinHook<Game::Functions::DB_ReadXFileStringT>(
				reinterpret_cast<Game::Functions::DB_ReadXFileStringT*>(readString));
			this->m_DB_ReadXFileStringHK->Hook<HK_DB_ReadXFileString>();

			const bool zoneLive = *static_cast<std::uint8_t*>(zone) == 0xE9;
			const bool assetLive = *static_cast<std::uint8_t*>(asset) == 0xE9
				&& *static_cast<std::uint8_t*>(assetPreload) == 0xE9;
			const bool readLive = *static_cast<std::uint8_t*>(read) == 0xE9;
			const bool stringLive = *static_cast<std::uint8_t*>(readString) == 0xE9;
			if (zoneLive && assetLive && readLive && stringLive) {
				LOG("Hooks", INFO, "mapkit zone trace: zone, asset (both loader families) and stream-read detours live.");
			}
			else {
				// A trace with a missing reader has wrong stream offsets: ffinfo would reject it anyway.
				LOG("Hooks", ERROR, "mapkit zone trace: detours NOT all live (zone {}, assets {}, read {}, string {}); "
					"any trace written this boot is wrong.", zoneLive, assetLive, readLive, stringLive);
			}
		}

		// Custom maps from cw-mod/maps: six detours, each a pass-through except for our own zones and the
		// Maps-tab redirect. The folders are mounted from the game-thread tick (OnShowOverStack). Only when
		// MapKit::Init said yes.
		if (Game::MapKit::Enabled()) {
			const Common::Utility::NT::Library game;
			auto target = [&](std::uintptr_t dumpAbs) {
				return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(game.GetPtr())
					+ (dumpAbs - Game::kDumpImagebase));
			};
			void* const verify = target(Game::kDump_DB_Signature_VerifyZone);
			void* const open = target(Game::kDump_FS_OpenFileRead);
			void* const start = target(Game::kDump_SV_StartMap);
			void* const preload = target(Game::kDump_MapPreload_StartZoneRead);
			void* const load = target(Game::kDump_DB_LoadXAssets);
			void* const expand = target(Game::kDump_DB_ExpandZoneVariants);
			void* const world = target(Game::kDump_R_InitWorld_cand);
			this->m_DB_Signature_VerifyZoneHK = new Memory::MinHook<Game::Functions::DB_Signature_VerifyZoneT>(
				reinterpret_cast<Game::Functions::DB_Signature_VerifyZoneT*>(verify));
			this->m_DB_Signature_VerifyZoneHK->Hook<HK_DB_Signature_VerifyZone>();
			this->m_FS_OpenFileReadHK = new Memory::MinHook<Game::Functions::FS_OpenFileReadT>(
				reinterpret_cast<Game::Functions::FS_OpenFileReadT*>(open));
			this->m_FS_OpenFileReadHK->Hook<HK_FS_OpenFileRead>();
			this->m_SV_StartMapHK = new Memory::MinHook<Game::Functions::SV_StartMapT>(
				reinterpret_cast<Game::Functions::SV_StartMapT*>(start));
			this->m_SV_StartMapHK->Hook<HK_SV_StartMap>();
			this->m_MapPreload_StartZoneReadHK = new Memory::MinHook<Game::Functions::MapPreload_StartZoneReadT>(
				reinterpret_cast<Game::Functions::MapPreload_StartZoneReadT*>(preload));
			this->m_MapPreload_StartZoneReadHK->Hook<HK_MapPreload_StartZoneRead>();
			this->m_DB_LoadXAssetsHK = new Memory::MinHook<Game::Functions::DB_LoadXAssetsT>(
				reinterpret_cast<Game::Functions::DB_LoadXAssetsT*>(load));
			this->m_DB_LoadXAssetsHK->Hook<HK_DB_LoadXAssets>();
			this->m_DB_ExpandZoneVariantsHK = new Memory::MinHook<Game::Functions::DB_ExpandZoneVariantsT>(
				reinterpret_cast<Game::Functions::DB_ExpandZoneVariantsT*>(expand));
			this->m_DB_ExpandZoneVariantsHK->Hook<HK_DB_ExpandZoneVariants>();
			this->m_R_InitWorldHK = new Memory::MinHook<Game::Functions::R_InitWorldT>(
				reinterpret_cast<Game::Functions::R_InitWorldT*>(world));
			this->m_R_InitWorldHK->Hook<HK_R_InitWorld>();
			void* const setMap = target(Game::kDump_Session_SetMapName);
			void* const mapByHash = target(Game::kDump_MapTable_FindEntryByHash);
			void* const mapFlags = target(Game::kDump_MapTable_GetMapFlags);
			this->m_Session_SetMapNameHK = new Memory::MinHook<Game::Functions::Session_SetMapNameT>(
				reinterpret_cast<Game::Functions::Session_SetMapNameT*>(setMap));
			this->m_Session_SetMapNameHK->Hook<HK_Session_SetMapName>();
			this->m_MapTable_FindEntryByHashHK = new Memory::MinHook<Game::Functions::MapTable_FindEntryByHashT>(
				reinterpret_cast<Game::Functions::MapTable_FindEntryByHashT*>(mapByHash));
			this->m_MapTable_FindEntryByHashHK->Hook<HK_MapTable_FindEntryByHash>();
			this->m_MapTable_GetMapFlagsHK = new Memory::MinHook<Game::Functions::MapTable_GetMapFlagsT>(
				reinterpret_cast<Game::Functions::MapTable_GetMapFlagsT*>(mapFlags));
			this->m_MapTable_GetMapFlagsHK->Hook<HK_MapTable_GetMapFlags>();
			const bool ownMapLive = *static_cast<std::uint8_t*>(setMap) == 0xE9 && *static_cast<std::uint8_t*>(mapByHash) == 0xE9
				&& *static_cast<std::uint8_t*>(mapFlags) == 0xE9;
			if (ownMapLive) {
				LOG("Hooks", INFO, "Custom maps: lobby map and maptable detours live (maps of their own).");
			}
			else {
				LOG("Hooks", ERROR, "Custom maps: lobby map and maptable detours NOT all live: a map of its own will not start.");
			}

			const bool verifyLive = *static_cast<std::uint8_t*>(verify) == 0xE9;
			const bool openLive = *static_cast<std::uint8_t*>(open) == 0xE9;
			const bool startLive = *static_cast<std::uint8_t*>(start) == 0xE9;
			const bool preloadLive = *static_cast<std::uint8_t*>(preload) == 0xE9;
			const bool loadLive = *static_cast<std::uint8_t*>(load) == 0xE9;
			const bool expandLive = *static_cast<std::uint8_t*>(expand) == 0xE9;
			const bool worldLive = *static_cast<std::uint8_t*>(world) == 0xE9;
			if (verifyLive && openLive && startLive && preloadLive && loadLive && expandLive) {
				LOG("Hooks", INFO, "Custom maps: signature skip, .fd hide, preload, zone-load, map-start redirect and "
					"override-zone order detours live (world-start log {}).", worldLive ? "live" : "NOT live");
			}
			else {
				// Without the signature skip a custom zone trips the tamper response and the NEXT zone drops.
				LOG("Hooks", ERROR, "Custom maps: detours NOT all live (signature skip {}, .fd hide {}, map start {}, "
					"preload {}, zone load {}, zone order {}). Do not start a custom map this boot.", verifyLive, openLive,
					startLive, preloadLive, loadLive, expandLive);
			}
		}

		// LAN/offline LPC playlists: the zone-load allocator watch. The loading itself runs from the
		// game-thread tick (OnShowOverStack). Only when LocalLpc::Init said yes.
		if (Game::LocalLpc::Enabled()) {
			const Common::Utility::NT::Library game;
			void* const target = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(game.GetPtr())
				+ (Game::kDump_DB_AllocXAssetEntry - Game::kDumpImagebase));
			this->m_DB_AllocXAssetEntryHK = new Memory::MinHook<Game::Functions::DB_AllocXAssetEntryT>(
				reinterpret_cast<Game::Functions::DB_AllocXAssetEntryT*>(target));
			this->m_DB_AllocXAssetEntryHK->Hook<HK_DB_AllocXAssetEntry>();
			if (*static_cast<std::uint8_t*>(target) == 0xE9) {
				LOG("Hooks", INFO, "LPC playlists: zone-load allocator watch armed.");
			}
			else {
				LOG("Hooks", ERROR, "LPC playlists: allocator watch FAILED to install (prologue 0x{:02X}); a full "
					"asset pool during the playlists load will ERR_DROP and the log will not say which.",
					*static_cast<std::uint8_t*>(target));
			}
		}

		// TEMPORARY until the backend serves bdStorage: the dwuser -> hdd playerdata redirect, in two
		// halves (see hook.hpp). First the read-completion half.
		if (g_Pointers->m_PlayerData_OnStorageOpComplete) {
			this->m_PlayerData_OnStorageOpCompleteHK =
				new Memory::MinHook<Game::Functions::PlayerData_OnStorageOpCompleteT>(
					reinterpret_cast<Game::Functions::PlayerData_OnStorageOpCompleteT*>(
						g_Pointers->m_PlayerData_OnStorageOpComplete));
			this->m_PlayerData_OnStorageOpCompleteHK->Hook<HK_PlayerData_OnStorageOpComplete>();
			LOG("Hooks", INFO, "PlayerData storage-completion transcript armed — every async read/write "
				"completion will report the map it finished and the LOADED flag it left behind.");
		}
		else {
			LOG("Hooks", WARN, "PlayerData_OnStorageOpComplete did not resolve; the load transcript is "
				"unavailable this boot (build mismatch).");
		}

		// ...then the def rewrite. Installed unconditionally so the log records that it ran even
		// offline, where it deliberately does nothing.
		if (g_Pointers->m_PlayerData_ControllerStorageTick) {
			this->m_PlayerData_ControllerStorageTickHK =
				new Memory::MinHook<Game::Functions::PlayerData_ControllerStorageTickT>(
					reinterpret_cast<Game::Functions::PlayerData_ControllerStorageTickT*>(
						g_Pointers->m_PlayerData_ControllerStorageTick));
			this->m_PlayerData_ControllerStorageTickHK->Hook<HK_PlayerData_ControllerStorageTick>();
			LOG("Hooks", INFO, "PlayerData load driver hooked — dwuser-backed data maps will be rewritten "
				"to local storage before the first location is submitted (online boots, and offline/LAN while ZM "
				"progression is on).");
		}
		else {
			LOG("Hooks", WARN, "PlayerData_ControllerStorageTick did not resolve; dwuser data maps will "
				"stay unloadable this boot (build mismatch).");
		}

		// ZM progression (every boot profile): the two stats gates, the connect-time prematch copy, the
		// match-end commit, the AE block and a read-only XP transcript. Only when ZmProgression::Init
		// said yes.
		if (Game::ZmProgression::Enabled()) {
			const Common::Utility::NT::Library game;
			const auto base = reinterpret_cast<std::uintptr_t>(game.GetPtr());
			auto at = [base](std::uintptr_t dumpAbs) {
				return reinterpret_cast<void*>(base + (dumpAbs - Game::kDumpImagebase));
			};
			int live = 0;
			auto install = [&]<typename Plate, typename Fn>(void* target, Memory::MinHook<Fn>*& member) {
				member = new Memory::MinHook<Fn>(reinterpret_cast<Fn*>(target));
				member->template Hook<Plate>();
				live += *static_cast<std::uint8_t*>(target) == 0xE9;
			};
			install.operator()<HK_LiveStorage_AreMatchStatsEnabled>(
				at(Game::kDump_LiveStorage_AreMatchStatsEnabled), this->m_LiveStorage_AreMatchStatsEnabledHK);
			install.operator()<HK_GScr_AreStatWritesEnabled>(
				at(Game::kDump_GScr_AreStatWritesEnabled), this->m_GScr_AreStatWritesEnabledHK);
			install.operator()<HK_LiveStorage_BeginStatsTransfer>(
				at(Game::kDump_LiveStorage_BeginStatsTransfer), this->m_LiveStorage_BeginStatsTransferHK);
			install.operator()<HK_LiveStorage_CommitStatsTransfer>(
				at(Game::kDump_LiveStorage_CommitStatsTransfer), this->m_LiveStorage_CommitStatsTransferHK);
			install.operator()<HK_G_AddPlayerRankXp>(
				at(Game::kDump_G_AddPlayerRankXp), this->m_G_AddPlayerRankXpHK);
			install.operator()<HK_LiveStats_GetLiveUserStatsInstance>(
				at(Game::kDump_LiveStats_GetLiveUserStatsInstance), this->m_LiveStats_GetLiveUserStatsInstanceHK);
			install.operator()<HK_LiveStats_GetAeRootStateSlot>(
				at(Game::kDump_LiveStats_GetAeRootStateSlot), this->m_LiveStats_GetAeRootStateSlotHK);
			install.operator()<HK_Lua_PushAeSyncBuffer>(
				at(Game::kDump_Lua_PushAeSyncBuffer), this->m_Lua_PushAeSyncBufferHK);
			install.operator()<HK_SV_ClientStatsReady>(
				at(Game::kDump_SV_ClientStatsReady), this->m_SV_ClientStatsReadyHK);
			install.operator()<HK_LobbyRoot_SetNetworkModeModel>(
				at(Game::kDump_LobbyRoot_SetNetworkModeModel), this->m_LobbyRoot_SetNetworkModeModelHK);
			int expected = 10;
			if (Game::ZmProgression::LiveMenusEnabled()) {
				install.operator()<HK_Lua_GameModeIsMode_Impl>(
					at(Game::kDump_Lua_GameModeIsMode_Impl), this->m_Lua_GameModeIsMode_ImplHK);
				++expected;
			}
			// The asker log is recon for a LAN lobby's menus; a LIVE lobby has nothing to find.
			if (Game::ZmProgression::LogsNetworkModeQueries()) {
				install.operator()<HK_Lua_Engine_GetLobbyNetworkMode>(
					at(Game::kDump_Lua_Engine_GetLobbyNetworkMode), this->m_Lua_Engine_GetLobbyNetworkModeHK);
				++expected;
			}
			Game::ZmProgression::BindHookThunks();
			if (live == expected) {
				LOG("Hooks", INFO, "ZM progression detours armed ({}/{} live).", live, expected);
			}
			else {
				LOG("Hooks", ERROR, "ZM progression: only {}/{} detours live (prologue not 0xE9). Anything this "
					"boot says about XP or saving is unreliable.", live, expected);
			}
		}

		// "unlock_all": the lock and ownership predicates. Only when UnlockAll::Init said yes, so a boot
		// with the key off does not touch any of these functions.
		if (Game::UnlockAll::Enabled()) {
			const Common::Utility::NT::Library game;
			const auto base = reinterpret_cast<std::uintptr_t>(game.GetPtr());
			auto at = [base](std::uintptr_t dumpAbs) {
				return reinterpret_cast<void*>(base + (dumpAbs - Game::kDumpImagebase));
			};
			int live = 0;
			auto install = [&]<typename Plate, typename Fn>(void* target, Memory::MinHook<Fn>*& member) {
				member = new Memory::MinHook<Fn>(reinterpret_cast<Fn*>(target));
				member->template Hook<Plate>();
				live += *static_cast<std::uint8_t*>(target) == 0xE9;
			};
			install.operator()<HK_Progression_IsItemLocked>(
				at(Game::kDump_Progression_IsItemLocked), this->m_Progression_IsItemLockedHK);
			install.operator()<HK_Progression_IsAttachmentLockedInBlock>(
				at(Game::kDump_Progression_IsAttachmentLockedInBlock), this->m_Progression_IsAttachmentLockedInBlockHK);
			install.operator()<HK_Progression_IsAttachmentSlotLocked>(
				at(Game::kDump_Progression_IsAttachmentSlotLocked), this->m_Progression_IsAttachmentSlotLockedHK);
			install.operator()<HK_Progression_IsItemOptionLockedCore>(
				at(Game::kDump_Progression_IsItemOptionLockedCore), this->m_Progression_IsItemOptionLockedCoreHK);
			install.operator()<HK_Com_SessionMode_IsProgressionExemptContext>(
				at(Game::kDump_Com_SessionMode_IsProgressionExemptContext),
				this->m_Com_SessionMode_IsProgressionExemptContextHK);
			install.operator()<HK_Inventory_GetItemQuantity>(
				at(Game::kDump_Inventory_GetItemQuantity), this->m_Inventory_GetItemQuantityHK);
			install.operator()<HK_Loot_GetItemQuantity>(
				at(Game::kDump_Loot_GetItemQuantity), this->m_Loot_GetItemQuantityHK);
			install.operator()<HK_Entitlement_IsOwned>(
				at(Game::kDump_Entitlement_IsOwned), this->m_Entitlement_IsOwnedHK);
			install.operator()<HK_DwFetch_IsInventoryReady>(
				at(Game::kDump_DwFetch_IsInventoryReady), this->m_DwFetch_IsInventoryReadyHK);
			install.operator()<HK_Loot_GetBattlePassOwned>(
				at(Game::kDump_Loot_GetBattlePassOwned), this->m_Loot_GetBattlePassOwnedHK);
			install.operator()<HK_Loot_GetBattlePassRank>(
				at(Game::kDump_Loot_GetBattlePassRank), this->m_Loot_GetBattlePassRankHK);
			constexpr int expected = 11;
			Game::UnlockAll::BindHookThunks();
			if (live == expected) {
				LOG("Hooks", INFO, "unlock_all detours armed ({}/{} live).", live, expected);
			}
			else {
				LOG("Hooks", ERROR, "unlock_all: only {}/{} detours live (prologue not 0xE9). Some locks will stay.",
					live, expected);
			}
		}

		// Local Demonware backend: only when enabled (our public keys were patched in), force the
		// login flow to studio auth so the client actually reaches our Demonware auth server.
		if (Game::DwBackend::g_Enabled && g_Pointers->m_Dw_GetLoginFlow) {
			this->m_Dw_GetLoginFlowHK = new Memory::MinHook<Game::Functions::Dw_GetLoginFlowT>(
				reinterpret_cast<Game::Functions::Dw_GetLoginFlowT*>(g_Pointers->m_Dw_GetLoginFlow));
			this->m_Dw_GetLoginFlowHK->Hook<HK_Dw_GetLoginFlow>();
			LOG("Hooks", INFO, "DwBackend: forcing studio-auth login flow (9).");
		}

		// Login-status transcript mirror (read-only diagnostics). Shows how far Demonware login gets.
		if (Game::DwBackend::g_Enabled && g_Pointers->m_Login_SetStatus) {
			this->m_Login_SetStatusHK = new Memory::MinHook<Game::Functions::Login_SetStatusT>(
				reinterpret_cast<Game::Functions::Login_SetStatusT*>(g_Pointers->m_Login_SetStatus));
			this->m_Login_SetStatusHK->Hook<HK_Login_SetStatus>();
			LOG("Hooks", INFO, "DwBackend: mirroring login status transcript to console.");
		}

		// Studio-auth JWT builder mirror (read-only diagnostics). Tells us WHY the studio token fails.
		if (Game::DwBackend::g_Enabled && g_Pointers->m_DwLogin_BuildStudioToken) {
			this->m_DwLogin_BuildStudioTokenHK = new Memory::MinHook<Game::Functions::DwLogin_BuildStudioTokenT>(
				reinterpret_cast<Game::Functions::DwLogin_BuildStudioTokenT*>(g_Pointers->m_DwLogin_BuildStudioToken));
			this->m_DwLogin_BuildStudioTokenHK->Hook<HK_DwLogin_BuildStudioToken>();
			LOG("Hooks", INFO, "DwBackend: mirroring studio-token build (diagnostics).");
		}

		// The first-party presence gate: true to the login driver (so a no-Battle.net login can leave
		// state 1), and to the online-ready gates once DW login is complete. See the detour's header.
		if (Game::DwBackend::g_Enabled && g_Pointers->m_LiveUser_FirstPartyPresenceOk
			&& g_Pointers->m_LiveUser_LoginDriver_TickBase) {
			this->m_LiveUser_FirstPartyPresenceOkHK = new Memory::MinHook<Game::Functions::LiveUser_FirstPartyPresenceOkT>(
				reinterpret_cast<Game::Functions::LiveUser_FirstPartyPresenceOkT*>(g_Pointers->m_LiveUser_FirstPartyPresenceOk));
			this->m_LiveUser_FirstPartyPresenceOkHK->Hook<HK_LiveUser_FirstPartyPresenceOk>();
			LOG("Hooks", INFO, "DwBackend: first-party presence gate armed (login driver always; other callers "
				"once DW login is complete, each site logged once; identity setup excluded).");
		}

		// Null first-party SESSION probe. Only meaningful once login can actually complete, which is
		// why it rides the DwBackend gate: on any other boot the detour would never be installed and
		// the game is byte-identical. When the session IS present the detour is a plain passthrough,
		// so the only behaviour it can change is the case that otherwise crashes.
		if (Game::DwBackend::g_Enabled && g_Pointers->m_FirstParty_GetSession) {
			this->m_FirstParty_GetSessionHK = new Memory::MinHook<Game::Functions::FirstParty_GetSessionT>(
				reinterpret_cast<Game::Functions::FirstParty_GetSessionT*>(g_Pointers->m_FirstParty_GetSession));
			this->m_FirstParty_GetSessionHK->Hook<HK_FirstParty_GetSession>();
			// VERIFY THE PATCH ACTUALLY LANDED. Memory::MinHook::Hook() discards MH_CreateHook's and
			// MH_EnableHook's return codes, so a failure is silent — and the failure mode here is
			// nasty: the boot would log "armed", produce no census lines at all, and we would read
			// that as "no call site asks for a session" when the truth is "the detour never ran".
			// The target is only 5 bytes (mov rax,[rcx+18h] / ret), which is exactly the minimum
			// MinHook needs for its jmp rel32, so this is a real possibility and not paranoia.
			// A live detour leaves 0xE9 as the first byte.
			const std::uint8_t prologue =
				*reinterpret_cast<std::uint8_t*>(g_Pointers->m_FirstParty_GetSession);
			if (prologue == 0xE9) {
				LOG("Hooks", INFO,
					"DwBackend: first-party session watchman armed (names any new null-session call site).");
			}
			else {
				LOG("Hooks", ERROR,
					"DwBackend: first-party session watchman FAILED to install — prologue is 0x{:02X}, "
					"expected 0xE9. An empty census this boot means nothing.", prologue);
			}
		}

		// The fix the census pointed at: serve the ONE thing the null session was ever asked for —
		// the local user index — and return 0 for it, which is what a real session would hold on a
		// single-local-user PC. Rides the same DwBackend gate, and calls through untouched whenever a
		// real session exists, so the only behaviour it can change is the one that crashes.
		if (Game::DwBackend::g_Enabled && g_Pointers->m_FirstParty_GetLocalUserIndex
			&& g_Pointers->m_g_firstPartyManagerObj) {
			this->m_FirstParty_GetLocalUserIndexHK =
				new Memory::MinHook<Game::Functions::FirstParty_GetLocalUserIndexT>(
					reinterpret_cast<Game::Functions::FirstParty_GetLocalUserIndexT*>(
						g_Pointers->m_FirstParty_GetLocalUserIndex));
			this->m_FirstParty_GetLocalUserIndexHK->Hook<HK_FirstParty_GetLocalUserIndex>();
			// Same silent-failure check as above, and it matters more here: this one is load-bearing.
			// If it fails to install the game simply crashes again, and we should be able to tell that
			// apart from "the fix was wrong" without another round of guessing.
			const std::uint8_t indexPrologue =
				*reinterpret_cast<std::uint8_t*>(g_Pointers->m_FirstParty_GetLocalUserIndex);
			if (indexPrologue == 0xE9) {
				LOG("Hooks", INFO,
					"DwBackend: null-session local-user-index fix armed (returns controller 0).");
			}
			else {
				LOG("Hooks", ERROR,
					"DwBackend: null-session local-user-index fix FAILED to install — prologue is "
					"0x{:02X}, expected 0xE9. Expect the post-login crash to come back; that would be "
					"the hook missing, NOT the fix being wrong.", indexPrologue);
			}
		}

		// The ERR_DROP gate. Measured 2026-08-02 on the first boot where online mode and the local
		// backend ran together: "An error occurred: Boy 501 Gothic Missile" one second after
		// [status 27] Login Complete, then a menu restart and a fresh login, on a loop. The err_drop's
		// own caller list named the path — LiveUser_UpdateSigninState -> LiveUser_OnDwConnected ->
		// this function, which promotes signinState to 2 (online) and only asks for a drop when the
		// prior state was 1 AND Com_SessionMode_IsOnline(). It is a sign-IN, not a sign-out (B1). The
		// detour calls through (the promotion is unchanged) and returns 0.
		if (Game::DwBackend::g_Enabled && g_Pointers->m_LiveUser_SignOutBuildDropMessage) {
			this->m_LiveUser_SignOutDropGateHK =
				new Memory::MinHook<Game::Functions::LiveUser_SignOutBuildDropMessageT>(
					reinterpret_cast<Game::Functions::LiveUser_SignOutBuildDropMessageT*>(
						g_Pointers->m_LiveUser_SignOutBuildDropMessage));
			this->m_LiveUser_SignOutDropGateHK->Hook<HK_LiveUser_SignOutDropGate>();
			// Same silent-failure check as the fix above, and for the same reason: if this one does
			// not install, the boot loops exactly as it did before and we must be able to tell that
			// apart from "the suppression was the wrong call".
			const std::uint8_t dropPrologue =
				*reinterpret_cast<std::uint8_t*>(g_Pointers->m_LiveUser_SignOutBuildDropMessage);
			if (dropPrologue == 0xE9) {
				LOG("Hooks", INFO,
					"DwBackend: connect-time ERR_DROP gate armed — the promotion to signed-in-online runs, "
					"the extra drop-to-offline is skipped, and every call is logged.");
			}
			else {
				LOG("Hooks", ERROR,
					"DwBackend: sign-out ERR_DROP gate FAILED to install — prologue is 0x{:02X}, "
					"expected 0xE9. Expect 'Boy 501 Gothic Missile' and the menu-restart loop to "
					"come back; that would be the hook missing, NOT the fix being wrong.", dropPrologue);
			}
		}

		// B3 LPC list transcript (read-only). Every detour calls through; see the impl file.
		if (Game::DwBackend::g_Enabled) {
			int armed = 0;
			if (g_Pointers->m_BdRemoteHttpTask_FinishRow) {
				this->m_BdRemoteHttpTask_FinishRowHK = new Memory::MinHook<Game::Functions::BdRemoteHttpTask_FinishRowT>(
					reinterpret_cast<Game::Functions::BdRemoteHttpTask_FinishRowT*>(g_Pointers->m_BdRemoteHttpTask_FinishRow));
				this->m_BdRemoteHttpTask_FinishRowHK->Hook<HK_BdRemoteHttpTask_FinishRow>();
				armed += *reinterpret_cast<std::uint8_t*>(g_Pointers->m_BdRemoteHttpTask_FinishRow) == 0xE9;
			}
			if (g_Pointers->m_PublisherObjectsResource_Parse) {
				this->m_PublisherObjectsResource_ParseHK = new Memory::MinHook<Game::Functions::PublisherObjectsResource_ParseT>(
					reinterpret_cast<Game::Functions::PublisherObjectsResource_ParseT*>(g_Pointers->m_PublisherObjectsResource_Parse));
				this->m_PublisherObjectsResource_ParseHK->Hook<HK_PublisherObjectsResource_Parse>();
				armed += *reinterpret_cast<std::uint8_t*>(g_Pointers->m_PublisherObjectsResource_Parse) == 0xE9;
			}
			if (g_Pointers->m_ObjectMetadata_ParseJson) {
				this->m_ObjectMetadata_ParseJsonHK = new Memory::MinHook<Game::Functions::ObjectMetadata_ParseJsonT>(
					reinterpret_cast<Game::Functions::ObjectMetadata_ParseJsonT*>(g_Pointers->m_ObjectMetadata_ParseJson));
				this->m_ObjectMetadata_ParseJsonHK->Hook<HK_ObjectMetadata_ParseJson>();
				armed += *reinterpret_cast<std::uint8_t*>(g_Pointers->m_ObjectMetadata_ParseJson) == 0xE9;
			}
			if (g_Pointers->m_Lpc_WriteManifest) {
				this->m_Lpc_WriteManifestHK = new Memory::MinHook<Game::Functions::Lpc_ListCallbackT>(
					reinterpret_cast<Game::Functions::Lpc_ListCallbackT*>(g_Pointers->m_Lpc_WriteManifest));
				this->m_Lpc_WriteManifestHK->Hook<HK_Lpc_WriteManifest>();
				armed += *reinterpret_cast<std::uint8_t*>(g_Pointers->m_Lpc_WriteManifest) == 0xE9;
			}
			if (g_Pointers->m_Lpc_OnListFailed) {
				this->m_Lpc_OnListFailedHK = new Memory::MinHook<Game::Functions::Lpc_ListCallbackT>(
					reinterpret_cast<Game::Functions::Lpc_ListCallbackT*>(g_Pointers->m_Lpc_OnListFailed));
				this->m_Lpc_OnListFailedHK->Hook<HK_Lpc_OnListFailed>();
				armed += *reinterpret_cast<std::uint8_t*>(g_Pointers->m_Lpc_OnListFailed) == 0xE9;
			}
			if (g_Pointers->m_BdLobbyMsg_WriteHeader) {
				this->m_BdLobbyMsg_WriteHeaderHK = new Memory::MinHook<Game::Functions::BdLobbyMsg_WriteHeaderT>(
					reinterpret_cast<Game::Functions::BdLobbyMsg_WriteHeaderT*>(g_Pointers->m_BdLobbyMsg_WriteHeader));
				this->m_BdLobbyMsg_WriteHeaderHK->Hook<HK_BdLobbyMsg_WriteHeader>();
				if (*reinterpret_cast<std::uint8_t*>(g_Pointers->m_BdLobbyMsg_WriteHeader) == 0xE9) {
					LOG("Hooks", INFO, "DwBackend: lobby request census armed (header writer).");
				}
				else {
					LOG("Hooks", ERROR, "DwBackend: lobby request census FAILED to install; no census lines means nothing.");
				}
			}
			int mtxArmed = 0;
			auto hookMtx = [&](void* target, auto*& member, auto install) {
				if (!target) return;
				install(target, member);
				mtxArmed += *static_cast<std::uint8_t*>(target) == 0xE9;
			};
			hookMtx(g_Pointers->m_MtxSync_ShouldStart, this->m_MtxSync_ShouldStartHK, [](void* t, auto*& m) {
				m = new Memory::MinHook<Game::Functions::MtxSync_ControllerGateT>(
					static_cast<Game::Functions::MtxSync_ControllerGateT*>(t));
				m->template Hook<HK_MtxSync_ShouldStart>();
			});
			hookMtx(g_Pointers->m_MtxSync_RequestBnetTokenZEUS, this->m_MtxSync_RequestBnetTokenZEUSHK, [](void* t, auto*& m) {
				m = new Memory::MinHook<Game::Functions::MtxSync_ControllerGateT>(
					static_cast<Game::Functions::MtxSync_ControllerGateT*>(t));
				m->template Hook<HK_MtxSync_RequestBnetTokenZEUS>();
			});
			hookMtx(g_Pointers->m_MtxSync_OnBnetToken, this->m_MtxSync_OnBnetTokenHK, [](void* t, auto*& m) {
				m = new Memory::MinHook<Game::Functions::MtxSync_OnBnetTokenT>(
					static_cast<Game::Functions::MtxSync_OnBnetTokenT*>(t));
				m->template Hook<HK_MtxSync_OnBnetToken>();
			});
			hookMtx(g_Pointers->m_MtxSync_IsDoneOrInFlight, this->m_MtxSync_IsDoneOrInFlightHK, [](void* t, auto*& m) {
				m = new Memory::MinHook<Game::Functions::MtxSync_ControllerGateT>(
					static_cast<Game::Functions::MtxSync_ControllerGateT*>(t));
				m->template Hook<HK_MtxSync_IsDoneOrInFlight>();
			});
			if (g_Pointers->m_DwFetch_GetStatus) {
				this->m_DwFetch_GetStatusHK = new Memory::MinHook<Game::Functions::DwFetch_GetStatusT>(
					reinterpret_cast<Game::Functions::DwFetch_GetStatusT*>(g_Pointers->m_DwFetch_GetStatus));
				this->m_DwFetch_GetStatusHK->Hook<HK_DwFetch_GetStatus>();
				if (*reinterpret_cast<std::uint8_t*>(g_Pointers->m_DwFetch_GetStatus) == 0xE9) {
					LOG("Hooks", INFO, "DwBackend: DW fetch-status transcript armed (B6 lobby gate).");
				}
				else {
					LOG("Hooks", ERROR, "DwBackend: DW fetch-status transcript FAILED to install; no DwFetch lines means nothing.");
				}
			}
			// B6 lobby waiver: Lua's IsDemonwareFetchingDone may answer true when only the five public-
			// matchmaking/commerce bits are missing. Needs the transcript above (it reads its mask).
			if (g_Pointers->m_DwFetch_IsDone && g_Pointers->m_Lua_IsDemonwareFetchingDone_ImplBase
				&& this->m_DwFetch_GetStatusHK) {
				if (!Game::Settings::Get().lobbyWaiver) {
					LOG("Hooks", INFO, "DwBackend: \"lobby_waiver\" is off in cw-mod.json: IsDemonwareFetchingDone stays honest.");
				}
				else {
					this->m_DwFetch_IsDoneHK = new Memory::MinHook<Game::Functions::DwFetch_IsDoneT>(
						reinterpret_cast<Game::Functions::DwFetch_IsDoneT*>(g_Pointers->m_DwFetch_IsDone));
					this->m_DwFetch_IsDoneHK->Hook<HK_DwFetch_IsDone>();
					if (*reinterpret_cast<std::uint8_t*>(g_Pointers->m_DwFetch_IsDone) == 0xE9) {
						LOG("Hooks", INFO, "DwBackend: lobby waiver armed (Lua only; waives G/N/R/W/Y, nothing else).");
					}
					else {
						LOG("Hooks", ERROR, "DwBackend: lobby waiver FAILED to install; the tiles will stay padlocked.");
					}
				}
			}
			// "You don't own the game": not a trial on a backend boot, the answer nodw gives offline.
			if (g_Pointers->m_LiveUser_IsTrial) {
				this->m_LiveUser_IsTrialHK = new Memory::MinHook<Game::Functions::LiveUser_IsTrialT>(
					reinterpret_cast<Game::Functions::LiveUser_IsTrialT*>(g_Pointers->m_LiveUser_IsTrial));
				this->m_LiveUser_IsTrialHK->Hook<HK_LiveUser_IsTrial>();
				if (*reinterpret_cast<std::uint8_t*>(g_Pointers->m_LiveUser_IsTrial) == 0xE9) {
					LOG("Hooks", INFO, "DwBackend: full-game answer armed (IsTrial -> false, real answer logged per site).");
				}
				else {
					LOG("Hooks", ERROR, "DwBackend: full-game answer FAILED to install; expect the trial upsell.");
				}
			}
			// Peers reach each other the LAN way: own bdCommonAddr without a public address, NAT open.
			if (g_Pointers->m_bdCommonAddr_Ctor && g_Pointers->m_g_bdAddrEmpty) {
				this->m_bdCommonAddr_CtorHK = new Memory::MinHook<Game::Functions::bdCommonAddr_CtorT>(
					reinterpret_cast<Game::Functions::bdCommonAddr_CtorT*>(g_Pointers->m_bdCommonAddr_Ctor));
				this->m_bdCommonAddr_CtorHK->Hook<HK_bdCommonAddr_Ctor>();
				if (*reinterpret_cast<std::uint8_t*>(g_Pointers->m_bdCommonAddr_Ctor) == 0xE9) {
					LOG("Hooks", INFO, "DwBackend: peer addressing armed (own address built LAN-style: no public "
						"address, NAT open).");
				}
				else {
					LOG("Hooks", ERROR, "DwBackend: peer addressing FAILED to install; joins between PCs will not connect.");
				}
			}
			if (mtxArmed == 4) {
				LOG("Hooks", INFO, "DwBackend: MtxSync transcript armed (4/4 detours live).");
			}
			else {
				LOG("Hooks", ERROR, "DwBackend: MtxSync transcript only {}/4 detours live; silence proves nothing.", mtxArmed);
			}
			if (armed == 5) {
				LOG("Hooks", INFO, "DwBackend: LPC list transcript armed (5/5 detours live).");
			}
			else {
				LOG("Hooks", ERROR, "DwBackend: LPC list transcript only {}/5 detours live (prologue not 0xE9); "
					"a silent transcript this boot proves nothing.", armed);
			}
		}

		// Battle.net / first-party error suppression. Installed ALWAYS and inert: every one of these
		// four detours checks OnlineMode::g_SuppressBnetErrors and otherwise calls the original, so an
		// ordinary launch behaves exactly as before. Installation used to be gated on the boot marker,
		// which was wrong — hooks are decided in this ctor, long before the overlay exists, so flipping
		// to online from the Session tab got no suppression at all and hit the same BLZBNTBGS wall.
		{
			if (g_Pointers->m_BnetError_ReportFatalUnguarded) {
				this->m_BnetError_ReportFatalUnguardedHK =
					new Memory::MinHook<Game::Functions::BnetError_ReportFatalUnguardedT>(
						reinterpret_cast<Game::Functions::BnetError_ReportFatalUnguardedT*>(
							g_Pointers->m_BnetError_ReportFatalUnguarded));
				this->m_BnetError_ReportFatalUnguardedHK->Hook<HK_BnetError_ReportFatalUnguarded>();
			}
			if (g_Pointers->m_BnetError_ReportFatalIfSignedIn) {
				this->m_BnetError_ReportFatalIfSignedInHK =
					new Memory::MinHook<Game::Functions::BnetError_ReportFatalIfSignedInT>(
						reinterpret_cast<Game::Functions::BnetError_ReportFatalIfSignedInT*>(
							g_Pointers->m_BnetError_ReportFatalIfSignedIn));
				this->m_BnetError_ReportFatalIfSignedInHK->Hook<HK_BnetError_ReportFatalIfSignedIn>();
			}
			// The reporters above only cover the Com_Error consumer. The popup is LUI asking for the
			// stored error, so the state itself has to never be set — these two are the only writers.
			if (g_Pointers->m_FirstParty_SetError) {
				this->m_FirstParty_SetErrorHK = new Memory::MinHook<Game::Functions::FirstParty_SetErrorT>(
					reinterpret_cast<Game::Functions::FirstParty_SetErrorT*>(g_Pointers->m_FirstParty_SetError));
				this->m_FirstParty_SetErrorHK->Hook<HK_FirstParty_SetError>();
			}
			if (g_Pointers->m_FirstParty_SetErrorState) {
				this->m_FirstParty_SetErrorStateHK = new Memory::MinHook<Game::Functions::FirstParty_SetErrorStateT>(
					reinterpret_cast<Game::Functions::FirstParty_SetErrorStateT*>(
						g_Pointers->m_FirstParty_SetErrorState));
				this->m_FirstParty_SetErrorStateHK->Hook<HK_FirstParty_SetErrorState>();
			}
			if (g_Pointers->m_FirstParty_OnBgsDisconnected) {
				this->m_FirstParty_OnBgsDisconnectedHK = new Memory::MinHook<Game::Functions::FirstParty_OnBgsDisconnectedT>(
					reinterpret_cast<Game::Functions::FirstParty_OnBgsDisconnectedT*>(
						g_Pointers->m_FirstParty_OnBgsDisconnected));
				this->m_FirstParty_OnBgsDisconnectedHK->Hook<HK_FirstParty_OnBgsDisconnected>();
			}

			// The four above were all measured to be insufficient: the popup has a third producer,
			// LiveUser_HandleSignOut, which is a sign-out path and touches neither the reporters nor
			// the state writers. They all end at this queue, so this is the detour that matters.
			if (g_Pointers->m_ErrorQueue_Push) {
				this->m_ErrorQueue_PushHK = new Memory::MinHook<Game::Functions::ErrorQueue_PushT>(
					reinterpret_cast<Game::Functions::ErrorQueue_PushT*>(g_Pointers->m_ErrorQueue_Push));
				this->m_ErrorQueue_PushHK->Hook<HK_ErrorQueue_Push>();
			}

			// And the one that is actually responsible. The queue hook above never fired in a
			// measured online boot while the dialog appeared regardless, because this watchdog calls
			// Com_Error(1024) itself. Everything above it is belt-and-braces; this is the belt.
			if (g_Pointers->m_LiveUser_ForceSignOutAndFatal) {
				this->m_LiveUser_ForceSignOutAndFatalHK =
					new Memory::MinHook<Game::Functions::LiveUser_ForceSignOutAndFatalT>(
						reinterpret_cast<Game::Functions::LiveUser_ForceSignOutAndFatalT*>(
							g_Pointers->m_LiveUser_ForceSignOutAndFatal));
				this->m_LiveUser_ForceSignOutAndFatalHK->Hook<HK_LiveUser_ForceSignOutAndFatal>();
			}

			LOG("Hooks", INFO, "Battle.net error detours armed (inert until online mode) — "
				"reporters(unguarded={} latched={}) state-writers(SetError={} SetErrorState={} OnBgsDisconnected={}) "
				"errorQueue={} signOutWatchdog={}. Suppression currently {}.",
				this->m_BnetError_ReportFatalUnguardedHK != nullptr,
				this->m_BnetError_ReportFatalIfSignedInHK != nullptr,
				this->m_FirstParty_SetErrorHK != nullptr,
				this->m_FirstParty_SetErrorStateHK != nullptr,
				this->m_FirstParty_OnBgsDisconnectedHK != nullptr,
				this->m_ErrorQueue_PushHK != nullptr,
				this->m_LiveUser_ForceSignOutAndFatalHK != nullptr,
				Game::OnlineMode::g_SuppressBnetErrors.load(std::memory_order_relaxed) ? "ON" : "off");
		}

		// Overlay (Checkpoint 1): install DXGI Present + D3D12 queue hooks now that Arxan is handled.
		Overlay::Initialize();

		// GSC loader: loads <game>/cw-mod/scripts, hooks DB_FindXAssetHeader (the LazyLink
		// VM slot is patched when a script is first served). With no scripts on disk the detour is a pass-through. Off with "scripts": false.
		Scripting::AutoInstall();
	}
}
