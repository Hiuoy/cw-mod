#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/dw_backend.hpp"
#include "game/dump_anchors.hpp"
#include <utility/nt.hpp>

#include <intrin.h>
#include <mutex>

// LiveUser_FirstPartyPresenceOk (0x7FF7296B79B0) asks "is a Battle.net first-party user signed in and
// present?". Studio auth never creates one, so the honest answer is always false on our boots. Two
// different consumers need two different lies, and one must get the truth:
//
// 1. THE LOGIN DRIVER (state 1). Without true it parks forever and never dials Demonware. Answered
//    true unconditionally, as before (return-address window of LiveUser_LoginDriver_Tick).
//
// 2. EVERYTHING GATED ON "ONLINE READY", once Demonware login has completed. B1 (2026-09-16) found
//    that the silence after [status 27] is not a sign-out: signinState reaches 2 and loginState 4,
//    but LiveUser_IsOnlineReady (0x7FF7262ACD00) and LiveUser_PresenceOkAndSignedInOnline
//    (0x7FF7274B3540) also demand presence, and they gate every per-user online subsystem tick in
//    LiveUser_UpdateSigninState plus ~60 other sites. Those are what would issue the post-login
//    lobby requests. No server reply can satisfy a Battle.net check, so it is answered here: true,
//    but ONLY after DW login is complete (controller 0: loginState 4 AND signinState 2), which is
//    exactly the state a retail client is in when presence is true.
//
// 3. LiveUser_OnSigninStateChange_SetupIdentity gets the TRUTH. With presence true it reads the
//    gamertag through the first-party manager, which on studio auth was never signed in.
//    So do the two account-id pickers (kDump_LiveUser_AccountIdPickers): with presence true they ask
//    the Battle.net account object, which is null, and the match launch crashed on it (22:58).
//
// Two further guards:
// - Only while OnlineMode::g_SuppressBnetErrors is on. A true answer makes the BLZBNTBGS watchdog
//   branch of LiveFirstParty_Frame reachable (it asks via LiveUser_PresenceOkAndSignedInOnline, so a
//   return-address check cannot exclude it); on those boots the LiveUser_ForceSignOutAndFatal detour
//   neutralises it and logs once. Without that detour armed, we do not widen.
// - Each call site that gets an overridden answer is logged ONCE, as it happens (the census method
//   from the first-party session crash). If a newly reachable path faults, the last site logged
//   before the crash names it. Static xrefs say 30 callers; what actually runs is the number to trust.

namespace {
	using namespace Client;

	constexpr std::size_t kMaxSites = 64;

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

	std::uintptr_t ToRva(std::uintptr_t addr) {
		const std::uintptr_t base = GameBase();
		return (addr >= base) ? (addr - base) : addr;
	}

	bool InWindow(std::uintptr_t ra, std::uintptr_t base, std::size_t size) {
		return base && ra >= base && ra < base + size;
	}

	// Controller 0 only: this PC has one local user, and presence itself takes no controller.
	bool DwLoginComplete() {
		if (!g_Pointers->m_g_liveUserObjects) {
			return false;
		}
		const auto userObj = g_Pointers->m_g_liveUserObjects[0];
		if (!userObj) {
			return false;
		}
		const auto signin = *reinterpret_cast<const std::int32_t*>(userObj + Game::kLiveUser_SigninStateOffset);
		const auto login = *reinterpret_cast<const std::int32_t*>(userObj + Game::kLiveUser_LoginStateOffset);
		return signin == Game::kLiveUser_SigninState_Online && login == Game::kLiveUser_LoginState_Connected;
	}

	// Logged on first sighting only; presence is polled every frame by several callers.
	void RecordSite(std::uintptr_t ra, const char* verdict) {
		std::lock_guard<std::mutex> lock(g_Lock);
		for (std::size_t i = 0; i < g_SiteCount; ++i) {
			if (g_Sites[i] == ra) {
				return;
			}
		}
		if (g_SiteCount >= kMaxSites) {
			return;
		}
		g_Sites[g_SiteCount++] = ra;
		LOG("Presence", INFO, "first-party presence asked from {}+0x{:X} (site #{}): {}",
			Client::g_GameModuleName, ToRva(ra), g_SiteCount, verdict);
	}
}

template <>
bool Client::Hook::Hooks::HK_LiveUser_FirstPartyPresenceOk::hkCallback() {
	if (!Client::Game::DwBackend::g_Enabled || !g_Pointers) {
		return m_Original();
	}

	const std::uintptr_t ra = reinterpret_cast<std::uintptr_t>(_ReturnAddress());

	if (InWindow(ra, g_Pointers->m_LiveUser_LoginDriver_TickBase, Client::Game::kLiveUser_LoginDriver_TickSize)) {
		return true;
	}

	const bool original = m_Original();
	if (original) {
		// A real first-party sign-in exists; nothing to override.
		return true;
	}

	if (!Client::Game::OnlineMode::g_SuppressBnetErrors.load(std::memory_order_relaxed) || !DwLoginComplete()) {
		return false;
	}

	if (InWindow(ra, g_Pointers->m_LiveUser_SetupIdentityBase, Client::Game::kLiveUser_SetupIdentitySize)) {
		RecordSite(ra, "DENIED — identity setup would read the gamertag from the unsigned first-party manager; "
			"returning the real false");
		return false;
	}

	if (InWindow(ra, g_Pointers->m_LiveUser_AccountIdPickersBase, Client::Game::kLiveUser_AccountIdPickersSize)) {
		RecordSite(ra, "DENIED — account-id picker would call GetAccount on the null Battle.net account "
			"(the 22:58 match-launch crash); returning the real false, so it takes the Demonware id");
		return false;
	}

	RecordSite(ra, "answering TRUE (DW login complete: loginState 4, signinState 2; real answer false)");
	return true;
}
