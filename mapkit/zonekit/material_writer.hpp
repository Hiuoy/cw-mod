#pragma once
#include "xasset_list.hpp"
#include "xwriter.hpp"
#include "zone_trace.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The creator's own textures (docs/mapkit-plan.md, P4): images whose pixels the zone holds itself, and materials
// copied from a retail one with those images in place of its own.
//
// Images (GfxImage, 208 B; loader at LoadImageBody, asset_loaders.cpp). A RESIDENT image (+144 & 0x10 clear)
// keeps its pixels in block 6 (+40, +152 bytes) and has no mip records; Load_GfxImage 0x7FF71E7D95E0 hands it
// to Image_PostLoad_cand 0x7FF729114050, which builds the GPU texture at once (Image_CreateResident_cand
// 0x7FF729113680; IDB 2026-09-28). What that reads:
//   +156 u32 DXGI format, +160/+162/+164 u16 width / height / depth, +181 u8 type (1 = 2D), +182 u8 mip levels,
//   +144 flags. The pixels are the levels back to back from level 0, each Image_GetMipSize_cand(format, w >> l,
//   h >> l); levels past +182 are ignored.
// The other header bytes, copied from Die Maschine's resident 0x20 images with mips (the 2048 x 2048 BC1/BC4/BC7
// set, assets 141946-141948): +148 u32 an id per image (texture sets name images by it), +166 0x02, +168/+169 1,
// +172/+174 the size again, +177 = +182, +179 4, +180 the usage (2 colour, 4 normal map, 7 roughness map, 5 a BC4
// specColorMap0; the streamed images' too). The link callback (DB_Image_LinkCallback 0x7FF7295ED880) does nothing
// for a resident image.
//
// What the maps hold (ffinfo --semantics names each semantic; read from Die Maschine's images 2026-09-28):
//   colorMap0 A0AB1041 (BC1/BC7 sRGB); perceptualRoughnessMap E9817F0D (BC4: r = perceptual roughness);
//   normalMap0 59D30D0F (BC7): x, y in r, g with +y DOWN the image (the game's bitangent, sign * cross(n, t), runs
//   along +dP/dv on every retail mesh checked, and the retail maps' slopes are curl-free that way), b the roughness
//   a level's spread of normals adds (it grows level by level down a full chain: ~ sqrt(2 * (1 - |mean n|^2))), a 1;
//   specColorMap0 EC443804: a BC4 metalness (usage 5) on the metal techsets mapkit copies (an sRGB specular colour,
//   BC1, usage 6, on others); emissiveMap0 34614347: the glow's colour (BC1/BC7 sRGB, usage 2), its strength a
//   constant of the material (+292 on techset 2563079AB548D2FD). The slots mapkit leaves linked hold shared images: ambient occlusion 07176BF2
//   ($white_ao or $occlusion), reveal mask 199A03D3 ($white_reveal or $reveal), thermal 389DD40F
//   ($gray_32_one_channel); a colour map's alpha is the opacity of a cut-out or see-through material.
//
// Materials (344 B; loader at LoadMaterialBody, model_assets.cpp). A retail material is copied whole: its root,
// the +128 block's records and their vectors, the constant buffer (block 6), the image table, the +56 list, the
// texture sets and the +288 arrays. Retail zones share identical pieces between materials (a stored reference
// to data an earlier asset loaded); the copy resolves each to its bytes (ResolveStoredData) and writes it inline.
// The techset and every image stay links by name, so the zone needs an entry for each (EncodeAssetReference for
// the retail ones) before the material.
namespace MapKit::Zone {
	struct ResidentImage {
		std::uint64_t name = 0;
		std::uint32_t format = 0;  // DXGI_FORMAT
		std::uint16_t width = 0;
		std::uint16_t height = 0;
		std::uint8_t levels = 1;   // the levels the GPU texture gets (+177, +182); `pixels` may hold more
		std::uint8_t usage = 2;    // +180: 2 colour, 4 normal map, 5 metal map, 7 roughness map
		std::vector<std::uint8_t> pixels; // level 0 first
		std::uint32_t Id() const { return static_cast<std::uint32_t>(name); } // +148
	};
	void EncodeResidentImage(XWriter& w, const ResidentImage& image);

