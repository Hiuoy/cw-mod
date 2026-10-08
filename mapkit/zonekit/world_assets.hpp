#pragma once
#include "model_assets.hpp"
#include "xasset_list.hpp"
#include "xstream.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Loaders for the level's world assets. Registered in asset_loaders.cpp.
namespace MapKit::Zone {
	struct ZoneTrace;

	bool LoadStreamerWorldAsset(XStream& s, std::uint64_t header); // 0xAC

	// --- lighting (0xAA): Load_Lighting 0x7FF71E7DDE70, a 472-B root ------------------------------------------------
	// Transcribed from the research walker (<game>\cw-mod\mapkit\research\p4_lighting\lighting_walk.py, exact on
	// zm_silver, core_frontend and zm_gold, 2026-09-27). The root:
	//   +8/+16 lights, 688 B each (cookie image +672); +24/+32 and +40/+48 counted pairs ({i32 n, 20 or 28 B});
	//   +56/+64 the lighting volumes (9888 B each); +72/+80 their planes (16 B); +112..+136 four images; +144/+152
	//   shadow regions (464 B); +208/+216 image sets (128 B, six images at +80); +440/+448 wind volumes (40 B, a
	//   winddef at +32); +376..+432 image lists per lighting state; the rest plain arrays.
	// A volume (9888 B): +0/+4 its planes (count 0: the fallback volume, last), +12 the mask of states it has, +36
	//   an origin, then four lighting states of 2084 B from +76; +8416 4 x 128 B sun shadow per state (a 104-B
	//   streamed texture at +112), +8928 four sun cookie images, +9024 4 x 88 B reflection probes, +9376 four GI image
	//   lists, +9440 4 x 40 B sky {+0 skybox xmodel, +24 sky image, +32 klf}, +9600 4 x 64 B, +9856 four image-set
	//   indices.
	// A lighting state (2084 B; LightingState_Copy 0x7FF7258390F0, read every frame by R_SetupFrameLighting_cand
	//   0x7FF72587B650). The fields mapkit writes, named from the IDB and Die Maschine's four states
	//   (day / Dark Aether / lightning / black), 2026-09-29:
	//   +0 f32 yaw and +4 f32 pitch of the direction the SUN'S LIGHT TRAVELS (pitch down +; LightingState_GetSunDir
	//      0x7FF72946AAD0: the sun lies at -AngleVectors(pitch + [+1320], yaw + [+1324])); Die Maschine's day: yaw 120,
	//      pitch 25, so its sun sits at yaw 300, 25 degrees up; +1320/+1324 add to pitch and yaw (0 on all four);
	//   +16/+20/+24 the sun's colour (day 0.79 0.89 0.98, Dark Aether 0.07 0.17 1) and +28 its intensity (day 340,
	//      Dark Aether 1, lightning 24); a dvar can override both (R_SetupFrameLighting);
	//   +40 the sky's name as text (40 B; the skybox xmodel of the volume's +9440 entry for this state);
	//   +124/+128 the exposure range in EV (day 9.1..14, Dark Aether 8.8..12);
	//   +1336.. the atmosphere (planet radius 6.36e6 m, Rayleigh / Mie scale heights 8000 / 1200 m, coefficients);
	//   +1768.. 17 floats blended with the gfx_map's override volumes of category 8 (volumetric scattering);
	//   +1936.. the world fog, 10 floats blended with override volumes of category 7 (R_SetupFrameFog_cand
	//      0x7FF7258BC800): +1936 start distance (250), +1940 base height (-1700), +1944 halfway distance (5000),
	//      +1948 halfway height (770), +1952 0, +1956 (800; 400 in the Dark Aether with a halfway height of 400),
	//      +1960 opacity (0.9), +1964/+1968/+1972 colour in 0..255 display units (208 226 251; the Dark Aether's
	//      7 17 100), +1976 1 (0 in the black state, whose opacity is 0).
	bool LoadLightingAsset(XStream& s, std::uint64_t header);

	constexpr std::size_t kLightingRootSize = 472;
	constexpr std::size_t kLightingVolumeSize = 9888;
	constexpr std::size_t kLightingStatesAt = 76;   // in a volume
	constexpr std::size_t kLightingStateSize = 2084;
	constexpr std::size_t kLightingStateCount = 4;

