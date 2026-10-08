// mkasset: the game's models for mapkit's level builder, read from your own install.
//
//   mkasset [--game <dir>] [--zone <name>]... catalog <out.json>
//   mkasset [--game <dir>] [--zone <name>]... export <model> <out.glb> [--lod <n>]
//   mkasset [--game <dir>] [--zone <name>]... serve
//
// <dir> is the game folder (default %MAPKIT_GAME_DIR%, then the current folder). Each zone is read through the
// trace the cw-mod client recorded for it, <dir>\cw-mod\mapkit\trace\<zone>.mktrace; without --zone, every zone
// that has one. <model> is the xmodel's name or its hash (16 hex digits). LODs count from the full model (0)
// down, whatever order the xmodel stores them in (LibraryModel::detail).
//
// serve keeps the zones and the package index loaded and answers one JSON request per stdin line with one JSON
// line on stdout (the level builder runs it in the background):
//   {"id": 1, "cmd": "export", "model": "<model>", "out": "<file.glb>", "lod": 0}  (lod 0 = full detail)
//   {"id": 2, "cmd": "catalog", "out": "<file.json>", "brief": true}  (brief: no per-LOD and material lists)
//   {"id": 3, "cmd": "quit"}
// It first prints {"ready": true, ...} once the zones are indexed. Every answer carries the request's id and
// "ok"; a failed one has "error".
//
// What it writes is game data: keep it in a local cache, never in the repo or anywhere shared.

#include "glb.hpp"

#include <zonekit/hash.hpp>
#include <zonekit/model_library.hpp>
#include <zonekit/oodle.hpp>
#include <zonekit/zone.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace MapKit;
using namespace MapKit::Zone;

namespace {
	struct Options {
		fs::path gameDir;
		std::vector<std::string> zones;
		std::string command;
		std::vector<std::string> args;
		std::size_t lod = 0;
	};

	void Usage() {
		std::fprintf(stderr,
			"usage: mkasset [--game <dir>] [--zone <name>]... catalog <out.json>\n"
			"       mkasset [--game <dir>] [--zone <name>]... export <model> <out.glb> [--lod <n>]\n"
			"       mkasset [--game <dir>] [--zone <name>]... serve\n"
			"\n"
			"  --game <dir>   game folder (default: %%MAPKIT_GAME_DIR%%, then the current folder)\n"
			"  --zone <name>  a zone to read, through <game>\\cw-mod\\mapkit\\trace\\<name>.mktrace (repeatable;\n"
			"                 default: every zone with a trace)\n"
			"  <model>        an xmodel's name or hash (16 hex digits)\n"
			"\n"
			"The output is game data: keep it local.\n");
	}

	bool ParseArgs(int argc, char** argv, Options& options) {
		for (int i = 1; i < argc; ++i) {
			const std::string arg = argv[i];
			const bool hasValue = i + 1 < argc;
			if (arg == "--game" && hasValue) {
				options.gameDir = argv[++i];
			}
			else if (arg == "--zone" && hasValue) {
				options.zones.push_back(argv[++i]);
			}
			else if (arg == "--lod" && hasValue) {
				options.lod = std::stoul(argv[++i]);
			}
			else if (arg.starts_with("--")) {
				return false;
			}
			else if (options.command.empty()) {
				options.command = arg;
			}
			else {
				options.args.push_back(arg);
			}
		}
		if (options.gameDir.empty()) {
			wchar_t* env = nullptr;
			std::size_t length = 0;
			if (_wdupenv_s(&env, &length, L"MAPKIT_GAME_DIR") == 0 && env) {
				options.gameDir = env;
				std::free(env);
			}
			else {
				options.gameDir = fs::current_path();
			}
		}
		const std::size_t needed = options.command == "catalog" ? 1 : options.command == "export" ? 2 : options.command == "serve" ? 0 : ~0u;
		return options.args.size() == needed;
	}

	fs::path TraceDir(const Options& options) {
		return options.gameDir / "cw-mod" / "mapkit" / "trace";
	}

	std::string Hex(std::uint64_t value) {
		return std::format("{:016X}", value);
	}

