#pragma once
#include "asset_loaders.hpp"
#include "xasset_list.hpp"
#include "xstream.hpp"
#include "xwriter.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Copying a retail asset byte for byte (docs/mapkit-plan.md, P6). A recording reader walks the asset the way its
// Load_<Type> does and notes where in the stream every reference, script-string index and sub-array sits; the
// splice writer then copies the asset's stream with those re-pointed and chosen sub-arrays cut out. Nothing is
// modelled field by field, so a copy is exact wherever it is not edited. First written for the gfx_map
// (gfx_world.hpp); the level's other world assets use it too (level_assets.hpp).
namespace MapKit::Zone {
	constexpr std::size_t kNoStream = static_cast<std::size_t>(-1);

	// A stored pointer that points at data loaded earlier. An asset field (DB_ConvertOffsetToAlias) points at
	// the header field of an entry of the zone's XAsset array (+8 of the 16-B entry): the asset it links to.
	struct RecordRef {
		std::size_t at = 0;        // stream offset of the stored 8-byte value
		std::uint64_t stored = 0;
		std::uint64_t assetType = ~0ull; // the field's asset type, or ~0 for a pointer to data
	};

	// A zone-local script-string index (DB_ResolveScriptStringIndex 0x7FF72966F0E0 converts it at load).
	struct RecordString {
		std::size_t at = 0;
		std::uint32_t index = 0;
	};

	// One DB_LoadXFileData of the asset, in load order: where the data landed and where its bytes are.
	struct RecordChunk {
		int block = 0;
		std::uint64_t pos = 0;
		std::uint64_t size = 0;
		std::size_t at = kNoStream; // kNoStream in a block the stream does not store (2, 3)
	};

	// A pointer field whose data follows inline: `field` is where the pointer is stored, [begin, end) the
	// stream bytes of its data and everything nested in it, [firstChunk, endChunk) the chunks.
	struct RecordSubtree {
		std::string name;
		std::size_t field = 0;
		std::size_t begin = 0;
		std::size_t end = 0;
		std::size_t firstChunk = 0;
		std::size_t endChunk = 0;
	};

	// What a recording reader found in one asset.
	struct AssetRecord {
		std::size_t rootAt = kNoStream;
		std::size_t begin = 0; // the asset's stream range (its Load_<Type>Asset's own data)
		std::size_t end = 0;
		std::array<std::uint64_t, kXBlockCount> startPositions{};
		int startBlock = 0;
		std::vector<RecordRef> refs;
		std::vector<RecordString> strings;
		std::vector<RecordChunk> chunks;
		std::vector<RecordSubtree> subtrees;
		std::size_t inlineAssets = 0; // nested assets stored inline (their own references are not recorded)
		std::vector<std::size_t> inlineAssetFields; // where each of them is stored (kNoStream: unknown)

		const RecordSubtree* Subtree(std::string_view subtreeName) const;
	};

	// What a reference of a recorded asset points at, given the zone's XAsset array (block 4, 16 B per entry;
	// it ends where the first asset starts).
	struct RecordTarget {
		enum Kind { Asset, Internal, External } kind = External;
		std::size_t asset = 0;       // Asset: index into the zone's XAsset array
		std::size_t chunk = 0;       // Internal: the chunk of this asset it lands in
		std::uint64_t offset = 0;    // Internal: offset into that chunk
	};
	RecordTarget ResolveRecordRef(const AssetRecord& record, const RecordRef& ref, std::uint64_t assetArrayPos,
		std::size_t assetCount);

	// A sub-array to leave out of the copy: the subtree named `name` (its pointer is written null) and the
	// count fields that size it, zeroed (runtime code may walk a count whatever the pointer says).
	struct RecordCut {
		std::string name;
		std::vector<std::pair<std::size_t, std::size_t>> counts; // {root offset, bytes}
	};

	// Where a copy's data landed in the zone being written: one of the retail asset's chunks, {block, position, size} in
	// the retail zone, and its block and position now.
	struct RecordLanding {
		int block = 0;
		std::uint64_t pos = 0;
		std::uint64_t size = 0;
		int newBlock = 0;
		std::uint64_t newPos = 0;
	};

