#include "fastfile.hpp"
#include "oodle.hpp"

#include <algorithm>
#include <cstring>

namespace MapKit::Zone {
	namespace {
		struct SectionLayout {
			HeaderTag tag;
			std::size_t structOffset;
			std::size_t maxSize;
		};

		// DB_ReadFastfileHeaderTLV: each known tag is copied (up to maxSize) to structOffset.
		constexpr SectionLayout kLayout[] = {
			{ HeaderTag::Version, 0, 4 },
			{ HeaderTag::Flags, 4, 6 },
			{ HeaderTag::Build, 16, 64 },
			{ HeaderTag::Linker, 80, 264 },
			{ HeaderTag::Stream, 344, 24 },
			{ HeaderTag::BlockSizes, 368, 104 },
			{ HeaderTag::BlockSizes2, 472, 104 },
			{ HeaderTag::Unknown576, 576, 480 },
			{ HeaderTag::Identity, 1056, 336 },
		};

		constexpr std::size_t kBlockHeaderSize = 16;
		constexpr std::size_t kZoneNameSize = 64;

		template <typename T>
		bool ReadAt(std::span<const std::uint8_t> data, std::size_t offset, T& value) {
			if (offset > data.size() || data.size() - offset < sizeof(T)) {
				return false;
			}
			std::memcpy(&value, data.data() + offset, sizeof(T));
			return true;
		}

		template <typename T>
		void Append(std::vector<std::uint8_t>& out, const T& value) {
			const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
			out.insert(out.end(), p, p + sizeof(T));
		}

		std::size_t AlignUp(std::size_t value, std::size_t alignment) {
			return (value + alignment - 1) & ~(alignment - 1);
		}
	}

	bool FastFileHeader::Parse(std::span<const std::uint8_t> file, std::size_t& offset, FastFileHeader& out, std::string& error) {
		std::uint32_t magic = 0;
		std::uint32_t count = 0;
		if (!ReadAt(file, offset, magic) || !ReadAt(file, offset + 4, count)) {
			error = "file too small for a fastfile header";
			return false;
		}
		if (magic != kFastFileMagic) {
			error = "Invalid fastfile magic";
			return false;
		}

		std::size_t cursor = offset + 8;
		out.sections.clear();
		for (std::uint32_t i = 0; i < count; ++i) {
			HeaderSection section;
			std::uint32_t size = 0;
			if (!ReadAt(file, cursor, section.tag) || !ReadAt(file, cursor + 4, size) || file.size() - cursor - 8 < size) {
				error = "header section " + std::to_string(i) + " runs past the end of the file";
				return false;
			}
			section.data.assign(file.begin() + cursor + 8, file.begin() + cursor + 8 + size);
			out.sections.push_back(std::move(section));
			cursor += 8 + size;
		}
		offset = cursor;
		return true;
	}

	void FastFileHeader::Serialize(std::vector<std::uint8_t>& out) const {
		Append(out, kFastFileMagic);
		Append(out, static_cast<std::uint32_t>(sections.size()));
		for (const HeaderSection& section : sections) {
			Append(out, section.tag);
			Append(out, static_cast<std::uint32_t>(section.data.size()));
			out.insert(out.end(), section.data.begin(), section.data.end());
		}
	}

	const HeaderSection* FastFileHeader::Find(HeaderTag tag) const {
		for (const HeaderSection& section : sections) {
			if (section.tag == static_cast<std::uint32_t>(tag)) {
				return &section;
			}
		}
		return nullptr;
	}

	HeaderSection& FastFileHeader::Get(HeaderTag tag, std::size_t minSize) {
		for (HeaderSection& section : sections) {
			if (section.tag == static_cast<std::uint32_t>(tag)) {
				if (section.data.size() < minSize) {
					section.data.resize(minSize);
				}
				return section;
			}
		}
		HeaderSection section;
		section.tag = static_cast<std::uint32_t>(tag);
		section.data.resize(minSize);
		sections.push_back(std::move(section));
		return sections.back();
	}

