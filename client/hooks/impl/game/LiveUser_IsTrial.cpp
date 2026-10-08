#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/dump_anchors.hpp"

#include <utility/nt.hpp>

#include <intrin.h>
#include <mutex>

// "You don't own the game" on the mode tiles of an online boot (2026-09-24 06:24; the presence JSON the
// game sent said "trial":true). LuaUtils.IsTrial -> native 0x33698526482CB1F8 -> LiveUser_IsTrial
// (0x7FF728151A00):
//   if (nodw) return 0;                                   // offline: never a trial, why LAN is fine
//   if (!firstPartyMgr.licenses) return 1;                // studio auth: no Battle.net session at all
//   if (overrideDvar || FirstParty_IsFlagged()) return 0;
//   return !licenses.contains(0xCDE9);                    // the full-game product id
// With studio auth the Battle.net license list never exists, so every backend boot reads as a trial.
// The player owns the game (the client will not run otherwise); only the first-party proof is missing,
// the same gap as presence (B1). So on a backend boot the answer is what nodw gives offline: not a trial.
// DwBackend only; each call site that gets the changed answer is logged once, with the real one.

namespace {
	using namespace Client;

	constexpr std::size_t kMaxSites = 32;
	std::mutex g_Lock;
	std::uintptr_t g_Sites[kMaxSites]{};
	std::size_t g_SiteCount = 0;

	std::uintptr_t GameBase() {
		static const std::uintptr_t base = [] {
			const Common::Utility::NT::Library game;
			return reinterpret_cast<std::uintptr_t>(game.GetPtr());
		}();
		return base;
	}

	void RecordSite(std::uintptr_t ra) {
		std::lock_guard<std::mutex> lock(g_Lock);
		for (std::size_t i = 0; i < g_SiteCount; ++i) {
			if (g_Sites[i] == ra) return;
		}
		if (g_SiteCount >= kMaxSites) return;
		g_Sites[g_SiteCount++] = ra;
		const std::uintptr_t base = GameBase();
		const std::uintptr_t rva = ra >= base ? ra - base : ra;
		LOG("Trial", INFO, "IsTrial asked from {}+0x{:X} (dump 0x{:X}, site #{}): real answer TRUE (no Battle.net "
			"licenses on studio auth) -> answering FALSE, the full game, as nodw does offline",
			Client::g_GameModuleName, rva, rva + Game::kDumpImagebase, g_SiteCount);
	}
}

template <>
std::uint8_t Client::Hook::Hooks::HK_LiveUser_IsTrial::hkCallback() {
	const std::uint8_t real = m_Original();
	if (!real) {
		return real;
	}
	RecordSite(reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
	return 0;
}
