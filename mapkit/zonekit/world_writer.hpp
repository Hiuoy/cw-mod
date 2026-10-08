#pragma once
#include "world_assets.hpp"
#include "xwriter.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Encoders for a level's world assets, each the mirror of its loader in world_assets.cpp. They write what a
// mapkit map carries, which is less than a retail map: the parts a map built from scratch has no use for
// are left out (see each function).
namespace MapKit::Zone {
	// A clip map as mapkit writes it (Load_ClipMap 0x7FF71E7FA1D0): the root, the cell index (+16), the
	// collision trees (+48), their index (+56), the runtime block (+152) and the terrain heightfield (+160).
	// No cells (+8), static model collision (+24/+32/+40 992-B structs, +64 xcollisions), dynents of the map's
	// own (+72/+80, +120/+136; counts +90 and +460 zeroed) or constraints (+144). The dynent array (+96, count
	// +88) holds only the kDynEntDebrisSlots empty slots, with their +104/+112/+128 runtime arrays.
	// Every pointer is written from what `map` holds, whatever the root says, and the +168 script strings
	// from map.rootStrings through the writer's table: the asset must declare ClipMapScriptStrings(map).
	void EncodeClipMap(XWriter& w, const ClipMap& map);

	// The dynents the game spawns at runtime, a dying zombie's gibs among them, go into clip map dynent slot
	// @460 + (a free one of these), whose +96/+104/+112/+128 entries it writes (DynEnt_Create_cand
	// 0x7FF72406D7A0). Retail maps carry exactly this many empty slots after their own dynents (zm_silver
	// 518 = 18 + 500). Without them the first gib writes through null (test G1, 20:32).
	constexpr std::size_t kDynEntDebrisSlots = 500;

	// The clip map's +168: 64 script-string indices (Finish_ClipMap 0x7FF71E83C3E0 converts them at link
	// time, with the other zone-local ones in cells and dynents, which mapkit does not write).
	constexpr std::size_t kClipMapStringsField = 168;
	// Fills map.rootStrings from the root's indices and the table of the zone the clip map was read from.
	// False (with a reason) for an index the table does not hold.
	bool ResolveClipMapStrings(ClipMap& map, std::span<const std::optional<std::string>> zoneStrings, std::string& error);
	// The distinct strings EncodeClipMap will store, for ZoneAsset::scriptStrings.
	std::vector<std::string> ClipMapScriptStrings(const ClipMap& map);

	// The plane map's clip map, made from a retail one: the same trees (the map's triggers use them) and
	// terrain grid, every tile flat at the height nearest `z` and of one surface, and none of the parts
	// EncodeClipMap leaves out. Returns false (with a reason) when the terrain is not the shape seen in
	// retail maps. `height` receives the floor height the heights encode.
	bool MakePlaneClipMap(const ClipMap& retail, float z, ClipMap& out, float& height, std::string& error);

	// A convex hull as a collision tree stores it: an AABB, the planes that are not the AABB's own (inside
	// when dot(normal, p) - distance <= 0), its corners and its contents. The engine's sweep
	// (CM_Hull_TraceCapsule 0x7FF7295F32C0) uses the AABB as six axial planes, then the extra planes; it never
	// reads the corners.
	struct ConvexHull {
		std::array<float, 3> mins{};
		std::array<float, 3> maxs{};
		std::vector<std::array<float, 4>> planes;
		std::vector<std::array<float, 3>> vertices;
		std::uint32_t contents = 1; // 1 = solid (what retail brush-model hulls hold)
		// Off for an obstacle that is no floor: the navmesh goes round it and never over its top, however low it is
		// (GenerateNavPolygons). Collision does not read it.
		bool walkable = true;
	};

	// What a hull blocks: its contents, which every trace tests against its own mask (CM_TreeHull_Trace
	// 0x7FF7295F5DC0: mask & contents & 0x3FFFFFF). kHullSolid stops everything, as a wall does. kHullPlayerClip stops
	// players and lets bullets, grenades and zombies through: the contents of the game's own collision_player_wall_*
	// models, 0x10000 (player clip, in a player's movement mask 0xA18011) and 0x40000 (player vehicle clip).
	constexpr std::uint32_t kHullSolid = 0x1;
	constexpr std::uint32_t kHullPlayerClip = 0x50000;

	// The hull of a box given as its center and three half-axis vectors (rotated, scaled or sheared: an
	// .mkmap brush). False (with a reason) for a flat or degenerate box.
	bool BoxHull(const std::array<float, 3>& center, const std::array<std::array<float, 3>, 3>& halfAxes,
		std::uint32_t contents, ConvexHull& out, std::string& error);

	// The hull around a model's points (its own space): the 26-DOP, one plane across each of the model's 3 axes,
	// 12 edge and 8 corner directions, both ways, at the farthest point. It is made in the model's axes, so a boxy
	// model gets a tight box; a concave one is filled in. Then it is turned by `axes` (the model's X, Y and Z in the
	// world, unit length), scaled and moved to `origin`. A shape thinner than 1 unit along a direction is widened
	// to 1. False (with a reason) for no points or too many corners.
	bool MeshHull(std::span<const std::array<float, 3>> points, const std::array<std::array<float, 3>, 3>& axes, float scale,
		const std::array<float, 3>& origin, std::uint32_t contents, ConvexHull& out, std::string& error);

