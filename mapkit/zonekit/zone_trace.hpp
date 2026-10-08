#pragma once
#include "fastfile.hpp"
#include "xasset_list.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// A zone trace: where every asset of a zone starts, recorded by the cw-mod client while the game loads
// the zone (client/game/mapkit_trace.hpp, cw-mod.json "mapkit_trace"). The stream holds no sizes, so
// without it an asset can only be reached by walking every asset before it, which needs a loader for
// every type in the zone (a level zone has ~90). With it, one asset decodes from its recorded start.
//
// File: TraceFileHeader + TraceRecord[recordCount], little endian, the same layout the client writes
// (keep the two in sync). One record per asset, taken when Load_XAsset starts it, plus an end record
// (index = assetCount) taken after the last asset. `stream` is the client's count of bytes read from the
// zone's (patched) stream since the load began; it is aligned to our stream offsets with the first asset,
// whose offset we know from the XAssetList.
namespace MapKit::Zone {
	constexpr std::uint32_t kTraceVersion = 1;
	constexpr std::uint32_t kTraceEndType = 0xFFFFFFFF;

	struct TraceFileHeader {
		char magic[4];                          // "MKTR"
		std::uint32_t version;
		std::uint32_t recordSize;
		std::uint32_t assetCount;
		std::uint32_t recordCount;
		std::uint32_t flags;                    // the zone load flags
		char zone[64];
		std::uint64_t blockBase[kXBlockCount];  // the client process's block addresses (reference only)
	};
	static_assert(sizeof(TraceFileHeader) == 192);

	struct TraceRecord {
		std::uint32_t index;
		std::uint32_t type;
		std::uint64_t header;
		std::uint64_t stream;
		std::uint32_t block;
		std::uint32_t depth;
		std::uint64_t pos[kXBlockCount];
	};
	static_assert(sizeof(TraceRecord) == 136);

	// One asset's start, in our terms.
	struct TracedAsset {
		std::uint64_t type = 0;
		std::uint64_t header = 0;
		std::size_t offset = 0;                       // stream offset of the asset's data
		int block = 0;                                // current XBlock at its start (4 in every zone seen)
		std::array<std::uint64_t, kXBlockCount> pos{}; // every block's position at its start
	};

	struct ZoneTrace {
		std::string zone;
		std::uint32_t flags = 0;
		// assets[i] for i < assetCount, then the end state at assets[assetCount] when the load finished.
		std::vector<TracedAsset> assets;
		std::size_t assetCount = 0;
		bool complete = false;

		const TracedAsset* End() const { return complete ? &assets.back() : nullptr; }

		// flags & 0x6A0 (the lobby's level-zone preload is 0x200): the engine loaded the zone with its twin
		// loader family (Load_XAsset_Preload 0x7FF71E867350). Same stream grammar, but asset roots go to block
		// 1 instead of temp block 0, and every pointer field logs an 8-byte fixup in block 11. mapkit's loaders
		// follow the normal family, so blocks 0, 1 and 11 are not comparable in such a trace.
		bool PreloadFamily() const { return (flags & 0x6A0) != 0; }
		bool Comparable(std::size_t block) const {
			return !PreloadFamily() || (block != 0 && block != 1 && block != 11);
		}
	};

	bool ReadZoneTrace(const std::filesystem::path& path, ZoneTrace& out, std::string& error);

	// Aligns the trace to a stream and checks it against the stream's own XAssetList: same asset count,
	// same type and header per asset, offsets that only grow, and an end record on the last byte.
	// Fills each asset's offset. Returns "" when it all matches, else what did not.
	std::string AlignZoneTrace(ZoneTrace& trace, std::span<const std::uint8_t> stream, const XAssetList& list);

	struct TraceReplay {
		std::size_t index = 0;
		bool ok = false;
		std::string error; // the loader's, or which end state differed
	};

	// Decodes asset `index` with mapkit's loader from its recorded start and checks that it ends exactly
	// where the next asset starts: same stream offset and the same position in every block. That proves
	// the loader on this asset whether or not the zone can be walked. False (with no error) when mapkit
	// has no loader for its type.
	bool ReplayTracedAsset(const ZoneTrace& trace, std::span<const std::uint8_t> stream, std::size_t index,
		TraceReplay& out);

	// The bytes a stored reference points at (block << 60 | position, plus one), when an earlier asset stored
	// them: finds the asset whose load reached that position (the trace's block positions only grow outside the
	// temp block), replays it with a load log, and copies `size` bytes from the load that covers it. Needs a
	// loader for that asset's type. False with `error` set when it cannot.
	bool ResolveStoredData(const ZoneTrace& trace, std::span<const std::uint8_t> stream, std::uint64_t stored,
		std::size_t size, std::vector<std::uint8_t>& out, std::string& error);

	// A stored link to another asset of the zone points at that asset's XAsset entry (block 4, just before the first
	// asset; +8 = the header field): the entry's index, or SIZE_MAX when it is no such link.
	std::size_t TracedAssetOfReference(const ZoneTrace& trace, const XAssetList& list, std::uint64_t stored);
	// The name hash in an asset's root (at XAssetNameOffset; top bit cleared): its own for an asset stored here, the
	// linked one's for a by-name reference.
	std::uint64_t TracedAssetName(const ZoneTrace& trace, std::span<const std::uint8_t> stream, std::size_t asset);
	// The asset whose data a stored pointer points into: the last one that started at or before that position in its
	// block (as ResolveStoredData finds it). SIZE_MAX for no asset, or a block that rewinds (0, the temp block).
	std::size_t TracedAssetOfData(const ZoneTrace& trace, std::uint64_t stored);
}
