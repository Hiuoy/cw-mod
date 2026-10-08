#include "navmesh.hpp"
#include "xstream.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <set>

namespace MapKit::Zone {
	namespace {
		constexpr float kMetersPerInch = 0.0254f;
		constexpr float kInchesPerMeter = 39.3700787f;
		// walkable, and the numbered flags 18, 19, 20 and 21 (bits 17..20) Die Maschine's faces carry.
		constexpr std::int32_t kFaceData = 0x1E0001;
		constexpr std::uint8_t kEdgeOriginal = 4; // every one of Die Maschine's plain edges
		constexpr std::size_t kClusterFaces = 10; // Die Maschine: 3919 faces in 395 clusters

		template <typename T>
		void Append(std::vector<std::uint8_t>& out, const T& value) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
			out.insert(out.end(), bytes, bytes + sizeof(T));
		}

		template <typename T>
		void Store(std::vector<std::uint8_t>& data, std::size_t offset, const T& value) {
			std::memcpy(data.data() + offset, &value, sizeof(T));
		}

		// hkHalf16: the top 16 bits of a float, rounded to nearest even.
		std::uint16_t ToHalf16(float value) {
			std::uint32_t bits = 0;
			std::memcpy(&bits, &value, 4);
			return static_cast<std::uint16_t>((bits + 0x7FFFu + ((bits >> 16) & 1u)) >> 16);
		}

		// The element type of the array at `offset` of `owner`, which must be named `name`; 0 when it is not.
		std::uint32_t ArrayType(const HkTagfile& file, std::uint32_t owner, std::uint32_t offset, std::string_view name) {
			const std::uint32_t target = file.Target(owner, offset);
			if (!target || file.types[file.items[target].type].name != name) {
				return 0;
			}
			return file.items[target].type;
		}