	// Bytes of one w x h level in a DXGI format (block formats pad to 4 x 4 blocks). 0: not a format mapkit writes.
	std::size_t ImageLevelBytes(std::uint32_t format, std::uint32_t width, std::uint32_t height);
	// How many levels the GPU texture gets from a chain starting at w x h: while both sides are at least 4 (every
	// level whole blocks), at most 7 (Die Maschine's largest resident image, 2048 x 2048, uses 7: 2048 to 32).
	std::uint8_t ResidentImageLevels(std::uint32_t width, std::uint32_t height, std::uint32_t available);

	// A .dds file (a DX10 header, or the legacy DXT1/DXT3/DXT5/ATI1/ATI2/BC4U/BC5U/RGBA ones): the texture a
	// creator's editor exported.
	struct DdsImage {
		std::uint32_t format = 0; // DXGI_FORMAT
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t levels = 0;
		std::vector<std::uint8_t> pixels; // every level, level 0 first
	};
	bool ReadDds(const std::filesystem::path& path, DdsImage& out, std::string& error);

	// A retail material, self-contained: everything it loads or points at, as bytes, plus the names of the assets
	// it links (techset, images).
	struct MaterialCopy {
		std::array<std::uint8_t, 344> root{};
		std::uint64_t techset = 0;
		struct Record {
			std::array<std::uint8_t, 64> raw{};
			std::array<std::optional<std::array<std::uint8_t, 12>>, 8> vectors;
		};
		std::array<std::optional<Record>, 13> records;
		std::vector<std::uint8_t> constants;
		struct TableImage {
			std::array<std::uint8_t, 24> raw{}; // +8 u32 semantic, the rest the table's own
			std::uint64_t image = 0;            // the linked image's name
			std::uint32_t id = 0;               // its +148 id when the zone stores it (0 for a by-name reference)
			std::uint32_t Semantic() const;
		};
		std::vector<TableImage> images;
		std::vector<std::uint8_t> list56;
		struct TextureSet {
			std::array<std::uint8_t, 24> root{};
			std::vector<std::uint8_t> data;     // 4 B x u32 @16: image ids (+148) and the settings between them
			std::vector<std::uint8_t> entries;  // 16 B x u32 @20: an image link, then its own fields
			std::vector<std::uint64_t> entryImages;
		};
		std::array<std::optional<TextureSet>, 8> sets;
		std::vector<std::uint8_t> tail296;
		std::vector<std::uint8_t> tail312;
	};
	// Reads material `asset` of a traced zone into out. False (with error) when a piece cannot be resolved or the
	// material has a local techset (not handled).
	bool CopyMaterial(const ZoneTrace& trace, std::span<const std::uint8_t> stream, const XAssetList& list,
		std::size_t asset, MaterialCopy& out, std::string& error);

	// One image of the copy replaced by another: the table entry with `semantic` links `image` instead, and the
	// texture sets follow (their entries link it, their data names its id; only when the zone stored the old image, as a
	// by-name image's id is unknown here, which drew right on 2026-09-28). False when the table has no such entry.
	struct ReplacedImage {
		std::uint64_t name = 0;
		std::uint32_t id = 0;
	};
	bool ReplaceMaterialImage(MaterialCopy& copy, std::uint32_t semantic, const ReplacedImage& image);

	// Writes the copy under `name`, every piece inline; `materialId` goes to +16 (a u32 each retail material has
	// its own of). The techset and images link through w.AssetEntry.
	void EncodeMaterialCopy(XWriter& w, const MaterialCopy& copy, std::uint64_t name, std::uint32_t materialId);
}
