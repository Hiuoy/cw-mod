#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/dw_net.hpp"

// The backstop, and the only hook here that can see traffic the resolver never named.
//
// Two things reach the network without a hostname: the STUN/P2P layer, which is handed raw addresses
// out of matchmaking, and anything that cached a resolve from before our switches were set. Neither
// is covered by getaddrinfo, so without this the "the client cannot dial retail" claim would be a
// claim about name resolution rather than about connections — which is a weaker statement than the
// guardrail needs.
//
// Journalling is the primary purpose. Discovering WHICH endpoints an online boot actually reaches
// for is the shopping list the local backend gets built from, and an address the game connects to
// without ever resolving is exactly the kind of thing that would otherwise go unnoticed.

namespace {
	// Loopback, RFC1918 private space, and link-local. Traffic to these is either ours or the LAN's,
	// and LAN play has to keep working (see the offline/LAN constraint) — so these are never blocked
	// and never treated as "phoning home".
	bool IsLocalOrPrivate(std::uint32_t hostOrder) {
		const std::uint8_t a = static_cast<std::uint8_t>(hostOrder >> 24);
		const std::uint8_t b = static_cast<std::uint8_t>(hostOrder >> 16);
		if (a == 127) return true;                          // 127.0.0.0/8
		if (a == 10) return true;                           // 10.0.0.0/8
		if (a == 192 && b == 168) return true;              // 192.168.0.0/16
		if (a == 172 && b >= 16 && b <= 31) return true;    // 172.16.0.0/12
		if (a == 169 && b == 254) return true;              // 169.254.0.0/16
		if (a == 0 || a >= 224) return true;                // unspecified / multicast / broadcast
		return false;
	}
}

template <>
int __stdcall Client::Hook::Hooks::HK_connect::hkCallback(SOCKET s, const sockaddr* name, int namelen) {
	// Only IPv4 is inspected. T9's Demonware transport is IPv4 throughout (the virtual-address
	// netadr it uses internally is a 4-byte address plus a port), and an unrecognised family is
	// passed through rather than guessed at.
	if (!name || name->sa_family != AF_INET || namelen < static_cast<int>(sizeof(sockaddr_in))) {
		return m_Original(s, name, namelen);
	}

	const auto* const in = reinterpret_cast<const sockaddr_in*>(name);
	const std::uint32_t addr = ntohl(in->sin_addr.s_addr);
	const std::uint16_t port = ntohs(in->sin_port);

	if (IsLocalOrPrivate(addr)) {
		Client::Game::DwNet::NoteConnect(addr, port, "local/LAN — allowed");
		return m_Original(s, name, namelen);
	}

	if (Client::Game::DwNet::g_Block.load(std::memory_order_relaxed)) {
		// Refused the way a firewalled host refuses, so the client's own retry/timeout logic runs
		// instead of an error it has no branch for.
		Client::Game::DwNet::NoteConnect(addr, port, "BLOCKED (routable address on a guarded boot)");
		WSASetLastError(WSAECONNREFUSED);
		return SOCKET_ERROR;
	}

	Client::Game::DwNet::NoteConnect(addr, port, "routable — allowed (blocking is off)");
	return m_Original(s, name, namelen);
}
