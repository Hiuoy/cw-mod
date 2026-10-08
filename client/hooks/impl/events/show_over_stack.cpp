#include "common.hpp"
#include "game/game.hpp"
#include "game/local_lpc.hpp"
#include "game/mapkit_loader.hpp"
#include "game/ui_scripts.hpp"
#include "game/unlock_all.hpp"
#include "game/zm_progression.hpp"
#include "hooks/hook.hpp"
#include "overlay/menu.hpp"

void Client::Hook::Hooks::OnShowOverStack() {
	// do stuff
	g_Pointers->PatchAuth();

	// Run any menu-scheduled actions here, on the game thread (safe to call game functions).
	Client::Overlay::Menu::DrainActions();

	// Re-assert online session mode if it was requested AND pinning is on. No-op otherwise (one
	// atomic load), and no-op even when on unless the engine has reset the nibble off 2.
	Client::Game::OnlineMode::Tick();

	// Read-only: log the marketplace inventory gates when they change (online boots only).
	Client::Game::EntitlementGates::Tick();

	// LAN browser: refresh our advert from the live session objects once a second (read-only).
	Client::Game::LanBrowser::Tick();

	// LAN/offline LPC playlists: stage, add the search path, load slot 0 (once per boot).
	Client::Game::LocalLpc::Tick();

	// Custom maps: add the cw-mod/maps folders as zone search paths (once per boot).
	Client::Game::MapKit::Tick();

	// LIVE menus: keep the menus' lobby network mode at LIVE (our boot set LAN before the detour existed).
	Client::Game::ZmProgression::TickMenuNetworkMode();

	// Online boots: fill the engine's own level block from the local save as soon as a sign-in makes it.
	Client::Game::ZmProgression::TickEngineAe();

	// "unlock_all": fill the battle pass model when it is due, and write the log's summary line.
	Client::Game::UnlockAll::Tick();

	// cw-mod/ui_scripts: re-run a script whose file changed (checks once a second); feed the SERVER BROWSER
	// menu and run its join while it is open.
	Client::Game::UiScripts::Tick();

	static std::string watermarkText = std::format("t9-mod: " GIT_DESCRIBE " [v{}]", g_GameIdentifier.m_Version);

	T9::Font_s* font = *g_Pointers->m_unk_WatermarkFont;
	if (font) {
		float color[4] = { .666f, .666f, .666f, .666f };
		g_Pointers->m_CL_DrawTextPhysical(watermarkText.c_str(), 0x7FFFFFFF, font,
			4.f, 4.f + static_cast<float>(font->m_FontHeight), 1.f, 1.f, 0.f, color, 0, 0);
	}
}
