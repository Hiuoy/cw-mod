#pragma once
#include "xwriter.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// A static model mapkit writes itself: one LOD, its mesh resident in the zone (block 12, no .xsub), one
// surface per material. Written as three assets, each the mirror of its loader in model_assets.cpp: its
// skeleton (xskeleton 0x08), the LOD (xmodelmesh 0x09) and the model (xmodel 0x06), which links the skeleton,
// the LOD, a collision and its materials by name (XWriter::AssetEntry), so the zone must carry an entry for
// each (the skeleton and LOD themselves, and by-name references to retail ones for the rest) BEFORE the model.
//
// Layouts, read 2026-09-25 off zm_silver's resident static models (the 4-vertex quad
// p8_fxanim_zm_zod_wormhole_mod and the 12-vertex p8_zm_zod_pap_plinth_sequence_air):
//   xmodel (232 B)      +0 name, +8 skeleton, +16 collision, +32 LOD 0 (+40.. LODs 1-7), +96 material tables
//                       (32 B each: u16 surface count, +8 material handles, +24 f32 per surface = UV area per
//                       world area), +104 a 96-B block, +112 u16 LOD count, +116 f32 x8 LOD distances, +144..
//                       +180 constants, +184 mins, +196 maxs, +208 flags, +216 radius (from the origin), +220 0x20
//   xmodelmesh (64 B)   +0 name, +8 surfaces (48 B each), +16 mesh info (464 B), +24 a 96-B block, +32 stream key
//                       (0: resident), +56 f32, +60 u16 surface count
//   mesh info (464 B)   +0 flags 0x08 (resident, 16-B vertices, no weights), +4 vertices, +12 indices, +32 buffer
//                       (block 12, 256-aligned), +40 its size, +44 positions, +48 vertices, +52 indices (0);
//                       12 bytes of 0xFF at +264, +304 and +344
//   surface (48 B)      +2 u16 vertices, +4 u16 triangles, +8 first vertex, +12 first index, +16 the LOD's mesh
//                       info, +32 the 96-B block
//   buffer              u16 indices (relative to the surface's first vertex; each padded to 16), f32 x3 positions
//                       (padded to 16), then per vertex 16 B: u32 color, f16 u, f16 v, u32 normal, u32 tangent. A
//                       normal/tangent packs x, y, z as 10-bit unorm ((n + 1) / 2 * 1023) and a 2-bit w: the
//                       tangent's is the bitangent sign (0 = -1, 3 = +1; bitangent = sign * cross(normal, tangent)).
//                       A triangle is clockwise seen from its front.
//   96-B block          u32 0x80000000, then zeros (every model, LOD and surface of both points at one copy)
//   xskeleton (88 B)    +0 name, +8 u32 bone names (script strings), +16 8 B per bone (u32 the same name, u32 0;
//                       sorted by the loader), +24/+32/+40 parents, rotations, translations (non-root bones
//                       only), +48 u8 part classification, +56 base matrices (32 B: quat, translation, f32 2.0),
//                       +64 bone info (40 B: f32 mins[3], maxs[3], center[3], radius squared), +72 u16 cosmetic
//                       bones, +74 bones, +76 roots. Read off Die Maschine's 1170 one-bone skeletons (2026-09-26):
//                       1169 share one identity matrix and 1164 one part class 0; each has its own bone name and
//                       bone info, the bounds of its own mesh. So the bone info is per model, and a borrowed
//                       skeleton gives the model the other model's bounds: the PaP plinth's (a flat 21 x 22 unit
//                       box, radius 15) made mapkit's walls vanish whenever that box at their origin left the view.
//
// Field table: who reads each field at RUNTIME, beyond the loader (docs/mapkit-plan.md, P0; IDB 2026-09-26).
//   xmodel +32..+88     LOD meshes: XModel_GetDrawableLod_cand 0x7FF7293EF5B0 (a resident LOD is taken as is),
//                       XModel_PrefetchLods_cand 0x7FF726869680 (streamed LODs only)
//          +112 u16     LOD count: XModel_GetLodCount, XModel_LodForDistance_cand, XModel_GetMaxLod_cand
//          +114 u8      distance class: index into the view's +64 scale table (XModel_LodForDistance_cand 0x7FF7293EF940)
//          +116 f32[]   LOD switch distances: read for LODs 1.. only; a one-LOD model never reads them
//          +152 f32     cull factor, times +168/+172/+176 f32 (by the view's class): culled when factor * distance >
//                       view+52 * radius (XModel_LodForDistance_cand returns -1). mapkit writes the retail constants
//          +180..+182   i8 LOD clamps and bias (XModel_SelectLod_cand 0x7FF7293F03F0, XModel_GetMaxLod_cand); 0 = none
//          +208 u64     flags: 0x40 ignores the LOD bias, 0x80 caps it at 1, 0x100 alternate transform (scaled);
//                       0x08 (not traced) is never on a model Die Maschine places by entity (StaticModel::flags,
//                       default 0). It was not why mapkit's models were invisible: no bgcache listed them
//                       (world_writer.hpp EncodeBgCache)
//   xmodelmesh +16      mesh info, flags bit 0 = streamed (XModelMesh_IsStreamed 0x7FF7293F0860): the link callback,
//                       the stream code and XModelMesh_IsReadyToDraw_cand only look at streamed meshes. A resident
//                       mesh is always ready and gets no link-time registration.
//   Not traced yet: the draw submission (surfaces, buffer views), the material/techset side, the 96-B block, mesh
//   info +256..+375 (three 40-B records holding 12 bytes of 0xFF each in the file). The client's live dump
//   (client/game/mapkit_live.hpp) compares them with a retail model in memory.
namespace MapKit::Zone {
	struct StaticVertex {
		std::array<float, 3> position{};
		std::array<float, 3> normal{};
		std::array<float, 3> tangent{};
		float bitangentSign = 1.0f;
		std::array<float, 2> uv{};
	};

