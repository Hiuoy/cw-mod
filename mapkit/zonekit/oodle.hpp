#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// Fastfile blocks are Oodle Kraken. mapkit binds to the game's own oo2core_8_win64.dll at runtime;
// the DLL belongs to the user's install and is never shipped or committed with mapkit.
namespace MapKit::Zone::Oodle {
	bool Load(const std::filesystem::path& gameDir, std::string& error);
	bool IsLoaded();

	// Decompresses exactly dst.size() bytes. False when the block is damaged or the size is wrong.
	bool Decompress(std::span<const std::uint8_t> src, std::span<std::uint8_t> dst);

	// Kraken at level Normal (what the shipped block headers decode as). Replaces dst.
	bool Compress(std::span<const std::uint8_t> src, std::vector<std::uint8_t>& dst);
}
