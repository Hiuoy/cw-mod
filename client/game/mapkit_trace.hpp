#pragma once
// mapkit zone trace: records where every asset of a zone starts while the game loads it, so mapkit's
// offline tools can decode any one asset of a retail zone without a loader for every type before it.
//
// A level zone mixes ~90 asset types (zm_silver: 182,002 assets) and its stream holds no sizes, so an
// offline walk stops at the first type mapkit has no loader for (asset 2 of zm_silver). The engine knows
// where each asset starts; this writes that down.
//
// On: cw-mod.json "mapkit_trace": ["zm_silver"] (zone names; "*" = every zone). Off (the default): nothing
// is hooked. For each traced zone load, <game>/cw-mod/mapkit/trace/<zone>.mktrace holds one record per
// asset, taken as Load_XAsset starts it, plus one end record. Read by `ffinfo <zone> --trace <file>`.
// The format is TraceFileHeader + TraceRecord[recordCount], little endian; mapkit/zonekit/zone_trace.hpp
// reads the same layout (keep the two in sync).
// A trace holds offsets and block positions only, no asset data. It stays on the player's disk all the
// same: it describes the retail zone it was taken from.

#include <cstddef>
#include <cstdint>

namespace Client::Game::MapKitTrace {
	constexpr std::uint32_t kTraceVersion = 1;
	constexpr std::uint32_t kEndRecordType = 0xFFFFFFFF;

	struct TraceFileHeader {
		char magic[4];                // "MKTR"
		std::uint32_t version;        // kTraceVersion
		std::uint32_t recordSize;     // sizeof(TraceRecord)
		std::uint32_t assetCount;     // the zone's XAssetList count
		std::uint32_t recordCount;    // assets traced + the end record (when the load got that far)
		std::uint32_t flags;          // DB_LoadXFile_Internal's last argument (zone load flags)
		char zone[64];                // the name the zone was loaded under
		std::uint64_t blockBase[13];  // each XBlock's base address in that process (for reference)
	};
	static_assert(sizeof(TraceFileHeader) == 192);

	struct TraceRecord {
		std::uint32_t index;          // asset index; assetCount for the end record
		std::uint32_t type;           // XAsset type (low 32 bits of the entry's first qword); kEndRecordType at the end
		std::uint64_t header;         // the entry's stored header value: -1 inline, -2 insert, else a reference
		std::uint64_t stream;         // bytes read from the zone's stream since the load began (see the .cpp)
		std::uint32_t block;          // current XBlock
		std::uint32_t depth;          // stream position stack depth
		std::uint64_t pos[13];        // every XBlock's position, as an offset from its base
	};
	static_assert(sizeof(TraceRecord) == 136);

	// Reads the setting. True when at least one zone is to be traced: then the four detours go in.
	bool Init(std::uintptr_t moduleBase, std::size_t imageSize);
	bool Enabled();

	// Detour bodies. Zone loads run one at a time on the database thread; the counters only count on the
	// thread that began the traced zone.
	void OnZoneBegin(const char* zoneName, void* assetList, void* blocks, int flags);
	void OnZoneEnd();
	void OnAssetBegin(const std::uint8_t* asset);
	void OnAssetEnd();
	void OnStreamBytes(std::int64_t count);

	// From the crash logger, on the faulting thread: when that thread is loading a traced zone, writes what it
	// recorded so far (no end record) and names the asset it faulted in.
	void OnCrash();
}
