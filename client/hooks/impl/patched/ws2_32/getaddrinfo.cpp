#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/dw_net.hpp"

#include <string>

// The modern resolver, and the one that matters: it is what libcurl and WinHTTP both go through, so
// every HTTPS call to auth3 / loginqueue / umbrella / uno / objectstore passes here.
//
// The redirect is done by RESOLVING A DIFFERENT NAME rather than by synthesizing an addrinfo chain.
// That detail is worth the sentence: the caller owns the result and will hand it back to
// freeaddrinfo, so a chain we allocated ourselves would have to be recognised and freed by a matching
// freeaddrinfo detour, and getting that wrong is a heap corruption that surfaces somewhere else
// entirely. Asking the real getaddrinfo for "127.0.0.1" yields a chain the system allocated, with the
// hints, family, socktype and port the caller asked for, which the stock freeaddrinfo then frees
// correctly with no hook at all.
template <>
int __stdcall Client::Hook::Hooks::HK_getaddrinfo::hkCallback(
	const char* nodeName, const char* serviceName, const addrinfo* hints, addrinfo** result) {

	if (!Client::Game::DwNet::IsDemonwareHost(nodeName)) {
		return m_Original(nodeName, serviceName, hints, result);
	}

	if (Client::Game::DwNet::g_Redirect.load(std::memory_order_relaxed)) {
		Client::Game::DwNet::NoteResolve(nodeName, "-> 127.0.0.1 (local Demonware backend)");
		return m_Original("127.0.0.1", serviceName, hints, result);
	}

	if (Client::Game::DwNet::g_Block.load(std::memory_order_relaxed)) {
		// Fail the way an unknown name fails, so the client takes its own "cannot reach Demonware"
		// path rather than an unfamiliar one. WSAHOST_NOT_FOUND is what the resolver returns for a
		// name that does not exist, which is exactly the fiction we want it to believe.
		Client::Game::DwNet::NoteResolve(nodeName, "BLOCKED (no local server; retail is not a target)");
		return WSAHOST_NOT_FOUND;
	}

	// Neither switch set. Pass through, but say so: an unlogged pass-through is indistinguishable
	// from a hook that never ran, and this project has already lost time to that once.
	Client::Game::DwNet::NoteResolve(nodeName, "passed through to the real resolver");
	return m_Original(nodeName, serviceName, hints, result);
}