	// One triangle made solid: a slab `thickness` deep behind its front face (the side `front` points to), so what
	// players touch is the face itself. False (with a reason) for a triangle with no area.
	bool TriangleHull(const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c,
		const std::array<float, 3>& front, float thickness, std::uint32_t contents, ConvexHull& out, std::string& error);

	// Appends hulls to a collision tree: tree 0 of a clip map is the world, which the engine traces every
	// time (CM_World_Trace_cand 0x7FF7293E3270), with a linear walk over its hulls when it has no BVH (+88).
	// Every face gets `surface`, an index into the clip map's +56 surface table (the load remaps it,
	// CM_Tree_RemapFaceSurfaces 0x7FF7292AC0F0). The tree's bounds grow to hold the hulls, 1 unit wider as
	// retail's are. False (with a reason) for a tree with a BVH, or past the packed fields' limits.
	bool AppendHulls(CollisionTree& tree, std::span<const ConvexHull> hulls, std::uint16_t surface, std::string& error);

	// An empty streamerworld (Load_StreamerWorld 0x7FF71E7ED3B0) named like `retailRoot` (+0): no cells,
	// models or grid, no terraingfx, and the level's lighting (+216) linked by name from the map's own
	// zone. Fields that are neither counts nor pointers (+8, +128..+175) keep the retail values.
	void EncodeEmptyStreamerWorld(XWriter& w, const std::array<std::uint8_t, 224>& retailRoot, std::uint64_t lightingName);

	// An empty districts (Load_Districts 0x7FF71E7D4440) named like `retailRoot` (+0): no districts, district
	// sets or pointer table, so the streamer streams nothing, the old world's collision cells included
	// (CM_ForEachStreamedCell_cand 0x7FF71E628130). +8 gfx_map is null: no streamer code reads it. +56/+60 (district
	// ids the streamer compares against) keep the retail values.
	void EncodeEmptyDistricts(XWriter& w, const std::array<std::uint8_t, 96>& retailRoot);

	// A bgcache (0x6D, Load_BgcacheAsset 0x7FF71E7CA8C0): a level's precache list, a name and entries of
	// {u8 table, u64 name}. At level load BG_Cache_RegisterAll_cand 0x7FF726213850 adds the entries of every
	// loaded bgcache (up to 32, in zones whose flags pass its masks) to the engine's per-table name lists
	// (BG_Cache_Register_cand 0x7FF726213DB0). Those lists are the only place a map entity's model is found:
	// G_SetModel_cand 0x7FF723DA8F70 asks BG_Cache_FindIndex_cand (table 2) and leaves an unlisted model unset,
	// so its entity spawns and never draws. Die Maschine's is named after the map and lists 27551 entries
	// (12162 models); a new model mapkit writes needs a bgcache of its own.
	struct BgCacheEntry {
		std::uint8_t table = 0;
		std::uint64_t name = 0;
	};
	// The tables (off_7FF72AADB4F0, 64-B records: +0 name, +8 asset type, +12 capacity): 2 "model" (xmodel,
	// 32768 names).
	constexpr std::uint8_t kBgCacheModel = 2;
	void EncodeBgCache(XWriter& w, std::uint64_t name, std::span<const BgCacheEntry> entries);

	// A keyvaluepairs asset (0x4B, Load_KeyvaluepairsAsset 0x7FF71E7DD6A0; layout at ReadKeyValuePairs): a name and
	// {key hash, string} pairs. Its link callback registers it (BuildKv_Register_cand 0x7FF729681490), and once a zone has
	// loaded, DB_OpenZonePackages_cand 0x7FF72928AB20 opens every streamed-data package that the one named after the zone
	// lists under "xpak_read". Packages are per mode, not per map (zone\zm.xpak and its .xsub files, zm_postship, zm_sink,
	// core...): a level zone lists the ones its streamed data lives in.
	std::uint32_t KeyValueKeyHash(std::string_view key); // BuildKv_KeyHash_cand 0x7FF7295FC9B0
	constexpr std::uint32_t kKeyXPakRead = 0x1C4D6;      // KeyValueKeyHash("xpak_read")
	void EncodeKeyValuePairs(XWriter& w, std::uint64_t name, std::span<const std::pair<std::uint32_t, std::string>> pairs);

	// A by-name reference to an asset of another zone, written inline where a nested asset goes: the
	// root struct of `rootSize` bytes holding only the name with its top bit set, where the type keeps its
	// name (XAssetNameOffset; what retail zones hold for assets that live elsewhere; DB_LinkXAsset links it by
	// name). Every other byte is zero: for a klf, +0 is a string pointer, and -1 there reads a string inline.
	void EncodeAssetReference(XWriter& w, std::uint64_t type, std::uint64_t name, std::size_t rootSize,
		std::uint64_t alignment = 8);
}
