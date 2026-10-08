#pragma once
#include "asset_walk.hpp"
#include "fastfile.hpp"
#include "xwriter.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Writes a zone from scratch: the XAssetList (script strings, then the XAsset array with every header
// inline), then each asset's data as its encoder writes it (Load_<Type>Asset(0, &header) with header =
// kPtrInline). The stream is then walked with mapkit's own loaders: the walk must be complete (every asset
// read back, the stream ending on its last byte), and the header's XBlock sizes come from it
// (ExpectedBlockSizes). A zone mapkit cannot read back is never written.
namespace MapKit::Zone {
	struct ZoneAsset {
		std::uint64_t type = 0;
		std::string label;                       // for reports
		std::function<void(XWriter&)> encode;    // writes the asset's data
		// The script strings the data stores (as zone-local indices, through XWriter::ScriptString). An
		// index means nothing outside the zone that holds the table.
		std::vector<std::string> scriptStrings;
		// Set on an asset later assets link to by name through XWriter::AssetEntry (a by-name reference that a
		// copied gfx_map points at, say). 0: none.
		std::uint64_t linkName = 0;
		// A by-name reference (EncodeAssetReference): only the name, linking to the asset of that name wherever it is
		// loaded when the zone links.
		bool byName = false;
	};

	// A zone-local script-string index inside copied bytes (`at`), and the string it stood for in the zone
	// the bytes came from. The copy stores that string's index in the new zone's table instead.
	struct CopiedString {
		std::size_t at = 0;
		std::string text;
	};

	// A field inside copied bytes (`at`) that links another asset: the copy points it at this zone's entry for that
	// asset (XWriter::AssetEntry), which the caller adds before the copy (a by-name reference, usually).
	struct CopiedLink {
		std::size_t at = 0;
		std::uint64_t type = 0;
		std::uint64_t name = 0;
	};

	// A pointer inside copied bytes (`at`) to data the copy itself loads earlier (the byte `target` of the copy): the
	// copy stores where that byte lands in this zone (found by reading the copy back; needs the type's loader).
	struct CopiedPointer {
		std::size_t at = 0;
		std::size_t target = 0;
	};

	// An asset copied byte for byte from another zone's stream. Only valid for an asset whose only references are
	// the links in `links` and the pointers into its own data in `pointers` (every other pointer is null or inline);
	// its script strings must all be listed in `strings`, since an index means nothing outside its own zone's table.
	// When mapkit has its loader, the writer's positions continue after it (so later assets may point by position),
	// and a link name makes it reachable through XWriter::AssetEntry.
	ZoneAsset CopiedAsset(std::uint64_t type, std::string label, std::vector<std::uint8_t> bytes,
		std::vector<CopiedString> strings = {}, std::vector<CopiedLink> links = {}, std::vector<CopiedPointer> pointers = {});

	// The zone's script-string table is `scriptStrings` (null entries stay null), then every string an asset
	// declares that it lacks. Entry 0 is kept null, as in retail zones: an index of 0 means no string.
	// Empty, with `error` set, when an asset links one written after it (XWriter::ForwardLinks).
	std::vector<std::uint8_t> BuildZoneStream(std::span<const std::optional<std::string>> scriptStrings,
		std::span<const ZoneAsset> assets, std::string& error);

	struct ZoneWriteResult {
		WalkResult walk;
		std::array<std::uint64_t, kXBlockCount> blockSizes{};
		std::uint64_t block1Rule = 0; // block 1 by the retail rule (ExpectedBlockSizes); blockSizes[1] is never less
		std::string error;
	};

	// Walks `stream`, then writes `header` (a retail zone's, for the build checksum and flags) renamed to
	// zoneName, with the walk's block sizes, as a .ff into `file`. The signature is left as it was: the
	// cw-mod client skips the check for zones loaded from cw-mod/maps.
	bool WriteZone(FastFileHeader header, std::string_view zoneName, std::span<const std::uint8_t> stream,
		std::vector<std::uint8_t>& file, ZoneWriteResult& result);
}
