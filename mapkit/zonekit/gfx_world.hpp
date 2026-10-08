#pragma once
#include "asset_record.hpp"
#include "xasset_list.hpp"
#include "xstream.hpp"
#include "xwriter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The renderer's world, gfx_map (0x1B): Load_GfxMapAsset 0x7FF71E7DC6B0 -> Load_GfxWorld 0x7FF71E7DB2A0 (7976-B
// root), with Load_GfxWorldDraw 0x7FF71E7DBEA0 (784 B at +432) and Load_GfxWorldRuntimeBuffers 0x7FF71E7DBDE0
// (88 B at +1216). R_LoadWorldFrame 0x7FF727F65100 finds it by the map's .d3dbsp name, and R_InitWorld_cand
// 0x7FF727F64E50 starts the level's rendering from it (its +7448 lighting and +7456 streamerworld included).
//
// mapkit copies a retail gfx_map byte for byte and re-points what cannot travel: the reader records where in
// the stream every reference, script-string index and sub-array sits, and the splice writer (asset_record.hpp) copies
// the asset's stream with those patched and chosen sub-arrays cut out. Nothing is modelled field by field, so
// the copy is exact wherever it is not edited.
namespace MapKit::Zone {
	bool LoadGfxWorldAsset(XStream& s, std::uint64_t header);      // 0x1B
	bool LoadGroupLodModelAsset(XStream& s, std::uint64_t header); // 0xBB: Load_GrouplodmodelAsset 0x7FF71E7D9410
	bool LoadSAnimAsset(XStream& s, std::uint64_t header);         // 0x66: Load_SanimAsset 0x7FF71E7DA300

	// A sanim copied whole (its stream bytes, root first) still holds its zone's script-string indices, which
	// Load_SAnim converts through the loading zone's table (DB_ResolveScriptStringIndex 0x7FF72966F0E0). These
	// are their offsets in `bytes`; nullopt when the bytes do not read back as exactly one sanim. An index left
	// as it was reads past a smaller table (Test G1 20:16: a copied sanim's index gave the pointer 0xA).
	std::optional<std::vector<std::size_t>> SAnimScriptStrings(std::span<const std::uint8_t> bytes);

	constexpr std::size_t kGfxWorldRootSize = 7976;

	// The recorded-asset types (asset_record.hpp) under the names the gfx_map code has always used.
	using GfxWorldRef = RecordRef;
	using GfxWorldString = RecordString;
	using GfxWorldChunk = RecordChunk;
	using GfxWorldSubtree = RecordSubtree;
	using GfxWorldTarget = RecordTarget;
	using GfxWorldCut = RecordCut;
	using GfxWorldSplice = RecordSplice;
	using GfxWorldLink = RecordLink;

	struct GfxWorld : AssetRecord {
		std::array<std::uint8_t, kGfxWorldRootSize> root{};
		std::string name;
	};

	// Decodes one gfx_map asset (the stream positioned at its start) into out.
	bool ReadGfxWorld(XStream& s, std::uint64_t header, GfxWorld& out);

	inline GfxWorldTarget ResolveGfxWorldRef(const GfxWorld& world, const GfxWorldRef& ref, std::uint64_t assetArrayPos,
		std::size_t assetCount) {
		return ResolveRecordRef(world, ref, assetArrayPos, assetCount);
	}

	inline bool GfxWorldLinks(const GfxWorldSplice& splice, std::vector<GfxWorldLink>& links, std::vector<std::string>& strings,
		std::string& error) {
		return RecordLinks(splice, links, strings, error);
	}

	// EncodeRecordedAsset with the gfx_map's reader: Load_GfxMapAsset(0, &header) with header inline reads it.
	bool EncodeGfxWorld(XWriter& w, const GfxWorldSplice& splice, std::string& error);
}
