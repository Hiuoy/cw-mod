// The asset library's assets a map's copies link, transcribed from their Load_<Type> functions (addresses in the comments;
// IDB bocw_fixed_renamed.i64, build 1.34.0.15931218, 2026-10-01). See library_assets.hpp.
#include "library_assets.hpp"

namespace MapKit::Zone {
	namespace {
		using Record::Chunk;
		using Record::Count;
		using Record::Ctx;
		using Record::Entry;
		using Record::View;

		constexpr int XBlockResident = 12; // mesh data kept in the .ff (Load_XModelMeshSurfaceData)

		// --- image (0x10): Load_ImageAsset 0x7FF71E7D97A0 -> Load_GfxImage 0x7FF71E7D95E0, a 208-B root ------------------
		void ImageBody(Ctx& c, const Chunk& rootChunk) {
			c.ImageBody(View{ &rootChunk, 0 });
		}

		// --- streamkey (0xB8): Load_StreamkeyAsset 0x7FF71E7ECFC0 -> Load_StreamKeyData 0x7FF71E7ECF00, a 56-B root --------
		// Under a push of block 4: +32 data, u32 @48 bytes, loaded when non-null where the flags @55 say: & 3 == 0 block 7
		// (when DB_ShouldLoadStreamedBlock 0x7FF72686C7C0), & 1 and not & 0x22 block 8, & 2 the current block (the only
		// case the stream stores), & 1 and & 0x20 block 8 64-KB aligned. Blocks 7 and 8 are never stored, so a streamkey
		// usually travels as its root alone: the data streams from the packages by its key.
		void StreamKeyBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			const std::uint8_t flags = r.U8(55);
			const std::uint64_t size = r.U32(48);
			if ((flags & 3) == 0) {
				c.Runtime(XBlockStreamed, r, 32, size, 1, 256, "streamkey data");
			}
			else if ((flags & 1) && !(flags & 0x22)) {
				c.Runtime(8, r, 32, size, 1, 256, "streamkey data");
			}
			else if (flags & 2) {
				c.Array(r, 32, size, 1, 256, "streamkey data");
			}
			else if (flags & 0x20) {
				c.Runtime(8, r, 32, size, 1, 0x10000, "streamkey data");
			}
			c.s.Pop();
		}

