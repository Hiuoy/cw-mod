// GenerateNavPolygons (navmesh.hpp): Recast (vendor/recastnavigation, zlib license) over a map's collision hulls.
#include "navmesh.hpp"

#include <Recast.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <deque>
#include <format>
#include <memory>

namespace MapKit::Zone {
	namespace {
		using Vec = std::array<float, 3>;

		// Game axes (x forward, y left, z up) to Recast's (y up): a turn, not a mirror, so faces keep their winding.
		Vec ToRecast(const Vec& g) { return { g[0], g[2], -g[1] }; }
		Vec FromRecast(const Vec& r) { return { r[0], -r[2], r[1] }; }

		float Dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
		Vec Cross(const Vec& a, const Vec& b) {
			return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
		}
		Vec Normalized(Vec v) {
			const float length = std::sqrt(Dot(v, v));
			if (length > 1e-12f) {
				for (float& x : v) {
					x /= length;
				}
			}
			return v;
		}

		// A hull's faces as triangles, counter-clockwise seen from outside: each bounding plane (the AABB's six and the
		// extra ones, dot(normal, p) <= distance inside) with the corners on it, in turn around the plane's normal.
		void HullTriangles(const ConvexHull& hull, std::vector<float>& verts, std::vector<int>& tris) {
			std::vector<std::array<float, 4>> planes = hull.planes;
			for (int axis = 0; axis < 3; ++axis) {
				std::array<float, 4> positive{}, negative{};
				positive[axis] = 1.0f;
				positive[3] = hull.maxs[axis];
				negative[axis] = -1.0f;
				negative[3] = -hull.mins[axis];
				planes.push_back(positive);
				planes.push_back(negative);
			}
			for (const auto& plane : planes) {
				const Vec normal = { plane[0], plane[1], plane[2] };
				std::vector<Vec> on;
				for (const Vec& corner : hull.vertices) {
					if (std::abs(Dot(normal, corner) - plane[3]) <= 0.05f
						&& std::ranges::none_of(on, [&](const Vec& p) {
							return std::abs(p[0] - corner[0]) + std::abs(p[1] - corner[1]) + std::abs(p[2] - corner[2]) < 0.01f;
						})) {
						on.push_back(corner);
					}
				}
				if (on.size() < 3) {
					continue;
				}
				Vec centre{};
				for (const Vec& p : on) {
					for (int k = 0; k < 3; ++k) {
						centre[k] += p[k] / on.size();
					}
				}
				// u, v and the normal make a right-handed frame: increasing angle turns counter-clockwise about the normal.
				const Vec helper = std::abs(normal[2]) < 0.9f ? Vec{ 0, 0, 1 } : Vec{ 1, 0, 0 };
				const Vec u = Normalized(Cross(helper, normal));
				const Vec v = Cross(normal, u);
				std::ranges::sort(on, {}, [&](const Vec& p) {
					const Vec d = { p[0] - centre[0], p[1] - centre[1], p[2] - centre[2] };
					return std::atan2(Dot(d, v), Dot(d, u));
				});
				const int first = static_cast<int>(verts.size() / 3);
				for (const Vec& p : on) {
					const Vec r = ToRecast(p);
					verts.insert(verts.end(), r.begin(), r.end());
				}
				for (int i = 1; i + 1 < static_cast<int>(on.size()); ++i) {
					tris.insert(tris.end(), { first, first + i, first + i + 1 });
				}
			}
		}

		struct FreeHeightfield { void operator()(rcHeightfield* p) const { rcFreeHeightField(p); } };
		struct FreeCompact { void operator()(rcCompactHeightfield* p) const { rcFreeCompactHeightfield(p); } };
		struct FreeContours { void operator()(rcContourSet* p) const { rcFreeContourSet(p); } };
		struct FreePolyMesh { void operator()(rcPolyMesh* p) const { rcFreePolyMesh(p); } };
	}