	// A light (688 B; the lighting asset's +16 and the com_map's +16 hold the same list, Die Maschine 948). The client
	// game copies the com_map's into its own array when a level starts (Com_CopyPrimaryLightsToCG_cand 0x7FF7295F0520,
	// 696 B each, from g_comWorld_cand 0x7FF736C10750). Named from the copy function's field boundaries
	// (Light_Copy688_cand 0x7FF725828760) and Die Maschine's values, 2026-10-05:
	//   +64 u32 type (2 on 890 of Die Maschine's, 4 on 58), +72 u32 a name hash, +104 the origin, +116.. its axes,
	//   +200 the colour times the intensity (linear; up to 9.5e6), +232 and +640 the radius, +336 the cone in degrees
	//   (360 on a type 2), +388 a 64-B name text (the cookie's), +524 the origin again and +536/+548 its bounds, +624 the
	//   colour's largest part, +644 the colour again, +672 the cookie image.
	constexpr std::size_t kLightSize = 688;
	constexpr std::size_t kLightColor = 200;      // 3 x f32
	constexpr std::size_t kLightColorAgain = 644; // 3 x f32, equal to +200 on every one of Die Maschine's

	// A baked shadow tree of a lighting asset (the engine's "SST": the strings "DrawSST" and "SST Only" of the exe). A
	// tree is the sun's shadow of the level's static geometry, baked for one sun direction and drawn beyond the reach of
	// the shadow maps the game renders each frame. Each lighting state of a volume has one (the 128-B record at volume
	// +8416, R_SetupFrameLighting_cand: frame +2792), and so has each of a shadow region's four entries (root +152, 464 B:
	// +0 first plane, +4 planes, then 4 x 112 B).
	// The record's body (96 B; at +16 of a volume's record, +0 of a region's entry), from Die Maschine's and
	// core_frontend's, 2026-10-05:
	//   +0 three rows of four floats (world to the tree's space; the third is the direction the light travels),
	//   +48 an origin, +64/+68 f32 the tree's size in tiles (Die Maschine's day sun 61 x 34), +72 f32 the texel size
	//   (2.79; a tile is 128 texels: R_SunShadowTreeConstants_cand 0x7FF729475900), +76 f32 the depth range, +80/+84 f32
	//   the yaw and pitch it was baked for (120 / 25: Die Maschine's day sun), then the pointer to the tree (+96).
	// The tree (104 B, Load_LightingStreamedTexture104_cand 0x7FF71E7DA810; -1 = stored after the record, else shared with
	// another record): +0 a streamkey (type 2 a volume's, 3 a region's; the packages hold the data under the key's +8),
	//   +8 / +16 u32 per level (+28 of them): where each level's nodes start in that data and how many, +24 u32 the node
	//   count, +80 an image holder (40 B: +0/+4 u32 512 x 512, +8/+12 f32 the part of the image the tiles use, +24 the
	//   image, +32 f32 the depth range), +88 pixels and +96 a holder for the image drawn until the data has streamed in.
	//   Lighting_StreamedTexture_Install_cand 0x7FF7294939B0 makes the node buffer (4 B a node) and gives the image its
	//   pixels, which follow the nodes at the next 64 KB.
	// An empty tree is how a retail zone says "no baked shadow here": one node, the image's used part 1/511, the depth
	// range 393216, a 786,432-B key (every one of them the same data), under a record of no tiles. Die Maschine's
	// fallback volume and its invalid state have one, and so have core_frontend's volumes 0, 1 and 4 in the states they
	// run in.
	constexpr std::size_t kShadowRecordSize = 96;
	constexpr std::size_t kShadowTreeSize = 104;
	constexpr std::size_t kShadowTreeNodeCount = 24;   // u32, in a tree
	constexpr std::size_t kShadowHolderUsed = 8;       // 2 x f32, in a tree's image holder
	constexpr std::size_t kShadowHolderDepth = 32;     // f32
	constexpr std::size_t kShadowRecordTiles = 64;     // 2 x f32, in a record's body

