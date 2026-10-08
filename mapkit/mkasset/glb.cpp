#include "glb.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <map>

namespace MapKit {
	namespace {
		using json = nlohmann::json;

		constexpr float kUnitsPerMeter = 39.37008f;

		// Game (X forward, Y left, Z up) to glTF (-Z forward, -X left, Y up): an axis swap, no mirror.
		std::array<float, 3> Direction(const std::array<float, 3>& v) {
			return { -v[1], v[2], -v[0] };
		}

		std::array<float, 3> Position(const std::array<float, 3>& v) {
			const auto d = Direction(v);
			return { d[0] / kUnitsPerMeter, d[1] / kUnitsPerMeter, d[2] / kUnitsPerMeter };
		}

		std::array<float, 3> Cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
			return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
		}

		std::array<float, 3> Sub(const std::array<float, 3>& a, const std::array<float, 3>& b) {
			return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
		}

		float Dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}

		class BinWriter {
		public:
			// Appends one tightly packed array as a buffer view (4-aligned) and returns the view's index.
			template <typename T>
			std::size_t View(const std::vector<T>& items, int target, json& views) {
				while (m_Data.size() % 4) {
					m_Data.push_back(0);
				}
				const std::size_t offset = m_Data.size();
				const std::size_t size = items.size() * sizeof(T);
				m_Data.resize(offset + size);
				std::memcpy(m_Data.data() + offset, items.data(), size);
				views.push_back({ { "buffer", 0 }, { "byteOffset", offset }, { "byteLength", size }, { "target", target } });
				return views.size() - 1;
			}
			std::vector<std::uint8_t>& Data() { return m_Data; }

		private:
			std::vector<std::uint8_t> m_Data;
		};

		void AppendChunk(std::vector<std::uint8_t>& out, std::uint32_t type, std::vector<std::uint8_t> data, std::uint8_t pad) {
			while (data.size() % 4) {
				data.push_back(pad);
			}
			const std::uint32_t length = static_cast<std::uint32_t>(data.size());
			const auto* l = reinterpret_cast<const std::uint8_t*>(&length);
			const auto* t = reinterpret_cast<const std::uint8_t*>(&type);
			out.insert(out.end(), l, l + 4);
			out.insert(out.end(), t, t + 4);
			out.insert(out.end(), data.begin(), data.end());
		}
	}

	std::vector<std::uint8_t> EncodeGlb(const Zone::ModelGeometry& geometry, const std::string& name) {
		json views = json::array();
		json accessors = json::array();
		json primitives = json::array();
		json materials = json::array();
		std::map<std::uint64_t, std::size_t> materialIndex;
		BinWriter bin;

		for (const Zone::GeometrySurface& surface : geometry.surfaces) {
			if (surface.triangles.empty() || surface.positions.empty()) {
				continue;
			}
			std::vector<std::array<float, 3>> positions, normals;
			positions.reserve(surface.positions.size());
			normals.reserve(surface.normals.size());
			std::array<float, 3> lo = { 1e30f, 1e30f, 1e30f }, hi = { -1e30f, -1e30f, -1e30f };
			for (const auto& p : surface.positions) {
				positions.push_back(Position(p));
				for (int k = 0; k < 3; ++k) {
					lo[k] = std::min(lo[k], positions.back()[k]);
					hi[k] = std::max(hi[k], positions.back()[k]);
				}
			}
			for (const auto& n : surface.normals) {
				normals.push_back(Direction(n));
			}
			// Clockwise front (the game) to counter-clockwise front (glTF); the axis swap keeps handedness.
			std::vector<std::uint32_t> indices;
			indices.reserve(surface.triangles.size() * 3);
			for (const auto& t : surface.triangles) {
				indices.insert(indices.end(), { t[0], t[2], t[1] });
			}

			const std::size_t count = positions.size();
			accessors.push_back({ { "bufferView", bin.View(positions, 34962, views) }, { "componentType", 5126 },
				{ "count", count }, { "type", "VEC3" }, { "min", lo }, { "max", hi } });
			const std::size_t position = accessors.size() - 1;
			accessors.push_back({ { "bufferView", bin.View(normals, 34962, views) }, { "componentType", 5126 },
				{ "count", count }, { "type", "VEC3" } });
			const std::size_t normal = accessors.size() - 1;
			accessors.push_back({ { "bufferView", bin.View(surface.uvs, 34962, views) }, { "componentType", 5126 },
				{ "count", count }, { "type", "VEC2" } });
			const std::size_t uv = accessors.size() - 1;
			accessors.push_back({ { "bufferView", bin.View(indices, 34963, views) }, { "componentType", 5125 },
				{ "count", indices.size() }, { "type", "SCALAR" } });
			const std::size_t index = accessors.size() - 1;

			auto [it, added] = materialIndex.emplace(surface.material, materials.size());
			if (added) {
				materials.push_back({ { "name", std::format("{:016X}", surface.material) },
					{ "pbrMetallicRoughness", { { "baseColorFactor", { 0.72, 0.72, 0.72, 1.0 } }, { "metallicFactor", 0.0 },
						{ "roughnessFactor", 0.85 } } } });
			}
			primitives.push_back({ { "attributes", { { "POSITION", position }, { "NORMAL", normal }, { "TEXCOORD_0", uv } } },
				{ "indices", index }, { "material", it->second } });
		}

		json scene = json::object();
		scene["nodes"] = json::array({ 0 });
		json node = json::object();
		node["name"] = name;
		node["mesh"] = 0;
		json mesh = json::object();
		mesh["name"] = name;
		mesh["primitives"] = primitives;
		json buffer = json::object();
		buffer["byteLength"] = bin.Data().size();

		json gltf = json::object();
		gltf["asset"] = { { "version", "2.0" }, { "generator", "mapkit mkasset" } };
		gltf["scene"] = 0;
		gltf["scenes"] = json::array({ scene });
		gltf["nodes"] = json::array({ node });
		gltf["meshes"] = json::array({ mesh });
		gltf["materials"] = materials;
		gltf["accessors"] = accessors;
		gltf["bufferViews"] = views;
		gltf["buffers"] = json::array({ buffer });
		const std::string text = gltf.dump();

		std::vector<std::uint8_t> out = { 'g', 'l', 'T', 'F', 2, 0, 0, 0, 0, 0, 0, 0 };
		AppendChunk(out, 0x4E4F534A, std::vector<std::uint8_t>(text.begin(), text.end()), ' ');
		AppendChunk(out, 0x004E4942, std::move(bin.Data()), 0);
		const std::uint32_t total = static_cast<std::uint32_t>(out.size());
		std::memcpy(out.data() + 8, &total, 4);
		return out;
	}

	double WindingAgreement(const Zone::ModelGeometry& geometry) {
		std::size_t agree = 0, counted = 0;
		for (const Zone::GeometrySurface& surface : geometry.surfaces) {
			for (const auto& t : surface.triangles) {
				const auto& a = surface.positions[t[0]];
				const auto& b = surface.positions[t[1]];
				const auto& c = surface.positions[t[2]];
				// Clockwise seen from the front, in a right-handed space: the front normal is (c - a) x (b - a).
				const auto face = Cross(Sub(c, a), Sub(b, a));
				std::array<float, 3> vertex{};
				for (const auto i : t) {
					for (int k = 0; k < 3; ++k) {
						vertex[k] += surface.normals[i][k];
					}
				}
				const float d = Dot(face, vertex);
				if (d != 0.0f) {
					++counted;
					agree += d > 0.0f;
				}
			}
		}
		return counted ? static_cast<double>(agree) / static_cast<double>(counted) : 0.0;
	}
}
