#pragma once
// What kind of boot this is, decided once.
//
// Two inputs, both under <game>/cw-mod/, both read in the Pointers ctor:
//   cw-mod.json "mode"        the network mode the frontends are BUILT from (see Network below)
//   dwserver/auth_pub.der     the local Demonware backend (DwBackend::PatchEmbeddedKeys)
//
// The frontends are built from the session network mode long before the overlay exists, so the mode
// cannot be a button; it has to be set in the ctor. Everything that used to re-read the mode, or
// branch on the two inputs separately, asks Current() instead.

#include <string>

namespace Client::Game {
	class Pointers;
}

namespace Client::Game::Boot {
	// cw-mod.json "mode" picks the variant. The session nibble and the lobby enum are two separate
	// engine settings:
	//   "offline"   Offline   networkMode 0
	//   "lan"       Lan       networkMode 1 + lobby LAN
	//   "lanlobby"  Online    networkMode 2 + lobby LAN   (the online nibble alone)
	//   "online"    Online    networkMode 2 + lobby LIVE
	enum class Network { Offline, Lan, Online };

	struct Profile {
		Network network{ Network::Offline };
		bool lobbyLive{ false };
		bool modeSet{ false };  // any mode but "offline"
		bool backend{ false };  // our auth key is patched in: dial 127.0.0.1 instead of forcing nodw
		// Online + lobby LIVE + backend: open on the title screen instead of jumping to director_lan, so
		// pressing start runs the game's own BeginLivePlay -> CreatePrivateLobby and makes the party.
		// "start_screen": false in cw-mod.json restores the jump.
		bool startScreen{ false };

		bool IsOnline() const { return network == Network::Online; }
		int NetworkMode() const { return static_cast<int>(network); }
		std::string Describe() const;
	};

	// Reads the mode and snapshots DwBackend::g_Enabled. Call once from the Pointers ctor, AFTER
	// PatchEmbeddedKeys. Before that, Current() is the default offline profile.
	void Resolve();
	const Profile& Current();

	// Pointers ctor, before the frontend is built: session/lobby mode, the Battle.net suppression
	// flags, and nodw=true early on an online boot.
	void ApplyEarly(Pointers& pointers);

	// Once Scr_Initialized is set: the second half of the nodw sequence.
	void ApplyAfterScriptInit(Pointers& pointers);
}
