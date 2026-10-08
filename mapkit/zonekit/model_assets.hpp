#pragma once
#include "asset_loaders.hpp"
#include "xstream.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// Loaders for the model family: an xmodel and everything it pulls in (skeleton, collision, LOD meshes,
// materials) plus the small physics assets under collision. Registered in asset_loaders.cpp.
namespace MapKit::Zone {
	// Load_XModelMeshSurfaceData's 464-B struct (Greyhound's BOCWXModelMeshInfo): where a LOD's vertex and
	// index data sit inside its mesh buffer. The buffer is resident (block 12, kept in `resident`) or
	// streamed from an .xsub under the LOD's stream key (flags & 1).
	struct MeshInfo {
		std::array<std::uint8_t, 464> raw{};
		std::uint32_t Flags() const { return raw[0]; }
		bool Streamed() const { return (Flags() & 1) != 0; }
		bool ExtendedVertices() const { return (Flags() & 64) != 0; } // 24-B vertex records instead of 16
		std::uint32_t VertexCount() const;
		std::uint32_t FaceCount() const;
		std::uint32_t BufferSize() const;   // +40
		std::uint32_t PositionOffset() const; // +44: f32 x3 per vertex
		std::uint32_t VertexOffset() const;   // +48: color, half UV, packed normal/tangent
		std::uint32_t FaceOffset() const;     // +52: u16 x3 per triangle
		std::uint32_t WeightOffset() const;   // +56
		std::vector<std::uint8_t> resident;
		// Where the info and its resident buffer landed, (block << 60) | position: the value a later reference to
		// them stores, minus one. 0 when not loaded inline here.
		std::uint64_t at = 0;
		std::uint64_t residentAt = 0;
	};

	// One surface of a LOD (48 B, Greyhound's BOCWXModelSurface).
	struct MeshSurface {
		std::array<std::uint8_t, 48> raw{};
		std::uint16_t VertexCount() const;
		std::uint16_t FaceCount() const;
		std::uint32_t FirstVertex() const; // +8, in vertices
		std::uint32_t FirstIndex() const;  // +12, in u16 indices
	};

	// An xmodelmesh (one LOD, Greyhound's BOCWXModelLod).
	struct XModelMeshData {
		std::size_t cursor = 0;          // stream offset of the root
		std::array<std::uint8_t, 64> root{};
		std::uint64_t Name() const;      // +0 name hash
		std::uint64_t StreamKey() const; // +32: the .xsub key of the mesh buffer when streamed
		bool hasInfo = false;            // false when +16 was a reference to a mesh info loaded earlier
		MeshInfo info;
		std::vector<MeshSurface> surfaces;
	};

	// Decodes one xmodelmesh asset (the stream positioned at its start) into out.
	bool ReadXModelMesh(XStream& s, std::uint64_t header, XModelMeshData& out);

	// An xmodel (232 B, layout in model_writer.hpp) with what it loaded inline: LODs stored inside it rather
	// than as assets of their own, and the name of each material it loaded inline.
	struct XModelData {
		std::array<std::uint8_t, 232> root{};
		std::uint64_t Name() const;              // +0, top bit cleared
		std::uint16_t LodCount() const;          // +112
		std::uint64_t Lod(std::size_t i) const;  // +32 + 8i as stored: inline, or a reference to an earlier LOD
		std::array<float, 3> Mins() const;       // +184
		std::array<float, 3> Maxs() const;       // +196
		float Radius() const;                    // +216
		std::array<XModelMeshData, 8> lods{};    // the LODs loaded inline (hasInline[i])
		std::array<bool, 8> hasInline{};
		// Per LOD, one material per surface (the table's +8 handles): the pointer as stored, and the name when the
		// material was loaded inline here (else 0: a reference to resolve).
		struct Material {
			std::uint64_t stored = 0;
			std::uint64_t name = 0;
		};
		std::vector<std::vector<Material>> materials;
		std::vector<std::vector<Material>> materials16; // the table's +16 handles, the same way (empty when null)
	};

	// Decodes one xmodel asset (the stream positioned at its start) into out.
	bool ReadXModel(XStream& s, std::uint64_t header, XModelData& out);

