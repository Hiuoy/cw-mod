#pragma once
// <game>/cw-mod/cw-mod.json: every cw-mod setting in one file, read once at boot.
//
// It replaces the marker files (online, no-scripts, no-progression, ...) and cw_mod_name.txt. A boot
// with no cw-mod.json writes one from whatever markers are there, so an existing setup carries over;
// from then on the markers are not read (the log names any that are still lying around).
//
// Most settings take effect at boot only: the frontends, the key patch and the hooks are all decided
// before the overlay exists.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Client::Game::Settings {
	// "mode": the network mode the frontends are built from (see Boot::Network).
	//   "offline"   networkMode 0
	//   "lan"       networkMode 1 + lobby LAN
	//   "lanlobby"  networkMode 2 + lobby LAN   (the online nibble alone)
	//   "online"    networkMode 2 + lobby LIVE
	enum class Mode { Offline, Lan, LanLobby, Online };

	struct Values {
		std::string name;              // "name": in-game name; empty = the Windows account name
		std::uint64_t xuid{};          // "xuid": this PC's player id on a backend boot; generated once
		Mode mode{ Mode::Offline };    // "mode"
		bool backend{ true };          // "backend": use cw-mod/dwserver/*_pub.der when they are there
		bool startScreen{ true };      // "start_screen": online + backend opens on the title screen
		bool scripts{ true };          // "scripts": GSC loader on at boot
		bool progression{ true };      // "progression": LAN/offline ZM progression
		bool liveMenus{ true };        // "live_menus": menus see the LAN lobby as LIVE
		bool localPlaylists{ true };   // "local_playlists": load cw-mod/lpc playlists
		bool lobbyWaiver{ true };      // "lobby_waiver": IsDemonwareFetchingDone waiver (backend)
		bool luaPrint{ true };         // "lua_print": lobby Lua print transcript in client.log
		bool fpsessionStandin{ false };// "fpsession_standin": diagnostic, zeroed first-party session
		bool uiTextLog{ false };       // "ui_text_log": list every UI string the game shows, once each
		bool customMaps{ true };       // "custom_maps": load mapkit maps from cw-mod/maps
		bool uiScripts{ true };        // "ui_scripts": run cw-mod/ui_scripts/*.lua in the menus
		bool mapkitUsage{ false };     // "mapkit_usage": record what a match looks up (game/mapkit_usage.hpp)
		bool unlockAll{ false };       // "unlock_all": every lock answers unlocked / owned (game/unlock_all.hpp)
		// "ui_text": { "text the game shows": "text to show instead" }. Whole strings, ASCII case ignored.
		std::vector<std::pair<std::string, std::string>> uiText;
		// "mapkit_trace": zone names (lowercase, "*" = every zone) whose load is recorded for mapkit's
		// offline tools (game/mapkit_trace.hpp). Empty = nothing is hooked.
		std::vector<std::string> mapkitTrace;
	};

	// Loaded on the first call. Never fails: a missing key keeps its default, and a file that does not
	// parse leaves every key at its default (LoadNote() says so, and so does the log).
	const Values& Get();

	// The name the game is given: the CW_MOD_NAME environment variable (per process, for two copies on
	// one PC), else "name", else the Windows account. At most 32 bytes, the engine's name field.
	// Two PCs on the same Windows account name would otherwise present the SAME identity to each
	// other, which on the wire is indistinguishable from a self-join.
	const std::string& PlayerName();

	// The player id our auth server puts in the login ticket, i.e. the XUID every other PC sees: the
	// CW_MOD_XUID environment variable (hex, per process), else "xuid". A PC without one gets a random id
	// written into cw-mod.json on its first boot, so it is stable from then on. Two PCs with the same id
	// (the backend used to hand out 1 to everyone) look like one player joining itself.
	std::uint64_t PlayerXuid();

	// What happened at load, for the overlay: "loaded", "created from marker files", or the error.
	const std::string& LoadNote();

	std::filesystem::path Path();
	const char* ModeName(Mode mode);
}
