// World assets, each loader transcribed from the game's (addresses in the comments; IDB
// bocw_fixed_renamed.i64, build 1.34.0.15931218).
#include "world_assets.hpp"
#include "asset_loaders.hpp"
#include "model_assets.hpp"
#include "xwriter.hpp"
#include "zone_trace.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <tuple>
#include <vector>

namespace MapKit::Zone {
	namespace {
		// A field the generated code only tests for non-null: Alloc + count * stride bytes, returned.
		std::vector<std::uint8_t> LoadCounted(XStream& s, std::uint64_t pointer, std::uint64_t count, std::size_t stride,
			std::uint64_t alignment) {
			return LoadArray(s, pointer, static_cast<std::size_t>(count), stride, alignment);
		}

		// 0x7FF71E7ED0A0 (72 B, embedded): +8 32 B x u64 @0, +16 a bit array (u32 @28 entries of u32 @32
		// bits, rounded as the engine does), +48 8 B x u64 @40, +64 12 B x u64 @56.
		template <std::size_t N>
		std::vector<std::array<std::uint8_t, N>> Split(const std::vector<std::uint8_t>& data) {
			std::vector<std::array<std::uint8_t, N>> out(data.size() / N);
			for (std::size_t i = 0; i < out.size(); ++i) {
				std::memcpy(out[i].data(), data.data() + i * N, N);
			}
			return out;
		}

		void LoadCellGrid(XStream& s, std::span<const std::uint8_t> grid, StreamerWorldCell* out) {
			LoadCounted(s, Get<std::uint64_t>(grid, 8), Get<std::uint64_t>(grid, 0), 32, 4);
			if (Get<std::uint64_t>(grid, 16)) {
				std::uint32_t bytes = Get<std::uint32_t>(grid, 28);
				if (bytes) {
					bytes = (Get<std::uint32_t>(grid, 32) * (bytes - 1) + 64) >> 3;
				}
				s.Alloc(1);
				s.Load(nullptr, bytes);
			}
			LoadCounted(s, Get<std::uint64_t>(grid, 48), Get<std::uint64_t>(grid, 40), 8, 4);
			const auto vectors = LoadCounted(s, Get<std::uint64_t>(grid, 64), Get<std::uint64_t>(grid, 56), 12, 4);
			if (out) {
				out->gridVectors = Split<12>(vectors);
			}
		}

		// 0x7FF71E7DA6E0 (168 B, embedded at cell +8): +8 48-B records x u64 @0 (streamkey @+8), +16 the
		// grid, +88 16 B {u64 n, 24-B entries} (0x7FF71E7DA620), +104 materials x u64 @96, +120 images
		// x u64 @112 (0x7FF71E7D9850).
		void LoadCellData(XStream& s, std::span<const std::uint8_t> data, StreamerWorldCell* out) {
			const auto keys = LoadCounted(s, Get<std::uint64_t>(data, 8), Get<std::uint64_t>(data, 0), 48, 8);
			for (std::size_t i = 0; i + 48 <= keys.size() && !s.Failed(); i += 48) {
				LoadAsset(s, 0xB8, Get<std::uint64_t>(keys, i + 8), "world cell streamkey");
			}
			if (out) {
				out->keyed = Split<48>(keys);
			}
			LoadCellGrid(s, data.subspan(16, 72), out);
			if (Get<std::uint64_t>(data, 88)) {
				s.Alloc(8);
				std::vector<std::uint8_t> head(16);
				if (s.Load(head.data(), head.size())) {
					const auto entries = LoadCounted(s, Get<std::uint64_t>(head, 8), Get<std::uint64_t>(head, 0), 24, 8);
					for (std::size_t i = 0; i + 24 <= entries.size(); i += 24) {
						const auto records = LoadCounted(s, Get<std::uint64_t>(entries, i + 16), Get<std::uint64_t>(entries, i + 8), 36, 4);
						if (out) {
							auto& list = out->lists.emplace_back();
							std::memcpy(list.data(), entries.data() + i, 24);
							out->listRecords.push_back(Split<36>(records));
						}
					}
				}
			}
			const auto materials = LoadCounted(s, Get<std::uint64_t>(data, 104), Get<std::uint64_t>(data, 96), 8, 8);
			for (std::size_t i = 0; i + 8 <= materials.size() && !s.Failed(); i += 8) {
				LoadMaterialAsset(s, Get<std::uint64_t>(materials, i));
			}
			const auto images = LoadCounted(s, Get<std::uint64_t>(data, 120), Get<std::uint64_t>(data, 112), 8, 8);
			for (std::size_t i = 0; i + 8 <= images.size() && !s.Failed(); i += 8) {
				LoadAsset(s, 0x10, Get<std::uint64_t>(images, i), "world cell image");
			}
		}

		// 0x7FF71E7DC850 (48 B, embedded at cell +176): +8 16-B {xmodel, ?} x u32 @0, +16 eight 16-B
		// groups (0x7FF71E7DC780: u32 n, 72-B entries each with u32 @56 / +64 52-B records), +24 32 B x
		// u32 @0, +40 streamkey. The 52-B records have the size of a static model placement (origin,
		// 3x3 axis, scale).
		void LoadCellModels(XStream& s, std::span<const std::uint8_t> models, StreamerWorldCell* out) {
			const std::uint32_t count = Get<std::uint32_t>(models, 0);
			const auto list = LoadCounted(s, Get<std::uint64_t>(models, 8), count, 16, 8);
			for (std::size_t i = 0; i + 16 <= list.size() && !s.Failed(); i += 16) {
				LoadXModelAsset(s, Get<std::uint64_t>(list, i));
			}
			const auto groups = LoadCounted(s, Get<std::uint64_t>(models, 16), 8, 16, 8);
			for (std::size_t g = 0; g + 16 <= groups.size(); g += 16) {
				const auto entries = LoadCounted(s, Get<std::uint64_t>(groups, g + 8), Get<std::uint32_t>(groups, g), 72, 8);
				for (std::size_t e = 0; e + 72 <= entries.size(); e += 72) {
					const auto records = LoadCounted(s, Get<std::uint64_t>(entries, e + 64), Get<std::uint32_t>(entries, e + 56), 52, 4);
					if (out) {
						StreamerWorldGroupEntry& entry = out->groups[g / 16].emplace_back();
						std::memcpy(entry.header.data(), entries.data() + e, 72);
						entry.records = Split<52>(records);
					}
				}
			}
			const auto info = LoadCounted(s, Get<std::uint64_t>(models, 24), count, 32, 4);
			LoadAsset(s, 0xB8, Get<std::uint64_t>(models, 40), "world cell models streamkey");
			if (out) {
				out->models = Split<16>(list);
				out->modelInfo = Split<32>(info);
			}
		}

