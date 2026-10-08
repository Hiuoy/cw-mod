// World asset encoders: each calls the XWriter primitives in the order its loader in world_assets.cpp
// reads, so zone_writer's walk reads back exactly what was written.
#include "world_writer.hpp"
#include "asset_loaders.hpp"
#include "model_assets.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>

namespace MapKit::Zone {
	namespace {
		std::uint64_t Present(bool present) {
			return present ? kPtrInline : kPtrNull;
		}

		template <typename T>
		void Set(std::span<std::uint8_t> data, std::size_t offset, const T& value) {
			PutAt(data, offset, value);
		}

		void WriteBytes(XWriter& w, const std::vector<std::uint8_t>& bytes) {
			w.Write(bytes.data(), bytes.size());
		}

		// ReadCollisionTree's mirror: the 184-B roots were written by the caller, the arrays follow here.
		void EncodeTreeArrays(XWriter& w, const CollisionTree& tree) {
			for (std::size_t i = 0; i < kCollisionTreeArrayCount; ++i) {
				const CollisionTreeArray& array = kCollisionTreeArrays[i];
				w.Push(array.pushed9 ? XBlockCollisionAlias : w.Block());
				if (!tree.arrays[i].empty() || Get<std::uint64_t>(tree.raw, array.field)) {
					w.Alloc(array.alignment);
					WriteBytes(w, tree.arrays[i]);
				}
				w.Pop();
			}
		}

		// The +160 terrain, as LoadClipMapBody reads it.
		void EncodeTerrain(XWriter& w, const ClipMap& map) {
			std::array<std::uint8_t, 72> head = map.worldHead;
			Set(std::span<std::uint8_t>(head), 60, static_cast<std::uint32_t>(map.worldEntries.size()));
			Set(std::span<std::uint8_t>(head), 64, Present(!map.worldEntries.empty()));
			w.Alloc(8);
			w.Write(head.data(), head.size());
			if (map.worldEntries.empty()) {
				return;
			}
			w.Alloc(8);
			for (const ClipMapWorldEntry& entry : map.worldEntries) {
				std::array<std::uint8_t, 176> raw = entry.raw;
				Set(std::span<std::uint8_t>(raw), 0, static_cast<std::uint32_t>(entry.records.size()));
				Set(std::span<std::uint8_t>(raw), 24, Present(!entry.records.empty()));
				Set(std::span<std::uint8_t>(raw), 152, Present(!entry.list.empty()));
				Set(std::span<std::uint8_t>(raw), 160, static_cast<std::uint32_t>(entry.list.size()));
				w.Write(raw.data(), raw.size());
			}
			for (const ClipMapWorldEntry& entry : map.worldEntries) {
				if (!entry.records.empty()) {
					w.Alloc(8);
					for (const ClipMapWorldRecord& record : entry.records) {
						std::array<std::uint8_t, 80> raw = record.raw;
						if (!record.indices.empty()) {
							Set(std::span<std::uint8_t>(raw), 32, static_cast<std::uint16_t>(record.indices.size() / 2));
						}
						Set(std::span<std::uint8_t>(raw), 40, Present(!record.indices.empty()));
						Set(std::span<std::uint8_t>(raw), 48, Present(!record.bits.empty()));
						Set(std::span<std::uint8_t>(raw), 56, Present(!record.blob.empty()));
						Set(std::span<std::uint8_t>(raw), 72, static_cast<std::uint32_t>(record.blob.size()));
						w.Write(raw.data(), raw.size());
					}
					for (const ClipMapWorldRecord& record : entry.records) {
						const std::pair<const std::vector<std::uint8_t>*, std::uint64_t> parts[] = {
							{ &record.indices, 2 }, { &record.bits, 1 }, { &record.blob, 1 },
						};
						for (const auto& [bytes, alignment] : parts) {
							w.Push(XBlockCollisionAlias);
							if (!bytes->empty()) {
								w.Alloc(alignment);
								WriteBytes(w, *bytes);
							}
							w.Pop();
						}
					}
				}
				if (!entry.list.empty()) {
					w.Alloc(8);
					for (const auto& item : entry.list) {
						w.Put(Present(!item.empty()));
					}
					for (const auto& item : entry.list) {
						if (!item.empty()) {
							w.Alloc(8);
							WriteBytes(w, item);
						}
					}
				}
			}
		}
	}

