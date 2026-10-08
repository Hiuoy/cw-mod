#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Havok 2019.2 binary tagfiles ("TAG0", SDK 20190200), the format a navmesh's data is stored in (docs/mapkit-plan.md,
// P5). A file is a tree of sections, each with a big-endian u32 header (flags << 30 | size, flags 1 = a leaf) and a
// 4-character tag: TAG0 { SDKV, DATA, TYPE { TPTR, TST1, TNA1, FST1, TBDY, THSH, TPAD }, INDX { ITEM, PTCH } }.
//   DATA  every item's bytes. A pointer or array field holds the index of the item it refers to (as a u64); an
//         array's size and capacity are 0 there, its item gives the count.
//   ITEM  12 B per item: u32 (flags << 24 | type), u32 offset into DATA, u32 count. Item 0 is null; flags 0x10 = an
//         object (what a pointer points at), 0x20 = an array's elements.
//   PTCH  every reference slot in DATA, grouped by the type of the field holding it: {u32 type, u32 count,
//         u32 offsets[count]}, types and offsets ascending.
// Havok's writer numbers the items breadth first from the root: an item's references in field order (for an array
// of records, one field of every element before the next field). An array is laid out as soon as it is numbered,
// an object when its turn in the queue comes; objects are aligned as their type is, arrays to 16.
// mapkit keeps a file's TYPE section as it was, so a file read and written back unchanged is the same byte for
// byte (ffinfo --navmesh checks Die Maschine's).
namespace MapKit::Zone {
	struct HkType {
		std::string name;           // with its template arguments, as Havok spells them
		std::uint32_t parent = 0;
		std::uint32_t format = 0;   // its own, else its parent's
		std::uint32_t size = 0;     // its own, else its parent's
		std::uint32_t alignment = 1;
	};

	// A reference slot inside an item: the 8 bytes at its offset hold another item's index when written.
	struct HkRef {
		std::uint32_t patchType = 0; // the type of the field holding it (a pointer or an array type)
		std::uint32_t target = 0;    // an index into HkTagfile::items; 0 = null
	};

	struct HkItem {
		static constexpr std::uint8_t kObject = 0x10;
		static constexpr std::uint8_t kArray = 0x20;
		std::uint32_t type = 0;
		std::uint8_t flags = 0;
		std::uint32_t count = 0;
		std::vector<std::uint8_t> data;       // count elements of the type's size; reference slots read as 0
		std::map<std::uint32_t, HkRef> refs;  // by offset into data
	};

	struct HkTagfile {
		std::vector<std::uint8_t> sdkVersion;  // SDKV's bytes
		std::vector<std::uint8_t> typeSection; // TYPE as stored, its header included
		std::vector<HkType> types;
		std::vector<HkItem> items;             // [0] null, [1] the root object

		std::uint32_t FindType(std::string_view name) const; // 0 when the file has no such type
		// The item the slot at `offset` of `item` refers to (0: null, or no reference slot there).
		std::uint32_t Target(std::uint32_t item, std::uint32_t offset) const;
		// Points an array slot at a new item of `count` elements of `elementType` (bytes: count times its size), or
		// makes it null when count is 0. A slot that held no reference yet needs its field's type as patchType.
		// False when neither gives the slot a patch type.
		bool SetArray(std::uint32_t item, std::uint32_t offset, std::uint32_t elementType, std::vector<std::uint8_t> bytes,
			std::uint32_t count, std::uint32_t patchType = 0);
		void SetNull(std::uint32_t item, std::uint32_t offset);
	};

	bool ReadHkTagfile(std::span<const std::uint8_t> bytes, HkTagfile& out, std::string& error);
	// Writes the items reachable from item 1; anything else is dropped.
	std::vector<std::uint8_t> WriteHkTagfile(const HkTagfile& file);

	// A navmesh stream key's payload: a 0x100-byte stream header (its first u64 the size of the rest), then the blob
	// the engine reads, {u64 runtime slot, u32 tagfile size, u32 checksum (never read), u64 runtime pointer, u64 0},
	// and the tagfile at +0x120 (Nav_TagfileBlobFromStreamBuffer_cand 0x7FF7267D1500).
	constexpr std::size_t kNavPayloadHeader = 0x120;
	std::vector<std::uint8_t> NavPayload(std::span<const std::uint8_t> tagfile);
	// The tagfile inside a payload; empty when the bytes are not a navmesh payload.
	std::span<const std::uint8_t> NavPayloadTagfile(std::span<const std::uint8_t> payload);
}
