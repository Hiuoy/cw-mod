// gfx_map, transcribed from Load_GfxWorld 0x7FF71E7DB2A0 and the loaders it calls (addresses in the comments;
// IDB bocw_fixed_renamed.i64, build 1.34.0.15931218). See gfx_world.hpp.
#include "gfx_world.hpp"
#include "asset_loaders.hpp"
#include "model_assets.hpp"

#include <algorithm>
#include <format>
#include <map>

namespace MapKit::Zone {
	namespace {
		using Record::Blob3;
		using Record::Chunk;
		using Record::Ctx;
		using Record::Entry;
		using Record::Pixels56;
		using Record::View;

		// sub_7FF71E7D3840 (992 B, a model group; the clip map holds them too): see LoadClipMap992Body in
		// world_assets.cpp, the same transcription with the pointers recorded.
		void ModelGroup(Ctx& c, const Chunk& entry) {
			const View e{ &entry, 0 };
			c.Array(e, 24, 912, 1, 8, "model group +24");
			c.Array(e, 32, 1, 72, 8, "model group +32", [&](const Chunk& head) {
				const View h{ &head, 0 };
				c.Array(h, 0, h.Get<std::uint16_t>(68), 264, 8, "model group +32 entries");
			});
			c.Array(e, 64, e.U8(986), 8, 8, "model group xmodels",
				[&](const Chunk& models) { c.AssetArray(models, e.U8(986), 0x06, "model group xmodel"); });
			c.Array(e, 72, e.U8(986), 2, 2, "model group +72");
			c.Array(e, 80, e.U8(989), 32, 4, "model group +80");
			const View tail = e.Sub(96);
			c.Array(tail, 0, static_cast<std::uint64_t>(32ll * e.Get<std::int16_t>(984)), 1, 16, "model group tail +0");
			c.Array(tail, 8, static_cast<std::uint64_t>((e.Get<std::uint16_t>(976) + e.Get<std::int16_t>(984)) << 6), 1, 4,
				"model group tail +8");
			c.Array(tail, 16, 32ull * e.U8(987), 1, 4, "model group tail +16");
			const std::uint32_t n = e.U32(944);
			c.Array(e, 936, n ? n + 1 : 0, 4, 2, "model group +936");
		}

		// sub_7FF71E7EBFF0 (80 B, draw +536): +16 u32 x u64 @8, +32 u32 x u64 @24, +48 model groups x u64 @40
		// (pointers, each -1 or a reference), +64 xmodels x u64 @56.
		void DrawModels(Ctx& c, View m) {
			c.Array(m, 16, m.P(8), 4, 4, "draw models +16");
			c.Array(m, 32, m.P(24), 4, 4, "draw models +32");
			c.Array(m, 48, m.P(40), 8, 8, "draw model groups", [&](const Chunk& pointers) {
				for (std::size_t i = 0; i < m.P(40) && !c.s.Failed(); ++i) {
					c.Inline(Entry(pointers, i, 8), 0, 1, 992, 8, "model group",
						[&](const Chunk& entry) { ModelGroup(c, entry); });
				}
			});
			c.Array(m, 64, m.P(56), 8, 8, "draw xmodels",
				[&](const Chunk& models) { c.AssetArray(models, m.P(56), 0x06, "draw xmodel"); });
		}

		// sub_7FF71E7D0640: 136-B entries: +32 and +40 xmodels, +56 n / +64 40-B material sets (each two
		// {n, material handles} pairs at +0/+8 and +16/+24), +72 n / +80 48 B, +88 / +96 and +104 / +112 block-2
		// bytes (64-aligned).
		void TerrainModels(Ctx& c, const Chunk& entries, std::size_t count) {
			for (std::size_t i = 0; i < count && !c.s.Failed(); ++i) {
				const View e = Entry(entries, i, 136);
				c.Asset(e, 32, 0x06, "terrain xmodel");
				c.Asset(e, 40, 0x06, "terrain xmodel");
				c.Inline(e, 64, e.P(56), 40, 8, "terrain material sets", [&](const Chunk& sets) {
					for (std::size_t k = 0; k < e.P(56) && !c.s.Failed(); ++k) {
						const View set = Entry(sets, k, 40);
						for (const std::size_t pair : { 0, 16 }) {
							c.Inline(set, pair + 8, set.P(pair), 8, 8, "terrain materials",
								[&](const Chunk& handles) { c.AssetArray(handles, set.P(pair), 0x0A, "terrain material"); });
						}
					}
				});
				c.Inline(e, 80, e.P(72), 48, 4, "terrain model +80");
				c.Runtime(XBlockRuntime, e, 96, e.P(88), 1, 64, "terrain model +96");
				c.Runtime(XBlockRuntime, e, 112, e.P(104), 1, 64, "terrain model +112");
			}
		}