	std::array<std::uint8_t, kHeaderStructSize> FastFileHeader::ToStruct() const {
		std::array<std::uint8_t, kHeaderStructSize> result{};
		for (const HeaderSection& section : sections) {
			for (const SectionLayout& layout : kLayout) {
				if (section.tag == static_cast<std::uint32_t>(layout.tag)) {
					std::memcpy(result.data() + layout.structOffset, section.data.data(), std::min(section.data.size(), layout.maxSize));
				}
			}
		}
		return result;
	}

	std::uint32_t FastFileHeader::Version() const {
		std::uint32_t version = 0;
		if (const HeaderSection* section = Find(HeaderTag::Version)) {
			ReadAt(std::span<const std::uint8_t>(section->data), 0, version);
		}
		return version;
	}

	std::uint8_t FastFileHeader::Flag(FlagIndex index) const {
		const HeaderSection* section = Find(HeaderTag::Flags);
		const auto i = static_cast<std::size_t>(index);
		return section && i < section->data.size() ? section->data[i] : 0;
	}

	void FastFileHeader::SetFlag(FlagIndex index, std::uint8_t value) {
		Get(HeaderTag::Flags, 6).data[static_cast<std::size_t>(index)] = value;
	}

	std::uint64_t FastFileHeader::StreamSize() const {
		std::uint64_t size = 0;
		if (const HeaderSection* section = Find(HeaderTag::Stream)) {
			ReadAt(std::span<const std::uint8_t>(section->data), 0, size);
		}
		return size;
	}

	void FastFileHeader::SetStreamSize(std::uint64_t size) {
		std::memcpy(Get(HeaderTag::Stream, 24).data.data(), &size, sizeof(size));
	}

	std::array<std::uint64_t, kXBlockCount> FastFileHeader::BlockSizes() const {
		std::array<std::uint64_t, kXBlockCount> sizes{};
		if (const HeaderSection* section = Find(HeaderTag::BlockSizes)) {
			std::memcpy(sizes.data(), section->data.data(), std::min(section->data.size(), sizeof(sizes)));
		}
		return sizes;
	}

	void FastFileHeader::SetBlockSizes(const std::array<std::uint64_t, kXBlockCount>& sizes) {
		std::memcpy(Get(HeaderTag::BlockSizes, sizeof(sizes)).data.data(), sizes.data(), sizeof(sizes));
	}

	std::string FastFileHeader::ZoneName() const {
		const HeaderSection* section = Find(HeaderTag::Identity);
		if (!section) {
			return {};
		}
		const auto* begin = reinterpret_cast<const char*>(section->data.data());
		return std::string(begin, strnlen(begin, std::min(section->data.size(), kZoneNameSize)));
	}

	void FastFileHeader::SetZoneName(std::string_view name) {
		HeaderSection& section = Get(HeaderTag::Identity, 336);
		std::memset(section.data.data(), 0, kZoneNameSize);
		std::memcpy(section.data.data(), name.data(), std::min(name.size(), kZoneNameSize - 1));
	}

	std::string FastFileHeader::BuilderName() const {
		const HeaderSection* section = Find(HeaderTag::Build);
		if (!section || section->data.size() <= 28) {
			return {};
		}
		const auto* begin = reinterpret_cast<const char*>(section->data.data() + 28);
		return std::string(begin, strnlen(begin, section->data.size() - 28));
	}

	std::array<std::uint8_t, 16> FastFileHeader::BuildChecksum() const {
		std::array<std::uint8_t, 16> checksum{};
		const HeaderSection* section = Find(HeaderTag::Build);
		if (section && section->data.size() >= 28) {
			// struct +28 is section +12 (the section lands at struct +16).
			std::memcpy(checksum.data(), section->data.data() + 12, checksum.size());
		}
		return checksum;
	}

