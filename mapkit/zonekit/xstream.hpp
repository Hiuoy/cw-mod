#pragma once
#include "fastfile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// The engine's xfile stream state, replayed offline. Every primitive mirrors one engine function, so an
// asset loader written against this class reads the stream exactly the way the game does:
//   Push        DB_PushStreamPos         0x7FF72966EEB0
//   Pop         DB_PopStreamPos          0x7FF72966EE30  (block 0 rewinds, block 1 aligns to 16)
//   Alloc       DB_AllocStreamPos        0x7FF72966EB50
//   Load        DB_LoadXFileData         0x7FF729668D60  (only stored blocks consume stream bytes)
//   LoadString  Load_XStringInline       0x7FF729668E00  (always consumes, up to and with the NUL)
//   Insert      DB_InsertPointer         0x7FF72966ECC0  (8 bytes in block 4, nothing read)
//   Reference   DB_ConvertOffsetToPointer 0x7FF729668D00 / DB_ConvertOffsetToAlias 0x7FF729668CC0
// Positions are offsets from each block's base; the engine starts every block at 0 with block 0 current
// (DB_InitStreams 0x7FF72966EBA0). There is no size or marker anywhere in the stream, so a wrong loader
// desynchronises everything after it: the checks below are what catch that.
namespace MapKit::Zone {
	enum XBlock : int {
		XBlockTemp = 0,     // rewound by every Pop: asset headers are copied out before that
		XBlockRuntime = 2,  // zero-filled, never stored
		XBlockVirtual = 4,  // the default data block; DB_InsertPointer slots live here
		XBlockPhysical = 6,
		XBlockStreamed = 7, // streamed from .xpak at runtime, never stored
		XBlockCollisionAlias = 9, // DB_PushStreamPos(9) stays in the current block (see XStream::Push)
	};

	// DB_LoadXFileData: which blocks are read from the stream.
	constexpr bool XBlockIsStored(int block) {
		return block == 0 || block == 1 || block == 4 || block == 5 || block == 6 || block == 10 || block == 12;
	}

	class XStream {
	public:
		XStream(std::span<const std::uint8_t> stream, std::size_t cursor);

		void Push(int block);
		void Pop();
		std::uint64_t Alloc(std::uint64_t alignment);
		// dst may be null (skip). Returns false on a truncated stream.
		bool Load(void* dst, std::size_t size);
		bool LoadString(std::string* out);
		std::uint64_t Insert();
		// A stored pointer that is neither null nor inline: it must point into data loaded earlier.
		bool Reference(std::uint64_t stored, const char* what);

		// For decoding one asset found by its content instead of walking the stream up to it (see
		// map_entities.hpp): block positions then start at 0 and references to earlier data cannot be
		// checked, so Reference only counts them.
		void Detach() { m_Detached = true; }

		// Records every inline string by where it landed, (block << 60) | position: the value a reference to
		// it stores, minus one. Used to resolve references into data another asset loaded.
		void LogStrings(std::unordered_map<std::uint64_t, std::string>* log) { m_StringLog = log; }
		// Records every DB_InsertPointer slot, (block 4 << 60) | slot, with the stream cursor of the asset root
		// loaded right after it: a later reference to the slot names that asset (its root starts with its name).
		void LogInserts(std::unordered_map<std::uint64_t, std::size_t>* log) { m_InsertLog = log; }
		// Records every Load that consumed stream bytes: where it landed and where its bytes are in the stream.
		// Used to resolve a reference into data another asset stored (zone_trace.hpp ResolveStoredData).
		struct LoadRecord {
			int block = 0;
			std::uint64_t pos = 0;
			std::size_t cursor = 0;
			std::size_t size = 0;
		};
		void LogLoads(std::vector<LoadRecord>* log) { m_LoadLog = log; }
		// Records every asset root stored inline, listed or nested (LoadAssetHeader): where its bytes start in the
		// stream and how many there are. A by-name reference to another zone's asset is a root that holds only its
		// name, top bit set (world_writer.hpp EncodeAssetReference), so the log finds a zone's links by name.
		static constexpr std::uint64_t kNoRootType = ~0ull;
		struct RootRecord {
			std::size_t cursor = 0;
			std::size_t size = 0;
			std::uint64_t type = kNoRootType; // what the loader that read it said (SetRootType)
		};
		void LogRoots(std::vector<RootRecord>* log) { m_RootLog = log; }
		// Each typed loader names its type before it reads its root: a nested root has no type of its own in the stream.
		void SetRootType(std::uint64_t type) { m_RootType = type; }
		std::uint64_t RootType() const { return m_RootType; }
		void NoteRoot(std::size_t size) {
			if (m_RootLog) {
				m_RootLog->push_back({ m_Cursor, size, m_RootType });
			}
			m_RootType = kNoRootType;
		}