	// A name or 16 hex digits (optionally # or 0x first).
	std::uint64_t ModelKey(std::string text) {
		if (text.starts_with('#')) {
			text.erase(0, 1);
		}
		else if (text.starts_with("0x") || text.starts_with("0X")) {
			text.erase(0, 2);
		}
		if (text.size() == 16 && text.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
			return std::stoull(text, nullptr, 16) & ~(1ull << 63);
		}
		return HashName(text);
	}

	json Vec3(const std::array<float, 3>& v) {
		return json::array({ v[0], v[1], v[2] });
	}

	bool OpenLibrary(const Options& options, ModelLibrary& library, json& zonesOut) {
		std::vector<std::string> zones = options.zones;
		if (zones.empty()) {
			std::error_code ec;
			for (const auto& entry : fs::directory_iterator(TraceDir(options), ec)) {
				if (entry.path().extension() == ".mktrace") {
					zones.push_back(entry.path().stem().string());
				}
			}
			if (zones.empty()) {
				std::fprintf(stderr, "no zone traces in %s: record one with cw-mod.json \"mapkit_trace\", or pass --zone\n",
					TraceDir(options).string().c_str());
				return false;
			}
		}
		library.SetPackageDir(options.gameDir / "zone");
		zonesOut = json::array();
		for (const std::string& zone : zones) {
			const auto start = std::chrono::steady_clock::now();
			ModelLibraryStats stats;
			std::string error;
			if (!library.AddZone(options.gameDir / "zone" / (zone + ".ff"), TraceDir(options) / (zone + ".mktrace"), stats, error)) {
				std::fprintf(stderr, "%s: %s\n", zone.c_str(), error.c_str());
				return false;
			}
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			std::fprintf(stderr, "%s: %zu xmodels, %zu new, %zu decodable; %zu meshes, %zu failed decodes (%lld ms)\n", zone.c_str(),
				stats.xmodels, stats.indexed, stats.decodable, stats.meshes, stats.failed, static_cast<long long>(ms));
			for (const auto& [problem, count] : stats.problems) {
				std::fprintf(stderr, "  %6zu  %s\n", count, problem.c_str());
			}
			zonesOut.push_back(zone);
		}
		return true;
	}

	json Catalog(const ModelLibrary& library, const json& zones, bool brief) {
		json models = json::array();
		for (const LibraryModel& model : library.Models()) {
			if (!model.problem.empty()) {
				continue;
			}
			json entry = {
				{ "model", Hex(model.name) },
				{ "zone", library.ZoneName(model.zone) },
				{ "mins", Vec3(model.mins) },
				{ "maxs", Vec3(model.maxs) },
				{ "triangles", model.Lod(0)->triangles },
			};
			if (brief) {
				models.push_back(std::move(entry));
				continue;
			}
			// The LODs most detailed first: "lod" in an export request is an index into this list.
			json lods = json::array();
			for (const std::size_t slot : model.detail) {
				const LibraryLod& lod = model.slots[slot];
				lods.push_back({ { "slot", slot }, { "surfaces", lod.surfaces }, { "vertices", lod.vertices },
					{ "triangles", lod.triangles }, { "streamed", lod.streamed }, { "skinned", lod.skinned } });
			}
			json materials = json::array();
			for (const std::uint64_t material : model.Lod(0)->materials) {
				materials.push_back(Hex(material));
			}
			entry["radius"] = model.radius;
			entry["attachments"] = model.attachments;
			entry["lods"] = lods;
			entry["materials"] = materials;
			models.push_back(std::move(entry));
		}
		json catalog = json::object();
		catalog["format"] = 1;
		catalog["zones"] = zones;
		catalog["models"] = models;
		return catalog;
	}

	bool WriteText(const fs::path& path, const std::string& text, std::string& error) {
		std::error_code ec;
		if (path.has_parent_path()) {
			fs::create_directories(path.parent_path(), ec);
		}
		if (!WriteWholeFile(path, std::vector<std::uint8_t>(text.begin(), text.end()))) {
			error = "could not write " + path.string();
			return false;
		}
		return true;
	}

