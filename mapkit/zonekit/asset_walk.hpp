#pragma once
#include "fastfile.hpp"
#include "xstream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Walks an xfile stream asset by asset with loaders transcribed from the game's Load_<Type>Asset
// functions (see xstream.hpp for the stream model). The stream holds no sizes, so the walk can only go
// as far as the first asset whose type has no loader here yet.
//
// A complete walk proves every loader it used, three ways at once: the walk ends exactly at the last
// byte of the stream, every back-reference lands inside data already loaded, and each block's high-water
// mark equals the size the zone header declares for it.
namespace MapKit::Zone {
	struct WalkedAsset {
		std::uint64_t type = 0;
		std::size_t begin = 0; // stream byte range of the asset's data, [begin, end)
		std::size_t end = 0;
		std::array<std::uint64_t, kXBlockCount> pos{}; // every block's position where it begins
	};

	struct WalkResult {
		std::vector<WalkedAsset> assets; // the ones walked, in stream order
		std::size_t assetCount = 0;      // how many the zone has
		std::size_t bodyOffset = 0;      // where the first asset's data starts
		std::array<std::uint64_t, kXBlockCount> bodyPositions{}; // every block's position there
		std::size_t cursor = 0;          // where the walk stopped
		bool complete = false;           // every asset walked and the stream ended exactly there
		std::uint64_t blockedType = ~0ull; // the first type without a loader, when that is what stopped it
		std::string error;               // a loader or a check failed (a real bug, not a missing loader)
		std::array<std::uint64_t, kXBlockCount> highWater{};
		std::size_t references = 0;
		std::size_t rootCopyBytes = 0;
		std::uint64_t preloadBlock1 = 0; // block 1's high-water in the lobby preload (XStream::PreloadRoot)
	};

	bool HasAssetLoader(std::uint64_t type);
	std::vector<std::uint64_t> AssetLoaderTypes();

	// roots, when given, gets every asset root stored inline (XStream::LogRoots).
	WalkResult WalkStream(std::span<const std::uint8_t> stream, std::vector<XStream::RootRecord>* roots = nullptr);

	// What the zone header's XBlock sizes should be after this walk. Measured on the 273 zones the first
	// two loaders walk completely (2026-09-25):
	//   block 0  = the temp high-water mark + 40 (the XAssetList header is counted there);
	//   block 1  = 208 + every inline asset root struct, 16-aligned (the link-time copies). The lobby preload lays the
	//              roots out there itself, with a 104-B save per techset field, and needs more for a zone of many
	//              materials: WalkResult::preloadBlock1 (zone_writer.cpp writes the larger);
	//   block 11 = filled at link time, not modelled yet (nullopt);
	//   others   = the high-water mark.
	std::array<std::optional<std::uint64_t>, kXBlockCount> ExpectedBlockSizes(const WalkResult& walk);
	// For a complete walk: the blocks whose expected size differs from the header's, as text ("" = all match).
	std::string CompareBlockSizes(const WalkResult& walk, const std::array<std::uint64_t, kXBlockCount>& declared);
}
