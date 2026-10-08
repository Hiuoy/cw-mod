#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/dump_anchors.hpp"

#include <utility/nt.hpp>

#include <atomic>
#include <format>
#include <intrin.h>
#include <string>

// B6 lobby waiver. The 2026-09-24 06:24 boot (DwFetch transcript) reached every required bit of the
// Demonware fetch checklist except five, and IDA says all five are public matchmaking or commerce:
//   G 0x40      core_ffotd listed in the LPC manifest       (we serve no ffotd; TU35 files ERR_DROP)
//   Y 0x1000000 ingamestore_<lang>.json fetched and parsed  (the store; only fetched after G)
//   R 0x20000   marketplace inventory loaded                (needs a Battle.net ZEUS token, B5)
//   N 0x2000    dedicated-server QoS done                   (Lua Lobby.MatchmakingAsync.EventQoSHosts)
//   W 0x400000  SocketRouter relay bound                    (Lua PumpLocalClients, relay tokens)
// Lua's BeginLivePlay opens the online menu (and so builds the lobby the mode tiles bind to) only when
// Engine.IsDemonwareFetchingDone is true, so those five keep the tiles padlocked forever.
//
// This detour answers true for Lua ONLY when every missing required bit is one of those five. Any other
// clear bit (presence, login, publisher variables, data maps, LPC manifest...) keeps the honest false.
// Return-address guarded: only the call from Lua_IsDemonwareFetchingDone_Impl is changed. The engine's
// other DwFetch_IsDone callers (the in-game store tick, matchmaking) still get the truth, so nothing
// starts dedicated-server matchmaking or a store fetch because of this.
// Opt out: "lobby_waiver": false in cw-mod.json (then the detour is not installed at all).

namespace {
	using namespace Client;

	constexpr int kMaxLines = 20;
	std::atomic<int> g_Lines{ 0 };
	std::atomic<std::uint64_t> g_Last{ ~0ull };

	std::uintptr_t GameBase() {
		static const std::uintptr_t base = [] {
			const Common::Utility::NT::Library game;
			return reinterpret_cast<std::uintptr_t>(game.GetPtr());
		}();
		return base;
	}

	bool InWindow(std::uintptr_t ra, std::uintptr_t base, std::size_t size) {
		return base && ra >= base && ra < base + size;
	}

	std::string Names(std::uint32_t bits) {
		static constexpr struct { std::uint32_t mask; const char* name; } kNames[] = {
			{ 0x40, "G ffotd listed" }, { 0x2000, "N dedi QoS" }, { 0x20000, "R inventory" },
			{ 0x400000, "W relay bind" }, { 0x1000000, "Y in-game store" },
		};
		std::string out;
		for (const auto& n : kNames) {
			if (bits & n.mask) out += std::format("{}{}", out.empty() ? "" : ", ", n.name);
		}
		return out.empty() ? std::string("none") : out;
	}

	// One line per change of (verdict, missing), capped: Lua polls this every frame on the overlay.
	void Log(std::uint32_t controller, bool waived, std::uint32_t missing) {
		const std::uint64_t key = (static_cast<std::uint64_t>(waived) << 32) | missing;
		if (g_Last.exchange(key, std::memory_order_relaxed) == key) return;
		if (g_Lines.fetch_add(1, std::memory_order_relaxed) >= kMaxLines) return;
		if (waived) {
			LOG("DwFetch", INFO, "Lua IsDemonwareFetchingDone(c{}): missing only 0x{:X} ({}) -> answering TRUE "
				"(lobby waiver; the engine's own callers still see false)", controller, missing, Names(missing));
		}
		else {
			LOG("DwFetch", INFO, "Lua IsDemonwareFetchingDone(c{}): missing 0x{:X}, of which 0x{:X} is not waivable "
				"-> honest false", controller, missing, missing & ~Game::DwFetch::kWaivable);
		}
	}
}

template <>
std::uint8_t Client::Hook::Hooks::HK_DwFetch_IsDone::hkCallback(std::uint32_t controller) {
	Game::DwFetch::ResetLastGot();
	const std::uint8_t ret = m_Original(controller);
	if (ret || !g_Pointers) {
		return ret;
	}

	const std::uintptr_t ra = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
	if (!InWindow(ra, g_Pointers->m_Lua_IsDemonwareFetchingDone_ImplBase, Game::kLua_IsDemonwareFetchingDone_ImplSize)) {
		return ret;
	}

	std::uint32_t got = 0;
	if (!Game::DwFetch::LastGot(got)) {
		// The transcript detour did not see the call, so there is no mask to judge. Stay honest.
		static std::atomic_bool s_Warned{ false };
		if (!s_Warned.exchange(true)) {
			LOG("DwFetch", ERROR, "lobby waiver: no fetch mask seen (transcript not installed?) -> honest false");
		}
		return ret;
	}

	const std::uint32_t missing = Game::DwFetch::kRequired & ~got;
	const bool waived = missing != 0 && (missing & ~Game::DwFetch::kWaivable) == 0;
	Log(controller, waived, missing);
	return waived ? 1 : ret;
}