		// sub_7FF71E7D0450: 160-B entries: +8 n / +16 24 B, +24 fx, +40 n / +48 12 B.
		void TerrainLayers(Ctx& c, const Chunk& entries, std::size_t count) {
			for (std::size_t i = 0; i < count && !c.s.Failed(); ++i) {
				const View e = Entry(entries, i, 160);
				c.Inline(e, 16, e.P(8), 24, 4, "terrain layer +16");
				c.Asset(e, 24, 0x33, "terrain layer fx");
				c.Array(e, 48, e.P(40), 12, 4, "terrain layer +48");
			}
		}

		// sub_7FF71E7D09A0 (136 B, draw +688): the terrain's drawing.
		void Terrain(Ctx& c, View owner, std::size_t offset) {
			c.Inline(owner, offset, 1, 136, 8, "terrain", [&](const Chunk& head) {
				const View t{ &head, 0 };
				c.Inline(t, 32, t.P(24), 136, 8, "terrain models",
					[&](const Chunk& entries) { TerrainModels(c, entries, t.P(24)); });
				c.Inline(t, 48, t.P(40), 160, 8, "terrain images", [&](const Chunk& entries) {
					for (std::size_t i = 0; i < t.P(40) && !c.s.Failed(); ++i) {
						const View e = Entry(entries, i, 160);
						c.Asset(e, 0, 0x10, "terrain image");
						c.Inline(e, 152, e.P(144), 4, 4, "terrain image +152");
					}
				});
				c.Inline(t, 64, t.P(56), 160, 8, "terrain layers",
					[&](const Chunk& entries) { TerrainLayers(c, entries, t.P(56)); });
				for (std::size_t o = 72; o <= 112; o += 8) {
					Pixels56(c, t, o, "terrain pixels");
				}
				c.Inline(t, 128, t.P(120), 2, 2, "terrain +128");
			});
		}

