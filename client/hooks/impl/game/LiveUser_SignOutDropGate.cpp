#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/dw_backend.hpp"
#include "game/dump_anchors.hpp"

// THE MENU-RESTART LOOP ON AN ONLINE BOOT, and what it actually is.
//
// Symptom, measured 2026-08-02: login completes ([status 27]) and one second later
//
//     BB_Alert type='err_drop' msg='An error occurred: Boy 501 Gothic Missile'
//
// then the frontend restarts and logs in again, forever. The word-code is Com_FormatHash64 of an
// error hash with no localized string (on later boots it resolved to "You have been disconnected from
// the Call of Duty servers").
//
// B1, 2026-09-16 (static RE; names in the IDB). This was long read as a sign-out. It is the opposite:
//   - LiveUser_OnDwLoginComplete_SetState4 (0x7FF727F38F30) sets loginState +5768 := 4 (connected).
//   - LiveUser_UpdateSigninState calls LiveUser_OnDwConnected (0x7FF729722670) on entering 4.
//   - That calls this target (IDB: LiveUser_PromoteSigninOnline_MaybeDrop, 0x7FF729753740):
//
//     prev = signinState (+5764);            // 0 none, 1 local/signed-out, 2 signed in online
//     ... identity setup, signinState := 2, notify subsystems ...
//     if (prev is 0 or 2 || (prev == 1 && !Com_SessionMode_IsOnline())) return 0;
//     if (UI string dvar non-empty || session top nibble == 6) return 0;
//     *outMsg = "disconnected"; return 1;    // caller forces session mode offline (|0x6030), ERR_DROP
//
// A client that was signed in locally and is now connected in online session mode gets its
// promotion AND a drop to offline. None of the inputs come from the server. Returning 0 keeps the
// promotion (signinState really is 2 afterwards) and skips only the forced-offline + ERR_DROP, which
// is the verdict an offline boot gets. It is not a mask over a failure.
//
// The silence after login is a different gate: LiveUser_IsOnlineReady also needs first-party
// presence. See the LiveUser_FirstPartyPresenceOk detour.

template <>
char Client::Hook::Hooks::HK_LiveUser_SignOutDropGate::hkCallback(
	std::uint32_t controller, const char** outMsg) {
	// Read BEFORE calling through: the original overwrites it with 2, and the prior value is what
	// decides the verdict.
	std::uint32_t signinState = 0xFFFFFFFFu;
	if (g_Pointers->m_g_liveUserObjects && controller < 2) {
		const auto userObj = g_Pointers->m_g_liveUserObjects[controller];
		if (userObj) {
			signinState = *reinterpret_cast<const std::uint32_t*>(
				userObj + Client::Game::kLiveUser_SigninStateOffset);
		}
	}

	const char verdict = m_Original(controller, outMsg);

	if (verdict) {
		const char* const msg = (outMsg && *outMsg) ? *outMsg : "<none>";
		LOG("LiveUser", INFO,
			"controller {} promoted to signed-in-online (signinState {} -> 2). The engine "
			"also asked to drop to offline with '{}' because the prior state was local in online "
			"session mode; returning 0 skips that drop, the promotion stands.",
			controller, signinState, msg);
		return 0;
	}

	// Log the pass-throughs too, so a silent hook can be told apart from one that never ran.
	LOG("LiveUser", INFO,
		"controller {} promoted to signed-in-online (signinState {} -> 2); the engine "
		"asked for no drop, nothing overridden.", controller, signinState);
	return verdict;
}