	// A material (344 B, layout at LoadMaterialBody in model_assets.cpp) with the data it stored inline. Each
	// vector is empty when its field was null or a reference to earlier data.
	struct MaterialData {
		std::array<std::uint8_t, 344> root{};
		std::uint64_t Name() const; // +0, top bit cleared
		std::vector<std::vector<std::uint8_t>> records; // the +128 block's 64-B records stored here
		std::vector<std::uint8_t> constants;  // the +128 block's constant buffer (+152, block 6)
		std::vector<std::uint8_t> imageTable; // +48: 24 B per image (u8 count @328)
		std::vector<ImageData> inlineImages;  // table images stored inside the material, in table order
		std::vector<std::uint8_t> list56;     // +56: 8 B x u8 @329
		struct TextureSet {
			std::vector<std::uint8_t> root;    // 24 B
			std::vector<std::uint8_t> data;    // +0: 4 B x u32 @16
			std::vector<std::uint8_t> entries; // +8: 16 B x u32 @20, image @+0
		};
		std::vector<TextureSet> sets;         // +64: up to eight
		std::vector<std::uint8_t> tail296;    // +296: 24 B x u32 @304
		std::vector<std::uint8_t> tail312;    // +312: 8 B x u32 @320
		bool localTechset = false;            // +336 stored here
	};

	// Decodes one material asset (the stream positioned at its start) into out.
	bool ReadMaterial(XStream& s, std::uint64_t header, MaterialData& out);

	// A collision tree (184 B, 0x7FF71E7F9A90): the world trees of a clip map and the trees under an
	// xcollision. Its arrays, in load order; each one's size comes from a count in the root:
	//   0 +64  16 B x u32 @24      5 +80  8 B x ceil((@24 + 6 * @32) / 6)   10 +168 32 B x u32 @48
	//   1 +72  12 B x u32 @28      6 +128 28 B x u32 @32                     11 +152 bytes, u32 @160
	//   2 +104 4 B x u32 @32       7 +88  bytes, u32 @96                     12 +176 44 B x u32 @56
	//   3 +112 4 B x u32 @32       8 +136 12 B x u32 @40
	//   4 +120 4 B x u32 @32       9 +144 bytes, u32 @44
	constexpr std::size_t kCollisionTreeArrayCount = 13;
	// Where each array's pointer sits in the root, its alignment, and whether the loader pushes block 9
	// for it (which stays in the current block anyway) or loads it in the current block directly.
	struct CollisionTreeArray {
		std::size_t field;
		std::uint64_t alignment;
		bool pushed9;
	};
	constexpr CollisionTreeArray kCollisionTreeArrays[kCollisionTreeArrayCount] = {
		{ 64, 4, true }, { 72, 4, true }, { 104, 4, true }, { 112, 4, true }, { 120, 4, true }, { 80, 8, false },
		{ 128, 4, true }, { 88, 1, true }, { 136, 4, true }, { 144, 1, true }, { 168, 8, false }, { 152, 1, true },
		{ 176, 4, false },
	};
	struct CollisionTree {
		std::array<std::uint8_t, 184> raw{};
		std::array<std::vector<std::uint8_t>, kCollisionTreeArrayCount> arrays;
	};
	// Loads a tree's arrays (its 184-B root already loaded into `tree`); out may be null.
	void ReadCollisionTree(XStream& s, std::span<const std::uint8_t> tree, CollisionTree* out);
	bool LoadPhysPresetAsset(XStream& s, std::uint64_t header);        // 0x02
	bool LoadPhysConstraintsAsset(XStream& s, std::uint64_t header);   // 0x03
	bool LoadXModelAsset(XStream& s, std::uint64_t header);            // 0x06
	bool LoadXCollisionAsset(XStream& s, std::uint64_t header);        // 0x07
	bool LoadXSkeletonAsset(XStream& s, std::uint64_t header);         // 0x08
	bool LoadXModelMeshAsset(XStream& s, std::uint64_t header);        // 0x09
	bool LoadMaterialAsset(XStream& s, std::uint64_t header);          // 0x0A
	bool LoadImpactFxTableAsset(XStream& s, std::uint64_t header);     // 0x36
	bool LoadImpactSoundsTableAsset(XStream& s, std::uint64_t header); // 0x37
	bool LoadXAnimCurveAsset(XStream& s, std::uint64_t header);        // 0xD0
}
