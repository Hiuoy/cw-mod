#include "common.hpp"
#include "game/boot_profile.hpp"
#include "game/game.hpp"
#include "game/dw_backend.hpp"
#include "game/settings.hpp"

namespace Client::Game::Boot {
	namespace {
		Profile s_Profile{};

		void ReadMode(Profile& profile) {
			switch (Settings::Get().mode) {
			case Settings::Mode::Offline:
				return;
			case Settings::Mode::Lan:
				profile.network = Network::Lan;
				profile.lobbyLive = false;
				break;
			case Settings::Mode::LanLobby:
				profile.network = Network::Online;
				profile.lobbyLive = false;
				break;
			case Settings::Mode::Online:
				profile.network = Network::Online;
				profile.lobbyLive = true;
				break;
			}
			profile.modeSet = true;
		}
	}

	std::string Profile::Describe() const {
		const char* net = network == Network::Online ? (lobbyLive ? "online (lobby LIVE)" : "online (lobby LAN)")
			: network == Network::Lan ? "lan" : "offline";
		return std::format("{}{}{}{}", net, backend ? " + local backend" : "", startScreen ? " + title screen" : "",
			modeSet ? "" : " (cw-mod.json mode \"offline\")");
	}

	void Resolve() {
		Profile profile{};
		ReadMode(profile);
		profile.backend = DwBackend::g_Enabled;
		if (profile.IsOnline() && profile.lobbyLive && profile.backend) {
			profile.startScreen = Settings::Get().startScreen;
		}
		s_Profile = profile;
		LOG("Boot", INFO, "profile: {}", s_Profile.Describe());
	}

	const Profile& Current() {
		return s_Profile;
	}

	void ApplyEarly(Pointers& p) {
		const Profile& profile = s_Profile;
		if (!profile.modeSet) {
			p.SetMode(0, 10);
			return;
		}

		// Jumping to director_lan (10) skips the title screen, and with it the party: the only thing
		// that creates one is the director -> select-menu step that pressing start runs.
		p.SetMode(profile.NetworkMode(), profile.startScreen ? -1 : 10, profile.lobbyLive ? 1 : 0);
		if (profile.startScreen) {
			LOG("Boot", INFO, "title screen kept (no jump to director_lan): press start to run BeginLivePlay, "
				"which creates the party. \"start_screen\": false in cw-mod.json restores the jump.");
		}
		OnlineMode::g_Requested.store(profile.IsOnline(), std::memory_order_relaxed);
		OnlineMode::g_SuppressBnetErrors.store(profile.IsOnline(), std::memory_order_relaxed);
		if (!profile.IsOnline()) {
			return;
		}

		// The BLZBNTBGS wall comes from LiveUser_ForceSignOutAndFatal, a sign-in watchdog inside
		// LiveFirstParty_Frame, and that whole frame is gated on !nodw. Setting nodw here lands before
		// the frontend is built; doing it after Scr_Initialized measured ~6 seconds too late. The
		// session network mode is left at 2, so the online frontends are still built.
		if (p.m_Dvar_SetBoolFromSource && p.m_Dvar_NoDW && *p.m_Dvar_NoDW) {
			p.WriteDvarBool(*p.m_Dvar_NoDW, true);
			LOG("Boot", INFO, "nodw = true early, so the BLZBNTBGS sign-in watchdog never ticks.");
		}
		else {
			// Not fatal: the LiveUser_ForceSignOutAndFatal detour still catches the watchdog.
			LOG("Boot", WARN, "could not set nodw this early (dvar not registered yet?); relying on "
				"the LiveUser_ForceSignOutAndFatal detour instead.");
		}
	}

	void ApplyAfterScriptInit(Pointers& p) {
		const Profile& profile = s_Profile;
		if (!p.m_Dvar_NoDW || !*p.m_Dvar_NoDW) {
			LOG("Boot", ERROR, "nodw dvar unresolved; cannot finish the boot sequence for {}.", profile.Describe());
			return;
		}

		if (profile.IsOnline()) {
			// No CL_Disconnect: it would tear down the session state this boot exists for.
			//
			// The login driver reads the same nodw that ApplyEarly set (state 1 is
			// `if (Dvar_GetBool(nodw)) return;`), so leaving it true means an online boot never dials
			// our server. The frontend is built by now, so the watchdog's window has passed.
			if (profile.backend) {
				p.WriteDvarBool(*p.m_Dvar_NoDW, false);
				LOG("Boot", INFO, "online + backend: nodw = false now that the frontend is built, so login "
					"dials 127.0.0.1. If BLZBNTBGS returns, this is what re-armed the watchdog.");
			}
			else {
				LOG("Boot", INFO, "online without backend: nodw stays true, no disconnect.");
			}
			return;
		}

		if (!profile.backend) {
			p.WriteDvarBool(*p.m_Dvar_NoDW, true);
			p.m_CL_Disconnect(0, false, "");
			LOG("Boot", DEBUG, "{}: forced offline (nodw = true + CL_Disconnect), game version {}",
				profile.Describe(), g_GameIdentifier.m_Version);
			return;
		}

		// On this build nodw defaults to true, so without this the login driver never opens the socket.
		p.WriteDvarBool(*p.m_Dvar_NoDW, false);
		LOG("Boot", INFO, "{}: nodw = false, login dials the local backend.", profile.Describe());
	}
}
