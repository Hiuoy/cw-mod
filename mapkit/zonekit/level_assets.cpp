// The level's world assets besides the gfx_map, transcribed from their Load_<Type> functions (addresses in the comments;
// IDB bocw_fixed_renamed.i64, build 1.34.0.15931218, 2026-09-30). See level_assets.hpp.
#include "level_assets.hpp"

namespace MapKit::Zone {
	namespace {
		using Record::Blob3;
		using Record::Chunk;
		using Record::Count;
		using Record::Ctx;
		using Record::Entry;
		using Record::Pixels56;
		using Record::View;

		// --- terraingfx (0xB1): Load_TerrainGfx 0x7FF71E7EB650, a 296-B root ------------------------------------------

		// sub_7FF71E7EB550 (216 B, embedded): +0 bytes x u32 @108 and +72 bytes x u32 @112 (each 128-aligned, loaded when
		// non-null), +120 u16 x u32 @116 (-1 or a reference), +152 a streamkey.
		void TerrainBuffers(Ctx& c, View e) {
			c.Array(e, 0, e.U32(108), 1, 128, "terrain buffer +0");
			c.Array(e, 72, e.U32(112), 1, 128, "terrain buffer +72");
			c.Inline(e, 120, e.U32(116), 2, 2, "terrain buffer +120");
			c.PlainAsset(e, 152, 0xB8, "terrain buffer streamkey");
		}