	bool GenerateNavPolygons(std::span<const ConvexHull> hulls, std::span<const std::array<float, 3>> seeds,
		const NavSettings& settings, NavPolygons& out, NavGenerateReport& report, std::string& error) {
		out = {};
		report = {};
		std::vector<float> verts;
		std::vector<int> tris;      // the hulls a character may stand on
		std::vector<int> obstacles; // the ones that only block (ConvexHull::walkable off)
		for (const ConvexHull& hull : hulls) {
			HullTriangles(hull, verts, hull.walkable ? tris : obstacles);
		}
		report.triangles = (tris.size() + obstacles.size()) / 3;
		if (tris.empty()) {
			error = "no collision to walk on";
			return false;
		}

		rcConfig cfg{};
		cfg.cs = settings.cellSize;
		cfg.ch = settings.cellHeight;
		rcCalcBounds(verts.data(), static_cast<int>(verts.size() / 3), cfg.bmin, cfg.bmax);
		for (int k = 0; k < 3; ++k) {
			cfg.bmin[k] -= 2 * cfg.cs;
			cfg.bmax[k] += 2 * cfg.cs;
		}
		cfg.bmax[1] += settings.agentHeight; // room above the highest floor
		// A map far wider than the voxel budget gets a coarser grid (at most about 16 million columns).
		const float across = (cfg.bmax[0] - cfg.bmin[0]) * (cfg.bmax[2] - cfg.bmin[2]);
		cfg.cs = std::max(cfg.cs, std::sqrt(across / 16e6f));
		rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
		cfg.walkableSlopeAngle = settings.maxSlope;
		cfg.walkableHeight = static_cast<int>(std::ceil(settings.agentHeight / cfg.ch));
		cfg.walkableClimb = static_cast<int>(std::floor(settings.agentClimb / cfg.ch));
		cfg.walkableRadius = static_cast<int>(std::ceil(settings.agentRadius / cfg.cs));
		cfg.maxEdgeLen = static_cast<int>(settings.maxEdge / cfg.cs);
		cfg.maxSimplificationError = 1.3f;
		cfg.minRegionArea = static_cast<int>(settings.minRegion * settings.minRegion);
		cfg.mergeRegionArea = static_cast<int>(settings.mergeRegion * settings.mergeRegion);
		cfg.maxVertsPerPoly = 6;

		rcContext context(false);
		const int vertexCount = static_cast<int>(verts.size() / 3);
		const int triangleCount = static_cast<int>(tris.size() / 3);
		std::unique_ptr<rcHeightfield, FreeHeightfield> solid(rcAllocHeightfield());
		if (!solid || !rcCreateHeightfield(&context, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
			error = std::format("could not make a {} x {} voxel grid", cfg.width, cfg.height);
			return false;
		}
		std::vector<unsigned char> areas(triangleCount, RC_NULL_AREA);
		rcMarkWalkableTriangles(&context, cfg.walkableSlopeAngle, verts.data(), vertexCount, tris.data(), triangleCount, areas.data());
		if (!rcRasterizeTriangles(&context, verts.data(), vertexCount, tris.data(), areas.data(), triangleCount, *solid,
			cfg.walkableClimb)) {
			error = "voxelizing the collision failed";
			return false;
		}
		rcFilterLowHangingWalkableObstacles(&context, cfg.walkableClimb, *solid);
		// The obstacles go in after that filter and merge with a threshold of 0: both would turn the top of one within a
		// step of the floor into floor (a Mystery Box is 19 units high, a step 18, and the grid rounds either way).
		if (!obstacles.empty()) {
			const int obstacleCount = static_cast<int>(obstacles.size() / 3);
			const std::vector<unsigned char> blocked(obstacleCount, RC_NULL_AREA);
			if (!rcRasterizeTriangles(&context, verts.data(), vertexCount, obstacles.data(), blocked.data(), obstacleCount, *solid,
				0)) {
				error = "voxelizing the obstacles failed";
				return false;
			}
		}
		rcFilterLedgeSpans(&context, cfg.walkableHeight, cfg.walkableClimb, *solid);
		rcFilterWalkableLowHeightSpans(&context, cfg.walkableHeight, *solid);

		std::unique_ptr<rcCompactHeightfield, FreeCompact> compact(rcAllocCompactHeightfield());
		if (!compact || !rcBuildCompactHeightfield(&context, cfg.walkableHeight, cfg.walkableClimb, *solid, *compact)) {
			error = "building the walkable spans failed";
			return false;
		}
		solid.reset();
		if (cfg.walkableRadius > 0 && !rcErodeWalkableArea(&context, cfg.walkableRadius, *compact)) {
			error = "shrinking the walkable area failed";
			return false;
		}
		if (!rcBuildDistanceField(&context, *compact)
			|| !rcBuildRegions(&context, *compact, 0, cfg.minRegionArea, cfg.mergeRegionArea)) {
			error = "splitting the walkable area into regions failed";
			return false;
		}
		std::unique_ptr<rcContourSet, FreeContours> contours(rcAllocContourSet());
		if (!contours || !rcBuildContours(&context, *compact, cfg.maxSimplificationError, cfg.maxEdgeLen, *contours)) {
			error = "tracing the regions' outlines failed";
			return false;
		}
		std::unique_ptr<rcPolyMesh, FreePolyMesh> polys(rcAllocPolyMesh());
		if (!polys || !rcBuildPolyMesh(&context, *contours, cfg.maxVertsPerPoly, *polys)) {
			error = "building the polygons failed";
			return false;
		}
		const int nvp = polys->nvp;
		report.polygons = static_cast<std::size_t>(polys->npolys);
		if (!polys->npolys) {
			error = "nothing is walkable";
			return false;
		}

		// Every polygon's corners, in Recast space.
		const auto corner = [&](int vertex) {
			const unsigned short* v = &polys->verts[vertex * 3];
			return Vec{ polys->bmin[0] + v[0] * polys->cs, polys->bmin[1] + v[1] * polys->ch, polys->bmin[2] + v[2] * polys->cs };
		};
		const auto polygonCorners = [&](int p) {
			std::vector<int> corners;
			for (int j = 0; j < nvp && polys->polys[p * nvp * 2 + j] != RC_MESH_NULL_IDX; ++j) {
				corners.push_back(polys->polys[p * nvp * 2 + j]);
			}
			return corners;
		};

		// Keep what a seed reaches: a seed stands on the polygon just under (or over) it.
		std::vector<bool> keep(polys->npolys, false);
		std::deque<int> open;
		for (const auto& seed : seeds) {
			const Vec s = ToRecast(seed);
			int best = -1;
			float bestGap = FLT_MAX;
			for (int p = 0; p < polys->npolys; ++p) {
				const std::vector<int> corners = polygonCorners(p);
				bool inside = true;
				int side = 0;
				float height = 0;
				for (std::size_t j = 0; j < corners.size() && inside; ++j) {
					const Vec a = corner(corners[j]);
					const Vec b = corner(corners[(j + 1) % corners.size()]);
					const float cross = (b[0] - a[0]) * (s[2] - a[2]) - (b[2] - a[2]) * (s[0] - a[0]);
					const int sign = cross > 1e-4f ? 1 : (cross < -1e-4f ? -1 : 0);
					if (sign && side && sign != side) {
						inside = false;
					}
					side = side ? side : sign;
					height += a[1] / corners.size();
				}
				// Not the top of the object the seed is (a power switch's hull fills its own origin).
				const float gap = std::abs(height - s[1]);
				if (inside && height - s[1] <= 24.0f && s[1] - height <= 48.0f && gap < bestGap) {
					best = p;
					bestGap = gap;
				}
			}
			if (best >= 0) {
				++report.seedsOnMesh;
				if (!keep[best]) {
					keep[best] = true;
					open.push_back(best);
				}
			}
		}
		if (open.empty()) {
			std::fill(keep.begin(), keep.end(), true);
		}
		while (!open.empty()) {
			const int p = open.front();
			open.pop_front();
			for (int j = 0; j < nvp; ++j) {
				const unsigned short next = polys->polys[p * nvp * 2 + nvp + j];
				if (next != RC_MESH_NULL_IDX && !(next & 0x8000) && next < polys->npolys && !keep[next]) {
					keep[next] = true;
					open.push_back(next);
				}
			}
		}

		std::vector<std::int32_t> remap(polys->nverts, -1);
		report.mins = { FLT_MAX, FLT_MAX, FLT_MAX };
		report.maxs = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (int p = 0; p < polys->npolys; ++p) {
			if (!keep[p]) {
				continue;
			}
			std::vector<std::uint32_t> polygon;
			for (const int v : polygonCorners(p)) {
				if (remap[v] < 0) {
					remap[v] = static_cast<std::int32_t>(out.vertices.size());
					const Vec g = FromRecast(corner(v));
					out.vertices.push_back(g);
					for (int k = 0; k < 3; ++k) {
						report.mins[k] = std::min(report.mins[k], g[k]);
						report.maxs[k] = std::max(report.maxs[k], g[k]);
					}
				}
				polygon.push_back(static_cast<std::uint32_t>(remap[v]));
			}
			out.polygons.push_back(std::move(polygon));
		}
		report.kept = out.polygons.size();
		return true;
	}
}