		// --- streamerworld (0xAC): Load_StreamerworldAsset 0x7FF71E7ED870 -> Load_StreamerWorld 0x7FF71E7ED3B0
		// struct (224 B), {u64 count, pointer} pairs: +16/+24 24-B entries (each {u64 n @8, 40-B entries
		// @16, each {u64 n @24, 8 B @32}}), +32/+40 24-B entries ({n @8, 8 B @16}), +48/+56 8 B, +64/+72
		// 40 B, +80/+88 256-B cells, +96/+104 32 B, +112/+120 8 B, +176/+184 the xmodel list
		// (Load_XModelPtrArray 0x7FF71E7F8E00), +192/+200 bytes; +208 terraingfx, +216 lighting.
		void LoadStreamerWorldBody(XStream& s, std::span<const std::uint8_t> world, StreamerWorld* out) {
			if (out) {
				std::memcpy(out->root.data(), world.data(), std::min(world.size(), out->root.size()));
			}
			s.Push(XBlockVirtual);
			const auto outer = LoadCounted(s, Get<std::uint64_t>(world, 24), Get<std::uint64_t>(world, 16), 24, 8);
			for (std::size_t i = 0; i + 24 <= outer.size(); i += 24) {
				const auto inner = LoadCounted(s, Get<std::uint64_t>(outer, i + 16), Get<std::uint64_t>(outer, i + 8), 40, 8);
				for (std::size_t j = 0; j + 40 <= inner.size(); j += 40) {
					LoadCounted(s, Get<std::uint64_t>(inner, j + 32), Get<std::uint64_t>(inner, j + 24), 8, 4);
				}
			}
			const auto lists = LoadCounted(s, Get<std::uint64_t>(world, 40), Get<std::uint64_t>(world, 32), 24, 8);
			for (std::size_t i = 0; i + 24 <= lists.size(); i += 24) {
				LoadCounted(s, Get<std::uint64_t>(lists, i + 16), Get<std::uint64_t>(lists, i + 8), 8, 4);
			}
			LoadCounted(s, Get<std::uint64_t>(world, 56), Get<std::uint64_t>(world, 48), 8, 8);
			auto records40 = LoadCounted(s, Get<std::uint64_t>(world, 72), Get<std::uint64_t>(world, 64), 40, 8);

			const auto cells = LoadCounted(s, Get<std::uint64_t>(world, 88), Get<std::uint64_t>(world, 80), 256, 8);
			for (std::size_t i = 0; i + 256 <= cells.size() && !s.Failed(); i += 256) {
				const auto cell = std::span<const std::uint8_t>(cells).subspan(i, 256);
				StreamerWorldCell* captured = nullptr;
				if (out) {
					captured = &out->cells.emplace_back();
					std::memcpy(captured->raw.data(), cell.data(), 256);
				}
				LoadCellData(s, cell.subspan(8, 168), captured);
				LoadCellModels(s, cell.subspan(176, 48), captured);
			}

			auto records32 = LoadCounted(s, Get<std::uint64_t>(world, 104), Get<std::uint64_t>(world, 96), 32, 8);
			LoadCounted(s, Get<std::uint64_t>(world, 120), Get<std::uint64_t>(world, 112), 8, 8);
			const auto models = LoadCounted(s, Get<std::uint64_t>(world, 184), Get<std::uint64_t>(world, 176), 8, 8);
			for (std::size_t i = 0; i + 8 <= models.size() && !s.Failed(); i += 8) {
				LoadXModelAsset(s, Get<std::uint64_t>(models, i));
			}
			if (out) {
				out->records40 = std::move(records40);
				out->records32 = std::move(records32);
				for (std::size_t i = 0; i + 8 <= models.size(); i += 8) {
					out->models.push_back(Get<std::uint64_t>(models, i));
				}
			}
			LoadCounted(s, Get<std::uint64_t>(world, 200), Get<std::uint64_t>(world, 192), 1, 1);
			LoadAsset(s, 0xB1, Get<std::uint64_t>(world, 208), "streamerworld terraingfx");
			LoadAsset(s, 0xAA, Get<std::uint64_t>(world, 216), "streamerworld lighting");
			s.Pop();
		}

		// --- districts (0xAB): Load_DistrictsAsset 0x7FF71E7D45B0 -> Load_Districts 0x7FF71E7D4440 -----------
		// struct (96 B): +8 gfx_map, +16 the district sets (40 B, 0x7FF71E7D4360: +0 8 B x u32 @32, +8 4 B x u32 @32,
		// +24 4 B x u32 @32, +16 4 B x u32 @36, read in that order), +64/+72 136-B districts (+56 ten
		// streamkeys), +80/+88 pointers into data loaded earlier.
		void LoadDistrictsBody(XStream& s, std::span<const std::uint8_t> root) {
			s.Push(XBlockVirtual);
			LoadAsset(s, 0x1B, Get<std::uint64_t>(root, 8), "districts gfx_map");
			const auto sets = root.subspan(16, 40);
			LoadCounted(s, Get<std::uint64_t>(sets, 0), Get<std::uint32_t>(sets, 32), 8, 8);
			LoadCounted(s, Get<std::uint64_t>(sets, 8), Get<std::uint32_t>(sets, 32), 4, 4);
			LoadCounted(s, Get<std::uint64_t>(sets, 24), Get<std::uint32_t>(sets, 32), 4, 4);
			LoadCounted(s, Get<std::uint64_t>(sets, 16), Get<std::uint32_t>(sets, 36), 4, 4);
			const auto districts = LoadCounted(s, Get<std::uint64_t>(root, 72), Get<std::uint64_t>(root, 64), 136, 8);
			for (std::size_t i = 0; i + 136 <= districts.size() && !s.Failed(); i += 136) {
				for (std::size_t k = 0; k < 10; ++k) {
					LoadAsset(s, 0xB8, Get<std::uint64_t>(districts, i + 56 + 8 * k), "district streamkey");
				}
			}
			const auto pointers = LoadCounted(s, Get<std::uint64_t>(root, 88), Get<std::uint64_t>(root, 80), 8, 8);
			for (std::size_t i = 0; i + 8 <= pointers.size() && !s.Failed(); i += 8) {
				if (const std::uint64_t pointer = Get<std::uint64_t>(pointers, i)) {
					s.Reference(pointer, "districts pointer");
				}
			}
			s.Pop();
		}

		// --- clip_map (0x18): Load_ClipMapAsset 0x7FF71E7FA100 -> Load_ClipMap 0x7FF71E7FA1D0 ---------------

		// sub_7FF71E7D3A20 (536 B, embedded at +96 of the 992-B struct): counts live in that struct.
		void LoadClipMap992Tail(XStream& s, std::span<const std::uint8_t> tail, std::span<const std::uint8_t> owner) {
			LoadCounted(s, Get<std::uint64_t>(tail, 0), static_cast<std::uint64_t>(32ll * Get<std::int16_t>(owner, 984)), 1, 16);
			LoadCounted(s, Get<std::uint64_t>(tail, 8),
				static_cast<std::uint64_t>((Get<std::uint16_t>(owner, 976) + Get<std::int16_t>(owner, 984)) << 6), 1, 4);
			LoadCounted(s, Get<std::uint64_t>(tail, 16), 32ull * owner[987], 1, 4);
		}

		// sub_7FF71E7D3840 (992 B): +24 912 B, +32 72 B {+0 264-B entries x u16 @68}, +64 xmodels x u8 @986,
		// +72 u16 x u8 @986, +80 32 B x u8 @989, +96 the 536-B tail, +936 u32 x (u32 @944 + 1, or 0).
		void LoadClipMap992Body(XStream& s, std::span<const std::uint8_t> entry) {
			LoadCounted(s, Get<std::uint64_t>(entry, 24), 912, 1, 8);
			if (Get<std::uint64_t>(entry, 32)) {
				s.Alloc(8);
				std::vector<std::uint8_t> head(72);
				if (s.Load(head.data(), head.size())) {
					LoadCounted(s, Get<std::uint64_t>(head, 0), Get<std::uint16_t>(head, 68), 264, 8);
				}
			}
			const auto models = LoadCounted(s, Get<std::uint64_t>(entry, 64), entry[986], 8, 8);
			for (std::size_t i = 0; i + 8 <= models.size() && !s.Failed(); i += 8) {
				LoadXModelAsset(s, Get<std::uint64_t>(models, i));
			}
			LoadCounted(s, Get<std::uint64_t>(entry, 72), entry[986], 2, 2);
			LoadCounted(s, Get<std::uint64_t>(entry, 80), entry[989], 32, 4);
			LoadClipMap992Tail(s, entry.subspan(96, 536), entry);
			const std::uint32_t n = Get<std::uint32_t>(entry, 944);
			LoadCounted(s, Get<std::uint64_t>(entry, 936), n ? n + 1 : 0, 4, 2);
		}

