#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/ui_scripts.hpp"
#include "game/unlock_all.hpp"
#include "game/zm_progression.hpp"
#include "scripting/scripting.hpp"

#include <cstring>
#include <string>
#include <unordered_map>

// The engine's fatal LUI error path. LuiEvent_Dispatch (0x7FF727B8CA80) runs menu builders under
// lua_pcall, so a builder that raises is CAUGHT - and the engine then comes here, files telemetry
// through LuiError_ReportAndDie (0x7FF7222C04C0), busy-waits about a second, and terminates.
//
// Two jobs:
//   1. Always read the Lua error text off the stack and log it. The engine's own `context` is
//      usually empty, so without this a fatal names nothing.
//   2. On an ONLINE boot only, suppress the termination. That is a crutch, not a repair: the online
//      frontend still raises on data only a real backend provides, and a frontend that builds
//      partially is observable while a dead process is not. Remove it once an online+backend boot
//      builds clean. Offline play is unaffected: a genuine fatal still reports and dies.
//   3. While Pointers::OpenLuiMenu is dispatching (LuiMenus::g_InDispatch), suppress on any boot and
//      hand the message back to the overlay. Scoped to that one call.
//   4. Once the overlay has opened any menu (LuiMenus::g_HandOpened), suppress for the rest of the
//      session: a menu that built fine can raise later from its own handlers. Repeats are deduped
//      because such an error typically fires again on every input change.
//   5. The same once the LAN AAR path is armed (ZmProgression::SuppressLuiFatals): the AAR Lua was
//      written for a LIVE AE reply that we only imitate.
//   6. While our own SERVER BROWSER menu is open (UiScripts::MenuOpen): its Lua guards its own code,
//      but it stands on stock widgets, and an error in those while they hold our data lands here.
//
// When suppressing we replay the harmless half of the original: pop the message the way its
// lua_settop(L, -2) would. Reading the TString directly rather than calling lua_tolstring keeps us
// off the Arxan-guarded accessors on a path that is already handling an error.
template <>
std::uint64_t Client::Hook::Hooks::HK_LuiError_ReportFatal::hkCallback(const char* context, void* luaState) {
	const bool onlineBoot = Client::Game::OnlineMode::g_SuppressBnetErrors.load(std::memory_order_relaxed);
	// Also suppressed for the one menu the overlay's LUI menus tab is opening right now: a hand-opened
	// menu that wants data this UI state never loaded must cost a status line, not the process.
	const bool ourDispatch = Client::Game::LuiMenus::g_InDispatch.load(std::memory_order_relaxed);
	const bool lanAar = Client::Game::ZmProgression::SuppressLuiFatals();
	const bool ourMenu = Client::Game::UiScripts::MenuOpen();
	const bool handOpened = Client::Game::LuiMenus::g_HandOpened.load(std::memory_order_relaxed) || lanAar || ourMenu;
	const bool suppress = luaState && (onlineBoot || ourDispatch || handOpened);

	std::string message;
	auto* const topSlot = luaState
		? reinterpret_cast<std::uint64_t**>(
			reinterpret_cast<std::uint8_t*>(luaState) + Client::Game::Pointers::kLuaState_Top)
		: nullptr;

	std::uint64_t* top = nullptr;
	if (topSlot && Client::Game::SafeRead(topSlot, top) && top) {
		std::uint64_t value = 0;
		if (Client::Game::SafeRead(top - 1, value)) {
			const int tag = static_cast<int>(static_cast<std::int64_t>(value) >> Client::Game::Pointers::kLuaTagShift);
			if (tag == Client::Game::Pointers::kLuaTag_String) {
				const auto ts = reinterpret_cast<const std::uint8_t*>(
					value & Client::Game::Pointers::kLuaPayloadMask);
				std::uint32_t len = 0;
				if (Client::Game::SafeRead(ts + Client::Game::Pointers::kLuaTString_Len, len)) {
					if (len > 512) len = 512;
					for (std::uint32_t i = 0; i < len; ++i) {
						char c = 0;
						if (!Client::Game::SafeRead(ts + Client::Game::Pointers::kLuaTString_Data + i, c)) break;
						if (c == '\0') break;
						message.push_back(c);
					}
				}
			}
		}
		// Only pop when we are standing in for the original. On the pass-through path the original
		// does its own lua_settop, and popping here first would hand it a stack one slot short of
		// what it expects — reading the message must not have side effects we do not own.
		if (suppress) {
			*topSlot = top - 1;   // the original's lua_settop(L, -2)
		}
	}

	const char* const where = context && *context ? context : "<no context>";
	const char* const what = message.empty() ? "<no message on the stack>" : message.c_str();

	if (!suppress) {
		LOG("LUI", WARN, "Fatal LUI error [{}]: {} — NOT suppressed, the engine will report and "
			"terminate.", where, what);
		return m_Original(context, luaState);
	}

	if (ourDispatch) {
		Client::Game::LuiMenus::g_LastError = what;
		LOG("LUI", WARN, "Suppressed fatal LUI error [{}] (menu opened from the overlay): {}", where, what);
		return 0;
	}
	if (handOpened && !onlineBoot) {
		// Game thread only, like the rest of LUI.
		static std::unordered_map<std::string, int> s_Seen;
		const int n = ++s_Seen[message];
		Client::Game::LuiMenus::g_LateSuppressed.fetch_add(1, std::memory_order_relaxed);
		if (n == 1 || n == 10 || n == 100 || n % 1000 == 0) {
			LOG("LUI", WARN, "Suppressed fatal LUI error [{}] (x{}; {}): {}", where, n, ourMenu
				? "the cw-mod server browser menu is open"
				: lanAar ? "the LAN AAR path is armed, most likely the AAR Lua missing a LIVE-only reply"
				: "a menu was opened from the overlay this session, most likely it raising from a later update",
				what);
		}
		return 0;
	}
	LOG("LUI", WARN, "Suppressed fatal LUI error [{}] (online boot — letting the frontend keep "
		"building): {}", where, what);
	return 0;
}

