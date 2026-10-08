#include "mkmap.hpp"

#include <cmath>
#include <format>
#include <fstream>

namespace MapKit {
	namespace {
		using json = nlohmann::json;

		std::array<float, 3> Vector(const json& value, const std::string& where) {
			if (!value.is_array() || value.size() != 3) {
				throw std::runtime_error(where + ": expected [x, y, z]");
			}
			return { value[0].get<float>(), value[1].get<float>(), value[2].get<float>() };
		}
	}

	std::string MkEntity::Prop(const char* name) const {
		const auto it = props.find(name);
		if (it == props.end() || it->is_null()) {
			return {};
		}
		if (it->is_string()) {
			return it->get<std::string>();
		}
		if (it->is_number()) {
			const double number = it->get<double>();
			return number == std::floor(number) ? std::format("{}", static_cast<long long>(number)) : std::format("{}", number);
		}
		return it->dump();
	}

	bool ReadMkMap(const std::filesystem::path& path, MkMap& out, std::string& error) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			error = std::format("cannot open {}", path.string());
			return false;
		}
		try {
			const json root = json::parse(file);
			if (root.value("mkmap", 0) != 1) {
				error = std::format("{}: not a format-1 .mkmap (\"mkmap\": {})", path.string(), root.value("mkmap", 0));
				return false;
			}
			if (root.value("units", "inch") != "inch") {
				error = std::format("{}: units are \"{}\", cwlink reads inches", path.string(), root.value("units", ""));
				return false;
			}
			out = MkMap{};
			const json& map = root.at("map");
			out.name = map.value("name", "");
			out.title = map.value("title", "");
			out.author = map.value("author", "");
			out.mode = map.value("mode", "zm");
			if (root.contains("sky") && root.at("sky").is_object()) {
				out.sky.image = root.at("sky").value("image", "");
				out.sky.energy = root.at("sky").value("energy", 1.0f);
			}
			if (root.contains("lighting") && root.at("lighting").is_object()) {
				const json& lighting = root.at("lighting");
				MkLighting& own = out.lighting.emplace();
				if (lighting.contains("sun") && lighting.at("sun").is_object()) {
					const json& sun = lighting.at("sun");
					MkSun& s = own.sun.emplace();
					s.direction = Vector(sun.at("direction"), "lighting sun direction");
					if (sun.contains("color")) {
						s.color = Vector(sun.at("color"), "lighting sun color");
					}
					s.energy = sun.value("energy", 1.0f);
				}
				if (lighting.contains("fog") && lighting.at("fog").is_object()) {
					const json& fog = lighting.at("fog");
					MkFog& f = own.fog.emplace();
					f.enabled = fog.value("enabled", false);
					f.start = fog.value("start", 0.0f);
					f.halfway = fog.value("halfway", 2000.0f);
					f.baseHeight = fog.value("base_height", 0.0f);
					f.halfwayHeight = fog.value("halfway_height", 0.0f);
					if (fog.contains("color")) {
						f.color = Vector(fog.at("color"), "lighting fog color");
					}
					f.opacity = fog.value("opacity", 1.0f);
				}
			}
			for (const json& item : root.value("brushes", json::array())) {
				MkBrush& brush = out.brushes.emplace_back();
				brush.path = item.value("path", "");
				const std::string where = "brush " + brush.path;
				brush.center = Vector(item.at("center"), where + " center");
				const json& axes = item.at("half_axes");
				if (!axes.is_array() || axes.size() != 3) {
					throw std::runtime_error(where + ": half_axes wants three vectors");
				}
				for (int i = 0; i < 3; ++i) {
					brush.halfAxes[i] = Vector(axes[i], where + " half_axes");
				}
				brush.material = item.value("material", "");
				brush.solid = item.value("solid", true);
				brush.rendered = item.value("rendered", true);
			}
			for (const json& item : root.value("entities", json::array())) {
				MkEntity& entity = out.entities.emplace_back();
				entity.path = item.value("path", "");
				entity.cls = item.at("class").get<std::string>();
				const std::string where = "entity " + entity.path;
				entity.origin = Vector(item.at("origin"), where + " origin");
				if (item.contains("angles")) {
					entity.angles = Vector(item.at("angles"), where + " angles");
				}
				if (item.contains("props") && item.at("props").is_object()) {
					entity.props = item.at("props");
				}
				if (item.contains("bounds")) {
					entity.bounds = MkBounds{ Vector(item.at("bounds").at("min"), where + " bounds"),
						Vector(item.at("bounds").at("max"), where + " bounds") };
				}
			}
			for (const json& item : root.value("meshes", json::array())) {
				MkMesh& mesh = out.meshes.emplace_back();
				mesh.path = item.value("path", "");
				mesh.collision = item.value("collision", "faces");
				mesh.gameMaterial = item.value("game_material", "");
				for (const json& partItem : item.value("parts", json::array())) {
					MkMeshPart& part = mesh.parts.emplace_back();
					part.path = partItem.value("path", "");
					const std::string where = "mesh " + part.path;
					for (const json& surfaceItem : partItem.value("surfaces", json::array())) {
						MkMeshSurface& surface = part.surfaces.emplace_back();
						surface.material = surfaceItem.value("material", "");
						surface.colorTexture = surfaceItem.value("color_texture", "");
						surface.normalTexture = surfaceItem.value("normal_texture", "");
						surface.roughnessTexture = surfaceItem.value("roughness_texture", "");
						surface.metalTexture = surfaceItem.value("metal_texture", "");
						surface.alpha = surfaceItem.value("alpha", "opaque");
						if (surface.alpha != "opaque" && surface.alpha != "clip" && surface.alpha != "blend") {
							throw std::runtime_error(where + ": alpha must be opaque, clip or blend, not " + surface.alpha);
						}
						surface.emissionTexture = surfaceItem.value("emission_texture", "");
						surface.emissionEnergy = surfaceItem.value("emission_energy", 1.0f);
						for (const json& v : surfaceItem.at("vertices")) {
							surface.vertices.push_back(Vector(v, where + " vertices"));
						}
						for (const json& v : surfaceItem.value("normals", json::array())) {
							surface.normals.push_back(Vector(v, where + " normals"));
						}
						for (const json& v : surfaceItem.value("uvs", json::array())) {
							if (!v.is_array() || v.size() != 2) {
								throw std::runtime_error(where + ": expected [u, v]");
							}
							surface.uvs.push_back({ v[0].get<float>(), v[1].get<float>() });
						}
						for (const json& t : surfaceItem.at("triangles")) {
							if (!t.is_array() || t.size() != 3) {
								throw std::runtime_error(where + ": expected [a, b, c]");
							}
							const std::array<std::uint32_t, 3> triangle = { t[0].get<std::uint32_t>(), t[1].get<std::uint32_t>(),
								t[2].get<std::uint32_t>() };
							for (const std::uint32_t index : triangle) {
								if (index >= surface.vertices.size()) {
									throw std::runtime_error(std::format("{}: triangle index {} past its {} vertices", where, index,
										surface.vertices.size()));
								}
							}
							surface.triangles.push_back(triangle);
						}
						if ((!surface.normals.empty() && surface.normals.size() != surface.vertices.size())
							|| (!surface.uvs.empty() && surface.uvs.size() != surface.vertices.size())) {
							throw std::runtime_error(where + ": normals and uvs need one per vertex");
						}
					}
				}
			}
		}
		catch (const std::exception& e) {
			error = std::format("{}: {}", path.string(), e.what());
			return false;
		}
		return true;
	}
}