		// A pointer array to 992-B structs (clip map +24/+32, and the list at +40): each element -1 (inline)
		// or a reference.
		void LoadClipMap992Pointers(XStream& s, std::uint64_t count, const char* what,
			std::vector<std::vector<std::uint8_t>>* out = nullptr) {
			s.Alloc(8);
			std::vector<std::uint8_t> pointers(8 * count);
			if (s.Load(pointers.data(), pointers.size())) {
				for (std::size_t i = 0; i + 8 <= pointers.size() && !s.Failed(); i += 8) {
					std::vector<std::uint8_t> entry;
					if (LoadInline(s, Get<std::uint64_t>(pointers, i), 992, 8, what, &entry)) {
						LoadClipMap992Body(s, entry);
						if (out) {
							out->push_back(std::move(entry));
						}
					}
				}
			}
		}

		void LoadClipMap992Array(XStream& s, std::uint64_t pointer, std::uint64_t count, const char* what,
			std::vector<std::vector<std::uint8_t>>* out = nullptr) {
			if (pointer == kPtrInline) {
				LoadClipMap992Pointers(s, count, what, out);
			}
			else if (pointer != kPtrNull) {
				s.Reference(pointer, what);
			}
		}

		// sub_7FF71E7E7E80 (32 B): {u64 n, 32-B entries (-1 or a raw offset)}, {u64 n, 32-B groups, each
		// {.., u64 n @16, 32-B children @24, recursive}}.
		void LoadSettingsTreeImpl(XStream& s, std::span<const std::uint8_t> node) {
			const std::uint64_t count = Get<std::uint64_t>(node, 0);
			const std::uint64_t entries = Get<std::uint64_t>(node, 8);
			if (entries == kPtrInline) {
				s.Alloc(8);
				s.Load(nullptr, static_cast<std::size_t>(32 * count));
			}
			else if (entries != kPtrNull) {
				s.Reference(entries, "settings entries");
			}
			if (Get<std::uint64_t>(node, 24)) {
				s.Alloc(8);
				std::vector<std::uint8_t> groups(static_cast<std::size_t>(32 * Get<std::uint64_t>(node, 16)));
				if (s.Load(groups.data(), groups.size())) {
					for (std::size_t g = 0; g + 32 <= groups.size() && !s.Failed(); g += 32) {
						if (Get<std::uint64_t>(groups, g + 24)) {
							s.Alloc(8);
							std::vector<std::uint8_t> children(static_cast<std::size_t>(32 * Get<std::uint64_t>(groups, g + 16)));
							if (s.Load(children.data(), children.size())) {
								for (std::size_t c = 0; c + 32 <= children.size() && !s.Failed(); c += 32) {
									LoadSettingsTreeImpl(s, std::span<const std::uint8_t>(children).subspan(c, 32));
								}
							}
						}
					}
				}
			}
		}

		// sub_7FF71E7D4660 (288 B, a dynent def): fx at +0/+24/+48/+72/+96/+120/+128/+136/+144/+152/+160,
		// +168 a settings tree, xmodels at +200/+208, physpreset at +216.
		void LoadDynEntDef(XStream& s, std::span<const std::uint8_t> def) {
			s.Push(XBlockVirtual);
			for (const std::size_t field : { 0, 24, 48, 72, 96, 120, 128, 136, 144, 152, 160 }) {
				LoadAsset(s, 0x33, Get<std::uint64_t>(def, field), "dynent fx");
			}
			LoadSettingsTreeImpl(s, def.subspan(168, 32));
			LoadXModelAsset(s, Get<std::uint64_t>(def, 200));
			LoadXModelAsset(s, Get<std::uint64_t>(def, 208));
			LoadPhysPresetAsset(s, Get<std::uint64_t>(def, 216));
			s.Pop();
		}