		bool ObjectIs(const HkTagfile& file, std::uint32_t item, std::string_view name) {
			return item && item < file.items.size() && file.types[file.items[item].type].name == name;
		}
	}

	bool BuildNavMeshCell(const HkTagfile& retail, const NavPolygons& mesh, HkTagfile& out, NavCellReport& report,
		std::string& error) {
		out = retail;
		report = {};
		const std::uint32_t navMesh = out.Target(1, 32);
		const std::uint32_t graph = out.Target(1, 40);
		const std::uint32_t tactical = out.Target(1, 48);
		if (!ObjectIs(out, 1, "Runtime_NavMeshCell") || !ObjectIs(out, navMesh, "hkaiNavMesh")
			|| !ObjectIs(out, graph, "hkaiDirectedGraphExplicitCost") || !ObjectIs(out, tactical, "TacticalGraphCell")) {
			error = "the template is not a navmesh cell laid out as Die Maschine's";
			return false;
		}
		const std::uint32_t faceType = ArrayType(out, navMesh, 24, "hkaiNavMesh::Face");
		const std::uint32_t edgeType = ArrayType(out, navMesh, 40, "hkaiNavMesh::Edge");
		const std::uint32_t vertexType = ArrayType(out, navMesh, 56, "hkVector4");
		const std::uint32_t faceDataType = ArrayType(out, navMesh, 88, "hkInt32");
		const std::uint32_t edgeDataType = ArrayType(out, navMesh, 104, "hkInt32");
		const std::uint32_t positionType = ArrayType(out, graph, 24, "hkVector4");
		const std::uint32_t nodeType = ArrayType(out, graph, 40, "hkaiDirectedGraphExplicitCost::Node");
		const std::uint32_t graphEdgeType = ArrayType(out, graph, 56, "hkaiDirectedGraphExplicitCost::Edge");
		const std::uint32_t tfaceType = ArrayType(out, tactical, 32, "tacface_t");
		if (!faceType || !edgeType || !vertexType || !faceDataType || !edgeDataType || !positionType || !nodeType
			|| !graphEdgeType || !tfaceType) {
			error = "the template cell lacks one of the arrays mapkit replaces";
			return false;
		}

		// Faces counter-clockwise seen from above, as Die Maschine's all are.
		std::vector<std::vector<std::uint32_t>> faces;
		for (const auto& polygon : mesh.polygons) {
			if (polygon.size() < 3 || std::ranges::any_of(polygon, [&](std::uint32_t v) { return v >= mesh.vertices.size(); })) {
				continue;
			}
			double area = 0;
			for (std::size_t k = 0; k < polygon.size(); ++k) {
				const auto& a = mesh.vertices[polygon[k]];
				const auto& b = mesh.vertices[polygon[(k + 1) % polygon.size()]];
				area += double(a[0]) * b[1] - double(b[0]) * a[1];
			}
			if (std::abs(area) < 1e-3) {
				continue;
			}
			std::vector<std::uint32_t> face = polygon;
			if (area < 0) {
				std::ranges::reverse(face);
			}
			faces.push_back(std::move(face));
		}
		if (faces.empty()) {
			error = "no walkable polygon";
			return false;
		}
		if (faces.size() > 0x3FFFFF) {
			error = std::format("{} faces: more than a packed key holds", faces.size());
			return false;
		}

		// Edges in face order; the opposite of a->b is the edge b->a of the face next door.
		struct Edge {
			std::uint32_t a = 0;
			std::uint32_t b = 0;
			std::uint32_t face = 0;
			std::uint32_t opposite = ~0u;
		};
		std::vector<Edge> edges;
		std::vector<std::uint32_t> firstEdge;
		std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> byEnds;
		for (std::uint32_t f = 0; f < faces.size(); ++f) {
			firstEdge.push_back(static_cast<std::uint32_t>(edges.size()));
			for (std::size_t k = 0; k < faces[f].size(); ++k) {
				Edge edge{ faces[f][k], faces[f][(k + 1) % faces[f].size()], f };
				byEnds.try_emplace({ edge.a, edge.b }, static_cast<std::uint32_t>(edges.size()));
				edges.push_back(edge);
			}
		}
		std::vector<std::vector<std::uint32_t>> neighbours(faces.size());
		for (Edge& edge : edges) {
			const auto other = byEnds.find({ edge.b, edge.a });
			if (other != byEnds.end() && edges[other->second].face != edge.face) {
				edge.opposite = other->second;
				neighbours[edge.face].push_back(edges[other->second].face);
			}
			else {
				++report.openEdges;
			}
		}

		// Clusters: about kClusterFaces touching faces each, grown breadth first.
		std::vector<std::int32_t> cluster(faces.size(), -1);
		std::vector<std::vector<std::uint32_t>> members;
		for (std::uint32_t seed = 0; seed < faces.size(); ++seed) {
			if (cluster[seed] >= 0) {
				continue;
			}
			const auto id = static_cast<std::int32_t>(members.size());
			members.emplace_back();
			std::deque<std::uint32_t> open = { seed };
			cluster[seed] = id;
			while (!open.empty() && members.back().size() < kClusterFaces) {
				const std::uint32_t face = open.front();
				open.pop_front();
				members.back().push_back(face);
				for (const std::uint32_t next : neighbours[face]) {
					if (cluster[next] < 0 && members.back().size() + open.size() < kClusterFaces) {
						cluster[next] = id;
						open.push_back(next);
					}
				}
			}
			// Faces taken but not reached before the cluster filled up stay in it.
			for (const std::uint32_t face : open) {
				members.back().push_back(face);
			}
		}
		if (members.size() > 0x7FFF) {
			error = std::format("{} clusters: more than a face's cluster index holds", members.size());
			return false;
		}
		std::vector<std::array<float, 3>> centres(members.size());
		for (std::size_t c = 0; c < members.size(); ++c) {
			std::array<double, 3> sum{};
			for (const std::uint32_t face : members[c]) {
				for (const std::uint32_t v : faces[face]) {
					for (int k = 0; k < 3; ++k) {
						sum[k] += mesh.vertices[v][k] / faces[face].size();
					}
				}
			}
			for (int k = 0; k < 3; ++k) {
				centres[c][k] = static_cast<float>(sum[k] / members[c].size()) * kMetersPerInch;
			}
		}
		std::vector<std::set<std::uint32_t>> links(members.size());
		for (std::uint32_t f = 0; f < faces.size(); ++f) {
			for (const std::uint32_t next : neighbours[f]) {
				if (cluster[next] != cluster[f]) {
					links[cluster[f]].insert(static_cast<std::uint32_t>(cluster[next]));
				}
			}
		}

		// The arrays' bytes.
		std::vector<std::uint8_t> faceBytes, edgeBytes, vertexBytes, faceData, edgeData, positions, nodes, graphEdges, tfaces;
		for (std::uint32_t f = 0; f < faces.size(); ++f) {
			Append(faceBytes, static_cast<std::int32_t>(firstEdge[f]));
			Append(faceBytes, std::int32_t(-1));  // startUserEdgeIndex
			Append(faceBytes, static_cast<std::int16_t>(faces[f].size()));
			Append(faceBytes, std::int16_t(0));   // numUserEdges
			Append(faceBytes, static_cast<std::int16_t>(cluster[f]));
			Append(faceBytes, std::uint16_t(0));  // padding
			Append(faceData, kFaceData);
			Append(tfaces, std::int32_t(-1));
			Append(tfaces, std::int32_t(-1));
		}
		for (const Edge& edge : edges) {
			Append(edgeBytes, static_cast<std::int32_t>(edge.a));
			Append(edgeBytes, static_cast<std::int32_t>(edge.b));
			Append(edgeBytes, edge.opposite);                                          // section 0: the index alone
			Append(edgeBytes, edge.opposite == ~0u ? ~0u : edges[edge.opposite].face);
			Append(edgeBytes, kEdgeOriginal);
			Append(edgeBytes, std::uint8_t(0));
			Append(edgeBytes, std::uint16_t(0));                                      // userEdgeCost
			Append(edgeData, std::int32_t(0));
		}
		std::array<float, 3> lo = { FLT_MAX, FLT_MAX, FLT_MAX };
		std::array<float, 3> hi = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const auto& vertex : mesh.vertices) {
			for (int k = 0; k < 3; ++k) {
				const float meters = vertex[k] * kMetersPerInch;
				Append(vertexBytes, meters);
				lo[k] = std::min(lo[k], meters);
				hi[k] = std::max(hi[k], meters);
			}
			Append(vertexBytes, 1.0f);
		}
		std::uint32_t graphEdgeCount = 0;
		for (std::size_t c = 0; c < members.size(); ++c) {
			for (int k = 0; k < 3; ++k) {
				Append(positions, centres[c][k]);
			}
			Append(positions, 1.0f);
			Append(nodes, static_cast<std::int32_t>(graphEdgeCount));
			Append(nodes, static_cast<std::int32_t>(links[c].size()));
			for (const std::uint32_t target : links[c]) {
				const float dx = centres[target][0] - centres[c][0];
				const float dy = centres[target][1] - centres[c][1];
				const float dz = centres[target][2] - centres[c][2];
				Append(graphEdges, ToHalf16(std::sqrt(dx * dx + dy * dy + dz * dz)));
				Append(graphEdges, std::uint16_t(0)); // flags
				Append(graphEdges, target);           // section 0: the index alone
				++graphEdgeCount;
			}
		}

		const auto count = [](std::size_t n) { return static_cast<std::uint32_t>(n); };
		bool ok = out.SetArray(navMesh, 24, faceType, std::move(faceBytes), count(faces.size()))
			&& out.SetArray(navMesh, 40, edgeType, std::move(edgeBytes), count(edges.size()))
			&& out.SetArray(navMesh, 56, vertexType, std::move(vertexBytes), count(mesh.vertices.size()))
			&& out.SetArray(navMesh, 88, faceDataType, std::move(faceData), count(faces.size()))
			&& out.SetArray(navMesh, 104, edgeDataType, std::move(edgeData), count(edges.size()))
			&& out.SetArray(graph, 24, positionType, std::move(positions), count(members.size()))
			&& out.SetArray(graph, 40, nodeType, std::move(nodes), count(members.size()))
			&& out.SetArray(graph, 56, graphEdgeType, std::move(graphEdges), graphEdgeCount)
			&& out.SetArray(tactical, 32, tfaceType, std::move(tfaces), count(faces.size()));
		if (!ok) {
			error = "could not replace the template cell's arrays";
			return false;
		}
		out.SetNull(navMesh, 72);   // streamingSets
		out.SetNull(navMesh, 192);  // cachedFaceIterator: the engine builds the tree
		out.SetNull(navMesh, 200);  // clearanceCacheSeedingDataSet
		out.SetNull(graph, 72);     // nodeData
		out.SetNull(graph, 88);     // edgeData
		out.SetNull(graph, 112);    // streamingSets
		out.SetNull(tactical, 48);  // tpoints
		out.SetNull(tactical, 64);  // vpoints

		// The AABB (w 1, as Die Maschine's) with room around the ground: Die Maschine's reaches 70 m above its highest face.
		std::vector<std::uint8_t>& root = out.items[navMesh].data;
		const std::array<float, 3> margin = { 10.0f, 10.0f, 5.0f };
		for (int k = 0; k < 3; ++k) {
			lo[k] -= margin[k];
			hi[k] += k == 2 ? 30.0f : margin[k];
			report.mins[k] = lo[k] * kInchesPerMeter;
			report.maxs[k] = hi[k] * kInchesPerMeter;
		}
		for (int k = 0; k < 3; ++k) {
			Store(root, 144 + 4 * k, lo[k]);
			Store(root, 160 + 4 * k, hi[k]);
		}
		Store(root, 156, 1.0f);
		Store(root, 172, 1.0f);
		Store(root, 176, 0.0f); // erosionRadius
		Store(out.items[1].data, 24, std::uint32_t(0));    // sectionUid
		Store(out.items[tactical].data, 24, std::uint32_t(0));

		report.faces = faces.size();
		report.edges = edges.size();
		report.vertices = mesh.vertices.size();
		report.clusters = members.size();
		report.clusterEdges = graphEdgeCount;
		return true;
	}

	bool CheckNavMeshCell(const HkTagfile& cell, std::string& error) {
		const std::uint32_t navMesh = cell.Target(1, 32);
		const std::uint32_t graph = cell.Target(1, 40);
		const std::uint32_t tactical = cell.Target(1, 48);
		if (!ObjectIs(cell, 1, "Runtime_NavMeshCell") || !ObjectIs(cell, navMesh, "hkaiNavMesh")
			|| !ObjectIs(cell, graph, "hkaiDirectedGraphExplicitCost") || !ObjectIs(cell, tactical, "TacticalGraphCell")) {
			error = "not a navmesh cell";
			return false;
		}
		const auto array = [&](std::uint32_t owner, std::uint32_t offset) -> const HkItem* {
			const std::uint32_t target = cell.Target(owner, offset);
			return target ? &cell.items[target] : nullptr;
		};
		const HkItem* faces = array(navMesh, 24);
		const HkItem* edges = array(navMesh, 40);
		const HkItem* vertices = array(navMesh, 56);
		const HkItem* faceData = array(navMesh, 88);
		const HkItem* edgeData = array(navMesh, 104);
		const HkItem* positions = array(graph, 24);
		const HkItem* nodes = array(graph, 40);
		const HkItem* links = array(graph, 56);
		const HkItem* tfaces = array(tactical, 32);
		if (!faces || !edges || !vertices || !faceData || !edgeData || !positions || !nodes || !tfaces) {
			error = "an array is missing";
			return false;
		}
		const auto get = [](const HkItem& item, std::size_t at, auto zero) {
			decltype(zero) value{};
			std::memcpy(&value, item.data.data() + at, sizeof(value));
			return value;
		};
		const std::uint32_t faceCount = faces->count;
		const std::uint32_t edgeCount = edges->count;
		const std::uint32_t vertexCount = vertices->count;
		if (faceData->count != faceCount || edgeData->count != edgeCount || tfaces->count != faceCount
			|| positions->count != nodes->count) {
			error = std::format("counts disagree: {} faces, {} faceData, {} tfaces; {} edges, {} edgeData; {} clusters, {} positions",
				faceCount, faceData->count, tfaces->count, edgeCount, edgeData->count, nodes->count, positions->count);
			return false;
		}
		std::vector<std::uint32_t> owner(edgeCount, ~0u);
		for (std::uint32_t f = 0; f < faceCount; ++f) {
			const std::int32_t start = get(*faces, 16 * f, std::int32_t());
			const std::int16_t count = get(*faces, 16 * f + 8, std::int16_t());
			const std::int16_t cluster = get(*faces, 16 * f + 12, std::int16_t());
			if (start < 0 || count < 3 || std::uint32_t(start) + count > edgeCount || cluster < 0 || std::uint32_t(cluster) >= nodes->count) {
				error = std::format("face {}: edges {}+{} or cluster {} out of range", f, start, count, cluster);
				return false;
			}
			double area = 0;
			for (std::int32_t k = 0; k < count; ++k) {
				const std::uint32_t e = start + k;
				const std::uint32_t next = start + (k + 1) % count;
				owner[e] = f;
				const std::int32_t a = get(*edges, 20 * e, std::int32_t());
				const std::int32_t b = get(*edges, 20 * e + 4, std::int32_t());
				if (a < 0 || b < 0 || std::uint32_t(a) >= vertexCount || std::uint32_t(b) >= vertexCount
					|| get(*edges, 20 * next, std::int32_t()) != b) {
					error = std::format("face {}: edge {} does not chain round it", f, e);
					return false;
				}
				const float ax = get(*vertices, 16 * a, 0.0f), ay = get(*vertices, 16 * a + 4, 0.0f);
				const float bx = get(*vertices, 16 * b, 0.0f), by = get(*vertices, 16 * b + 4, 0.0f);
				area += double(ax) * by - double(bx) * ay;
			}
			if (!(area > 0)) {
				error = std::format("face {} is not counter-clockwise seen from above", f);
				return false;
			}
		}
		for (std::uint32_t e = 0; e < edgeCount; ++e) {
			const std::uint32_t opposite = get(*edges, 20 * e + 8, std::uint32_t());
			const std::uint32_t oppositeFace = get(*edges, 20 * e + 12, std::uint32_t());
			if (opposite == ~0u) {
				if (oppositeFace != ~0u) {
					error = std::format("edge {}: an opposite face without an opposite edge", e);
					return false;
				}
				continue;
			}
			if (opposite >= edgeCount || get(*edges, 20 * opposite + 8, std::uint32_t()) != e || owner[opposite] != oppositeFace
				|| get(*edges, 20 * opposite, std::int32_t()) != get(*edges, 20 * e + 4, std::int32_t())
				|| get(*edges, 20 * opposite + 4, std::int32_t()) != get(*edges, 20 * e, std::int32_t())) {
				error = std::format("edge {}: its opposite {} does not pair back", e, opposite);
				return false;
			}
		}
		std::set<std::pair<std::uint32_t, std::uint32_t>> graphLinks;
		const std::uint32_t linkCount = links ? links->count : 0;
		for (std::uint32_t n = 0; n < nodes->count; ++n) {
			const std::int32_t start = get(*nodes, 8 * n, std::int32_t());
			const std::int32_t count = get(*nodes, 8 * n + 4, std::int32_t());
			if (start < 0 || count < 0 || std::uint32_t(start) + count > linkCount) {
				error = std::format("cluster {}: links {}+{} out of range", n, start, count);
				return false;
			}
			for (std::int32_t k = 0; k < count; ++k) {
				const std::uint32_t target = get(*links, 8 * (start + k) + 4, std::uint32_t());
				if (target >= nodes->count) {
					error = std::format("cluster {}: a link to cluster {} of {}", n, target, nodes->count);
					return false;
				}
				graphLinks.emplace(n, target);
			}
		}
		for (const auto& [from, to] : graphLinks) {
			if (!graphLinks.contains({ to, from })) {
				error = std::format("clusters {} and {} are linked one way only", from, to);
				return false;
			}
		}
		return true;
	}

	bool BuildNavMeshShared(const HkTagfile& retail, HkTagfile& out, std::string& error) {
		out = retail;
		const std::uint32_t tactical = out.Target(1, 728);
		if (!ObjectIs(out, 1, "Runtime_NavMeshAssetShared") || !ObjectIs(out, tactical, "TacticalGraphShared")) {
			error = "the template is not a navmesh's shared file laid out as Die Maschine's";
			return false;
		}
		out.SetNull(tactical, 32); // pathNodePointKeys
		out.SetNull(tactical, 48); // pathNodeAttackPointKeys
		out.SetNull(tactical, 64); // regions
		out.SetNull(1, 736);       // dynamicUserEdgePairs: they name faces of the template's cells
		return true;
	}

	namespace {
		// Load_StreamkeyAsset(0, &field) with the field -1: the 56-B key in the temp block, linked as an asset, then its
		// data in block 4 (Load_StreamKeyData 0x7FF71E7ECF00: flags & 2, 256-aligned).
		void EncodeInlineStreamKey(XWriter& w, std::uint64_t name, std::span<const std::uint8_t> payload) {
			std::array<std::uint8_t, 56> key{};
			const std::span<std::uint8_t> k(key);
			PutAt(k, 0, name);
			PutAt(k, 32, kPtrInline);
			PutAt(k, 48, static_cast<std::uint32_t>(payload.size()));
			key[52] = 4;    // as Die Maschine's navmesh keys
			key[54] = 5;
			key[55] = 3;    // inline, as Die Maschine's inline keys
			w.Push(XBlockTemp);
			w.Alloc(8);
			w.Write(key.data(), key.size());
			w.Push(XBlockVirtual);
			w.Alloc(256);
			w.Write(payload.data(), payload.size());
			w.Pop();
			w.Pop();
		}
	}

	void EncodeNavMesh(XWriter& w, const NavMeshAsset& navmesh) {
		std::array<std::uint8_t, 104> root{};
		const std::span<std::uint8_t> r(root);
		PutAt(r, 0, navmesh.name);
		PutAt(r, 8, kPtrInline);
		PutAt(r, 32, std::int32_t(1));
		PutAt(r, 40, kPtrInline);
		for (int k = 0; k < 3; ++k) {
			PutAt(r, 48 + 4 * k, navmesh.mins[k]);
			PutAt(r, 60 + 4 * k, navmesh.maxs[k]);
		}
		PutAt(r, 72, kPtrNull);

		std::array<std::uint8_t, 64> cell{};
		const std::span<std::uint8_t> c(cell);
		for (int k = 0; k < 3; ++k) {
			PutAt(c, 12 + 4 * k, navmesh.mins[k]);
			PutAt(c, 24 + 4 * k, navmesh.maxs[k]);
		}
		PutAt(c, 36, navmesh.faceCount);
		PutAt(c, 40, kPtrInline);

		// Load_NavMeshData: the root in the temp block, the rest in block 4.
		w.Push(XBlockTemp);
		w.Alloc(8);
		w.Write(root.data(), root.size());
		w.Push(XBlockVirtual);
		EncodeInlineStreamKey(w, navmesh.sharedKey, navmesh.shared);
		w.Alloc(8);
		w.Write(cell.data(), cell.size());
		EncodeInlineStreamKey(w, navmesh.cellKey, navmesh.cell);
		w.Pop();
		w.Pop();
	}

	std::string NavPolygonsObj(const NavPolygons& mesh) {
		std::string out = "# mapkit navmesh: game units and axes (x forward, y left, z up)\n";
		for (const auto& v : mesh.vertices) {
			out += std::format("v {:.3f} {:.3f} {:.3f}\n", v[0], v[1], v[2]);
		}
		for (const auto& polygon : mesh.polygons) {
			out += "f";
			for (const std::uint32_t v : polygon) {
				out += std::format(" {}", v + 1);
			}
			out += "\n";
		}
		return out;
	}
}
