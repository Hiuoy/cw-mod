#pragma once
// LPC playlists without Demonware's objectstore: load the retail core_playlists zones on an offline/LAN
// boot, or a LIVE boot on our local backend, so the game has the playlist (the mode list the ZM main
// screen builds its tiles from) that it otherwise only gets from a real Demonware LPC list.
//
// LIVE + backend (2026-09-24): the backend serves an empty LPC list and session.cpp's B3 bypass marks
// both content slots loaded, so the online director had no modes or maps. Nothing in the engine frees
// the playlists zone (flag 0x2000 is in no DB_LoadXAssets free mask) and only Playlist_LoadFromAssets
// writes g_playlistValid, so loading it before or after the loader's one-time slot reset both hold.
//
// Why it is missing (IDB-verified 2026-09-23, see dump_anchors.hpp): the playlist asset is loaded only
// by the LPC content loader's slot 0 callback (Playlist_LoadFromAssets). That loader
// (OnlineContent_LoaderFrame) needs PubVars from Demonware and refuses to run in the offline/LAN
// frontend, and the zone file is only reachable once the engine has MD5-verified its LPC manifest and
// added the LPC folder as a search path, which needs Demonware too.
//
// What this does, once per boot, from the game-thread tick:
//   1. Mount: pick <game>/cw-mod/lpc/[<lang>_]core_playlists_tu<N>_100_<buildId>.ff with this exe's
//      build id and FS_AddSearchPath(cw-mod/lpc, 300, 2, 0), the same call the engine makes for its
//      verified LPC folder.
//   2. Slot 0 the way OnlineContent_LoadSlotZones runs it (DB_LoadXAssets + DB_SyncXAssets) once the DB
//      is idle and no level is loading, but under the files' OWN names. The engine would build the names
//      from its own tu (34) while the retail files are tu35, and the zone signature covers the name the
//      zone is loaded under: a renamed file fails it, and the engine answers by corrupting the
//      asset-entry free list so the next zone ERR_DROPs 0x3F6FDE09 on the DB thread (fatal). That is
//      what the 2026-09-16 and first 2026-09-23 boots hit.
//   3. Wait for zone status 4 or 9, then the slot callback (Playlist_LoadFromAssets). The ffotd slot is
//      left alone: its callback reinitialises playerdata, gametypes and Lua.
//   4. While those zones load, the DB_AllocXAssetEntry detour counts allocations per asset type and
//      logs the check that is about to drop, by name.
//
// The files are Activision publisher data: they stay on the player's machine and never enter the repo.
// Opt-out: "local_playlists": false in cw-mod.json. LIVE boots without our backend are never touched.

#include <cstddef>
#include <cstdint>

namespace Client::Game::LocalLpc {
	// Decides for this boot and resolves the engine calls. Call once, after ArxanCall is initialised
	// and before the hooks go in. Logs its decision.
	void Init(std::uintptr_t moduleBase, std::size_t imageSize);
	bool Enabled();

	// Game thread, every frame (OnShowOverStack). Returns at once when there is nothing left to do.
	void Tick();

	// DB_AllocXAssetEntry detour, any DB thread. True only while our two zones load.
	bool LoadInFlight();
	// Counts the allocation and logs a failed zone signature, an empty entry list or an empty type pool.
	// Runs before the original; changes nothing.
	void BeforeAssetAlloc(std::uint8_t type);
}
