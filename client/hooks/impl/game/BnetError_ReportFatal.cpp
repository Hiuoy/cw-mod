#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"

// The wall an online-session-mode boot hits, and why it cannot be clicked away.
//
// With networkMode == 2 the client brings the Battle.net/BGS layer up for real. Nothing answers, so
// BGS produces an error code, which is formatted as "BLZBNTBGS%08X" and handed to Com_Error
// (0x7FF7222DA030) at level 1024. Level 1024 is not a soft error class: it falls through Com_Error's
// dispatch to `level |= 2` and then into the full shutdown chain, so the dialog is modal-until-exit
// by construction. There is no dvar and no dismiss path — the only place to intervene is before the
// Com_Error call, i.e. in the reporters themselves.
//
// Both reporters are reached exclusively through callback slots in a table (0x7FF7377338AC /
// 0x7FF7377338B8), never a direct call, so there is no call site to gate and no useful return-address
// discrimination to do (the caller is the BGS SDK either way). We therefore detour the functions
// wholesale — but only ever install them when the boot marker asked for online mode, so a normal
// launch keeps the stock fatal behaviour for a genuine Battle.net failure.
//
// Suppressing a fatal is deliberately a testing lever, not a fix: the client really has no Battle.net
// session, so whatever wanted one is still going to be broken. What this buys is that the frontend
// gets built and stays on screen long enough to answer the actual question — which menus does an
// online-mode boot produce — instead of the process dying before we can look.

namespace {
	// The BGS code is offset by 1000000 for the "service" range, exactly as BnetError_FormatMessage
	// does before printing it, so the number we log matches the BLZBNTBGS........ the dialog would
	// have shown. Keeps the log directly comparable to what the user saw.
	std::uint32_t BgsDisplayCode(std::uint32_t raw) {
		return raw >= 1000000u ? raw - 1000000u : raw;
	}
}

template <>
std::uint64_t Client::Hook::Hooks::HK_BnetError_ReportFatalUnguarded::hkCallback(std::uint32_t bgsErrorCode) {
	if (!Client::Game::OnlineMode::g_SuppressBnetErrors.load(std::memory_order_relaxed)) {
		return m_Original(bgsErrorCode);
	}
	LOG("Bnet", WARN, "Suppressed FATAL Battle.net error BLZBNTBGS{:08X} (raw {}) — unguarded reporter. "
		"Online-mode test boot: the client has no Battle.net session, so anything downstream of it is "
		"still unavailable; this only keeps the process alive.",
		BgsDisplayCode(bgsErrorCode), bgsErrorCode);
	return 0;
}

template <>
void Client::Hook::Hooks::HK_BnetError_ReportFatalIfSignedIn::hkCallback(std::uintptr_t ctx, std::uint32_t* bgsErrorCode) {
	if (!Client::Game::OnlineMode::g_SuppressBnetErrors.load(std::memory_order_relaxed)) {
		m_Original(ctx, bgsErrorCode);
		return;
	}

	// The original reads *bgsErrorCode and does nothing at all when it is 0, so mirror that check
	// rather than logging a line for every benign tick. The pointer comes from the BGS SDK, so read
	// it guarded — a null/garbage one must cost a log line, not the process.
	std::uint32_t code = 0;
	if (!bgsErrorCode || !Client::Game::SafeRead(bgsErrorCode, code) || !code) {
		return;
	}

	LOG("Bnet", WARN, "Suppressed FATAL Battle.net error BLZBNTBGS{:08X} (raw {}) — latched reporter "
		"(g_bnetEverSignedInLatch was set, i.e. the client believes it LOST a session it once had).",
		BgsDisplayCode(code), code);
}