	void EncodeAssetReference(XWriter& w, std::uint64_t type, std::uint64_t name, std::size_t rootSize,
		std::uint64_t alignment) {
		std::vector<std::uint8_t> root(rootSize);
		PutAt(std::span<std::uint8_t>(root), XAssetNameOffset(type), name | (1ull << 63));
		w.Push(XBlockTemp);
		w.Alloc(alignment);
		WriteBytes(w, root);
		w.Pop();
	}

	void EncodeClipMap(XWriter& w, const ClipMap& map) {
		std::array<std::uint8_t, 472> root = map.root;
		const std::span<std::uint8_t> r(root);
		// Left out: cells, the 992-B structs, xcollisions, dynents (and the arrays sized by them), constraints.
		for (const std::size_t field : { 8, 24, 32, 40, 64, 72, 80, 96, 104, 112, 120, 128, 136, 144 }) {
			Set(r, field, kPtrNull);
		}
		for (const std::size_t field : { 424, 428, 432, 440, 444, 448 }) {
			Set(r, field, 0u);
		}
		// No dynents of the map's own (@90, and @460: CM_RegisterDynEntSettingsAssets 0x7FF7290210E0 walks +96 for
		// that many at level load, the D2 17:40 null read), but the debris slots after them (@88 counts both).
		Set(r, 88, static_cast<std::uint16_t>(kDynEntDebrisSlots));
		Set(r, 90, static_cast<std::uint16_t>(0));
		Set(r, 460, static_cast<std::uint16_t>(0));
		for (const std::size_t field : { 96, 104, 112, 128 }) {
			Set(r, field, kPtrInline);
		}
		Set(r, 16, Present(!map.cellIndex.empty()));
		Set(r, 48, Present(!map.trees.empty()));
		Set(r, 436, static_cast<std::uint32_t>(map.trees.size()));
		Set(r, 56, Present(map.hasTreeIndex));
		Set(r, 152, Present(Get<std::int32_t>(map.root, 452) > 0));
		Set(r, 160, Present(map.hasWorld));
		for (std::size_t i = 0; i < map.rootStrings.size(); ++i) {
			const std::string& text = map.rootStrings[i];
			Set(r, kClipMapStringsField + 4 * i, text.empty() ? 0u : w.ScriptString(text));
		}

		// Load_ClipMapAsset: the root in the temp block, then Load_ClipMap's body in block 4.
		w.Push(XBlockTemp);
		w.Alloc(8);
		w.Write(root.data(), root.size());
		w.Push(XBlockVirtual);

		if (!map.cellIndex.empty()) {
			w.Alloc(4);
			WriteBytes(w, map.cellIndex);
		}
		if (!map.trees.empty()) {
			w.Alloc(8);
			for (const CollisionTree& tree : map.trees) {
				std::array<std::uint8_t, 184> raw = tree.raw;
				for (std::size_t i = 0; i < kCollisionTreeArrayCount; ++i) {
					const std::size_t field = kCollisionTreeArrays[i].field;
					Set(std::span<std::uint8_t>(raw), field, Present(!tree.arrays[i].empty() || Get<std::uint64_t>(tree.raw, field)));
				}
				w.Write(raw.data(), raw.size());
			}
			for (const CollisionTree& tree : map.trees) {
				EncodeTreeArrays(w, tree);
			}
		}
		if (map.hasTreeIndex) {
			std::array<std::uint8_t, 32> index = map.treeIndex;
			const std::uint64_t alignments[3] = { 4, 4, 1 };
			bool present[3] = {};
			for (std::size_t i = 0; i < 3; ++i) {
				present[i] = !map.treeIndexArrays[i].empty() || Get<std::uint64_t>(map.treeIndex, 8 * i);
				Set(std::span<std::uint8_t>(index), 8 * i, Present(present[i]));
			}
			w.Alloc(8);
			w.Write(index.data(), index.size());
			for (std::size_t i = 0; i < 3; ++i) {
				if (present[i]) {
					w.Alloc(alignments[i]);
					WriteBytes(w, map.treeIndexArrays[i]);
				}
			}
		}
		// The debris slots (+96): empty records, each as retail stores them (no def at +0; +36/+40 hold what
		// DynEnt_Create_cand writes there too). The loader then looks at each record's def in the temp block.
		{
			std::vector<std::uint8_t> slots(96ull * kDynEntDebrisSlots);
			for (std::size_t i = 0; i < kDynEntDebrisSlots; ++i) {
				Set(std::span<std::uint8_t>(slots), 96 * i + 36, 0x04B004B0u);
				Set(std::span<std::uint8_t>(slots), 96 * i + 40, 0x04B004B0u);
			}
			w.Alloc(8);
			WriteBytes(w, slots);
			for (std::size_t i = 0; i < kDynEntDebrisSlots; ++i) {
				w.Push(XBlockTemp);
				w.Pop();
			}
		}
		// Their runtime arrays in block 2 (not stored: this only moves its position): +104 248 B, +112 280 B and
		// +128 44 B per slot. +120 and +136 are sized by the map's own dynents (@90), so they stay null.
		const std::pair<std::size_t, std::uint64_t> runtime[] = { { 248, 8 }, { 280, 8 }, { 0, 8 }, { 44, 4 }, { 0, 4 } };
		for (const auto& [stride, alignment] : runtime) {
			w.Push(XBlockRuntime);
			if (stride) {
				w.Alloc(alignment);
				WriteBytes(w, std::vector<std::uint8_t>(stride * kDynEntDebrisSlots));
			}
			w.Pop();
		}
		w.Push(XBlockRuntime);
		if (const std::int32_t count = Get<std::int32_t>(map.root, 452); count > 0) {
			w.Alloc(8);
			const std::vector<std::uint8_t> zeros(4512ull * count);
			WriteBytes(w, zeros); // block 2 is not stored: this only moves its position
		}
		w.Pop();
		if (map.hasWorld) {
			EncodeTerrain(w, map);
		}
		w.Pop();
		w.Pop();
	}