	struct StaticSurface {
		std::uint64_t material = 0; // name hash
		std::vector<StaticVertex> vertices; // at most 65535
		std::vector<std::array<std::uint16_t, 3>> triangles;
	};

	struct StaticModel {
		std::uint64_t name = 0;      // the xmodel's name hash
		std::uint64_t meshName = 0;  // its LOD 0's
		std::uint64_t skeleton = 0;  // its own one-bone skeleton's name (EncodeStaticSkeleton)
		std::string boneName = "tag_origin"; // that bone's name, a script string the skeleton asset must declare
		std::uint64_t collision = 0; // a retail xcollision, by name (an empty one: collision is the clip map's)
		std::vector<StaticSurface> surfaces;
		// xmodel +208 flags. Every resident model Die Maschine places by entity stores 0 or 16. 8 (bit 3) is on 134
		// others, the PaP plinth among them (mapkit copied it from there until 2026-09-26). Not the draw problem:
		// the retail door rewritten with 0 did not draw either while no bgcache listed it.
		std::uint64_t flags = 0;
		// Header bytes retail varies per model, not understood yet: xmodel +221 and xmodelmesh +62 (both 2 on most
		// of Die Maschine's models, 0 on some it places by entity).
		std::uint8_t modelByte221 = 0;
		std::uint16_t meshWord62 = 0;
	};

	// Adds `surface`'s vertices and triangles to `model`, starting a new surface when the material changes or
	// the current one would pass 65535 vertices.
	void AppendSurface(StaticModel& model, const StaticSurface& surface);

	// The model's own skeleton (Load_XskeletonAsset 0x7FF71E7F9180): one root bone, `boneName`, at the origin,
	// its bone info the model's bounds. `boneName` must be in the zone's script string table.
	void EncodeStaticSkeleton(XWriter& w, const StaticModel& model);
	// The xmodelmesh asset (Load_XmodelmeshAsset(0, &header), header inline).
	void EncodeStaticModelMesh(XWriter& w, const StaticModel& model);
	// The xmodel asset. Every link goes through w.AssetEntry: the LOD (meshName), skeleton, collision and each
	// surface's material must be entries of the zone written before it.
	void EncodeStaticModel(XWriter& w, const StaticModel& model);

	// Bounds and radius as the model stores them.
	struct StaticModelBounds {
		std::array<float, 3> mins{};
		std::array<float, 3> maxs{};
		float radius = 0;
	};
	StaticModelBounds ModelBounds(const StaticModel& model);
}
