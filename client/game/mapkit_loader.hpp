#pragma once
// Custom maps (mapkit): zones the player built or rebuilt with mapkit, loaded from <game>/cw-mod/maps.
//
// Layout: one folder per map, holding that map's whole zone set, e.g.
//   cw-mod/maps/zm_mapkit/zm_mapkit.ff, techset_zm_mapkit.ff, ww_zm_mapkit.ff, 1080_zm_mapkit.ff,
//                         4k_zm_mapkit.ff, ww_1080_zm_mapkit.ff, ww_4k_zm_mapkit.ff, en_zm_mapkit.ff
// The engine expands a level zone into all of those names and a missing one is fatal (dump_anchors.hpp).
// A folder may also hold a zone under a RETAIL name (e.g. zm_silver.ff): it then replaces the retail one.
//
// What this does (IDB-verified 2026-09-25, see dump_anchors.hpp "mapkit"):
//   1. Mount, once, on the game thread: FS_AddSearchPath(folder, 200, 0), which beats the retail zone
//      dir (400), for every folder that has a .ff.
//   2. DB_Signature_VerifyZone detour: skipped ONLY for a zone whose name is one of the .ff files found
//      in cw-mod/maps. Our zones are not signed by Treyarch; retail zones are still verified as before.
//   3. FS_OpenFileRead detour: <custom zone>.fd is "not found", so a replaced retail zone is never
//      patched with the retail .fd (it would fail the base-header check and drop).
//   4. An optional redirect set from the overlay's Maps tab, e.g. "when the lobby starts zm_silver, start
//      zm_mapkit", in three parts. MapPreload_StartZoneRead detour: the lobby preloads the target's zone
//      instead (a level zone can only be loaded into the lobby's preload buffer). DB_LoadXAssets detour:
//      any other level-zone load of the retail name (the client's own map load at launch) loads the
//      target, or it would unload the preloaded target. SV_StartMap detour: the map start names the
//      target, and only if that zone is loaded. So the redirect must be set before the lobby preloads
//      the map. Refused (and logged) while the target's zone set is incomplete.
//   5. DB_ExpandZoneVariants detour: an OVERRIDE zone (ww_1080_<map> / ww_4k_<map> in a folder whose
//      <map>.ff is retail, what `cwlink patch` / `cwlink plane` write) moves to just after <map>. The
//      engine lists the map's own zone last, and links in that order. An override zone listed first finds
//      nothing when it references the map's assets by name, and a type without a default asset (lighting)
//      drops. Priority, not order, still decides which copy of an asset wins.
//   6. Listed maps (a map.json in the folder, what `cwlink build` writes): the Zombies Private CUSTOM MAPS
//      tab (ui_scripts.hpp) lists them, and each is live ONLY while picked there (SetActiveMap), so the
//      stock maps stay stock. An OVERLAY map ships ww_1080_<id>.ff / ww_4k_<id>.ff: while it is active, the
//      DB_ExpandZoneVariants detour loads them right after <base>, besides <base>'s own ww_1080_<base> /
//      ww_4k_<base> (not in their place: the lobby preloads <base> before the pick, and a preloaded zone
//      the launch drops breaks the order zone memory is freed in). A lobby preload stays stock. A CLONE
//      map ships <id>.ff and its variants: picking it sets the redirect of 4. Its level
//      scripts (maps/<id>/scripts) are served only while it is active. A folder without a map.json is
//      live for every match, as before.
//   7. A MAP OF ITS OWN (map.json "format": "standalone", what `cwlink build` writes since 2026-09-29): <id>.ff
//      alone, with its level scripts scripts/<p>/<id>.gsc/.csc. It starts under its own name, so every PC in the
//      lobby loads it by that name (the lobby sends the map as a string: an overlay was live only on the PC that
//      picked it, and a joining client loaded stock Die Maschine: "Clientfield Mismatch", 2026-09-29). Picking it
//      makes the host's lobby map <id> (Session_SetMapName detour; the playlist names its asset library). Wherever
//      <id> loads, as a lobby preload or a level load, DB_ExpandZoneVariants puts its asset library ("base", e.g.
//      zm_silver: AI, weapons, FX, sounds, shaders) before it, drops its own variants, and gives it the override
//      zones' flags (priority 27), and the loaded map decides which level scripts are served (not the pick: a
//      client never picks). The maptable knows <id> by the library's entry (getmapfields).
//      Its world keeps the library's names and replaces the library's world, as an overlay's does: the engine
//      keeps one world loaded (IDB 2026-09-29: g_clipMap is the clip_map pool's first slot, whatever CM_LoadMap
//      finds; the renderer loads the world named after the level zone that finishes loading, zm_silver here,
//      DB_PostLoadFrame_ApplyOverrides -> R_BeginLoadWorld). A world named after <id> sat next to the library's
//      instead, and the renderer drew the library's while the client game ran <id>'s: the 19:04 crash. The engine
//      still looks the world up by <id>'s .d3dbsp name (CM_LoadMap, the navmesh, the entity lists, the client
//      game's world), so every such lookup looks up the library's name instead (the DB_FindXAssetHeader detour in
//      scripting.cpp asks WorldAssetName), which holds the map's world after the swap.
//      Since P6 step 2b-2 (2026-10-02) map.json may list the library's zones that load ("library": [...]; cwlink
//      writes only "techset_<base>", for the sky's shaders): the map's zone holds the rest of what it takes from the
//      base, and its own package list opens the streamed data. Every other zone of the base is then dropped from the
//      expanded list, and <id>.ff loads as the level zone (no override flags): the renderer loads the world of the
//      first such zone that finishes loading (R_BeginLoadWorld(<id>)), and the world-name alias above finds the
//      map's world, which keeps the base's names. Without the list the whole base loads, as before.
// Nothing is installed when cw-mod/maps has no .ff, or with "custom_maps": false in cw-mod.json.

