#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/dump_anchors.hpp"

#include <utility/nt.hpp>

#include <array>
#include <atomic>
#include <format>
#include <intrin.h>
#include <string>

// B6: why an online boot never builds the lobby the mode tiles bind to. Read-only; calls the original
// first and never changes its answer.
//
// The chain, read from the Lua dump and IDA on 2026-09-24:
//   PressStart -> ShouldBeginLAN false -> BeginLivePlay, which navigates to the online menu (and so
//   runs LobbyHostStart) only when Engine.IsDemonwareFetchingDone && Engine.AreLocalFilesReady.
//   Otherwise it shows the "connecting to online services" overlay and waits.
// IsDemonwareFetchingDone ends here: one bit per ready subsystem ORed into *got, true only when every
// required bit is set. So the clear required bits ARE the list of what the backend still owes.
//
// Logs the first call, every change of (return, got), and each new immediate caller. The engine shows
// the same mask as letters (A = bit 0, '-' = clear), so the log prints it that way too.

namespace {
	constexpr int kMaxLines = 40;
	// Required mask is 0x17337FA, or 0x17B37FA while MtxSync is not done; that extra bit self-sets.
	constexpr std::uint32_t kMtxNotDone = 0x80000;

	struct Bit {
		std::uint32_t mask;
		const char* what;
	};
	// Names from the predicates in DwFetch_GetStatus (0x7FF7262A4BA0). Only the required ones matter.
	constexpr std::array<Bit, 17> kRequiredBits{ {
		{ 0x2,       "B presence ok + signed in online (B1 presence detour)" },
		{ 0x8,       "D g_netSessionLaunchState == 8" },
		{ 0x10,      "E LiveUser_IsOnlineReady (waived when a sibling controller is online-ready)" },
		{ 0x20,      "F LPC manifest loaded (B3)" },
		{ 0x40,      "G content slot installed (off_7FF72A395F38)" },
		{ 0x80,      "H content slot 0 == 2" },
		{ 0x100,     "I PubVars state == 4 (retrievePublisherVariables, 95/3)" },
		{ 0x200,     "J content slot 1 == 2" },
		{ 0x400,     "K online stats data maps + 10 data maps ready" },
		{ 0x1000,    "M byte 0x7FF72E7FA64F" },
		{ 0x2000,    "N state 0x7FF73295357C >= 4" },
		{ 0x10000,   "Q LiveUser+5776 != 0" },
		{ 0x20000,   "R per-controller 0x7FF733B79520: +944 && +900 == 3 (waived when a dvar is off)" },
		{ 0x100000,  "U two dvar-gated checks (0x7FF7274B3170 / 0x7FF726A7DFC0)" },
		{ 0x200000,  "V byte 0x7FF72EF33444 (only when a dvar >= 2)" },
		{ 0x400000,  "W state 0x7FF73082D838 in 2..3" },
		{ 0x1000000, "Y content slot 10 == 2" },
	} };

	std::uintptr_t Base() {
		const Common::Utility::NT::Library game;
		return reinterpret_cast<std::uintptr_t>(game.GetPtr());
	}

	template <typename T>
	T ReadDump(std::uintptr_t dumpAbs, T fallback) {
		T value = fallback;
		Client::Game::SafeRead(reinterpret_cast<const void*>(Base() + (dumpAbs - Client::Game::kDumpImagebase)), value);
		return value;
	}

	std::string Letters(std::uint32_t got) {
		std::string out;
		for (int i = 0; i < 25; ++i) {
			out += (got >> i & 1) ? static_cast<char>('A' + i) : '-';
			if (i != 24) out += '.';
		}
		return out;
	}

	// The raw state behind the clear bits, so the next step starts from a value, not a guess.
	std::string State(std::uint32_t controller) {
		using namespace Client::Game;
		std::string slots;
		for (int i = 0; i < 11; ++i) {
			slots += std::format("{}{}", i ? "," : "", ReadDump<std::int32_t>(kDump_g_onlineContentSlots + 4 * i, -1));
		}
		const std::uintptr_t ctrl = kDump_g_dwFetchCtrl20000 + 801056ull * (controller < 2 ? controller : 0);
		return std::format("launchState={} pubVars={} slots=[{}] M={} N={} R=({},{}) V={} W={}",
			ReadDump<std::int32_t>(kDump_g_netSessionLaunchState, -1),
			ReadDump<std::int32_t>(kDump_g_pubVarsState, -1), slots,
			ReadDump<std::uint8_t>(kDump_g_dwFetchFlag1000, 0xEE),
			ReadDump<std::int32_t>(kDump_g_dwFetchState2000, -1),
			ReadDump<std::uint8_t>(ctrl + 944, 0xEE), ReadDump<std::int32_t>(ctrl + 900, -1),
			ReadDump<std::uint8_t>(kDump_g_dwFetchFlag200000, 0xEE),
			ReadDump<std::int32_t>(kDump_g_dwFetchState400000, -1));
	}

	thread_local bool t_HaveGot = false;
	thread_local std::uint32_t t_Got = 0;

	std::atomic<int> g_Lines{ 0 };
	std::atomic<std::uint64_t> g_Last{ ~0ull };
	std::array<std::atomic<std::uintptr_t>, 12> g_Callers{};

	// True the first time this caller RVA is seen (up to 12 distinct callers).
	bool NewCaller(std::uintptr_t rva) {
		for (auto& slot : g_Callers) {
			std::uintptr_t seen = slot.load(std::memory_order_relaxed);
			if (seen == rva) return false;
			if (seen == 0) {
				if (slot.compare_exchange_strong(seen, rva)) return true;
				if (seen == rva) return false;              // another thread just recorded it
			}
		}
		return false;
	}
}

void Client::Game::DwFetch::ResetLastGot() {
	t_HaveGot = false;
}

bool Client::Game::DwFetch::LastGot(std::uint32_t& got) {
	got = t_Got;
	return t_HaveGot;
}

template <>
std::uint8_t Client::Hook::Hooks::HK_DwFetch_GetStatus::hkCallback(std::uint32_t controller, std::uint32_t* got,
	void* scratch) {
	const std::uint8_t ret = m_Original(controller, got, scratch);
	std::uint32_t mask = 0;
	if (got) Game::SafeRead(got, mask);
	t_Got = mask;
	t_HaveGot = got != nullptr;

	const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - Base();
	if (NewCaller(caller) && g_Lines.load(std::memory_order_relaxed) < kMaxLines) {
		LOG("DwFetch", INFO, "new caller +0x{:X} (dump 0x{:X}) c{}", caller, caller + Game::kDumpImagebase, controller);
	}

	const std::uint64_t key = (static_cast<std::uint64_t>(ret) << 32) | mask;
	if (g_Last.exchange(key, std::memory_order_relaxed) == key) return ret;
	if (g_Lines.fetch_add(1, std::memory_order_relaxed) >= kMaxLines) return ret;

	std::string missing;
	for (const Bit& bit : kRequiredBits) {
		if (!(mask & bit.mask)) {
			missing += std::format("\n    missing 0x{:X} {}", bit.mask, bit.what);
		}
	}
	LOG("DwFetch", INFO, "IsDemonwareFetchingDone(c{}) -> {} got=0x{:07X} {} mtx={}{}\n    {}{}",
		controller, ret, mask, Letters(mask), (mask & kMtxNotDone) ? "not-done(ok)" : "done",
		ret ? " -- ALL REQUIRED BITS SET: BeginLivePlay may open the online menu" : "",
		State(controller), missing.empty() ? std::string("\n    (no required bit clear; a waiver or MtxSync decides)") : missing);
	return ret;
}
