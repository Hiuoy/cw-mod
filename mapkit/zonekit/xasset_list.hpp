#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The start of every xfile stream. Reversed from DB_LoadXFile_Internal (0x7FF72769FC00) and
// Load_ScriptStringList (0x7FF71E7E8A80):
//   struct XAssetList (40 B) {
//     u32 stringCount; char** strings; u32* stringHashes;  // hashes live in XBlock 2: not in the file
//     u64 assetCount;  XAsset* assets;                      // XAsset = { u64 type; void* header; }
//   };
// then the string pointer array, the inline strings, the XAsset array, and the asset bodies in order.
namespace MapKit::Zone {
	constexpr std::size_t kXAssetTypeCount = 221;
	constexpr std::size_t kXAssetListSize = 40;

	// Stored pointer values. Anything else is a reference to data loaded earlier:
	// block = (v - 1) >> 60, offset = (v - 1) & 0x0FFFFFFFFFFFFFFF (DB_ConvertOffsetToPointer 0x7FF729668D00).
	constexpr std::uint64_t kPtrNull = 0;
	constexpr std::uint64_t kPtrInline = ~0ull;
	constexpr std::uint64_t kPtrInsert = ~1ull;

	std::string_view XAssetTypeName(std::uint64_t type);
	std::optional<std::uint64_t> XAssetTypeFromName(std::string_view name);

	// Where a type's root keeps its 64-bit name hash (top bit set: a by-name reference, DB_LinkXAssetEntry
	// 0x7FF727EC3AA0). DB_GetXAssetName jumps to [0x7FF72A386B18 + 40 * type], each type's `mov rax, [rcx+N]` getter.
	// The 40-byte records are {type name, size, alignment, GetName, SetName}, so the name and size that sit next to a
	// getter belong to the NEXT type. +0 for almost every type; 24 are not, among them xanim +112, klf +16 (+0 is an
	// XString), sanim +8, localizeentry +8, weapontunables +48 and dynmodel +224 (the .cpp has them all).
	std::size_t XAssetNameOffset(std::uint64_t type);

	struct XAssetEntry {
		std::uint64_t type = 0;
		std::uint64_t header = 0; // kPtrInline for every asset in shipped zones
	};

	struct XAssetList {
		std::vector<std::optional<std::string>> strings; // nullopt: null or a reference
		std::vector<XAssetEntry> assets;
		std::size_t assetsOffset = 0; // stream offset of the XAsset array
		std::size_t bodyOffset = 0;   // stream offset of the first asset body
	};

	bool ParseXAssetList(std::span<const std::uint8_t> stream, XAssetList& out, std::string& error);

	// Every stream offset in [begin, end) holding a stored link to an asset of the zone: a reference into its XAsset
	// array (block 4 from assetArrayPos, 16 B an entry) on an entry's header field (+8). A loader that misses a link
	// field still reads a zone exactly, since a link loads no stream bytes, so a copy checks the links its walk
	// recorded against these (P6 step 2a: the lighting's image sets hold six images, the walk took five).
	std::vector<std::size_t> FindAssetLinks(std::span<const std::uint8_t> stream, std::size_t begin, std::size_t end,
		std::uint64_t assetArrayPos, std::size_t assetCount);
}
