#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/dw_backend.hpp"
#include "game/dump_anchors.hpp"

#include <atomic>

// THE FIX FOR THE CRASH AT "[status 27] Login Complete".
//
// FirstParty_GetLocalUserIndex_ViaSession (0x7FF7297D9EE0) is
//
//     *(uint*)( *(void**)(g_firstPartyManagerObj + 24) + 0xD0 )
//
// and on a studio-auth boot the session at +24 is null, so it faults reading 0xD0 with rcx=0.
// The manager's constructor zeroes that field and login flow 9 goes straight to the Demonware auth
// task, skipping Battle.net first-party entirely — our own deliberate bypass. Nothing ever assigns
// it, so no amount of waiting will make it appear.
//
// WHY RETURNING 0 IS THE RIGHT ANSWER AND NOT A PATCH-OVER. session+0xD0 is the LOCAL USER /
// CONTROLLER INDEX. The only consumer is FirstParty_RefreshUserRequest (0x7FF7297E81B0), which
// passes it verbatim to BdUser_SubmitRequestForLocalUser (0x7FF729F3C084) as the user index; that
// function resolves a per-user object from the index and reports bdError 3027 through the request's
// callback when it resolves to nothing. On a single-local-user PC the live user IS index 0. So the
// value we hand back is the value a real session would have held, not a number chosen to survive.
//
// WHY THIS FUNCTION AND NOT THE SESSION GETTER. The in-game census on 2026-08-02 hooked
// FirstParty_GetSession_MayBeNull, substituted a zeroed stand-in and logged every distinct call site
// that asked while the session was null. The boot reached the main menu and produced exactly ONE
// line, and it was this function (RA 0x7FF7297D9EF1). One site, one call, one field. Serving that
// integer here means no synthesised session object and no fake vtable ever enter the process; the
// census hook stays installed as a watchman that returns null exactly as the original does.
//
// SCOPE. Installed only when DwBackend is enabled. On any other boot the detour does not exist.
// Even when installed, the non-null path calls straight through, so the only behaviour that can
// change is the one that currently crashes.

namespace {
	std::atomic<bool> g_Reported{ false };
}

template <>
std::uint32_t Client::Hook::Hooks::HK_FirstParty_GetLocalUserIndex::hkCallback() {
	// Read the manager OBJECT directly rather than calling FirstParty_GetManagerSingleton. That
	// getter is a magic-statics wrapper whose only job is to run the constructor once and return
	// &this, so the address is a compile-time constant; going through it would add a TLS read and a
	// guarded init for nothing. Before construction the object is a zeroed static, which reads as
	// "no session" — the correct answer, so the early call is safe too.
	const auto* const mgr =
		static_cast<const std::uint8_t*>(g_Pointers->m_g_firstPartyManagerObj);

	const void* session = nullptr;
	if (mgr) {
		session = *reinterpret_cast<void* const*>(mgr + Client::Game::kFirstPartyMgr_SessionOffset);
	}

	if (session) {
		// A real session exists: behave exactly like the original, field offset and all.
		return *reinterpret_cast<const std::uint32_t*>(
			static_cast<const std::uint8_t*>(session) + Client::Game::kFirstPartySession_LocalUserIndex);
	}

	// Logged once, not per call. The census measured this as a single call per boot, but that was one
	// boot — if a later build starts calling it every frame we want the log to stay readable, and if
	// it stops being called at all we want to notice the silence.
	if (!g_Reported.exchange(true)) {
		LOG("FirstParty", INFO,
			"local-user-index read with a NULL first-party session (studio auth never creates one) — "
			"returning 0, i.e. controller 0. This is the read that used to crash one second after "
			"[status 27] Login Complete.");
	}
	return 0;
}
