#pragma once
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// The .mkmap map source (mapkit/mkmap-format.md, format 1), as cwlink reads it. Game space: inches, X forward,
// Y left, Z up, relative to the map's origin.
namespace MapKit {
	struct MkBrush {
		std::string path;
		std::array<float, 3> center{};
		std::array<std::array<float, 3>, 3> halfAxes{};
		std::string material;
		bool solid = true;
		bool rendered = true;
	};

	struct MkBounds {
		std::array<float, 3> min{};
		std::array<float, 3> max{};
	};

	struct MkEntity {
		std::string path;
		std::string cls;
		std::array<float, 3> origin{};
		std::array<float, 3> angles{}; // pitch (down +), yaw, roll
		nlohmann::json props = nlohmann::json::object();
		std::optional<MkBounds> bounds; // volumes: in the entity's own axes, relative to origin

		// A prop as text ("" when missing); numbers print without a fraction when whole.
		std::string Prop(const char* name) const;
	};

	// A creator's own mesh (MkMesh): parts (one per mesh under the node), each of triangle surfaces in map space.
	struct MkMeshSurface {
		std::string material; // the editor material's name: a game material when it names one
		// The editor material's own look, when it is no game material: its colour texture (or a swatch of its
		// colour), a .dds path relative to the .mkmap. Empty: none exported.
		std::string colorTexture;
		// With it: its normal map in the game's convention (x, y in r, g with y down the image; b the roughness the
		// normals' spread adds), its roughness map (r) and its metal map (r, metalness). Empty: a flat normal map,
		// full roughness, not metal.
		std::string normalTexture;
		std::string roughnessTexture;
		std::string metalTexture;
		// How the colour texture's alpha draws: "opaque" (ignored), "clip" (cut out: the editor has already made it
		// 0 or 1) or "blend" (see-through).
		std::string alpha = "opaque";
		// With it: the light the surface gives off (its glow colour, sRGB, a .dds like the colour texture) and how
		// strong (Godot's emission energy: 1 = as bright as Die Maschine's lit signs). Empty: it doesn't glow.
		std::string emissionTexture;
		float emissionEnergy = 1.0f;
		std::vector<std::array<float, 3>> vertices;
		std::vector<std::array<float, 3>> normals; // empty or one per vertex
		std::vector<std::array<float, 2>> uvs;     // empty or one per vertex
		std::vector<std::array<std::uint32_t, 3>> triangles; // clockwise seen from the front
	};

	struct MkMeshPart {
		std::string path;
		std::vector<MkMeshSurface> surfaces;
	};

	struct MkMesh {
		std::string path;
		std::string collision = "faces"; // faces, hull or none
		std::string gameMaterial;        // for surfaces whose material names no game material
		std::vector<MkMeshPart> parts;
	};

	// The map's own sky (a WorldEnvironment's sky in the editor): an equirectangular .dds (2:1, the top row straight up,
	// the middle row the horizon) already turned to the game's directions, and how bright (1 = the pixel values as
	// bright as Die Maschine's sky's). Empty image: Die Maschine's sky.
	struct MkSky {
		std::string image;
		float energy = 1.0f;
	};

	// The sun (a DirectionalLight3D in the editor): the direction its light travels (map axes, unit length), its colour
	// (linear) and how strong (1 = Die Maschine's daytime sun).
	struct MkSun {
		std::array<float, 3> direction{ 0.0f, 0.0f, -1.0f };
		std::array<float, 3> color{ 1.0f, 1.0f, 1.0f };
		float energy = 1.0f;
	};

	// The fog (a WorldEnvironment's fog in the editor), as the game's world fog: it starts `start` units away and
	// covers half the view `halfway` units further, thins with height above `baseHeight` (map space) by half every
	// `halfwayHeight` units (0: the same at every height), in `color` (0..1, as displayed) up to `opacity`.
	struct MkFog {
		bool enabled = false;
		float start = 0.0f;
		float halfway = 2000.0f;
		float baseHeight = 0.0f;
		float halfwayHeight = 0.0f;
		std::array<float, 3> color{ 0.5f, 0.5f, 0.5f };
		float opacity = 1.0f;
	};

	// The map's own lighting over the base map's (its daytime state): what the map sets, the rest the base map's.
	struct MkLighting {
		std::optional<MkSun> sun;
		std::optional<MkFog> fog;
	};

	struct MkMap {
		std::string name;
		std::string title;
		std::string author;
		std::string mode;
		std::vector<MkBrush> brushes;
		std::vector<MkEntity> entities;
		std::vector<MkMesh> meshes;
		MkSky sky;
		std::optional<MkLighting> lighting; // none: the base map's lighting as it is
	};

	// False (with a reason) for a file that is not a format-1 .mkmap.
	bool ReadMkMap(const std::filesystem::path& path, MkMap& out, std::string& error);
}
