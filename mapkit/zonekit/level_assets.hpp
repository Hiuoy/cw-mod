#pragma once
#include "asset_record.hpp"

#include <cstdint>

// The level's world assets besides the gfx_map, clip map, streamerworld and districts (docs/mapkit-plan.md, P6 step
// 2): each is found by the map's .d3dbsp name (terraingfx, staticlevelfxlist, glasses, com_map, cpu_occlusion_data,
// navvolume) or is the one asset of its pool (game_map), so a map that loads without its asset library's zone needs
// its own. Each has a loader for the zone walk and a recording reader (asset_record.hpp) for copies. Transcribed from
// the IDB (bocw_fixed_renamed.i64, build 1.34.0.15931218) on 2026-09-30; the offsets are in level_assets.cpp.
namespace MapKit::Zone {
	bool LoadTerrainGfxAsset(XStream& s, std::uint64_t header);        // 0xB1: Load_TerraingfxAsset 0x7FF71E7EBBC0
	bool LoadStaticLevelFxListAsset(XStream& s, std::uint64_t header); // 0x7F: Load_StaticlevelfxlistAsset 0x7FF71E7EBF40
	bool LoadGlassesAsset(XStream& s, std::uint64_t header);           // 0x43: Load_GlassesAsset 0x7FF71E7DCD30
	bool LoadComWorldAsset(XStream& s, std::uint64_t header);          // 0x19: Load_ComMapAsset 0x7FF71E7D1080
	bool LoadCpuOcclusionDataAsset(XStream& s, std::uint64_t header);  // 0xA9: Load_CpuOcclusionDataAsset 0x7FF71E7D19D0
	bool LoadGameWorldAsset(XStream& s, std::uint64_t header);         // 0x1A: Load_GameMapAsset 0x7FF71E7D8420
	bool LoadNavVolumeAsset(XStream& s, std::uint64_t header);         // 0x76: Load_NavvolumeAsset 0x7FF71E7E2120

	// The recording reader of one of those types; empty for any other.
	RecordReader LevelAssetReader(std::uint64_t type);

	// The sub-arrays a copy cuts to leave only what the level needs from it, by name, with the root counts that size
	// them. An empty staticlevelfxlist has no entries and no strings; an empty terraingfx keeps only its +224 block (the
	// map-wide textures the renderer binds whatever the terrain: without them every render thread read a null image
	// on texture slot 105, 2026-09-26 00:31).
	std::vector<RecordCut> EmptyStaticLevelFxListCuts();
	std::vector<RecordCut> TerrainGfxWithoutTilesCuts();
}