		void LoadClipMapBody(XStream& s, std::span<const std::uint8_t> map, ClipMap* out) {
			if (out) {
				std::memcpy(out->root.data(), map.data(), std::min(map.size(), out->root.size()));
			}
			std::size_t mark = s.Cursor();
			auto part = [&](const char* name) {
				if (out) {
					out->parts.emplace_back(name, s.Cursor() - mark);
				}
				mark = s.Cursor();
			};

			s.Push(XBlockVirtual);
			const std::uint16_t cellCount = Get<std::uint16_t>(map, 462);
			const auto cells = LoadCounted(s, Get<std::uint64_t>(map, 8), cellCount, 64, 8);
			for (std::size_t i = 0; i + 64 <= cells.size() && !s.Failed(); i += 64) {
				const auto cell = std::span<const std::uint8_t>(cells).subspan(i, 64);
				ClipMapCell* captured = out ? &out->cells.emplace_back() : nullptr;
				if (captured) {
					std::memcpy(captured->raw.data(), cell.data(), 64);
				}
				s.Push(XBlockCollisionAlias);
				auto blob = LoadCounted(s, Get<std::uint64_t>(cell, 24), Get<std::uint32_t>(cell, 32), 1, 1);
				s.Pop();
				const auto records = LoadCounted(s, Get<std::uint64_t>(cell, 40), Get<std::uint32_t>(cell, 48), 80, 8);
				for (std::size_t r = 0; r + 80 <= records.size() && !s.Failed(); r += 80) {
					LoadXCollisionAsset(s, Get<std::uint64_t>(records, r));
				}
				if (captured) {
					captured->blob = std::move(blob);
					captured->records = Split<80>(records);
				}
			}
			auto cellIndex = LoadCounted(s, Get<std::uint64_t>(map, 16), cellCount, 4, 4);
			if (out) {
				out->cellIndex = std::move(cellIndex);
			}
			part("cells (+8/+16)");

			LoadClipMap992Array(s, Get<std::uint64_t>(map, 24), Get<std::uint32_t>(map, 428), "clip map +24",
				out ? &out->models : nullptr);
			LoadClipMap992Array(s, Get<std::uint64_t>(map, 32), Get<std::uint32_t>(map, 432), "clip map +32");
			if (Get<std::uint64_t>(map, 40)) {
				// sub_7FF71E7DE820 (24 B): +0 992-B pointers x u32 @16 (Alloc'd, not -1-checked), +8 16 B x u32 @16.
				s.Alloc(8);
				std::vector<std::uint8_t> list(24);
				if (s.Load(list.data(), list.size())) {
					if (Get<std::uint64_t>(list, 0)) {
						LoadClipMap992Pointers(s, Get<std::uint32_t>(list, 16), "clip map +40 list");
					}
					LoadCounted(s, Get<std::uint64_t>(list, 8), Get<std::uint32_t>(list, 16), 16, 4);
				}
			}
			part("992-B structs (+24/+32/+40)");

			const std::uint32_t treeCount = Get<std::uint32_t>(map, 436);
			if (Get<std::uint64_t>(map, 48)) {
				s.Alloc(8);
				std::vector<std::uint8_t> trees(184ull * treeCount);
				if (s.Load(trees.data(), trees.size())) {
					for (std::size_t i = 0; i + 184 <= trees.size() && !s.Failed(); i += 184) {
						CollisionTree* captured = out ? &out->trees.emplace_back() : nullptr;
						ReadCollisionTree(s, std::span<const std::uint8_t>(trees).subspan(i, 184), captured);
					}
				}
			}
			part("collision trees (+48)");
			if (Get<std::uint64_t>(map, 56)) {
				// sub_7FF71E7FA920 (32 B): +0 8 B x u32 @24, +8 4 B x u32 @24, +16 bytes u32 @28.
				s.Alloc(8);
				std::vector<std::uint8_t> index(32);
				if (s.Load(index.data(), index.size())) {
					auto first = LoadArray(s, Get<std::uint64_t>(index, 0), Get<std::uint32_t>(index, 24), 8, 4);
					auto second = LoadArray(s, Get<std::uint64_t>(index, 8), Get<std::uint32_t>(index, 24), 4, 4);
					auto third = LoadArray(s, Get<std::uint64_t>(index, 16), Get<std::uint32_t>(index, 28), 1, 1);
					if (out) {
						out->hasTreeIndex = true;
						std::memcpy(out->treeIndex.data(), index.data(), 32);
						out->treeIndexArrays = { std::move(first), std::move(second), std::move(third) };
					}
				}
			}
			part("tree index (+56)");

			const auto collisions = LoadCounted(s, Get<std::uint64_t>(map, 64), Get<std::uint32_t>(map, 424), 8, 8);
			for (std::size_t i = 0; i + 8 <= collisions.size() && !s.Failed(); i += 8) {
				LoadXCollisionAsset(s, Get<std::uint64_t>(collisions, i));
				if (out) {
					out->collisions.push_back(Get<std::uint64_t>(collisions, i));
				}
			}
			part("xcollisions (+64)");
			auto array72 = LoadCounted(s, Get<std::uint64_t>(map, 72), Get<std::uint32_t>(map, 440), 4, 4);
			auto array80 = LoadCounted(s, Get<std::uint64_t>(map, 80), Get<std::uint32_t>(map, 444), 2, 2);
			if (out) {
				out->array72 = std::move(array72);
				out->array80 = std::move(array80);
			}

			const std::uint16_t dynEnts = Get<std::uint16_t>(map, 88);
			const std::uint16_t dynEnts2 = Get<std::uint16_t>(map, 90);
			const auto defs = LoadCounted(s, Get<std::uint64_t>(map, 96), dynEnts, 96, 8);
			for (std::size_t i = 0; i + 96 <= defs.size() && !s.Failed(); i += 96) {
				const std::uint64_t pointer = Get<std::uint64_t>(defs, i);
				if (out) {
					std::memcpy(out->dynEnts.emplace_back().data(), defs.data() + i, 96);
				}
				s.Push(XBlockTemp);
				if (pointer == kPtrInline || pointer == kPtrInsert) {
					if (pointer == kPtrInsert) {
						s.Insert();
					}
					s.Alloc(8);
					std::vector<std::uint8_t> def(288);
					if (s.Load(def.data(), def.size())) {
						LoadDynEntDef(s, def);
					}
				}
				else if (pointer != kPtrNull) {
					s.Reference(pointer, "dynent def");
				}
				s.Pop();
			}
			part("dynent defs (+96)");
			const std::tuple<std::size_t, std::size_t, std::size_t, std::uint64_t> runtime[] = {
				{ 104, 248, dynEnts, 8 }, { 112, 280, dynEnts, 8 }, { 120, 440, dynEnts2, 8 },
				{ 128, 44, dynEnts, 4 }, { 136, 44, dynEnts2, 4 },
			};
			for (const auto& [field, stride, count, alignment] : runtime) {
				s.Push(XBlockRuntime);
				LoadCounted(s, Get<std::uint64_t>(map, field), count, stride, alignment);
				s.Pop();
			}
			if (Get<std::uint64_t>(map, 144) && Get<std::int32_t>(map, 448) > 0) {
				s.Fail("clip map constraints (+144) are not supported yet");
				return;
			}
			s.Push(XBlockRuntime);
			LoadCounted(s, Get<std::uint64_t>(map, 152), static_cast<std::uint64_t>(std::max(0, Get<std::int32_t>(map, 452))), 4512, 8);
			s.Pop();

			// +160 (72 B, -1 or a reference): +64 176-B entries x u32 @60 (sub_7FF71E7EAB70), each +24 80-B
			// records x u32 @0 {+40 u16 x u16 @32, +48 bits of u16 @32, +56 bytes u32 @72}, +152 24-B
			// pointers x u32 @160.
			std::vector<std::uint8_t> world;
			if (LoadInline(s, Get<std::uint64_t>(map, 160), 72, 8, "clip map +160", &world)) {
				if (out) {
					out->hasWorld = true;
					std::memcpy(out->worldHead.data(), world.data(), 72);
				}
			}
			if (world.size() == 72 && Get<std::uint64_t>(world, 64)) {
				s.Alloc(8);
				std::vector<std::uint8_t> entries(176ull * Get<std::uint32_t>(world, 60));
				if (s.Load(entries.data(), entries.size())) {
					for (std::size_t e = 0; e + 176 <= entries.size() && !s.Failed(); e += 176) {
						const auto entry = std::span<const std::uint8_t>(entries).subspan(e, 176);
						ClipMapWorldEntry* captured = out ? &out->worldEntries.emplace_back() : nullptr;
						if (captured) {
							std::memcpy(captured->raw.data(), entry.data(), 176);
						}
						const auto records = LoadCounted(s, Get<std::uint64_t>(entry, 24), Get<std::uint32_t>(entry, 0), 80, 8);
						for (std::size_t r = 0; r + 80 <= records.size() && !s.Failed(); r += 80) {
							const auto record = std::span<const std::uint8_t>(records).subspan(r, 80);
							ClipMapWorldRecord* rec = captured ? &captured->records.emplace_back() : nullptr;
							if (rec) {
								std::memcpy(rec->raw.data(), record.data(), 80);
							}
							const std::uint16_t n = Get<std::uint16_t>(record, 32);
							s.Push(XBlockCollisionAlias);
							LoadInline(s, Get<std::uint64_t>(record, 40), 2ull * n, 2, "clip map +160 record +40",
								rec ? &rec->indices : nullptr);
							s.Pop();
							s.Push(XBlockCollisionAlias);
							auto bits = LoadCounted(s, Get<std::uint64_t>(record, 48), (n + 7ull) >> 3, 1, 1);
							s.Pop();
							s.Push(XBlockCollisionAlias);
							LoadInline(s, Get<std::uint64_t>(record, 56), Get<std::uint32_t>(record, 72), 1, "clip map +160 record +56",
								rec ? &rec->blob : nullptr);
							s.Pop();
							if (rec) {
								rec->bits = std::move(bits);
							}
						}
						if (Get<std::uint64_t>(entry, 152)) {
							s.Alloc(8);
							std::vector<std::uint8_t> list(8ull * Get<std::uint32_t>(entry, 160));
							if (s.Load(list.data(), list.size())) {
								for (std::size_t k = 0; k + 8 <= list.size(); k += 8) {
									std::vector<std::uint8_t> item;
									LoadInline(s, Get<std::uint64_t>(list, k), 24, 8, "clip map +160 entry list", &item);
									if (captured) {
										captured->list.push_back(std::move(item));
									}
								}
							}
						}
					}
				}
			}
			part("tail (+72..+160)");
			s.Pop();
		}
	}

	void LoadSettingsTree(XStream& s, std::span<const std::uint8_t> node) {
		LoadSettingsTreeImpl(s, node);
	}

	void LoadCollisionTreeIndexBody(XStream& s, std::span<const std::uint8_t> index) {
		LoadArray(s, Get<std::uint64_t>(index, 0), Get<std::uint32_t>(index, 24), 8, 4);
		LoadArray(s, Get<std::uint64_t>(index, 8), Get<std::uint32_t>(index, 24), 4, 4);
		LoadArray(s, Get<std::uint64_t>(index, 16), Get<std::uint32_t>(index, 28), 1, 1);
	}