	// A lighting asset copied whole from a traced zone: its stream bytes from the root on, every field that links
	// another asset (an image, a skybox model, a streamkey...), and every pointer into its own data (Die Maschine's
	// states share 47 pieces). Written back as a CopiedAsset with those links and pointers placed again, it loads
	// exactly as the original, with whatever the caller changed in its states.
	struct LightingCopy {
		struct Link {
			std::size_t at = 0;        // the 8-B field, as an offset into bytes
			std::uint64_t type = 0;
			std::uint64_t name = 0;    // the linked asset's name
		};
		struct Pointer {
			std::size_t at = 0;        // the 8-B field, as an offset into bytes
			std::size_t target = 0;    // the byte of bytes it points at
		};
		// One record of a baked shadow tree, and the tree it leads to (its own, stored after it, or another record's).
		struct ShadowRecord {
			std::size_t body = 0;          // the record's 96-B body, as an offset into bytes
			bool region = false;           // a shadow region's entry (else a volume state's sun shadow)
			std::size_t tree = SIZE_MAX;   // the 104-B tree it points at, as an offset into bytes
		};
		// A tree the asset stores, with the stream key that names its data in the packages.
		struct ShadowTree {
			std::size_t at = 0;            // as an offset into bytes
			std::size_t nodeCounts = SIZE_MAX; // its +16 array (u32 per level)
			std::size_t holder = SIZE_MAX;     // its +80 image holder, when stored with it
			std::uint64_t key = 0;         // the streamkey's name
			std::uint64_t package = 0;     // the streamkey's +8: what the packages hold its data under
			std::uint32_t size = 0;        // the streamkey's +48: that data's size
		};
		// A stream key to change in the zone's copy of it: the data it names in the packages, and that data's size.
		struct KeyEdit {
			std::uint64_t key = 0;
			std::uint64_t package = 0;
			std::uint32_t size = 0;
		};
		std::vector<std::uint8_t> bytes;
		std::vector<Link> links;
		std::vector<Pointer> pointers;
		std::size_t volumesAt = 0;     // the volumes, as an offset into bytes
		std::uint32_t volumeCount = 0;
		std::size_t lightsAt = 0;      // the lights (kLightSize each), as an offset into bytes
		std::uint32_t lightCount = 0;
		std::vector<ShadowRecord> shadowRecords;
		std::vector<ShadowTree> shadowTrees;
		// Lighting state `state` of volume `volume`, inside bytes.
		std::span<std::uint8_t> State(std::size_t volume, std::size_t state);
		// Every light's colour set to black: the lights stay where the asset's other lists expect them and light nothing.
		// Returns how many gave light before.
		std::size_t DarkenLights();
		// Every baked shadow tree made an empty one, modelled on an empty tree the asset already holds (of a volume's for
		// the volumes', of a region's for the regions'): its records as that tree's, one node, and its stream key to name
		// the empty tree's data (`keys`: the caller changes its copies of those keys; the tree's own image stays, every
		// one being 512 x 512 of one format). Returns how many trees it emptied; false (with error) when a kind of tree
		// has no empty one to model on, or a baked tree's parts are not stored with it. The copy may then be changed in
		// part: the caller gives it up.
		bool EmptyShadowTrees(std::vector<KeyEdit>& keys, std::size_t& emptied, std::string& error);
	};
	// False (with error) when the asset is not a lighting asset the zone stores, or stores assets inside it or points
	// into other assets' data (only links to other assets and pointers into its own data are placed again).
	bool CopyLighting(const ZoneTrace& trace, std::span<const std::uint8_t> stream, const XAssetList& list,
		std::size_t asset, LightingCopy& out, std::string& error);

	// What a streamerworld holds, kept raw where the meaning is not settled yet. Model fields hold the
	// stored pointer: a reference (block << 60 | offset) + 1 to an xmodel loaded earlier, or kPtrInline.
	struct StreamerWorldGroupEntry {
		std::array<std::uint8_t, 72> header{};
		std::vector<std::array<std::uint8_t, 52>> records; // placement-sized: origin, 3x3 axis, scale?
	};

	struct StreamerWorldCell {
		std::array<std::uint8_t, 256> raw{};
		std::vector<std::array<std::uint8_t, 24>> lists;              // cell +8+88: 24-B {?, u64 n, 36-B records}
		std::vector<std::vector<std::array<std::uint8_t, 36>>> listRecords;
		std::vector<std::array<std::uint8_t, 12>> gridVectors;        // cell +8+16 grid: 12 B x n
		std::vector<std::array<std::uint8_t, 48>> keyed;              // cell +8+8: 48-B records, streamkey @+8
		std::vector<std::array<std::uint8_t, 16>> models;     // {xmodel, 8 B}
		std::vector<std::array<std::uint8_t, 32>> modelInfo;  // 32 B per model
		std::array<std::vector<StreamerWorldGroupEntry>, 8> groups;
	};

	struct StreamerWorld {
		std::array<std::uint8_t, 224> root{};
		std::vector<std::uint64_t> models;          // +176/+184: the world's xmodel list
		std::vector<std::uint8_t> records40;        // +64/+72
		std::vector<std::uint8_t> records32;        // +96/+104
		std::vector<StreamerWorldCell> cells;       // +80/+88
	};

	// Decodes one streamerworld asset (the stream positioned at its start) into out.
	bool ReadStreamerWorld(XStream& s, std::uint64_t header, StreamerWorld& out);

	// The streamer's cells (qword_7FF7353474C0 = DB_FindXAssetHeader(0xAB) at world load): each district has an
	// id, bounds and ten typed streamkeys; slot 9 is the district's clip map cell (its collision).
	bool LoadDistrictsAsset(XStream& s, std::uint64_t header); // 0xAB

