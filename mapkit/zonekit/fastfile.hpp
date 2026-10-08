#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The .ff container: a tagged header, then a chain of compressed blocks that inflate to the xfile
// stream. Reversed from DB_ReadFastfileHeaderTLV (0x7FF7276A1C30), DB_FillCompressedBlocks
// (0x7FF7276A2050) and DB_DecompressBlockJob (0x7FF72769F6E0).
namespace MapKit::Zone {
	constexpr std::uint32_t kFastFileMagic = 0x46464154; // "TAFF"
	constexpr std::uint32_t kXFileVersion = 100;
	constexpr std::size_t kXBlockCount = 13;
	constexpr std::size_t kHeaderStructSize = 1392;      // what the loader fills from the sections
	constexpr std::uint32_t kBlockRawSize = 0xFFFA0;     // raw size of every shipped block but the last
	constexpr std::size_t kFilePadding = 64;             // shipped files end on a 64-byte boundary

	// Section tags. The comment gives the size the loader accepts and where it lands in the struct.
	enum class HeaderTag : std::uint32_t {
		Version = 0x39CB44F1,     // 4 B    +0     xfile version, must be 100
		Flags = 0x2C2381CF,       // 6 B    +4     see FlagIndex
		Build = 0x37F9D612,       // 64 B   +16    build timestamp, 16-B build checksum at +28, builder name
		Linker = 0x8578C004,      // 264 B  +80    +84 == 2 means the linker failed, text at +85
		Stream = 0x0F992DFE,      // 24 B   +344   u64 decompressed stream size, then two u64
		BlockSizes = 0x66C65056,  // 104 B  +368   13 x u64 XBlock sizes
		BlockSizes2 = 0x6AEBB196, // 104 B  +472   13 x u64, second set (not needed to load)
		Unknown576 = 0x1CE68F50,  // 480 B  +576
		Identity = 0xC7708CDA,    // 336 B  +1056  zone name[64], RSA-2048 signature[256], 16 B
	};

	enum class FlagIndex : std::size_t {
		Server = 0,         // nonzero: "Cannot load server fastfile on client"
		Compression = 1,    // block codec: 0 none, 1-2 zlib, 3-4 LZ, 8-11 Oodle (every shipped .ff: 8)
		ChecksumMode = 2,   // 3: the build checksum must equal the exe's (DB_ValidateFastfileHeader)
		Encrypted = 3,      // nonzero: blocks are decrypted before decompression (never set in shipped zones)
	};

	constexpr std::uint8_t kCompressionOodle = 8;

	struct HeaderSection {
		std::uint32_t tag = 0;
		std::vector<std::uint8_t> data;
	};

	// Sections are kept in file order so an untouched header serializes byte-identically.
	class FastFileHeader {
	public:
		std::vector<HeaderSection> sections;

		// Parses "TAFF" + count + sections starting at offset; advances offset past them.
		static bool Parse(std::span<const std::uint8_t> file, std::size_t& offset, FastFileHeader& out, std::string& error);
		void Serialize(std::vector<std::uint8_t>& out) const;

		const HeaderSection* Find(HeaderTag tag) const;
		HeaderSection& Get(HeaderTag tag, std::size_t minSize);

		// The 1392-byte struct the loader builds; two headers are "the same" when these match.
		std::array<std::uint8_t, kHeaderStructSize> ToStruct() const;

		std::uint32_t Version() const;
		std::uint8_t Flag(FlagIndex index) const;
		void SetFlag(FlagIndex index, std::uint8_t value);
		std::uint64_t StreamSize() const;
		void SetStreamSize(std::uint64_t size);
		std::array<std::uint64_t, kXBlockCount> BlockSizes() const;
		void SetBlockSizes(const std::array<std::uint64_t, kXBlockCount>& sizes);
		std::string ZoneName() const;
		void SetZoneName(std::string_view name);
		std::string BuilderName() const;
		std::array<std::uint8_t, 16> BuildChecksum() const;
	};

	struct BlockInfo {
		std::uint32_t compressedSize = 0;
		std::uint32_t rawSize = 0;
		std::uint32_t storedSize = 0; // compressedSize rounded up to 4
		std::uint32_t fileOffset = 0; // where this 16-byte block header sits
	};

	// Parses the header and inflates every block into stream. blocks may be null.
	bool ReadFastFile(std::span<const std::uint8_t> file, FastFileHeader& header, std::vector<std::uint8_t>& stream,
		std::vector<BlockInfo>* blocks, std::string& error);

	// Writes header + Oodle blocks + terminator, padded like the shipped files. The header's stream
	// size and codec are set from what is written; its signature is left as given.
	bool WriteFastFile(FastFileHeader header, std::span<const std::uint8_t> stream, std::vector<std::uint8_t>& out,
		std::string& error);
}