	// One model LOD to a .glb. Fills `result` with what was written.
	bool Export(ModelLibrary& library, const std::string& modelText, std::size_t lod, const fs::path& out, json& result,
		std::string& error) {
		const LibraryModel* model = library.Find(ModelKey(modelText));
		if (!model) {
			error = std::format("no model {} in the loaded zones", modelText);
			return false;
		}
		if (!model->problem.empty()) {
			error = std::format("model {} cannot be decoded: {}", Hex(model->name), model->problem);
			return false;
		}
		ModelGeometry geometry;
		if (!library.Geometry(*model, lod, geometry, error)) {
			error = std::format("model {} LOD {}: {}", Hex(model->name), lod, error);
			return false;
		}
		const std::vector<std::uint8_t> glb = EncodeGlb(geometry, Hex(model->name));
		std::error_code ec;
		if (out.has_parent_path()) {
			fs::create_directories(out.parent_path(), ec);
		}
		if (!WriteWholeFile(out, glb)) {
			error = "could not write " + out.string();
			return false;
		}
		result["model"] = Hex(model->name);
		result["zone"] = library.ZoneName(model->zone);
		result["lod"] = lod;
		result["slot"] = model->detail[lod];
		result["surfaces"] = geometry.surfaces.size();
		result["vertices"] = geometry.Vertices();
		result["triangles"] = geometry.Triangles();
		result["mins"] = Vec3(geometry.mins);
		result["maxs"] = Vec3(geometry.maxs);
		result["winding"] = WindingAgreement(geometry);
		result["bytes"] = glb.size();
		return true;
	}

	int Serve(ModelLibrary& library, const json& zones) {
		std::size_t decodable = 0;
		for (const LibraryModel& model : library.Models()) {
			decodable += model.problem.empty();
		}
		auto reply = [](const json& message) {
			std::fputs((message.dump() + "\n").c_str(), stdout);
			std::fflush(stdout);
		};
		reply({ { "ready", true }, { "zones", zones }, { "models", decodable } });

		std::string line;
		while (std::getline(std::cin, line)) {
			if (line.find_first_not_of(" \t\r") == std::string::npos) {
				continue;
			}
			json answer = json::object();
			json request;
			try {
				request = json::parse(line);
			}
			catch (const std::exception& e) {
				answer["ok"] = false;
				answer["error"] = std::string("bad request: ") + e.what();
				reply(answer);
				continue;
			}
			answer["id"] = request.value("id", json());
			const std::string cmd = request.value("cmd", "");
			std::string error;
			bool ok = false;
			if (cmd == "quit") {
				answer["ok"] = true;
				reply(answer);
				return 0;
			}
			else if (cmd == "export") {
				ok = Export(library, request.value("model", ""), request.value("lod", 0u), fs::path(request.value("out", "")), answer, error);
			}
			else if (cmd == "catalog") {
				const json catalog = Catalog(library, zones, request.value("brief", false));
				ok = WriteText(fs::path(request.value("out", "")), catalog.dump(1), error);
				answer["models"] = catalog["models"].size();
			}
			else {
				error = "unknown cmd \"" + cmd + "\"";
			}
			answer["ok"] = ok;
			if (!ok) {
				answer["error"] = error;
			}
			reply(answer);
		}
		return 0;
	}
}

int main(int argc, char** argv) {
	Options options;
	if (!ParseArgs(argc, argv, options)) {
		Usage();
		return 2;
	}
	std::string error;
	if (!Oodle::Load(options.gameDir, error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	ModelLibrary library;
	json zones;
	if (!OpenLibrary(options, library, zones)) {
		return 1;
	}

	if (options.command == "serve") {
		return Serve(library, zones);
	}
	if (options.command == "catalog") {
		const json catalog = Catalog(library, zones, false);
		if (!WriteText(options.args[0], catalog.dump(1), error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		std::printf("%zu models -> %s\n", catalog["models"].size(), options.args[0].c_str());
		return 0;
	}
	json result = json::object();
	if (!Export(library, options.args[0], options.lod, options.args[1], result, error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	std::printf("%s\n", result.dump().c_str());
	return 0;
}