	bool ResolveClipMapStrings(ClipMap& map, std::span<const std::optional<std::string>> zoneStrings, std::string& error) {
		for (std::size_t i = 0; i < map.rootStrings.size(); ++i) {
			const auto index = Get<std::uint32_t>(map.root, kClipMapStringsField + 4 * i);
			if (index >= zoneStrings.size() && index != 0) {
				error = std::format("clip map script string {} is index {}, past the zone's {} strings", i, index,
					zoneStrings.size());
				return false;
			}
			map.rootStrings[i] = index < zoneStrings.size() ? zoneStrings[index].value_or(std::string()) : std::string();
		}
		return true;
	}

	std::vector<std::string> ClipMapScriptStrings(const ClipMap& map) {
		std::vector<std::string> strings;
		for (const std::string& text : map.rootStrings) {
			if (!text.empty() && std::ranges::find(strings, text) == strings.end()) {
				strings.push_back(text);
			}
		}
		return strings;
	}

	bool MakePlaneClipMap(const ClipMap& retail, float z, ClipMap& out, float& height, std::string& error) {
		if (!retail.hasWorld || retail.worldEntries.empty()) {
			error = "the clip map has no terrain to flatten";
			return false;
		}
		out = ClipMap{};
		out.root = retail.root;
		out.cellIndex = retail.cellIndex;
		out.trees = retail.trees;
		out.hasTreeIndex = retail.hasTreeIndex;
		out.treeIndex = retail.treeIndex;
		out.treeIndexArrays = retail.treeIndexArrays;
		out.hasWorld = true;
		out.worldHead = retail.worldHead;
		out.rootStrings = retail.rootStrings;

		for (const ClipMapWorldEntry& source : retail.worldEntries) {
			const float offset = Get<float>(source.raw, 112);
			const float scale = Get<float>(source.raw, 148);
			if (!(scale > 0.0f) || source.list.empty()) {
				error = std::format("a terrain entry has scale {} and {} surfaces: not the retail shape", scale, source.list.size());
				return false;
			}
			const long quantized = std::lround((z - offset) / scale);
			if (quantized < 0 || quantized > 0xFFFF) {
				error = std::format("height {} is outside what the terrain can store ({} .. {})", z, offset, offset + 0xFFFF * scale);
				return false;
			}
			const auto h = static_cast<std::uint16_t>(quantized);
			height = h * scale + offset;

			ClipMapWorldEntry& entry = out.worldEntries.emplace_back();
			entry.raw = source.raw;
			entry.list = source.list;
			for (const auto& item : entry.list) {
				if (item.empty()) {
					error = "a terrain surface is not stored inline";
					return false;
				}
			}
			// The surface every tile uses: the one retail single-surface tiles use most (u16 @70 when u16 @68 is 1).
			std::vector<std::size_t> votes(65536);
			for (const ClipMapWorldRecord& record : source.records) {
				if (Get<std::uint16_t>(record.raw, 68) == 1) {
					++votes[Get<std::uint16_t>(record.raw, 70)];
				}
			}
			const auto surface = static_cast<std::uint16_t>(std::max_element(votes.begin(), votes.end()) - votes.begin());

			for (const ClipMapWorldRecord& tile : source.records) {
				const std::uint16_t rows = Get<std::uint16_t>(tile.raw, 34);
				const std::uint16_t columns = Get<std::uint16_t>(tile.raw, 36);
				if (Get<std::uint16_t>(tile.raw, 32) != rows * columns) {
					error = std::format("a terrain tile has {} heights for {} x {}", Get<std::uint16_t>(tile.raw, 32), rows, columns);
					return false;
				}
				ClipMapWorldRecord& record = entry.records.emplace_back();
				record.raw = tile.raw;
				const std::span<std::uint8_t> r(record.raw);
				Set(r, 8, height);  // mins z
				Set(r, 20, height); // maxs z
				// Every quad present, so no +48 bits (21 zm_silver tiles have holes: +38 2 with bits; kept, that was
				// the D2 17:48 null read in CM_TerrainTile_TraceQuad 0x7FF7287C2270).
				Set(r, 38, static_cast<std::uint16_t>(1));
				Set(r, 68, static_cast<std::uint16_t>(1));
				Set(r, 70, surface);
				record.indices.resize(2ull * rows * columns);
				for (std::size_t k = 0; k < record.indices.size(); k += 2) {
					PutAt(std::span<std::uint8_t>(record.indices), k, h);
				}
			}
		}
		return true;
	}

