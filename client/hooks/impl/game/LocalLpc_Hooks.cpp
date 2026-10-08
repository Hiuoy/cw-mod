#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/local_lpc.hpp"

// The one detour behind LAN/offline LPC playlists (game/local_lpc.hpp). A pass-through except while
// our playlists zones load.
//
// The original is called directly, not through an ArxanCall thunk: DB_AllocXAssetEntry is 49
// instructions with no caller check (IDB 2026-09-23), and it runs for every asset of every zone, so
// the extra hop would be pure cost. Its first bytes (mov [rsp+8], rbx) contain no call, so the
// callees it reaches still see an in-image return address.
template <>
void* Client::Hook::Hooks::HK_DB_AllocXAssetEntry::hkCallback(std::uint64_t type, std::uint64_t zone) {
	if (Game::LocalLpc::LoadInFlight()) Game::LocalLpc::BeforeAssetAlloc(static_cast<std::uint8_t>(type));
	return m_Original(type, zone);
}
