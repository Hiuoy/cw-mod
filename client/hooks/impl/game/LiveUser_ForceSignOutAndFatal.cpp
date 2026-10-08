#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"

// The function that actually puts BLZBNTBGS000003EA on screen, found on the fourth attempt.
//
// The three that were wrong, and what each measurement ruled out:
//   1. The two Com_Error Battle.net reporters. Popup unchanged, and it re-raised on every menu change
//      — so it was not a Com_Error dialog. (That inference was right; the conclusion drawn from it
//      was not.)
//   2. The two writers of first-party STATE_ERROR, on the theory that LUI was querying stored error
//      state. Log then showed the latched reporter suppressed and BOTH state writers never firing,
//      popup still present.
//   3. ErrorQueue_Push, the 4-slot ring LUI drains to render the popup — genuinely the choke point
//      for three separate producers, just not for ours. Armed and hooked, it never fired ONCE while
//      the dialog appeared exactly as before.
//
// Each of those was found by reasoning backwards from the message text. Reasoning forwards from what
// is actually running finds it immediately — it is the tail call of LiveFirstParty_Frame:
//
//   LiveFirstParty_Frame  (gated on !Dvar_GetBool(nodw) && byte_7FF7371BF1FF)
//     -> if Dvar_GetBool(0x7FF733D20160) && LiveUser_IsSignedIn(0) && ...4 more
//       -> LiveUser_ForceSignOutAndFatal()
//            LiveUser_HandleSignOut(0), LiveUser_HandleSignOut(1)
//            loginState (+5768) := 9, then := 10
//            build localised message from qword_7FF72AF57F28
//            g_pendingErrorLevel := 1024, g_pendingErrorMessage := msg
//            Com_Error(file, 0, 0x400, msg)          <-- direct. no queue, no reporter.
//
// It is a "you must stay signed in" watchdog, and it is a dead end by construction: every path out of
// it ends in Com_Error, so returning early is not skipping a fallback, it is the only way not to die.
//
// Worth being clear about what this costs. By the time the watchdog runs, its FIRST act — signing both
// controllers out — is what we are also preventing, so this is not purely cosmetic: it keeps the
// LiveUser objects intact rather than letting them be torn down and then fataling about it. But the
// underlying condition is untouched. The client still has no Battle.net session; the watchdog will
// keep being reached and keep being turned away every frame the predicates hold.
//
// Which is why the real lever is upstream and lives in the Pointers ctor: LiveFirstParty_Frame's
// entire body is gated on !Dvar_GetBool(nodw), so forcing nodw true before the frontend comes up means
// this never runs at all. This detour is the guarantee that nothing else can reach it; nodw is the fix.

template <>
char* Client::Hook::Hooks::HK_LiveUser_ForceSignOutAndFatal::hkCallback() {
	if (!Client::Game::OnlineMode::g_SuppressBnetErrors.load(std::memory_order_relaxed)) {
		return m_Original();
	}

	// Rate-limited to once. The predicates that lead here are evaluated every frame and do not clear
	// themselves, so an unconditional log would emit at frame rate. The first occurrence is the only
	// informative one — it says the watchdog was reached at all, i.e. nodw did not shut the frame
	// function down in time and the upstream fix is not working.
	static std::atomic_bool s_Reported{ false };
	if (!s_Reported.exchange(true, std::memory_order_relaxed)) {
		LOG("Bnet", WARN, "Blocked LiveUser_ForceSignOutAndFatal — the sign-in watchdog in "
			"LiveFirstParty_Frame that signs both controllers out and then calls Com_Error(1024). "
			"THIS is the BLZBNTBGS dialog; it never touches the error queue or either BnetError "
			"reporter, which is why the earlier five detours did nothing. Reaching it at all means "
			"nodw was not true in time — first-party polling should have been off. Logged once.");
	}
	return nullptr;
}
