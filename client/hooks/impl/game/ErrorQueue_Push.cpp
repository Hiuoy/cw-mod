#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/dump_anchors.hpp"

// The detour that actually removes the BLZBNTBGS wall, after two that did not.
//
// What was measured, in order:
//   1. Hooked Com_Error's two Battle.net reporters. Popup unchanged, and it re-raised on every menu
//      change — so it was never a Com_Error dialog to begin with.
//   2. Hooked the two writers of first-party STATE_ERROR, on the theory that LUI was querying stored
//      error state. The log then showed the LATCHED reporter suppressed, the two state writers never
//      firing at all, and the popup still on screen. That combination rules out both theories.
//
// The dialog is a LUI popup driven by an error QUEUE, and it has three independent producers:
//
//   BnetError_ReportFatal_IfEverSignedIn      (an error path — the one we had already caught)
//   BnetError_ReportFatal_Unguarded           (an error path)
//   LiveUser_HandleSignOut  0x7FF729752990    (NOT an error path — a sign-out path)
//
// The third is the one that got us. It tears the LiveUser object down, asks
// LiveUser_BuildSignOutErrorMessage (0x7FF729753600) for a user-facing reason, and pushes whatever it
// gets at level 1024. It never goes near FirstParty_SetError, which is exactly why hooking the state
// writers changed nothing.
//
// And that message builder is the whole online/offline asymmetry:
//
//     if (signinState == 1 || signinState == 2 && !Com_SessionMode_IsOnline()) return 0;   // silent
//
// Offline, a sign-out with signinState 2 yields no message and the sign-out is silent — which is why
// nobody has ever seen this dialog in the mod before. Online, the identical sign-out falls through,
// produces a message, and becomes an undismissable fatal. Flipping networkMode to 2 is literally what
// converts a routine silent sign-out into the wall.
//
// So we gate the QUEUE rather than chase producers. Every one of them ends here, and every one of them
// treats a 0 return as "then you display it", falling through to Com_Error(level, ...) or to
// ErrorQueue_StorePending — so we must return 1 (claiming we queued it), not 0.
//
// Only level 1024 (the Battle.net/BGS class) is dropped. Every other level — including the level 16
// that the same sign-out path uses for its non-BGS variant — is passed through untouched, because a
// real engine error still needs to reach the user. As always this is a testing lever, not a repair:
// the client genuinely has no Battle.net session, so whatever wanted one is still broken. What it buys
// is a frontend that stays on screen long enough to answer the actual question.

template <>
char Client::Hook::Hooks::HK_ErrorQueue_Push::hkCallback(int level, const char* message, char flag) {
	const bool suppress = level == Client::Game::kErrorLevel_BattleNet
		&& Client::Game::OnlineMode::g_SuppressBnetErrors.load(std::memory_order_relaxed);

	// The message is engine-owned and already formatted, so log it verbatim rather than re-deriving
	// anything. Read guarded: the pointer comes from a producer we do not control, and a bad one must
	// cost a log line rather than the process.
	// Copied a byte at a time through SafeRead rather than with strnlen: the queue entry is 1024 bytes
	// of text, so a producer that hands us an unterminated or freed buffer would otherwise walk off
	// the end inside our own log call.
	std::string text;
	for (std::size_t i = 0; message && i < 1024; ++i) {
		char c = 0;
		if (!Client::Game::SafeRead(message + i, c) || !c) {
			break;
		}
		text.push_back(c);
	}
	if (text.empty()) {
		text = "<empty or unreadable>";
	}

	// Log EVERY level, not just the one we drop. Suppressing 1024 and logging only 1024 is how the
	// last three rounds each "proved" the wrong thing: a hook that is silent tells you nothing about
	// whether it ran. If a dialog appears while this stays quiet, that is now positive evidence the
	// queue is not involved and the producer calls Com_Error directly — which is exactly the
	// distinction that took four attempts to make. Not de-duplicated: the ring is 4 slots deep and
	// each push is a distinct event, so volume here is itself information.
	LOG("Bnet", WARN, "ErrorQueue_Push(level={}, flag={}) msg=\"{}\" -> {}", level,
		static_cast<int>(flag), text,
		suppress ? "DROPPED (returning 'queued' so no caller falls through to Com_Error)"
		         : "passed through to the engine");

	if (!suppress) {
		return m_Original(level, message, flag);
	}
	return 1;
}