		// --- xskeleton (0x08): Load_XskeletonAsset 0x7FF71E7F9180 -> Load_XSkeleton 0x7FF71E7F8EB0, an 88-B root ----------
		// Under a push of block 4, each -1 or a reference. Bones: u16 @72 cosmetic, u16 @74 others, u16 @76 roots. +8 a
		// script string per bone (u32, 4-aligned), +16 8 B per non-cosmetic bone with a script string at +0 (4-aligned):
		// both resolved one by one (DB_ResolveScriptStringIndexLazy 0x7FF72966F070). Per bone that is not a root: +24 2 B
		// (2-aligned), +32 8 B (8-aligned), +40 16 B (4-aligned). Per bone: +48 1 B, +56 32 B (16-aligned). +64 40 B per
		// non-cosmetic bone (4-aligned).
		void XSkeletonBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			const std::size_t bones = r.U16(74);
			const std::size_t all = static_cast<std::size_t>(r.U16(72)) + bones;
			const std::size_t parented = all - r.U16(76);
			c.Inline(r, 8, all, 4, 4, "skeleton bone names", [&](const Chunk& names) {
				for (std::size_t i = 0; i < all; ++i) {
					c.String(Entry(names, i, 4), 0);
				}
			});
			c.Inline(r, 16, bones, 8, 4, "skeleton +16", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < bones; ++i) {
					c.String(Entry(entries, i, 8), 0);
				}
			});
			c.Inline(r, 24, parented, 2, 2, "skeleton parents");
			c.Inline(r, 32, parented, 8, 8, "skeleton rotations");
			c.Inline(r, 40, parented, 16, 4, "skeleton translations");
			c.Inline(r, 48, all, 1, 1, "skeleton part classification");
			c.Inline(r, 56, all, 32, 16, "skeleton base matrices");
			c.Inline(r, 64, bones, 40, 4, "skeleton +64");
			c.s.Pop();
		}

		// --- xmodelmesh (0x09): Load_XmodelmeshAsset 0x7FF71E7F8BF0 -> Load_XModelMesh 0x7FF71E7F8A50, a 64-B root ------

		// Load_XModelMeshSurfaceData 0x7FF71E7F93B0 (464 B, after the caller's Alloc(16)). flags @0 & 1 = streamed: +32
		// data (u32 @40 bytes) in block 7 (when DB_ShouldLoadStreamedBlock; never stored), loaded when non-null; else in
		// block 12, resident in the .ff, -1 or a reference. Then a script string at +436, and per (u32 @428 + u32 @432):
		// +440 a script string (u32), +448 8 B with a script string at +0, +456 8 B; each -1 or a reference, 4-aligned.
		// Its strings are resolved one by one (DB_ResolveScriptStringIndex 0x7FF72966F0E0).
		void MeshInfoBody(Ctx& c, const View& info) {
			const std::uint64_t size = info.U32(40);
			if (info.U32(0) & 1) {
				c.Runtime(XBlockStreamed, info, 32, size, 1, 256, "mesh data");
			}
			else {
				c.s.Push(XBlockResident);
				c.Inline(info, 32, size, 1, 256, "mesh data");
				c.s.Pop();
			}
			c.String(info, 436);
			const std::size_t n = static_cast<std::uint32_t>(info.U32(428) + info.U32(432));
			c.Inline(info, 440, n, 4, 4, "mesh info +440", [&](const Chunk& strings) {
				for (std::size_t i = 0; i < n; ++i) {
					c.String(Entry(strings, i, 4), 0);
				}
			});
			c.Inline(info, 448, n, 8, 4, "mesh info +448", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < n; ++i) {
					c.String(Entry(entries, i, 8), 0);
				}
			});
			c.Inline(info, 456, n, 8, 4, "mesh info +456");
		}

		void MeshInfo(Ctx& c, const View& owner, std::size_t offset, const char* name) {
			c.Inline(owner, offset, 1, 464, 16, name, [&](const Chunk& info) { MeshInfoBody(c, View{ &info, 0 }); });
		}

		// Load_XModelMeshPart 0x7FF71E7F9290 (40 B): +0 an XString, +16 32 B x u16 @10 (4-aligned), +24 3 B x u16 @8
		// (1-aligned), +32 24 B x u16 @12 (4-aligned), each -1 or a reference.
		void MeshPartBody(Ctx& c, const View& part) {
			c.XString(part, 0, "mesh part name");
			c.Inline(part, 16, part.U16(10), 32, 4, "mesh part +16");
			c.Inline(part, 24, part.U16(8), 3, 1, "mesh part +24");
			c.Inline(part, 32, part.U16(12), 24, 4, "mesh part +32");
		}

		// One LOD. Under a push of block 4: +24 96 B (-1 or a reference, 4-aligned), +16 a mesh info, +8 surfaces (48 B x
		// u16 @60, 8-aligned, loaded when non-null). Per surface: +24 a mesh part (8-aligned, loaded when non-null), +16 a
		// mesh info, +32 96 B (-1 or a reference, 4-aligned).
		void XModelMeshBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.Inline(r, 24, 1, 96, 4, "lod +24");
			MeshInfo(c, r, 16, "lod mesh info");
			const std::size_t surfaces = r.U16(60);
			c.Array(r, 8, surfaces, 48, 8, "surfaces", [&](const Chunk& array) {
				for (std::size_t i = 0; i < surfaces && !c.s.Failed(); ++i) {
					const View surface = Entry(array, i, 48);
					c.Array(surface, 24, 1, 40, 8, "mesh part", [&](const Chunk& part) { MeshPartBody(c, View{ &part, 0 }); });
					MeshInfo(c, surface, 16, "surface mesh info");
					c.Inline(surface, 32, 1, 96, 4, "surface +32");
				}
			});
			c.s.Pop();
		}

		// --- xmodel (0x06): Load_XmodelAsset 0x7FF71E7F8D50 -> Load_XModel 0x7FF71E7F8420, a 232-B root ----------------
		// Under a push of block 4: +8 a skeleton, +16 an xcollision, +32 eight LODs (xmodelmesh), each a nested asset; +96
		// one 32-B table per LOD (u16 @112, 8-aligned, loaded when non-null): +0 u16 surfaces, +8 and +16 material handles
		// (Load_MaterialHandleArray 0x7FF71E7E0A10: 8 B each, 8-aligned, -1 or a reference), +24 4 B per surface (-1 or a
		// reference); +104 96 B (-1 or a reference, 4-aligned); +24 attached models (40 B x i8 @222, 8-aligned, loaded when
		// non-null): a nested xmodel at +0 and a script string at +8 (DB_ResolveScriptStringIndexLazy).
		void XModelBody(Ctx& c, const Chunk& rootChunk) {
			const View m{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.Asset(m, 8, 0x08, "xmodel skeleton");
			c.Asset(m, 16, 0x07, "xmodel collision");
			for (std::size_t i = 0; i < 8 && !c.s.Failed(); ++i) {
				c.Asset(m, 32 + 8 * i, 0x09, "xmodel lod");
			}
			const std::size_t lods = m.U16(112);
			c.Array(m, 96, lods, 32, 8, "lod material tables", [&](const Chunk& tables) {
				for (std::size_t i = 0; i < lods && !c.s.Failed(); ++i) {
					const View table = Entry(tables, i, 32);
					const std::size_t count = table.U16(0);
					for (const std::size_t field : { 8, 16 }) {
						c.Inline(table, field, count, 8, 8, "lod material handles", [&](const Chunk& handles) {
							c.AssetArray(handles, count, 0x0A, "lod material");
						});
					}
					c.Inline(table, 24, count, 4, 4, "lod material +24");
				}
			});
			c.Inline(m, 104, 1, 96, 4, "xmodel +104");
			const auto attached = static_cast<std::int8_t>(m.U8(222));
			if (m.P(24) && attached < 0) {
				c.s.Fail("xmodel attachment count is negative");
			}
			const std::size_t attachments = attached < 0 ? 0 : static_cast<std::size_t>(attached);
			c.Array(m, 24, attachments, 40, 8, "attached models", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < attachments && !c.s.Failed(); ++i) {
					c.Asset(Entry(entries, i, 40), 0, 0x06, "attached model");
					c.String(Entry(entries, i, 40), 8);
				}
			});
			c.s.Pop();
		}

		// --- xcollision (0x07): Load_XcollisionAsset 0x7FF71E7F81B0 -> Load_XCollision 0x7FF71E7F7FE0, a 96-B root -------
		// Under a push of block 4: +8 a skeleton and +16 a streamkey (its collision data streams by it), each a nested asset;
		// +24 the collision data (Load_XCollisionData 0x7FF71E7F8260, 40 B, loaded when non-null); +40 a physpreset, +48
		// trigger actions (an 88-B asset of type 0xD8), +56 physconstraints, each a nested asset. The collision data and the
		// trigger actions are not recorded yet, so a collision holding either cannot be copied. Die Maschine's power switch
		// (p9_zm_ndu_power_on_switch) holds neither: its root, its own skeleton and its streamkey.
		void XCollisionBody(Ctx& c, const Chunk& rootChunk) {
			const View x{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.Asset(x, 8, 0x08, "xcollision skeleton");
			c.PlainAsset(x, 16, 0xB8, "xcollision streamkey");
			if (x.P(24) && !c.s.Failed()) {
				c.s.Fail("the xcollision holds collision data, which mapkit does not record yet");
			}
			c.Asset(x, 40, 0x02, "xcollision physpreset");
			if (x.P(48) && !c.s.Failed()) {
				c.s.Fail("the xcollision has trigger actions, which mapkit does not record yet");
			}
			c.Asset(x, 56, 0x03, "xcollision physconstraints");
			c.s.Pop();
		}

		// --- physpreset (0x02): Load_PhyspresetAsset 0x7FF71E7E4340, a 112-B root --------------------------------------
		// Under a push of block 4: +88 an fx, +96 an impactsfxtable, +104 an impactsoundstable, each a nested asset; no
		// strings. A model that physics can move links one through its xcollision (+40): the first build of zm_debug failed
		// on one (2026-10-03), the only copy that build could not make.
		void PhysPresetBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.Asset(r, 88, 0x33, "physpreset fx");
			c.Asset(r, 96, 0x36, "physpreset impact fx");
			c.Asset(r, 104, 0x37, "physpreset impact sounds");
			c.s.Pop();
		}

		// --- material (0x0A): Load_MaterialAsset 0x7FF71E7E0960 -> Load_Material 0x7FF71E7DFFB0, a 344-B root -----------
		// Under a push of block 4, in the order model_assets.cpp's LoadMaterialBody reads:
		//   the embedded block at +128 (0x7FF71E7E0590): thirteen 64-B records at +40 (0x7FF71E7E14F0; -1 or a reference,
		//   8-aligned), each with eight 12-B vectors (-1 or a reference, 4-aligned), then under a push of block 6 its +152
		//   constant buffer, u64 @144 bytes (-1 or a reference, 256-aligned);
		//   +40 a techset (a nested asset);
		//   +48 the image table (0x7FF71E7E1A40): 24 B x u8 @328 (-1 or a reference, 8-aligned), an image at +0 of each;
		//   +56 8 B x u8 @329 (-1 or a reference, 16-aligned);
		//   +64 eight texture sets (0x7FF71E7E0820; 24 B, -1 or a reference, 8-aligned): +0 4 B x u32 @16 (64-aligned),
		//   +8 16 B x u32 @20 (8-aligned) with an image at +0 of each, both -1 or a reference;
		//   the embedded block at +288 (0x7FF71E7E7C50): +8 24 B x u32 @16, +24 8 B x u32 @32 (-1 or a reference, 4-aligned);
		//   +336 a material-local techset (0x7FF71E7E0AC0, 24 B): not recorded yet, so a material with one cannot be copied.
		// No field of a material is a script string.
		void MaterialBody(Ctx& c, const Chunk& rootChunk) {
			const View m{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			const View block = m.Sub(128);
			for (std::size_t i = 0; i < 13 && !c.s.Failed(); ++i) {
				c.Inline(block, 40 + 8 * i, 1, 64, 8, "material record", [&](const Chunk& record) {
					for (std::size_t v = 0; v < 8; ++v) {
						c.Inline(View{ &record, 0 }, 8 * v, 1, 12, 4, "material record vector");
					}
				});
			}
			c.s.Push(XBlockPhysical);
			c.Inline(block, 152, block.P(144), 1, 256, "material constant buffer");
			c.s.Pop();
			c.Asset(m, 40, 0x0F, "material techset");
			const std::size_t images = m.U8(328);
			c.Inline(m, 48, images, 24, 8, "material image table", [&](const Chunk& table) {
				for (std::size_t i = 0; i < images && !c.s.Failed(); ++i) {
					c.Image(Entry(table, i, 24), 0, "material image");
				}
			});
			c.Inline(m, 56, m.U8(329), 8, 16, "material +56");
			for (std::size_t t = 0; t < 8 && !c.s.Failed(); ++t) {
				c.Inline(m, 64 + 8 * t, 1, 24, 8, "material texture set", [&](const Chunk& setChunk) {
					const View set{ &setChunk, 0 };
					c.Inline(set, 0, set.U32(16), 4, 64, "material set data");
					const std::size_t entries = set.U32(20);
					c.Inline(set, 8, entries, 16, 8, "material set images", [&](const Chunk& list) {
						for (std::size_t i = 0; i < entries && !c.s.Failed(); ++i) {
							c.Image(Entry(list, i, 16), 0, "material set image");
						}
					});
				});
			}
			const View tail = m.Sub(288);
			c.Inline(tail, 8, tail.U32(16), 24, 4, "material +296");
			c.Inline(tail, 24, tail.U32(32), 8, 4, "material +312");
			if (m.P(336) && !c.s.Failed()) {
				c.s.Fail("the material has a material-local techset, which mapkit does not record yet");
			}
			c.s.Pop();
		}

		// --- klf (0x35): Load_KlfAsset 0x7FF71E7D7340 -> Load_Klf 0x7FF71E7D75A0, an 88-B root ---------------------------

		// A Load_GfxPixels56 0x7FF71E7DA0D0 field as the klf loads it: 56 B (16-aligned, loaded when non-null), then under a
		// push of block 6 its +0 pixels, u32 @8 x u32 @12 bytes (-1 or a reference, 256-aligned).
		void KlfPixels(Ctx& c, const View& owner, std::size_t offset, const char* name) {
			c.Array(owner, offset, 1, 56, 16, name, [&](const Chunk& head) {
				const View h{ &head, 0 };
				c.s.Push(XBlockPhysical);
				c.Inline(h, 0, static_cast<std::uint64_t>(h.U32(8)) * h.U32(12), 1, 256, "klf pixels data");
				c.s.Pop();
			});
		}

		// Under a push of block 4: +0 an XString (not the name: the hash is at +16), +24 712-B entries x u16 @64
		// (Load_KlfEntryArray_cand 0x7FF71E7D73F0, 8-aligned, loaded when non-null: +0 an XString, +648 pixels, +656 an
		// image); under a push of block 6, +32 424 B x u16 @64 and +40 8 B x u16 @66 (-1 or a reference, 16-aligned); +48
		// and +56 pixels; +72 an image.
		void KlfBody(Ctx& c, const Chunk& rootChunk) {
			const View k{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.XString(k, 0, "klf string");
			const std::size_t entries = k.U16(64);
			c.Array(k, 24, entries, 712, 8, "klf entries", [&](const Chunk& array) {
				for (std::size_t i = 0; i < entries && !c.s.Failed(); ++i) {
					const View entry = Entry(array, i, 712);
					c.XString(entry, 0, "klf entry string");
					KlfPixels(c, entry, 648, "klf entry pixels");
					c.Image(entry, 656, "klf entry image");
				}
			});
			c.s.Push(XBlockPhysical);
			c.Inline(k, 32, entries, 424, 16, "klf +32");
			c.s.Pop();
			c.s.Push(XBlockPhysical);
			c.Inline(k, 40, k.U16(66), 8, 16, "klf +40");
			c.s.Pop();
			KlfPixels(c, k, 48, "klf pixels +48");
			KlfPixels(c, k, 56, "klf pixels +56");
			c.Image(k, 72, "klf image");
			c.s.Pop();
		}

		// --- winddef (0xD3): Load_WinddefAsset 0x7FF71E7F65D0, a 128-B root (16-aligned) and nothing else -----------------
		void WindDefBody(Ctx& c, const Chunk&) {
			c.s.Push(XBlockVirtual);
			c.s.Pop();
		}

		// --- the sound family: the level's sound bank and what it holds (docs/mapkit-plan.md, "Ambient rooms") ---
		// asset_loaders.cpp walks the same (LoadSoundBank and the loaders above it). A bank stores its sound assets, alias
		// modifiers, ducks and acoustics inline, each a nested asset recorded with the bank (Ctx::Nested).
		void StreamKeyField(Ctx& c, const View& owner, std::size_t offset, const char* what) {
			c.Nested(owner, offset, 0xB8, 56, 8, what, [&](const Chunk& key) { StreamKeyBody(c, key); });
		}

		// sound_asset (0x13): Load_SoundAssetAsset 0x7FF71E7E9950, an 88-B root. Under a push of block 4: +32 u32 @24 bytes
		// (1-aligned) and +48 u32 @40 bytes (4-aligned), each loaded when non-null; +64 a streamkey (the audio streams from
		// the packages by it).
		void SoundAssetBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.Array(r, 32, r.U32(24), 1, 1, "sound asset +32");
			c.Array(r, 48, r.U32(40), 1, 4, "sound asset +48");
			StreamKeyField(c, r, 64, "sound asset streamkey");
			c.s.Pop();
		}

		// sound_alias_modifier (0x15): Load_SoundAliasModifierAsset 0x7FF71E7E98A0 -> Load_SoundAliasModifier 0x7FF71E7E9730,
		// a 152-B root, also inline in an alias's params. Under a push of block 4: seven arrays of 8 B at +16 .. +112, each
		// with its count after the pointer (4-aligned, loaded when non-null).
		void SoundAliasModifierBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			for (std::size_t field = 16; field <= 112 && !c.s.Failed(); field += 16) {
				c.Array(r, field, r.U32(field + 8), 8, 4, "sound alias modifier array");
			}
			c.s.Pop();
		}

		// sound_duck (0x14): Load_SoundDuckAsset 0x7FF71E7EA1C0, a 1632-B root (32-aligned) and nothing below it.
		void SoundDuckBody(Ctx& c, const Chunk&) {
			c.s.Push(XBlockVirtual);
			c.s.Pop();
		}

		// sound_acoustics (0x16): Load_SoundAcousticsAsset 0x7FF71E7E94F0 -> 0x7FF71E7E9260, a 352-B root. Under a push of
		// block 4: +8 an XString, +24 a streamkey; the 56-B struct at +40 (0x7FF71E7E9190): +8 u32 @0 bytes (16-aligned),
		// +32 64-B entries x u32 @16 (16-aligned) with a streamkey at +40 of each, +40 a streamkey; then nine arrays of 8 B
		// at +128 .. +304 and 84-B entries at +336 (4-aligned), each with its u32 count after the pointer. Every array is
		// loaded when non-null. Microsoft Project Acoustics (Triton) data, baked from the level's geometry
		// (SND_Triton_LoadBankAcoustics 0x7FF729992B50).
		void SoundAcousticsBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			c.XString(r, 8, "acoustics name");
			StreamKeyField(c, r, 24, "acoustics streamkey");
			const View inner = r.Sub(40);
			c.Array(inner, 8, inner.U32(0), 1, 16, "acoustics +48");
			const std::size_t keys = inner.U32(16);
			c.Array(inner, 32, keys, 64, 16, "acoustics +72", [&](const Chunk& entries) {
				for (std::size_t i = 0; i < keys && !c.s.Failed(); ++i) {
					StreamKeyField(c, Entry(entries, i, 64), 40, "acoustics +72 streamkey");
				}
			});
			StreamKeyField(c, inner, 40, "acoustics +80 streamkey");
			for (const std::size_t field : { 128, 144, 160, 184, 224, 240, 256, 280, 304 }) {
				c.Array(r, field, r.U32(field + 8), 8, 4, "acoustics array");
			}
			c.Array(r, 336, r.U32(344), 84, 4, "acoustics +336");
			c.s.Pop();
		}

		// sound_bank (0x12): Load_SoundBankAsset 0x7FF71E7E9D50 -> Load_SoundBank 0x7FF71E7E9A80, a 112-B root. Under a push
		// of block 4:
		//   +40 32-B alias lists x u32 @32 (8-aligned, loaded when non-null), each with +8 its 96-B aliases x i32 @16
		//   (Load_SoundAliasArray 0x7FF71E7E95A0; 8-aligned, -1 or a reference). Per alias: three sound assets at +32, +40
		//   and +48; +80 a 208-B params block (8-aligned, -1 or a reference) with a sound_alias_modifier at +0 (a nested
		//   asset under a push of block 0, Load_SoundAliasModifier 0x7FF71E7E9730) and a script string at +80, resolved
		//   whatever +0 holds (DB_ResolveScriptStringIndex, the bank's only one); +88 8 B (1-aligned, -1 or a reference);
		//   +48 4 B x u32 @32 (2-aligned); +64 the ducks, an 8-B pointer x u32 @56 (8-aligned), each a sound_duck (0x14: a
		//   1632-B root, 32-aligned, nothing below it); +80 136-B records x u32 @72; +96 88-B records x u32 @88, each with
		//   +80 288-B entries x u32 @72, each with +272 128-B entries x u32 @264 (all 8-aligned, loaded when non-null);
		//   +104 the acoustics.
		void SoundBankBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			c.s.Push(XBlockVirtual);
			const std::size_t lists = r.U32(32);
			c.Array(r, 40, lists, 32, 8, "sound alias lists", [&](const Chunk& listArray) {
				for (std::size_t i = 0; i < lists && !c.s.Failed(); ++i) {
					const View list = Entry(listArray, i, 32);
					const std::size_t aliases = Count(list.I32(16));
					c.Inline(list, 8, aliases, 96, 8, "sound aliases", [&](const Chunk& aliasArray) {
						for (std::size_t a = 0; a < aliases && !c.s.Failed(); ++a) {
							const View alias = Entry(aliasArray, a, 96);
							for (const std::size_t field : { 32, 40, 48 }) {
								c.Nested(alias, field, 0x13, 88, 8, "sound asset", [&](const Chunk& sound) { SoundAssetBody(c, sound); });
							}
							c.Inline(alias, 80, 1, 208, 8, "sound alias params", [&](const Chunk& params) {
								c.Nested(View{ &params, 0 }, 0, 0x15, 152, 8, "sound alias modifier",
									[&](const Chunk& modifier) { SoundAliasModifierBody(c, modifier); });
								c.String(View{ &params, 0 }, 80);
							});
							c.Inline(alias, 88, 1, 8, 1, "sound alias +88");
						}
					});
				}
			});
			c.Array(r, 48, r.U32(32), 4, 2, "sound bank +48");
			const std::size_t ducks = r.U32(56);
			c.Array(r, 64, ducks, 8, 8, "sound ducks", [&](const Chunk& duckArray) {
				for (std::size_t i = 0; i < ducks && !c.s.Failed(); ++i) {
					c.Nested(Entry(duckArray, i, 8), 0, 0x14, 1632, 32, "sound duck", [&](const Chunk& duck) { SoundDuckBody(c, duck); });
				}
			});
			c.Array(r, 80, r.U32(72), 136, 8, "sound bank +80");
			const std::size_t records = r.U32(88);
			c.Array(r, 96, records, 88, 8, "sound bank +96", [&](const Chunk& recordArray) {
				for (std::size_t i = 0; i < records && !c.s.Failed(); ++i) {
					const View record = Entry(recordArray, i, 88);
					const std::size_t entries = record.U32(72);
					c.Array(record, 80, entries, 288, 8, "sound bank +96 entries", [&](const Chunk& entryArray) {
						for (std::size_t j = 0; j < entries && !c.s.Failed(); ++j) {
							const View entry = Entry(entryArray, j, 288);
							c.Array(entry, 272, entry.U32(264), 128, 8, "sound bank +96 entry +272");
						}
					});
				}
			});
			// Stored inline, the acoustics are a sub-array a copy can leave out (cwlink --no-acoustics).
			auto acoustics = [&] {
				c.Nested(r, 104, 0x16, 352, 8, "sound acoustics", [&](const Chunk& root) { SoundAcousticsBody(c, root); });
			};
			if (r.P(104) == kPtrInline || r.P(104) == kPtrInsert) {
				c.Subtree(r, 104, "sound acoustics", acoustics);
			}
			else {
				acoustics();
			}
			c.s.Pop();
		}

		// --- zbarrier (0x53): Load_ZbarrierAsset 0x7FF71E7F99E0 -> Load_ZBarrierDef 0x7FF71E7F9910, a 944-B root ------------
		// A window barrier's boards: what a zbarrier_<name> entity draws and animates (G_RegisterZBarriers_cand 0x7FF720912790
		// looks it up by that name when the level starts). Under a push of block 4: +120 an xmodel; six 136-B boards embedded
		// at +128 (Load_ZBarrierBoardArray 0x7FF71E7F95C0), each with three xmodels at +0, +8 and +16 and two fx at +56 and
		// +64, every one a nested asset. A board's four animations are xanim names at +24 .. +48, no pointers:
		// ZBarrier_InitAnimTree_cand 0x7FF72090D3A0 looks them up by name. No script strings. The models are read as nested
		// assets recorded with the barrier, so one stored inline travels with the copy too.
		void ZBarrierBody(Ctx& c, const Chunk& rootChunk) {
			const View r{ &rootChunk, 0 };
			auto model = [&](const View& owner, std::size_t offset, const char* what) {
				c.Nested(owner, offset, 0x06, 232, 8, what, [&](const Chunk& root) { XModelBody(c, root); });
			};
			c.s.Push(XBlockVirtual);
			model(r, 120, "zbarrier model");
			for (std::size_t b = 0; b < 6 && !c.s.Failed(); ++b) {
				const View board = r.Sub(128 + 136 * b);
				for (const std::size_t field : { 0, 8, 16 }) {
					model(board, field, "zbarrier board model");
				}
				for (const std::size_t field : { 56, 64 }) {
					c.Asset(board, field, 0x33, "zbarrier board fx");
				}
			}
			c.s.Pop();
		}

		struct LibraryType {
			std::uint64_t type = 0;
			std::size_t rootSize = 0;
			std::uint64_t alignment = 8; // DB_AllocStreamPos in the generated Load_<Type>Asset wrapper
			const char* what = nullptr;
			void (*body)(Ctx&, const Chunk&) = nullptr;
		};
		constexpr LibraryType kLibraryTypes[] = {
			{ 0x10, 208, 8, "image header", ImageBody },
			{ 0xB8, 56, 8, "streamkey header", StreamKeyBody },
			{ 0x08, 88, 8, "xskeleton header", XSkeletonBody },
			{ 0x09, 64, 8, "xmodelmesh header", XModelMeshBody },
			{ 0x06, 232, 8, "xmodel header", XModelBody },
			{ 0x07, 96, 8, "xcollision header", XCollisionBody },
			{ 0x02, 112, 8, "physpreset header", PhysPresetBody },
			{ 0x0A, 344, 16, "material header", MaterialBody },
			{ 0x35, 88, 8, "klf header", KlfBody },
			{ 0xD3, 128, 16, "winddef header", WindDefBody },
			{ 0x12, 112, 8, "sound_bank header", SoundBankBody },
			{ 0x13, 88, 8, "sound_asset header", SoundAssetBody },
			{ 0x14, 1632, 32, "sound_duck header", SoundDuckBody },
			{ 0x15, 152, 8, "sound_alias_modifier header", SoundAliasModifierBody },
			{ 0x16, 352, 8, "sound_acoustics header", SoundAcousticsBody },
			{ 0x53, 944, 8, "zbarrier header", ZBarrierBody },
		};

		const LibraryType* FindLibraryType(std::uint64_t type) {
			for (const LibraryType& library : kLibraryTypes) {
				if (library.type == type) {
					return &library;
				}
			}
			return nullptr;
		}

		bool ReadLibraryAsset(std::uint64_t type, XStream& s, std::uint64_t header, AssetRecord* out) {
			const LibraryType* library = FindLibraryType(type);
			s.SetRootType(type);
			return Record::ReadRoot(s, header, out, library->rootSize, library->alignment, library->what,
				[&](Ctx& c, const Chunk& root) { library->body(c, root); });
		}
	}

	bool LoadKlfAsset(XStream& s, std::uint64_t header) { return ReadLibraryAsset(0x35, s, header, nullptr); }
	bool LoadWindDefAsset(XStream& s, std::uint64_t header) { return ReadLibraryAsset(0xD3, s, header, nullptr); }
	bool LoadZBarrierAsset(XStream& s, std::uint64_t header) { return ReadLibraryAsset(0x53, s, header, nullptr); }

	RecordReader LibraryAssetReader(std::uint64_t type) {
		if (!FindLibraryType(type)) {
			return {};
		}
		return [type](XStream& s, std::uint64_t header, AssetRecord& out) { return ReadLibraryAsset(type, s, header, &out); };
	}

	bool LoadWritesInto(std::uint64_t type, std::span<const std::uint8_t> root) {
		// The install callbacks per key type (g_streamKeyTypeCallbacks 0x7FF72AF35800, 40 B each): 1 to 4 read the owner
		// at +40; 0 and 5 (navmesh and navvolume keys) have none.
		return type == 0xB8 && root.size() >= 56 && root[54] >= 1 && root[54] <= 4;
	}
}
