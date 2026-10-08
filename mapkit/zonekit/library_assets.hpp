#pragma once
#include "asset_record.hpp"

#include <cstdint>
#include <span>

// The asset library's assets that the map's own world copies link (docs/mapkit-plan.md, P6 step 2b): the lighting's
// images and streamkeys, its sky domes (xmodels with their meshes, skeletons and materials), its klf and winddef, and
// the navvolume's streamkeys; since step 2b-2 also the models the map's entities draw (the power switch's, with its
// xcollision, and the physpreset an xcollision links); the level's sound bank, with the sound assets, ducks, alias
// modifiers and acoustics it holds; and since 2026-10-03 the zbarriers the map's barriers name. Each has a recording
// reader (asset_record.hpp), so a map's zone can carry its own copy.
// Transcribed from the IDB (bocw_fixed_renamed.i64, build 1.34.0.15931218) on 2026-10-01; the offsets are in
// library_assets.cpp. Image, streamkey, the model family and material keep their plain loaders (asset_loaders.cpp,
// model_assets.cpp), which read the same; klf and winddef get theirs here.
namespace MapKit::Zone {
	bool LoadKlfAsset(XStream& s, std::uint64_t header);     // 0x35: Load_KlfAsset 0x7FF71E7D7340
	bool LoadWindDefAsset(XStream& s, std::uint64_t header); // 0xD3: Load_WinddefAsset 0x7FF71E7F65D0
	bool LoadZBarrierAsset(XStream& s, std::uint64_t header); // 0x53: Load_ZbarrierAsset 0x7FF71E7F99E0

	// The recording reader of one of those types (image, streamkey, xskeleton, xmodelmesh, xmodel, xcollision, physpreset,
	// material, klf, winddef, sound_bank, sound_asset, sound_duck, sound_alias_modifier, sound_acoustics, zbarrier); empty
	// for any other.
	RecordReader LibraryAssetReader(std::uint64_t type);

	// True for an asset whose runtime state another asset's load writes into, from its 56-B root for a streamkey: a key
	// of type 1 to 4 (u8 @54) is installed through an owner at +40, which the asset linking it sets while it loads (the
	// lighting's streamed textures, types 2 and 3: Lighting_LinkStreamedTextureA/B_cand 0x7FF7294938A0/0x7FF7294938B0).
	// Such an asset cannot be copied over a loaded zone's: the override swap runs only after the whole zone has loaded
	// (DB_PostLoadFrame_ApplyOverrides 0x7FF727EC4A30 -> DB_SwapXAssetHeaders 0x7FF727EC4F80, a swap of the bytes), so
	// the write lands on the other zone's asset, and the swap then puts the copy's bytes, owner null, in its place (the
	// crash of 2026-10-01: StreamKeyType2_Install_cand 0x7FF729493880 reading null + 0x70). Only type 4 has a swap hook
	// that moves the owner (0x7FF72495FB50). Where no other zone holds the asset there is no swap, and a copy is fine.
	bool LoadWritesInto(std::uint64_t type, std::span<const std::uint8_t> root);
}
