#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace MapKit::Zone {
	// Asset and zone name hash (XHash): FNV-1a 64 over the lowercased name, top bit cleared.
	// Checked against the header of ww_1080_zm_silver's keyvaluepairs asset (0x37C92CEC44F4F0C5).
	constexpr std::uint64_t HashName(std::string_view name) {
		std::uint64_t hash = 0xCBF29CE484222325ull;
		for (char c : name) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c - 'A' + 'a');
			}
			hash ^= static_cast<std::uint8_t>(c);
			hash *= 0x100000001B3ull;
		}
		return hash & 0x7FFFFFFFFFFFFFFFull;
	}

	// XXH32. With seed 0 it is the checksum that trails each .fd VCDIFF window (XXH32, 0x7FF71E6745B0).
	std::uint32_t Xxh32(std::span<const std::uint8_t> data, std::uint32_t seed = 0);
}
