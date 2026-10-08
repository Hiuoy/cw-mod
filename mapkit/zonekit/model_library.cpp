#include "model_library.hpp"
#include "asset_loaders.hpp"
#include "model_assets.hpp"
#include "xasset_list.hpp"
#include "zone.hpp"
#include "zone_trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>

namespace fs = std::filesystem;

namespace MapKit::Zone {
	struct ModelLibrary::Mesh {
		XModelMeshData data;
	};

	struct ModelLibrary::ZoneData {
		std::string name;
		std::vector<Mesh> meshes;
		std::unordered_map<std::uint64_t, std::size_t> infoAt; // MeshInfo::at -> mesh
		std::map<std::uint64_t, std::size_t> residentAt;       // MeshInfo::residentAt -> mesh (a reference may point inside)
	};

	namespace {
		constexpr std::uint64_t kNameMask = ~(1ull << 63);
		constexpr std::size_t kNone = SIZE_MAX;

		float HalfToFloat(std::uint16_t h) {
			const std::uint32_t sign = (h & 0x8000u) << 16;
			const std::uint32_t exponent = (h >> 10) & 0x1F;
			const std::uint32_t mantissa = h & 0x3FF;
			std::uint32_t bits;
			if (exponent == 0) {
				if (mantissa == 0) {
					bits = sign;
				}
				else {
					const float f = std::ldexp(static_cast<float>(mantissa), -24);
					std::memcpy(&bits, &f, 4);
					bits |= sign;
				}
			}
			else if (exponent == 31) {
				bits = sign | 0x7F800000u | (mantissa << 13);
			}
			else {
				bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
			}
			float out;
			std::memcpy(&out, &bits, 4);
			return out;
		}

		// 10:10:10:2, n * 511 + 512 per axis (model_writer.hpp).
		std::array<float, 3> UnpackUnitVector(std::uint32_t packed) {
			std::array<float, 3> v{};
			for (int axis = 0; axis < 3; ++axis) {
				v[axis] = (static_cast<float>((packed >> (10 * axis)) & 0x3FFu) - 512.0f) / 511.0f;
			}
			const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			if (length > 1e-6f) {
				for (float& c : v) {
					c /= length;
				}
			}
			return v;
		}

		// A stored reference into the XAsset array (block 4, just before the first asset) names the asset it
		// points at.
		std::size_t AssetOfReference(const ZoneTrace& trace, const XAssetList& list, std::uint64_t stored) {
			if (stored == kPtrNull || stored == kPtrInline || stored == kPtrInsert) {
				return kNone;
			}
			const std::uint64_t value = stored - 1;
			const std::uint64_t offset = value & 0x0FFFFFFFFFFFFFFFull;
			const std::uint64_t tableEnd = trace.assets[0].pos[XBlockVirtual];
			const std::uint64_t tableBegin = tableEnd - 16ull * list.assets.size();
			if ((value >> 60) != XBlockVirtual || offset < tableBegin || offset >= tableEnd) {
				return kNone;
			}
			return static_cast<std::size_t>((offset - tableBegin) / 16);
		}
	}

	std::size_t ModelGeometry::Vertices() const {
		std::size_t count = 0;
		for (const GeometrySurface& surface : surfaces) {
			count += surface.positions.size();
		}
		return count;
	}

	std::size_t ModelGeometry::Triangles() const {
		std::size_t count = 0;
		for (const GeometrySurface& surface : surfaces) {
			count += surface.triangles.size();
		}
		return count;
	}

	ModelLibrary::ModelLibrary() = default;
	ModelLibrary::~ModelLibrary() = default;

	const LibraryModel* ModelLibrary::Find(std::uint64_t name) const {
		const auto it = m_ByName.find(name & kNameMask);
		return it == m_ByName.end() ? nullptr : &m_Models[it->second];
	}

	const std::string& ModelLibrary::ZoneName(std::size_t zone) const {
		return m_Zones[zone]->name;
	}

