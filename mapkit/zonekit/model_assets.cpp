// The model family, each loader transcribed from the game's (addresses in the comments; IDB
// bocw_fixed_renamed.i64, build 1.34.0.15931218). Where the loader alone does not say what a field is, the
// names follow the in-memory layouts in Greyhound's Cold War reader (github.com/Scobalula/Greyhound,
// GameBlackOpsCW.cpp / DBGameAssets.h: BOCWXModel, BOCWXModelLod, BOCWXModelSurface, BOCWXModelMeshInfo,
// BOCWXSkeleton), which match these loaders' struct sizes and counts field for field.
#include "model_assets.hpp"
#include "asset_loaders.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace MapKit::Zone {
	namespace {
		constexpr int XBlockResident = 12; // mesh data kept in the .ff (Load_XModelMeshSurfaceData)
		// The collision loaders push block 9, which the engine maps to the current block (XStream::Push): their
		// data is stored inline. Retail zones leave these fields null and stream the data through a streamkey.
		constexpr int XBlockCollision = XBlockCollisionAlias;

		// Push(block) around one inline-or-reference field, as the collision loaders do for block 9.
		void LoadInlineIn(XStream& s, int block, std::uint64_t pointer, std::size_t size, std::uint64_t alignment,
			const char* what) {
			s.Push(block);
			LoadInline(s, pointer, size, alignment, what);
			s.Pop();
		}

		// A field the generated code only tests for non-null: Alloc + Load, no reference case.
		void LoadIfSet(XStream& s, std::uint64_t pointer, std::size_t size, std::uint64_t alignment) {
			if (pointer) {
				s.Alloc(alignment);
				s.Load(nullptr, size);
			}
		}

		void LoadIfSetIn(XStream& s, int block, std::uint64_t pointer, std::size_t size, std::uint64_t alignment) {
			s.Push(block);
			LoadIfSet(s, pointer, size, alignment);
			s.Pop();
		}

		// Where data of this alignment loaded inline next lands in the current block: (block << 60) | position,
		// the value a later reference to it stores, minus one.
		std::uint64_t NextInlineAt(const XStream& s, std::uint64_t alignment) {
			const std::uint64_t pos = s.Positions()[s.Block()];
			return (static_cast<std::uint64_t>(s.Block()) << 60) | ((pos + alignment - 1) & ~(alignment - 1));
		}

		// --- xskeleton (0x08): Load_XskeletonAsset 0x7FF71E7F9180 -> Load_XSkeleton 0x7FF71E7F8EB0 --------
		// struct XSkeleton (88 B): +72 u16 cosmetic bones, +74 u16 bones, +76 u16 root bones (Greyhound's
		// BoneCounts[0..2]). Per bone: +8 names (script strings), +16 8 B, +48 part classification, +56
		// base matrices (32 B); per non-root bone: +24 parents, +32 rotations, +40 translations; +64 40 B
		// per non-cosmetic bone.
		void LoadXSkeletonBody(XStream& s, std::span<const std::uint8_t> skel) {
			s.Push(XBlockVirtual);
			const std::size_t cosmetic = Get<std::uint16_t>(skel, 72);
			const std::size_t bones = Get<std::uint16_t>(skel, 74);
			const std::size_t roots = Get<std::uint16_t>(skel, 76);
			const std::size_t all = cosmetic + bones;
			const std::size_t parented = all - roots;
			LoadInline(s, Get<std::uint64_t>(skel, 8), 4 * all, 4, "skeleton bone names");
			LoadInline(s, Get<std::uint64_t>(skel, 16), 8 * bones, 4, "skeleton +16");
			LoadInline(s, Get<std::uint64_t>(skel, 24), 2 * parented, 2, "skeleton parents");
			LoadInline(s, Get<std::uint64_t>(skel, 32), 8 * parented, 8, "skeleton rotations");
			LoadInline(s, Get<std::uint64_t>(skel, 40), 16 * parented, 4, "skeleton translations");
			LoadInline(s, Get<std::uint64_t>(skel, 48), all, 1, "skeleton part classification");
			LoadInline(s, Get<std::uint64_t>(skel, 56), 32 * all, 16, "skeleton base matrices");
			LoadInline(s, Get<std::uint64_t>(skel, 64), 40 * bones, 4, "skeleton +64");
			s.Pop();
		}

		// --- material (0x0A): Load_MaterialAsset 0x7FF71E7E0960 -> Load_Material 0x7FF71E7DFFB0 -----------
		// struct Material (344 B, 16-aligned): +40 techset, +48 image table (24 B x u8 @328, image @+0),
		// +56 8 B x u8 @329, +64 eight texture-set pointers, +128 an embedded 160-B block, +288 an embedded
		// 40-B block, +336 a material-local techset.

		// 0x7FF71E7E0820: 24 B {+0 u32 x @16 (64-aligned), +8 16-B entries x @20, image @+0}.
		void LoadMaterialTextureSet(XStream& s, std::span<const std::uint8_t> set, MaterialData* out) {
			std::vector<std::uint8_t> data;
			LoadInline(s, Get<std::uint64_t>(set, 0), 4ull * Get<std::uint32_t>(set, 16), 64, "material set data", &data);
			std::vector<std::uint8_t> entries;
			if (LoadInline(s, Get<std::uint64_t>(set, 8), 16ull * Get<std::uint32_t>(set, 20), 8, "material set images",
				&entries)) {
				for (std::size_t i = 0; i + 16 <= entries.size() && !s.Failed(); i += 16) {
					LoadAsset(s, 0x10, Get<std::uint64_t>(entries, i), "material set image");
				}
			}
			if (out) {
				out->sets.push_back({ std::vector<std::uint8_t>(set.begin(), set.end()), std::move(data), std::move(entries) });
			}
		}

		// Inline data of a material, kept only when `out` is set.
		std::vector<std::uint8_t>* Keep(MaterialData* out, std::vector<std::uint8_t> MaterialData::* field) {
			return out ? &(out->*field) : nullptr;
		}

		void LoadMaterialBody(XStream& s, std::span<const std::uint8_t> material, MaterialData* out) {
			s.Push(XBlockVirtual);
			if (out) {
				std::memcpy(out->root.data(), material.data(), std::min(material.size(), out->root.size()));
			}

			// 0x7FF71E7E0590 on the embedded block at +128: thirteen 64-B records (0x7FF71E7E14F0, eight
			// 12-B vectors each) at +40, then +144 u64 size / +152 data in block 6.
			const auto block = material.subspan(128, 160);
			for (std::size_t i = 0; i < 13; ++i) {
				std::vector<std::uint8_t> record;
				if (LoadInline(s, Get<std::uint64_t>(block, 40 + i * 8), 64, 8, "material record", &record)) {
					if (out) {
						out->records.push_back(record);
					}
					for (std::size_t v = 0; v < 8; ++v) {
						LoadInline(s, Get<std::uint64_t>(record, v * 8), 12, 4, "material record vector");
					}
				}
			}
			s.Push(XBlockPhysical);
			LoadInline(s, Get<std::uint64_t>(block, 152), static_cast<std::size_t>(Get<std::uint64_t>(block, 144)), 256,
				"material constant buffer", Keep(out, &MaterialData::constants));
			s.Pop();

			LoadAsset(s, 0x0F, Get<std::uint64_t>(material, 40), "material techset");

			// 0x7FF71E7E1A40
			std::vector<std::uint8_t> images;
			if (LoadInline(s, Get<std::uint64_t>(material, 48), 24ull * material[328], 8, "material image table", &images)) {
				for (std::size_t i = 0; i + 24 <= images.size() && !s.Failed(); i += 24) {
					const std::uint64_t image = Get<std::uint64_t>(images, i);
					if (out && (image == kPtrInline || image == kPtrInsert)) {
						ReadImage(s, image, out->inlineImages.emplace_back());
					}
					else {
						LoadAsset(s, 0x10, image, "material image");
					}
				}
				if (out) {
					out->imageTable = images;
				}
			}
			LoadInline(s, Get<std::uint64_t>(material, 56), 8ull * material[329], 16, "material +56", Keep(out, &MaterialData::list56));

			for (std::size_t i = 0; i < 8; ++i) {
				std::vector<std::uint8_t> set;
				if (LoadInline(s, Get<std::uint64_t>(material, 64 + i * 8), 24, 8, "material texture set", &set)) {
					LoadMaterialTextureSet(s, set, out);
				}
			}

			// 0x7FF71E7E7C50 on the embedded block at +288.
			const auto tail = material.subspan(288, 40);
			LoadInline(s, Get<std::uint64_t>(tail, 8), 24ull * Get<std::uint32_t>(tail, 16), 4, "material +288 array",
				Keep(out, &MaterialData::tail296));
			LoadInline(s, Get<std::uint64_t>(tail, 24), 8ull * Get<std::uint32_t>(tail, 32), 4, "material +288 array",
				Keep(out, &MaterialData::tail312));

			// 0x7FF71E7E0AC0: 24 B {+0 32 B x u8 @8 (16-aligned), +16 a techset body (0x7FF71E7E1750)}.
			std::vector<std::uint8_t> local;
			if (LoadInline(s, Get<std::uint64_t>(material, 336), 24, 8, "material local techset", &local)) {
				if (out) {
					out->localTechset = true;
				}
				LoadInline(s, Get<std::uint64_t>(local, 0), 32ull * local[8], 16, "material local techset data");
				std::vector<std::uint8_t> techset;
				if (LoadInline(s, Get<std::uint64_t>(local, 16), 168, 8, "material local techset body", &techset)) {
					LoadTechsetBody(s, techset);
					s.PreloadSave(); // sub_7FF71E852E10, the preload's twin of the body loader
				}
			}
			s.Pop();
		}

		// --- xmodelmesh (0x09): Load_XmodelmeshAsset 0x7FF71E7F8BF0 -> Load_XModelMesh 0x7FF71E7F8A50 ------
		// One LOD (Greyhound's BOCWXModelLod, 64 B): +8 surfaces (48 B x u16 @60), +16 mesh info, +24 96 B,
		// +32 u64 stream key: the .xsub hash key of the LOD's mesh data when the mesh info says streamed.

		// Load_XModelMeshSurfaceData 0x7FF71E7F93B0 (Greyhound's BOCWXModelMeshInfo, 464 B): +0 flags,
		// +32 data / +40 u32 size. flags & 1 = streamed: block 7 (never stored; pushed only if
		// sub_7FF72686C7C0, which held in the traced load, as for images); else block 12, resident in the .ff.
		// +428 / +432 u32 counts sized the three arrays at +440, +448, +456.
		void LoadMeshInfoBody(XStream& s, std::span<const std::uint8_t> info, MeshInfo* out) {
			const auto data = Get<std::uint64_t>(info, 32);
			const auto size = Get<std::uint32_t>(info, 40);
			if (Get<std::uint32_t>(info, 0) & 1) {
				LoadIfSetIn(s, XBlockStreamed, data, size, 256);
			}
			else {
				s.Push(XBlockResident);
				if (out && data == kPtrInline) {
					out->residentAt = NextInlineAt(s, 256);
				}
				LoadInline(s, data, size, 256, "mesh data", out ? &out->resident : nullptr);
				s.Pop();
			}
			if (out) {
				std::memcpy(out->raw.data(), info.data(), std::min(info.size(), out->raw.size()));
			}
			const std::size_t count = static_cast<std::uint32_t>(Get<std::uint32_t>(info, 428) + Get<std::uint32_t>(info, 432));
			LoadInline(s, Get<std::uint64_t>(info, 440), 4 * count, 4, "mesh info +440");
			LoadInline(s, Get<std::uint64_t>(info, 448), 8 * count, 4, "mesh info +448");
			LoadInline(s, Get<std::uint64_t>(info, 456), 8 * count, 4, "mesh info +456");
		}

		bool LoadMeshInfo(XStream& s, std::uint64_t pointer, MeshInfo* out = nullptr) {
			std::vector<std::uint8_t> info;
			const std::uint64_t at = pointer == kPtrInline ? NextInlineAt(s, 16) : 0;
			if (LoadInline(s, pointer, 464, 16, "mesh info", &info)) {
				LoadMeshInfoBody(s, info, out);
				if (out) {
					out->at = at;
				}
				return true;
			}
			return false;
		}

		// Load_XModelMeshPart 0x7FF71E7F9290 (40 B): +0 name, +16 32 B x u16 @10, +24 3 B x u16 @8,
		// +32 24 B x u16 @12.
		void LoadMeshPartBody(XStream& s, std::span<const std::uint8_t> part) {
			LoadXString(s, Get<std::uint64_t>(part, 0), "mesh part name");
			LoadInline(s, Get<std::uint64_t>(part, 16), 32ull * Get<std::uint16_t>(part, 10), 4, "mesh part +16");
			LoadInline(s, Get<std::uint64_t>(part, 24), 3ull * Get<std::uint16_t>(part, 8), 1, "mesh part +24");
			LoadInline(s, Get<std::uint64_t>(part, 32), 24ull * Get<std::uint16_t>(part, 12), 4, "mesh part +32");
		}

		void LoadXModelMeshBody(XStream& s, std::span<const std::uint8_t> lod, XModelMeshData* out) {
			s.Push(XBlockVirtual);
			LoadInline(s, Get<std::uint64_t>(lod, 24), 96, 4, "lod +24");
			const bool hasInfo = LoadMeshInfo(s, Get<std::uint64_t>(lod, 16), out ? &out->info : nullptr);
			if (out) {
				std::memcpy(out->root.data(), lod.data(), std::min(lod.size(), out->root.size()));
				out->hasInfo = hasInfo;
			}
			if (Get<std::uint64_t>(lod, 8)) {
				s.Alloc(8);
				std::vector<std::uint8_t> surfaces(48ull * Get<std::uint16_t>(lod, 60));
				if (s.Load(surfaces.data(), surfaces.size())) {
					if (out) {
						out->surfaces.resize(surfaces.size() / 48);
						for (std::size_t i = 0; i < out->surfaces.size(); ++i) {
							std::memcpy(out->surfaces[i].raw.data(), surfaces.data() + i * 48, 48);
						}
					}
					// Greyhound's BOCWXModelSurface: +2 u16 vertices, +4 u16 faces, +8 / +12 first vertex / face.
					for (std::size_t i = 0; i + 48 <= surfaces.size() && !s.Failed(); i += 48) {
						const auto surface = std::span<const std::uint8_t>(surfaces).subspan(i, 48);
						if (Get<std::uint64_t>(surface, 24)) {
							s.Alloc(8);
							std::vector<std::uint8_t> part(40);
							if (s.Load(part.data(), part.size())) {
								LoadMeshPartBody(s, part);
							}
						}
						LoadMeshInfo(s, Get<std::uint64_t>(surface, 16));
						LoadInline(s, Get<std::uint64_t>(surface, 32), 96, 4, "surface +32");
					}
				}
			}
			s.Pop();
		}

		// --- physics under xcollision -----------------------------------------------------------------
		// ragdoll (0x97), 0x7FF71E7E75E0 (2992 B): only an impactsoundstable at +2968 below it.
		bool LoadRagdoll(XStream& s, std::uint64_t header) {
			return LoadAssetHeader(s, header, 2992, 8, [&](std::span<std::uint8_t> ragdoll) {
				s.Push(XBlockVirtual);
				LoadAsset(s, 0x37, Get<std::uint64_t>(ragdoll, 2968), "ragdoll impact sounds");
				s.Pop();
			});
		}

		// triggeractions (0xD8), inline in sub_7FF71E7F7FE0: 88 B, nothing below it.
		bool LoadTriggerActions(XStream& s, std::uint64_t header) {
			return LoadAssetHeader(s, header, 88, 8, [](std::span<std::uint8_t>) {});
		}

		// A field the generated code only tests for non-null, pushed to `block` (9 = the current block),
		// kept in *out when asked.
		void LoadTreeArray(XStream& s, int block, std::uint64_t pointer, std::size_t size, std::uint64_t alignment,
			std::vector<std::uint8_t>* out) {
			s.Push(block);
			if (pointer) {
				s.Alloc(alignment);
				if (out) {
					out->assign(size, 0);
					s.Load(out->data(), size);
				}
				else {
					s.Load(nullptr, size);
				}
			}
			s.Pop();
		}

		// 0x7FF71E7F8260 (40 B): +0 bytes x u16 @32, +8 80-B shapes x u16 @34 (0x7FF71E7F88D0, their data
		// in block 9), +16 24-B {tree, 32-B record} x u8 @36.
		void LoadCollisionData(XStream& s, std::span<const std::uint8_t> data) {
			LoadIfSet(s, Get<std::uint64_t>(data, 0), Get<std::uint16_t>(data, 32), 1);

			if (Get<std::uint64_t>(data, 8)) {
				s.Alloc(8);
				std::vector<std::uint8_t> shapes(80ull * Get<std::uint16_t>(data, 34));
				if (s.Load(shapes.data(), shapes.size())) {
					for (std::size_t i = 0; i + 80 <= shapes.size(); i += 80) {
						const auto shape = std::span<const std::uint8_t>(shapes).subspan(i, 80);
						LoadInlineIn(s, XBlockCollision, Get<std::uint64_t>(shape, 8), 3ull * Get<std::uint16_t>(shape, 40), 1,
							"collision shape +8");
						LoadInlineIn(s, XBlockCollision, Get<std::uint64_t>(shape, 0), 6ull * Get<std::uint16_t>(shape, 42), 2,
							"collision shape +0");
						LoadInlineIn(s, XBlockCollision, Get<std::uint64_t>(shape, 16), 4ull * Get<std::uint16_t>(shape, 38), 4,
							"collision shape +16");
						LoadInlineIn(s, XBlockCollision, Get<std::uint64_t>(shape, 24), Get<std::uint32_t>(shape, 32), 1,
							"collision shape +24");
					}
				}
			}

			if (Get<std::uint64_t>(data, 16)) {
				s.Alloc(8);
				std::vector<std::uint8_t> entries(24ull * data[36]);
				if (s.Load(entries.data(), entries.size())) {
					for (std::size_t i = 0; i + 24 <= entries.size(); i += 24) {
						if (Get<std::uint64_t>(entries, i)) {
							s.Alloc(8);
							std::vector<std::uint8_t> tree(184);
							if (s.Load(tree.data(), tree.size())) {
								ReadCollisionTree(s, tree, nullptr);
							}
						}
						if (Get<std::uint64_t>(entries, i + 8)) {
							s.Alloc(8);
							std::vector<std::uint8_t> record(32);
							if (s.Load(record.data(), record.size())) {
								const std::size_t n = Get<std::uint32_t>(record, 24);
								LoadIfSet(s, Get<std::uint64_t>(record, 0), 8 * n, 4);
								LoadIfSet(s, Get<std::uint64_t>(record, 8), 4 * n, 4);
								LoadIfSet(s, Get<std::uint64_t>(record, 16), Get<std::uint32_t>(record, 28), 1);
							}
						}
					}
				}
			}
		}

		// Physconstraints entries (0x7FF71E7E3D50, 488 B): i32 kind @0, the rest from +8.
		void LoadConstraint(XStream& s, std::span<const std::uint8_t> entry) {
			const auto kind = Get<std::int32_t>(entry, 0);
			const auto body = entry.subspan(8);
			if (kind == 16) {
				// 0x7FF71E7E3FC0: +128 36 B x i32 @120, xanimcurves at +152/+176/+200 and +232/+256/+280.
				LoadIfSet(s, Get<std::uint64_t>(body, 128), 36ull * Get<std::int32_t>(body, 120), 4);
				for (const std::size_t field : { 152, 176, 200, 232, 256, 280 }) {
					LoadXAnimCurveAsset(s, Get<std::uint64_t>(body, field));
				}
			}
			else if (kind == 18) {
				LoadIfSet(s, Get<std::uint64_t>(body, 16), 36ull * Get<std::int32_t>(body, 8), 4);
			}
			else if (kind != 17) {
				// 0x7FF71E7E3E50: a material at +160, then six {u64 count, 8-B array} at +256 + 40k.
				LoadMaterialAsset(s, Get<std::uint64_t>(body, 160));
				for (std::size_t k = 0; k < 6; ++k) {
					const std::size_t at = 256 + 40 * k;
					LoadIfSet(s, Get<std::uint64_t>(body, at + 8), static_cast<std::size_t>(8 * Get<std::uint64_t>(body, at)), 4);
				}
			}
		}

		void LoadXCollisionBody(XStream& s, std::span<const std::uint8_t> collision) {
			s.Push(XBlockVirtual);
			LoadXSkeletonAsset(s, Get<std::uint64_t>(collision, 8));
			LoadAsset(s, 0xB8, Get<std::uint64_t>(collision, 16), "collision streamkey");
			if (Get<std::uint64_t>(collision, 24)) {
				s.Alloc(8);
				std::vector<std::uint8_t> data(40);
				if (s.Load(data.data(), data.size())) {
					LoadCollisionData(s, data);
				}
			}
			LoadPhysPresetAsset(s, Get<std::uint64_t>(collision, 40));
			LoadTriggerActions(s, Get<std::uint64_t>(collision, 48));
			LoadPhysConstraintsAsset(s, Get<std::uint64_t>(collision, 56));
			s.Pop();
		}

		// --- xmodel (0x06): Load_XmodelAsset 0x7FF71E7F8D50 -> Load_XModel 0x7FF71E7F8420 ----------------
		// struct XModel (232 B, Greyhound's BOCWXModel): +8 skeleton, +16 collision, +24 attached models
		// (40 B x i8 @222, xmodel @+0), +32 eight LODs (xmodelmesh), +96 per-LOD material tables (32 B x
		// u16 @112), +104 96 B.
		void LoadXModelBody(XStream& s, std::span<const std::uint8_t> model, XModelData* out) {
			s.Push(XBlockVirtual);
			LoadXSkeletonAsset(s, Get<std::uint64_t>(model, 8));
			LoadXCollisionAsset(s, Get<std::uint64_t>(model, 16));
			for (std::size_t i = 0; i < 8 && !s.Failed(); ++i) {
				const std::uint64_t lod = Get<std::uint64_t>(model, 32 + i * 8);
				if (out && (lod == kPtrInline || lod == kPtrInsert)) {
					out->hasInline[i] = ReadXModelMesh(s, lod, out->lods[i]);
				}
				else {
					LoadXModelMeshAsset(s, lod);
				}
			}

			// One 32-B table per LOD: +0 u16 surface count, +8 / +16 material handles (0x7FF71E7E0A10), +24
			// u32 per surface.
			if (Get<std::uint64_t>(model, 96)) {
				s.Alloc(8);
				std::vector<std::uint8_t> tables(32ull * Get<std::uint16_t>(model, 112));
				if (s.Load(tables.data(), tables.size())) {
					for (std::size_t i = 0; i + 32 <= tables.size() && !s.Failed(); i += 32) {
						const std::size_t count = Get<std::uint16_t>(tables, i);
						for (const std::size_t field : { 8, 16 }) {
							std::vector<XModelData::Material>* materials = nullptr;
							if (out) {
								materials = &(field == 8 ? out->materials : out->materials16).emplace_back();
							}
							std::vector<std::uint8_t> handles;
							if (LoadInline(s, Get<std::uint64_t>(tables, i + field), 8 * count, 8, "material handles", &handles)) {
								for (std::size_t h = 0; h + 8 <= handles.size() && !s.Failed(); h += 8) {
									const std::uint64_t handle = Get<std::uint64_t>(handles, h);
									if (materials) {
										XModelData::Material& material = materials->emplace_back();
										material.stored = handle;
										LoadAssetHeader(s, handle, 344, 16, [&](std::span<std::uint8_t> m) {
											material.name = Get<std::uint64_t>(m, 0) & ~(1ull << 63);
											LoadMaterialBody(s, m, nullptr);
										});
									}
									else {
										LoadMaterialAsset(s, handle);
									}
								}
							}
						}
						LoadInline(s, Get<std::uint64_t>(tables, i + 24), 4 * count, 4, "lod material +24");
					}
				}
			}
			LoadInline(s, Get<std::uint64_t>(model, 104), 96, 4, "xmodel +104");

			if (Get<std::uint64_t>(model, 24)) {
				const auto count = static_cast<std::int8_t>(model[222]);
				if (count < 0) {
					s.Fail("xmodel attachment count is negative");
				}
				else {
					s.Alloc(8);
					std::vector<std::uint8_t> attached(40ull * count);
					if (s.Load(attached.data(), attached.size())) {
						for (std::size_t i = 0; i + 40 <= attached.size() && !s.Failed(); i += 40) {
							LoadXModelAsset(s, Get<std::uint64_t>(attached, i));
						}
					}
				}
			}
			s.Pop();
		}
	}

	// 0x7FF71E7F9A90 (184 B): a collision tree. The arrays go in the order of CollisionTreeArray; the ones
	// pushed to block 9 end up inline like the rest (XStream::Push). Retail xcollisions leave them null and
	// stream the tree through their streamkey; the clip map's world trees carry them inline.
	void ReadCollisionTree(XStream& s, std::span<const std::uint8_t> tree, CollisionTree* out) {
		const std::uint32_t a = Get<std::uint32_t>(tree, 24);
		const std::uint32_t b = Get<std::uint32_t>(tree, 28);
		const std::uint32_t c = Get<std::uint32_t>(tree, 32);
		const std::uint32_t packed = a + 6 * c; // 32-bit, as the engine computes it
		const std::uint64_t sizes[kCollisionTreeArrayCount] = {
			16ull * a, 12ull * b, 4ull * c, 4ull * c, 4ull * c, 8ull * ((static_cast<std::uint64_t>(packed) + 5) / 6),
			28ull * c, Get<std::uint32_t>(tree, 96), 12ull * Get<std::uint32_t>(tree, 40), Get<std::uint32_t>(tree, 44),
			32ull * Get<std::uint32_t>(tree, 48), Get<std::uint32_t>(tree, 160), 44ull * Get<std::uint32_t>(tree, 56),
		};
		if (out) {
			std::memcpy(out->raw.data(), tree.data(), std::min(tree.size(), out->raw.size()));
		}
		for (std::size_t i = 0; i < kCollisionTreeArrayCount; ++i) {
			const CollisionTreeArray& array = kCollisionTreeArrays[i];
			const int block = array.pushed9 ? XBlockCollisionAlias : s.Block();
			LoadTreeArray(s, block, Get<std::uint64_t>(tree, array.field), static_cast<std::size_t>(sizes[i]),
				array.alignment, out ? &out->arrays[i] : nullptr);
		}
	}

	bool LoadXModelAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x06);
		return LoadAssetHeader(s, header, 232, 8, [&](std::span<std::uint8_t> model) { LoadXModelBody(s, model, nullptr); });
	}

	bool ReadXModel(XStream& s, std::uint64_t header, XModelData& out) {
		out = {};
		return LoadAssetHeader(s, header, 232, 8, [&](std::span<std::uint8_t> model) {
			std::memcpy(out.root.data(), model.data(), std::min(model.size(), out.root.size()));
			LoadXModelBody(s, model, &out);
		});
	}

	std::uint64_t XModelData::Name() const { return Get<std::uint64_t>(root, 0) & ~(1ull << 63); }
	std::uint16_t XModelData::LodCount() const { return Get<std::uint16_t>(root, 112); }
	std::uint64_t XModelData::Lod(std::size_t i) const { return Get<std::uint64_t>(root, 32 + 8 * i); }
	std::array<float, 3> XModelData::Mins() const {
		return { Get<float>(root, 184), Get<float>(root, 188), Get<float>(root, 192) };
	}
	std::array<float, 3> XModelData::Maxs() const {
		return { Get<float>(root, 196), Get<float>(root, 200), Get<float>(root, 204) };
	}
	float XModelData::Radius() const { return Get<float>(root, 216); }

	// --- xcollision (0x07): Load_XcollisionAsset 0x7FF71E7F81B0 -> 0x7FF71E7F7FE0 --------------------------
	// struct (96 B): +8 skeleton, +16 streamkey, +24 collision data, +40 physpreset, +48 triggeractions,
	// +56 physconstraints.
	bool LoadXCollisionAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x07);
		return LoadAssetHeader(s, header, 96, 8, [&](std::span<std::uint8_t> c) { LoadXCollisionBody(s, c); });
	}

	bool LoadXSkeletonAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x08);
		return LoadAssetHeader(s, header, 88, 8, [&](std::span<std::uint8_t> skel) { LoadXSkeletonBody(s, skel); });
	}

	bool LoadXModelMeshAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x09);
		return LoadAssetHeader(s, header, 64, 8, [&](std::span<std::uint8_t> lod) { LoadXModelMeshBody(s, lod, nullptr); });
	}

	bool ReadXModelMesh(XStream& s, std::uint64_t header, XModelMeshData& out) {
		out = {};
		out.cursor = s.Cursor();
		return LoadAssetHeader(s, header, 64, 8, [&](std::span<std::uint8_t> lod) { LoadXModelMeshBody(s, lod, &out); });
	}

	std::uint32_t MeshInfo::VertexCount() const { return Get<std::uint32_t>(raw, 4); }
	std::uint32_t MeshInfo::FaceCount() const { return Get<std::uint32_t>(raw, 12); }
	std::uint32_t MeshInfo::BufferSize() const { return Get<std::uint32_t>(raw, 40); }
	std::uint32_t MeshInfo::PositionOffset() const { return Get<std::uint32_t>(raw, 44); }
	std::uint32_t MeshInfo::VertexOffset() const { return Get<std::uint32_t>(raw, 48); }
	std::uint32_t MeshInfo::FaceOffset() const { return Get<std::uint32_t>(raw, 52); }
	std::uint32_t MeshInfo::WeightOffset() const { return Get<std::uint32_t>(raw, 56); }
	std::uint16_t MeshSurface::VertexCount() const { return Get<std::uint16_t>(raw, 2); }
	std::uint16_t MeshSurface::FaceCount() const { return Get<std::uint16_t>(raw, 4); }
	std::uint32_t MeshSurface::FirstVertex() const { return Get<std::uint32_t>(raw, 8); }
	std::uint32_t MeshSurface::FirstIndex() const { return Get<std::uint32_t>(raw, 12); }
	std::uint64_t XModelMeshData::Name() const { return Get<std::uint64_t>(root, 0); }
	std::uint64_t XModelMeshData::StreamKey() const { return Get<std::uint64_t>(root, 32); }

	bool LoadMaterialAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x0A);
		return LoadAssetHeader(s, header, 344, 16, [&](std::span<std::uint8_t> m) { LoadMaterialBody(s, m, nullptr); });
	}

	bool ReadMaterial(XStream& s, std::uint64_t header, MaterialData& out) {
		out = {};
		return LoadAssetHeader(s, header, 344, 16, [&](std::span<std::uint8_t> m) { LoadMaterialBody(s, m, &out); });
	}

	std::uint64_t MaterialData::Name() const { return Get<std::uint64_t>(root, 0) & ~(1ull << 63); }

	// --- physpreset (0x02): Load_PhyspresetAsset 0x7FF71E7E4340 (112 B): +88 fx, +96 impactsfxtable,
	// +104 impactsoundstable.
	bool LoadPhysPresetAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x02);
		return LoadAssetHeader(s, header, 112, 8, [&](std::span<std::uint8_t> preset) {
			s.Push(XBlockVirtual);
			LoadAsset(s, 0x33, Get<std::uint64_t>(preset, 88), "physpreset fx");
			LoadAsset(s, 0x36, Get<std::uint64_t>(preset, 96), "physpreset impact fx");
			LoadAsset(s, 0x37, Get<std::uint64_t>(preset, 104), "physpreset impact sounds");
			s.Pop();
		});
	}

	// --- physconstraints (0x03): Load_PhysconstraintsAsset 0x7FF71E7E4290 -> 0x7FF71E7E40A0 (48 B):
	// +16 ragdoll, +24 320 B, +32 constraints (488 B x u32 @8), +40 8 B x u32 @12.
	bool LoadPhysConstraintsAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x03);
		return LoadAssetHeader(s, header, 48, 8, [&](std::span<std::uint8_t> pc) {
			s.Push(XBlockVirtual);
			LoadRagdoll(s, Get<std::uint64_t>(pc, 16));
			LoadInline(s, Get<std::uint64_t>(pc, 24), 320, 16, "physconstraints +24");
			if (Get<std::uint64_t>(pc, 32)) {
				s.Alloc(8);
				std::vector<std::uint8_t> entries(488ull * Get<std::uint32_t>(pc, 8));
				if (s.Load(entries.data(), entries.size())) {
					for (std::size_t i = 0; i + 488 <= entries.size() && !s.Failed(); i += 488) {
						LoadConstraint(s, std::span<const std::uint8_t>(entries).subspan(i, 488));
					}
				}
			}
			LoadIfSet(s, Get<std::uint64_t>(pc, 40), 8ull * Get<std::uint32_t>(pc, 12), 4);
			s.Pop();
		});
	}

	// --- impactsfxtable (0x36): Load_ImpactsfxtableAsset 0x7FF71E7D7250 (24 B): +8 surfacefxtable,
	// +16 entityfximpacts.
	bool LoadImpactFxTableAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x36);
		return LoadAssetHeader(s, header, 24, 8, [&](std::span<std::uint8_t> table) {
			s.Push(XBlockVirtual);
			LoadAsset(s, 0x4E, Get<std::uint64_t>(table, 8), "impact fx surfaces");
			LoadAsset(s, 0x51, Get<std::uint64_t>(table, 16), "impact fx entities");
			s.Pop();
		});
	}

	// --- impactsoundstable (0x37): Load_ImpactsoundstableAsset 0x7FF71E7EA280 (56 B): +8 surfacesounddef,
	// +16..+40 four entitysoundimpacts.
	bool LoadImpactSoundsTableAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x37);
		return LoadAssetHeader(s, header, 56, 8, [&](std::span<std::uint8_t> table) {
			s.Push(XBlockVirtual);
			LoadAsset(s, 0x4F, Get<std::uint64_t>(table, 8), "impact sounds surfaces");
			for (const std::size_t field : { 16, 24, 32, 40 }) {
				LoadAsset(s, 0x52, Get<std::uint64_t>(table, field), "impact sounds entities");
			}
			s.Pop();
		});
	}

	// --- xanimcurve (0xD0): Load_XanimcurveAsset 0x7FF71E7F68B0 (32 B): +8 12 B x i32 @16.
	bool LoadXAnimCurveAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0xD0);
		return LoadAssetHeader(s, header, 32, 8, [&](std::span<std::uint8_t> curve) {
			s.Push(XBlockVirtual);
			LoadIfSet(s, Get<std::uint64_t>(curve, 8), 12ull * Get<std::int32_t>(curve, 16), 4);
			s.Pop();
		});
	}
}
