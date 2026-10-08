#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// An asset-usage census, recorded by the cw-mod client while a level runs (client/game/mapkit_usage.hpp, cw-mod.json
// "mapkit_usage": true): what the match looked up, and which zone held each loaded asset. Names, zone indices and code
// addresses only. The layout is the client's (keep the two in sync): UsageFileHeader, then zones, entries, bgcache
// tables, bgcache list entries, DB lookups and bgcache lookups, little endian.
namespace MapKit::Zone {
	constexpr std::uint32_t kUsageVersion = 1;

	struct UsageFileHeader {
		char magic[4];             // "MKUS"
		std::uint32_t version;
		std::uint32_t zoneCount;
		std::uint32_t entryCount;
		std::uint32_t tableCount;
		std::uint32_t listCount;
		std::uint32_t findCount;
		std::uint32_t cacheCount;
		std::uint64_t moduleBase;
		std::uint64_t time;        // unix seconds
		char map[64];              // the custom map active when the world started, "" for a retail one
	};
	static_assert(sizeof(UsageFileHeader) == 112);

	struct UsageZone {
		std::uint32_t index;       // what an entry's zone byte says
		std::uint32_t flags;
		std::uint32_t status;      // 3 or 5 = loaded
		std::uint32_t pad;
		char name[64];
	};
	static_assert(sizeof(UsageZone) == 80);

	struct UsageEntry {            // one loaded asset (an XAssetEntry)
		std::uint64_t name;        // top bit cleared
		std::uint8_t type;
		std::uint8_t zone;
		std::uint8_t head;         // 1: the entry a lookup finds; 0: an override chained behind it
		std::uint8_t pad[5];
	};
	static_assert(sizeof(UsageEntry) == 16);

	struct UsageTable {            // a bgcache table (the engine has 40)
		std::uint32_t index;
		std::uint32_t type;        // the asset type of its names; 0xDD = names with no asset
		char name[56];
	};
	static_assert(sizeof(UsageTable) == 64);

	struct UsageListEntry {        // one name a loaded bgcache lists
		std::uint64_t name;
		std::uint8_t table;
		std::uint8_t zone;         // the bgcache's zone
		std::uint8_t pad[6];
	};
	static_assert(sizeof(UsageListEntry) == 16);

	struct UsageLookup {           // one (kind, name, caller) the match looked up
		std::uint64_t name;        // top bit cleared
		std::uint32_t kind;        // a DB lookup's asset type, a bgcache lookup's table
		std::uint32_t count;
		std::uint64_t caller;      // the return address as an IDB address; 0 = not in the game module
	};
	static_assert(sizeof(UsageLookup) == 24);

	constexpr std::uint32_t kBgCacheNoAsset = 0xDD;

	struct UsageFile {
		UsageFileHeader header{};
		std::vector<UsageZone> zones;
		std::vector<UsageEntry> entries;
		std::vector<UsageTable> tables;
		std::vector<UsageListEntry> lists;
		std::vector<UsageLookup> finds;  // DB_FindXAssetHeader
		std::vector<UsageLookup> caches; // BG_Cache_FindIndex_cand

		std::string Map() const;
		// The zone with that index ("" when the file has none).
		std::string ZoneName(std::uint32_t index) const;
		// A bgcache table's asset type (kBgCacheNoAsset when it has none or is unknown).
		std::uint32_t TableType(std::uint32_t table) const;
		std::string TableName(std::uint32_t table) const;
	};

	bool ReadUsageFile(const std::filesystem::path& path, UsageFile& out, std::string& error);

	// The engine's own bgcache lookups while a level loads: BG_Cache_Register_cand (0x7FF726213DB0, 0x24D bytes) looks
	// up every name every loaded bgcache lists, and BG_Cache_RegisterAll_cand (0x7FF726213850, 0x42B bytes) re-finds a
	// few. They say "listed", not "used". Callers are IDB addresses.
	bool IsRegistrationCaller(std::uint64_t caller);

	// The renderer's stream-in (0x7FF727F504C0, 0x14F bytes) resolves a stream request {name, kind @36} by name: an image
	// (kind's low byte = its mips), an xmodelmesh (0x100) or a streamkey (0x200). It runs from the stream-in queue
	// (0x7FF729284C70, which copies the data read from the packages into the asset) and from the streamerworld's +64/+72
	// records (0x7FF727F50610). It streams whatever any loaded world asks for, a replaced one included (zm_navtest,
	// 2026-09-30: 6,720 of zm_silver's world meshes, images and streamkeys that nothing of the map links to, and that its
	// zone does not even name), so it says "streamed", not "used".
	bool IsStreamInCaller(std::uint64_t caller);
}