	// Everything the splice writer needs besides the retail asset: how its links and strings re-point in the
	// zone being written.
	struct RecordSplice {
		std::span<const std::uint8_t> stream;      // the retail zone's stream
		const AssetRecord* record = nullptr;        // read from it
		std::uint64_t assetArrayPos = 0;            // the retail XAsset array's block-4 position
		std::span<const XAssetEntry> assets;        // the retail XAsset array
		std::span<const std::uint64_t> assetNames;  // the name hash of each (top bit cleared)
		std::span<const std::optional<std::string>> strings; // the retail script-string table
		std::vector<RecordCut> cuts;
		std::vector<std::pair<std::size_t, std::uint64_t>> rootEdits; // {root offset, u64}, applied after the cuts
		// Bytes written over the copy's own, after the root edits: {the retail stream offset they replace, the bytes}. For
		// fields that size nothing and point nowhere (a light's colour); one inside a cut sub-array goes with it.
		std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> edits;
		// Every asset link of `type` inside the named sub-array goes to `name` instead (the count and every index
		// stay as they were: the entries now draw another model, say).
		struct Relink {
			std::string subtree;
			std::uint64_t type = 0;
			std::uint64_t name = 0;
		};
		std::vector<Relink> relinks;
		// Data shared with another asset: a fastfile stores identical data once, and a later asset points into the data
		// of the one that loaded it first (most retail materials, meshes, skeletons and models do). When set, the copy
		// re-points such a pointer (its stored value) to where that data lands in the zone being written, which must
		// hold a copy of the owning asset, written earlier; nullopt when it does not.
		std::function<std::optional<std::uint64_t>(std::uint64_t stored)> shared;
		// When set, the encoder appends where each of the copy's chunks landed (for the copies written after it).
		std::vector<RecordLanding>* landings = nullptr;
	};

	// The assets a copy links to, by type and name: the zone being written must hold each one before the copy
	// (a by-name reference is enough; EncodeAssetReference), found through XWriter::AssetEntry.
	struct RecordLink {
		std::uint64_t type = 0;
		std::uint64_t name = 0;
		bool operator==(const RecordLink&) const = default;
	};
	// Fails (with a reason) on a reference the copy cannot carry: a nested asset stored inline, or data shared with
	// another asset. Given `shared`, it lists the stored value of each pointer into another asset's data there instead
	// of failing (the caller copies the owners first, and sets RecordSplice::shared).
	bool RecordLinks(const RecordSplice& splice, std::vector<RecordLink>& links, std::vector<std::string>& strings,
		std::string& error, std::vector<std::uint64_t>* shared = nullptr);

	// Reads one asset of the type (the stream positioned at its start) into the record: Load_<Type>Asset(0, &header).
	using RecordReader = std::function<bool(XStream& s, std::uint64_t header, AssetRecord& out)>;

	// Writes the copy at the writer's position, as Load_<Type>Asset(0, &header) with header inline reads it: the
	// retail bytes with the cuts taken out, every asset link re-pointed into the writer's XAsset array, every script
	// string into its table, and every pointer into the asset itself moved to where its data now lands. `read` is the
	// type's recording reader; the copy's root must be stored at the asset's start. The writer's positions then
	// continue after it.
	bool EncodeRecordedAsset(XWriter& w, const RecordSplice& splice, const RecordReader& read, std::string& error);

	// The recording walk: the pieces a recording reader is written with. Each mirrors the engine's generated
	// loader code (addresses in the comments where a reader uses them).
	namespace Record {
		// A loaded piece of data and where its bytes sit in the stream.
		struct Chunk {
			std::vector<std::uint8_t> data;
			std::size_t at = kNoStream;
		};

		// A view into a chunk: an embedded struct, or one entry of an array.
		struct View {
			const Chunk* chunk = nullptr;
			std::size_t base = 0;

			template <typename T>
			T Get(std::size_t offset) const {
				return chunk ? MapKit::Zone::Get<T>(chunk->data, base + offset) : T{};
			}
			std::uint64_t P(std::size_t offset) const { return Get<std::uint64_t>(offset); }
			std::uint32_t U32(std::size_t offset) const { return Get<std::uint32_t>(offset); }
			std::int32_t I32(std::size_t offset) const { return Get<std::int32_t>(offset); }
			std::uint16_t U16(std::size_t offset) const { return Get<std::uint16_t>(offset); }
			std::uint8_t U8(std::size_t offset) const { return Get<std::uint8_t>(offset); }
			std::size_t At(std::size_t offset) const {
				return chunk && chunk->at != kNoStream ? chunk->at + base + offset : kNoStream;
			}
			View Sub(std::size_t offset) const { return { chunk, base + offset }; }
		};

