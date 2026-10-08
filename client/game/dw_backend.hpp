#pragma once
// Local Demonware backend — client side.
//
// The pivot (memory cw-mod-demonware-backend): rather than defeat each offline predicate,
// stand up a local server the client authenticates against, so Dw_GetConnectionState reports
// connected and the existing progression code paths run. The client half of that is small:
// replace the two RSA-2048 public keys baked into the image with our own, so replies signed by
// our local server's private keys verify. Everything else lives in the Python server under
// tools/dwserver/.
//
// This is deliberately a standalone concern (no new Pointers members): it is pure address math
// + a guarded in-memory byte replace, driven once from the Pointers ctor.

#include <cstdint>
#include <string>

namespace Client::Game::DwBackend {
	// False until PatchEmbeddedKeys() successfully installs our auth key. Opt-in is the presence of
	// <game>/cw-mod/dwserver/auth_pub.der (+ lsg_pub.der): drop the public keys there once the local
	// server + hosts redirect + CA are in place. With no key files, PatchEmbeddedKeys() no-ops and
	// this stays false, so the startup path keeps the normal offline behaviour untouched. Read by
	// main.cpp to decide whether to leave the DW connection active (skip com_noDW).
	extern bool g_Enabled;

	// Result of the patch attempt, for logging / the overlay probe.
	struct PatchStatus {
		bool authReplaced{};
		bool lsgReplaced{};
		bool authKeyFileFound{};
		bool lsgKeyFileFound{};
		std::string detail{};
	};

	// Overwrite the client's embedded auth + LSG public keys with the ones in cw-mod/dwserver/.
	// Safe to call once from the ctor; guarded so a build mismatch or missing files leaves the
	// image untouched. Returns what actually happened.
	PatchStatus PatchEmbeddedKeys(std::uintptr_t moduleBase);

	// Last status, for the overlay to display.
	const PatchStatus& LastStatus();
}
