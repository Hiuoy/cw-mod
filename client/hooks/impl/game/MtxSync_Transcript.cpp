#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/dump_anchors.hpp"

#include <utility/nt.hpp>

#include <atomic>
#include <intrin.h>

// B5: why the marketplace sync (MtxSync) never leaves state 0, so Inventory_Frame never requests the
// inventory and every HasEntitlement stays false. Read-only; each detour calls the original first.
//
// Boot 2026-09-17 19:19 had every gate we already read open (pubVars 4, slots 2,2, +5776 set) and
// mtxSync 0 for a whole minute. The four detours name the next closed gate:
//
//   ShouldStart never logs         UpdateSigninState stops earlier (active flag / presence / guest check)
//   ShouldStart -> 0               dvar, IsOnlineReady or backoff timer (logged) closed; state and pubVars known
//   RequestBnetTokenZEUS, state 0  nodw is true or the timer closed inside it
//   no OnBnetToken after state 1   Battle.net never answers the ZEUS token (the expected wall)
//   IsDoneOrInFlight never logs    Inventory_Frame stops before it (dvar, AreOnlineDataMapsReady, LAN check)
//
// Logs the first call and then only changes of (return, state), 24 lines per detour at most.

namespace {
	constexpr int kMaxLines = 24;

	int MtxState(std::int32_t controller) {
		int state = -1;
		if (controller >= 0 && controller < 2 && Client::g_Pointers && Client::g_Pointers->m_g_mtxSyncState) {
			Client::Game::SafeRead(static_cast<const char*>(Client::g_Pointers->m_g_mtxSyncState) + 4 * controller, state);
		}
		return state;
	}

	struct ChangeLog {
		std::atomic<int> lines{ 0 };
		std::atomic<std::uint64_t> last{ ~0ull };

		// True when (return, state) differs from the previous call and budget remains. The caller is not
		// part of the key: two callers alternating every frame spent the whole budget in one second.
		bool ShouldLog(std::uint8_t ret, int state, std::uintptr_t) {
			const std::uint64_t key = (static_cast<std::uint64_t>(ret) << 56)
				^ (static_cast<std::uint64_t>(static_cast<std::uint8_t>(state)) << 48);
			if (last.exchange(key, std::memory_order_relaxed) == key) return false;
			return lines.fetch_add(1, std::memory_order_relaxed) < kMaxLines;
		}
	};
	ChangeLog g_ShouldStart, g_RequestToken, g_IsDone;
	std::atomic<int> g_TokenLines{ 0 };

	std::uintptr_t CallerRva(void* returnAddress) {
		const Common::Utility::NT::Library game;
		return reinterpret_cast<std::uintptr_t>(returnAddress) - reinterpret_cast<std::uintptr_t>(game.GetPtr());
	}
}

template <>
std::uint8_t Client::Hook::Hooks::HK_MtxSync_ShouldStart::hkCallback(std::int32_t controller) {
	const std::uint8_t ret = m_Original(controller);
	const std::uintptr_t caller = CallerRva(_ReturnAddress());
	const int state = MtxState(controller);
	if (g_ShouldStart.ShouldLog(ret, state, caller)) {
		// Backoff timer (sub_7FF7262A2F00): open when now >= [+20]+[+28] and (![+16] || [+24] <= [+8]).
		std::int32_t t8 = -1, t20 = -1, t24 = -1, t28 = -1;
		std::uint8_t t16 = 0xEE;
		const std::uintptr_t timer = reinterpret_cast<std::uintptr_t>(g_Pointers->m_g_mtxSyncBackoff) + 36 * controller;
		Game::SafeRead(reinterpret_cast<const void*>(timer + 8), t8);
		Game::SafeRead(reinterpret_cast<const void*>(timer + 16), t16);
		Game::SafeRead(reinterpret_cast<const void*>(timer + 20), t20);
		Game::SafeRead(reinterpret_cast<const void*>(timer + 24), t24);
		Game::SafeRead(reinterpret_cast<const void*>(timer + 28), t28);
		LOG("MtxSync", INFO,
			"ShouldStart(c{}) -> {} (1 = request ZEUS token) state={} caller=+0x{:X} | backoff: start={} delay={} "
			"limited={} tries={} max={} (needs dvar + IsOnlineReady + PubVars 4 + state 0 + timer open)",
			controller, ret, state, caller, t20, t28, t16, t24, t8);
	}
	return ret;
}

template <>
std::uint8_t Client::Hook::Hooks::HK_MtxSync_RequestBnetTokenZEUS::hkCallback(std::int32_t controller) {
	const int before = MtxState(controller);
	const std::uint8_t ret = m_Original(controller);
	const int after = MtxState(controller);
	if (g_RequestToken.ShouldLog(ret, after, CallerRva(_ReturnAddress()))) {
		LOG("MtxSync", INFO,
			"RequestBnetTokenZEUS(c{}) -> {} state {} -> {} (stays 0 = dvar off, nodw or timer closed; "
			"1 = asked Battle.net for the ZEUS token)", controller, ret, before, after);
	}
	return ret;
}

template <>
std::uint8_t Client::Hook::Hooks::HK_MtxSync_OnBnetToken::hkCallback(std::int32_t error, std::int64_t* token) {
	std::int64_t length = -1;
	if (!error && token) {
		Game::SafeRead(token + 1, length);
	}
	const std::uint8_t ret = m_Original(error, token);
	if (g_TokenLines.fetch_add(1, std::memory_order_relaxed) < kMaxLines) {
		LOG("MtxSync", INFO,
			"OnBnetToken error={} tokenLen={} -> {} (error 0 sends the marketplace sync; else finishes with backoff) "
			"state now {}", error, length, ret, MtxState(0));
	}
	return ret;
}

template <>
std::uint8_t Client::Hook::Hooks::HK_MtxSync_IsDoneOrInFlight::hkCallback(std::int32_t controller) {
	const std::uint8_t ret = m_Original(controller);
	const std::uintptr_t caller = CallerRva(_ReturnAddress());
	const int state = MtxState(controller);
	if (g_IsDone.ShouldLog(ret, state, caller)) {
		LOG("MtxSync", INFO,
			"IsDoneOrInFlight(c{}) -> {} state={} caller=+0x{:X} (Inventory_Frame reached its last gate; "
			"1 lets it request the inventory)", controller, ret, state, caller);
	}
	return ret;
}