	bool LoadClipMapAsset(XStream& s, std::uint64_t header); // 0x18

	// sub_7FF71E7E7E80 (32 B, embedded): a settings tree, {u64 n, 32-B entries}, {u64 n, 32-B groups of
	// recursive children}. Used by dynent defs and vehicles.
	void LoadSettingsTree(XStream& s, std::span<const std::uint8_t> node);
	// sub_7FF71E7FA920 on an already loaded 32-B collision tree index: +0 8 B x u32 @24, +8 4 B x u32 @24,
	// +16 bytes x u32 @28.
	void LoadCollisionTreeIndexBody(XStream& s, std::span<const std::uint8_t> index);

	// A clip map cell (64 B, clip map +8, u16 @462 of them): +24 a byte blob (u32 @32), +40 80-B records
	// (u32 @48), each {+0 xcollision, ..., +68 script string}.
	struct ClipMapCell {
		std::array<std::uint8_t, 64> raw{};
		std::vector<std::uint8_t> blob;
		std::vector<std::array<std::uint8_t, 80>> records;
	};

	// The clip map's +160: the terrain collision, a heightfield (72 B, entries x u32 @60 at +64).
	// - Entry (176 B): a grid of tiles, +4/+8 tiles in x/y, the tiles (80-B records, u32 @0 at +24), +112
	//   height offset and +148 height scale (z = u16 * scale + offset; zm_silver 0.12498665, -122), +152
	//   the surface list (24-B items {u64 name hash, ...}, u32 @160).
	// - Tile (80 B): +0 mins, +12 maxs, +24 grid origin, +32 u16 n = (+34 u16) x (+36 u16) heights (33 x 33
	//   on zm_silver, 512 units a side), +38 u16 which quads exist (0 none, 1 all, else the +48 bits; zm_silver 1003 x 1,
	//   21 x 2), +40 the u16 heights, +48 n bits (a quad exists when its corner's bit is set), +56 the
	//   surface blob (u32 @72 bytes), +64 the tile's offset into a runtime height buffer, +68 u16 surface
	//   count then (count 1) the surface index or (more) bits per quad, the blob holding the list and the
	//   packed indices. A retail tile often points at an earlier tile's heights or blob (identical data).
	struct ClipMapWorldRecord {
		std::array<std::uint8_t, 80> raw{};
		std::vector<std::uint8_t> indices; // +40 heights, when stored inline
		std::vector<std::uint8_t> bits;    // +48
		std::vector<std::uint8_t> blob;    // +56, when stored inline
	};
	struct ClipMapWorldEntry {
		std::array<std::uint8_t, 176> raw{};
		std::vector<ClipMapWorldRecord> records;
		std::vector<std::vector<std::uint8_t>> list; // empty where the stored pointer was not inline
	};

	// Load_ClipMap 0x7FF71E7FA1D0 (472 B). Kept raw where the meaning is not settled yet.
	struct ClipMap {
		std::array<std::uint8_t, 472> root{};
		std::vector<ClipMapCell> cells;          // +8
		std::vector<std::uint8_t> cellIndex;     // +16, 4 B x u16 @462
		std::vector<CollisionTree> trees;        // +48, u32 @436
		bool hasTreeIndex = false;               // +56 (sub_7FF71E7FA920, 32 B)
		std::array<std::uint8_t, 32> treeIndex{};
		std::array<std::vector<std::uint8_t>, 3> treeIndexArrays; // +0 8 B / +8 4 B x u32 @24, +16 bytes x u32 @28
		std::vector<std::uint8_t> array72;       // +72, 4 B x u32 @440
		std::vector<std::uint8_t> array80;       // +80, 2 B x u32 @444
		std::vector<std::uint64_t> collisions;   // +64, u32 @424: xcollision pointers as stored
		std::vector<std::vector<std::uint8_t>> models; // +24, u32 @428: the 992-B structs
		std::vector<std::array<std::uint8_t, 96>> dynEnts; // +96, u16 @88: +0 = the 288-B def as stored (kPtr*)
		bool hasWorld = false;
		std::array<std::uint8_t, 72> worldHead{};  // +160
		std::vector<ClipMapWorldEntry> worldEntries;
		// +168: 64 script-string indices into the zone's own table, as text (ResolveClipMapStrings in
		// world_writer.hpp; the reader has no table). Empty = no string.
		std::array<std::string, 64> rootStrings;
		// Stream bytes each part took, in load order, for sizing a writer against the retail layout.
		std::vector<std::pair<std::string, std::size_t>> parts;
	};

	// Decodes one clip_map asset (the stream positioned at its start) into out.
	bool ReadClipMap(XStream& s, std::uint64_t header, ClipMap& out);
}
