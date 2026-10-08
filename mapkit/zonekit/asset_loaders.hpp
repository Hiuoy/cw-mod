#pragma once
#include "xasset_list.hpp"
#include "xstream.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

// Building blocks for the per-type loaders (asset_loaders.cpp). The game's loaders are generated code
// with a handful of shapes; each helper below is one of them.
namespace MapKit::Zone {
	// A loader gets the header pointer from the XAsset array and consumes the asset's data.
	using AssetLoader = bool (*)(XStream& s, std::uint64_t header);
	AssetLoader FindAssetLoader(std::uint64_t type);

	template <typename T>
	T Get(std::span<const std::uint8_t> data, std::size_t offset) {
		T value{};
		if (offset + sizeof(T) <= data.size()) {
			std::memcpy(&value, data.data() + offset, sizeof(T));
		}
		return value;
	}

	// An XString field: -1 = Alloc(1) + the inline string, 0 = null, anything else a reference.
	inline bool LoadXString(XStream& s, std::uint64_t pointer, const char* what, std::string* out = nullptr) {
		if (pointer == kPtrInline) {
			s.Alloc(1);
			return s.LoadString(out);
		}
		return pointer == kPtrNull || s.Reference(pointer, what);
	}

	// An array field loaded in place: Alloc(alignment) + count * stride bytes, returned for the caller to
	// walk its pointers. Non-null is enough: the generated code does not check for -1 here.
	inline std::vector<std::uint8_t> LoadArray(XStream& s, std::uint64_t pointer, std::size_t count, std::size_t stride,
		std::uint64_t alignment) {
		std::vector<std::uint8_t> data;
		if (pointer) {
			s.Alloc(alignment);
			data.resize(count * stride);
			s.Load(data.data(), data.size());
		}
		return data;
	}

	// A pointer field the generated code checks for -1: Alloc(alignment) + size bytes when inline (kept in
	// *out if given), a reference to earlier data when neither null nor inline. True when it loaded.
	inline bool LoadInline(XStream& s, std::uint64_t pointer, std::size_t size, std::uint64_t alignment, const char* what,
		std::vector<std::uint8_t>* out = nullptr) {
		if (pointer == kPtrInline) {
			s.Alloc(alignment);
			if (out) {
				out->assign(size, 0);
				return s.Load(out->data(), size);
			}
			return s.Load(nullptr, size);
		}
		if (pointer != kPtrNull) {
			s.Reference(pointer, what);
		}
		return false;
	}

	// A field pointing at another asset, loaded with that type's Load_<Type>Asset(0, &field): the same call
	// the asset list makes, so the type's own loader handles it. A type mapkit has no loader for passes as
	// long as the field is null or a reference, and fails the stream if the asset is stored inline.
	bool LoadAsset(XStream& s, std::uint64_t type, std::uint64_t pointer, const char* what);
	// A type LoadAsset reads only as a by-name reference (it has no loader of its own).
	bool IsReferenceRootType(std::uint64_t type);

	// An image (GfxImage, 208 B; layout at LoadImageBody in asset_loaders.cpp) with what it stored inline.
	struct ImageData {
		std::array<std::uint8_t, 208> root{};
		std::vector<std::uint8_t> mips;   // 32 B each
		std::vector<std::uint8_t> pixels; // +40, when resident and stored here (block 6)
		std::vector<std::uint8_t> data;   // +8, when stored here
		std::uint64_t Name() const;
		std::uint32_t Flags() const;  // +144; 0x10 = streamed
		std::uint32_t Format() const; // +156, a DXGI_FORMAT
		std::uint16_t Width() const;
		std::uint16_t Height() const;
		std::uint8_t MipCount() const; // +184
	};
	// Decodes one image asset (the stream positioned at its start) into out.
	bool ReadImage(XStream& s, std::uint64_t header, ImageData& out);

	// A keyvaluepairs asset (0x4B): its name and its pairs, each a key hash (BuildKv_KeyHash_cand) and a string. A value
	// stored by an earlier asset (a pointer into its data) reads as "" with `shared` set.
	struct KeyValuePairsData {
		struct Pair {
			std::uint32_t key = 0;
			std::string value;
			bool shared = false;
		};
		std::uint64_t name = 0;
		std::vector<Pair> pairs;
	};
	// Decodes one keyvaluepairs asset (the stream positioned at its start) into out.
	bool ReadKeyValuePairs(XStream& s, std::uint64_t header, KeyValuePairsData& out);

	// The body of a techset (Load_TechsetAsset's inner 0x7FF71E7E1750): 17 techniques and the name block.
	// Materials also embed one without linking it as an asset.
	void LoadTechsetBody(XStream& s, std::span<const std::uint8_t> techset);

	// Load_<Type>Asset(0, &header): the asset's root struct goes to the temp block (the engine copies it
	// into the asset pool); -2 first reserves a DB_InsertPointer slot. body() gets the loaded struct.
	// The lobby preload's twin of each Load_<Type>Asset lays its root out in block 1 instead (XStream::PreloadRoot).
	template <typename Body>
	bool LoadAssetHeader(XStream& s, std::uint64_t header, std::size_t size, std::uint64_t alignment, Body&& body,
		std::size_t linkExtra = 0) {
		const std::uint64_t type = s.RootType();
		s.Push(XBlockTemp);
		if (header == kPtrInline || header == kPtrInsert) {
			if (header == kPtrInsert) {
				s.Insert();
			}
			s.Alloc(alignment);
			s.CountRootCopy(size, linkExtra);
			s.PreloadRoot(type, alignment, size);
			s.NoteRoot(size);
			std::vector<std::uint8_t> data(size);
			if (s.Load(data.data(), data.size())) {
				body(std::span<std::uint8_t>(data));
			}
		}
		else if (header != kPtrNull) {
			s.Reference(header, "asset header");
		}
		s.Pop();
		s.PreloadFieldEnd(type);
		s.SetRootType(XStream::kNoRootType);
		return !s.Failed();
	}
}
