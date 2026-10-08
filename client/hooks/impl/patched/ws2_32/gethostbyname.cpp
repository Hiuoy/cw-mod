#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/dw_net.hpp"

// The legacy resolver. T9 still imports it, and Demonware's own socket layer is old enough to use
// it, so leaving it unhooked would be a hole straight through the choke point.
//
// Same trick as getaddrinfo: redirect by resolving a different name rather than by building a
// hostent. gethostbyname returns a pointer into thread-local storage owned by WS2_32, so handing back
// a structure of our own would be a lifetime problem for whichever caller cached it.
template <>
hostent* __stdcall Client::Hook::Hooks::HK_gethostbyname::hkCallback(const char* name) {
	if (!Client::Game::DwNet::IsDemonwareHost(name)) {
		return m_Original(name);
	}

	if (Client::Game::DwNet::g_Redirect.load(std::memory_order_relaxed)) {
		Client::Game::DwNet::NoteResolve(name, "-> 127.0.0.1 (local Demonware backend)");
		return m_Original("127.0.0.1");
	}

	if (Client::Game::DwNet::g_Block.load(std::memory_order_relaxed)) {
		Client::Game::DwNet::NoteResolve(name, "BLOCKED (no local server; retail is not a target)");
		WSASetLastError(WSAHOST_NOT_FOUND);
		return nullptr;
	}

	Client::Game::DwNet::NoteResolve(name, "passed through to the real resolver");
	return m_Original(name);
}