namespace {
	// LIVE sets loot_season_stream from the Demonware publisher variables. Nothing sets it here: the local
	// backend answers that request with an empty list, and a LAN or offline boot never sends it. While it
	// is "", CoDMTXShared hands the menus season hash 0 and no season number, and the Lua that takes one
	// raises (both measured on online boots, 2026-09-29 to 2026-10-03):
	//   - The AAR's battle-pass list (datasource 0xBFC0B2270411126, AARUtility) concatenates the nil into a
	//     bundle name, x64:34f7b9134b089568.lua:2387, while the AAR menu opens. ui/main.lua opens the AAR
	//     from its own top level, BEFORE it registers the main and director menus, so the error ends that
	//     chunk too: the AAR has no button prompts, and Esc from it lands on a frontend with no menus.
	//   - In a match, the reward lookup of every challenge-complete notification uses the nil as a 'for'
	//     limit, x64:4697c588421e6ee1.lua:3175, and the notification is lost.
	// Seasons 0-6 are BOCW's own rows in mtx_seasons.csv (7+ are Warzone placeholders); 6 was live at the end.
	constexpr const char* kSeasonStreamDvar = "loot_season_stream";
	constexpr const char* kSeasonStreamValue = "mtx_season_6";

	// Game thread, right before the UI's main chunk runs, on every boot profile. A value some backend
	// did send is left alone.
	void EnsureSeasonStream() {
		static int s_Sets = 0, s_Failures = 0;
		static bool s_ForeignLogged = false;
		if (!Client::g_Pointers) return;

		std::uintptr_t* const dvar = Client::g_Pointers->FindDvar(kSeasonStreamDvar);
		if (!dvar) {
			if (++s_Failures <= 5) {
				LOG("LUI", WARN, "Season: {} is not registered at this UI load; trying again at the next one.",
					kSeasonStreamDvar);
			}
			return;
		}
		const std::string before = Client::g_Pointers->ReadDvarString(dvar);
		if (!before.empty()) {
			if (before != kSeasonStreamValue && !s_ForeignLogged) {
				s_ForeignLogged = true;
				LOG("LUI", INFO, "Season: {} is already '{}', left alone.", kSeasonStreamDvar, before);
			}
			return;
		}

		const bool wrote = Client::g_Pointers->WriteDvarString(dvar, kSeasonStreamValue);
		const std::string after = wrote ? Client::g_Pointers->ReadDvarString(dvar) : std::string{};
		if (after == kSeasonStreamValue) {
			if (++s_Sets <= 5) {
				LOG("LUI", INFO, "Season: {} '' -> '{}' before ui/main.lua (the publisher variable LIVE would have "
					"sent), so the AAR and the in-match challenge notifications find their season.{}", kSeasonStreamDvar,
					after, s_Sets > 1 ? std::format(" Set #{}: something emptied it since the last UI load.", s_Sets) : "");
			}
			return;
		}
		if (++s_Failures <= 5) {
			LOG("LUI", ERROR, "Season: could not set {} ({}; reads back '{}'). The AAR will raise 'attempt to "
				"concatenate a nil value', lose its button prompts and leave the frontend without its menus.",
				kSeasonStreamDvar, wrote ? "write dropped" : "string setters unresolved or not a string dvar", after);
		}
	}
}