		// Block 1 as the lobby preload fills it (flags & 0x6A0: Load_XAsset_Preload 0x7FF71E867350, then
		// DB_FinishPreloadedZone 0x7FF72769F990 at launch; IDB 2026-10-03). Every asset field's handler pushes block 1,
		// puts an inline root there and pops it, which aligns block 1 to 16 (null and linked fields too); a techset or
		// keyvaluepairs handler instead puts its root in the temp block and then saves every block position, 104 B
		// at block 1 (sub_7FF72966EF90), whatever the field held; an inline material-local techset saves them too.
		// Finish replays the zone from those roots and saves, and nothing checks block 1's size: a preload that
		// outgrows the header's block 1 writes into block 2, whose script-string id cache DB_SetZoneScriptStrings
		// zeroes before the replay. A material costs 448 there and the header rule (ExpectedBlockSizes) gives it
		// 352, while a techset costs 104 and the rule gives it 384.
		void PreloadRoot(std::uint64_t type, std::uint64_t alignment, std::size_t size);
		void PreloadFieldEnd(std::uint64_t type);
		void PreloadSave();
		std::uint64_t PreloadBlock1() const { return m_PreloadBlock1High; }
		static bool PreloadInTempBlock(std::uint64_t type) { return type == 0x0F || type == 0x4B; }

		// For decoding one asset from a recorded start (zone_trace.hpp): the stream cursor, the current block
		// and every block's position, as the engine had them. Earlier data counts as loaded up to those
		// positions; block 0 (temp, rewound by every Pop) is not checked. The position stack starts empty:
		// a loader only pops what it pushed.
		void Restore(std::size_t cursor, int block, const std::array<std::uint64_t, kXBlockCount>& positions);
		// Every block's position now (the current block's live one included).
		std::array<std::uint64_t, kXBlockCount> Positions() const;

		bool Fail(std::string message);
		bool Failed() const { return !m_Error.empty(); }
		const std::string& Error() const { return m_Error; }

		std::size_t Cursor() const { return m_Cursor; }
		std::size_t Size() const { return m_Stream.size(); }
		int Block() const { return m_Block; }
		int Depth() const { return m_Depth; }
		// The highest position each block reached: what the zone header's XBlock sizes describe.
		const std::array<std::uint64_t, kXBlockCount>& HighWater() const { return m_HighWater; }
		std::size_t References() const { return m_References; }

		// Every asset root struct loaded inline is copied into block 1 when the asset is linked, 16-aligned.
		// Measured, not yet read in the linker: a techset also takes 208 more bytes there.
		void CountRootCopy(std::size_t size, std::size_t extra = 0) {
			m_RootCopyBytes += ((size + 15) & ~std::size_t(15)) + extra;
		}
		std::size_t RootCopyBytes() const { return m_RootCopyBytes; }

	private:
		struct Frame {
			int block = 0;
			std::uint64_t pos = 0;
		};

		void Advance(std::uint64_t size);
		void Switch(int block);

		std::span<const std::uint8_t> m_Stream;
		std::size_t m_Cursor = 0;
		int m_Block = XBlockTemp;
		std::uint64_t m_Pos = 0;
		std::array<std::uint64_t, kXBlockCount> m_BlockPos{};
		std::array<std::uint64_t, kXBlockCount> m_HighWater{};
		std::array<Frame, 64> m_Stack{};
		int m_Depth = 0;
		std::size_t m_References = 0;
		std::size_t m_RootCopyBytes = 0;
		bool m_Detached = false;
		std::unordered_map<std::uint64_t, std::string>* m_StringLog = nullptr;
		std::unordered_map<std::uint64_t, std::size_t>* m_InsertLog = nullptr;
		std::vector<LoadRecord>* m_LoadLog = nullptr;
		std::vector<RootRecord>* m_RootLog = nullptr;
		std::uint64_t m_RootType = kNoRootType;
		std::uint64_t m_PreloadBlock1 = 0;
		std::uint64_t m_PreloadBlock1High = 0;
		std::string m_Error;
	};
}