		// sub_7FF71E7EAD80 (472 B): one terrain tile. +0 u32 n: +8 n 216-B buffers and +16 n 96-B draw materials
		// (Load_GfxWorldDrawMaterials 0x7FF71E7DAB80, the material at +72), both 16-aligned; +32 u16 x u32 @24; +64 a
		// 16-B grid (sub_7FF71E7EBA80: +0 n, +8 16-B rows of u32 @0 x u32 @4 352-B cells, each +0 a 144-B struct holding
		// a material at +72 (sub_7FF71E7EB4A0) and a 216-B buffer at +8); +208 u32 x u32 @200; +352 8 B x u64 @344;
		// +272 304-B entries x u64 @264 (a material, +8 22 B); +392 images x u64 @384 (Load_GfxImagePtrArray
		// 0x7FF71E7D9850); +440 u16 x u64 @432; images at +360, +368, +376, +400, +408, +448, +456.
		void TerrainTile(Ctx& c, View t) {
			c.Array(t, 8, t.U32(0), 216, 16, "tile buffers", [&](const Chunk& buffers) {
				for (std::size_t i = 0; i < t.U32(0) && !c.s.Failed(); ++i) {
					TerrainBuffers(c, Entry(buffers, i, 216));
				}
			});
			c.Array(t, 16, t.U32(0), 96, 16, "tile materials", [&](const Chunk& materials) {
				for (std::size_t i = 0; i < t.U32(0) && !c.s.Failed(); ++i) {
					c.Asset(Entry(materials, i, 96), 72, 0x0A, "tile material");
				}
			});
			c.Inline(t, 32, t.U32(24), 2, 2, "tile +32");
			const View grid = t.Sub(64);
			c.Array(grid, 8, grid.U32(0), 16, 8, "tile grid", [&](const Chunk& rows) {
				for (std::size_t i = 0; i < grid.U32(0) && !c.s.Failed(); ++i) {
					const View row = Entry(rows, i, 16);
					const std::uint64_t cells = static_cast<std::uint64_t>(row.U32(0)) * row.U32(4);
					c.Array(row, 8, cells, 352, 16, "tile grid cells", [&](const Chunk& entries) {
						for (std::size_t k = 0; k < cells && !c.s.Failed(); ++k) {
							const View cell = Entry(entries, k, 352);
							c.Inline(cell, 0, 1, 144, 16, "tile grid cell +0", [&](const Chunk& head) {
								c.Asset(View{ &head, 0 }, 72, 0x0A, "tile grid cell material");
							});
							TerrainBuffers(c, cell.Sub(8));
						}
					});
				}
			});
			c.Array(t, 208, t.U32(200), 4, 4, "tile +208");
			c.Inline(t, 352, t.P(344), 8, 4, "tile +352");
			c.Inline(t, 272, t.P(264), 304, 8, "tile +272", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < t.P(264) && !c.s.Failed(); ++i) {
					const View e = Entry(entries, i, 304);
					c.Asset(e, 0, 0x0A, "tile +272 material");
					c.Inline(e, 8, 22, 1, 2, "tile +272 +8");
				}
			});
			c.Image(t, 360, "tile image");
			c.Image(t, 368, "tile image");
			c.Image(t, 376, "tile image");
			c.Inline(t, 392, t.P(384), 8, 8, "tile images", [&](const Chunk& images) {
				for (std::size_t i = 0; i < t.P(384) && !c.s.Failed(); ++i) {
					c.Image(Entry(images, i, 8), 0, "tile image");
				}
			});
			c.Image(t, 400, "tile image");
			c.Image(t, 408, "tile image");
			c.Inline(t, 440, t.P(432), 2, 2, "tile +440");
			c.Image(t, 448, "tile image");
			c.Image(t, 456, "tile image");
		}

		// Load_ClipMapTerrainEntries 0x7FF71E7EAB70 (the clip map's +160 holds the same): 176-B entries, +24 80-B tile
		// records x u32 @0 (each under pushes of block 9: +40 u16 heights x u16 @32, -1 or a reference; +48 bits,
		// (u16 @32 + 7) / 8 bytes; +56 bytes x u32 @72, -1 or a reference), +152 pointers x u32 @160 (each -1: 24 B,
		// else a reference).
		void TerrainCollision(Ctx& c, const Chunk& entries, std::size_t count) {
			for (std::size_t i = 0; i < count && !c.s.Failed(); ++i) {
				const View e = Entry(entries, i, 176);
				c.Array(e, 24, e.U32(0), 80, 8, "terrain collision tiles", [&](const Chunk& records) {
					for (std::size_t k = 0; k < e.U32(0) && !c.s.Failed(); ++k) {
						const View r = Entry(records, k, 80);
						c.s.Push(XBlockCollisionAlias);
						c.Inline(r, 40, r.U16(32), 2, 2, "terrain collision heights");
						c.s.Pop();
						c.s.Push(XBlockCollisionAlias);
						c.Array(r, 48, (static_cast<std::uint64_t>(r.U16(32)) + 7) >> 3, 1, 1, "terrain collision bits");
						c.s.Pop();
						c.s.Push(XBlockCollisionAlias);
						c.Inline(r, 56, r.U32(72), 1, 1, "terrain collision bytes");
						c.s.Pop();
					}
				});
				c.Array(e, 152, e.U32(160), 8, 8, "terrain collision items", [&](const Chunk& items) {
					for (std::size_t k = 0; k < e.U32(160) && !c.s.Failed(); ++k) {
						c.Inline(Entry(items, k, 8), 0, 1, 24, 8, "terrain collision item");
					}
				});
			}
		}

		// Load_TerrainGfx 0x7FF71E7EB650, under a push of block 4: +16 472-B tiles x u32 @8, +56 images x u64 @48
		// (-1 or a reference), +72 u32 x u64 @64, +152 a 72-B struct (-1 or a reference; its +64 terrain collision
		// entries x u32 @60), +168 64 B x u64 @160, +184 256-B entries x u64 @176 (+112 u32 x u64 @104, +128 u32 x
		// u64 @120, an image at +208), +200 120-B entries x u64 @192 (+24 60 B x u64 @16), +216 64 B x u64 @208, then
		// the +224 block (sub_7FF71E7DFE00, embedded, 64 B): images at +248 and +256, +264 a GfxPixels56 (-1 or a
		// reference), +280 48-B entries of six images x u64 @272 (sub_7FF71E7DAD00; -1 or a reference).
		void TerrainGfxBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.Array(r, 16, r.U32(8), 472, 8, "tiles", [&](const Chunk& tiles) {
				for (std::size_t i = 0; i < r.U32(8) && !c.s.Failed(); ++i) {
					TerrainTile(c, Entry(tiles, i, 472));
				}
			});
			c.Inline(r, 56, r.P(48), 8, 8, "images", [&](const Chunk& images) {
				for (std::size_t i = 0; i < r.P(48) && !c.s.Failed(); ++i) {
					c.Image(Entry(images, i, 8), 0, "terrain image");
				}
			});
			c.Array(r, 72, r.P(64), 4, 4, "+72");
			c.Inline(r, 152, 1, 72, 8, "collision", [&](const Chunk& head) {
				const View h{ &head, 0 };
				c.Array(h, 64, h.U32(60), 176, 8, "collision entries", [&](const Chunk& entries) {
					TerrainCollision(c, entries, h.U32(60));
				});
			});
			c.Array(r, 168, r.P(160), 64, 8, "+168");
			c.Array(r, 184, r.P(176), 256, 8, "+184", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < r.P(176) && !c.s.Failed(); ++i) {
					const View e = Entry(entries, i, 256);
					c.Array(e, 112, e.P(104), 4, 4, "+184 +112");
					c.Array(e, 128, e.P(120), 4, 4, "+184 +128");
					c.Image(e, 208, "+184 image");
				}
			});
			c.Array(r, 200, r.P(192), 120, 8, "+200", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < r.P(192) && !c.s.Failed(); ++i) {
					const View e = Entry(entries, i, 120);
					c.Array(e, 24, e.P(16), 60, 4, "+200 +24");
				}
			});
			c.Array(r, 216, r.P(208), 64, 8, "+216");
			const View maps = r.Sub(224);
			c.Image(maps, 24, "map-wide image");
			c.Image(maps, 32, "map-wide image");
			Pixels56(c, maps, 40, "map-wide pixels");
			c.Inline(maps, 56, maps.P(48), 48, 8, "map-wide image sets", [&](const Chunk& sets) {
				for (std::size_t i = 0; i < maps.P(48) && !c.s.Failed(); ++i) {
					for (std::size_t k = 0; k < 6; ++k) {
						c.Image(Entry(sets, i, 48), 8 * k, "map-wide image set image");
					}
				}
			});
			c.s.Pop();
		}

		// --- staticlevelfxlist (0x7F): 0x7FF71E7EBE60, a 40-B root -----------------------------------------------------
		// Under a push of block 4: +16 80-B placed effects x i32 @8 (sub_7FF71E7EBC70: +0 fx, +8 origin, +20 angles, +64
		// a name, +72 32-B entries x i32 @56 each starting with a string), +32 strings x i32 @24.
		void StaticLevelFxListBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.Array(r, 16, Count(r.I32(8)), 80, 8, "effects", [&](const Chunk& effects) {
				for (std::size_t i = 0; i < Count(r.I32(8)) && !c.s.Failed(); ++i) {
					const View e = Entry(effects, i, 80);
					c.Asset(e, 0, 0x33, "placed effect fx");
					c.XString(e, 64, "placed effect name");
					c.Array(e, 72, Count(e.I32(56)), 32, 8, "placed effect +72", [&](const Chunk& entries) {
						for (std::size_t k = 0; k < Count(e.I32(56)) && !c.s.Failed(); ++k) {
							c.XString(Entry(entries, k, 32), 0, "placed effect +72 string");
						}
					});
				}
			});
			c.Array(r, 32, Count(r.I32(24)), 8, 8, "strings", [&](const Chunk& strings) {
				for (std::size_t i = 0; i < Count(r.I32(24)) && !c.s.Failed(); ++i) {
					c.XString(Entry(strings, i, 8), 0, "string");
				}
			});
			c.s.Pop();
		}

		// --- glasses (0x43): 0x7FF71E7DCC00, an 80-B root ---------------------------------------------------------------
		// Under a push of block 4: +16 144-B glasses x u32 @8 (each +0 a 104-B definition, -1 or a reference: materials
		// at +40, +48 and +56 and effects at +88 and +96, sub_7FF71E7DC9A0; +72 8 B x u8 @69), then under a push of block
		// 9, +24 bytes x u32 @32.
		void GlassesBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.Array(r, 16, r.U32(8), 144, 8, "glasses", [&](const Chunk& glasses) {
				for (std::size_t i = 0; i < r.U32(8) && !c.s.Failed(); ++i) {
					const View g = Entry(glasses, i, 144);
					c.Inline(g, 0, 1, 104, 8, "glass definition", [&](const Chunk& definition) {
						const View d{ &definition, 0 };
						for (const std::size_t o : { 40, 48, 56 }) {
							c.Asset(d, o, 0x0A, "glass material");
						}
						for (const std::size_t o : { 88, 96 }) {
							c.Asset(d, o, 0x33, "glass fx");
						}
					});
					c.Array(g, 72, g.U8(69), 8, 4, "glass +72");
				}
			});
			c.s.Push(XBlockCollisionAlias);
			c.Array(r, 24, r.U32(32), 1, 1, "glass bytes");
			c.s.Pop();
			c.s.Pop();
		}

		// --- com_map (0x19): Load_ComWorld 0x7FF71E7D0EE0, an 88-B root ---------------------------------------------------
		// Under a push of block 4: +16 688-B lights x u32 @12, 16-aligned (Load_LightingLightArray_cand 0x7FF71E7D0E10: a
		// cookie image at +672, as the lighting asset's lights), +32 24-B entries x u32 @24 (+16 20 B x i32 @8), +48 u32
		// x u32 @40, +64 u32 x u32 @56, +80 24-B entries x u32 @72 (+16 28 B x i32 @8).
		// 24-B entries x u32 @count at +pointer, each +16 `stride` B x i32 @8.
		void ComWorldEntries(Ctx& c, View r, std::size_t pointer, std::size_t count, std::size_t stride, const char* name) {
			c.Array(r, pointer, r.U32(count), 24, 8, name, [&](const Chunk& entries) {
				for (std::size_t i = 0; i < r.U32(count) && !c.s.Failed(); ++i) {
					c.Array(Entry(entries, i, 24), 16, Count(Entry(entries, i, 24).I32(8)), stride, 4, "com_map entry list");
				}
			});
		}

		void ComWorldBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.Array(r, 16, r.U32(12), 688, 16, "lights", [&](const Chunk& lights) {
				for (std::size_t i = 0; i < r.U32(12) && !c.s.Failed(); ++i) {
					c.Image(Entry(lights, i, 688), 672, "light cookie image");
				}
			});
			ComWorldEntries(c, r, 32, 24, 20, "com_map +32");
			c.Array(r, 48, r.U32(40), 4, 4, "com_map +48");
			c.Array(r, 64, r.U32(56), 4, 4, "com_map +64");
			ComWorldEntries(c, r, 80, 72, 28, "com_map +80");
			c.s.Pop();
		}

		// --- cpu_occlusion_data (0xA9): 0x7FF71E7D16F0, a 184-B root -----------------------------------------------------
		// Under a push of block 4: +16 80-B entries x u64 @8 (a script string at +28; +40 12 B x u64 @32, +56 u16 x u64
		// @48, +72 u16 x u64 @64), +32 40 B x u64 @24, +48 40 B x u64 @40, +64 script strings x u64 @56, blobs at +72 and
		// +120 (Load_GfxBlob3), then block-2 data: +168 16 B and +176 4 B x u32 @8 (64-aligned).
		void CpuOcclusionDataBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.Array(r, 16, r.P(8), 80, 8, "occluders", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < r.P(8) && !c.s.Failed(); ++i) {
					const View e = Entry(entries, i, 80);
					c.String(e, 28);
					c.Array(e, 40, e.P(32), 12, 4, "occluder +40");
					c.Array(e, 56, e.P(48), 2, 2, "occluder +56");
					c.Array(e, 72, e.P(64), 2, 2, "occluder +72");
				}
			});
			c.Array(r, 32, r.P(24), 40, 8, "occlusion +32");
			c.Array(r, 48, r.P(40), 40, 8, "occlusion +48");
			c.Array(r, 64, r.P(56), 4, 4, "occlusion strings", [&](const Chunk& strings) {
				for (std::size_t i = 0; i < r.P(56); ++i) {
					c.String(Entry(strings, i, 4), 0);
				}
			});
			Blob3(c, r.Sub(72), "occlusion blob");
			Blob3(c, r.Sub(120), "occlusion blob");
			c.Runtime(XBlockRuntime, r, 168, 16ull * r.U32(8), 1, 64, "occlusion +168");
			c.Runtime(XBlockRuntime, r, 176, 4ull * static_cast<std::uint64_t>(r.I32(8)), 1, 64, "occlusion +176");
			c.s.Pop();
		}

		// --- game_map (0x1A): Load_GameWorld 0x7FF71E7D8390, an 80-B root -------------------------------------------------
		// Under a push of block 4: the path data at +8 (sub_7FF71E7E3B60, 56 B embedded: +8 176-B nodes x (u32 @4 + 256),
		// each five script strings at +44..+60 and +0 12 B x u16 @116; block-2 16 B x (u32 @4 + 256); +32 bytes x i32
		// @24; +48 24-B entries x i32 @40: i32 @0 < 0 gives +16 u16 x i32 @8, else +8 and +16 are pointers to earlier
		// data), then +64 and +72 1,703,944 B each.
		constexpr std::size_t kGameWorldBlobSize = 1703944;

		void GameWorldBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			const View path = r.Sub(8);
			const std::uint64_t nodeCount = static_cast<std::uint64_t>(path.U32(4)) + 256;
			c.Array(path, 8, nodeCount, 176, 8, "path nodes", [&](const Chunk& nodes) {
				for (std::size_t i = 0; i < nodeCount && !c.s.Failed(); ++i) {
					const View n = Entry(nodes, i, 176);
					for (std::size_t o = 44; o <= 60; o += 4) {
						c.String(n, o);
					}
					c.Array(n, 0, n.U16(116), 12, 4, "path node +0");
				}
			});
			c.Runtime(XBlockRuntime, path, 16, nodeCount, 16, 16, "path +16");
			c.Array(path, 32, Count(path.I32(24)), 1, 1, "path +32");
			c.Array(path, 48, Count(path.I32(40)), 24, 8, "path +48", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < Count(path.I32(40)) && !c.s.Failed(); ++i) {
					const View e = Entry(entries, i, 24);
					if (e.I32(0) < 0) {
						c.Array(e, 16, Count(e.I32(8)), 2, 2, "path +48 list");
					}
					else {
						c.Ref(e, 8, "path +48 pointer");
						c.Ref(e, 16, "path +48 pointer");
					}
				}
			});
			c.Array(r, 64, 1, kGameWorldBlobSize, 8, "game_map +64");
			c.Array(r, 72, 1, kGameWorldBlobSize, 8, "game_map +72");
			c.s.Pop();
		}

		// --- navvolume (0x76): Load_NavVolumeData 0x7FF71E7E2070, a 24-B root ---------------------------------------------
		// Under a push of block 4: +16 56-B layers x i32 @8, streamkeys at +8 and +32 (the Havok data and its debug data).
		void NavVolumeBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.Array(r, 16, Count(r.I32(8)), 56, 8, "layers", [&](const Chunk& layers) {
				for (std::size_t i = 0; i < Count(r.I32(8)) && !c.s.Failed(); ++i) {
					c.PlainAsset(Entry(layers, i, 56), 8, 0xB8, "navvolume streamkey");
					c.PlainAsset(Entry(layers, i, 56), 32, 0xB8, "navvolume debug streamkey");
				}
			});
			c.s.Pop();
		}

		struct LevelType {
			std::uint64_t type = 0;
			std::size_t rootSize = 0;
			const char* what = nullptr;
			void (*body)(Ctx&, const Chunk&) = nullptr;
		};
		// Every Load_<Type>Asset of these types allocates its root 8-aligned (DB_AllocStreamPos(8) at +0x61 of the
		// generated wrapper).
		constexpr LevelType kLevelTypes[] = {
			{ 0xB1, 296, "terraingfx header", TerrainGfxBody },
			{ 0x7F, 40, "staticlevelfxlist header", StaticLevelFxListBody },
			{ 0x43, 80, "glasses header", GlassesBody },
			{ 0x19, 88, "com_map header", ComWorldBody },
			{ 0xA9, 184, "cpu_occlusion_data header", CpuOcclusionDataBody },
			{ 0x1A, 80, "game_map header", GameWorldBody },
			{ 0x76, 24, "navvolume header", NavVolumeBody },
		};

		const LevelType* FindLevelType(std::uint64_t type) {
			for (const LevelType& level : kLevelTypes) {
				if (level.type == type) {
					return &level;
				}
			}
			return nullptr;
		}

		bool ReadLevelAsset(std::uint64_t type, XStream& s, std::uint64_t header, AssetRecord* out) {
			const LevelType* level = FindLevelType(type);
			s.SetRootType(type);
			return Record::ReadRoot(s, header, out, level->rootSize, 8, level->what,
				[&](Ctx& c, const Chunk& root) { level->body(c, root); });
		}
	}

	bool LoadTerrainGfxAsset(XStream& s, std::uint64_t header) { return ReadLevelAsset(0xB1, s, header, nullptr); }
	bool LoadStaticLevelFxListAsset(XStream& s, std::uint64_t header) { return ReadLevelAsset(0x7F, s, header, nullptr); }
	bool LoadGlassesAsset(XStream& s, std::uint64_t header) { return ReadLevelAsset(0x43, s, header, nullptr); }
	bool LoadComWorldAsset(XStream& s, std::uint64_t header) { return ReadLevelAsset(0x19, s, header, nullptr); }
	bool LoadCpuOcclusionDataAsset(XStream& s, std::uint64_t header) { return ReadLevelAsset(0xA9, s, header, nullptr); }
	bool LoadGameWorldAsset(XStream& s, std::uint64_t header) { return ReadLevelAsset(0x1A, s, header, nullptr); }
	bool LoadNavVolumeAsset(XStream& s, std::uint64_t header) { return ReadLevelAsset(0x76, s, header, nullptr); }

	RecordReader LevelAssetReader(std::uint64_t type) {
		if (!FindLevelType(type)) {
			return {};
		}
		return [type](XStream& s, std::uint64_t header, AssetRecord& out) { return ReadLevelAsset(type, s, header, &out); };
	}

	std::vector<RecordCut> EmptyStaticLevelFxListCuts() {
		return { { "effects", { { 8, 4 } } }, { "strings", { { 24, 4 } } } };
	}

	std::vector<RecordCut> TerrainGfxWithoutTilesCuts() {
		return {
			{ "tiles", { { 8, 4 } } },
			{ "images", { { 48, 8 } } },
			{ "+72", { { 64, 8 } } },
			{ "collision", {} },
			{ "+168", { { 160, 8 } } },
			{ "+184", { { 176, 8 } } },
			{ "+200", { { 192, 8 } } },
			{ "+216", { { 208, 8 } } },
		};
	}
}
