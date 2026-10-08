#pragma once
#include <zonekit/model_library.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace MapKit {
	// A decoded model as glTF 2.0 binary (.glb), in glTF's space, which is also Godot's: Y up, -Z forward,
	// meters, counter-clockwise front faces (mapkit/godot/addons/mapkit/mk_space.gd has the game-side mapping).
	// One primitive per surface, one plain material per game material, named by its hash.
	std::vector<std::uint8_t> EncodeGlb(const Zone::ModelGeometry& geometry, const std::string& name);

	// Of the triangles, the share whose game winding (clockwise from the front) agrees with their vertex normals:
	// near 1 when the winding reading is right.
	double WindingAgreement(const Zone::ModelGeometry& geometry);
}
