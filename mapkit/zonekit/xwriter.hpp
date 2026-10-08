#pragma once
#include "fastfile.hpp"
#include "xasset_list.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The writing side of XStream: an encoder calls the same primitives, in the same order, as the loader it
// mirrors, and XWriter produces the stream bytes that loader will read back. Only stored blocks put bytes
// in the stream (XBlockIsStored), exactly as DB_LoadXFileData only reads those; a push of block 9 stays in
// the current block. Alloc only moves positions (the stream holds no padding), but positions are kept the
// engine's way so an encoder can make references to data it wrote earlier.
//
// Encoders write every pointer as kPtrInline (data follows), kPtrNull, or a Reference() to earlier data;
// a written zone is then proven by walking it with mapkit's own loaders (zone_writer.hpp).
namespace MapKit::Zone {
	class XWriter {
	public:
		// Assets start with block 4 current (DB_LoadXFile_Internal pushes it before the asset list).
		explicit XWriter(int block = 4);

		void Push(int block);
		void Pop();
		std::uint64_t Alloc(std::uint64_t alignment);
		void Write(const void* data, std::size_t size);
		void WriteString(std::string_view text); // the characters, then a NUL

		template <typename T>
		void Put(const T& value) {
			Write(&value, sizeof(value));
		}

		int Block() const { return m_Block; }
		std::uint64_t Position() const { return m_Pos; }
		// Every block's position (the current block's live one included), and a way to continue from positions
		// worked out elsewhere (an encoder that appends bytes it built itself; see EncodeGfxWorld).
		std::array<std::uint64_t, kXBlockCount> Positions() const {
			auto positions = m_BlockPos;
			positions[m_Block] = m_Pos;
			return positions;
		}
		void SetPositions(const std::array<std::uint64_t, kXBlockCount>& positions) {
			m_BlockPos = positions;
			m_Pos = positions[m_Block];
		}

		// The zone's XAsset array (BuildZoneStream sets it): an asset field that links to an earlier asset of the
		// zone stores a pointer to that entry's header field (+8 of the 16-B entry; DB_ConvertOffsetToAlias).
		// Only assets written with a link name can be found this way.
		void SetAssetEntries(std::uint64_t arrayPos, std::vector<std::pair<std::uint64_t, std::uint64_t>> typeNames) {
			m_AssetArrayPos = arrayPos;
			m_AssetEntries = std::move(typeNames);
		}
		// The asset being written (BuildZoneStream sets it before each encoder). A link to an entry at or after it is a
		// forward link, and broken in the game: the engine turns a link into that entry's header pointer while the
		// linking asset loads (DB_ConvertOffsetToAlias), so the entry must have loaded first. Each is kept to report.
		struct ForwardLink {
			std::size_t from = 0;
			std::size_t to = 0;
			std::uint64_t type = 0;
			std::uint64_t name = 0;
		};
		void SetCurrentAsset(std::size_t index) { m_CurrentAsset = index; }
		const std::vector<ForwardLink>& ForwardLinks() const { return m_ForwardLinks; }
		std::optional<std::uint64_t> AssetEntry(std::uint64_t type, std::uint64_t name) {
			for (std::size_t i = 0; i < m_AssetEntries.size(); ++i) {
				if (m_AssetEntries[i].first == type && m_AssetEntries[i].second == name) {
					if (i >= m_CurrentAsset) {
						m_ForwardLinks.push_back({ m_CurrentAsset, i, type, name });
					}
					return Reference(4, m_AssetArrayPos + 16 * i + 8);
				}
			}
			return std::nullopt;
		}
		// The stored value of a pointer to (block, position): what DB_ConvertOffsetToPointer decodes.
		static std::uint64_t Reference(int block, std::uint64_t position) {
			return ((static_cast<std::uint64_t>(block) << 60) | position) + 1;
		}

		// Appends bytes produced elsewhere (an asset copied from another zone) as if written in the current
		// block. Positions are not advanced: references to anything after it would be wrong, so a copied
		// asset must not be referenced, nor contain references.
		void AppendRaw(std::span<const std::uint8_t> bytes) {
			m_Bytes.insert(m_Bytes.end(), bytes.begin(), bytes.end());
		}

		// Script strings are stored as indices into the zone's own table (DB_ConvertZoneScriptString
		// 0x7FF72966F1E0 resolves them at link time). BuildZoneStream sets the table before any encoder runs;
		// text it does not hold gets 0, the table's null entry.
		void SetScriptStrings(std::unordered_map<std::string, std::uint32_t> indices) { m_ScriptStrings = std::move(indices); }
		std::uint32_t ScriptString(const std::string& text) const {
			const auto found = m_ScriptStrings.find(text);
			return found == m_ScriptStrings.end() ? 0 : found->second;
		}

		std::vector<std::uint8_t>& Bytes() { return m_Bytes; }
		const std::vector<std::uint8_t>& Bytes() const { return m_Bytes; }

	private:
		struct Frame {
			int block = 0;
			std::uint64_t pos = 0;
		};
		void Switch(int block);

		std::vector<std::uint8_t> m_Bytes;
		int m_Block = 0;
		std::uint64_t m_Pos = 0;
		std::array<std::uint64_t, kXBlockCount> m_BlockPos{};
		std::vector<Frame> m_Stack;
		std::unordered_map<std::string, std::uint32_t> m_ScriptStrings;
		std::uint64_t m_AssetArrayPos = 0;
		std::vector<std::pair<std::uint64_t, std::uint64_t>> m_AssetEntries; // {type, link name}, 0 = none
		std::size_t m_CurrentAsset = SIZE_MAX; // none set: nothing counts as a forward link
		std::vector<ForwardLink> m_ForwardLinks;
	};

	// Little helpers for building a root struct in memory before writing it.
	template <typename T>
	void PutAt(std::span<std::uint8_t> data, std::size_t offset, const T& value) {
		std::memcpy(data.data() + offset, &value, sizeof(value));
	}
}
