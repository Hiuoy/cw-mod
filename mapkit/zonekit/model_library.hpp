#pragma once
#include "kapi.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The models a set of zones holds, found by name and decoded to plain geometry: what mapkit's level builder
// draws in place of a game object. A zone is read through its trace (zone_trace.hpp): level zones cannot be
// walked yet, so every xmodel and xmodelmesh is decoded from its recorded start.
//
// A LOD's parts are often shared with data loaded earlier in the zone, and the library follows each kind:
//   the LOD itself      an xmodelmesh asset (a reference into the asset table), or stored inline in the xmodel
//                       (possibly behind a DB_InsertPointer slot a later model references);
//   its mesh info       inline, or a reference to another LOD's (MeshInfo::at);
//   its buffer          resident in block 12 (its own, or a reference into another LOD's: MeshInfo::residentAt),
//                       or streamed from the .xsub packages by the LOD's stream key.
// Only the decoded meshes are kept once a zone is indexed; the zone's stream is let go.
namespace MapKit::Zone {
	// One surface of a decoded LOD, in game space: inches, X forward, Y left, Z up.
	struct GeometrySurface {
		std::uint64_t material = 0; // name hash, 0 when not resolved
		std::vector<std::array<float, 3>> positions;
		std::vector<std::array<float, 3>> normals;
		std::vector<std::array<float, 2>> uvs;
		std::vector<std::array<std::uint32_t, 3>> triangles; // clockwise seen from the front, as the game stores them
	};

	struct ModelGeometry {
		std::vector<GeometrySurface> surfaces;
		std::array<float, 3> mins{};
		std::array<float, 3> maxs{};
		std::size_t Vertices() const;
		std::size_t Triangles() const;
	};

	// One of an xmodel's LOD slots (+32 + 8 * slot).
	struct LibraryLod {
		std::size_t mesh = SIZE_MAX; // SIZE_MAX when the slot is empty or its LOD was not found
		std::vector<std::uint64_t> materials; // one name per surface (0 = not resolved)
		bool streamed = false;
		bool skinned = false; // its mesh info has a weights section
		std::size_t surfaces = 0;
		std::size_t vertices = 0;
		std::size_t triangles = 0;
		std::string problem;  // why it cannot be decoded ("" = it can)
	};

	struct LibraryModel {
		std::uint64_t name = 0;
		std::size_t zone = 0;  // ModelLibrary::ZoneName index
		std::size_t asset = 0; // the xmodel's index in that zone
		std::array<float, 3> mins{}; // the model's own bounds (xmodel +184 / +196): its full-detail LOD's
		std::array<float, 3> maxs{};
		float radius = 0;
		std::uint8_t attachments = 0; // other models it carries (xmodel +222)
		std::vector<LibraryLod> slots; // xmodel +112 of them, in the order stored
		// The decodable slots, most detailed first. The slots are NOT stored in detail order: on Die Maschine the
		// last one is the full model and the first the coarsest (a table: 16 triangles in slot 0, 6504 in slot 7).
		std::vector<std::size_t> detail;
		// Why the model cannot be decoded at all ("" = it can: `detail` is not empty).
		std::string problem;

		const LibraryLod* Lod(std::size_t rank) const { return rank < detail.size() ? &slots[detail[rank]] : nullptr; }
	};

	struct ModelLibraryStats {
		std::size_t xmodels = 0;     // xmodel assets in the zone
		std::size_t indexed = 0;     // new models (not in an earlier zone)
		std::size_t decodable = 0;   // of those, with at least one decodable LOD
		std::size_t meshes = 0;      // xmodelmesh assets + LODs stored inline
		std::size_t failed = 0;      // assets whose decode failed
		std::unordered_map<std::string, std::size_t> problems; // why the rest are not decodable
	};

	class ModelLibrary {
	public:
		ModelLibrary();
		~ModelLibrary();

		// Loads <zone>.ff (+ .fd), aligns the trace and indexes every xmodel it stores. A model an earlier zone
		// already gave keeps that zone.
		bool AddZone(const std::filesystem::path& ffPath, const std::filesystem::path& tracePath, ModelLibraryStats& stats,
			std::string& error);
		// Where streamed meshes come from (<game>/zone). The packages are opened on first need.
		void SetPackageDir(std::filesystem::path zoneDir) { m_PackageDir = std::move(zoneDir); }

		const std::vector<LibraryModel>& Models() const { return m_Models; }
		const LibraryModel* Find(std::uint64_t name) const;
		const std::string& ZoneName(std::size_t zone) const;

		// Decodes one LOD of a model, by detail rank: 0 is the full model (LibraryModel::detail).
		bool Geometry(const LibraryModel& model, std::size_t rank, ModelGeometry& out, std::string& error);

	private:
		struct Mesh;
		struct ZoneData;

		// Fills a slot's counts and says whether its geometry can be found, without decoding it.
		void CheckLod(const ZoneData& zone, LibraryLod& lod) const;

		// The mesh info and resident buffer a mesh uses, following references to other meshes of its zone.
		const Mesh* InfoOwner(const ZoneData& zone, const Mesh& mesh) const;
		bool Buffer(const ZoneData& zone, const Mesh& mesh, std::vector<std::uint8_t>& out, std::string& error);

		std::vector<std::unique_ptr<ZoneData>> m_Zones;
		std::vector<LibraryModel> m_Models;
		std::unordered_map<std::uint64_t, std::size_t> m_ByName;
		std::filesystem::path m_PackageDir;
		std::unique_ptr<Kapi::PackageIndex> m_Packages;
	};
}