	bool ReadFastFile(std::span<const std::uint8_t> file, FastFileHeader& header, std::vector<std::uint8_t>& stream,
		std::vector<BlockInfo>* blocks, std::string& error) {
		std::size_t offset = 0;
		if (!FastFileHeader::Parse(file, offset, header, error)) {
			return false;
		}
		if (header.Flag(FlagIndex::Encrypted)) {
			error = "encrypted fastfile blocks are not supported";
			return false;
		}
		const std::uint8_t codec = header.Flag(FlagIndex::Compression);
		if (codec < 8 || codec > 11) {
			error = "block codec " + std::to_string(codec) + " is not Oodle";
			return false;
		}

		stream.clear();
		stream.reserve(static_cast<std::size_t>(header.StreamSize()));
		while (true) {
			BlockInfo block;
			if (!ReadAt(file, offset, block.compressedSize) || !ReadAt(file, offset + 4, block.rawSize)
				|| !ReadAt(file, offset + 8, block.storedSize) || !ReadAt(file, offset + 12, block.fileOffset)) {
				error = "block chain runs past the end of the file";
				return false;
			}
			if (block.rawSize == 0) {
				break;
			}
			if (block.fileOffset != offset) {
				error = "block header at " + std::to_string(offset) + " claims offset " + std::to_string(block.fileOffset);
				return false;
			}
			if (block.compressedSize > block.storedSize || file.size() - offset - kBlockHeaderSize < block.storedSize) {
				error = "block at " + std::to_string(offset) + " is truncated";
				return false;
			}

			const std::size_t start = stream.size();
			stream.resize(start + block.rawSize);
			const auto compressed = file.subspan(offset + kBlockHeaderSize, block.compressedSize);
			if (!Oodle::Decompress(compressed, std::span<std::uint8_t>(stream.data() + start, block.rawSize))) {
				error = "Oodle failed on the block at " + std::to_string(offset);
				return false;
			}
			if (blocks) {
				blocks->push_back(block);
			}
			offset += kBlockHeaderSize + block.storedSize;
		}

		if (stream.size() != header.StreamSize()) {
			error = "stream is " + std::to_string(stream.size()) + " bytes, header says " + std::to_string(header.StreamSize());
			return false;
		}
		return true;
	}

	bool WriteFastFile(FastFileHeader header, std::span<const std::uint8_t> stream, std::vector<std::uint8_t>& out,
		std::string& error) {
		header.SetStreamSize(stream.size());
		header.SetFlag(FlagIndex::Compression, kCompressionOodle);
		header.SetFlag(FlagIndex::Encrypted, 0);

		out.clear();
		header.Serialize(out);

		std::vector<std::uint8_t> compressed;
		for (std::size_t pos = 0; pos < stream.size(); pos += kBlockRawSize) {
			const std::size_t rawSize = std::min<std::size_t>(kBlockRawSize, stream.size() - pos);
			if (!Oodle::Compress(stream.subspan(pos, rawSize), compressed)) {
				error = "Oodle failed to compress the block at stream offset " + std::to_string(pos);
				return false;
			}

			BlockInfo block;
			block.compressedSize = static_cast<std::uint32_t>(compressed.size());
			block.rawSize = static_cast<std::uint32_t>(rawSize);
			block.storedSize = static_cast<std::uint32_t>(AlignUp(compressed.size(), 4));
			block.fileOffset = static_cast<std::uint32_t>(out.size());
			Append(out, block.compressedSize);
			Append(out, block.rawSize);
			Append(out, block.storedSize);
			Append(out, block.fileOffset);
			out.insert(out.end(), compressed.begin(), compressed.end());
			out.resize(out.size() + (block.storedSize - block.compressedSize), 0);
		}

		// A zero header ends the chain; the file is then padded to 64 like the shipped ones.
		out.resize(AlignUp(out.size() + kBlockHeaderSize, kFilePadding), 0);
		return true;
	}
}