		inline View Entry(const Chunk& array, std::size_t index, std::size_t stride) {
			return { &array, index * stride };
		}

		inline bool IsReference(std::uint64_t v) {
			return v != kPtrNull && v != kPtrInline && v != kPtrInsert;
		}

		// A count read as the engine reads it before multiplying: negative i32 counts load nothing.
		inline std::uint64_t Count(std::int32_t n) {
			return static_cast<std::uint64_t>(std::max(n, 0));
		}

		struct Ctx {
			XStream& s;
			AssetRecord* out = nullptr;

			// DB_AllocStreamPos + DB_LoadXFileData.
			Chunk Load(std::size_t size, std::uint64_t alignment) {
				Chunk chunk;
				const std::uint64_t pos = s.Alloc(alignment);
				if (XBlockIsStored(s.Block())) {
					chunk.at = s.Cursor();
				}
				if (out) {
					out->chunks.push_back({ s.Block(), pos, size, chunk.at });
				}
				chunk.data.resize(size);
				s.Load(chunk.data.data(), size);
				return chunk;
			}

			// A stored pointer to data loaded earlier (DB_ConvertOffsetToPointer / DB_OffsetToPointerRaw).
			void Ref(View v, std::size_t offset, const char* what) {
				const std::uint64_t stored = v.P(offset);
				if (!IsReference(stored)) {
					return;
				}
				s.Reference(stored, what);
				if (out) {
					out->refs.push_back({ v.At(offset), stored });
				}
			}

			// A nested asset field, read as Load_<Type>Asset(0, &field): pushes the temp block, links by alias or
			// loads inline.
			void Asset(View v, std::size_t offset, std::uint64_t type, const char* what) {
				const std::uint64_t stored = v.P(offset);
				if (out) {
					if (IsReference(stored)) {
						out->refs.push_back({ v.At(offset), stored, type });
					}
					else if (stored != kPtrNull) {
						++out->inlineAssets;
						out->inlineAssetFields.push_back(v.At(offset));
					}
				}
				LoadAsset(s, type, stored, what);
			}

			// A nested asset field of a type whose data points nowhere (a streamkey: Load_StreamkeyAsset
			// 0x7FF71E7ECFC0 loads its data by size alone). Stored inline, it travels in the copy's bytes as it is.
			void PlainAsset(View v, std::size_t offset, std::uint64_t type, const char* what) {
				const std::uint64_t stored = v.P(offset);
				if (out && IsReference(stored)) {
					out->refs.push_back({ v.At(offset), stored, type });
				}
				LoadAsset(s, type, stored, what);
			}

			// A nested asset field read as Load_<Type>Asset(0, &field), for a type recorded with the asset that holds it
			// (the sound bank's sound assets, ducks and acoustics: library_assets.cpp). Stored inline (-1, or -2 after a
			// DB_InsertPointer slot), its root goes to the temp block and body(root) reads the rest, all recorded like the
			// asset's own data. The slot is recorded too, as 8 B of block 4 with no stream bytes, so a later field of the
			// asset that links this one through the slot follows it into the copy. Any other non-null value is a link: to
			// an entry of the zone's XAsset array, or to such a slot.
			template <typename F>
			void Nested(View v, std::size_t offset, std::uint64_t type, std::size_t rootSize, std::uint64_t alignment,
				const char* what, F&& body) {
				const std::uint64_t stored = v.P(offset);
				s.Push(XBlockTemp);
				if (stored == kPtrInline || stored == kPtrInsert) {
					if (stored == kPtrInsert) {
						const std::uint64_t slot = s.Insert();
						if (out) {
							out->chunks.push_back({ XBlockVirtual, slot, 8, kNoStream });
						}
					}
					s.Alloc(alignment);
					s.CountRootCopy(rootSize);
					s.PreloadRoot(type, alignment, rootSize);
					s.SetRootType(type);
					s.NoteRoot(rootSize);
					const Chunk root = Load(rootSize, alignment);
					if (!s.Failed()) {
						body(root);
					}
				}
				else if (stored != kPtrNull) {
					s.Reference(stored, what);
					if (out) {
						out->refs.push_back({ v.At(offset), stored, type });
					}
				}
				s.Pop();
				s.PreloadFieldEnd(type);
			}

