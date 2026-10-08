// Per-type loaders, each transcribed from the game's Load_<Type>Asset and the struct loaders it calls
// (addresses in the comments; IDB bocw_fixed_renamed.i64, build 1.34.0.15931218). Field offsets are
// named only where the loader itself gives them a meaning.
#include "asset_loaders.hpp"
#include "gameplay_assets.hpp"
#include "gfx_world.hpp"
#include "level_assets.hpp"
#include "library_assets.hpp"
#include "asset_walk.hpp"
#include "map_entities.hpp"
#include "model_assets.hpp"
#include "world_assets.hpp"
#include "world_writer.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace MapKit::Zone {
	namespace {
		// --- keyvaluepairs (0x4B): Load_KeyvaluepairsAsset 0x7FF71E7DD6A0 -> 0x7FF71E7DD5D0 -------------
		// struct KeyValuePairs (24 B) { u64 name; i32 count; KeyValuePair* pairs @16; }
		// struct KeyValuePair (16 B) { u32 key (BuildKv_KeyHash_cand); XString value @8; }
		bool LoadKeyValuePairsData(XStream& s, std::uint64_t header, KeyValuePairsData* out) {
			return LoadAssetHeader(s, header, 24, 8, [&](std::span<std::uint8_t> kvp) {
				s.Push(XBlockVirtual);
				const auto pairs = LoadArray(s, Get<std::uint64_t>(kvp, 16), Get<std::int32_t>(kvp, 8), 16, 8);
				if (out) {
					out->name = Get<std::uint64_t>(kvp, 0);
				}
				for (std::size_t i = 0; i + 16 <= pairs.size(); i += 16) {
					const auto value = Get<std::uint64_t>(pairs, i + 8);
					KeyValuePairsData::Pair pair{ Get<std::uint32_t>(pairs, i), {}, value != kPtrInline && value != kPtrNull };
					LoadXString(s, value, "keyvaluepair value", out ? &pair.value : nullptr);
					if (out) {
						out->pairs.push_back(std::move(pair));
					}
				}
				s.Pop();
			});
		}

		bool LoadKeyValuePairs(XStream& s, std::uint64_t header) {
			return LoadKeyValuePairsData(s, header, nullptr);
		}

		// --- bgcache (0x6D): Load_BgcacheAsset 0x7FF71E7CA8C0 --------------------------------------------
		// struct BgCache (24 B) { u64 name; BgCacheEntry* entries @8; i32 count @16; }
		// struct BgCacheEntry (24 B) { u8 table; u64 name @8; u64 slot @16 (filled at level load); }
		// world_writer.hpp (EncodeBgCache) has what the engine does with it.
		bool LoadBgCache(XStream& s, std::uint64_t header) {
			return LoadAssetHeader(s, header, 24, 8, [&](std::span<std::uint8_t> root) {
				s.Push(XBlockVirtual);
				LoadArray(s, Get<std::uint64_t>(root, 8), static_cast<std::size_t>(std::max(Get<std::int32_t>(root, 16), 0)), 24, 8);
				s.Pop();
			});
		}

		// --- image (0x10): Load_ImageAsset 0x7FF71E7D97A0 -> 0x7FF71E7D95E0 -----------------------------
		// struct GfxImage (208 B): +8 data, +40 pixels, +48 mips (32 B x mipCount @184), +144 flags,
		// +152 pixel size. Flag 0x10 = streamed: the pixels go to block 7 (never stored), else block 6.
		// The +8 data size is mip 0's size field, (mips[0] @24 >> 4) & 0x1FFFFFFF (0x7FF729221E20).
		// Field names beyond the loader's (Greyhound's BOCWGfxImage / BOCWGfxMip): +156 u32 DXGI format, +160 u16
		// width, +162 u16 height, +164 u8 mip levels; a mip is {u64 package key, 16 B, u32 size @24, u16 width, height}.
		void LoadImageBody(XStream& s, std::span<const std::uint8_t> image, ImageData* out) {
			s.Push(XBlockVirtual);

			// 0x7FF71E7D96C0. For a streamed image it pushes block 7 only if sub_7FF72686C7C0 says so;
			// block 7 is never stored, so that choice cannot change what is read.
			const bool streamed = Get<std::uint32_t>(image, 144) & 0x10;
			const auto pixels = Get<std::uint64_t>(image, 40);
			s.Push(streamed ? XBlockStreamed : XBlockPhysical);
			if (pixels == kPtrInline) {
				s.Alloc(streamed ? 0x10000 : 256);
				std::vector<std::uint8_t> bytes(Get<std::uint32_t>(image, 152));
				s.Load(bytes.data(), bytes.size());
				if (out && !streamed) {
					out->pixels = std::move(bytes);
				}
			}
			else if (pixels != kPtrNull) {
				s.Reference(pixels, "image pixels");
			}
			s.Pop();

			const auto mips = LoadArray(s, Get<std::uint64_t>(image, 48), image[184], 32, 8);

			const auto data = Get<std::uint64_t>(image, 8);
			if (data == kPtrInline) {
				s.Alloc(256);
				std::vector<std::uint8_t> bytes((Get<std::uint64_t>(mips, 24) >> 4) & 0x1FFFFFFF);
				s.Load(bytes.data(), bytes.size());
				if (out) {
					out->data = std::move(bytes);
				}
			}
			else if (data != kPtrNull) {
				s.Reference(data, "image data");
			}
			s.Pop();
			if (out) {
				out->mips = mips;
			}
		}

		bool LoadImage(XStream& s, std::uint64_t header) {
			s.SetRootType(0x10);
			return LoadAssetHeader(s, header, 208, 8, [&](std::span<std::uint8_t> image) { LoadImageBody(s, image, nullptr); });
		}

		// Many structs end in runs of "if (ptr) { Alloc(align); Load(count * stride); }" with the count an i32
		// or u32 right after the pointer. One call per such field, in the loader's order.
		void LoadCountedArray(XStream& s, std::span<const std::uint8_t> owner, std::size_t pointerOffset,
			std::size_t countOffset, std::size_t stride, std::uint64_t alignment) {
			if (Get<std::uint64_t>(owner, pointerOffset)) {
				s.Alloc(alignment);
				s.Load(nullptr, static_cast<std::size_t>(Get<std::uint32_t>(owner, countOffset)) * stride);
			}
		}

		// --- streamkey (0xB8): Load_StreamkeyAsset 0x7FF71E7ECFC0 -> 0x7FF71E7ECF00 ----------------------
		// struct StreamKey (56 B): +32 data, +48 u32 size, +55 u8 flags. The flags pick where the data goes:
		//   & 3 == 0         block 7 (streamed; only if sub_7FF72686C7C0, never stored either way)
		//   & 1, not & 0x22  block 8 (never stored)
		//   & 2              inline in the current block (block 4): the only case that reads the stream
		//   & 1 and & 0x20   block 8, 64 KB aligned (never stored)
		bool LoadStreamKey(XStream& s, std::uint64_t header) {
			s.SetRootType(0xB8);
			return LoadAssetHeader(s, header, 56, 8, [&](std::span<std::uint8_t> key) {
				s.Push(XBlockVirtual);
				const std::uint8_t flags = key[55];
				const auto data = Get<std::uint64_t>(key, 32);
				const auto size = Get<std::uint32_t>(key, 48);
				auto toBlock = [&](int block, std::uint64_t alignment) {
					s.Push(block);
					if (data) {
						s.Alloc(alignment);
						s.Load(nullptr, size);
					}
					s.Pop();
				};
				if ((flags & 3) == 0) {
					toBlock(XBlockStreamed, 256);
				}
				else if ((flags & 1) && !(flags & 0x22)) {
					toBlock(8, 256);
				}
				else if (flags & 2) {
					if (data) {
						s.Alloc(256);
						s.Load(nullptr, size);
					}
				}
				else if (flags & 0x20) {
					toBlock(8, 0x10000);
				}
				s.Pop();
			});
		}

		// --- navmesh (0x75): Load_NavMeshData 0x7FF71E7E1EE0 ----------------------------------------------
		// struct NavMeshData (104 B): +8 streamkey (the shared tagfile), +32 i32 cell count, +40 cells (64 B each, +40
		// the cell's streamkey), +72 streamkey (debug data); everything after the root is in block 4 (navmesh.hpp).
		bool LoadNavMesh(XStream& s, std::uint64_t header) {
			s.SetRootType(0x75);
			return LoadAssetHeader(s, header, 104, 8, [&](std::span<std::uint8_t> root) {
				s.Push(XBlockVirtual);
				LoadAsset(s, 0xB8, Get<std::uint64_t>(root, 8), "navmesh shared streamkey");
				const auto cells = LoadArray(s, Get<std::uint64_t>(root, 40),
					static_cast<std::size_t>(std::max(Get<std::int32_t>(root, 32), 0)), 64, 8);
				for (std::size_t i = 0; i + 64 <= cells.size(); i += 64) {
					LoadAsset(s, 0xB8, Get<std::uint64_t>(cells, i + 40), "navmesh cell streamkey");
				}
				LoadAsset(s, 0xB8, Get<std::uint64_t>(root, 72), "navmesh debug streamkey");
				s.Pop();
			});
		}

		// --- sound_asset (0x13): Load_SoundAssetAsset 0x7FF71E7E9950 --------------------------------------
		// struct SoundAsset (88 B): +24 u32 / +32 data (align 1), +40 u32 / +48 data (align 4), +64 streamkey.
		bool LoadSoundAsset(XStream& s, std::uint64_t header) {
			s.SetRootType(0x13);
			return LoadAssetHeader(s, header, 88, 8, [&](std::span<std::uint8_t> sound) {
				s.Push(XBlockVirtual);
				LoadCountedArray(s, sound, 32, 24, 1, 1);
				LoadCountedArray(s, sound, 48, 40, 1, 4);
				LoadStreamKey(s, Get<std::uint64_t>(sound, 64));
				s.Pop();
			});
		}

		// --- sound_acoustics (0x16): Load_SoundAcousticsAsset 0x7FF71E7E94F0 -> 0x7FF71E7E9260 -----------
		// struct (352 B): +8 name, +24 streamkey, +40 an embedded 56-B struct (0x7FF71E7E9190), then nine
		// "i32 count after the pointer" arrays of 8 B and one of 84 B.
		bool LoadSoundAcoustics(XStream& s, std::uint64_t header) {
			return LoadAssetHeader(s, header, 352, 8, [&](std::span<std::uint8_t> acoustics) {
				s.Push(XBlockVirtual);
				LoadXString(s, Get<std::uint64_t>(acoustics, 8), "acoustics name");
				LoadStreamKey(s, Get<std::uint64_t>(acoustics, 24));

				const auto inner = acoustics.subspan(40, 56);
				LoadCountedArray(s, inner, 8, 0, 1, 16);
				if (Get<std::uint64_t>(inner, 32)) {
					s.Alloc(16);
					std::vector<std::uint8_t> keys(static_cast<std::size_t>(Get<std::uint32_t>(inner, 16)) * 64);
					s.Load(keys.data(), keys.size());
					for (std::size_t i = 0; i < keys.size(); i += 64) {
						LoadStreamKey(s, Get<std::uint64_t>(keys, i + 40));
					}
				}
				LoadStreamKey(s, Get<std::uint64_t>(inner, 40));

				for (const std::size_t field : { 128, 144, 160, 184, 224, 240, 256, 280, 304 }) {
					LoadCountedArray(s, acoustics, field, field + 8, 8, 4);
				}
				LoadCountedArray(s, acoustics, 336, 344, 84, 4);
				s.Pop();
			});
		}

		// --- sound_alias_modifier (0x15): Load_SoundAliasModifierAsset 0x7FF71E7E98A0 -> 0x7FF71E7E9730 ---------
		// 152 B: seven "i32 count after the pointer" arrays of 8 B at +16 .. +112. Sound banks load it the same way,
		// inline in an alias's params (Load_SoundAliasArray 0x7FF71E7E95A0).
		bool LoadSoundAliasModifier(XStream& s, std::uint64_t header) {
			return LoadAssetHeader(s, header, 152, 8, [&](std::span<std::uint8_t> modifier) {
				s.Push(XBlockVirtual);
				for (std::size_t field = 16; field <= 112; field += 16) {
					LoadCountedArray(s, modifier, field, field + 8, 8, 4);
				}
				s.Pop();
			});
		}

		// --- sound_duck (0x14): Load_SoundDuckAsset 0x7FF71E7EA1C0, a 1632-B root (32-aligned), nothing below it -
		// Sound banks load theirs the same way, inline (Load_SoundBank 0x7FF71E7E9A80).
		bool LoadSoundDuck(XStream& s, std::uint64_t header) {
			s.SetRootType(0x14);
			return LoadAssetHeader(s, header, 1632, 32, [&](std::span<std::uint8_t>) {
				s.Push(XBlockVirtual);
				s.Pop();
			});
		}

		// --- sound_bank (0x12): Load_SoundBankAsset 0x7FF71E7E9D50 -> 0x7FF71E7E9A80 --------------------
		// struct SoundBank (112 B): +32 u32 count; +40 alias lists (32 B, 0x7FF71E7E95A0 per entry);
		// +48 u32s; +56/+64 sound_duck pointers; +72/+80 136-B records (the rooms: cwlink ReadBankRooms);
		// +88/+96 88-B records with nested 288-B and 128-B arrays; +104 sound_acoustics. An alias's 208-B params
		// hold a script string at +80 (no stream bytes: library_assets.cpp records it for a copy).
		bool LoadSoundBank(XStream& s, std::uint64_t header) {
			s.SetRootType(0x12);
			return LoadAssetHeader(s, header, 112, 8, [&](std::span<std::uint8_t> bank) {
				s.Push(XBlockVirtual);
				const auto count = Get<std::uint32_t>(bank, 32);

				const auto lists = LoadArray(s, Get<std::uint64_t>(bank, 40), count, 32, 8);
				for (std::size_t i = 0; i < lists.size(); i += 32) {
					const auto aliases = Get<std::uint64_t>(lists, i + 8);
					if (aliases != kPtrInline) {
						if (aliases != kPtrNull) {
							s.Reference(aliases, "sound alias list");
						}
						continue;
					}
					// 0x7FF71E7E95A0: 96-B aliases.
					s.Alloc(8);
					std::vector<std::uint8_t> entries(static_cast<std::size_t>(Get<std::int32_t>(lists, i + 16)) * 96);
					s.Load(entries.data(), entries.size());
					for (std::size_t e = 0; e < entries.size(); e += 96) {
						LoadSoundAsset(s, Get<std::uint64_t>(entries, e + 32));
						LoadSoundAsset(s, Get<std::uint64_t>(entries, e + 40));
						LoadSoundAsset(s, Get<std::uint64_t>(entries, e + 48));
						const auto params = Get<std::uint64_t>(entries, e + 80);
						if (params == kPtrInline) {
							s.Alloc(8);
							std::vector<std::uint8_t> block(208);
							s.Load(block.data(), block.size());
							LoadSoundAliasModifier(s, Get<std::uint64_t>(block, 0));
						}
						else if (params != kPtrNull) {
							s.Reference(params, "sound alias params");
						}
						const auto extra = Get<std::uint64_t>(entries, e + 88);
						if (extra == kPtrInline) {
							s.Alloc(1);
							s.Load(nullptr, 8);
						}
						else if (extra != kPtrNull) {
							s.Reference(extra, "sound alias extra");
						}
					}
				}

				LoadCountedArray(s, bank, 48, 32, 4, 2);

				const auto ducks = LoadArray(s, Get<std::uint64_t>(bank, 64), Get<std::uint32_t>(bank, 56), 8, 8);
				for (std::size_t i = 0; i < ducks.size(); i += 8) {
					LoadSoundDuck(s, Get<std::uint64_t>(ducks, i));
				}

				LoadCountedArray(s, bank, 80, 72, 136, 8);

				const auto records = LoadArray(s, Get<std::uint64_t>(bank, 96), Get<std::uint32_t>(bank, 88), 88, 8);
				for (std::size_t i = 0; i < records.size(); i += 88) {
					if (!Get<std::uint64_t>(records, i + 80)) {
						continue;
					}
					s.Alloc(8);
					std::vector<std::uint8_t> inner(static_cast<std::size_t>(Get<std::uint32_t>(records, i + 72)) * 288);
					s.Load(inner.data(), inner.size());
					for (std::size_t j = 0; j < inner.size(); j += 288) {
						LoadCountedArray(s, inner, j + 272, j + 264, 128, 8);
					}
				}

				LoadSoundAcoustics(s, Get<std::uint64_t>(bank, 104));
				s.Pop();
			});
		}

		// --- localizeentry (0x1D): Load_LocalizeentryAsset 0x7FF71E7DEAF0 --------------------------------
		// struct LocalizeEntry (16 B) { XString value; u64 name; }
		bool LoadLocalizeEntry(XStream& s, std::uint64_t header) {
			return LoadAssetHeader(s, header, 16, 8, [&](std::span<std::uint8_t> entry) {
				s.Push(XBlockVirtual);
				LoadXString(s, Get<std::uint64_t>(entry, 0), "localized string");
				s.Pop();
			});
		}

		// --- rawfile (0x3B): Load_RawfileAsset 0x7FF71E7E7870 --------------------------------------------
		// struct RawFile (24 B) { u64 name; u32 length; char* buffer @16 (length + 1 bytes, 16-aligned); }
		bool LoadRawFile(XStream& s, std::uint64_t header) {
			return LoadAssetHeader(s, header, 24, 8, [&](std::span<std::uint8_t> raw) {
				s.Push(XBlockVirtual);
				if (Get<std::uint64_t>(raw, 16)) {
					s.Alloc(16);
					s.Load(nullptr, Get<std::uint32_t>(raw, 8) + 1ull);
				}
				s.Pop();
			});
		}

		// --- techset (0x0F): Load_TechsetAsset 0x7FF71E7E1920 --------------------------------------------
		// struct Techset (168 B): +24 17 technique pointers (0x7FF71E7E1650), +160 a name block
		// (0x7FF71E7E17E0). A technique is 120 B: +8 two 48-B passes (0x7FF71E7E0680), +110 u8 count /
		// +112 8-B array.
		void LoadShaderBlob(XStream& s, std::uint64_t pointer, const char* what) {
			// 24-B {?, data @8, u32 size @16}; the data is 256-aligned.
			if (pointer != kPtrInline) {
				if (pointer != kPtrNull) {
					s.Reference(pointer, what);
				}
				return;
			}
			s.Alloc(8);
			std::vector<std::uint8_t> blob(24);
			s.Load(blob.data(), blob.size());
			const auto data = Get<std::uint64_t>(blob, 8);
			if (data == kPtrInline) {
				s.Alloc(256);
				s.Load(nullptr, Get<std::uint32_t>(blob, 16));
			}
			else if (data != kPtrNull) {
				s.Reference(data, what);
			}
		}

		void LoadPassShaders(XStream& s, std::span<const std::uint8_t> shaders) {
			// 0x7FF71E7E0B60: an 80-B block of 10 pointers, visited in the order 0, 2, 3, 5, 4, 6, 7, 1.
			const auto first = Get<std::uint64_t>(shaders, 0);
			if (first == kPtrInline) {
				s.Alloc(8);
				s.Load(nullptr, 56);
			}
			else if (first != kPtrNull) {
				s.Reference(first, "pass shader header");
			}
			for (const std::size_t slot : { 2, 3, 5, 4, 6, 7 }) {
				LoadShaderBlob(s, Get<std::uint64_t>(shaders, slot * 8), "pass shader");
			}
			const auto last = Get<std::uint64_t>(shaders, 8);
			if (last == kPtrInline) {
				s.Alloc(8);
				s.Load(nullptr, 32);
			}
			else if (last != kPtrNull) {
				s.Reference(last, "pass shader tail");
			}
		}

		void LoadPasses(XStream& s, std::span<const std::uint8_t> passes) {
			for (std::size_t p = 0; p + 48 <= passes.size(); p += 48) {
				const auto pass = passes.subspan(p, 48);
				const auto args = Get<std::uint64_t>(pass, 16);
				if (args == kPtrInline) {
					s.Alloc(4);
					s.Load(nullptr, 12ull * (pass[1] + pass[2]));
				}
				else if (args != kPtrNull) {
					s.Reference(args, "pass arguments");
				}
				if (Get<std::uint64_t>(pass, 24)) {
					s.Alloc(4);
					s.Load(nullptr, 8ull * pass[14]);
				}
				if (Get<std::uint64_t>(pass, 32)) {
					s.Alloc(8);
					std::vector<std::uint8_t> shaders(80);
					s.Load(shaders.data(), shaders.size());
					LoadPassShaders(s, shaders);
				}
				if (Get<std::uint64_t>(pass, 40)) {
					s.Alloc(8);
					std::vector<std::uint8_t> state(32);
					s.Load(state.data(), state.size());
					if (Get<std::uint64_t>(state, 24)) {
						s.Alloc(1);
						s.Load(nullptr, 0x8000);
					}
					const auto table = Get<std::uint64_t>(state, 16);
					if (table == kPtrInline) {
						s.Alloc(8);
						s.Load(nullptr, 8ull * Get<std::int32_t>(state, 4));
					}
					else if (table != kPtrNull) {
						s.Reference(table, "pass state table");
					}
				}
			}
		}

		// Block 1 grows by 384 per techset, not align16(168) = 176 (techset_zm_silver: 240 + 562 x 384 =
		// 0x34BF0). The pool item size is 168 (g_xassetPools in the dump), so the rest is linker side data.
		constexpr std::size_t kTechsetLinkExtra = 208;

		bool LoadTechset(XStream& s, std::uint64_t header) {
			s.SetRootType(0x0F);
			return LoadAssetHeader(s, header, 168, 8, [&](std::span<std::uint8_t> techset) {
				LoadTechsetBody(s, techset);
			}, kTechsetLinkExtra);
		}

		// --- entitylist (0x8E) / triggerlist (0x80): the level's entities, map_entities.cpp -------------
		// Load_EntitylistAsset 0x7FF71E7D50F0 (24-B root) and Load_TriggerlistAsset 0x7FF71E7EF010 (72-B root).
		bool LoadEntityList(XStream& s, std::uint64_t header) {
			return LoadAssetHeader(s, header, 24, 8, [&](std::span<std::uint8_t> root) {
				LoadEntityListBody(s, root, nullptr, nullptr);
			});
		}

		bool LoadTriggerList(XStream& s, std::uint64_t header) {
			return LoadAssetHeader(s, header, 72, 8, [&](std::span<std::uint8_t> root) {
				LoadTriggerListBody(s, root, nullptr, nullptr);
			});
		}

		// Only types whose top-level Load_<Type>Asset was read. triggeractions and ragdoll are only loaded inside the
		// model family.
		constexpr std::array<std::pair<std::uint64_t, AssetLoader>, 43> kLoaders = { {
			{ 0x02, LoadPhysPresetAsset },
			{ 0x03, LoadPhysConstraintsAsset },
			{ 0x06, LoadXModelAsset },
			{ 0x07, LoadXCollisionAsset },
			{ 0x08, LoadXSkeletonAsset },
			{ 0x09, LoadXModelMeshAsset },
			{ 0x0A, LoadMaterialAsset },
			{ 0x0F, LoadTechset },
			{ 0x10, LoadImage },
			{ 0x36, LoadImpactFxTableAsset },
			{ 0x37, LoadImpactSoundsTableAsset },
			{ 0xD0, LoadXAnimCurveAsset },
			{ 0x12, LoadSoundBank },
			{ 0x13, LoadSoundAsset },
			{ 0x14, LoadSoundDuck },
			{ 0x15, LoadSoundAliasModifier },
			{ 0x16, LoadSoundAcoustics },
			{ 0x1D, LoadLocalizeEntry },
			{ 0x3B, LoadRawFile },
			{ 0x4B, LoadKeyValuePairs },
			{ 0x80, LoadTriggerList },
			{ 0x8E, LoadEntityList },
			{ 0x18, LoadClipMapAsset },
			{ 0x4C, LoadVehicleAsset },
			{ 0xAC, LoadStreamerWorldAsset },
			{ 0xAB, LoadDistrictsAsset },
			{ 0xB8, LoadStreamKey },
			{ 0x1B, LoadGfxWorldAsset },
			{ 0xBB, LoadGroupLodModelAsset },
			{ 0x66, LoadSAnimAsset },
			{ 0x6D, LoadBgCache },
			{ 0xAA, LoadLightingAsset },
			{ 0x75, LoadNavMesh },
			{ 0xB1, LoadTerrainGfxAsset },
			{ 0x7F, LoadStaticLevelFxListAsset },
			{ 0x43, LoadGlassesAsset },
			{ 0x19, LoadComWorldAsset },
			{ 0xA9, LoadCpuOcclusionDataAsset },
			{ 0x1A, LoadGameWorldAsset },
			{ 0x76, LoadNavVolumeAsset },
			{ 0x35, LoadKlfAsset },
			{ 0xD3, LoadWindDefAsset },
			{ 0x53, LoadZBarrierAsset },
		} };
	}

	void LoadTechsetBody(XStream& s, std::span<const std::uint8_t> techset) {
		s.Push(XBlockVirtual);
		for (std::size_t t = 0; t < 17; ++t) {
			const auto technique = Get<std::uint64_t>(techset, 24 + t * 8);
			if (technique != kPtrInline) {
				if (technique != kPtrNull) {
					s.Reference(technique, "technique");
				}
				continue;
			}
			s.Alloc(8);
			std::vector<std::uint8_t> tech(120);
			if (!s.Load(tech.data(), tech.size())) {
				break;
			}
			LoadPasses(s, std::span<const std::uint8_t>(tech).subspan(8, 96));
			if (Get<std::uint64_t>(tech, 112)) {
				s.Alloc(8);
				s.Load(nullptr, 8ull * tech[110]);
			}
		}

		const auto names = Get<std::uint64_t>(techset, 160);
		if (names == kPtrInline) {
			s.Alloc(8);
			std::vector<std::uint8_t> block(48);
			s.Load(block.data(), block.size());
			for (std::size_t i = 0; i < 3; ++i) {
				LoadXString(s, Get<std::uint64_t>(block, i * 8), "techset name");
			}
			if (Get<std::uint64_t>(block, 40)) {
				s.Alloc(8);
				std::vector<std::uint64_t> strings(static_cast<std::size_t>(Get<std::uint64_t>(block, 32)));
				s.Load(strings.data(), strings.size() * 8);
				for (std::uint64_t pointer : strings) {
					LoadXString(s, pointer, "techset string");
				}
			}
		}
		else if (names != kPtrNull) {
			s.Reference(names, "techset names");
		}
		s.Pop();
	}

	namespace {
		// Types mapkit only ever writes as a by-name reference: the root struct with the name's top bit set
		// and nothing else, which the engine links to the asset of that name in another zone (see
		// zone_writer.hpp). Any other inline one is real content mapkit cannot read yet.
		struct ReferenceRootType {
			std::uint64_t type = 0;
			std::size_t size = 0;
			std::uint64_t alignment = 8;
		};
		// klf and winddef were here until they got loaders of their own (library_assets.hpp, 2026-10-01).
		constexpr ReferenceRootType kReferenceRoots[] = {
			{ 0x33, 144 },      // fx: Load_FxAsset 0x7FF71E7D6B40
		};
	}

	bool IsReferenceRootType(std::uint64_t type) {
		return !FindAssetLoader(type) && std::ranges::any_of(kReferenceRoots, [&](const auto& root) { return root.type == type; });
	}

	bool LoadAsset(XStream& s, std::uint64_t type, std::uint64_t pointer, const char* what) {
		if (const AssetLoader loader = FindAssetLoader(type)) {
			s.SetRootType(type);
			return loader(s, pointer);
		}
		for (const auto& [t, size, alignment] : kReferenceRoots) {
			if (t != type || (pointer != kPtrInline && pointer != kPtrInsert)) {
				continue;
			}
			s.SetRootType(type);
			std::vector<std::uint8_t> root;
			LoadAssetHeader(s, pointer, size, alignment, [&](std::span<std::uint8_t> data) { root.assign(data.begin(), data.end()); });
			// The name where the type keeps it, every other byte zero: a klf's +0 is a string pointer (-1 reads a string
			// inline), so a name there would move the engine's stream off ours.
			const std::size_t at = XAssetNameOffset(type);
			std::vector<std::uint8_t> rest = root;
			if (at + 8 <= rest.size()) {
				std::fill_n(rest.begin() + static_cast<std::ptrdiff_t>(at), 8, std::uint8_t(0));
			}
			const bool reference = root.size() == size && at + 8 <= root.size() && (Get<std::uint64_t>(root, at) >> 63)
				&& std::ranges::all_of(rest, [](std::uint8_t b) { return b == 0; });
			if (!s.Failed() && !reference) {
				s.Fail(std::format("{} is a {} stored inline: mapkit reads it only as a by-name reference", what,
					XAssetTypeName(type)));
			}
			return !s.Failed();
		}
		// Load_<Type>Asset pushes the temp block around every case; only the inline ones need the loader.
		s.Push(XBlockTemp);
		if (pointer == kPtrInline || pointer == kPtrInsert) {
			s.Fail(std::format("{} is a {} stored inline: mapkit has no loader for that type yet", what,
				XAssetTypeName(type)));
		}
		else if (pointer != kPtrNull) {
			s.Reference(pointer, what);
		}
		s.Pop();
		s.PreloadFieldEnd(type);
		s.SetRootType(XStream::kNoRootType);
		return !s.Failed();
	}

	bool ReadImage(XStream& s, std::uint64_t header, ImageData& out) {
		s.SetRootType(0x10);
		out = {};
		return LoadAssetHeader(s, header, 208, 8, [&](std::span<std::uint8_t> image) {
			std::memcpy(out.root.data(), image.data(), std::min(image.size(), out.root.size()));
			LoadImageBody(s, image, &out);
		});
	}

	bool ReadKeyValuePairs(XStream& s, std::uint64_t header, KeyValuePairsData& out) {
		s.SetRootType(0x4B);
		out = {};
		return LoadKeyValuePairsData(s, header, &out);
	}

	std::uint64_t ImageData::Name() const { return Get<std::uint64_t>(root, 0) & ~(1ull << 63); }
	std::uint32_t ImageData::Flags() const { return Get<std::uint32_t>(root, 144); }
	std::uint32_t ImageData::Format() const { return Get<std::uint32_t>(root, 156); }
	std::uint16_t ImageData::Width() const { return Get<std::uint16_t>(root, 160); }
	std::uint16_t ImageData::Height() const { return Get<std::uint16_t>(root, 162); }
	std::uint8_t ImageData::MipCount() const { return root[184]; }

	AssetLoader FindAssetLoader(std::uint64_t type) {
		for (const auto& [t, loader] : kLoaders) {
			if (t == type) {
				return loader;
			}
		}
		return nullptr;
	}

	bool HasAssetLoader(std::uint64_t type) {
		return FindAssetLoader(type) != nullptr;
	}

	std::vector<std::uint64_t> AssetLoaderTypes() {
		std::vector<std::uint64_t> types;
		for (const auto& [t, loader] : kLoaders) {
			types.push_back(t);
		}
		return types;
	}
}
