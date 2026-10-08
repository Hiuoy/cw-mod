// Opening a LUI menu by name or hash - a replay of the engine's own `openmenu` console command
// (Cmd_OpenMenu_f, 0x7FF726FC37E0), step for step. See kDump_g_luiCtx in dump_anchors.hpp.
#include "common.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace Client::Game {
	namespace {
		constexpr std::uint64_t kMask63 = 0x7FFFFFFFFFFFFFFFULL;
		constexpr std::uint64_t kMask60 = 0x0FFFFFFFFFFFFFFFULL;   // what the Lua decompiler prints

		// The dispatch runs the menu builder all the way through Lua. The builder's own errors are
		// caught by the engine's lua_pcall (and end in the suppressed fatal); this __try is for a
		// fault in native code on the way, which would otherwise kill the process with no log line.
		//
		// The controller IS passed, unlike Cmd_OpenMenu_f's -1. With -1 the event carries no
		// "controller", every builder gets controller = nil, and any menu with a button prompt dies
		// in CoD.Menu.AddGamepadButtonCallbackFunction with "'for' initial value must be a number"
		// (measured 2026-09-23: ZMUpgrades and ~10 others). The console command only ever worked for
		// menus without prompts.
		bool SafeDispatch(Functions::LUI_DispatchAddMenuEventT* fn, const char* root, std::uint64_t hash,
			int controller, void* L) {
			__try { fn(root, hash, controller, L); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
	}

	bool Pointers::ParseMenuHash(const char* in, std::uint64_t& out) {
		if (!in) return false;
		std::string s = in;
		while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
		while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();

		// A shorter bare hex word is far likelier a name, and no real menu name is 15+ hex digits.
		const bool prefixed = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
		if (prefixed) s.erase(0, 2);
		if (s.empty() || s.size() > 16) return false;
		if (!prefixed && s.size() < 15) return false;
		if (s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;

		out = std::strtoull(s.c_str(), nullptr, 16);
		return true;
	}

	std::string Pointers::OpenLuiMenu(const char* nameOrHash, bool force, int localClient) {
		if (!nameOrHash || !*nameOrHash) return "Enter a menu name or a hash.";
		if (!this->m_g_luiCtx || !this->m_LUI_DispatchAddMenuEvent || !this->m_LUI_GetRootName
			|| !this->m_CL_LocalClientToController || !this->m_UI_SetUiActive) {
			return "The openmenu anchors did not resolve on this build.";
		}
		void* L = *this->m_g_luiCtx;
		if (!L) return "LUI is not up yet (the Lua state is null). Reach the main menu first.";

		std::uint64_t hash = 0;
		const bool byHash = Pointers::ParseMenuHash(nameOrHash, hash);
		if (!byHash) hash = Pointers::HashString(nameOrHash);   // FNV-1a over the lowercased name
		hash &= kMask63;
		const std::string label = byHash ? std::format("0x{:016X}", hash) : std::format("'{}'", nameOrHash);

		// Check against the live registry. A hash copied out of the Lua dump is only 60 bits wide;
		// widen it when exactly one registry key shares those bits.
		std::vector<std::uint64_t> registry;
		const bool registryUsable = this->LuaCollectMenuHashes(registry).empty();
		if (registryUsable && !std::binary_search(registry.begin(), registry.end(), hash)) {
			std::uint64_t widened = 0;
			int matches = 0;
			for (const std::uint64_t k : registry) {
				if ((k & kMask60) == (hash & kMask60)) { widened = k; ++matches; }
			}
			if (byHash && matches == 1) {
				hash = widened;
			}
			else if (!force) {
				return std::format("{} (hash 0x{:016X}) is not in LUI.createMenu here, so it was NOT sent. "
					"Menus register per UI state (frontend vs in-game). Tick 'force' to send it anyway - "
					"a few real menus ('lobby') are not in the registry.", label, hash);
			}
		}

		// Cmd_OpenMenu_f maps the local client to a controller before asking for the root; -1 means
		// none is bound, and LUI_GetRootName indexes by 176 * controller with no bounds check.
		const int controller = this->m_CL_LocalClientToController(localClient);
		if (controller < 0) return std::format("Local client {} has no controller bound.", localClient);
		const char* const root = this->m_LUI_GetRootName(controller);
		if (!root) return std::format("No UI root for controller {}.", controller);

		// The console command raises UI flag 0x10 before dispatching, so the root takes input for the
		// menu it is about to build.
		this->m_UI_SetUiActive(localClient, true);

		LuiMenus::g_LastError.clear();
		LuiMenus::g_InDispatch = true;
		const bool ok = SafeDispatch(this->m_LUI_DispatchAddMenuEvent, root, hash, controller, L);
		LuiMenus::g_InDispatch = false;
		LuiMenus::g_HandOpened = true;

		LOG("LuiMenus", INFO, "open {} (hash 0x{:016X}) root '{}' controller {} -> {}{}", label, hash, root,
			controller, ok ? "dispatched" : "FAULTED",
			LuiMenus::g_LastError.empty() ? "" : std::format(", builder raised: {}", LuiMenus::g_LastError));

		if (!ok) {
			return std::format("{} faulted inside the dispatch (contained; the game is still alive).", label);
		}
		if (!LuiMenus::g_LastError.empty()) {
			return std::format("{} was sent but its builder raised (suppressed, the engine would have quit): {}",
				label, LuiMenus::g_LastError);
		}
		return std::format("Sent {} (hash 0x{:016X}) to '{}'. If nothing appeared, the menu refused this "
			"UI state without raising.", label, hash, root);
	}
}