	bool LoadStreamerWorldAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0xAC);
		return LoadAssetHeader(s, header, 224, 8, [&](std::span<std::uint8_t> world) {
			LoadStreamerWorldBody(s, world, nullptr);
		});
	}

	bool LoadDistrictsAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0xAB);
		return LoadAssetHeader(s, header, 96, 8, [&](std::span<std::uint8_t> root) { LoadDistrictsBody(s, root); });
	}

	bool ReadStreamerWorld(XStream& s, std::uint64_t header, StreamerWorld& out) {
		out = {};
		return LoadAssetHeader(s, header, 224, 8, [&](std::span<std::uint8_t> world) {
			LoadStreamerWorldBody(s, world, &out);
		});
	}
	bool LoadClipMapAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x18);
		return LoadAssetHeader(s, header, 472, 8, [&](std::span<std::uint8_t> map) { LoadClipMapBody(s, map, nullptr); });
	}

	bool ReadClipMap(XStream& s, std::uint64_t header, ClipMap& out) {
		out = {};
		return LoadAssetHeader(s, header, 472, 8, [&](std::span<std::uint8_t> map) { LoadClipMapBody(s, map, &out); });
	}

	// --- lighting (0xAA) ------------------------------------------------------------------------------------------
	namespace {
		// What CopyLighting needs from a walk: where every link to another asset sits, and what it cannot copy.
		struct LightingLog {
			// A baked shadow tree's record: where its pointer to the tree is stored, and the tree when it follows inline.
			struct Shadow {
				std::size_t pointerAt = SIZE_MAX;
				bool region = false;
				std::size_t tree = SIZE_MAX, nodeCounts = SIZE_MAX, holder = SIZE_MAX; // stream offsets
			};
			std::vector<std::tuple<std::size_t, std::uint64_t, std::uint64_t>> links; // stream offset, type, stored value
			std::vector<std::pair<std::size_t, std::uint64_t>> pointers; // pointers to data loaded earlier: stream offset, value
			std::size_t inlineAssets = 0;   // assets stored inside the lighting asset
			std::size_t unplaced = 0;       // fields whose stream offset is unknown
			std::size_t volumesAt = SIZE_MAX;
			std::size_t lightsAt = SIZE_MAX;
			std::vector<Shadow> shadows;
		};

		// Load_Lighting's body in the loader's order; the research walker's functions, one for one.
		class LightingWalk {
		public:
			LightingWalk(XStream& s, LightingLog* log) : m_S(s), m_Log(log) {}

			void Body(std::span<const std::uint8_t> r, std::size_t rootAt) {
				XStream& s = m_S;
				const Loaded root{ std::vector<std::uint8_t>(r.begin(), r.end()), rootAt };
				s.Push(XBlockVirtual);
				// +8 n / +16: 688-B lights, the cookie image at +672 (0x7FF71E7D0E10).
				if (Get<std::uint64_t>(r, 16)) {
					s.Alloc(16);
					const Loaded lights = Load(688ull * Get<std::uint32_t>(r, 8));
					if (m_Log) {
						m_Log->lightsAt = lights.at;
					}
					for (std::size_t i = 0; i + 688 <= lights.data.size(); i += 688) {
						Asset(0x10, lights, i + 672, "light cookie image");
					}
				}
				Counted(r, 32, Get<std::uint32_t>(r, 24), 20);
				Counted(r, 48, Get<std::uint32_t>(r, 40), 28);
				if (Get<std::uint64_t>(r, 64)) {
					s.Alloc(16);
					Volumes(Get<std::uint32_t>(r, 56));
				}
				Array(Get<std::uint64_t>(r, 80), 16ull * Get<std::uint32_t>(r, 72), 4);
				Array(Get<std::uint64_t>(r, 88), 4ull * Get<std::uint32_t>(r, 56), 4);
				Array(Get<std::uint64_t>(r, 104), 4ull * Get<std::uint32_t>(r, 96), 4);
				for (std::size_t i = 0; i < 4; ++i) {
					Asset(0x10, root, 112 + 8 * i, "lighting image");
				}
				// +144 n / +152: 464-B shadow regions, four streamed textures each.
				if (Get<std::uint64_t>(r, 152)) {
					s.Alloc(16);
					const Loaded regions = Load(464ull * Get<std::uint32_t>(r, 144));
					for (std::size_t i = 0; i + 464 <= regions.data.size(); i += 464) {
						for (std::size_t k = 0; k < 4; ++k) {
							Streamed104Field(regions, i + 16 + 112 * k + 96, true);
						}
					}
				}
				Array(Get<std::uint64_t>(r, 168), 16ull * Get<std::uint32_t>(r, 160), 4);
				Array(Get<std::uint64_t>(r, 184), 624ull * Get<std::uint64_t>(r, 176), 16);
				Array(Get<std::uint64_t>(r, 200), 16ull * Get<std::uint64_t>(r, 192), 4);
				// +208 n / +216: 128-B image sets, six images at +80 (Load_LightingImageSet128Array_cand 0x7FF71E7DAFD0).
				// The one the view stands in gives the frame its six images (R_SetupFrameLightingImageSet_cand).
				if (Get<std::uint64_t>(r, 216)) {
					s.Alloc(8);
					const Loaded sets = Load(128ull * Get<std::uint64_t>(r, 208));
					for (std::size_t i = 0; i + 128 <= sets.data.size(); i += 128) {
						for (std::size_t k = 0; k < 6; ++k) {
							Asset(0x10, sets, i + 80 + 8 * k, "image set image");
						}
					}
				}
				Array(Get<std::uint64_t>(r, 232), 16ull * Get<std::uint64_t>(r, 224), 4);
				Array(Get<std::uint64_t>(r, 248), 40ull * Get<std::uint64_t>(r, 240), 4);
				Array(Get<std::uint64_t>(r, 264), 16ull * Get<std::uint64_t>(r, 256), 4);
				Array(Get<std::uint64_t>(r, 280), 144ull * Get<std::uint32_t>(r, 272), 16);
				Array(Get<std::uint64_t>(r, 296), 224ull * Get<std::uint64_t>(r, 288), 4);
				Array(Get<std::uint64_t>(r, 336), 360ull * Get<std::uint64_t>(r, 328), 16);
				Array(Get<std::uint64_t>(r, 352), 16ull * Get<std::uint64_t>(r, 344), 4);
				if (Get<std::uint64_t>(r, 448)) {
					s.Alloc(8);
					const Loaded wind = Load(40ull * Get<std::uint64_t>(r, 440));
					for (std::size_t i = 0; i + 40 <= wind.data.size(); i += 40) {
						Asset(0xD3, wind, i + 32, "wind volume winddef");
					}
				}
				Array(Get<std::uint64_t>(r, 464), 16ull * Get<std::uint64_t>(r, 456), 4);
				Array(Get<std::uint64_t>(r, 368), 16ull * Get<std::uint32_t>(r, 360), 4);
				if (Get<std::uint64_t>(r, 384)) {
					s.Alloc(8);
					const Loaded images = Load(56ull * Get<std::uint32_t>(r, 376));
					for (std::size_t i = 0; i + 56 <= images.data.size(); i += 56) {
						Asset(0x10, images, i, "+384 image");
					}
				}
				if (Get<std::uint64_t>(r, 400)) {
					s.Alloc(8);
					const Loaded images = Load(56ull * Get<std::uint32_t>(r, 392));
					for (std::size_t i = 0; i + 56 <= images.data.size(); i += 56) {
						Asset(0x10, images, i, "+400 image");
						Asset(0x10, images, i + 8, "+400 image");
						Array(Get<std::uint64_t>(images.data, i + 48), 4ull * Get<std::uint32_t>(images.data, i + 40), 4);
					}
				}
				if (Get<std::uint64_t>(r, 416)) {
					s.Alloc(8);
					const Loaded images = Load(48ull * Get<std::uint32_t>(r, 408));
					for (std::size_t i = 0; i + 48 <= images.data.size(); i += 48) {
						Asset(0x10, images, i, "+416 image");
						for (const std::size_t o : { 24, 32, 40 }) {
							Array(Get<std::uint64_t>(images.data, i + o), 4ull * Get<std::uint32_t>(images.data, i + 16), 4);
						}
					}
				}
				if (Get<std::uint64_t>(r, 432)) {
					s.Alloc(8);
					const Loaded images = Load(128ull * Get<std::uint32_t>(r, 424));
					for (std::size_t i = 0; i + 128 <= images.data.size(); i += 128) {
						Asset(0x10, images, i, "+432 image");
					}
				}
				s.Pop();
			}

		private:
			struct Loaded {
				std::vector<std::uint8_t> data;
				std::size_t at = SIZE_MAX; // its stream offset (every block the lighting asset loads into is stored)
			};

			Loaded Load(std::size_t size) {
				Loaded out;
				out.at = XBlockIsStored(m_S.Block()) ? m_S.Cursor() : SIZE_MAX;
				out.data.resize(size);
				m_S.Load(out.data.data(), size);
				return out;
			}

			// `if (p) { Alloc(a); Load(n) }`
			void Array(std::uint64_t pointer, std::uint64_t size, std::uint64_t alignment) {
				if (pointer) {
					m_S.Alloc(alignment);
					m_S.Load(nullptr, static_cast<std::size_t>(size));
				}
			}

			// `if (p == -1) { Alloc(a); Load(n) } else if (p) ConvertOffsetToPointer`, p the field at owner + offset.
			void Inline(const Loaded& owner, std::size_t offset, std::uint64_t alignment, std::uint64_t size) {
				const auto pointer = Get<std::uint64_t>(owner.data, offset);
				if (pointer == kPtrInline) {
					m_S.Alloc(alignment);
					m_S.Load(nullptr, static_cast<std::size_t>(size));
				}
				else if (pointer != kPtrNull) {
					DataReference(owner, offset, pointer);
				}
			}

			// A pointer to data loaded earlier (in Die Maschine's lighting: always its own, shared between states).
			void DataReference(const Loaded& owner, std::size_t offset, std::uint64_t pointer) {
				m_S.Reference(pointer, "lighting data");
				if (m_Log) {
					if (owner.at == SIZE_MAX) {
						++m_Log->unplaced;
					}
					else {
						m_Log->pointers.emplace_back(owner.at + offset, pointer);
					}
				}
			}

			// {u64 p, i32 n} x count at +pointerOffset: each n x stride bytes (the +32 and +48 pairs).
			void Counted(std::span<const std::uint8_t> r, std::size_t pointerOffset, std::uint32_t count, std::size_t stride) {
				if (!Get<std::uint64_t>(r, pointerOffset)) {
					return;
				}
				m_S.Alloc(8);
				const Loaded pairs = Load(24ull * count);
				for (std::size_t i = 0; i + 24 <= pairs.data.size(); i += 24) {
					if (Get<std::uint64_t>(pairs.data, i + 16)) {
						m_S.Alloc(4);
						m_S.Load(nullptr, stride * static_cast<std::size_t>(std::max(Get<std::int32_t>(pairs.data, i + 8), 0)));
					}
				}
			}

			// Load_<Type>Asset(0, &field) on a field of `owner`: a link is logged where it sits.
			void Asset(std::uint64_t type, const Loaded& owner, std::size_t offset, const char* what) {
				const auto value = Get<std::uint64_t>(owner.data, offset);
				const bool inlined = value == kPtrInline || value == kPtrInsert;
				if (m_Log && value != kPtrNull) {
					if (inlined) {
						++m_Log->inlineAssets;
					}
					else if (owner.at == SIZE_MAX) {
						++m_Log->unplaced;
					}
					else {
						m_Log->links.emplace_back(owner.at + offset, type, value);
					}
				}
				if (inlined && (type == 0xD3 || type == 0x35)) {
					InlineAsset(type, value);
				}
				else {
					LoadAsset(m_S, type, value, what);
				}
			}

			// The winddef (Load_WinddefAsset 0x7FF71E7F65D0: 128 B, 16-aligned, nothing inside) and klf (Load_Klf
			// 0x7FF71E7D75A0: 88 B) kinds, which have no loader of their own in mapkit.
			void InlineAsset(std::uint64_t type, std::uint64_t header) {
				XStream& s = m_S;
				if (type == 0xD3) {
					LoadAssetHeader(s, header, 128, 16, [&](std::span<std::uint8_t>) {
						s.Push(XBlockVirtual);
						s.Pop();
					});
					return;
				}
				LoadAssetHeader(s, header, 88, 8, [&](std::span<std::uint8_t> k) {
					const Loaded klf{ std::vector<std::uint8_t>(k.begin(), k.end()), s.Cursor() - 88 };
					s.Push(XBlockVirtual);
					LoadXString(s, Get<std::uint64_t>(k, 0), "klf name");
					if (Get<std::uint64_t>(k, 24)) {
						s.Alloc(8);
						const Loaded entries = Load(712ull * Get<std::uint16_t>(k, 64));
						for (std::size_t i = 0; i + 712 <= entries.data.size(); i += 712) {
							LoadXString(s, Get<std::uint64_t>(entries.data, i), "klf entry name");
							if (Get<std::uint64_t>(entries.data, i + 648)) {
								s.Alloc(16);
								Pixels56();
							}
							Asset(0x10, entries, i + 656, "klf entry image");
						}
					}
					s.Push(XBlockPhysical);
					Inline(klf, 32, 16, 424ull * Get<std::uint16_t>(k, 64));
					s.Pop();
					s.Push(XBlockPhysical);
					Inline(klf, 40, 16, 8ull * Get<std::uint16_t>(k, 66));
					s.Pop();
					for (const std::size_t o : { 48, 56 }) {
						if (Get<std::uint64_t>(k, o)) {
							s.Alloc(16);
							Pixels56();
						}
					}
					Asset(0x10, klf, 72, "klf image");
					s.Pop();
				});
			}

			// Load_GfxPixels56 0x7FF71E7DA0D0 (56 B): push 6, +0 -1 -> u32 @8 x u32 @12 bytes, 256-aligned.
			void Pixels56() {
				const Loaded h = Load(56);
				m_S.Push(XBlockPhysical);
				const auto pixels = Get<std::uint64_t>(h.data, 0);
				if (pixels == kPtrInline) {
					m_S.Alloc(256);
					m_S.Load(nullptr, static_cast<std::size_t>(Get<std::uint32_t>(h.data, 8)) * Get<std::uint32_t>(h.data, 12));
				}
				else if (pixels != kPtrNull) {
					DataReference(h, 0, pixels);
				}
				m_S.Pop();
			}

			// Load_GfxBlob3 0x7FF71E7D8D80 (48 B embedded at owner + o): {n, p} x 3: n bytes (align 1), n x u32, n x u32.
			void Blob3(const Loaded& owner, std::size_t o) {
				Inline(owner, o + 8, 1, Get<std::uint64_t>(owner.data, o + 0));
				Inline(owner, o + 24, 4, 4 * Get<std::uint64_t>(owner.data, o + 16));
				Inline(owner, o + 40, 4, 4 * Get<std::uint64_t>(owner.data, o + 32));
			}

			// A pointer to a streamed texture (0x7FF71E7DA810, 104 B) at owner + offset: -1 = one follows, 16-aligned. It is
			// a baked shadow tree's (world_assets.hpp): a volume state's sun shadow, or a shadow region's entry.
			void Streamed104Field(const Loaded& owner, std::size_t offset, bool region) {
				const auto pointer = Get<std::uint64_t>(owner.data, offset);
				LightingLog::Shadow shadow;
				shadow.pointerAt = owner.at == SIZE_MAX ? SIZE_MAX : owner.at + offset;
				shadow.region = region;
				if (pointer == kPtrInline) {
					m_S.Alloc(16);
					Streamed104(shadow);
				}
				else if (pointer != kPtrNull) {
					DataReference(owner, offset, pointer);
				}
				if (m_Log) {
					m_Log->shadows.push_back(shadow);
				}
			}

			// +0 streamkey, +8 / +16 u32 x u32 @28, +80 and +96 image holders (0x7FF71E7D9A20, 40 B, image at +24),
			// +88 a GfxPixels56.
			void Streamed104(LightingLog::Shadow& shadow) {
				const Loaded b = Load(104);
				shadow.tree = b.at;
				Asset(0xB8, b, 0, "streamed texture key");
				Array(Get<std::uint64_t>(b.data, 8), 4ull * Get<std::uint32_t>(b.data, 28), 4);
				if (Get<std::uint64_t>(b.data, 16) && XBlockIsStored(m_S.Block())) {
					shadow.nodeCounts = m_S.Cursor(); // the stream has no padding: the array starts where the cursor is
				}
				Array(Get<std::uint64_t>(b.data, 16), 4ull * Get<std::uint32_t>(b.data, 28), 4);
				for (const std::size_t offset : { 80, 88, 96 }) {
					const auto pointer = Get<std::uint64_t>(b.data, offset);
					if (pointer == kPtrInline) {
						if (offset == 88) {
							m_S.Alloc(16);
							Pixels56();
						}
						else {
							m_S.Alloc(8);
							const Loaded holder = Load(40);
							if (offset == 80) {
								shadow.holder = holder.at;
							}
							Asset(0x10, holder, 24, "streamed texture image");
						}
					}
					else if (pointer != kPtrNull) {
						DataReference(b, offset, pointer);
					}
				}
			}

			// Load_LightingVolumeArray 0x7FF71E7DA940: the 9888-B volumes, then what each points at.
			void Volumes(std::uint32_t count) {
				XStream& s = m_S;
				const Loaded volumes = Load(kLightingVolumeSize * count);
				if (m_Log) {
					m_Log->volumesAt = volumes.at;
				}
				for (std::size_t i = 0; i < count && !s.Failed(); ++i) {
					const std::size_t e = kLightingVolumeSize * i;
					const std::span<const std::uint8_t> v(volumes.data.data() + e, kLightingVolumeSize);
					for (std::size_t k = 0; k < 4; ++k) { // +8416 sun shadow per state
						Streamed104Field(volumes, e + 8416 + 128 * k + 112, false);
					}
					Array(Get<std::uint64_t>(v, 8968), 2ull * Get<std::uint32_t>(v, 8960), 2);
					for (std::size_t k = 0; k < 4; ++k) { // +9024 reflection probes per state
						const std::size_t o = 9024 + 88 * k;
						Inline(volumes, e + o + 16, 16, 376ull * (Get<std::uint32_t>(v, o + 80) + 1));
						Inline(volumes, e + o + 24, 16, 604ull * Get<std::uint32_t>(v, o + 84));
						Blob3(volumes, e + o + 32);
						const auto pointer = Get<std::uint64_t>(v, o + 8);
						if (pointer == kPtrInline) {
							s.Alloc(8);
							const Loaded images = Load(8 * Get<std::uint64_t>(v, o));
							for (std::size_t j = 0; j + 8 <= images.data.size(); j += 8) {
								Asset(0x10, images, j, "reflection probe image");
							}
						}
						else if (pointer != kPtrNull) {
							DataReference(volumes, e + o + 8, pointer);
						}
					}
					for (std::size_t k = 0; k < 4; ++k) { // +9376 GI image triples per state (0x7FF71E7D8C10)
						const std::size_t o = 9376 + 16 * k;
						const auto pointer = Get<std::uint64_t>(v, o + 8);
						if (pointer == kPtrInline) {
							s.Alloc(8);
							const Loaded triples = Load(32 * Get<std::uint64_t>(v, o));
							for (std::size_t j = 0; j + 32 <= triples.data.size(); j += 32) {
								for (std::size_t m = 0; m < 3; ++m) {
									Asset(0x10, triples, j + 8 * m, "GI image");
								}
							}
						}
						else if (pointer != kPtrNull) {
							DataReference(volumes, e + o + 8, pointer);
						}
					}
					for (std::size_t k = 0; k < 4; ++k) {
						Asset(0x10, volumes, e + 8928 + 8 * k, "sun cookie image");
					}
					for (std::size_t k = 0; k < 4; ++k) { // +9440 sky per state (Load_LightingSkyStateArray)
						const std::size_t o = e + 9440 + 40 * k;
						Asset(0x06, volumes, o, "skybox model");
						Asset(0x10, volumes, o + 24, "sky image");
						Asset(0x35, volumes, o + 32, "sky klf");
					}
					for (std::size_t k = 0; k < 4; ++k) { // +9600 64 B per state (0x7FF71E7D90C0)
						const std::size_t o = 9600 + 64 * k;
						const auto pointer = Get<std::uint64_t>(v, o + 8);
						if (pointer == kPtrInline) {
							s.Alloc(8);
							const Loaded records = Load(80ull * Get<std::uint32_t>(v, o));
							for (std::size_t j = 0; j + 80 <= records.data.size(); j += 80) {
								const std::span<const std::uint8_t> a(records.data.data() + j, 80);
								Array(Get<std::uint64_t>(a, 56),
									12ull * Get<std::uint16_t>(a, 48) * Get<std::uint16_t>(a, 46) * Get<std::uint16_t>(a, 44), 4);
								if (Get<std::uint64_t>(a, 64)) {
									s.Alloc(8);
									const Loaded inner = Load(80ull * Get<std::uint16_t>(a, 50));
									for (std::size_t q = 0; q + 80 <= inner.data.size(); q += 80) {
										Array(Get<std::uint64_t>(inner.data, q + 64), 16ull * Get<std::uint32_t>(inner.data, q + 60), 4);
									}
								}
							}
						}
						else if (pointer != kPtrNull) {
							DataReference(volumes, e + o + 8, pointer);
						}
						Inline(volumes, e + o + 48, 4, 8ull * Get<std::uint32_t>(v, o + 40) * Get<std::uint32_t>(v, o + 40));
						Inline(volumes, e + o + 56, 4, 4ull * Get<std::uint32_t>(v, o + 44));
					}
					Blob3(volumes, e + 8976);
				}
			}

			XStream& m_S;
			LightingLog* m_Log;
		};
	}

	bool LoadLightingAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0xAA);
		return LoadAssetHeader(s, header, kLightingRootSize, 8, [&](std::span<std::uint8_t> root) {
			LightingWalk(s, nullptr).Body(root, s.Cursor() - kLightingRootSize);
		});
	}

	std::span<std::uint8_t> LightingCopy::State(std::size_t volume, std::size_t state) {
		const std::size_t at = volumesAt + kLightingVolumeSize * volume + kLightingStatesAt + kLightingStateSize * state;
		return at + kLightingStateSize <= bytes.size() ? std::span<std::uint8_t>(bytes).subspan(at, kLightingStateSize)
			: std::span<std::uint8_t>();
	}

	bool CopyLighting(const ZoneTrace& trace, std::span<const std::uint8_t> stream, const XAssetList& list,
		std::size_t asset, LightingCopy& out, std::string& error) {
		out = {};
		if (asset >= list.assets.size() || list.assets[asset].type != 0xAA || list.assets[asset].header != kPtrInline) {
			error = std::format("asset {} is not a lighting asset stored in this zone", asset);
			return false;
		}
		const TracedAsset& start = trace.assets[asset];
		XStream s(stream, 0);
		s.Restore(start.offset, start.block, start.pos);
		LightingLog log;
		std::vector<XStream::LoadRecord> loads;
		s.LogLoads(&loads);
		LoadAssetHeader(s, kPtrInline, kLightingRootSize, 8, [&](std::span<std::uint8_t> root) {
			LightingWalk(s, &log).Body(root, s.Cursor() - kLightingRootSize);
		});
		if (s.Failed()) {
			error = s.Error();
			return false;
		}
		if (log.inlineAssets || log.unplaced) {
			error = std::format("it stores {} asset(s) inside it and {} field(s) where mapkit cannot place them; only links and "
				"pointers into its own data are handled", log.inlineAssets, log.unplaced);
			return false;
		}
		// Every link the bytes hold must be one the walk logged: a missed one would be copied as the base map's stored
		// value, which leads anywhere in the new zone (a missed sixth image-set image crashed the renderer, 2026-09-30).
		const std::uint64_t assetArrayPos = trace.assets[0].pos[XBlockVirtual] - 16ull * list.assets.size();
		for (const std::size_t at : FindAssetLinks(stream, start.offset, s.Cursor(), assetArrayPos, list.assets.size())) {
			if (std::ranges::none_of(log.links, [&](const auto& link) { return std::get<0>(link) == at; })) {
				error = std::format("+0x{:X} holds a link the walk does not log", at - start.offset);
				return false;
			}
		}
		out.bytes.assign(stream.begin() + start.offset, stream.begin() + s.Cursor());
		// A pointer to data loaded earlier leads, in Die Maschine's lighting, into the asset's own data (states sharing
		// a piece): kept as the byte of the copy it points at, placed again when the copy is written.
		for (const auto& [at, value] : log.pointers) {
			const std::uint64_t v = value - 1;
			const int block = static_cast<int>(v >> 60);
			const std::uint64_t offset = v & 0x0FFFFFFFFFFFFFFFull;
			const auto record = std::ranges::find_if(loads, [&](const XStream::LoadRecord& r) {
				return r.block == block && offset >= r.pos && offset < r.pos + r.size;
			});
			if (record == loads.end() || record->cursor < start.offset) {
				error = std::format("the pointer at +0x{:X} leads outside the lighting asset's own data (block {} +0x{:X})",
					at - start.offset, block, offset);
				return false;
			}
			out.pointers.push_back({ at - start.offset, record->cursor + static_cast<std::size_t>(offset - record->pos) - start.offset });
		}
		for (const auto& [at, type, value] : log.links) {
			const std::size_t linked = TracedAssetOfReference(trace, list, value);
			if (linked == SIZE_MAX || list.assets[linked].type != type) {
				error = std::format("the {} link at +0x{:X} (0x{:X}) is not a link to an asset of that type", XAssetTypeName(type),
					at - start.offset, value);
				return false;
			}
			out.links.push_back({ at - start.offset, type, TracedAssetName(trace, stream, linked) });
		}
		out.volumesAt = log.volumesAt == SIZE_MAX ? 0 : log.volumesAt - start.offset;
		out.volumeCount = Get<std::uint32_t>(std::span<const std::uint8_t>(out.bytes), 56);
		if (log.lightsAt != SIZE_MAX) {
			out.lightsAt = log.lightsAt - start.offset;
			out.lightCount = Get<std::uint32_t>(std::span<const std::uint8_t>(out.bytes), 8);
		}
		// The baked shadow trees: each record with the tree it leads to, each stored tree with its stream key's root.
		for (const LightingLog::Shadow& shadow : log.shadows) {
			if (shadow.pointerAt == SIZE_MAX || shadow.pointerAt < start.offset + kShadowRecordSize) {
				continue;
			}
			LightingCopy::ShadowRecord record;
			record.body = shadow.pointerAt - start.offset - kShadowRecordSize;
			record.region = shadow.region;
			if (shadow.tree != SIZE_MAX) {
				LightingCopy::ShadowTree tree;
				tree.at = record.tree = shadow.tree - start.offset;
				tree.nodeCounts = shadow.nodeCounts == SIZE_MAX ? SIZE_MAX : shadow.nodeCounts - start.offset;
				tree.holder = shadow.holder == SIZE_MAX ? SIZE_MAX : shadow.holder - start.offset;
				for (const auto& [at, type, value] : log.links) {
					const std::size_t key = at == shadow.tree && type == 0xB8 ? TracedAssetOfReference(trace, list, value) : SIZE_MAX;
					const std::size_t root = key == SIZE_MAX ? SIZE_MAX : trace.assets[key].offset;
					// A key the zone holds itself: one it only links by name (the top bit of its name) has no root here.
					if (root != SIZE_MAX && root + 56 <= stream.size() && !(Get<std::uint64_t>(stream, root) >> 63)) {
						tree.key = Get<std::uint64_t>(stream, root);
						tree.package = Get<std::uint64_t>(stream, root + 8);
						tree.size = Get<std::uint32_t>(stream, root + 48);
					}
				}
				out.shadowTrees.push_back(tree);
			}
			else {
				const std::size_t field = shadow.pointerAt - start.offset;
				const auto shared = std::ranges::find_if(out.pointers, [&](const LightingCopy::Pointer& p) { return p.at == field; });
				record.tree = shared == out.pointers.end() ? SIZE_MAX : shared->target;
			}
			out.shadowRecords.push_back(record);
		}
		return true;
	}

	std::size_t LightingCopy::DarkenLights() {
		const std::span<std::uint8_t> data(bytes);
		std::size_t lit = 0;
		for (std::uint32_t i = 0; i < lightCount; ++i) {
			const std::size_t light = lightsAt + kLightSize * i;
			if (light + kLightSize > bytes.size()) {
				break;
			}
			bool gaveLight = false;
			for (const std::size_t field : { kLightColor, kLightColorAgain }) {
				for (std::size_t c = 0; c < 3; ++c) {
					gaveLight |= Get<float>(data, light + field + 4 * c) != 0.0f;
					PutAt(data, light + field + 4 * c, 0.0f);
				}
			}
			lit += gaveLight;
		}
		return lit;
	}

	bool LightingCopy::EmptyShadowTrees(std::vector<KeyEdit>& keys, std::size_t& emptied, std::string& error) {
		const std::span<std::uint8_t> data(bytes);
		emptied = 0;
		auto treeAt = [&](std::size_t at) {
			return std::ranges::find_if(shadowTrees, [&](const ShadowTree& tree) { return tree.at == at; });
		};
		auto nodes = [&](const ShadowTree& tree) { return Get<std::uint32_t>(data, tree.at + kShadowTreeNodeCount); };
		for (const bool region : { false, true }) {
			const char* kind = region ? "shadow region" : "sun shadow";
			// The empty tree of this kind to model on, with a record that leads to it.
			const ShadowRecord* model = nullptr;
			const ShadowTree* empty = nullptr;
			std::size_t baked = 0;
			for (const ShadowRecord& record : shadowRecords) {
				const auto tree = record.region == region ? treeAt(record.tree) : shadowTrees.end();
				if (tree == shadowTrees.end()) {
					continue;
				}
				if (nodes(*tree) != 1) {
					++baked;
				}
				else if (!model && tree->nodeCounts != SIZE_MAX && tree->holder != SIZE_MAX && tree->package) {
					model = &record;
					empty = &*tree;
				}
			}
			if (!baked) {
				continue;
			}
			if (!model) {
				error = std::format("its {} trees are baked, and it holds no empty one to model an empty tree on", kind);
				return false;
			}
			std::vector<std::uint8_t> body(bytes.begin() + model->body, bytes.begin() + model->body + kShadowRecordSize);
			for (const ShadowRecord& record : shadowRecords) {
				const auto tree = record.region == region ? treeAt(record.tree) : shadowTrees.end();
				if (tree == shadowTrees.end() || nodes(*tree) == 1) {
					continue;
				}
				if (tree->nodeCounts == SIZE_MAX || tree->holder == SIZE_MAX || !tree->key) {
					error = std::format("a baked {} tree (+0x{:X}) does not store its node counts, its image holder or its stream key "
						"with it", kind, tree->at);
					return false;
				}
				std::copy(body.begin(), body.end(), bytes.begin() + record.body);
			}
			// The trees themselves, once each (several records may share one).
			for (ShadowTree& tree : shadowTrees) {
				const bool ofKind = std::ranges::any_of(shadowRecords, [&](const ShadowRecord& record) {
					return record.region == region && record.tree == tree.at;
				});
				if (!ofKind || nodes(tree) == 1) {
					continue;
				}
				PutAt(data, tree.at + kShadowTreeNodeCount, std::uint32_t(1));
				PutAt(data, tree.nodeCounts, Get<std::uint32_t>(data, empty->nodeCounts));
				for (const std::size_t field : { kShadowHolderUsed, kShadowHolderUsed + 4, kShadowHolderDepth }) {
					PutAt(data, tree.holder + field, Get<std::uint32_t>(data, empty->holder + field));
				}
				keys.push_back({ tree.key, empty->package, empty->size });
				tree.package = empty->package;
				tree.size = empty->size;
				++emptied;
			}
		}
		return true;
	}
}