		// Load_GfxWorldDraw 0x7FF71E7DBEA0 (784 B at +432).
		void Draw(Ctx& c, View d, View root) {
			for (std::size_t i = 0; i < 4; ++i) {
				c.Runtime(XBlockRuntime, d, 72 + 16 * i, d.U32(64 + 16 * i), 1, 16, "draw buffer A");
			}
			for (std::size_t i = 0; i < 4; ++i) {
				c.Runtime(XBlockRuntime, d, 136 + 16 * i, d.U32(128 + 16 * i), 1, 16, "draw buffer B");
			}
			for (std::size_t i = 0; i < 4; ++i) {
				c.Runtime(XBlockRuntime, d, 200 + 16 * i, d.U32(192 + 16 * i), 1, 16, "draw buffer C");
			}
			for (std::size_t i = 0; i < 3; ++i) {
				c.Array(d, 272 + 24 * i, d.P(264 + 24 * i), 4, 4, "draw u32 list");
			}
			Blob3(c, d.Sub(328), "draw blob 328");
			Blob3(c, d.Sub(376), "draw blob 376");
			Blob3(c, d.Sub(424), "draw blob 424");
			c.Array(d, 472, d.U32(12), 2, 2, "draw +472");
			c.Array(d, 488, d.U32(480), 4, 4, "draw +488");
			c.Array(d, 496, d.U32(484), 4, 4, "draw +496");
			// sub_7FF71E7DAB80: 96-B entries, a material at +72; sized by the root's i32 @16.
			const std::size_t materialCount = static_cast<std::size_t>(std::max(root.I32(16), 0));
			c.Array(d, 512, materialCount, 96, 16, "draw materials", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < materialCount && !c.s.Failed(); ++i) {
					c.Asset(Entry(entries, i, 96), 72, 0x0A, "draw material");
				}
			});
			DrawModels(c, d.Sub(536));
			c.Array(d, 528, d.P(520), 12, 4, "draw +528");
			c.Runtime(XBlockRuntime, d, 624, d.U32(12), 8, 8, "draw +624");
			c.Array(d, 616, d.U32(504), 28, 4, "draw +616");
			c.Array(d, 640, d.U32(632), 1, 1, "draw +640");
			c.Array(d, 656, d.U32(648), 1, 1, "draw +656");
			Pixels56(c, d, 664, "draw pixels 664");
			Pixels56(c, d, 672, "draw pixels 672");
			Pixels56(c, d, 680, "draw pixels 680");
			Terrain(c, d, 688);
			// Group LOD models (0xBB): 72-B entries {model, ..., +40 n / +48 bytes}, then a plain list.
			c.Array(d, 704, d.P(696), 72, 8, "group lod entries", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < d.P(696) && !c.s.Failed(); ++i) {
					const View e = Entry(entries, i, 72);
					c.Asset(e, 0, 0xBB, "group lod model");
					c.Array(e, 48, e.P(40), 1, 1, "group lod entry bytes");
				}
			});
			c.Array(d, 720, d.P(712), 8, 8, "group lod models",
				[&](const Chunk& models) { c.AssetArray(models, d.P(712), 0xBB, "group lod model"); });
			c.Array(d, 760, d.P(752), 1, 1, "draw +760");
			// sub_7FF71E7E1E20 (16 B): 24-B entries {script string, n, u32 x n}.
			const View named = d.Sub(768);
			c.Array(named, 8, named.P(0), 24, 8, "draw named lists", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < named.P(0) && !c.s.Failed(); ++i) {
					const View e = Entry(entries, i, 24);
					c.String(e, 0);
					c.Array(e, 16, e.P(8), 4, 4, "draw named list");
				}
			});
		}

		// sub_7FF71E7F14F0 (160 B, embedded): +28 n / +32 40 B, +40 n / +48 32-B {u32 x8 @0, n @8}, +56 n / +64 40-B
		// {+8 u32 x8, n @16}, +72 material.
		void Volume160(Ctx& c, View v) {
			c.Array(v, 32, static_cast<std::uint64_t>(std::max(v.I32(28), 0)), 40, 4, "volume +32");
			const std::size_t n48 = static_cast<std::size_t>(std::max(v.I32(40), 0));
			c.Array(v, 48, n48, 32, 8, "volume +48", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < n48 && !c.s.Failed(); ++i) {
					const View e = Entry(entries, i, 32);
					c.Array(e, 0, static_cast<std::uint64_t>(std::max(e.I32(8), 0)), 8, 4, "volume +48 list");
				}
			});
			const std::size_t n64 = static_cast<std::size_t>(std::max(v.I32(56), 0));
			c.Array(v, 64, n64, 40, 8, "volume +64", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < n64 && !c.s.Failed(); ++i) {
					const View e = Entry(entries, i, 40);
					c.Array(e, 8, static_cast<std::uint64_t>(std::max(e.I32(16), 0)), 8, 4, "volume +64 list");
				}
			});
			c.Asset(v, 72, 0x0A, "volume material");
		}

		// Load_GfxWorld 0x7FF71E7DB2A0 on the loaded root.
		void Body(Ctx& c, const Chunk& rootChunk, std::string* name) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			if (r.P(8) == kPtrInline) {
				c.s.Alloc(1);
				c.s.LoadString(name);
			}
			else {
				c.Ref(r, 8, "gfx_map name");
			}
			c.Array(r, 184, r.P(176), 12, 4, "+184");
			c.Array(r, 200, r.P(192), 16, 4, "+200");
			// sub_7FF71E7DC540 (168 B at +208): +8 image, +24 12 B / +56 20 B x u32 @16, +96 u16 x u32 @88.
			const View probes = r.Sub(208);
			c.Asset(probes, 8, 0x10, "+208 image");
			c.Array(probes, 24, probes.U32(16), 12, 16, "+232");
			c.Array(probes, 56, probes.U32(16), 20, 16, "+264");
			c.Array(probes, 96, probes.U32(88), 2, 2, "+304");
			c.Array(r, 384, static_cast<std::uint64_t>(std::max(r.I32(376), 0)), 80, 16, "+384");
			c.Runtime(XBlockRuntime, r, 424, r.U32(1216), 8, 4, "+424");
			Draw(c, r.Sub(432), r);
			// Load_GfxWorldRuntimeBuffers (88 B at +1216): block-2 buffers only.
			const View runtime = r.Sub(1216);
			for (std::size_t i = 0; i < 4; ++i) {
				c.Runtime(XBlockRuntime, runtime, 16 + 16 * i, runtime.U32(8 + 16 * i), 1, 16, "runtime buffer");
			}
			c.Runtime(XBlockRuntime, runtime, 80, runtime.U32(72), 1, 16, "runtime buffer");
			c.Asset(r, 1304, 0x0A, "+1304 material");
			c.Asset(r, 1312, 0x0A, "+1312 material");
			for (std::size_t o = 1320; o <= 1344; o += 8) {
				c.Asset(r, o, 0x10, "+1320 image");
			}
			c.Asset(r, 1352, 0x10, "+1352 image");
			c.Asset(r, 1360, 0x10, "+1360 image");
			// +1376: pointers to 72-B structs (sub_7FF71E7E90D0: +40 sanim, +56/+60 script strings).
			c.Array(r, 1376, r.P(1368), 8, 8, "sanims", [&](const Chunk& pointers) {
				for (std::size_t i = 0; i < r.P(1368) && !c.s.Failed(); ++i) {
					c.Inline(Entry(pointers, i, 8), 0, 1, 72, 8, "sanim entry", [&](const Chunk& entry) {
						const View e{ &entry, 0 };
						c.Asset(e, 40, 0x66, "sanim");
						c.String(e, 56);
						c.String(e, 60);
					});
				}
			});
			c.Array(r, 1392, r.U32(1384), 84, 4, "+1392");
			// +1408: the projected decals (216-B entries: a transform and bounds, the decal material +184, a second
			// material +192 (null on zm_silver), a script string +204). zm_silver's 3061 are all vd/ materials:
			// blood, graffiti, snow footprints, ground blends (2026-09-25, named with acts).
			c.Array(r, 1408, r.U32(1400), 216, 8, "decals", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < r.U32(1400) && !c.s.Failed(); ++i) {
					const View e = Entry(entries, i, 216);
					c.Asset(e, 184, 0x0A, "decal material");
					c.Asset(e, 192, 0x0A, "decal material");
					c.String(e, 204);
				}
			});
			c.Runtime(XBlockRuntime, r, 1416, r.U32(1400), 8, 8, "+1416");
			c.Array(r, 1464, r.U32(1456), 88, 8, "+1464", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < r.U32(1456) && !c.s.Failed(); ++i) {
					c.Asset(Entry(entries, i, 88), 72, 0x0A, "+1464 material");
				}
			});
			c.Array(r, 1480, r.U32(1472), 48, 8, "+1480", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < r.U32(1472) && !c.s.Failed(); ++i) {
					c.Asset(Entry(entries, i, 48), 32, 0x0A, "+1480 material");
				}
			});
			c.s.Push(XBlockCollisionAlias);
			c.Array(r, 1488, r.U32(1496), 1, 1, "+1488");
			c.s.Pop();
			c.s.Push(XBlockCollisionAlias);
			c.Array(r, 1504, r.U32(1512), 1, 1, "+1504");
			c.s.Pop();
			c.Asset(r, 1424, 0x10, "+1424 image");
			// sub_7FF71E7F67B0 (32 B at +1600): +4 n / +8 216-B entries (a 160-B volume, +160 n / +168 72 B), +16 n /
			// +24 block-2 bytes.
			const View volumes = r.Sub(1600);
			const std::size_t volumeCount = static_cast<std::size_t>(std::max(volumes.I32(4), 0));
			c.Array(volumes, 8, volumeCount, 216, 8, "volumes", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < volumeCount && !c.s.Failed(); ++i) {
					const View e = Entry(entries, i, 216);
					Volume160(c, e);
					c.Array(e, 168, static_cast<std::uint64_t>(std::max(e.I32(160), 0)), 72, 8, "volume +168");
				}
			});
			c.Runtime(XBlockRuntime, volumes, 24, static_cast<std::uint64_t>(std::max(volumes.I32(16), 0)), 1, 16, "volumes +24");
			// sub_7FF71E7DAC60 (24 B at +1832): block 3 only.
			const View block3 = r.Sub(1832);
			c.Runtime(3, block3, 8, block3.U32(0), 2, 16, "+1840");
			c.Runtime(3, block3, 16, 8ull * block3.U32(4) + 8, 4, 256, "+1848");
			// sub_7FF71E7D9B40 (1432 B at +1856).
			const View big = r.Sub(1856);
			c.Array(big, 1344, big.U32(1320), 2, 2, "+3200");
			c.Array(big, 1352, big.U32(1324), 140, 4, "+3208");
			c.Array(big, 1360, big.U32(1328), 120, 4, "+3216");
			c.Array(big, 1368, big.U32(1332), 216, 4, "+3224");
			c.Array(big, 1376, big.U32(1336), 216, 4, "+3232");
			c.Array(big, 1400, big.U32(1384), 128, 1, "+3256");
			c.Array(big, 1408, big.U32(1388), 128, 1, "+3264");
			c.Array(big, 1416, big.U32(1392), 128, 1, "+3272");
			c.Array(big, 1424, big.U32(1396), 128, 1, "+3280");
			c.Array(r, 7408, r.U32(7400), 64, 4, "+7408");
			c.Array(r, 7424, r.U32(7416), 176, 4, "+7424");
			c.Array(r, 7440, r.U32(7432), 176, 4, "+7440");
			c.Asset(r, 7448, 0xAA, "lighting");
			c.Asset(r, 7456, 0xAC, "streamerworld");
			// +7472: 80-B static models (sub_7FF71E7DA550 on the first 64: +8 reference, +16 xmodel, +48 reference;
			// then an xmodel at +64).
			c.Array(r, 7472, r.P(7464), 80, 8, "static models", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < r.P(7464) && !c.s.Failed(); ++i) {
					const View e = Entry(entries, i, 80);
					c.Ref(e, 8, "static model +8");
					c.Asset(e, 16, 0x06, "static model xmodel");
					c.Ref(e, 48, "static model +48");
					c.Asset(e, 64, 0x06, "static model xmodel");
				}
			});
			c.Array(r, 7488, r.U32(7480), 442, 2, "+7488");
			c.Array(r, 7504, r.U32(7496), 4084, 4, "+7504");
			c.Asset(r, 7520, 0x10, "+7520 image");
			c.Array(r, 7536, r.U32(7528), 836, 4, "+7536");
			c.Array(r, 7552, r.U32(7544), 4048, 4, "+7552");
			c.Array(r, 7568, r.U32(7560), 688, 4, "+7568");
			c.Array(r, 7584, r.U32(7576), 512, 4, "+7584");
			c.Array(r, 7600, r.U32(7592), 1084, 4, "+7600");
			c.Array(r, 7616, r.U32(7608), 148, 4, "+7616");
			c.Array(r, 7632, r.U32(7624), 96, 4, "+7632");
			c.Array(r, 7648, r.U32(7640), 20, 4, "+7648");
			for (std::size_t i = 0; i < 15; ++i) {
				c.Array(r, 7656 + 16 * i + 8, r.U32(7656 + 16 * i), 64, 1, "+7656 blob");
			}
			for (std::size_t i = 0; i < 10; ++i) {
				c.Asset(r, 7896 + 8 * i, 0x10, "+7896 image");
			}
			c.s.Pop();
		}

		// `world`, when given, is `out` itself: the gfx_map's root and name go there too.
		bool ReadImpl(XStream& s, std::uint64_t header, AssetRecord* out, GfxWorld* world) {
			Ctx c{ s, out };
			if (out) {
				out->begin = s.Cursor();
				out->startPositions = s.Positions();
				out->startBlock = s.Block();
			}
			// Load_GfxMapAsset: the root in the temp block, 16-aligned.
			s.Push(XBlockTemp);
			if (header == kPtrInline || header == kPtrInsert) {
				if (header == kPtrInsert) {
					s.Insert();
				}
				Chunk root = c.Load(kGfxWorldRootSize, 16);
				s.CountRootCopy(kGfxWorldRootSize);
				s.PreloadRoot(0x1B, 16, kGfxWorldRootSize);
				if (!s.Failed()) {
					if (out) {
						out->rootAt = root.at;
					}
					if (world) {
						std::copy(root.data.begin(), root.data.end(), world->root.begin());
					}
					Body(c, root, world ? &world->name : nullptr);
				}
			}
			else if (header != kPtrNull) {
				s.Reference(header, "gfx_map header");
			}
			s.Pop();
			s.PreloadFieldEnd(0x1B);
			s.SetRootType(XStream::kNoRootType);
			if (out) {
				out->end = s.Cursor();
			}
			return !s.Failed();
		}

		// Load_Sanim body (sub_7FF71E7DA160, 112 B): +0 name, +16 128 B {+0 block-6 bytes x u32 @112, 64 KB
		// aligned}, +24 12 B x u16 @98, +32 28 B x u16 @104; all under a push of block 4. `strings` (if given) gets
		// the stream offsets of the zone-local script-string indices it converts (DB_ResolveScriptStringIndex):
		// root +92, +8 of each +24 entry, +16/+20/+24 of each +32 entry.
		void SAnimBody(XStream& s, std::span<const std::uint8_t> a, std::vector<std::size_t>* strings = nullptr) {
			const std::size_t rootAt = s.Cursor() - a.size();
			s.Push(XBlockVirtual);
			LoadXString(s, Get<std::uint64_t>(a, 0), "sanim name");
			if (Get<std::uint64_t>(a, 16)) {
				s.Alloc(8);
				std::vector<std::uint8_t> head(128);
				if (s.Load(head.data(), head.size())) {
					s.Push(XBlockPhysical);
					if (Get<std::uint64_t>(head, 0)) {
						s.Alloc(0x10000);
						s.Load(nullptr, Get<std::uint32_t>(head, 112));
					}
					s.Pop();
				}
			}
			const std::size_t at24 = s.Cursor();
			LoadArray(s, Get<std::uint64_t>(a, 24), Get<std::uint16_t>(a, 98), 12, 4);
			const std::size_t at32 = s.Cursor();
			LoadArray(s, Get<std::uint64_t>(a, 32), Get<std::uint16_t>(a, 104), 28, 4);
			s.Pop();
			if (strings) {
				strings->push_back(rootAt + 92);
				for (std::size_t i = 0; Get<std::uint64_t>(a, 24) && i < Get<std::uint16_t>(a, 98); ++i) {
					strings->push_back(at24 + 12 * i + 8);
				}
				for (std::size_t i = 0; Get<std::uint64_t>(a, 32) && i < Get<std::uint16_t>(a, 104); ++i) {
					for (std::size_t field : { 16, 20, 24 }) {
						strings->push_back(at32 + 28 * i + field);
					}
				}
			}
		}
	}

	bool LoadGfxWorldAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x1B);
		return ReadImpl(s, header, nullptr, nullptr);
	}

	bool ReadGfxWorld(XStream& s, std::uint64_t header, GfxWorld& out) {
		out = {};
		return ReadImpl(s, header, &out, &out);
	}

	// sub_7FF71E7D9320 (40 B): under a push of block 4, +8 xmodel, +16 n / +24 xmodelmeshes.
	bool LoadGroupLodModelAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0xBB);
		return LoadAssetHeader(s, header, 40, 8, [&](std::span<std::uint8_t> model) {
			s.Push(XBlockVirtual);
			LoadXModelAsset(s, Get<std::uint64_t>(model, 8));
			const auto meshes = LoadArray(s, Get<std::uint64_t>(model, 24), Get<std::uint64_t>(model, 16), 8, 8);
			for (std::size_t i = 0; i + 8 <= meshes.size() && !s.Failed(); i += 8) {
				LoadXModelMeshAsset(s, Get<std::uint64_t>(meshes, i));
			}
			s.Pop();
		});
	}

	bool LoadSAnimAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x66);
		return LoadAssetHeader(s, header, 112, 8, [&](std::span<std::uint8_t> a) { SAnimBody(s, a); });
	}

	std::optional<std::vector<std::size_t>> SAnimScriptStrings(std::span<const std::uint8_t> bytes) {
		XStream s(bytes, 0);
		s.Detach();
		std::vector<std::size_t> strings;
		const bool read = LoadAssetHeader(s, kPtrInline, 112, 8, [&](std::span<std::uint8_t> a) { SAnimBody(s, a, &strings); });
		if (!read || s.Cursor() != bytes.size()) {
			return std::nullopt;
		}
		return strings;
	}

	bool EncodeGfxWorld(XWriter& w, const GfxWorldSplice& splice, std::string& error) {
		return EncodeRecordedAsset(w, splice, [](XStream& s, std::uint64_t header, AssetRecord& out) {
			out = {};
			return ReadImpl(s, header, &out, nullptr);
		}, error);
	}
}