	bool ModelLibrary::AddZone(const fs::path& ffPath, const fs::path& tracePath, ModelLibraryStats& stats, std::string& error) {
		stats = {};
		LoadedZone zone;
		XAssetList list;
		ZoneTrace trace;
		if (!LoadZone(ffPath, zone, error) || !ParseXAssetList(zone.stream, list, error) || !ReadZoneTrace(tracePath, trace, error)) {
			return false;
		}
		if (const std::string mismatch = AlignZoneTrace(trace, zone.stream, list); !mismatch.empty()) {
			error = std::format("the trace {} does not fit {}: {}", tracePath.filename().string(), zone.name, mismatch);
			return false;
		}
		if (!m_PackageDir.empty() && !m_Packages) {
			m_Packages = std::make_unique<Kapi::PackageIndex>();
			if (!m_Packages->Open(m_PackageDir, error)) {
				m_Packages.reset();
				return false;
			}
		}

		auto data = std::make_unique<ZoneData>();
		data->name = zone.name;
		const std::size_t zoneIndex = m_Zones.size();
		const std::span<const std::uint8_t> stream(zone.stream);

		std::unordered_map<std::uint64_t, std::size_t> inserts;    // insert slot -> the root cursor after it
		std::unordered_map<std::size_t, std::size_t> meshAtCursor; // root cursor -> mesh
		std::unordered_map<std::size_t, std::size_t> meshOfAsset;

		auto addMesh = [&](XModelMeshData&& mesh) {
			const std::size_t index = data->meshes.size();
			if (mesh.info.at) {
				data->infoAt.emplace(mesh.info.at, index);
			}
			if (mesh.info.residentAt) {
				data->residentAt.emplace(mesh.info.residentAt, index);
			}
			meshAtCursor.emplace(mesh.cursor, index);
			data->meshes.push_back({ std::move(mesh) });
			++stats.meshes;
			return index;
		};
		// The name of the asset a stored pointer leads to: through the asset table, or an insert slot.
		auto nameOf = [&](std::uint64_t stored) -> std::uint64_t {
			if (const std::size_t asset = AssetOfReference(trace, list, stored); asset != kNone) {
				const std::size_t at = trace.assets[asset].offset;
				return list.assets[asset].header == kPtrInline && at + 8 <= stream.size()
					? Get<std::uint64_t>(stream.subspan(at, 8), 0) & kNameMask : 0;
			}
			if (stored != kPtrNull && stored != kPtrInline && stored != kPtrInsert) {
				if (const auto it = inserts.find(stored - 1); it != inserts.end() && it->second + 8 <= stream.size()) {
					return Get<std::uint64_t>(stream.subspan(it->second, 8), 0) & kNameMask;
				}
			}
			return 0;
		};
		auto meshOf = [&](std::uint64_t stored) -> std::size_t {
			if (const std::size_t asset = AssetOfReference(trace, list, stored); asset != kNone) {
				const auto it = meshOfAsset.find(asset);
				return it == meshOfAsset.end() ? kNone : it->second;
			}
			if (stored != kPtrNull && stored != kPtrInline && stored != kPtrInsert) {
				if (const auto slot = inserts.find(stored - 1); slot != inserts.end()) {
					const auto it = meshAtCursor.find(slot->second);
					return it == meshAtCursor.end() ? kNone : it->second;
				}
			}
			return kNone;
		};

		// One pass in asset order: every reference points back, so what it names is indexed by then.
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			const std::uint64_t type = list.assets[i].type;
			if (type != 0x06 && type != 0x09) {
				continue;
			}
			const TracedAsset& start = trace.assets[i];
			XStream s(stream, 0);
			s.Restore(start.offset, start.block, start.pos);
			s.LogInserts(&inserts);
			if (type == 0x09) {
				XModelMeshData mesh;
				if (!ReadXModelMesh(s, list.assets[i].header, mesh)) {
					++stats.failed;
					continue;
				}
				meshOfAsset.emplace(i, addMesh(std::move(mesh)));
				continue;
			}

			++stats.xmodels;
			XModelData model;
			if (!ReadXModel(s, list.assets[i].header, model)) {
				++stats.failed;
				continue;
			}
			LibraryModel entry;
			entry.name = model.Name();
			entry.zone = zoneIndex;
			entry.asset = i;
			entry.mins = model.Mins();
			entry.maxs = model.Maxs();
			entry.radius = model.Radius();
			entry.attachments = model.root[222];
			entry.slots.resize(std::min<std::size_t>(model.LodCount(), 8));
			for (std::size_t slot = 0; slot < 8; ++slot) {
				// Every inline LOD joins the zone's meshes, even past the count: a later model may reference it.
				const std::size_t mesh = model.hasInline[slot] ? addMesh(std::move(model.lods[slot])) : meshOf(model.Lod(slot));
				if (slot < entry.slots.size()) {
					entry.slots[slot].mesh = mesh;
				}
			}
			for (std::size_t slot = 0; slot < entry.slots.size() && slot < model.materials.size(); ++slot) {
				for (const XModelData::Material& material : model.materials[slot]) {
					entry.slots[slot].materials.push_back(material.name ? material.name : nameOf(material.stored));
				}
			}
			if (m_ByName.contains(entry.name)) {
				continue;
			}

			if (Get<std::uint64_t>(model.root, 0) >> 63) {
				entry.problem = "a reference to another zone's model";
			}
			else {
				for (std::size_t slot = 0; slot < entry.slots.size(); ++slot) {
					LibraryLod& lod = entry.slots[slot];
					if (lod.mesh == kNone) {
						lod.problem = model.Lod(slot) == kPtrNull ? "empty slot" : "LOD not found";
					}
					else {
						CheckLod(*data, lod);
					}
					if (lod.problem.empty()) {
						entry.detail.push_back(slot);
					}
				}
				std::stable_sort(entry.detail.begin(), entry.detail.end(), [&](std::size_t a, std::size_t b) {
					return entry.slots[a].triangles > entry.slots[b].triangles;
				});
				if (entry.detail.empty()) {
					entry.problem = entry.slots.empty() ? "no LODs" : entry.slots.back().problem;
				}
			}
			++stats.indexed;
			if (entry.problem.empty()) {
				++stats.decodable;
			}
			else {
				++stats.problems[entry.problem];
			}
			m_ByName.emplace(entry.name, m_Models.size());
			m_Models.push_back(std::move(entry));
		}
		m_Zones.push_back(std::move(data));
		return true;
	}

	void ModelLibrary::CheckLod(const ZoneData& zone, LibraryLod& lod) const {
		const Mesh& mesh = zone.meshes[lod.mesh];
		for (const MeshSurface& surface : mesh.data.surfaces) {
			lod.vertices += surface.VertexCount();
			lod.triangles += surface.FaceCount();
		}
		lod.surfaces = mesh.data.surfaces.size();
		const Mesh* owner = InfoOwner(zone, mesh);
		if (!owner) {
			lod.problem = "mesh info not found";
			return;
		}
		const MeshInfo& info = owner->data.info;
		lod.streamed = info.Streamed();
		lod.skinned = info.WeightOffset() != 0;
		if (info.Streamed()) {
			const std::uint64_t key = mesh.data.StreamKey() ? mesh.data.StreamKey() : owner->data.StreamKey();
			if (m_Packages && !m_Packages->Find(key)) {
				lod.problem = "streamed mesh not in the installed packages";
			}
		}
		else if (info.resident.empty()) {
			const std::uint64_t stored = Get<std::uint64_t>(info.raw, 32);
			auto it = zone.residentAt.upper_bound(stored - 1);
			if (stored == kPtrNull || it == zone.residentAt.begin()
				|| (--it, (stored - 1) - it->first >= zone.meshes[it->second].data.info.resident.size())) {
				lod.problem = "resident buffer not found";
			}
		}
		if (lod.problem.empty() && lod.triangles == 0) {
			lod.problem = "no triangles";
		}
	}

	const ModelLibrary::Mesh* ModelLibrary::InfoOwner(const ZoneData& zone, const Mesh& mesh) const {
		if (mesh.data.hasInfo) {
			return &mesh;
		}
		const std::uint64_t stored = Get<std::uint64_t>(mesh.data.root, 16);
		if (stored == kPtrNull || stored == kPtrInline || stored == kPtrInsert) {
			return nullptr;
		}
		const auto it = zone.infoAt.find(stored - 1);
		return it == zone.infoAt.end() ? nullptr : &zone.meshes[it->second];
	}

	bool ModelLibrary::Buffer(const ZoneData& zone, const Mesh& mesh, std::vector<std::uint8_t>& out, std::string& error) {
		const Mesh* owner = InfoOwner(zone, mesh);
		if (!owner) {
			error = "mesh info not found";
			return false;
		}
		const MeshInfo& info = owner->data.info;
		if (info.Streamed()) {
			if (!m_Packages) {
				if (m_PackageDir.empty()) {
					error = "streamed mesh, and no package folder set";
					return false;
				}
				m_Packages = std::make_unique<Kapi::PackageIndex>();
				if (!m_Packages->Open(m_PackageDir, error)) {
					m_Packages.reset();
					return false;
				}
			}
			const std::uint64_t key = mesh.data.StreamKey() ? mesh.data.StreamKey() : owner->data.StreamKey();
			return m_Packages->Extract(key, out, error);
		}
		if (!info.resident.empty()) {
			out = info.resident;
			return true;
		}
		const std::uint64_t stored = Get<std::uint64_t>(info.raw, 32);
		auto it = zone.residentAt.upper_bound(stored - 1);
		if (stored == kPtrNull || it == zone.residentAt.begin()) {
			error = "resident buffer not found";
			return false;
		}
		--it;
		const std::vector<std::uint8_t>& shared = zone.meshes[it->second].data.info.resident;
		const std::uint64_t offset = (stored - 1) - it->first;
		if (offset + info.BufferSize() > shared.size()) {
			error = std::format("resident buffer reference +0x{:X} runs past the {}-byte buffer it points into", offset, shared.size());
			return false;
		}
		out.assign(shared.begin() + static_cast<std::ptrdiff_t>(offset),
			shared.begin() + static_cast<std::ptrdiff_t>(offset + info.BufferSize()));
		return true;
	}

	bool ModelLibrary::Geometry(const LibraryModel& model, std::size_t rank, ModelGeometry& out, std::string& error) {
		out = {};
		const LibraryLod* lod = model.Lod(rank);
		if (!lod) {
			error = std::format("the model has {} decodable LODs, no LOD {}", model.detail.size(), rank);
			return false;
		}
		const ZoneData& zone = *m_Zones[model.zone];
		const Mesh& mesh = zone.meshes[lod->mesh];
		const Mesh* owner = InfoOwner(zone, mesh);
		std::vector<std::uint8_t> buffer;
		if (!owner || !Buffer(zone, mesh, buffer, error)) {
			if (!owner) {
				error = "mesh info not found";
			}
			return false;
		}
		const MeshInfo& info = owner->data.info;
		const std::span<const std::uint8_t> data(buffer);
		const std::size_t stride = info.ExtendedVertices() ? 24 : 16;

		out.mins = { 1e30f, 1e30f, 1e30f };
		out.maxs = { -1e30f, -1e30f, -1e30f };
		for (std::size_t si = 0; si < mesh.data.surfaces.size(); ++si) {
			const MeshSurface& source = mesh.data.surfaces[si];
			GeometrySurface& surface = out.surfaces.emplace_back();
			if (si < lod->materials.size()) {
				surface.material = lod->materials[si];
			}
			const std::size_t first = source.FirstVertex();
			for (std::uint32_t v = 0; v < source.VertexCount(); ++v) {
				const std::size_t at = info.PositionOffset() + (first + v) * 12;
				const std::size_t vertexAt = info.VertexOffset() + (first + v) * stride;
				if (at + 12 > data.size() || vertexAt + 16 > data.size()) {
					error = std::format("surface {} vertex {} lies outside the {}-byte buffer", si, v, data.size());
					return false;
				}
				std::array<float, 3> p;
				std::memcpy(p.data(), data.data() + at, 12);
				for (int k = 0; k < 3; ++k) {
					out.mins[k] = std::min(out.mins[k], p[k]);
					out.maxs[k] = std::max(out.maxs[k], p[k]);
				}
				surface.positions.push_back(p);
				surface.uvs.push_back({ HalfToFloat(Get<std::uint16_t>(data, vertexAt + 4)),
					HalfToFloat(Get<std::uint16_t>(data, vertexAt + 6)) });
				surface.normals.push_back(UnpackUnitVector(Get<std::uint32_t>(data, vertexAt + 8)));
			}
			for (std::uint32_t f = 0; f < source.FaceCount(); ++f) {
				const std::size_t at = info.FaceOffset() + std::size_t(source.FirstIndex()) * 2 + std::size_t(f) * 6;
				if (at + 6 > data.size()) {
					error = std::format("surface {} face {} lies outside the {}-byte buffer", si, f, data.size());
					return false;
				}
				const std::array<std::uint32_t, 3> triangle = { Get<std::uint16_t>(data, at), Get<std::uint16_t>(data, at + 2),
					Get<std::uint16_t>(data, at + 4) };
				if (triangle[0] >= source.VertexCount() || triangle[1] >= source.VertexCount() || triangle[2] >= source.VertexCount()) {
					error = std::format("surface {} face {} indexes past its {} vertices", si, f, source.VertexCount());
					return false;
				}
				surface.triangles.push_back(triangle);
			}
		}
		if (out.surfaces.empty() || out.Vertices() == 0) {
			out.mins = {};
			out.maxs = {};
		}
		return true;
	}
}
