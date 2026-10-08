#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/dump_anchors.hpp"

// Why suppressing Com_Error was not enough.
//
// The BLZBNTBGS dialog is not a Com_Error. Lua_FirstParty_GetErrorMessage (0x7FF71E91EF40) is a
// lua_CFunction that formats the stored BGS code for LUI, so the dialog is a MENU asking the engine
// "is there a first-party error, and what is it?". That also explains the second symptom - the error
// coming back on every menu change - because each new menu asks again and the state is still set.
//
// The state is three fields on the first-party object: +56 state (3 = signed in, 4 = STATE_ERROR),
// +60 has-error, +64 the BGS code. FirstParty_StateTick switches on +56 and fans 2/3/4 out to a
// listener list, which is how ONE error reaches both the LUI popup and the Com_Error reporters. So
// the correct intervention is neither the popup nor the reporters but the state: with nothing ever
// stored, there is nothing for any consumer to find.
//
// Exactly two functions write STATE_ERROR, and both are detoured here. Note this is not merely
// cosmetic suppression: +56 is the same field FirstParty_IsSignedIn_State3 tests for 3, so letting an
// error land also flips the client to "not signed in".
//
// Scope, as with the reporters: installed only when the boot marker requested online mode. On a
// normal launch a genuine first-party error still sets state, still raises the popup, still fatals.

namespace {
	// One line per distinct code. These fire from a polling state machine, so logging every call
	// would flood the console with the same error many times a second and bury everything else.
	std::atomic<int> s_LastSuppressedCode{ -1 };

	void ReportOnce(int code, const char* which) {
		if (s_LastSuppressedCode.exchange(code, std::memory_order_relaxed) == code) {
			return;
		}
		const unsigned display = code >= 1000000 ? static_cast<unsigned>(code - 1000000) : static_cast<unsigned>(code);
		LOG("Bnet", WARN, "Suppressed first-party STATE_ERROR via {} — code {} (would have shown "
			"BLZBNTBGS{:08X}). Online-mode test boot: the error is dropped, not resolved.",
			which, code, display);
	}
}

template <>
std::uint64_t Client::Hook::Hooks::HK_FirstParty_SetError::hkCallback(std::uintptr_t firstPartyObj, int code) {
	if (!Client::Game::OnlineMode::g_SuppressBnetErrors.load(std::memory_order_relaxed)) {
		return m_Original(firstPartyObj, code);
	}
	ReportOnce(code, "FirstParty_SetError");
	return 0;
}

template <>
std::uint64_t Client::Hook::Hooks::HK_FirstParty_SetErrorState::hkCallback(std::uintptr_t firstPartyObj) {
	if (!Client::Game::OnlineMode::g_SuppressBnetErrors.load(std::memory_order_relaxed)) {
		return m_Original(firstPartyObj);
	}
	// The codeless variant carries no code of its own; report it under a sentinel so it still shows
	// up exactly once rather than sharing the de-dup slot with a real code.
	ReportOnce(-2, "FirstParty_SetErrorState");
	return 0;
}

// The third writer, and the one that actually fires: the BGS "disconnected" callback (see
// dump_anchors.hpp). The two above were never called on an online + backend boot. This one stores
// BLZBNTBGS000003EA about 10 s in, on every such boot. director_lan never looked; the title screen
// polls +60 every 400 ms and raises the dialog. The original runs untouched, notify and state 5
// (disconnected) included, so everything downstream behaves as on the boots that reached Login
// Complete. Only the stored error goes: +60 (what the title screen polls) and +64 (the text it shows).
template <>
std::uint64_t Client::Hook::Hooks::HK_FirstParty_OnBgsDisconnected::hkCallback(std::uintptr_t firstPartyObj,
	std::uintptr_t unused, const std::uint32_t* bgsCode) {
	const std::uint64_t ret = m_Original(firstPartyObj, unused, bgsCode);
	if (!Client::Game::OnlineMode::g_SuppressBnetErrors.load(std::memory_order_relaxed) || !firstPartyObj) {
		return ret;
	}

	auto* const obj = reinterpret_cast<std::uint8_t*>(firstPartyObj);
	std::uint8_t hasError = 0, disconnected = 0;
	std::uint32_t stored = 0;
	int state = -1;
	Client::Game::SafeRead(obj + 60, hasError);
	Client::Game::SafeRead(obj + 64, stored);
	Client::Game::SafeRead(obj + 56, state);
	Client::Game::SafeRead(obj + 61, disconnected);
	const bool cleared = hasError
		&& Client::Game::SafeWrite(obj + 60, std::uint8_t{ 0 })
		&& Client::Game::SafeWrite(obj + 64, std::uint32_t{ 0 });

	static std::atomic<int> s_Calls{ 0 };
	if (s_Calls.fetch_add(1, std::memory_order_relaxed) < 8) {
		std::uint32_t raw = 0;
		if (bgsCode) Client::Game::SafeRead(bgsCode, raw);
		LOG("Bnet", WARN, "Battle.net disconnected (BGS code {}): {} Original ran: state {} (5 = disconnected), "
			"disconnected flag {}.", raw,
			cleared ? std::format("cleared the stored error {} (BLZBNTBGS{:08X}), so the title screen has nothing to show.",
				stored, stored >= 1000000u ? stored - 1000000u : stored)
				: hasError ? std::string("could NOT clear the stored error; the dialog will still show.")
				: std::string("no error stored (it only stores one mid-sign-in)."),
			state, disconnected);
	}
	return ret;
}