// The chunk runner (dump_anchors.hpp, kDump_LUI_RunFile). A chunk missing from the luafile pool never
// reaches the handler above: DB_FindXAssetHeader raises a sys_error for a missing luafile and the process
// exits. On LIVE + backend the content slots read loaded while no ffotd zone ever is, so map load and
// the return to the frontend both ask for ui/ffotd_tu<N>.lua and die (22:45, 22:46). Only ui/ffotd*
// names are checked, so a wrong pool read can at worst skip the fix-of-the-day Lua, never ui/main.lua.
// Returns 0, the original's "failed", when it skips; every caller ignores it. Game thread only.
template <>
std::uint8_t Client::Hook::Hooks::HK_LUI_RunFile::hkCallback(void* luaState, const char* name) {
	if (!luaState || !name) {
		return m_Original(luaState, name);
	}

	const bool ffotd = _strnicmp(name, "ui/ffotd", 8) == 0;
	const std::uint64_t asset = Client::Scripting::HashScriptName(name);
	const int loaded = ffotd ? Client::Scripting::LuaFileLoaded(asset) : 1;

	if (ffotd && loaded == 0) {
		static int s_Skips = 0;
		++s_Skips;
		if (s_Skips <= 5 || s_Skips % 50 == 0) {
			LOG("LUI", WARN, "LUI_RunFile {} (asset {:016x}) skipped (x{}): not in the luafile pool, no ffotd zone is "
				"loaded. Running it would end the process with a missing-asset sys_error.", name, asset, s_Skips);
		}
		return 0;
	}

	static std::unordered_map<std::string, int> s_Seen;
	if (s_Seen.size() < 64 && s_Seen.emplace(name, 0).second) {
		LOG("LUI", INFO, "LUI_RunFile {} (asset {:016x}): {}", name, asset, !ffotd ? "passed through"
			: loaded == 1 ? "in the luafile pool, running it" : "luafile pool unreadable, running it");
	}
	const bool mainChunk = _stricmp(name, "ui/main.lua") == 0;
	// Before the chunk, not after: its own top level opens the AAR.
	if (mainChunk) {
		EnsureSeasonStream();
	}
	const std::uint8_t ran = m_Original(luaState, name);
	// Our cw-mod/ui_scripts go in right after the UI's own main chunk, before the first menu is built.
	if (mainChunk) {
		Client::Game::UiScripts::OnMainLoaded(luaState);
		// "unlock_all": the menus were rebuilt, so the battle pass model is filled again on the next tick.
		Client::Game::UnlockAll::OnUiLoaded();
	}
	return ran;
}
