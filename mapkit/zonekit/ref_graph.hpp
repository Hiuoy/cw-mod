#pragma once
#include "xasset_list.hpp"
#include "zone_trace.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Which assets of a zone link to which, found from the stream and its trace alone: no loader per type is needed, so
// it covers all ~90 types of a level zone (mapkit plan P6, "count what a map borrows").
//
// A stored pointer to data loaded earlier is ((block << 60) | position) + 1 (xasset_list.hpp), and the trace says
// how far every block had got when each asset started. So an 8-byte value in an asset's bytes that decodes to a
// position an EARLIER asset loaded is a link to that asset:
//   - into the XAsset array (block 4, just before the first asset; the entry's +8): a link to another asset of the
//     zone, or to a by-name reference, which is how a zone links to an asset of another zone;
//   - to the start of what that asset stored in a block (the position the block had reached when it started, rounded
//     up to an alignment of at most 128): its root, or an insert slot holding a nested asset;
//   - into the middle of what it stored: kept apart as an inner pointer, not a link. zm_silver has 227,918 of them and
//     they are systematic (37% of its materials point into another material, 21% of its skeletons into another
//     skeleton, 7,941 pointers into one streamkey), so most are data stored once and shared, the way a fastfile shares
//     any data it already wrote (a gfx_map into its clip map's structs, too): a copy needs those bytes, but not the
//     asset around them. Followed as links, they pulled whole assets into every count: zm_silver's 5.3 MB resident image
//     had 115 such "parents" (44 images, 23 xanims, 12 xskeletons...). Some may also be plain data that decodes like a
//     pointer (the scan reads every byte offset, since the stream keeps no alignment).
// Blocks 0 and 1 are left out: the temp block is rewound, and roots are copied to block 1 only at link time, so nothing
// stores a position there.
namespace MapKit::Zone {
	struct RefGraph {
		// Per asset, in XAsset list order: the assets it links to, sorted, itself left out.
		std::vector<std::vector<std::uint32_t>> links;
		// Per asset: the earlier assets it holds a pointer into the middle of (not in links), sorted.
		std::vector<std::vector<std::uint32_t>> inner;
		std::vector<std::size_t> bytes; // its stream bytes
		std::size_t entryLinks = 0;     // links found through the XAsset array (before removing duplicates)
		std::size_t rootLinks = 0;      // links found to the start of another asset's data
		std::size_t innerPointers = 0;  // pointers found into the middle of another asset's data
		std::string error;              // the trace does not fit: no links
	};

	// The trace must be aligned to the stream (AlignZoneTrace) and complete.
	RefGraph BuildRefGraph(const ZoneTrace& trace, std::span<const std::uint8_t> stream, const XAssetList& list);

	// A by-name reference root, as LoadAssetHeader stores it: the name (top bit set) at +0, +8 or +16 and every
	// other byte zero. Returns the name without its top bit, or 0 when the root is not one.
	std::uint64_t ByNameReference(std::span<const std::uint8_t> root);
}
