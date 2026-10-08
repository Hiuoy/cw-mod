#pragma once
#include "fastfile.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// .fd files. Every shipped .ff is the launch build; its .fd upgrades it to the current one:
//   40-byte patch header (u64 target size at +16, u64 source size at +24)
//   result header  (TAFF sections of the CURRENT zone; its codec byte selects the delta's
//                   compression: 6 zlib, 7 LZMA; every shipped .fd is 6)
//   base header    (must match the .ff's header, else "Patch file exists but is not for this fast file")
//   zlib stream of a VCDIFF delta from the .ff's inflated stream to the current one
// Reversed from DB_ReadZoneFileHeader (0x7FF727EC1510) and DB_Patch_VcdiffDecodeWindow (0x7FF72960AAD0).
namespace MapKit::Zone {
	constexpr std::size_t kPatchHeaderSize = 40;

	struct PatchFile {
		std::array<std::uint8_t, kPatchHeaderSize> patchHeader{};
		FastFileHeader result;
		FastFileHeader base;
		std::vector<std::uint8_t> delta; // inflated VCDIFF
	};

	bool ReadPatchFile(std::span<const std::uint8_t> file, PatchFile& out, std::string& error);

	// RFC 3284 VCDIFF with the default code table, plus the game's window bits: 0x04 = an extra
	// source-fetch varint, 0x08 = a trailing XXH32 of the window's output (verified here).
	bool ApplyVcdiff(std::span<const std::uint8_t> delta, std::span<const std::uint8_t> source,
		std::vector<std::uint8_t>& target, std::string& error);
}
