#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/dump_anchors.hpp"

#include <atomic>

// This PC's own bdCommonAddr, the address every other PC connects to (see dump_anchors.hpp,
// kDump_bdCommonAddr_Ctor). On a LIVE boot the net layer fills its public address and NAT type from STUN
// discovery; with no Demonware NAT traversal or relay behind our backend, two PCs then either cannot
// connect or, with matching public addresses, each take the other for itself. The 23:20 browser join
// sent JoinLobby 3 ms after picking the host and never heard back.
//
// LAN builds the same object with an empty public address and NAT type 1 (open), and that shape is what
// two PCs have joined over since 2026-07-23. So on a backend boot every construction gets LAN's two
// values; the local addresses (the ones peers actually reach) are untouched. Only bdNetImpl calls this
// constructor (start, and the two pump paths), so nothing else is affected. Installed on DwBackend
// boots only; each replacement is logged with what it replaced.
namespace {
	// A bdAddr starts with a Windows sockaddr_storage: u16 family, u16 port (network order), then the
	// address. Family 0 is the empty address.
	bool ReadHead(const void* addr, std::uint64_t& head) {
		return addr && Client::Game::SafeRead(addr, head);
	}

	std::string DescribeAddr(std::uint64_t head) {
		const auto byte = [head](int i) { return static_cast<unsigned>((head >> (8 * i)) & 0xFF); };
		const unsigned family = byte(0) | (byte(1) << 8);
		const unsigned port = (byte(2) << 8) | byte(3);
		if (family == 0) return "empty";
		if (family == 2) return std::format("{}.{}.{}.{}:{}", byte(4), byte(5), byte(6), byte(7), port);
		return std::format("family {} port {}", family, port);
	}
}

template <>
void* Client::Hook::Hooks::HK_bdCommonAddr_Ctor::hkCallback(void* self, void* localAddrs, const void* publicAddr,
	std::uint32_t natType, std::int32_t extra) {
	const void* empty = g_Pointers ? g_Pointers->m_g_bdAddrEmpty : nullptr;
	std::uint64_t head = 0;
	const bool readable = ReadHead(publicAddr, head);
	// Already the LAN shape (the LAN start builds exactly this), or nothing to substitute with.
	if (!empty || (readable && (head & 0xFFFF) == 0 && natType == Client::Game::kBdNatType_Open)) {
		return m_Original(self, localAddrs, publicAddr, natType, extra);
	}

	static std::atomic_int s_Calls{ 0 };
	const int n = ++s_Calls;
	if (n <= 10) {
		LOG("PeerAddr", INFO, "own peer address #{}: public {} NAT type {} -> empty, NAT 1 (open), the LAN shape, so "
			"other PCs reach this one on its local addresses", n, readable ? DescribeAddr(head) : "<unreadable>",
			natType);
	}
	return m_Original(self, localAddrs, empty, Client::Game::kBdNatType_Open, extra);
}
