#include "model_writer.hpp"
#include "xstream.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace MapKit::Zone {
	namespace {
		constexpr std::size_t kModelRootSize = 232;
		constexpr std::size_t kSkeletonRootSize = 88;
		constexpr std::size_t kMeshRootSize = 64;
		constexpr std::size_t kMeshInfoSize = 464;
		constexpr std::size_t kSurfaceSize = 48;
		constexpr std::size_t kVertexSize = 16;
		constexpr int XBlockResident = 12;
		constexpr std::uint32_t kMeshInfoFlags = 0x08; // resident, 16-B vertices, no weights: both reference models
		// The LOD 0 draw distance. Retail single-LOD props store 67182 to 554927; the level's own geometry must
		// never be culled by distance.
		constexpr float kDrawDistance = 1.0e6f;

		template <typename T>
		void Set(std::span<std::uint8_t> data, std::size_t offset, const T& value) {
			std::memcpy(data.data() + offset, &value, sizeof(value));
		}

		void WriteBytes(XWriter& w, std::span<const std::uint8_t> bytes) {
			w.Write(bytes.data(), bytes.size());
		}

		// The 96-B block every retail static model, LOD and surface points at: 0x80000000, then zeros.
		std::array<std::uint8_t, 96> DefaultBlock() {
			std::array<std::uint8_t, 96> block{};
			Set(std::span<std::uint8_t>(block), 0, 0x80000000u);
			return block;
		}

		std::uint16_t FloatToHalf(float value) {
			std::uint32_t bits;
			std::memcpy(&bits, &value, 4);
			const std::uint32_t sign = (bits >> 16) & 0x8000;
			const std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFF) - 127 + 15;
			std::uint32_t mantissa = bits & 0x7FFFFF;
			if (exponent <= 0) {
				if (exponent < -10) {
					return static_cast<std::uint16_t>(sign);
				}
				mantissa |= 0x800000;
				const int shift = 14 - exponent;
				std::uint32_t half = mantissa >> shift;
				if ((mantissa >> (shift - 1)) & 1) {
					++half;
				}
				return static_cast<std::uint16_t>(sign | half);
			}
			if (exponent >= 31) {
				return static_cast<std::uint16_t>(sign | 0x7BFF); // clamp to the largest finite half
			}
			std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
			if (mantissa & 0x1000) {
				++half; // round to nearest (a carry into the exponent is still correct)
			}
			return static_cast<std::uint16_t>(half);
		}

		// 10:10:10:2. Retail stores -1 as 1, 0 as 512 and +1 as 1023: n * 511 + 512.
		std::uint32_t PackUnitVector(const std::array<float, 3>& v, std::uint32_t w) {
			std::uint32_t out = (w & 3u) << 30;
			for (int axis = 0; axis < 3; ++axis) {
				const float n = std::clamp(v[axis], -1.0f, 1.0f);
				out |= (static_cast<std::uint32_t>(std::lround(n * 511.0f + 512.0f)) & 0x3FFu) << (10 * axis);
			}
			return out;
		}

		std::size_t Align16(std::size_t size) {
			return (size + 15) & ~std::size_t(15);
		}

		struct MeshLayout {
			std::size_t vertices = 0;
			std::size_t indices = 0;
			std::size_t positionOffset = 0;
			std::size_t vertexOffset = 0;
			std::size_t size = 0;
		};

		MeshLayout Layout(const StaticModel& model) {
			MeshLayout layout;
			for (const StaticSurface& surface : model.surfaces) {
				layout.vertices += surface.vertices.size();
				layout.indices += surface.triangles.size() * 3;
			}
			layout.positionOffset = Align16(layout.indices * 2);
			layout.vertexOffset = layout.positionOffset + Align16(layout.vertices * 12);
			layout.size = layout.vertexOffset + layout.vertices * kVertexSize;
			return layout;
		}

		std::vector<std::uint8_t> BuildBuffer(const StaticModel& model, const MeshLayout& layout) {
			std::vector<std::uint8_t> buffer(layout.size);
			const std::span<std::uint8_t> out(buffer);
			std::size_t index = 0;
			std::size_t vertex = 0;
			for (const StaticSurface& surface : model.surfaces) {
				for (const auto& triangle : surface.triangles) {
					for (const std::uint16_t corner : triangle) {
						Set(out, 2 * index++, corner);
					}
				}
				for (const StaticVertex& v : surface.vertices) {
					for (int axis = 0; axis < 3; ++axis) {
						Set(out, layout.positionOffset + 12 * vertex + 4 * axis, v.position[axis]);
					}
					const std::size_t at = layout.vertexOffset + kVertexSize * vertex;
					Set(out, at, 0xFFFFFFFFu);
					Set(out, at + 4, FloatToHalf(v.uv[0]));
					Set(out, at + 6, FloatToHalf(v.uv[1]));
					Set(out, at + 8, PackUnitVector(v.normal, 0));
					Set(out, at + 12, PackUnitVector(v.tangent, v.bitangentSign < 0 ? 0u : 3u));
					++vertex;
				}
			}
			return buffer;
		}

		float TriangleArea(const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c) {
			const float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			const float v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			const float x = u[1] * v[2] - u[2] * v[1], y = u[2] * v[0] - u[0] * v[2], z = u[0] * v[1] - u[1] * v[0];
			return 0.5f * std::sqrt(x * x + y * y + z * z);
		}

		// UV area per world area over a surface (the xmodel's per-surface texel-density float).
		float UvDensity(const StaticSurface& surface) {
			double world = 0, uv = 0;
			for (const auto& t : surface.triangles) {
				const StaticVertex& a = surface.vertices[t[0]];
				const StaticVertex& b = surface.vertices[t[1]];
				const StaticVertex& c = surface.vertices[t[2]];
				world += TriangleArea(a.position, b.position, c.position);
				uv += TriangleArea({ a.uv[0], a.uv[1], 0 }, { b.uv[0], b.uv[1], 0 }, { c.uv[0], c.uv[1], 0 });
			}
			return world > 0 ? static_cast<float>(uv / world) : 1.0f;
		}
	}

	void AppendSurface(StaticModel& model, const StaticSurface& surface) {
		if (surface.vertices.empty()) {
			return;
		}
		if (model.surfaces.empty() || model.surfaces.back().material != surface.material
			|| model.surfaces.back().vertices.size() + surface.vertices.size() > 0xFFFF) {
			StaticSurface next;
			next.material = surface.material;
			model.surfaces.push_back(std::move(next));
		}
		StaticSurface& into = model.surfaces.back();
		const auto base = static_cast<std::uint16_t>(into.vertices.size());
		into.vertices.insert(into.vertices.end(), surface.vertices.begin(), surface.vertices.end());
		for (const auto& t : surface.triangles) {
			into.triangles.push_back({ static_cast<std::uint16_t>(t[0] + base), static_cast<std::uint16_t>(t[1] + base),
				static_cast<std::uint16_t>(t[2] + base) });
		}
	}

	StaticModelBounds ModelBounds(const StaticModel& model) {
		StaticModelBounds bounds;
		bool first = true;
		for (const StaticSurface& surface : model.surfaces) {
			for (const StaticVertex& v : surface.vertices) {
				for (int axis = 0; axis < 3; ++axis) {
					bounds.mins[axis] = first ? v.position[axis] : std::min(bounds.mins[axis], v.position[axis]);
					bounds.maxs[axis] = first ? v.position[axis] : std::max(bounds.maxs[axis], v.position[axis]);
				}
				bounds.radius = std::max(bounds.radius,
					std::sqrt(v.position[0] * v.position[0] + v.position[1] * v.position[1] + v.position[2] * v.position[2]));
				first = false;
			}
		}
		return bounds;
	}

	// Mirrors Load_XSkeleton 0x7FF71E7F8EB0 (model_assets.cpp LoadXSkeletonBody): root, then in block 4 the
	// bone names, the +16 pairs, the part classification, the base matrix and the bone info (no parented bones).
	void EncodeStaticSkeleton(XWriter& w, const StaticModel& model) {
		const StaticModelBounds bounds = ModelBounds(model);

		std::array<std::uint8_t, kSkeletonRootSize> root{};
		const std::span<std::uint8_t> r(root);
		Set(r, 0, model.skeleton);
		for (const std::size_t at : { 8, 16, 48, 56, 64 }) {
			Set(r, at, kPtrInline);
		}
		Set(r, 74, static_cast<std::uint16_t>(1)); // bones
		Set(r, 76, static_cast<std::uint16_t>(1)); // roots

		w.Push(XBlockTemp);
		w.Alloc(8);
		WriteBytes(w, root);
		w.Push(XBlockVirtual);
		const std::uint32_t name = w.ScriptString(model.boneName);
		w.Alloc(4);
		w.Put(name);
		w.Alloc(4);
		w.Put(name);
		w.Put(std::uint32_t(0));
		w.Alloc(1);
		w.Put(std::uint8_t(0));
		w.Alloc(16);
		for (const float value : { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 2.0f }) {
			w.Put(value);
		}
		w.Alloc(4);
		float radiusSquared = 0;
		std::array<float, 3> center{};
		for (int axis = 0; axis < 3; ++axis) {
			center[axis] = (bounds.mins[axis] + bounds.maxs[axis]) / 2;
			const float half = (bounds.maxs[axis] - bounds.mins[axis]) / 2;
			radiusSquared += half * half;
		}
		for (const auto& vector : { bounds.mins, bounds.maxs, center }) {
			for (const float value : vector) {
				w.Put(value);
			}
		}
		w.Put(radiusSquared);
		w.Pop();
		w.Pop();
	}

	// Mirrors Load_XModelMesh 0x7FF71E7F8A50 (model_assets.cpp LoadXModelMeshBody): root, then in block 4 the
	// 96-B block (+24), the mesh info (+16) with its buffer in block 12, and the surfaces (+8).
	void EncodeStaticModelMesh(XWriter& w, const StaticModel& model) {
		const MeshLayout layout = Layout(model);
		const std::vector<std::uint8_t> buffer = BuildBuffer(model, layout);
		std::size_t triangles = 0;
		double area = 0;
		for (const StaticSurface& surface : model.surfaces) {
			triangles += surface.triangles.size();
			for (const auto& t : surface.triangles) {
				area += TriangleArea(surface.vertices[t[0]].position, surface.vertices[t[1]].position, surface.vertices[t[2]].position);
			}
		}

		std::array<std::uint8_t, kMeshRootSize> root{};
		const std::span<std::uint8_t> r(root);
		Set(r, 0, model.meshName);
		Set(r, 8, kPtrInline);
		Set(r, 16, kPtrInline);
		Set(r, 24, kPtrInline);
		// +56: 0.5 on the 1 x 1 quad (its mean triangle area); not understood beyond that.
		Set(r, 56, triangles ? static_cast<float>(area / triangles) : 0.0f);
		Set(r, 60, static_cast<std::uint16_t>(model.surfaces.size()));
		Set(r, 62, model.meshWord62);

		w.Push(XBlockTemp);
		w.Alloc(8);
		WriteBytes(w, root);
		w.Push(XBlockVirtual);

		w.Alloc(4);
		const std::uint64_t blockPos = w.Position();
		WriteBytes(w, DefaultBlock());

		std::array<std::uint8_t, kMeshInfoSize> info{};
		const std::span<std::uint8_t> i(info);
		Set(i, 0, kMeshInfoFlags);
		Set(i, 4, static_cast<std::uint32_t>(layout.vertices));
		Set(i, 12, static_cast<std::uint32_t>(layout.indices));
		Set(i, 32, kPtrInline);
		Set(i, 40, static_cast<std::uint32_t>(layout.size));
		Set(i, 44, static_cast<std::uint32_t>(layout.positionOffset));
		Set(i, 48, static_cast<std::uint32_t>(layout.vertexOffset));
		for (const std::size_t at : { 264, 304, 344 }) {
			std::memset(info.data() + at, 0xFF, 12);
		}
		w.Alloc(16);
		const std::uint64_t infoPos = w.Position();
		WriteBytes(w, info);
		w.Push(XBlockResident);
		w.Alloc(256);
		WriteBytes(w, buffer);
		w.Pop();

		w.Alloc(8);
		std::size_t firstVertex = 0, firstIndex = 0;
		for (const StaticSurface& surface : model.surfaces) {
			std::array<std::uint8_t, kSurfaceSize> s{};
			const std::span<std::uint8_t> sp(s);
			Set(sp, 2, static_cast<std::uint16_t>(surface.vertices.size()));
			Set(sp, 4, static_cast<std::uint16_t>(surface.triangles.size()));
			Set(sp, 8, static_cast<std::uint32_t>(firstVertex));
			Set(sp, 12, static_cast<std::uint32_t>(firstIndex));
			Set(sp, 16, XWriter::Reference(XBlockVirtual, infoPos));
			Set(sp, 32, XWriter::Reference(XBlockVirtual, blockPos));
			WriteBytes(w, s);
			firstVertex += surface.vertices.size();
			firstIndex += surface.triangles.size() * 3;
		}
		w.Pop();
		w.Pop();
	}

	// Mirrors Load_XModel 0x7FF71E7F8420 (LoadXModelBody): skeleton, collision and LOD 0 are links (nothing
	// follows them in the stream), then the material table and its arrays, then the 96-B block.
	void EncodeStaticModel(XWriter& w, const StaticModel& model) {
		const auto link = [&w](std::uint64_t type, std::uint64_t name) {
			const auto entry = w.AssetEntry(type, name);
			return entry ? *entry : kPtrNull;
		};
		const StaticModelBounds bounds = ModelBounds(model);

		std::array<std::uint8_t, kModelRootSize> root{};
		const std::span<std::uint8_t> r(root);
		Set(r, 0, model.name);
		Set(r, 8, link(0x08, model.skeleton));
		Set(r, 16, link(0x07, model.collision));
		Set(r, 32, link(0x09, model.meshName));
		Set(r, 96, kPtrInline);
		Set(r, 104, kPtrInline);
		Set(r, 112, static_cast<std::uint16_t>(1)); // LOD count
		Set(r, 116, kDrawDistance);
		// +148..+180: the same constants on both reference models.
		Set(r, 148, 0x3ED744FDu);
		Set(r, 152, 4.0f);
		Set(r, 156, 1.0f);
		Set(r, 160, 0.25f);
		Set(r, 164, 4.0f);
		Set(r, 168, 1.0f);
		Set(r, 172, 2.0f);
		Set(r, 176, 0.5f);
		for (int axis = 0; axis < 3; ++axis) {
			Set(r, 184 + 4 * axis, bounds.mins[axis]);
			Set(r, 196 + 4 * axis, bounds.maxs[axis]);
		}
		Set(r, 208, model.flags);
		Set(r, 216, bounds.radius);
		root[220] = 0x20;
		root[221] = model.modelByte221;

		w.Push(XBlockTemp);
		w.Alloc(8);
		WriteBytes(w, root);
		w.Push(XBlockVirtual);

		// One material table (LOD 0): u16 surface count, +8 handles, +16 none, +24 texel densities.
		std::array<std::uint8_t, 32> table{};
		const std::span<std::uint8_t> t(table);
		Set(t, 0, static_cast<std::uint16_t>(model.surfaces.size()));
		Set(t, 8, kPtrInline);
		Set(t, 24, kPtrInline);
		w.Alloc(8);
		WriteBytes(w, table);
		w.Alloc(8);
		for (const StaticSurface& surface : model.surfaces) {
			w.Put(link(0x0A, surface.material));
		}
		w.Alloc(4);
		for (const StaticSurface& surface : model.surfaces) {
			w.Put(UvDensity(surface));
		}

		w.Alloc(4);
		WriteBytes(w, DefaultBlock());
		w.Pop();
		w.Pop();
	}
}
