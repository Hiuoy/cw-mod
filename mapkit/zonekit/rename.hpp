#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Moving a map's zone set to a new name. The engine finds a map's content by hashes of names built
// from the map name, so those hashes are rewritten in place (all hashes are fixed width, so the stream
// layout never changes).
namespace MapKit::Zone {
	// T9's 32-bit script hash (GSC function and namespace names): Jenkins one-at-a-time over the
	// lowercased name, starting from 0x4B9ACE2F, then a final mix. Matches ACTS h32
	// (zm_silver = 0xEE0FC845, main = 0xFB7D78F8).
	constexpr std::uint32_t HashScript32(std::string_view name) {
		std::uint32_t hash = 0x4B9ACE2F;
		for (char c : name) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c - 'A' + 'a');
			}
			const std::uint32_t t = static_cast<std::uint8_t>(c) + hash;
			hash = (t ^ (t << 10)) + ((t ^ (t << 10)) >> 6);
		}
		const std::uint32_t x = 9 * hash;
		return 0x8001 * (x ^ (x >> 11));
	}

	struct HashRewrite {
		std::string from;
		std::string to;
		unsigned bits = 64;
		std::size_t count = 0;
	};

	// Rewrites everything in a stream that is keyed by the map's name, so the zone loads as `to`:
	//   64-bit  maps/<p>/<from>.d3dbsp   the world assets (Com_GetMapBspName; <p> = text before the first '_')
	//           scripts/<p>/<from>.gsc   the level scripts: asset names, GSC headers, every #using
	//           scripts/<p>/<from>.csc
	//           <from>                   the bare name (#"zm_silver" in scripts, per-map entries)
	//   32-bit  <from>                   the level scripts' GSC namespace, inside compiled scripts only:
	//                                    a 4-byte value is too short to replace blindly in 100+ MB
	std::vector<HashRewrite> RenameMapInStream(std::vector<std::uint8_t>& stream, std::string_view from, std::string_view to);

	// Every occurrence at any byte offset (stream data is packed, not aligned).
	std::size_t ReplaceValue64(std::span<std::uint8_t> data, std::uint64_t from, std::uint64_t to);
	std::size_t ReplaceValue32(std::span<std::uint8_t> data, std::uint32_t from, std::uint32_t to);

	// Compiled GSC objects in a stream: {offset, size} from each header's magic and file_size (+0x48).
	std::vector<std::pair<std::size_t, std::size_t>> FindGscObjects(std::span<const std::uint8_t> stream);
}