			// A nested image field (Load_GfxImage 0x7FF71E7D95E0 under the caller's push of block 0). Stored inline, its
			// root (208 B) and data are recorded like the asset's own (ImageBody).
			void Image(View v, std::size_t offset, const char* what) {
				const std::uint64_t stored = v.P(offset);
				if (stored != kPtrInline && stored != kPtrInsert) {
					Asset(v, offset, 0x10, what);
					return;
				}
				s.Push(XBlockTemp);
				if (stored == kPtrInsert) {
					s.Insert();
				}
				s.Alloc(8);
				s.CountRootCopy(208);
				s.PreloadRoot(0x10, 8, 208);
				s.SetRootType(0x10);
				s.NoteRoot(208);
				const Chunk root = Load(208, 8);
				ImageBody(View{ &root, 0 });
				s.Pop();
				s.PreloadFieldEnd(0x10);
			}

			// What Load_GfxImage loads after an image's 208-B root, under a push of block 4: +40 pixels
			// (Load_GfxImagePixels 0x7FF71E7D96C0: -1 or a reference; u32 @152 bytes in block 6, or in block 7 when +144 &
			// 0x10 says streamed), +48 mips (32 B x u8 @184, loaded when non-null), +8 data (-1 or a reference; as many
			// bytes as the first mip's size field says, Image_GetMipDataSize 0x7FF729221E20). asset_loaders.cpp
			// LoadImageBody reads the same.
			void ImageBody(const View& r) {
				s.Push(XBlockVirtual);
				const bool streamed = r.U32(144) & 0x10;
				s.Push(streamed ? XBlockStreamed : XBlockPhysical);
				Inline(r, 40, r.U32(152), 1, streamed ? 0x10000 : 256, "image pixels");
				s.Pop();
				std::uint64_t dataSize = 0;
				Array(r, 48, r.U8(184), 32, 8, "image mips", [&](const Chunk& mips) {
					dataSize = (MapKit::Zone::Get<std::uint64_t>(mips.data, 24) >> 4) & 0x1FFFFFFF;
				});
				Inline(r, 8, dataSize, 1, 256, "image data");
				s.Pop();
			}

			void String(View v, std::size_t offset) {
				if (out) {
					out->strings.push_back({ v.At(offset), v.U32(offset) });
				}
			}

			// An XString field (Load_XStringInline 0x7FF729668E00 after DB_AllocStreamPos(1)): -1 is the text inline, up
			// to and with its NUL (recorded like any other data, since later fields may point into it), anything else a
			// pointer to text loaded earlier.
			void XString(View v, std::size_t offset, const char* what) {
				const std::uint64_t stored = v.P(offset);
				if (stored == kPtrInline && !s.Failed()) {
					const std::uint64_t pos = s.Alloc(1);
					const std::size_t at = XBlockIsStored(s.Block()) ? s.Cursor() : kNoStream;
					std::string text;
					s.LoadString(&text);
					if (out) {
						out->chunks.push_back({ s.Block(), pos, text.size() + 1, at });
					}
				}
				else if (stored != kPtrNull) {
					Ref(v, offset, what);
				}
			}

			// The data of a pointer field and everything nested in it, recorded as one subtree.
			template <typename F>
			void Subtree(View v, std::size_t offset, const char* name, F&& body) {
				const std::size_t begin = s.Cursor();
				const std::size_t firstChunk = out ? out->chunks.size() : 0;
				body();
				if (out) {
					out->subtrees.push_back({ name, v.At(offset), begin, s.Cursor(), firstChunk, out->chunks.size() });
				}
			}

			// `if (field) { Alloc; Load(count * stride); children }`: the generated code only tests for non-null.
			template <typename F>
			void Array(View v, std::size_t offset, std::uint64_t count, std::size_t stride, std::uint64_t alignment,
				const char* name, F&& children) {
				if (!v.P(offset) || s.Failed()) {
					return;
				}
				Subtree(v, offset, name, [&] {
					const Chunk data = Load(static_cast<std::size_t>(count * stride), alignment);
					children(data);
				});
			}
			void Array(View v, std::size_t offset, std::uint64_t count, std::size_t stride, std::uint64_t alignment,
				const char* name) {
				Array(v, offset, count, stride, alignment, name, [](const Chunk&) {});
			}