	bool BoxHull(const std::array<float, 3>& center, const std::array<std::array<float, 3>, 3>& halfAxes,
		std::uint32_t contents, ConvexHull& out, std::string& error) {
		using Vec = std::array<float, 3>;
		const auto dot = [](const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
		const auto cross = [](const Vec& a, const Vec& b) {
			return Vec{ a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
		};
		out = ConvexHull{};
		out.contents = contents;
		out.mins = { FLT_MAX, FLT_MAX, FLT_MAX };
		out.maxs = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (int corner = 0; corner < 8; ++corner) {
			Vec p = center;
			for (int axis = 0; axis < 3; ++axis) {
				const float sign = (corner >> axis) & 1 ? 1.0f : -1.0f;
				for (int k = 0; k < 3; ++k) {
					p[k] += sign * halfAxes[axis][k];
				}
			}
			for (int k = 0; k < 3; ++k) {
				out.mins[k] = std::min(out.mins[k], p[k]);
				out.maxs[k] = std::max(out.maxs[k], p[k]);
			}
			out.vertices.push_back(p);
		}
		// Face pair `axis` lies through center +- halfAxes[axis]; its normal is across the other two axes, which
		// is halfAxes[axis] itself only when the box is not sheared.
		for (int axis = 0; axis < 3; ++axis) {
			Vec n = cross(halfAxes[(axis + 1) % 3], halfAxes[(axis + 2) % 3]);
			const float length = std::sqrt(dot(n, n));
			if (!(length > 1e-6f)) {
				error = "the box is flat";
				return false;
			}
			for (float& v : n) {
				v /= length;
			}
			const float thickness = dot(n, halfAxes[axis]);
			if (std::abs(thickness) < 1e-3f) {
				error = "the box is flat";
				return false;
			}
			if (thickness < 0) {
				for (float& v : n) {
					v = -v;
				}
			}
			// An axis-aligned face is one of the AABB's own.
			if (std::max({ std::abs(n[0]), std::abs(n[1]), std::abs(n[2]) }) > 1.0f - 1e-5f) {
				continue;
			}
			for (const float sign : { 1.0f, -1.0f }) {
				Vec face = center;
				for (int k = 0; k < 3; ++k) {
					face[k] += sign * halfAxes[axis][k];
				}
				const Vec normal = { sign * n[0], sign * n[1], sign * n[2] };
				out.planes.push_back({ normal[0], normal[1], normal[2], dot(normal, face) });
			}
		}
		return true;
	}

	bool MeshHull(std::span<const std::array<float, 3>> points, const std::array<std::array<float, 3>, 3>& axes, float scale,
		const std::array<float, 3>& origin, std::uint32_t contents, ConvexHull& out, std::string& error) {
		using Vec = std::array<float, 3>;
		const auto dot = [](const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
		const auto cross = [](const Vec& a, const Vec& b) {
			return Vec{ a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
		};
		if (points.empty()) {
			error = "the model has no points";
			return false;
		}
		// The 26 directions in opposite pairs: direction 2i and 2i + 1 point opposite ways.
		std::vector<Vec> normals;
		for (int x = -1; x <= 1; ++x) {
			for (int y = -1; y <= 1; ++y) {
				for (int z = -1; z <= 1; ++z) {
					if ((x || y || z) && (x > 0 || (x == 0 && (y > 0 || (y == 0 && z > 0))))) {
						const float length = std::sqrt(static_cast<float>(x * x + y * y + z * z));
						normals.push_back({ x / length, y / length, z / length });
						normals.push_back({ -x / length, -y / length, -z / length });
					}
				}
			}
		}
		std::vector<float> distances(normals.size(), -FLT_MAX);
		for (const Vec& p : points) {
			for (std::size_t i = 0; i < normals.size(); ++i) {
				distances[i] = std::max(distances[i], dot(normals[i], p));
			}
		}
		for (std::size_t i = 0; i < normals.size(); i += 2) {
			const float thickness = distances[i] + distances[i + 1];
			if (thickness < 1.0f) {
				distances[i] += (1.0f - thickness) / 2;
				distances[i + 1] += (1.0f - thickness) / 2;
			}
		}
		// The corners: every three planes that meet at a point no other plane cuts off.
		std::vector<Vec> corners;
		for (std::size_t i = 0; i < normals.size(); ++i) {
			for (std::size_t j = i + 1; j < normals.size(); ++j) {
				for (std::size_t k = j + 1; k < normals.size(); ++k) {
					const Vec jk = cross(normals[j], normals[k]);
					const float det = dot(normals[i], jk);
					if (std::abs(det) < 1e-4f) {
						continue;
					}
					const Vec ki = cross(normals[k], normals[i]);
					const Vec ij = cross(normals[i], normals[j]);
					Vec p{};
					for (int c = 0; c < 3; ++c) {
						p[c] = (distances[i] * jk[c] + distances[j] * ki[c] + distances[k] * ij[c]) / det;
					}
					bool inside = true;
					for (std::size_t m = 0; m < normals.size() && inside; ++m) {
						inside = dot(normals[m], p) <= distances[m] + 0.01f + 1e-5f * std::abs(distances[m]);
					}
					if (!inside) {
						continue;
					}
					const bool known = std::ranges::any_of(corners, [&p](const Vec& q) {
						return std::abs(p[0] - q[0]) + std::abs(p[1] - q[1]) + std::abs(p[2] - q[2]) < 0.01f;
					});
					if (!known) {
						corners.push_back(p);
					}
				}
			}
		}
		if (corners.size() < 4 || corners.size() > 255) {
			error = std::format("the hull has {} corners (4 to 255 fit)", corners.size());
			return false;
		}
		const auto toWorld = [&](const Vec& v) {
			Vec w = origin;
			for (int c = 0; c < 3; ++c) {
				w[c] += scale * (axes[0][c] * v[0] + axes[1][c] * v[1] + axes[2][c] * v[2]);
			}
			return w;
		};
		out = ConvexHull{};
		out.contents = contents;
		out.mins = { FLT_MAX, FLT_MAX, FLT_MAX };
		out.maxs = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const Vec& corner : corners) {
			const Vec w = toWorld(corner);
			for (int c = 0; c < 3; ++c) {
				out.mins[c] = std::min(out.mins[c], w[c]);
				out.maxs[c] = std::max(out.maxs[c], w[c]);
			}
			out.vertices.push_back(w);
		}
		// Each plane turned with the model. Every one touches the hull (it passes through a point of the model), so
		// one that ends up axis-aligned is one of the AABB's own faces.
		for (std::size_t i = 0; i < normals.size(); ++i) {
			Vec n{};
			for (int c = 0; c < 3; ++c) {
				n[c] = axes[0][c] * normals[i][0] + axes[1][c] * normals[i][1] + axes[2][c] * normals[i][2];
			}
			if (std::max({ std::abs(n[0]), std::abs(n[1]), std::abs(n[2]) }) > 1.0f - 1e-5f) {
				continue;
			}
			out.planes.push_back({ n[0], n[1], n[2], scale * distances[i] + dot(n, origin) });
		}
		return true;
	}

	bool TriangleHull(const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c,
		const std::array<float, 3>& front, float thickness, std::uint32_t contents, ConvexHull& out, std::string& error) {
		using Vec = std::array<float, 3>;
		const auto dot = [](const Vec& p, const Vec& q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
		const auto cross = [](const Vec& p, const Vec& q) {
			return Vec{ p[1] * q[2] - p[2] * q[1], p[2] * q[0] - p[0] * q[2], p[0] * q[1] - p[1] * q[0] };
		};
		const auto sub = [](const Vec& p, const Vec& q) { return Vec{ p[0] - q[0], p[1] - q[1], p[2] - q[2] }; };
		const auto unit = [&dot](Vec v) {
			const float length = std::sqrt(dot(v, v));
			for (float& x : v) {
				x = length > 1e-6f ? x / length : 0.0f;
			}
			return v;
		};
		const Vec area = cross(sub(b, a), sub(c, a));
		if (std::sqrt(dot(area, area)) < 1e-3f) {
			error = "the triangle has no area";
			return false;
		}
		Vec n = unit(area);
		if (dot(n, front) < 0) {
			n = { -n[0], -n[1], -n[2] };
		}
		const std::array<Vec, 3> top = { a, b, c };
		out = ConvexHull{};
		out.contents = contents;
		out.mins = { FLT_MAX, FLT_MAX, FLT_MAX };
		out.maxs = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const Vec& p : top) {
			for (const Vec& q : { p, Vec{ p[0] - n[0] * thickness, p[1] - n[1] * thickness, p[2] - n[2] * thickness } }) {
				out.vertices.push_back(q);
				for (int k = 0; k < 3; ++k) {
					out.mins[k] = std::min(out.mins[k], q[k]);
					out.maxs[k] = std::max(out.maxs[k], q[k]);
				}
			}
		}
		// The face, the back, and one side per edge (through the edge, along n, facing away from the third corner).
		std::vector<std::array<float, 4>> planes = { { n[0], n[1], n[2], dot(n, a) },
			{ -n[0], -n[1], -n[2], -dot(n, a) + thickness } };
		for (int e = 0; e < 3; ++e) {
			const Vec& p = top[e];
			const Vec& q = top[(e + 1) % 3];
			const Vec& other = top[(e + 2) % 3];
			Vec side = unit(cross(sub(q, p), n));
			if (dot(side, sub(other, p)) > 0) {
				side = { -side[0], -side[1], -side[2] };
			}
			planes.push_back({ side[0], side[1], side[2], dot(side, p) });
		}
		// Every plane touches the slab, so an axis-aligned one is one of the AABB's own faces.
		for (const auto& plane : planes) {
			if (std::max({ std::abs(plane[0]), std::abs(plane[1]), std::abs(plane[2]) }) <= 1.0f - 1e-5f) {
				out.planes.push_back(plane);
			}
		}
		return true;
	}

	bool AppendHulls(CollisionTree& tree, std::span<const ConvexHull> hulls, std::uint16_t surface, std::string& error) {
		const std::span<std::uint8_t> r(tree.raw);
		if (Get<std::uint64_t>(tree.raw, 88) || !tree.arrays[7].empty()) {
			error = "the tree has a BVH (+88), which mapkit does not build";
			return false;
		}
		if (surface > 0x3FF) {
			error = std::format("surface {} does not fit the 10-bit face field", surface);
			return false;
		}
		std::uint32_t planeCount = Get<std::uint32_t>(tree.raw, 24);
		std::uint32_t vertexCount = Get<std::uint32_t>(tree.raw, 28);
		std::uint32_t hullCount = Get<std::uint32_t>(tree.raw, 32);
		// The face surfaces (+80), unpacked: 10 bits each, 6 per u64 (CM_Hull_GetFaceSurface 0x7FF7295F51A0); a
		// hull's run is its AABB's 6 faces, then one per extra plane.
		std::vector<std::uint16_t> faces(planeCount + 6ull * hullCount);
		if (tree.arrays[5].size() < 8 * ((faces.size() + 5) / 6)) {
			error = "the tree's face array is shorter than its counts";
			return false;
		}
		for (std::size_t i = 0; i < faces.size(); ++i) {
			const auto word = Get<std::uint64_t>(tree.arrays[5], 8 * (i / 6));
			faces[i] = static_cast<std::uint16_t>((word >> (10 * (i % 6))) & 0x3FF);
		}
		const auto append = [](std::vector<std::uint8_t>& bytes, const auto& value) {
			const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
			bytes.insert(bytes.end(), p, p + sizeof(value));
		};
		std::uint32_t mask = Get<std::uint32_t>(tree.raw, 36);
		std::array<float, 3> mins{}, maxs{};
		for (int k = 0; k < 3; ++k) {
			mins[k] = Get<float>(tree.raw, 4 * k);
			maxs[k] = Get<float>(tree.raw, 12 + 4 * k);
		}
		for (const ConvexHull& hull : hulls) {
			if (hull.planes.size() > 63 || hull.vertices.size() > 255 || hull.contents > 0x3FFFFFF || vertexCount >= (1u << 24)) {
				error = std::format("a hull has {} planes, {} vertices and contents {:#x}: past the tree's limits (63, 255, 26 bits)",
					hull.planes.size(), hull.vertices.size(), hull.contents);
				return false;
			}
			const auto planes = static_cast<std::uint32_t>(hull.planes.size());
			const auto vertices = static_cast<std::uint32_t>(hull.vertices.size());
			append(tree.arrays[2], planeCount);                        // +104 first plane
			append(tree.arrays[3], (vertices << 24) | vertexCount);    // +112 vertex count, first vertex
			append(tree.arrays[4], static_cast<std::uint32_t>(faces.size())); // +120 first face
			faces.insert(faces.end(), 6 + planes, surface);
			for (const auto& plane : hull.planes) {
				append(tree.arrays[0], plane);
			}
			for (const auto& vertex : hull.vertices) {
				append(tree.arrays[1], vertex);
			}
			append(tree.arrays[6], hull.mins);
			append(tree.arrays[6], hull.maxs);
			append(tree.arrays[6], (planes << 26) | hull.contents);
			planeCount += planes;
			vertexCount += vertices;
			++hullCount;
			mask |= hull.contents;
			for (int k = 0; k < 3; ++k) {
				mins[k] = std::min(mins[k], hull.mins[k] - 1.0f);
				maxs[k] = std::max(maxs[k], hull.maxs[k] + 1.0f);
			}
		}
		tree.arrays[5].assign(8 * ((faces.size() + 5) / 6), 0);
		for (std::size_t i = 0; i < faces.size(); ++i) {
			const std::size_t at = 8 * (i / 6);
			const auto word = Get<std::uint64_t>(tree.arrays[5], at) | (static_cast<std::uint64_t>(faces[i]) << (10 * (i % 6)));
			PutAt(std::span<std::uint8_t>(tree.arrays[5]), at, word);
		}
		Set(r, 24, planeCount);
		Set(r, 28, vertexCount);
		Set(r, 32, hullCount);
		Set(r, 36, mask);
		for (int k = 0; k < 3; ++k) {
			Set(r, 4 * k, mins[k]);
			Set(r, 12 + 4 * k, maxs[k]);
		}
		return true;
	}

	void EncodeEmptyStreamerWorld(XWriter& w, const std::array<std::uint8_t, 224>& retailRoot, std::uint64_t lightingName) {
		std::array<std::uint8_t, 224> root = retailRoot;
		const std::span<std::uint8_t> r(root);
		for (std::size_t field = 16; field < 128; field += 8) {
			Set(r, field, 0ull);
		}
		for (std::size_t field = 176; field < 208; field += 8) {
			Set(r, field, 0ull);
		}
		Set(r, 208, kPtrNull);
		Set(r, 216, Present(lightingName != 0));

		// Load_StreamerworldAsset: the root in the temp block, the body in block 4 (every array null), then
		// the terraingfx and lighting, each under a push of the temp block.
		w.Push(XBlockTemp);
		w.Alloc(8);
		w.Write(root.data(), root.size());
		w.Push(XBlockVirtual);
		w.Push(XBlockTemp);
		w.Pop();
		if (lightingName) {
			EncodeAssetReference(w, 0xAA, lightingName, 472);
		}
		w.Pop();
		w.Pop();
	}

	void EncodeBgCache(XWriter& w, std::uint64_t name, std::span<const BgCacheEntry> entries) {
		std::array<std::uint8_t, 24> root{};
		const std::span<std::uint8_t> r(root);
		Set(r, 0, name);
		Set(r, 8, Present(!entries.empty()));
		Set(r, 16, static_cast<std::int32_t>(entries.size()));
		// Load_BgcacheAsset: the root in the temp block, the entries in block 4 (the engine writes their +16 slots
		// at level load).
		w.Push(XBlockTemp);
		w.Alloc(8);
		w.Write(root.data(), root.size());
		w.Push(XBlockVirtual);
		if (!entries.empty()) {
			w.Alloc(8);
			for (const BgCacheEntry& entry : entries) {
				std::array<std::uint8_t, 24> bytes{};
				bytes[0] = entry.table;
				Set(std::span<std::uint8_t>(bytes), 8, entry.name);
				w.Write(bytes.data(), bytes.size());
			}
		}
		w.Pop();
		w.Pop();
	}

	std::uint32_t KeyValueKeyHash(std::string_view key) {
		std::int32_t v = 0;
		for (std::size_t i = 0; i < key.size() && i < 64 && key[i]; ++i) {
			v += static_cast<std::int32_t>(static_cast<signed char>(key[i])) * static_cast<std::int32_t>(119 + i);
		}
		return static_cast<std::uint32_t>(v ^ ((v ^ (v >> 10)) >> 10));
	}

	void EncodeKeyValuePairs(XWriter& w, std::uint64_t name, std::span<const std::pair<std::uint32_t, std::string>> pairs) {
		std::array<std::uint8_t, 24> root{};
		const std::span<std::uint8_t> r(root);
		Set(r, 0, name);
		Set(r, 8, static_cast<std::int32_t>(pairs.size()));
		Set(r, 16, Present(!pairs.empty()));
		// Load_KeyValuePairs 0x7FF71E7DD5D0: the root in the temp block, then in block 4 the 16-B pairs and each value
		// inline after them.
		w.Push(XBlockTemp);
		w.Alloc(8);
		w.Write(root.data(), root.size());
		w.Push(XBlockVirtual);
		if (!pairs.empty()) {
			w.Alloc(8);
			for (const auto& [key, value] : pairs) {
				std::array<std::uint8_t, 16> bytes{};
				Set(std::span<std::uint8_t>(bytes), 0, key);
				Set(std::span<std::uint8_t>(bytes), 8, kPtrInline);
				w.Write(bytes.data(), bytes.size());
			}
			for (const auto& [key, value] : pairs) {
				w.Alloc(1);
				w.WriteString(value);
			}
		}
		w.Pop();
		w.Pop();
	}

	void EncodeEmptyDistricts(XWriter& w, const std::array<std::uint8_t, 96>& retailRoot) {
		std::array<std::uint8_t, 96> root = retailRoot;
		const std::span<std::uint8_t> r(root);
		for (std::size_t field = 8; field < 56; field += 8) {
			Set(r, field, 0ull);
		}
		for (std::size_t field = 64; field < 96; field += 8) {
			Set(r, field, 0ull);
		}

		// Load_DistrictsAsset: the root in the temp block, the body in block 4, where the null gfx_map is still
		// read under a push of the temp block.
		w.Push(XBlockTemp);
		w.Alloc(8);
		w.Write(root.data(), root.size());
		w.Push(XBlockVirtual);
		w.Push(XBlockTemp);
		w.Pop();
		w.Pop();
		w.Pop();
	}
}
