#pragma once
#include "havok_tagfile.hpp"
#include "world_writer.hpp"
#include "xwriter.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// A map's navmesh (P5, docs/mapkit-plan.md): the surfaces its AI walk on, as the engine loads them.
//
// The navmesh asset (0x75, Load_NavMeshData 0x7FF71E7E1EE0) is found by the hash of maps/zm/<map>.d3dbsp
// (AI_LoadNavmeshForMap 0x7FF723B9D5E0: none is a drop). Its root (104 B) holds the stream key of the shared
// Havok tagfile (+8), a cell count (+32), the cells (+40, 64 B each: +0 section uid, +12..+35 the cell's AABB in
// inches, +36 its face count, +40 the stream key of its tagfile), the level's bounds (+48, inches) and the debug
// data's stream key (+72, never loaded by this build). A stream key whose flags (+55) have bit 2 carries its data
// inline in the zone (block 4); the engine then reads it in place (StreamKey_IsReady_cand 0x7FF728FE5F30).
//
// Die Maschine's navmesh files are the templates: mapkit keeps their types and settings and replaces what describes
// Die Maschine's ground.
namespace MapKit::Zone {
	// A walkable surface as convex polygons, in the game's units and axes (inches; x forward, y left, z up). Polygons
	// that meet share the two vertices of their common edge.
	struct NavPolygons {
		std::vector<std::array<float, 3>> vertices;
		std::vector<std::vector<std::uint32_t>> polygons;
	};

	// What a navmesh is made for, in inches and degrees. The defaults are Die Maschine's generation settings (the
	// shared file's): a character 72 tall (1.8288 m), a step of 18 (0.4572 m), slopes up to 46 degrees. Its navmesh
	// is not shrunk away from walls (erosion radius 0): Havok keeps a character's radius off the edges itself.
	struct NavSettings {
		float cellSize = 4.0f;    // the voxel grid, across
		float cellHeight = 2.0f;  // and up
		float agentHeight = 72.0f;
		float agentClimb = 18.0f;
		float agentRadius = 0.0f;
		float maxSlope = 46.0f;
		float minRegion = 8.0f;   // a patch smaller than this many cells across is dropped
		float mergeRegion = 20.0f;
		float maxEdge = 480.0f;   // a boundary edge longer than this is split
	};

	struct NavGenerateReport {
		std::size_t triangles = 0;    // the hulls' faces fed to the voxelizer
		std::size_t polygons = 0;     // walkable polygons found
		std::size_t kept = 0;         // connected to a seed
		std::size_t seedsOnMesh = 0;
		std::array<float, 3> mins{}, maxs{};
	};

	// The walkable polygons over the solid hulls (what the game collides with, in world space). Only polygons connected
	// to a seed (where players and zombies start) are kept: islands no one can reach, wall tops say, are dropped. With
	// no seed on the mesh everything is kept. False (with a reason) when nothing is walkable.
	bool GenerateNavPolygons(std::span<const ConvexHull> hulls, std::span<const std::array<float, 3>> seeds,
		const NavSettings& settings, NavPolygons& out, NavGenerateReport& report, std::string& error);

	struct NavCellReport {
		std::size_t faces = 0;
		std::size_t edges = 0;
		std::size_t openEdges = 0;    // on the boundary
		std::size_t vertices = 0;
		std::size_t clusters = 0;
		std::size_t clusterEdges = 0;
		std::array<float, 3> mins{}, maxs{}; // the cell's AABB, inches
	};

	// Die Maschine's cell tagfile (Runtime_NavMeshCell) holding `mesh` instead of its own ground:
	//   hkaiNavMesh  faces counter-clockwise seen from above; edges with their opposite edge and face (-1 on the
	//                boundary) and flags 4 as Die Maschine's; vertices in meters; faceData 0x1E0001 on every face
	//                (walkable, and the numbered flags 18..21 Die Maschine's faces carry: every AI size may use it;
	//                Nav_FaceMaterialFlags_cand 0x7FF726A83400); edgeData 0; no face search tree (the engine builds
	//                one, hkaiNavMesh_GetOrBuildFaceIterator_cand 0x7FF729B56F50).
	//   cluster graph  faces grouped about ten a cluster, each cluster a node at its faces' centre, edges between
	//                clusters that touch costing the distance (hkHalf16: a float's top 16 bits).
	//   tactical graph  no points; one empty {-1, -1} tfaces entry a face (Tac_ClosestPointNear_cand 0x7FF725FC9910
	//                indexes it by face unchecked).
	bool BuildNavMeshCell(const HkTagfile& retail, const NavPolygons& mesh, HkTagfile& out, NavCellReport& report,
		std::string& error);

	// Checks a cell tagfile the way the engine walks it: each face's edges chain round it counter-clockwise seen from
	// above, opposite edges pair up both ways, every index is in range, faceData/edgeData/tfaces have one entry per
	// face or edge, and the cluster graph's links go both ways. False with the first problem in `error`.
	bool CheckNavMeshCell(const HkTagfile& cell, std::string& error);

	// Die Maschine's shared tagfile (Runtime_NavMeshAssetShared) with its tactical points emptied: the path nodes'
	// and regions' point keys would index the cell's tactical points, which mapkit's cell has none of. Its generation
	// and traversal settings stay: the engine generates moving platforms' navmeshes with them
	// (Nav_BuildMovingPlatformNavMesh_cand 0x7FF724F1AFE0).
	bool BuildNavMeshShared(const HkTagfile& retail, HkTagfile& out, std::string& error);

	struct NavMeshAsset {
		std::uint64_t name = 0;                  // HashName of maps/zm/<map>.d3dbsp
		std::uint64_t sharedKey = 0;             // the stream keys' names (unique: they are linked as assets)
		std::uint64_t cellKey = 0;
		std::vector<std::uint8_t> shared;        // NavPayload of each tagfile
		std::vector<std::uint8_t> cell;
		std::array<float, 3> mins{}, maxs{};     // the cell's AABB, inches
		std::uint32_t faceCount = 0;
	};

	// The navmesh asset with one cell and both stream keys inline (flags 3, as Die Maschine's inline keys), and no
	// debug data.
	void EncodeNavMesh(XWriter& w, const NavMeshAsset& navmesh);

	// A navmesh as a Wavefront OBJ, in the game's units and axes (for looking at it in a 3D tool).
	std::string NavPolygonsObj(const NavPolygons& mesh);
}