#include "game/function_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Client::Game::MapKit {
	struct MapFolder {
		std::string name;                 // folder name = the map's zone name
		std::string dir;
		std::vector<std::string> zones;   // .ff files in the folder, without extension
		std::vector<std::string> missing; // expected zones found neither here nor in the retail zone dir
		bool replacesRetail = false;      // <name>.ff also exists in the retail zone dir
		// From map.json. Only a listed map is offered in the CUSTOM MAPS tab, and live only while picked.
		bool listed = false;
		bool overlay = false;             // listed: ww_1080_<name>/ww_4k_<name> over <base>; else a clone of it
		bool standalone = false;          // listed: a map of its own, <name>.ff with <base> as its asset library (7.)
		bool partialLibrary = false;      // standalone: map.json lists the zones of <base> that load ("library")
		std::vector<std::string> library; // ...and these are they (cwlink: techset_<base>)
		std::string title;
		std::string base;                 // the retail map it starts through, e.g. zm_silver (standalone: its library)
		std::string description;
		int playlist = 0;                 // the private playlist to start it through; 0 = the tab's default for base
	};

	// Scans cw-mod/maps and resolves the engine calls. Call once, after ArxanCall is initialised and
	// before the hooks go in. Logs its decision.
	void Init(std::uintptr_t moduleBase, std::size_t imageSize);
	bool Enabled();

	// Game thread, every frame (OnShowOverStack). Mounts the folders once; then returns at once.
	void Tick();

	// Detour bodies.
	// DB_Signature_VerifyZone: true when the zone being verified is ours. Clears the signature state
	// the way the original does; the caller must then NOT call the original.
	bool SkipSignatureCheck();
	// FS_OpenFileRead: true when path is <custom zone>.fd, which must read as "not found".
	bool HidesPatchFile(const char* path);
	// MapPreload_StartZoneRead: the map zone the lobby preloads. mapName itself unless a redirect applies.
	const char* RedirectPreload(const char* mapName);
	// DB_LoadXAssets: zones itself, or a per-thread copy with the redirected level zone renamed. Logs every
	// level-zone load while a redirect is set, with the caller's return address.
	Functions::XZoneInfo* RedirectZones(Functions::XZoneInfo* zones, std::uint32_t count, int freeFlags,
		std::uintptr_t caller);
	// DB_ExpandZoneVariants, before the original: `zones` itself, or a per-thread copy with each map of its own's
	// asset library inserted before it (same flags); count is updated. A level load or lobby preload of one list
	// sets the map this PC loads (which level scripts are served, which world lookups resolve to the library).
	Functions::XZoneInfo* WithLibraries(Functions::XZoneInfo* zones, int& count);
	// DB_ExpandZoneVariants, after the original: drops every variant of a map of its own (cwlink writes none; one in
	// its folder is left over from an older build). A map loading its whole asset library gets the override zones'
	// flags; one whose map.json lists the library's zones keeps those alone of the library (7.). Returns the new
	// count. Logs each list it changed, once.
	int TrimOwnMapVariants(Functions::XZoneInfo* zones, int count);
	// Session_SetMapName: the map name to set. mapName itself, or the CUSTOM MAPS pick's id when the pick is a map of
	// its own and mapName is its asset library (the playlist's map), unless the caller is a member applying the
	// host's settings.
	const char* HostLobbyMap(std::uint32_t slot, const char* mapName, std::uintptr_t caller);
	// DB_FindXAssetHeader (every type but scripts, on the asset threads too; one atomic load when no map of its own
	// is loaded): a lookup by the loaded map of its own's .d3dbsp name looks up its asset library's instead, the
	// name its world has (7.). Else name itself.
	std::uint64_t WorldAssetName(std::uint32_t type, std::uint64_t name);
	// MapTable_FindEntryByHash / MapTable_GetMapFlags: a map of its own's hash / name becomes its asset library's.
	std::uint64_t MapTableHash(std::uint64_t mapHash);
	const char* MapTableName(const char* mapName);

	// DB_ExpandZoneVariants, first: while an overlay map is active, inserts the map's own ww_1080_/ww_4k_ zone
	// right after its base, for each of the base's that zones[0..count) lists (see 6.), unless the load is a
	// lobby preload. zones has room for max entries. Returns the new count. Logs every insert.
	int AddActiveMap(Functions::XZoneInfo* zones, int count, int max);
	// DB_ExpandZoneVariants, then: moves each override zone in zones[0..count) to just after its map's own zone,
	// in place. Logs every move.
	void OrderOverrideZones(Functions::XZoneInfo* zones, int count);
	// SV_StartMap: the map name to start. mapName itself unless a redirect applies and, for a lobby
	// launch (kinds 1 and 2), the lobby preloaded the target.
	const char* RedirectMap(const char* mapName, std::uint32_t kind);
	// R_InitWorld_cand: logs the gfx_map the level draws with. mapkit's own (cwlink plane/build, test G1) has no
	// model groups and no terrain; the map's own has both. Reads only, every read guarded.
	void LogWorldInit();

	// Overlay (render thread). Copies, taken under a lock.
	std::vector<MapFolder> Folders();
	bool Mounted();
	std::string RedirectFrom();
	std::string RedirectTo();
	// to empty = no redirect.
	void SetRedirect(const std::string& from, const std::string& to);
	std::vector<std::string> RecentEvents();

	// The listed map picked in the CUSTOM MAPS tab; "" = none, every map starts stock. Main thread (the Lua command):
	// picking a map of its own also sets the lobby map. False (and logged, with nothing made active) when id is not
	// a listed map or its zone set is incomplete.
	bool SetActiveMap(const std::string& id);
	// The listed map whose level scripts are served: the map of its own this PC loads (host or client), else the
	// picked overlay or clone. "" = none.
	std::string ActiveMap();
	// The CUSTOM MAPS pick itself (for the tab's Lua): "" = none.
	std::string PickedMap();
	// Folders() filtered to the listed maps, for the tab. Callable before Init (then empty).
	std::vector<MapFolder> ListedMaps();
}
