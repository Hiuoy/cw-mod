#pragma once
#include "fastfile.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace MapKit::Zone {
	struct LoadedZone {
		std::string name;
		FastFileHeader header;            // the effective header: the .fd's result header when patched
		FastFileHeader baseHeader;        // the .ff's own header
		std::vector<std::uint8_t> stream; // the current xfile stream
		std::vector<BlockInfo> blocks;    // the .ff's blocks
		bool patched = false;
		std::size_t baseStreamSize = 0;
	};

	// Reads <dir>/<name>.ff and, when <dir>/<name>.fd exists, applies it, the way the game does.
	bool LoadZone(const std::filesystem::path& ffPath, LoadedZone& out, std::string& error);

	bool ReadWholeFile(const std::filesystem::path& path, std::vector<std::uint8_t>& data);
	bool WriteWholeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& data);
}