			// `-1: Alloc; Load; children / other non-null: a reference`.
			template <typename F>
			void Inline(View v, std::size_t offset, std::uint64_t count, std::size_t stride, std::uint64_t alignment,
				const char* name, F&& children) {
				const std::uint64_t stored = v.P(offset);
				if (stored == kPtrInline && !s.Failed()) {
					Subtree(v, offset, name, [&] {
						const Chunk data = Load(static_cast<std::size_t>(count * stride), alignment);
						children(data);
					});
				}
				else if (stored != kPtrNull) {
					Ref(v, offset, name);
				}
			}
			void Inline(View v, std::size_t offset, std::uint64_t count, std::size_t stride, std::uint64_t alignment,
				const char* name) {
				Inline(v, offset, count, stride, alignment, name, [](const Chunk&) {});
			}

			// A block-2/3 array under a push of that block (nothing stored).
			void Runtime(int block, View v, std::size_t offset, std::uint64_t count, std::size_t stride,
				std::uint64_t alignment, const char* name) {
				s.Push(block);
				Array(v, offset, count, stride, alignment, name);
				s.Pop();
			}

			// An array of asset pointers (Load_MaterialHandleArray 0x7FF71E7E0A10, Load_XModelPtrArray 0x7FF71E7F8E00,
			// sub_7FF71E7F8CA0, Load_GfxImagePtrArray 0x7FF71E7D9850): 8 B each, every one a nested asset.
			void AssetArray(const Chunk& array, std::size_t count, std::uint64_t type, const char* what) {
				for (std::size_t i = 0; i < count && !s.Failed(); ++i) {
					Asset(Entry(array, i, 8), 0, type, what);
				}
			}
		};

		// Load_GfxPixels56 0x7FF71E7DA0D0 (56 B): +0 block-6 data, u32 @8 x u32 @12 bytes (a texture's pixels;
		// sub_7FF7291E1E60 makes the GPU resource from it).
		inline void Pixels56(Ctx& c, View owner, std::size_t offset, const char* name) {
			c.Inline(owner, offset, 1, 56, 16, name, [&](const Chunk& head) {
				const View h{ &head, 0 };
				c.s.Push(XBlockPhysical);
				c.Inline(h, 0, static_cast<std::uint64_t>(h.U32(8)) * h.U32(12), 1, 256, "pixels");
				c.s.Pop();
			});
		}

		// Load_GfxBlob3 0x7FF71E7D8D80 (48 B, embedded): three {u64 n, pointer} blobs, each -1 checked (else a raw
		// reference): n bytes, n x u32, n x u32.
		inline void Blob3(Ctx& c, View b, const char* name) {
			c.Inline(b, 8, b.P(0), 1, 1, name);
			c.Inline(b, 24, b.P(16), 4, 4, name);
			c.Inline(b, 40, b.P(32), 4, 4, name);
		}

		// Load_<Type>Asset(0, &header) for a recording reader whose root is `rootSize` bytes: the root in the temp
		// block (the engine copies it into the asset pool), then body(root) with the root's view. Records the
		// asset's range, start and root.
		template <typename Body>
		bool ReadRoot(XStream& s, std::uint64_t header, AssetRecord* out, std::size_t rootSize, std::uint64_t alignment,
			const char* what, Body&& body) {
			Ctx c{ s, out };
			if (out) {
				*out = {};
				out->begin = s.Cursor();
				out->startPositions = s.Positions();
				out->startBlock = s.Block();
			}
			const std::uint64_t type = s.RootType();
			s.Push(XBlockTemp);
			if (header == kPtrInline || header == kPtrInsert) {
				if (header == kPtrInsert) {
					s.Insert();
				}
				// As LoadAssetHeader: the root is logged where it starts (the second Alloc, in Load, is then a no-op).
				s.Alloc(alignment);
				s.CountRootCopy(rootSize);
				s.PreloadRoot(type, alignment, rootSize);
				s.NoteRoot(rootSize);
				Chunk root = c.Load(rootSize, alignment);
				if (!s.Failed()) {
					if (out) {
						out->rootAt = root.at;
					}
					body(c, root);
				}
			}
			else if (header != kPtrNull) {
				s.Reference(header, what);
			}
			s.Pop();
			s.PreloadFieldEnd(type);
			s.SetRootType(XStream::kNoRootType);
			if (out) {
				out->end = s.Cursor();
			}
			return !s.Failed();
		}
	}
}
