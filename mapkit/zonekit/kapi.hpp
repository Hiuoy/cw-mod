#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// KAPI packages: the .xsub files beside the zones hold everything a zone streams at runtime (XBlocks 7
// and 8: mesh buffers, image mips, streamer cells), each entry found by a 64-bit key. The layout follows
// Greyhound's Cold War reader (github.com/Scobalula/Greyhound, XSUBCache.cpp), checked on this install:
//   header (2024 B): u32 magic 'KAPI', u16 ?, u16 version (16), u64 ?, u64 type (3 = .xsub data; the
//     .xpak is an index, type 2), u64 size, 1896 B, then i64 fileCount, dataOffset, dataSize,
//     hashCount, hashOffset, hashSize, ...
//   hash table at hashOffset: {u64 key, u64 packed}; the entry starts at (packed >> 32) << 7 and takes
//     (packed >> 1) & 0x3FFFFFFF bytes.
//   an entry is a run of segments: {u32 count, u32 ?, u32 commands[count] (30 slots when count <= 30)},
//     then one block per command: size = command & 0xFFFFFF, kind = command >> 24 (8/9 Oodle with the
//     raw size in the first u32, 0 stored, 3 LZ4, anything else padding); the last block of a segment is
//     padded to 128 bytes.
namespace MapKit::Zone::Kapi {
	struct Entry {
		std::uint32_t file = 0;
		std::uint64_t offset = 0;
		std::uint32_t size = 0; // stored (compressed) size
	};

	class PackageIndex {
	public:
		// Reads the hash table of every .xsub in zoneDir; filter (the file stem, e.g. "zm-00008") may skip files.
		bool Open(const std::filesystem::path& zoneDir, std::string& error,
			const std::function<bool(const std::string&)>& filter = {});

		const Entry* Find(std::uint64_t key) const;
		// Decompresses one entry (needs Oodle loaded).
		bool Extract(std::uint64_t key, std::vector<std::uint8_t>& out, std::string& error) const;

		std::size_t Size() const { return m_Entries.size(); }
		std::size_t FileCount() const { return m_Files.size(); }
		const std::filesystem::path& File(std::uint32_t index) const { return m_Files[index]; }

	private:
		std::vector<std::filesystem::path> m_Files;
		std::unordered_map<std::uint64_t, Entry> m_Entries;
	};
}
