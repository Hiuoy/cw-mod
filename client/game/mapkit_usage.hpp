#pragma once
// mapkit asset-usage census (plan P6 step 1, docs/mapkit-plan.md): which assets a match looks up, and which zone
// holds each loaded asset. `ffinfo zm_silver --trace <f> --needs --usage <file>` turns that into the count of what a
// mapkit map still takes from its asset library (Die Maschine's zones) and would have to copy into its own zone.
//
// On: cw-mod.json "mapkit_usage": true. Off (the default): nothing is hooked or recorded. From boot on it records:
//   - every DB_FindXAssetHeader lookup: type, name and the calling function (the script loader's detour,
//     scripting.cpp, calls OnFind);
//   - every BG_Cache_FindIndex_cand lookup: bgcache table, name and caller. That is how scripts and map entities
//     reach the assets a zone's bgcache lists (models, weapons, AI types, FX, anims, bundles, sound aliases), so it
//     is most of what a level uses. The level load registers every listed name with a DB lookup from
//     BG_Cache_Register_cand; those say "listed", not "used", and the offline tool leaves them out.
// Once a level's world starts, <game>/cw-mod/mapkit/usage/<map>_<date>_<time>.mkuse is written 45 s later and again
// every 2 minutes while new lookups come in. Each write also takes every loaded asset (type, zone, name, and each
// entry of an override chain), the zones, the bgcache tables and each loaded bgcache's list.
// Names, zone indices and code addresses only, no asset data. The format is UsageFileHeader then the arrays below, in
// that order, little endian; mapkit/zonekit/usage_file.hpp reads the same layout (keep the two in sync).

#include <cstddef>
#include <cstdint>
#include <string>

namespace Client::Game::MapKitUsage {
	constexpr std::uint32_t kUsageVersion = 1;

	struct UsageFileHeader {
		char magic[4];             // "MKUS"
		std::uint32_t version;     // kUsageVersion
		std::uint32_t zoneCount;   // UsageZone[]
		std::uint32_t entryCount;  // UsageEntry[]
		std::uint32_t tableCount;  // UsageTable[]
		std::uint32_t listCount;   // UsageListEntry[]
		std::uint32_t findCount;   // UsageLookup[]: DB_FindXAssetHeader
		std::uint32_t cacheCount;  // UsageLookup[]: BG_Cache_FindIndex_cand
		std::uint64_t moduleBase;  // this process's (callers are written as IDB addresses)
		std::uint64_t time;        // unix seconds
		char map[64];              // the custom map active when the world started, "" for a retail one
	};
	static_assert(sizeof(UsageFileHeader) == 112);

	struct UsageZone {             // one loaded zone: g_zoneInfoRows[index - 1]
		std::uint32_t index;       // what an entry's zone byte says
		std::uint32_t flags;
		std::uint32_t status;      // 3 or 5 = loaded
		std::uint32_t pad;
		char name[64];
	};
	static_assert(sizeof(UsageZone) == 80);

	struct UsageEntry {            // one XAssetEntry
		std::uint64_t name;        // top bit cleared
		std::uint8_t type;
		std::uint8_t zone;
		std::uint8_t head;         // 1: the entry a lookup finds; 0: an override chained behind it
		std::uint8_t pad[5];
	};
	static_assert(sizeof(UsageEntry) == 16);

	struct UsageTable {            // g_bgCacheTables[index]
		std::uint32_t index;
		std::uint32_t type;        // the asset type of its names; 0xDD = names with no asset
		char name[56];
	};
	static_assert(sizeof(UsageTable) == 64);

	struct UsageListEntry {        // one name a loaded bgcache lists
		std::uint64_t name;        // top bit cleared
		std::uint8_t table;
		std::uint8_t zone;         // the bgcache's zone
		std::uint8_t pad[6];
	};
	static_assert(sizeof(UsageListEntry) == 16);

	struct UsageLookup {           // one (kind, name, caller) seen
		std::uint64_t name;        // top bit cleared
		std::uint32_t kind;        // a find's asset type, a cache find's table
		std::uint32_t count;
		std::uint64_t caller;      // the return address as an IDB address (dump imagebase); 0 = not in the game module
	};
	static_assert(sizeof(UsageLookup) == 24);

	// Reads the setting. When true, hook.cpp installs the BG_Cache_FindIndex_cand detour.
	void Init(std::uintptr_t moduleBase, std::size_t imageSize);
	bool Enabled();

	// Detour bodies; any thread. One relaxed load when the census is off.
	void OnFind(std::uint32_t type, std::uint64_t name, const void* returnAddress);
	void OnCacheFind(std::uint8_t table, std::uint64_t name, const void* returnAddress);

	// R_InitWorld: names this level's file and schedules its first write.
	void OnWorldStart(const std::string& map);
	// Game thread, every frame: starts a write when one is due (it runs on its own thread).
	void Tick();
	// The overlay's button: writes now. Returns what it did.
	std::string WriteNow();
	// The last write's result, for the overlay ("" before the first).
	std::string LastStatus();
}
