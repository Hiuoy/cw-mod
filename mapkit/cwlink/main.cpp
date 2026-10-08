// cwlink: builds map zone sets for cw-mod/maps.
//
//   cwlink [--game <dir>] [--out <dir>] clone <map> <newmap>
//       Every zone of <map> (the map, techset_, ww_, 1080_, 4k_, ww_1080_, ww_4k_ and each language),
//       patched to the current build, renamed to <newmap> and repacked without a .fd, into
//       <game>\cw-mod\maps\<newmap>\. Proves a map loads under a name the game has never seen.
//   cwlink [--game <dir>] [--out <dir>] replace <map> [--move <key>=<value>@<x>,<y>,<z>[,<yaw>]]...
//       <map>.ff alone, patched and repacked, into <game>\cw-mod\maps\<map>\. The game then loads it
//       in place of the retail zone. Proves our writer and the client loader, nothing else.
//       --move first moves every entity whose String key <key> equals <value> (map_entities.hpp): the
//       proof that the entity data mapkit decodes is what the game plays.
//   cwlink [--game <dir>] [--out <dir>] patch <map> --trace <zone.mktrace> [--move ...]...
//       The override zones ww_1080_<map>.ff and ww_4k_<map>.ff into <game>\cw-mod\maps\<map>\: each is
//       the retail variant's own assets plus mapkit's replacements, written from scratch (zone_writer.hpp).
//       The game loads them with the map at a higher zone priority, so their assets replace the map's own
//       of the same name. For now the replacement is the map's entity list, re-encoded from the traced
//       retail one with the --move edits applied: the proof that a zone mapkit writes loads and overrides.
//   cwlink [--game <dir>] [--out <dir>] plane <map> --trace <zone.mktrace> [--floor <z>] [--tiles <n>]
//          [--tile <xmodel hash>] [--move ...]...
//       The plane test map in the same override zones: <map>'s world replaced by an empty streamerworld,
//       its collision by the map's trigger trees plus a flat terrain at --floor, and a floor of n x n
//       --tile models (retail ones, by name) around the player spawns. The entities and scripts stay.
//       The renderer's world is mapkit's own gfx_map: <map>'s, copied with its model groups (the buildings and
//       props), group LOD models and terrain left out, and its remaining model list pointed at tag_origin.
//       --keep-terrain keeps <map>'s terrain in the gfx_map; --keep-decals keeps its projected decals; --retail-gfx
//       writes no gfx_map (<map>'s own stays).
//   cwlink [--game <dir>] [--out <dir>] build <map> <source.mkmap> --trace <zone.mktrace> [--at <x>,<y>,<z>]
//          [--floor <z>] [--overlay]
//       A map source (mkmap-format.md, what the Godot plugin exports) built into a map of its own: one zone
//       <id>.ff, whose level scripts are scripts/<p>/<id>.gsc/.csc, so it starts under its own name and every PC in
//       the lobby loads it by that name. <map>'s zones load under it only as its asset library (the client adds
//       them, mapkit_loader.hpp); an empty stand-in replaces <map>'s level script. Its world assets keep <map>'s
//       names and replace <map>'s world, because the engine keeps one world loaded (mapkit_loader.hpp 7.).
//       --overlay writes the old form instead: the same assets in override zones ww_1080_<id>.ff/ww_4k_<id>.ff,
//       laid over <map> on the PC that picks it.
//       What it holds: the plane's empty world, the source's solid brushes as world collision (clip map tree 0), its
//       rendered brushes as one static model of mapkit's own (model_writer.hpp), and <map>'s player spawns, perk
//       machines, Mystery Box locations, wall buys and Arsenal moved to the source's; the ones it does not place
//       are taken out, and <map>'s props (script models) go under the map. The source's origin lands at --at
//       (default: <map>'s player spawns), the terrain under its lowest brush (or at --floor). The same gfx_map
//       as plane (and the same --keep-terrain / --retail-gfx).
//       The world collision also holds a hull round each solid prop, perk machine and Mystery Box location, and a
//       player clip at each barrier (ModelHulls, BarrierClips): a retail map blocks at these with clip in its own
//       world. --no-object-clip leaves out the machines', boxes' and barriers'.
//       --draw-test adds the draw test row (docs/mapkit-plan.md P0): a retail door of <map> and three copies of it
//       written by mapkit's model writer in front of player 1's spawn, and two mapkit cubes behind it (AddDrawTestRow).
//       Which of them draw tells where mapkit's own models stop drawing.
//       It also ships the map's level scripts (WriteLevelScripts): mapkit\level\<map>\*.gsc/.csc compiled by ACTS
//       into <out>\scripts\, where the client serves them in place of <map>'s own level scripts (--level <dir>
//       picks another folder, --no-level-scripts ships none).
//
// Both copy the player's own game data into their own game folder, for testing on that PC. The
// result is Activision's content: it must never be shared or committed. Custom maps built from
// the player's own work are what gets shared.

#include <zonekit/fastfile.hpp>
#include <zonekit/gfx_world.hpp>
#include <zonekit/map_entities.hpp>
#include <zonekit/material_writer.hpp>
#include <zonekit/model_assets.hpp>
#include <zonekit/model_library.hpp>
#include <zonekit/model_writer.hpp>
#include <zonekit/oodle.hpp>
#include <zonekit/asset_loaders.hpp>
#include <zonekit/asset_walk.hpp>
#include <zonekit/hash.hpp>
#include <zonekit/havok_tagfile.hpp>
#include <zonekit/kapi.hpp>
#include <zonekit/level_assets.hpp>
#include <zonekit/library_assets.hpp>
#include <zonekit/navmesh.hpp>
#include <zonekit/rename.hpp>
#include <zonekit/xasset_list.hpp>
#include <zonekit/zone.hpp>
#include <zonekit/zone_trace.hpp>
#include <zonekit/world_assets.hpp>
#include <zonekit/world_writer.hpp>
#include <zonekit/zone_writer.hpp>

#include "mkmap.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <optional>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fs = std::filesystem;
using namespace MapKit::Zone;

namespace {
	struct Move {
		std::string key;
		std::string value;
		std::array<float, 3> origin{};
		std::optional<float> yaw;
	};

	struct Options {
		fs::path gameDir;
		fs::path outDir;
		fs::path trace;
		std::vector<std::string> args;            // lowercased
		std::vector<std::string> rawArgs;         // as given (paths)
		std::vector<Move> moves;
		std::optional<float> floor;               // plane: the floor height (default 0); build: the terrain height
		std::optional<std::array<float, 3>> at;   // build: where the source's origin lands
		int tiles = 7;                            // plane: tiles per side (odd)
		std::uint64_t tile = 0x6845F77F66511D3Dull; // plane: *zm_silver_floor.map_..._16, 476 x 476 x 1.5
		bool keepTerrain = false;                 // plane, build: keep <map>'s terrain in the gfx_map
		bool retailGfx = false;                   // plane, build: write no gfx_map
		bool keepDecals = false;                  // plane, build: keep <map>'s projected decals in the gfx_map
		bool libraryWorld = false;                // build: leave the level's other world assets to <map> (AddLevelWorldAssets)
		bool linkLibrary = false;                 // build: leave what the map's assets link to <map>, by name (AddLibraryCopies)
		bool withLibrary = false;                 // build: <map>'s whole zone set loads under the map (stage 1 to P6 2b-1)
		bool drawTest = false;                    // build: the draw test row in front of player 1
		fs::path levelDir;                        // build: the level scripts (default: mapkit/level/<map> above cwlink.exe)
		bool levelScripts = true;                 // build: compile and ship them
		std::string as;                           // build: the custom map's id (default: the source's name)
		bool navmesh = true;                      // build: the map's own navmesh (else <map>'s)
		bool acoustics = true;                    // build: the copied sound bank keeps <map>'s Triton acoustics
		bool keepLights = false;                  // build: <map>'s lights stay lit (the lighting's and the com_map's lists)
		bool keepBakedShadows = false;            // build: <map>'s baked shadow trees stay (the lighting's sun shadows and regions)
		bool objectClip = true;                   // build: perk machines, box locations and barriers block (ModelHulls, BarrierClips)
		bool overlay = false;                     // build: the old output, override zones laid over <map> (WriteOverrideZones)
	};

	// <key>=<value>@<x>,<y>,<z>[,<yaw>]
	bool ParseMove(const std::string& text, Move& move) {
		const auto equals = text.find('=');
		const auto at = text.rfind('@');
		if (equals == std::string::npos || at == std::string::npos || at < equals) {
			return false;
		}
		move.key = text.substr(0, equals);
		move.value = text.substr(equals + 1, at - equals - 1);
		std::vector<float> numbers;
		std::size_t start = at + 1;
		while (numbers.size() < 4) {
			const auto comma = text.find(',', start);
			try {
				numbers.push_back(std::stof(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
			}
			catch (const std::exception&) {
				return false;
			}
			if (comma == std::string::npos) {
				break;
			}
			start = comma + 1;
		}
		if (numbers.size() < 3 || move.key.empty() || move.value.empty()) {
			return false;
		}
		move.origin = { numbers[0], numbers[1], numbers[2] };
		if (numbers.size() == 4) {
			move.yaw = numbers[3];
		}
		return true;
	}

	void Usage() {
		std::puts(
			"usage: cwlink [--game <dir>] [--out <dir>] clone <map> <newmap>\n"
			"       cwlink [--game <dir>] [--out <dir>] replace <map> [--move <key>=<value>@<x>,<y>,<z>[,<yaw>]]...\n"
			"       cwlink [--game <dir>] [--out <dir>] patch <map> --trace <zone.mktrace> [--move ...]...\n"
			"       cwlink [--game <dir>] [--out <dir>] plane <map> --trace <zone.mktrace> [--floor <z>] [--tiles <n>]\n"
			"              [--tile <xmodel hash>] [--move ...]...\n"
			"       cwlink [--game <dir>] [--out <dir>] build <map> <source.mkmap> --trace <zone.mktrace> [--as <id>]\n"
			"              [--at <x>,<y>,<z>] [--floor <z>] [--keep-terrain] [--keep-decals] [--retail-gfx] [--draw-test]\n"
			"              [--level <dir> | --no-level-scripts] [--no-navmesh] [--no-acoustics] [--keep-lights]\n"
			"              [--keep-baked-shadows] [--no-object-clip] [--with-library [--library-world] [--link-library]]\n"
			"              [--overlay]\n"
			"\n"
			"  clone    every zone of <map>, renamed to <newmap>, into <game>\\cw-mod\\maps\\<newmap>\\\n"
			"  replace  <map>.ff repacked without its .fd, into <game>\\cw-mod\\maps\\<map>\\\n"
			"  patch    the override zones ww_1080_<map> and ww_4k_<map>: the map's entity list re-encoded\n"
			"           from the trace, --move edits applied, replacing the map's own at load\n"
			"  plane    the plane test map in the same override zones: no world, a flat floor at --floor\n"
			"           (default 0), n x n --tile models (default 7, *zm_silver_floor) at the spawns\n"
			"  build    a .mkmap (the Godot plugin's export) laid over <map> in the same override zones: the\n"
			"           plane's empty world, the source's solid brushes as collision and its rendered ones as a\n"
			"           model, <map>'s spawns, perks, box locations, wall buys and Arsenal moved to the source's\n"
			"           (the rest taken out, its props put under the map). --at places the source's origin (default: <map>'s\n"
			"           player spawns); the terrain goes under the lowest brush unless --floor says otherwise.\n"
			"           Written as a map of its own, <game>\\cw-mod\\maps\\<id>\\ (<id>.ff, scripts\\, map.json): it loads\n"
			"           under its own name <id>, and Zombies > Private lists it in CUSTOM MAPS. <id> is --as, else the\n"
			"           source's name. Of <map>'s zones only techset_<map> loads under it (P6 step 2b-2): the zone holds\n"
			"           what it takes from <map>, and its package list opens the streamed data. --with-library loads\n"
			"           <map>'s whole zone set under it as its asset library, as before.\n"
			"           --overlay writes the old form instead (ww_1080_<id>.ff, ww_4k_<id>.ff laid over <map>, live\n"
			"           only on the PC that picked it)\n"
			"  plane and build also write mapkit's own gfx_map (the renderer's world): <map>'s without its\n"
			"           buildings, props, group LODs, terrain and decals. --keep-terrain keeps the terrain,\n"
			"           --keep-decals the decals, --retail-gfx writes no gfx_map (<map>'s own draws).\n"
			"  build also writes the level's other world assets as the map's own (docs\\mapkit-plan.md, P6 step 2):\n"
			"           <map>'s terraingfx without its tiles (whole with --keep-terrain), an empty list of placed\n"
			"           effects, and copies of its glass, primary lights, occlusion data, path nodes and navvolume;\n"
			"           and a copy of its lighting even when the source sets no sun or fog. --library-world (with\n"
			"           --with-library) leaves them to <map>, as before\n"
			"  build also copies what the map's assets link from <map>'s zone (P6 step 2b): the lighting's images,\n"
			"           streamkeys and sky domes, and whatever else of <map>'s they link, under <map>'s names.\n"
			"           --link-library (with --with-library) leaves them to <map>, linked by name, as before.\n"
			"           Without --with-library it also copies <map>'s sound bank: its rooms (MkAmbientRoom), ambience\n"
			"           and level sounds, and its Triton acoustics\n"
			"  --no-acoustics  (build) the copied sound bank leaves out <map>'s Triton acoustics, which are baked from\n"
			"           <map>'s own walls: reverb and occlusion then come from the rooms alone\n"
			"  build writes the copied lighting without <map>'s level in it (docs\\mapkit-plan.md, \"The map's own\n"
			"           lighting\"): every light of <map>'s black, in the lighting and in the com_map, and each baked\n"
			"           shadow tree (the sun's shadow of <map>'s own buildings, and its shadow regions) an empty one.\n"
			"           --keep-lights leaves <map>'s lamps lit where <map> has them, --keep-baked-shadows its trees.\n"
			"           With --with-library the trees always stay: their stream keys are <map>'s then\n"
			"  build makes the perk machines, the Mystery Box locations and the barriers block: a retail map does that\n"
			"           with clip in its own world, which a map of yours has none of. Each machine and box location\n"
			"           gets a hull around its model, and each barrier a player clip over its boards (players stop,\n"
			"           bullets and zombies pass). The navmesh goes round them. --no-object-clip leaves them out\n"
			"  --draw-test  (build) test objects around player 1: a retail door and three copies mapkit writes\n"
			"           in front, two mapkit cubes behind\n"
			"  --level  (build) the level scripts shipped with the map (default: mapkit\\level\\<map> of the repo\n"
			"           cwlink.exe was built in). Each .gsc/.csc is compiled by ACTS (acts on PATH) under the stock\n"
			"           name scripts\\<mode>\\<file> into <out>\\scripts\\, where the client serves it in place of\n"
			"           <map>'s own. --no-level-scripts ships none: <map>'s own level scripts run\n"
			"  --no-navmesh  (build) keep <map>'s navmesh instead of the map's own: the walkable polygons over the\n"
			"           map's collision (Recast), in <map>'s navmesh files; its polygons go to generated\\navmesh.obj\n"
			"  --move   (replace, patch) first move every entity whose key <key> is <value>, e.g.\n"
			"           --move script_noteworthy=talent_speedcola@892,-178,55,16\n"
			"  --game   game folder (default: %MAPKIT_GAME_DIR%, then the current folder)\n"
			"  --out    output folder instead of <game>\\cw-mod\\maps\\<name>\\\n"
			"\n"
			"The output is a copy of your own game data for testing on this PC. Never share it.");
	}

	bool ParseArgs(int argc, char** argv, Options& options) {
		for (int i = 1; i < argc; ++i) {
			const std::string arg = argv[i];
			if (arg == "--game" && i + 1 < argc) {
				options.gameDir = argv[++i];
			}
			else if (arg == "--out" && i + 1 < argc) {
				options.outDir = argv[++i];
			}
			else if (arg == "--trace" && i + 1 < argc) {
				options.trace = argv[++i];
			}
			else if (arg == "--floor" && i + 1 < argc) {
				options.floor = std::stof(argv[++i]);
			}
			else if (arg == "--at" && i + 1 < argc) {
				std::array<float, 3> at{};
				if (sscanf_s(argv[++i], "%f,%f,%f", &at[0], &at[1], &at[2]) != 3) {
					std::fprintf(stderr, "--at %s: expected <x>,<y>,<z>\n", argv[i]);
					return false;
				}
				options.at = at;
			}
			else if (arg == "--tiles" && i + 1 < argc) {
				options.tiles = std::stoi(argv[++i]);
				if (options.tiles < 1 || options.tiles > 21 || options.tiles % 2 == 0) {
					std::fprintf(stderr, "--tiles wants an odd number from 1 to 21\n");
					return false;
				}
			}
			else if (arg == "--keep-terrain") {
				options.keepTerrain = true;
			}
			else if (arg == "--retail-gfx") {
				options.retailGfx = true;
			}
			else if (arg == "--keep-decals") {
				options.keepDecals = true;
			}
			else if (arg == "--library-world") {
				options.libraryWorld = true;
			}
			else if (arg == "--link-library") {
				options.linkLibrary = true;
			}
			else if (arg == "--with-library") {
				options.withLibrary = true;
			}
			else if (arg == "--draw-test") {
				options.drawTest = true;
			}
			else if (arg == "--level" && i + 1 < argc) {
				options.levelDir = argv[++i];
			}
			else if (arg == "--as" && i + 1 < argc) {
				options.as = argv[++i];
			}
			else if (arg == "--no-level-scripts") {
				options.levelScripts = false;
			}
			else if (arg == "--no-navmesh") {
				options.navmesh = false;
			}
			else if (arg == "--no-acoustics") {
				options.acoustics = false;
			}
			else if (arg == "--keep-lights") {
				options.keepLights = true;
			}
			else if (arg == "--keep-baked-shadows") {
				options.keepBakedShadows = true;
			}
			else if (arg == "--no-object-clip") {
				options.objectClip = false;
			}
			else if (arg == "--overlay") {
				options.overlay = true;
			}
			else if (arg == "--tile" && i + 1 < argc) {
				options.tile = std::stoull(argv[++i], nullptr, 16);
			}
			else if (arg == "--move" && i + 1 < argc) {
				Move move;
				if (!ParseMove(argv[++i], move)) {
					std::fprintf(stderr, "--move %s: expected <key>=<value>@<x>,<y>,<z>[,<yaw>]\n", argv[i]);
					return false;
				}
				options.moves.push_back(move);
			}
			else if (!arg.empty() && arg[0] != '-') {
				std::string lower = arg;
				std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				options.args.push_back(lower);
				options.rawArgs.push_back(arg);
			}
			else {
				return false;
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
		if (options.args.empty()) {
			return false;
		}
		const std::string& command = options.args[0];
		return ((command == "clone" || command == "build") && options.args.size() == 3)
			|| ((command == "replace" || command == "patch" || command == "plane") && options.args.size() == 2);
	}

	// A zone name the engine can load: letters, digits and '_' only, and short enough for the 64-byte
	// name fields once the longest variant prefix (ww_1080_) is added.
	bool ValidMapName(const std::string& name) {
		return !name.empty() && name.size() <= 48 && name.find('_') != std::string::npos
			&& std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
	}

	// "" for <map>.ff itself, else the variant prefix ("techset_", "ww_1080_", "en_", ...).
	std::vector<std::string> FindVariants(const fs::path& zoneDir, const std::string& map) {
		std::vector<std::string> prefixes;
		for (const auto& entry : fs::directory_iterator(zoneDir)) {
			if (entry.path().extension() != ".ff") continue;
			std::string stem = entry.path().stem().string();
			std::ranges::transform(stem, stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (stem == map) {
				prefixes.emplace_back();
			}
			else if (stem.size() > map.size() + 1 && stem.ends_with("_" + map)) {
				prefixes.push_back(stem.substr(0, stem.size() - map.size()));
			}
		}
		std::ranges::sort(prefixes);
		return prefixes;
	}

	bool WriteZone(const LoadedZone& zone, const std::string& name, const std::vector<std::uint8_t>& stream, const fs::path& dir) {
		FastFileHeader header = zone.header;
		header.SetZoneName(name);
		std::vector<std::uint8_t> file;
		std::string error;
		if (!WriteFastFile(header, stream, file, error)) {
			std::fprintf(stderr, "  %s: %s\n", name.c_str(), error.c_str());
			return false;
		}
		const fs::path path = dir / (name + ".ff");
		if (!WriteWholeFile(path, file)) {
			std::fprintf(stderr, "  could not write %s\n", path.string().c_str());
			return false;
		}
		std::printf("  %-28s %9.2f MB\n", (name + ".ff").c_str(), file.size() / (1024.0 * 1024.0));
		return true;
	}

	int Replace(const Options& options, const std::string& map) {
		const fs::path out = options.outDir.empty() ? options.gameDir / "cw-mod" / "maps" / map : options.outDir;
		fs::create_directories(out);

		LoadedZone zone;
		std::string error;
		if (!LoadZone(options.gameDir / "zone" / (map + ".ff"), zone, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		std::printf("replace %s -> %s\n", map.c_str(), out.string().c_str());
		if (!options.moves.empty()) {
			MapEntities entities;
			if (!FindMapEntities(zone.stream, map, entities, error)) {
				std::fprintf(stderr, "%s\n", error.c_str());
				return 1;
			}
			for (const Move& move : options.moves) {
				std::size_t moved = 0;
				for (const MapEntity& entity : entities.entities) {
					if (entity.Text(move.key) != move.value) {
						continue;
					}
					MoveEntity(zone.stream, entity, move.origin, move.yaw);
					std::printf("  moved %s %u (%s=%s) from %.1f %.1f %.1f yaw %.1f to %.1f %.1f %.1f yaw %.1f\n",
						entity.trigger ? "trigger" : "entity", entity.id, move.key.c_str(), move.value.c_str(),
						entity.origin[0], entity.origin[1], entity.origin[2], entity.angles[1],
						move.origin[0], move.origin[1], move.origin[2], move.yaw.value_or(entity.angles[1]));
					++moved;
				}
				if (!moved) {
					std::fprintf(stderr, "no entity has %s=%s\n", move.key.c_str(), move.value.c_str());
					return 1;
				}
			}
		}
		return WriteZone(zone, map, zone.stream, out) ? 0 : 1;
	}

	// The override zones: the region + texture-tier variants of the map (DB_ExpandZoneVariants 0x7FF7295F2490),
	// loaded with the map at zone priority 27 against the map's own 7 (DB_GetZonePriority 0x7FF727EC2F50), so
	// an asset of the same name linked from them replaces the map's (DB_LinkXAssetEntry 0x7FF727EC3AA0 queues
	// the swap). The game loads one of the two depending on the texture setting; both get the same content.
	constexpr std::array<const char*, 2> kOverrideZonePrefixes = { "ww_1080_", "ww_4k_" };

	// A level zone with its trace aligned: every asset decodes from its recorded start.
	struct TracedMap {
		LoadedZone zone;
		XAssetList list;
		ZoneTrace trace;

		std::size_t Find(std::uint64_t type) const {
			for (std::size_t i = 0; i < list.assets.size(); ++i) {
				if (list.assets[i].type == type) {
					return i;
				}
			}
			return list.assets.size();
		}
		void Start(XStream& s, std::size_t index) const {
			const TracedAsset& start = trace.assets[index];
			s.Restore(start.offset, start.block, start.pos);
		}
		// The root struct of the top-level asset of `type` named `name` (its header inline), or empty.
		std::span<const std::uint8_t> Root(std::uint64_t type, std::uint64_t name, std::size_t size) const {
			for (std::size_t i = 0; i < list.assets.size(); ++i) {
				const std::size_t at = trace.assets[i].offset;
				if (list.assets[i].type == type && list.assets[i].header == kPtrInline && at + size <= zone.stream.size()
					&& Get<std::uint64_t>(std::span<const std::uint8_t>(zone.stream).subspan(at, 8), 0) == name) {
					return std::span<const std::uint8_t>(zone.stream).subspan(at, size);
				}
			}
			return {};
		}
	};

	bool LoadTracedMap(const Options& options, const std::string& map, TracedMap& out) {
		if (options.trace.empty()) {
			std::fprintf(stderr, "this needs --trace <zone.mktrace>: the map's zone trace (cw-mod.json \"mapkit_trace\").\n");
			return false;
		}
		std::string error;
		if (!LoadZone(options.gameDir / "zone" / (map + ".ff"), out.zone, error) || !ParseXAssetList(out.zone.stream, out.list, error)
			|| !ReadZoneTrace(options.trace, out.trace, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		if (const std::string mismatch = AlignZoneTrace(out.trace, out.zone.stream, out.list); !mismatch.empty()) {
			std::fprintf(stderr, "the trace is not of this zone: %s\n", mismatch.c_str());
			return false;
		}
		return true;
	}

	using AssetKey = std::pair<std::uint64_t, std::uint64_t>; // {type, name}

	// What stays loaded under a map that leaves <map>'s zones out: <mode>_common (Com_LoadLevelFastFiles loads it before
	// every level of the mode), as the assets it holds itself (not by name), read through its trace, which the client
	// records next to <map>'s (cw-mod.json "mapkit_trace"). core's zones stay too, but nothing here needs them.
	bool LoadStayingAssets(const Options& options, const std::string& map, std::set<AssetKey>& out) {
		const std::string common = map.substr(0, map.find('_')) + "_common";
		Options zoneOptions = options;
		zoneOptions.trace = options.trace.parent_path() / (common + ".mktrace");
		TracedMap zone;
		if (!LoadTracedMap(zoneOptions, common, zone)) {
			std::fprintf(stderr, "(%s's trace, %s, says what stays loaded without %s's zones; --with-library builds without it)\n",
				common.c_str(), zoneOptions.trace.string().c_str(), map.c_str());
			return false;
		}
		const std::span<const std::uint8_t> stream(zone.zone.stream);
		for (std::size_t i = 0; i < zone.list.assets.size(); ++i) {
			const std::uint64_t type = zone.list.assets[i].type;
			const std::size_t at = zone.trace.assets[i].offset + XAssetNameOffset(type);
			if (at + 8 > stream.size()) {
				continue;
			}
			const std::uint64_t name = Get<std::uint64_t>(stream, at);
			if (name && name != kPtrInline && !(name >> 63)) {
				out.emplace(type, name);
			}
		}
		std::printf("  %s stays loaded under the map: %zu assets of its own\n", common.c_str(), out.size());
		return true;
	}

	// The map's entity list, every string resolved so it can be re-encoded, and (when asked) its trigger list.
	bool ReadEntityList(const TracedMap& traced, const std::string& map, std::vector<MapEntity>& entities,
		std::vector<MapEntity>* triggers = nullptr) {
		MapEntities decoded;
		std::string error;
		if (!ReadMapEntitiesTraced(traced.zone.stream, traced.list, traced.trace, decoded, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		for (MapEntity& entity : decoded.entities) {
			if (!entity.trigger) {
				entities.push_back(std::move(entity));
			}
			else if (triggers) {
				triggers->push_back(std::move(entity));
			}
		}
		std::printf("%s: %zu entities in the entity list, %zu string references unresolved\n", map.c_str(), entities.size(),
			decoded.unresolved);
		if (decoded.unresolved) {
			for (const MapEntity& entity : entities) {
				for (const EntityKey& key : entity.keys) {
					if (key.key.starts_with("<ref") || key.text.starts_with("<ref")) {
						std::fprintf(stderr, "  entity %u: %s = %s\n", entity.id, key.key.c_str(), key.text.c_str());
					}
				}
			}
			std::fprintf(stderr, "every string must resolve to re-encode the entity list.\n");
			return false;
		}
		return true;
	}

	bool ApplyMoves(const Options& options, std::vector<MapEntity>& entities) {
		for (const Move& move : options.moves) {
			std::size_t moved = 0;
			for (MapEntity& entity : entities) {
				if (entity.Text(move.key) != move.value) {
					continue;
				}
				std::printf("  moved entity %u (%s=%s) from %.1f %.1f %.1f yaw %.1f to %.1f %.1f %.1f yaw %.1f\n", entity.id,
					move.key.c_str(), move.value.c_str(), entity.origin[0], entity.origin[1], entity.origin[2], entity.angles[1],
					move.origin[0], move.origin[1], move.origin[2], move.yaw.value_or(entity.angles[1]));
				SetEntityOrigin(entity, move.origin, move.yaw);
				++moved;
			}
			if (!moved) {
				std::fprintf(stderr, "no entity has %s=%s\n", move.key.c_str(), move.value.c_str());
				return false;
			}
		}
		return true;
	}

	// Writes both override zones: each is the retail variant's own assets (reference-free, copied) plus
	// `replacements`. Every zone is walked before it is written (zone_writer.hpp), and its entity list
	// read back.
	// `name` is what they are named after: <map> itself (patch, plane: they replace <map>'s, every match) or a
	// custom map's id (build: ww_1080_<id> / ww_4k_<id> in cw-mod/maps/<id>, which the client loads in place of
	// <map>'s only while that map is picked in CUSTOM MAPS).
	int WriteOverrideZones(const Options& options, const std::string& map, const std::vector<ZoneAsset>& replacements,
		std::size_t entityCount, const std::string& name) {
		const fs::path zoneDir = options.gameDir / "zone";
		const fs::path out = options.outDir.empty() ? options.gameDir / "cw-mod" / "maps" / name : options.outDir;
		fs::create_directories(out);
		std::string error;
		int failed = 0;
		for (const char* prefix : kOverrideZonePrefixes) {
			const std::string variant = prefix + map;
			const std::string output = prefix + name;
			LoadedZone base;
			XAssetList baseList;
			if (!LoadZone(zoneDir / (variant + ".ff"), base, error) || !ParseXAssetList(base.stream, baseList, error)) {
				std::fprintf(stderr, "  %s: %s\n", variant.c_str(), error.c_str());
				++failed;
				continue;
			}
			// The variant's own assets come along byte for byte, which is only sound for assets that hold no
			// references: the walk proves that (and that mapkit reads every one of them).
			const WalkResult baseWalk = WalkStream(base.stream);
			if (!baseWalk.complete || baseWalk.references) {
				std::fprintf(stderr, "  %s: its assets cannot be carried over (%s)\n", variant.c_str(),
					baseWalk.complete ? "they hold references" : "mapkit cannot walk it");
				++failed;
				continue;
			}
			std::vector<ZoneAsset> assets;
			for (const WalkedAsset& walked : baseWalk.assets) {
				assets.push_back(CopiedAsset(walked.type, std::string(XAssetTypeName(walked.type)),
					std::vector<std::uint8_t>(base.stream.begin() + walked.begin, base.stream.begin() + walked.end)));
			}
			assets.insert(assets.end(), replacements.begin(), replacements.end());

			std::string streamError;
			const std::vector<std::uint8_t> stream = BuildZoneStream(baseList.strings, assets, streamError);
			if (!streamError.empty()) {
				std::fprintf(stderr, "  %s: %s\n", variant.c_str(), streamError.c_str());
				++failed;
				continue;
			}
			std::vector<std::uint8_t> file;
			ZoneWriteResult result;
			if (!WriteZone(base.header, output, stream, file, result)) {
				std::fprintf(stderr, "  %s: %s\n", variant.c_str(), result.error.c_str());
				++failed;
				continue;
			}

			// Read the file back the way the game will, and check the entity list survived the trip.
			FastFileHeader reread;
			std::vector<std::uint8_t> back;
			MapEntities check;
			if (!ReadFastFile(file, reread, back, nullptr, error) || back != stream
				|| !FindMapEntities(back, map, check, error)) {
				std::fprintf(stderr, "  %s: does not read back: %s\n", variant.c_str(), error.c_str());
				++failed;
				continue;
			}
			std::size_t count = 0;
			for (const MapEntity& entity : check.entities) {
				count += entity.trigger ? 0 : 1;
			}
			if (count != entityCount || check.unresolved) {
				std::fprintf(stderr, "  %s: read back %zu entities (%zu unresolved), wrote %zu\n", variant.c_str(), count,
					check.unresolved, entityCount);
				++failed;
				continue;
			}

			const fs::path path = out / (output + ".ff");
			if (!WriteWholeFile(path, file)) {
				std::fprintf(stderr, "  could not write %s\n", path.string().c_str());
				++failed;
				continue;
			}
			std::printf("  %-28s %9.2f MB: %zu assets (%zu carried over", (output + ".ff").c_str(),
				file.size() / (1024.0 * 1024.0), assets.size(), baseWalk.assets.size());
			std::size_t references = 0;
			for (const ZoneAsset& asset : replacements) {
				if (asset.label.empty()) {
					++references;
					continue;
				}
				std::printf(", %s", asset.label.c_str());
			}
			if (references) {
				std::printf(", %zu by-name references", references);
			}
			std::printf("), walk complete\n      blocks:");
			for (std::size_t b = 0; b < kXBlockCount; ++b) {
				if (result.blockSizes[b]) {
					std::printf(" [%zu] 0x%llx", b, static_cast<unsigned long long>(result.blockSizes[b]));
				}
			}
			std::printf("\n");
		}
		return failed;
	}

	// Without <map>'s zones nothing is swapped in after the zone loads, and a by-name reference finds only what has loaded
	// before it: one to an asset the zone itself holds further on gets the type's default as a stub, and for a type with no
	// default (lighting, clip_map) drops the game (DB_LinkMissingReference 0x7FF727EC1860). True when every reference in the
	// written zone comes after the asset of that name it holds, if any. A reference written inside another asset (the empty
	// streamerworld's lighting) is not a top-level asset, so Build orders that one itself.
	bool ReferencesFollowTheirAssets(std::span<const std::uint8_t> stream, const WalkResult& walk, const std::string& id) {
		std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> held; // {type, name} -> the first asset of it
		std::vector<std::tuple<std::size_t, std::uint64_t, std::uint64_t>> references;
		for (std::size_t i = 0; i < walk.assets.size(); ++i) {
			const WalkedAsset& asset = walk.assets[i];
			const std::size_t at = asset.begin + XAssetNameOffset(asset.type);
			const std::uint64_t name = at + 8 <= asset.end ? Get<std::uint64_t>(stream, at) : 0;
			if (name == 0 || name == kPtrInline) {
				continue; // nameless, or named by a string (a sanim)
			}
			if (name >> 63) {
				references.emplace_back(i, asset.type, name & ~(1ull << 63));
			}
			else {
				held.try_emplace({ asset.type, name }, i);
			}
		}
		std::size_t early = 0;
		std::string list;
		for (const auto& [index, type, name] : references) {
			const auto found = held.find({ type, name });
			if (found != held.end() && found->second > index) {
				if (early++ < 8) {
					list += std::format(" {} {:016X} (referenced by asset {}, held at {});", XAssetTypeName(type), name, index,
						found->second);
				}
			}
		}
		if (early) {
			std::fprintf(stderr, "  %s: %zu by-name reference(s) come before the asset they name:%s\n", id.c_str(), early,
				list.c_str());
			return false;
		}
		return true;
	}

	// A map of its own: one zone, <out>/<id>.ff, holding only what the build writes. Of <map>'s zones only techset_<map>
	// loads under it (all of them with --with-library, as its asset library: mapkit_loader.hpp), and nothing of theirs is
	// carried over. Without the library its references are checked to follow what they name. The header is <map>'s ww_4k_
	// variant's (a retail zone's, for the build checksum and flags, as the override zones had it), renamed. The file
	// is read back the way the game will, and its entity list found (under <map>'s name, like the rest of its world)
	// before it is written. An overlay build of the same id (ww_1080_<id>.ff, ww_4k_<id>.ff) is removed: a folder
	// holds one form.
	int WriteMapZone(const Options& options, const std::string& map, const std::vector<ZoneAsset>& assets,
		std::size_t entityCount, const std::string& id, bool library) {
		const fs::path out = options.outDir.empty() ? options.gameDir / "cw-mod" / "maps" / id : options.outDir;
		fs::create_directories(out);
		std::string error;
		const std::string templ = std::string(kOverrideZonePrefixes[1]) + map;
		LoadedZone base;
		if (!LoadZone(options.gameDir / "zone" / (templ + ".ff"), base, error)) {
			std::fprintf(stderr, "  %s (the header's template): %s\n", templ.c_str(), error.c_str());
			return 1;
		}
		const std::vector<std::optional<std::string>> strings = { std::nullopt };
		const std::vector<std::uint8_t> stream = BuildZoneStream(strings, assets, error);
		if (!error.empty()) {
			std::fprintf(stderr, "  %s: %s\n", id.c_str(), error.c_str());
			return 1;
		}
		std::vector<std::uint8_t> file;
		ZoneWriteResult result;
		if (!WriteZone(base.header, id, stream, file, result)) {
			std::fprintf(stderr, "  %s: %s\n", id.c_str(), result.error.c_str());
			return 1;
		}
		FastFileHeader reread;
		std::vector<std::uint8_t> back;
		MapEntities check;
		if (!ReadFastFile(file, reread, back, nullptr, error) || back != stream || !FindMapEntities(back, map, check, error)) {
			std::fprintf(stderr, "  %s: does not read back: %s\n", id.c_str(), error.c_str());
			return 1;
		}
		std::size_t count = 0;
		for (const MapEntity& entity : check.entities) {
			count += entity.trigger ? 0 : 1;
		}
		if (count != entityCount || check.unresolved) {
			std::fprintf(stderr, "  %s: read back %zu entities (%zu unresolved), wrote %zu\n", id.c_str(), count, check.unresolved,
				entityCount);
			return 1;
		}
		if (!library && !ReferencesFollowTheirAssets(stream, result.walk, id)) {
			return 1;
		}
		const fs::path path = out / (id + ".ff");
		if (!WriteWholeFile(path, file)) {
			std::fprintf(stderr, "  could not write %s\n", path.string().c_str());
			return 1;
		}
		std::size_t references = 0;
		std::vector<std::pair<std::string, std::size_t>> counted; // each label once, in order, with how many carry it
		for (const ZoneAsset& asset : assets) {
			if (asset.label.empty()) {
				++references;
				continue;
			}
			const auto found = std::ranges::find(counted, asset.label, &std::pair<std::string, std::size_t>::first);
			if (found == counted.end()) {
				counted.emplace_back(asset.label, 1);
			}
			else {
				++found->second;
			}
		}
		std::string labels;
		for (const auto& [label, n] : counted) {
			labels += n == 1 ? ", " + label : std::format(", {} x{}", label, n);
		}
		std::printf("  %-28s %9.2f MB: %zu assets (%zu by-name references%s), walk complete\n      blocks:",
			(id + ".ff").c_str(), file.size() / (1024.0 * 1024.0), assets.size(), references, labels.c_str());
		for (std::size_t b = 0; b < kXBlockCount; ++b) {
			if (result.blockSizes[b]) {
				std::printf(" [%zu] 0x%llx", b, static_cast<unsigned long long>(result.blockSizes[b]));
			}
		}
		std::printf("\n");
		if (result.blockSizes[1] > result.block1Rule) {
			// DB_AllocXBlocks 0x7FF7295D6020 gives block 1 its size + 15, 64-KB aligned, and block 2 follows.
			const std::uint64_t room = (result.block1Rule + 15 + 0xFFFF) & ~0xFFFFull;
			const std::uint64_t need = result.walk.preloadBlock1;
			std::printf("      block 1 sized for the lobby preload: %.1f KB more than the retail rule gives (its roots and techset "
				"saves); %s\n", (result.blockSizes[1] - result.block1Rule) / 1024.0,
				need > room ? std::format("by the rule the preload would run {:.1f} KB into block 2", (need - room) / 1024.0).c_str()
					: "the rule's block, rounded up to 64 KB, would still hold it");
		}
		for (const char* prefix : kOverrideZonePrefixes) {
			std::error_code ec;
			const fs::path old = out / (prefix + id + ".ff");
			if (fs::remove(old, ec)) {
				std::printf("  removed %s (the overlay build of this map)\n", old.filename().string().c_str());
			}
		}
		return 0;
	}

	ZoneAsset EntityListAsset(const std::string& map, const std::vector<MapEntity>& entities) {
		ZoneAsset asset;
		asset.type = 0x8E;
		asset.label = "entitylist";
		asset.encode = [name = HashName(MapBspName(map)), &entities](XWriter& w) { EncodeEntityList(w, name, entities); };
		return asset;
	}

	ZoneAsset TriggerListAsset(const std::string& map, const TriggerShapes& shapes, const std::vector<MapEntity>& triggers) {
		ZoneAsset asset;
		asset.type = 0x80;
		asset.label = "triggerlist";
		asset.encode = [name = HashName(MapBspName(map)), &shapes, &triggers](XWriter& w) {
			EncodeTriggerList(w, name, shapes, triggers);
		};
		return asset;
	}

	// A bgcache listing every xmodel the build writes (world_writer.hpp EncodeBgCache), plus `extra` (game models
	// the level's entities use: the doors'): the engine finds a map entity's model only through the bgcache name
	// tables, so without it mapkit's script_models spawn with no model and never draw (the draw test rows of
	// 2026-09-26). Its own name, so <map>'s own list stays as it is.
	ZoneAsset BgCacheAsset(const std::string& source, const std::vector<ZoneAsset>& assets,
		std::span<const std::uint64_t> extra = {}) {
		std::vector<BgCacheEntry> entries;
		for (const ZoneAsset& asset : assets) {
			if (asset.type == 0x06 && asset.linkName) {
				entries.push_back({ kBgCacheModel, asset.linkName });
			}
		}
		for (const std::uint64_t model : extra) {
			if (std::ranges::none_of(entries, [&](const BgCacheEntry& entry) { return entry.name == model; })) {
				entries.push_back({ kBgCacheModel, model });
			}
		}
		ZoneAsset asset;
		asset.type = 0x6D;
		asset.label = std::format("bgcache ({} models)", entries.size());
		asset.encode = [name = HashName("mapkit_" + source + "_bgcache"), entries](XWriter& w) { EncodeBgCache(w, name, entries); };
		return asset;
	}

	int Patch(const Options& options, const std::string& map) {
		TracedMap traced;
		std::vector<MapEntity> entities;
		if (!LoadTracedMap(options, map, traced) || !ReadEntityList(traced, map, entities) || !ApplyMoves(options, entities)) {
			return 1;
		}
		const int failed = WriteOverrideZones(options, map, { EntityListAsset(map, entities) }, entities.size(), map);
		std::printf("\n%s. The entity list is a copy of your own game data for testing on this PC: never share it.\n",
			failed ? "FAILED" : "Done");
		return failed ? 1 : 0;
	}

	// What the plane map lays over <map> (test D2, PASSED 18:08): no world (an empty streamerworld and
	// districts), and collision that is the map's trigger trees plus a flat terrain.
	struct PlaneWorld {
		std::array<std::uint8_t, 224> worldRoot{};
		std::array<std::uint8_t, 96> districtsRoot{};
		std::shared_ptr<ClipMap> clip;
		float height = 0; // the terrain's, as stored
		std::shared_ptr<struct GfxPlan> gfx; // mapkit's own gfx_map (null with --retail-gfx)
	};

	bool MakePlaneWorld(const TracedMap& traced, const std::string& map, float floor, PlaneWorld& out) {
		std::string error;
		const std::size_t clipIndex = traced.Find(0x18);
		const std::size_t worldIndex = traced.Find(0xAC);
		if (clipIndex == traced.list.assets.size() || worldIndex == traced.list.assets.size()) {
			std::fprintf(stderr, "%s has no clip_map or streamerworld\n", map.c_str());
			return false;
		}
		ClipMap retailClip;
		XStream clipStream(traced.zone.stream, 0);
		traced.Start(clipStream, clipIndex);
		if (!ReadClipMap(clipStream, traced.list.assets[clipIndex].header, retailClip)) {
			std::fprintf(stderr, "clip map: %s\n", clipStream.Error().c_str());
			return false;
		}
		// Its script strings are indices into the map zone's table, which the override zone does not have.
		if (!ResolveClipMapStrings(retailClip, traced.list.strings, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		StreamerWorld retailWorld;
		XStream worldStream(traced.zone.stream, 0);
		traced.Start(worldStream, worldIndex);
		if (!ReadStreamerWorld(worldStream, traced.list.assets[worldIndex].header, retailWorld)) {
			std::fprintf(stderr, "streamerworld: %s\n", worldStream.Error().c_str());
			return false;
		}
		out.worldRoot = retailWorld.root;
		const std::span<const std::uint8_t> districtsSource = traced.Root(0xAB, HashName(MapBspName(map)), 96);
		if (districtsSource.empty()) {
			std::fprintf(stderr, "%s has no districts named like its map\n", map.c_str());
			return false;
		}
		std::ranges::copy(districtsSource, out.districtsRoot.begin());
		std::printf("  districts: %llu dropped (the streamer's cells: the old world's collision and models)\n",
			static_cast<unsigned long long>(Get<std::uint64_t>(districtsSource, 64)));

		out.clip = std::make_shared<ClipMap>();
		if (!MakePlaneClipMap(retailClip, floor, *out.clip, out.height, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		const ClipMap& clip = *out.clip;
		std::size_t tiles = 0;
		for (const ClipMapWorldEntry& entry : clip.worldEntries) {
			tiles += entry.records.size();
		}
		std::printf("  collision: %zu trees (the map's), terrain %zu tiles flat at z %.3f, surface %u\n", clip.trees.size(),
			tiles, out.height, Get<std::uint16_t>(clip.worldEntries[0].records[0].raw, 70));
		const std::vector<std::string> clipStrings = ClipMapScriptStrings(clip);
		// Many are stored obfuscated (SL_GetString decodes them), so only the readable ones are named.
		std::size_t encoded = 0;
		std::string readable;
		for (const std::string& text : clipStrings) {
			if (std::ranges::all_of(text, [](char c) { return c >= 0x20 && c < 0x7F; })) {
				readable += " " + text;
			}
			else {
				++encoded;
			}
		}
		std::printf("  clip map script strings: %zu carried into the zone's table (%zu stored encoded):%s\n",
			clipStrings.size(), encoded, readable.c_str());
		return true;
	}

	// Every asset's name hash, as a copy's links name them (asset_record.hpp RecordSplice::assetNames).
	std::vector<std::uint64_t> TracedAssetNames(const TracedMap& traced) {
		const std::size_t count = traced.list.assets.size();
		std::vector<std::uint64_t> names(count);
		for (std::size_t i = 0; i < count; ++i) {
			const std::size_t at = traced.trace.assets[i].offset;
			names[i] = TracedAssetName(traced.trace, traced.zone.stream, i);
			// A sanim's root (112 B, Load_SanimAsset 0x7FF71E7DA300) starts with its name, stored inline right after it.
			if (traced.list.assets[i].type == 0x66 && Get<std::uint64_t>(std::span<const std::uint8_t>(traced.zone.stream).subspan(at, 8), 0) == kPtrInline) {
				const char* name = reinterpret_cast<const char*>(traced.zone.stream.data() + at + 112);
				names[i] = HashName(std::string(name, strnlen(name, traced.zone.stream.size() - at - 112)));
			}
		}
		return names;
	}

	// mapkit's own gfx_map (Test G1): <map>'s, copied byte for byte (gfx_world.hpp) with what draws its world
	// taken out. The model groups are the placed buildings and props (per-instance transforms; 136 of them are
	// the clip map's own 992-B structs, which the gfx_map points into, so they cannot be copied anyway); the
	// group LOD models are their merged far versions; the terrain is its ground. The draw list's remaining
	// models all become tag_origin (a model with nothing to draw), so every count and index stays valid.
	struct GfxPlan {
		GfxWorld world;
		GfxWorldSplice splice;
		std::vector<std::uint64_t> assetNames;
		std::vector<GfxWorldLink> links;
		std::vector<std::string> strings;
		// sanims are named by a string, not a hash, so they cannot be linked by name like the rest: the retail
		// ones travel whole (inline name and data, no references), keyed by HashName(name), with their script
		// strings re-pointed into mapkit's table (left as they were, they read past it: Test G1's 20:16 crash).
		struct SAnimCopy {
			std::uint64_t name = 0;
			std::vector<std::uint8_t> bytes;
			std::vector<CopiedString> strings;
		};
		std::vector<SAnimCopy> sanims;
		std::string error; // set if the encode fails (the zone then does not read back and is not written)
	};

	// Root offsets of the counts that size each cut (Load_GfxWorldDraw is at +432).
	constexpr std::size_t kGfxDrawModelGroupCount = 432 + 536 + 40; // u64, 158 on zm_silver
	constexpr std::size_t kGfxGroupLodEntryCount = 432 + 696;       // u64
	constexpr std::size_t kGfxGroupLodModelCount = 432 + 712;       // u64

	bool MakeGfxWorld(const TracedMap& traced, const std::string& map, const Options& options, PlaneWorld& out) {
		if (options.retailGfx) {
			std::printf("  gfx_map: %s's own (--retail-gfx): its buildings still draw\n", map.c_str());
			return true;
		}
		const std::size_t index = traced.Find(0x1B);
		if (index == traced.list.assets.size()) {
			std::fprintf(stderr, "%s has no gfx_map\n", map.c_str());
			return false;
		}
		auto plan = std::make_shared<GfxPlan>();
		XStream s(traced.zone.stream, 0);
		traced.Start(s, index);
		if (!ReadGfxWorld(s, traced.list.assets[index].header, plan->world)) {
			std::fprintf(stderr, "gfx_map: %s\n", s.Error().c_str());
			return false;
		}
		const std::size_t count = traced.list.assets.size();
		plan->assetNames = TracedAssetNames(traced);

		GfxWorldSplice& splice = plan->splice;
		splice.stream = traced.zone.stream;
		splice.record = &plan->world;
		// The XAsset array is the last thing block 4 holds before the first asset.
		splice.assetArrayPos = traced.trace.assets[0].pos[XBlockVirtual] - 16 * count;
		splice.assets = traced.list.assets;
		splice.assetNames = plan->assetNames;
		splice.strings = traced.list.strings;
		splice.cuts.push_back({ "draw model groups", { { kGfxDrawModelGroupCount, 8 } } });
		splice.cuts.push_back({ "group lod entries", { { kGfxGroupLodEntryCount, 8 } } });
		splice.cuts.push_back({ "group lod models", { { kGfxGroupLodModelCount, 8 } } });
		if (!options.keepTerrain) {
			splice.cuts.push_back({ "terrain", {} });
		}
		if (!options.keepDecals) {
			// The projected decals: blood, graffiti, snow footprints and ground blends in <map>'s places. The renderer
			// (R_CollectVisibleProjectedDecals_cand 0x7FF726A99E70) never reads the count @1400: it walks the index
			// ranges [+1432, +1432 + +1444) and [+1440, +1440 + +1452) (zm_silver: 0 + 3061), so those go to zero
			// too (the 23:56 zones crashed on the first visible decal, 2026-09-26).
			splice.cuts.push_back({ "decals", { { 1400, 4 }, { 1432, 24 } } });
		}
		splice.relinks.push_back({ "draw xmodels", 0x06, HashName("tag_origin") });
		// A map without one of them (no terrain, say) just has less to cut.
		std::erase_if(splice.cuts, [&](const GfxWorldCut& cut) { return !plan->world.Subtree(cut.name); });
		std::string error;
		if (!GfxWorldLinks(splice, plan->links, plan->strings, error)) {
			std::fprintf(stderr, "gfx_map: %s\n", error.c_str());
			return false;
		}
		for (const GfxWorldLink& link : plan->links) {
			if (link.type != 0x66) {
				continue;
			}
			for (std::size_t i = 0; i < count; ++i) {
				if (traced.list.assets[i].type == 0x66 && plan->assetNames[i] == link.name && i + 1 < traced.trace.assets.size()) {
					const auto begin = traced.zone.stream.begin() + static_cast<std::ptrdiff_t>(traced.trace.assets[i].offset);
					const auto end = traced.zone.stream.begin() + static_cast<std::ptrdiff_t>(traced.trace.assets[i + 1].offset);
					GfxPlan::SAnimCopy copy{ link.name, std::vector<std::uint8_t>(begin, end), {} };
					const auto fields = SAnimScriptStrings(copy.bytes);
					if (!fields) {
						std::fprintf(stderr, "gfx_map: sanim %016llX does not read back on its own\n",
							static_cast<unsigned long long>(link.name));
						return false;
					}
					if (const auto links = FindAssetLinks(traced.zone.stream, traced.trace.assets[i].offset,
							traced.trace.assets[i + 1].offset, splice.assetArrayPos, count); !links.empty()) {
						std::fprintf(stderr, "gfx_map: sanim %016llX links to another asset (+0x%zX): it cannot travel whole\n",
							static_cast<unsigned long long>(link.name), links[0] - traced.trace.assets[i].offset);
						return false;
					}
					for (const std::size_t at : *fields) {
						const std::uint32_t string = Get<std::uint32_t>(copy.bytes, at);
						if (!string) {
							continue;
						}
						if (string >= splice.strings.size() || !splice.strings[string]) {
							std::fprintf(stderr, "gfx_map: sanim %016llX: script string %u is not in %s's table\n",
								static_cast<unsigned long long>(link.name), string, map.c_str());
							return false;
						}
						copy.strings.push_back({ at, *splice.strings[string] });
					}
					plan->sanims.push_back(std::move(copy));
					break;
				}
			}
		}
		std::size_t cutBytes = 0;
		std::string cutNames;
		for (const GfxWorldCut& cut : splice.cuts) {
			const GfxWorldSubtree* subtree = plan->world.Subtree(cut.name);
			cutBytes += subtree->end - subtree->begin;
			cutNames += (cutNames.empty() ? "" : ", ") + cut.name;
		}
		std::size_t sanimStrings = 0;
		for (const GfxPlan::SAnimCopy& copy : plan->sanims) {
			sanimStrings += copy.strings.size();
		}
		std::printf("  gfx_map: mapkit's own, %s's without %s (%.2f of %.2f MB), %zu by-name links, %zu script strings; "
			"%zu sanims copied, %zu script strings re-pointed\n",
			map.c_str(), cutNames.c_str(), cutBytes / (1024.0 * 1024.0),
			(plan->world.end - plan->world.begin) / (1024.0 * 1024.0), plan->links.size(), plan->strings.size(),
			plan->sanims.size(), sanimStrings);
		out.gfx = std::move(plan);
		return true;
	}

	// The root size and alignment of each type a copy links to, for its by-name references.
	std::optional<std::pair<std::size_t, std::uint64_t>> ReferenceRoot(std::uint64_t type) {
		switch (type) {
		case 0x02: return std::pair<std::size_t, std::uint64_t>{ 112, 8 };  // physpreset
		case 0x03: return std::pair<std::size_t, std::uint64_t>{ 48, 8 };   // physconstraints
		case 0x06: return std::pair<std::size_t, std::uint64_t>{ 232, 8 };  // xmodel
		case 0x07: return std::pair<std::size_t, std::uint64_t>{ 96, 8 };   // xcollision
		case 0x08: return std::pair<std::size_t, std::uint64_t>{ 88, 8 };   // xskeleton
		case 0x09: return std::pair<std::size_t, std::uint64_t>{ 64, 8 };   // xmodelmesh
		case 0x0F: return std::pair<std::size_t, std::uint64_t>{ 168, 8 };  // techset
		case 0x0A: return std::pair<std::size_t, std::uint64_t>{ 344, 16 }; // material
		case 0x10: return std::pair<std::size_t, std::uint64_t>{ 208, 8 };  // image
		case 0x12: return std::pair<std::size_t, std::uint64_t>{ 112, 8 };  // sound_bank
		case 0x13: return std::pair<std::size_t, std::uint64_t>{ 88, 8 };   // sound_asset
		case 0x14: return std::pair<std::size_t, std::uint64_t>{ 1632, 32 }; // sound_duck
		case 0x15: return std::pair<std::size_t, std::uint64_t>{ 152, 8 };  // sound_alias_modifier
		case 0x16: return std::pair<std::size_t, std::uint64_t>{ 352, 8 };  // sound_acoustics
		case 0x33: return std::pair<std::size_t, std::uint64_t>{ 144, 8 };  // fx
		case 0x35: return std::pair<std::size_t, std::uint64_t>{ 88, 8 };   // klf
		case 0x36: return std::pair<std::size_t, std::uint64_t>{ 24, 8 };   // impactsfxtable
		case 0x37: return std::pair<std::size_t, std::uint64_t>{ 56, 8 };   // impactsoundstable
		case 0x53: return std::pair<std::size_t, std::uint64_t>{ 944, 8 };  // zbarrier
		case 0xAA: return std::pair<std::size_t, std::uint64_t>{ 472, 8 };  // lighting
		case 0xB8: return std::pair<std::size_t, std::uint64_t>{ 56, 8 };   // streamkey
		case 0xD3: return std::pair<std::size_t, std::uint64_t>{ 128, 16 }; // winddef
		case 0xAC: return std::pair<std::size_t, std::uint64_t>{ 224, 8 };  // streamerworld
		case 0xBB: return std::pair<std::size_t, std::uint64_t>{ 40, 8 };   // grouplodmodel
		default: return std::nullopt;
		}
	}

	// A by-name reference to (type, name) in the zone (EncodeAssetReference); nullopt for a type ReferenceRoot lacks.
	std::optional<ZoneAsset> ByNameReference(std::uint64_t type, std::uint64_t name) {
		const auto root = ReferenceRoot(type);
		if (!root) {
			return std::nullopt;
		}
		ZoneAsset reference;
		reference.type = type;
		reference.linkName = name;
		reference.byName = true;
		reference.encode = [type, name, size = root->first, alignment = root->second](XWriter& w) {
			EncodeAssetReference(w, type, name, size, alignment);
		};
		return reference;
	}

	// The gfx_map's by-name references (each links to the asset of that name wherever it lives: after the
	// override swap, the streamerworld one is mapkit's own), then the gfx_map. One the zone already has (the lighting's,
	// when it goes first) is not added twice.
	void AddGfxWorldAssets(const std::shared_ptr<GfxPlan>& plan, std::vector<ZoneAsset>& out) {
		for (const GfxWorldLink& link : plan->links) {
			if (link.type != 0x66 && std::ranges::any_of(out, [&](const ZoneAsset& a) {
					return a.byName && a.type == link.type && a.linkName == link.name; })) {
				continue;
			}
			if (link.type == 0x66) {
				const auto copy = std::ranges::find_if(plan->sanims, [&](const auto& sanim) { return sanim.name == link.name; });
				if (copy == plan->sanims.end()) {
					plan->error = std::format("no sanim {:016X} to carry over", link.name);
					continue;
				}
				ZoneAsset sanim = CopiedAsset(0x66, "", copy->bytes, copy->strings);
				sanim.linkName = link.name;
				out.push_back(std::move(sanim));
				continue;
			}
			auto reference = ByNameReference(link.type, link.name);
			if (!reference) {
				plan->error = std::format("no by-name reference layout for {}", XAssetTypeName(link.type));
				continue;
			}
			out.push_back(std::move(*reference));
		}
		ZoneAsset world;
		world.type = 0x1B;
		world.label = "gfx_map (mapkit's own)";
		world.scriptStrings = plan->strings;
		world.encode = [plan](XWriter& w) {
			std::string error;
			if (!EncodeGfxWorld(w, plan->splice, error)) {
				plan->error = error;
				std::fprintf(stderr, "  gfx_map: %s\n", error.c_str());
			}
		};
		out.push_back(std::move(world));
	}

	// The plane world's three assets. The clip map is encoded as it is when the zone is written.
	void AddPlaneWorldAssets(const std::string& map, const PlaneWorld& plane, std::vector<ZoneAsset>& out) {
		ZoneAsset world;
		world.type = 0xAC;
		world.label = "streamerworld (empty)";
		world.encode = [root = plane.worldRoot, lighting = HashName(MapBspName(map))](XWriter& w) {
			EncodeEmptyStreamerWorld(w, root, lighting);
		};
		out.push_back(std::move(world));
		// The streamer's cells come from districts, not the streamerworld: left as retail, they stream the old
		// world's collision back in, and its records index clip map +24, which the plane drops (the 17:57 crash).
		ZoneAsset districts;
		districts.type = 0xAB;
		districts.label = "districts (empty)";
		districts.encode = [root = plane.districtsRoot](XWriter& w) { EncodeEmptyDistricts(w, root); };
		out.push_back(std::move(districts));
		ZoneAsset clipAsset;
		clipAsset.type = 0x18;
		clipAsset.label = "clip_map (plane)";
		clipAsset.encode = [clip = plane.clip](XWriter& w) { EncodeClipMap(w, *clip); };
		clipAsset.scriptStrings = ClipMapScriptStrings(*plane.clip);
		out.push_back(std::move(clipAsset));
		if (plane.gfx) {
			AddGfxWorldAssets(plane.gfx, out);
		}
	}

	// The plane test map (M4 test D2) laid over <map>: the plane world with its terrain at --floor, and a floor
	// of --tile models around the player spawns. The map's entities and scripts stay, so its machines stand
	// where they were.
	int Plane(const Options& options, const std::string& map) {
		TracedMap traced;
		std::vector<MapEntity> entities;
		PlaneWorld plane;
		if (!LoadTracedMap(options, map, traced) || !ReadEntityList(traced, map, entities)
			|| !MakePlaneWorld(traced, map, options.floor.value_or(0.0f), plane) || !MakeGfxWorld(traced, map, options, plane)) {
			return 1;
		}
		const float height = plane.height;

		// The floor: a grid of the tile model around the player spawns, top face on the collision.
		const std::span<const std::uint8_t> tileRoot = traced.Root(0x06, options.tile, 232);
		if (tileRoot.empty()) {
			std::fprintf(stderr, "no xmodel %016llx stored in %s\n", static_cast<unsigned long long>(options.tile), map.c_str());
			return 1;
		}
		const std::array<float, 3> mins = { Get<float>(tileRoot, 184), Get<float>(tileRoot, 188), Get<float>(tileRoot, 192) };
		const std::array<float, 3> maxs = { Get<float>(tileRoot, 196), Get<float>(tileRoot, 200), Get<float>(tileRoot, 204) };
		const float sizeX = maxs[0] - mins[0], sizeY = maxs[1] - mins[1];
		std::array<float, 2> center{};
		std::size_t spawns = 0;
		const MapEntity* templ = nullptr;
		std::uint32_t lastId = 0;
		for (const MapEntity& entity : entities) {
			lastId = std::max(lastId, entity.id);
			if (entity.Text("targetname") == "initial_spawn_points") {
				center[0] += entity.origin[0];
				center[1] += entity.origin[1];
				++spawns;
			}
			if (!templ && entity.Text("classname") == "script_model" && entity.Find("model") && entity.Find("origin")
				&& entity.Find("angles")) {
				templ = &entity;
			}
		}
		if (!spawns || !templ || sizeX < 16 || sizeY < 16) {
			std::fprintf(stderr, "no player spawns, no script_model to copy, or a tile model under 16 units\n");
			return 1;
		}
		center[0] /= spawns;
		center[1] /= spawns;
		std::vector<MapEntity> floor;
		const int half = options.tiles / 2;
		for (int i = -half; i <= half; ++i) {
			for (int j = -half; j <= half; ++j) {
				MapEntity tile = *templ;
				std::erase_if(tile.keys, [](const EntityKey& key) {
					return key.key != "classname" && key.key != "model" && key.key != "origin" && key.key != "angles";
				});
				for (EntityKey& key : tile.keys) {
					if (key.key == "model") {
						key.hash = options.tile;
						PutAt(std::span<std::uint8_t>(key.raw), 8, options.tile);
					}
					else if (key.key == "angles") {
						key.vector = {};
					}
				}
				tile.id = ++lastId;
				tile.angles = {};
				// Tile (i, j) covers center + [i - 1/2, i + 1/2] tile sizes, whatever the model's own origin.
				const std::array<float, 3> origin = {
					center[0] + (i - 0.5f) * sizeX - mins[0],
					center[1] + (j - 0.5f) * sizeY - mins[1],
					height - maxs[2],
				};
				SetEntityOrigin(tile, origin, 0.0f);
				floor.push_back(std::move(tile));
			}
		}
		std::printf("  floor: %zu x %016llx (%.0f x %.0f) around the spawns at %.0f %.0f\n", floor.size(),
			static_cast<unsigned long long>(options.tile), sizeX, sizeY, center[0], center[1]);
		entities.insert(entities.end(), floor.begin(), floor.end());
		if (!ApplyMoves(options, entities)) {
			return 1;
		}

		std::vector<ZoneAsset> replacements;
		AddPlaneWorldAssets(map, plane, replacements);
		replacements.push_back(EntityListAsset(map, entities));

		const int failed = WriteOverrideZones(options, map, replacements, entities.size(), map);
		std::printf("\n%s. The zones hold copies of your own game data for testing on this PC: never share them.\n",
			failed ? "FAILED" : "Done");
		return failed ? 1 : 0;
	}

	// An .mkmap perk (mkmap-format.md) as the script_noteworthy of its zm_perk_machine struct.
	const char* PerkNoteworthy(const std::string& perk) {
		static const std::pair<const char*, const char*> kPerks[] = {
			{ "juggernog", "talent_juggernog" }, { "speed_cola", "talent_speedcola" },
			{ "quick_revive", "talent_quickrevive" }, { "stamin_up", "talent_staminup" },
			{ "deadshot_dealer", "talent_deadshot" }, { "elemental_pop", "talent_elemental_pop" },
			{ "tombstone_soda", "talent_tombstone" }, { "mule_kick", "talent_mulekick" },
			{ "phd_flopper", "talent_phdslider" }, { "death_perception", "talent_deathperception" },
		};
		for (const auto& [name, noteworthy] : kPerks) {
			if (perk == name) {
				return noteworthy;
			}
		}
		return nullptr;
	}

	// The drawn brushes: one static model (model_writer.hpp) holding every rendered brush's six faces, placed by
	// one script_model at the source's origin, so its vertices are the source's own coordinates.
	// A brush's material is a game material by name ("" = kDefaultBrushMaterial); it must be one <map>'s zone
	// stores, since the model links it by name at load.
	constexpr const char* kDefaultBrushMaterial = "mc/mtl_p7_cinder_block";
	// World units per texture repeat (planar mapping on each face).
	constexpr float kBrushTextureSize = 128.0f;
	// The collision the model links by name: the empty xcollision p8_zm_zod_pap_plinth_sequence_air and many others
	// link (zm_silver asset 11). Its skeleton is its own (AddStaticModelAssets): until 2026-09-26 it was the
	// plinth's, whose bone info (the plinth's bounds) made the model vanish as the camera moved.
	constexpr std::uint64_t kBrushModelCollision = 0x0CDB7A9417DA402Eull;

	std::array<float, 3> Cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
		return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
	}
	float Dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	}
	float Length(const std::array<float, 3>& a) {
		return std::sqrt(Dot(a, a));
	}
	std::array<float, 3> Scaled(const std::array<float, 3>& a, float s) {
		return { a[0] * s, a[1] * s, a[2] * s };
	}

	// A brush's six faces as one surface: four vertices each (flat normals), UVs planar along the face's two
	// half-axes, triangles clockwise seen from outside (as retail's are).
	StaticSurface BrushSurface(const MapKit::MkBrush& brush, std::uint64_t material) {
		StaticSurface surface;
		surface.material = material;
		const auto& h = brush.halfAxes;
		for (int k = 0; k < 3; ++k) {
			const std::array<float, 3>& aj = h[(k + 1) % 3];
			const std::array<float, 3>& al = h[(k + 2) % 3];
			const float lj = Length(aj), ll = Length(al);
			if (lj <= 0 || ll <= 0) {
				continue;
			}
			for (const float side : { -1.0f, 1.0f }) {
				const std::array<float, 3> out = Scaled(h[k], side);
				std::array<float, 3> normal = Cross(aj, al);
				const float nl = Length(normal);
				if (nl <= 0) {
					continue;
				}
				normal = Scaled(normal, (Dot(normal, out) < 0 ? -1.0f : 1.0f) / nl);
				const std::array<float, 3> tangent = Scaled(aj, 1.0f / lj);
				const std::array<float, 3> bitangent = Scaled(al, 1.0f / ll);
				const float sign = Dot(Cross(normal, tangent), bitangent) < 0 ? -1.0f : 1.0f;
				const auto base = static_cast<std::uint16_t>(surface.vertices.size());
				// Corners (-,-), (+,-), (+,+), (-,+) along (aj, al).
				for (const auto [sj, sl] : { std::pair{ -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } }) {
					StaticVertex v;
					for (int axis = 0; axis < 3; ++axis) {
						v.position[axis] = brush.center[axis] + out[axis] + sj * aj[axis] + sl * al[axis];
					}
					v.normal = normal;
					v.tangent = tangent;
					v.bitangentSign = sign;
					v.uv = { (sj + 1.0f) * lj / kBrushTextureSize, (sl + 1.0f) * ll / kBrushTextureSize };
					surface.vertices.push_back(v);
				}
				// (0, 1, 2) winds along cross(aj, al): reverse it when that faces out, so the front is clockwise.
				if (Dot(Cross(aj, al), normal) > 0) {
					surface.triangles.push_back({ base, static_cast<std::uint16_t>(base + 2), static_cast<std::uint16_t>(base + 1) });
					surface.triangles.push_back({ base, static_cast<std::uint16_t>(base + 3), static_cast<std::uint16_t>(base + 2) });
				}
				else {
					surface.triangles.push_back({ base, static_cast<std::uint16_t>(base + 1), static_cast<std::uint16_t>(base + 2) });
					surface.triangles.push_back({ base, static_cast<std::uint16_t>(base + 2), static_cast<std::uint16_t>(base + 3) });
				}
			}
		}
		return surface;
	}

	// --- the creator's own meshes (MkMesh, P3 2026-09-28) ------------------------------------------------------
	// How deep a face's collision slab reaches behind it (TriangleHull, MkMesh collision "faces").
	constexpr float kMeshFaceThickness = 4.0f;

	std::array<float, 3> Minus(const std::array<float, 3>& a, const std::array<float, 3>& b) {
		return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
	}

	// A mesh surface's triangles with their front clockwise, the game's winding (the front normal is
	// cross(c - a, b - a)). The editor exports them so; when the vertex normals say most face the other way (a tool that
	// wrote the other winding), every triangle is swapped and `swapped` says so.
	std::vector<std::array<std::uint32_t, 3>> FrontClockwise(const MapKit::MkMeshSurface& surface, bool& swapped) {
		std::size_t agree = 0, disagree = 0;
		if (!surface.normals.empty()) {
			for (const auto& t : surface.triangles) {
				const auto& a = surface.vertices[t[0]];
				const std::array<float, 3> front = Cross(Minus(surface.vertices[t[2]], a), Minus(surface.vertices[t[1]], a));
				std::array<float, 3> n{};
				for (const std::uint32_t index : t) {
					for (int k = 0; k < 3; ++k) {
						n[k] += surface.normals[index][k];
					}
				}
				++(Dot(front, n) >= 0 ? agree : disagree);
			}
		}
		swapped = disagree > agree;
		std::vector<std::array<std::uint32_t, 3>> triangles = surface.triangles;
		if (swapped) {
			for (auto& t : triangles) {
				std::swap(t[1], t[2]);
			}
		}
		return triangles;
	}

	// A mesh surface as static surfaces of at most 65535 vertices each (split between triangles). Normals are the
	// surface's, else from its faces; tangents follow the UVs (any direction across the normal where they give none).
	std::vector<StaticSurface> MeshSurfaces(const MapKit::MkMeshSurface& surface,
		const std::vector<std::array<std::uint32_t, 3>>& triangles, std::uint64_t material) {
		const std::size_t count = surface.vertices.size();
		std::vector<std::array<float, 3>> normals = surface.normals;
		if (normals.empty()) {
			normals.assign(count, {});
			for (const auto& t : triangles) {
				const auto& a = surface.vertices[t[0]];
				const std::array<float, 3> front = Cross(Minus(surface.vertices[t[2]], a), Minus(surface.vertices[t[1]], a));
				for (const std::uint32_t index : t) {
					for (int k = 0; k < 3; ++k) {
						normals[index][k] += front[k];
					}
				}
			}
		}
		std::vector<std::array<float, 3>> tangents(count), bitangents(count);
		if (!surface.uvs.empty()) {
			for (const auto& t : triangles) {
				const auto e1 = Minus(surface.vertices[t[1]], surface.vertices[t[0]]);
				const auto e2 = Minus(surface.vertices[t[2]], surface.vertices[t[0]]);
				const float du1 = surface.uvs[t[1]][0] - surface.uvs[t[0]][0], dv1 = surface.uvs[t[1]][1] - surface.uvs[t[0]][1];
				const float du2 = surface.uvs[t[2]][0] - surface.uvs[t[0]][0], dv2 = surface.uvs[t[2]][1] - surface.uvs[t[0]][1];
				const float r = du1 * dv2 - du2 * dv1;
				if (std::abs(r) < 1e-12f) {
					continue;
				}
				for (const std::uint32_t index : t) {
					for (int k = 0; k < 3; ++k) {
						tangents[index][k] += (e1[k] * dv2 - e2[k] * dv1) / r;
						bitangents[index][k] += (e2[k] * du1 - e1[k] * du2) / r;
					}
				}
			}
		}
		std::vector<StaticVertex> vertices(count);
		for (std::size_t i = 0; i < count; ++i) {
			StaticVertex& v = vertices[i];
			v.position = surface.vertices[i];
			const float nl = Length(normals[i]);
			v.normal = nl > 1e-6f ? Scaled(normals[i], 1.0f / nl) : std::array<float, 3>{ 0.0f, 0.0f, 1.0f };
			std::array<float, 3> tangent = Minus(tangents[i], Scaled(v.normal, Dot(v.normal, tangents[i])));
			if (Length(tangent) < 1e-6f) {
				tangent = Cross(v.normal, std::abs(v.normal[2]) < 0.9f ? std::array<float, 3>{ 0.0f, 0.0f, 1.0f }
					: std::array<float, 3>{ 1.0f, 0.0f, 0.0f });
			}
			v.tangent = Scaled(tangent, 1.0f / Length(tangent));
			v.bitangentSign = Dot(Cross(v.normal, v.tangent), bitangents[i]) < 0 ? -1.0f : 1.0f;
			if (!surface.uvs.empty()) {
				v.uv = surface.uvs[i];
			}
		}
		std::vector<StaticSurface> out;
		StaticSurface current;
		current.material = material;
		std::unordered_map<std::uint32_t, std::uint16_t> local;
		for (const auto& t : triangles) {
			if (current.vertices.size() + 3 > 0xFFFF) {
				out.push_back(std::move(current));
				current = StaticSurface{};
				current.material = material;
				local.clear();
			}
			std::array<std::uint16_t, 3> triangle{};
			for (int k = 0; k < 3; ++k) {
				auto it = local.find(t[k]);
				if (it == local.end()) {
					it = local.emplace(t[k], static_cast<std::uint16_t>(current.vertices.size())).first;
					current.vertices.push_back(vertices[t[k]]);
				}
				triangle[k] = it->second;
			}
			current.triangles.push_back(triangle);
		}
		if (!current.triangles.empty()) {
			out.push_back(std::move(current));
		}
		return out;
	}

	// The meshes' collision, by MkMesh collision: "faces" = one TriangleHull per triangle (exact, concave shapes
	// included), "hull" = one MeshHull per part (a mesh under the node), "none" = walk-through. `lowest` takes the
	// lowest hull point, for the safety floor.
	template <typename Place>
	void MeshHulls(const MapKit::MkMap& source, const Place& place, std::vector<ConvexHull>& hulls, float& lowest) {
		constexpr std::array<std::array<float, 3>, 3> kIdentity = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
		for (const MapKit::MkMesh& mesh : source.meshes) {
			if (mesh.collision == "none") {
				std::printf("  mesh %s: collision none (walk-through)\n", mesh.path.c_str());
				continue;
			}
			const bool faces = mesh.collision != "hull";
			const std::size_t first = hulls.size();
			std::size_t skipped = 0, planes = 0;
			for (const MapKit::MkMeshPart& part : mesh.parts) {
				if (!faces) {
					std::vector<std::array<float, 3>> points;
					for (const MapKit::MkMeshSurface& surface : part.surfaces) {
						points.insert(points.end(), surface.vertices.begin(), surface.vertices.end());
					}
					ConvexHull hull;
					std::string error;
					if (!points.empty() && MeshHull(points, kIdentity, 1.0f, place({ 0.0f, 0.0f, 0.0f }), 1, hull, error)) {
						hulls.push_back(std::move(hull));
					}
					else {
						std::printf("  mesh %s: no hull (%s)\n", part.path.c_str(), points.empty() ? "no points" : error.c_str());
						++skipped;
					}
					continue;
				}
				for (const MapKit::MkMeshSurface& surface : part.surfaces) {
					bool swapped = false;
					for (const auto& t : FrontClockwise(surface, swapped)) {
						const auto& a = surface.vertices[t[0]];
						const auto& b = surface.vertices[t[1]];
						const auto& c = surface.vertices[t[2]];
						ConvexHull hull;
						std::string error;
						if (TriangleHull(place(a), place(b), place(c), Cross(Minus(c, a), Minus(b, a)), kMeshFaceThickness, 1, hull, error)) {
							hulls.push_back(std::move(hull));
						}
						else {
							++skipped;
						}
					}
				}
			}
			for (std::size_t i = first; i < hulls.size(); ++i) {
				lowest = std::min(lowest, hulls[i].mins[2]);
				planes += hulls[i].planes.size();
			}
			std::printf("  mesh %s: %zu part(s), collision %s: %zu hulls in the world tree (%zu extra planes)%s%s\n",
				mesh.path.c_str(), mesh.parts.size(), faces ? "faces" : "hull", hulls.size() - first, planes,
				skipped ? std::format(", {} skipped ({})", skipped, faces ? "faces with no area" : "see above").c_str() : "",
				mesh.collision != "faces" && mesh.collision != "hull" ? std::format(" ('{}' is no mode: faces used)", mesh.collision).c_str() : "");
		}
	}

	// The name hashes of the materials <map>'s zone stores as assets of their own, sorted: the ones a model mapkit
	// writes can link by name.
	std::vector<std::uint64_t> StoredMaterials(const TracedMap& traced) {
		std::vector<std::uint64_t> stored;
		for (std::size_t i = 0; i < traced.list.assets.size(); ++i) {
			if (traced.list.assets[i].type == 0x0A && traced.list.assets[i].header == kPtrInline) {
				stored.push_back(Get<std::uint64_t>(std::span<const std::uint8_t>(traced.zone.stream).subspan(traced.trace.assets[i].offset, 8), 0)
					& ~(1ull << 63));
			}
		}
		std::ranges::sort(stored);
		return stored;
	}

	// A static model's assets, in the order they must be written: its own skeleton (model.skeleton names it), a
	// by-name reference for its collision and each material (once per zone), then its LOD, then the model.
	void AddStaticModelAssets(StaticModel model, const std::string& label, std::vector<ZoneAsset>& out) {
		const auto reference = [&out](std::uint64_t type, std::uint64_t name, std::size_t size, std::uint64_t alignment) {
			const bool have = std::ranges::any_of(out, [&](const ZoneAsset& a) { return a.type == type && a.linkName == name; });
			if (have) {
				return;
			}
			ZoneAsset asset;
			asset.type = type;
			asset.linkName = name;
			asset.byName = true;
			asset.encode = [type, name, size, alignment](XWriter& w) { EncodeAssetReference(w, type, name, size, alignment); };
			out.push_back(std::move(asset));
		};
		auto shared = std::make_shared<StaticModel>(std::move(model));
		ZoneAsset skeleton;
		skeleton.type = 0x08;
		skeleton.label = "xskeleton (" + label + ")";
		skeleton.linkName = shared->skeleton;
		skeleton.scriptStrings = { shared->boneName };
		skeleton.encode = [shared](XWriter& w) { EncodeStaticSkeleton(w, *shared); };
		out.push_back(std::move(skeleton));
		reference(0x07, shared->collision, 96, 8);
		for (const StaticSurface& surface : shared->surfaces) {
			reference(0x0A, surface.material, 344, 16);
		}
		ZoneAsset mesh;
		mesh.type = 0x09;
		mesh.label = "xmodelmesh (" + label + ")";
		mesh.linkName = shared->meshName;
		mesh.encode = [shared](XWriter& w) { EncodeStaticModelMesh(w, *shared); };
		out.push_back(std::move(mesh));
		ZoneAsset xmodel;
		xmodel.type = 0x06;
		xmodel.label = "xmodel (" + label + ")";
		xmodel.linkName = shared->name;
		xmodel.encode = [shared](XWriter& w) { EncodeStaticModel(w, *shared); };
		out.push_back(std::move(xmodel));
	}

	// --- the creator's own textures (P4 steps 1 to 3; material_writer.hpp) -----------------------------------------
	// A surface whose editor material is no game material draws with its own look: its colour texture (or a swatch
	// of its colour; the editor bakes its tint in), normal, roughness and metal maps, exported by the editor as .dds
	// files. Each becomes a resident image, and each combination a material copied from a retail one of its kind with
	// the creator's images in place of its own.
	//
	// The kinds, from Die Maschine's materials compared offline (ffinfo --material-table, 2026-09-28): the root's
	// +330/+331 is the draw class (02 00 opaque, 06 01 decal, 1A 14 see-through, 09 13 emissive), +324 02 marks a
	// cut-out (an alpha test on the colour map's alpha), +36 0x10 glass, +32 >> 20 the surface type (5 concrete,
	// 9 glass, 13 metal, 21 wood, 24 plastic), and the techset picks the image slots. Each kind's templates share the
	// opaque one's slots (plus the metal map), and their other slots link the neutral shared images (below), else the
	// template's own ambient occlusion or reveal mask would show on the creator's surface.
	enum Look : std::size_t { kLookOpaque, kLookMetal, kLookClip, kLookClipMetal, kLookBlend, kLookGlow, kLookCount };
	struct LookTemplates {
		const char* kind;
		std::array<const char*, 2> materials; // the first the base map can copy
	};
	constexpr LookTemplates kLookTemplates[kLookCount] = {
		// techset F79F5E9BABE46117 (301 users); the pillar (step 1's) has no roughness map
		{ "opaque", { "mc/mtl_com_trash_props_iw6", "mc/mtl_p7_concrete_pillar_damage" } },
		// 181EC03271DE722E: + a metal map; surface type metal
		{ "metal", { "mc/mtl_p7_debris_metal_scrap_04", "mc/t7_metal_bare_iron_matte" } },
		// 2FE9627E1FFFE887: cut out (+324 02 04); wood, paper
		{ "cut-out", { "mc/mtl_greece_classic_furniture_brown_destory", "mc/mtl_trash_debris_paper_01_alt_forms_burnt" } },
		// 20F0DC7D0A756A6D: cut out, + a metal map
		{ "metal cut-out", { "mc/mtl_p9_rus_machine_generator_02_strainer", "mc/mtl_p7_corrugated_sheet_metal_rust_dmg" } },
		// 5CC72245358C6B13 (Die Maschine's commonest glass), 1CCDF3322003E6A8: see-through (1A 14), glass
		{ "see-through", { "mc/mtl_p9_zm_ndu_ieu_locker_glass", "mc/mtl_wpn_t7_zmb_zod_rocket_shield_glass" } },
		// 2563079AB548D2FD (12 users): opaque with an emissive map (09 13); Die Maschine's lit signs, plastic
		{ "glowing", { "mc/mtl_p9_zm_ndu_sign_do_not_enter_light", "mc/mtl_p9_zm_ndu_sign_medical_lab_light" } },
	};
	// A kind without a template the base map can copy draws as the next simpler one (kLookCount: none left).
	constexpr Look kLookFallback[kLookCount] = { kLookCount, kLookOpaque, kLookOpaque, kLookClip, kLookOpaque, kLookOpaque };

	constexpr std::uint32_t kSemanticColor = 0xA0AB1041;     // colorMap0
	constexpr std::uint32_t kSemanticNormal = 0x59D30D0F;    // normalMap0
	constexpr std::uint32_t kSemanticRoughness = 0xE9817F0D; // perceptualRoughnessMap
	// specColorMap0: on these techsets a BC4 metalness (0 not metal, 1 metal; Die Maschine's paper averages 0.05,
	// its Pack-a-Punch frame 0.33); other techsets take an sRGB specular colour (BC1) there instead.
	constexpr std::uint32_t kSemanticMetal = 0xEC443804;
	// emissiveMap0: the light a surface gives off, an sRGB colour (BC1/BC7, usage 2, as the colour map's).
	constexpr std::uint32_t kSemanticEmissive = 0x34614347;
	// The glow's strength in the glowing template's constants (a float; 2026-09-28, compared over the 13 materials of
	// techset 2563079AB548D2FD): Die Maschine's lit signs 32, the Tempest lure lights 8, the antenna lights 64, a Russian
	// computer 256, the mutated fungus 2. Offsets 4..12 hold the glow's tint (1, 1, 1 on the signs; red on the red
	// lights), left as the template's: the editor bakes the colour into the emissive map. A guess from the data, to be
	// confirmed in the game.
	constexpr std::size_t kGlowStrengthOffset = 292;
	// The image slots mapkit leaves linked, and the shared image each must link: ambient occlusion 07176BF2
	// ($white_ao, none), reveal mask 199A03D3 ($white_reveal), thermal 389DD40F ($gray_32_one_channel).
	constexpr std::pair<std::uint32_t, std::uint64_t> kNeutralImages[] = {
		{ 0x07176BF2, 0x3FD1EB9D1E41B805 }, { 0x199A03D3, 0x2BABF948C70FE584 }, { 0x389DD40F, 0x3317746176DD0633 },
	};
	// The image header's usage byte (+180) per kind, as Die Maschine's.
	constexpr std::uint8_t kUsageColor = 2, kUsageNormal = 4, kUsageMetal = 5, kUsageRoughness = 7;
	// The flat normal map: x, y at 0.5; b (the roughness a mip level's spread of normals adds) 0; a 1.
	constexpr std::array<std::uint8_t, 4> kFlatNormal = { 128, 128, 0, 255 };
	// A surface with no roughness map is fully rough (glTF's default roughness).
	constexpr std::uint8_t kDefaultRoughness = 255;
	constexpr std::uint8_t kNoMetal = 0;

	class OwnMaterials {
	public:
		OwnMaterials(const TracedMap& traced, const std::string& map, fs::path sourceDir)
			: m_Traced(traced), m_Map(map), m_SourceDir(std::move(sourceDir)) {}

		// The material drawing a surface's own look (its colour texture; its normal, roughness and metal maps when it
		// has them; its alpha), its assets queued on first use. 0 when it cannot be made (said why).
		std::uint64_t For(const MapKit::MkMeshSurface& surface) {
			const std::string key = std::format("{}|{}|{}|{}|{}|{}|{}", surface.colorTexture, surface.normalTexture,
				surface.roughnessTexture, surface.metalTexture, surface.alpha, surface.emissionTexture, surface.emissionEnergy);
			if (const auto found = m_Materials.find(key); found != m_Materials.end()) {
				return found->second;
			}
			std::uint64_t& material = m_Materials[key];
			const auto color = Texture(surface.colorTexture, kUsageColor);
			if (!color) {
				std::printf("  surface drawn with the fallback material\n");
				return material = 0;
			}
			std::optional<ReplacedImage> metal;
			if (!surface.metalTexture.empty()) {
				metal = Texture(surface.metalTexture, kUsageMetal); // one that cannot be read: not metal
			}
			// A glowing surface is opaque and not metal: the glowing template has neither an alpha test nor a metal map.
			std::optional<ReplacedImage> glow;
			if (!surface.emissionTexture.empty() && surface.alpha != "opaque") {
				std::printf("  surface %s: its glow is dropped (a cut-out or see-through surface does not glow yet)\n",
					surface.colorTexture.c_str());
			}
			else if (!surface.emissionTexture.empty()) {
				glow = Texture(surface.emissionTexture, kUsageColor);
			}
			const Look wanted = glow ? kLookGlow
				: surface.alpha == "blend" ? kLookBlend
				: surface.alpha == "clip" ? (metal ? kLookClipMetal : kLookClip)
				: (metal ? kLookMetal : kLookOpaque);
			Look look = wanted;
			while (look != kLookCount && !Template(look)) {
				look = kLookFallback[look];
			}
			if (look == kLookCount) {
				std::printf("  surface drawn with the fallback material\n");
				return material = 0;
			}
			const MaterialCopy& copyOf = *m_Templates[look];

			std::optional<ReplacedImage> normal;
			if (!surface.normalTexture.empty()) {
				normal = Texture(surface.normalTexture, kUsageNormal);
			}
			normal = normal ? normal : Solid("flat_normal", 28 /* R8G8B8A8_UNORM */, kUsageNormal, kFlatNormal);
			std::optional<ReplacedImage> roughness; // none when the template has no roughness map
			if (HasSemantic(copyOf, kSemanticRoughness)) {
				if (!surface.roughnessTexture.empty()) {
					roughness = Texture(surface.roughnessTexture, kUsageRoughness);
				}
				roughness = roughness ? roughness : Solid("full_roughness", 61 /* R8_UNORM */, kUsageRoughness, std::span(&kDefaultRoughness, 1));
			}
			const bool metalSlot = HasSemantic(copyOf, kSemanticMetal);
			if (metalSlot && !metal) {
				metal = Solid("no_metal", 61 /* R8_UNORM */, kUsageMetal, std::span(&kNoMetal, 1));
			}

			MaterialCopy copy = copyOf;
			ReplaceMaterialImage(copy, kSemanticColor, *color);
			ReplaceMaterialImage(copy, kSemanticNormal, *normal);
			if (roughness) {
				ReplaceMaterialImage(copy, kSemanticRoughness, *roughness);
			}
			if (metalSlot) {
				ReplaceMaterialImage(copy, kSemanticMetal, *metal);
			}
			std::string glowText = "(none)";
			if (glow && look == kLookGlow) {
				ReplaceMaterialImage(copy, kSemanticEmissive, *glow);
				glowText = surface.emissionTexture;
				if (copy.constants.size() >= kGlowStrengthOffset + 4) {
					const float base = Get<float>(std::span<const std::uint8_t>(copy.constants), kGlowStrengthOffset);
					const float strength = base * std::max(surface.emissionEnergy, 0.0f);
					PutAt(std::span<std::uint8_t>(copy.constants), kGlowStrengthOffset, strength);
					glowText += std::format(" at strength {:g} (the template's {:g} x energy {:g})", strength, base, surface.emissionEnergy);
				}
				else {
					glowText += " at the template's strength (its constants are too short)";
				}
			}
			else if (glow) {
				glowText = "(dropped: no glowing template)";
			}
			const std::size_t n = m_MaterialCount++;
			material = HashName(std::format("mapkit_{}_mtl_{}", m_Map, n));
			std::printf("  material mapkit_%s_mtl_%zu (%s%s): colour %s, normal %s, roughness %s, metal %s, glow %s\n", m_Map.c_str(), n,
				kLookTemplates[look].kind, look == wanted ? "" : std::format(", wanted {}", kLookTemplates[wanted].kind).c_str(),
				surface.colorTexture.c_str(), surface.normalTexture.empty() ? "(flat)" : surface.normalTexture.c_str(),
				!roughness ? "(template has none)" : surface.roughnessTexture.empty() ? "(full)" : surface.roughnessTexture.c_str(),
				surface.metalTexture.empty() ? "(none)" : !metalSlot ? "(template has none)" : surface.metalTexture.c_str(),
				glowText.c_str());

			for (const MaterialCopy::TableImage& image : copy.images) {
				Reference(0x10, image.image, 208);
			}
			for (const auto& set : copy.sets) {
				for (const std::uint64_t image : set ? set->entryImages : std::vector<std::uint64_t>{}) {
					Reference(0x10, image, 208);
				}
			}
			ZoneAsset materialAsset;
			materialAsset.type = 0x0A;
			materialAsset.label = std::format("material {}", n);
			materialAsset.linkName = material;
			materialAsset.encode = [copy = std::make_shared<MaterialCopy>(std::move(copy)), material](XWriter& w) {
				EncodeMaterialCopy(w, *copy, material, static_cast<std::uint32_t>(material));
			};
			m_Assets.push_back(std::move(materialAsset));
			return material;
		}

		// Every asset the materials need, in writing order (links before what links them).
		std::vector<ZoneAsset>& Assets() { return m_Assets; }

	private:
		static const char* DxgiLabel(std::uint32_t format) {
			switch (format) {
			case 72: return "BC1 sRGB";
			case 75: return "BC2 sRGB";
			case 78: return "BC3 sRGB";
			case 99: return "BC7 sRGB";
			case 98: return "BC7";
			case 29: return "RGBA8 sRGB";
			case 28: return "RGBA8";
			case 91: return "BGRA8 sRGB";
			case 80: return "BC4";
			case 83: return "BC5";
			case 61: return "R8";
			default: return "?";
			}
		}

		// The resident image of one exported texture (a .dds path relative to the source), queued on first use.
		// A colour map holds sRGB colours (the editor's are): it gets its format's sRGB form; the others are data.
		std::optional<ReplacedImage> Texture(const std::string& texture, std::uint8_t usage) {
			const std::string key = std::format("{}:{}", usage, texture);
			if (const auto found = m_Images.find(key); found != m_Images.end()) {
				return found->second;
			}
			std::optional<ReplacedImage>& out = m_Images[key];
			DdsImage dds;
			std::string error;
			if (!ReadDds(m_SourceDir / texture, dds, error)) {
				std::printf("  texture %s: %s\n", texture.c_str(), error.c_str());
				return out;
			}
			const std::size_t n = m_ImageCount++;
			ResidentImage image;
			image.name = HashName(std::format("mapkit_{}_tex_{}", m_Map, n));
			image.format = dds.format;
			if (usage == kUsageColor) {
				const std::pair<std::uint32_t, std::uint32_t> srgb[] = { { 71, 72 }, { 74, 75 }, { 77, 78 }, { 98, 99 }, { 28, 29 }, { 87, 91 } };
				for (const auto& [linear, gamma] : srgb) {
					image.format = image.format == linear ? gamma : image.format;
				}
			}
			image.width = static_cast<std::uint16_t>(dds.width);
			image.height = static_cast<std::uint16_t>(dds.height);
			image.levels = ResidentImageLevels(dds.width, dds.height, dds.levels);
			image.usage = usage;
			image.pixels = std::move(dds.pixels);
			std::printf("  texture %s: %u x %u %s, %u levels (%zu stored), %zu KB -> mapkit_%s_tex_%zu\n", texture.c_str(),
				image.width, image.height, DxgiLabel(image.format), image.levels, static_cast<std::size_t>(dds.levels),
				image.pixels.size() / 1024, m_Map.c_str(), n);
			out = ReplacedImage{ image.name, image.Id() };
			Queue(std::format("image (texture {})", n), std::move(image));
			return out;
		}

		void Queue(std::string label, ResidentImage image) {
			ZoneAsset asset;
			asset.type = 0x10;
			asset.label = std::move(label);
			asset.linkName = image.name;
			asset.encode = [image = std::make_shared<ResidentImage>(std::move(image))](XWriter& w) { EncodeResidentImage(w, *image); };
			m_Assets.push_back(std::move(asset));
		}

		// A 4 x 4 image of one pixel value written by mapkit (mapkit_<map>_<what>), queued on first use.
		ReplacedImage Solid(const char* what, std::uint32_t format, std::uint8_t usage, std::span<const std::uint8_t> pixel) {
			if (const auto found = m_Solids.find(what); found != m_Solids.end()) {
				return found->second;
			}
			ResidentImage image;
			image.name = HashName(std::format("mapkit_{}_{}", m_Map, what));
			image.format = format;
			image.width = image.height = 4;
			image.levels = 1;
			image.usage = usage;
			for (int i = 0; i < 16; ++i) {
				image.pixels.insert(image.pixels.end(), pixel.begin(), pixel.end());
			}
			const ReplacedImage out{ image.name, image.Id() };
			Queue(std::format("image ({})", what), std::move(image));
			return m_Solids[what] = out;
		}

		void Reference(std::uint64_t type, std::uint64_t name, std::size_t size) {
			if (!name || std::ranges::any_of(m_Assets, [&](const ZoneAsset& a) { return a.type == type && a.linkName == name; })) {
				return;
			}
			ZoneAsset asset;
			asset.type = type;
			asset.linkName = name;
			asset.byName = true;
			asset.encode = [type, name, size](XWriter& w) { EncodeAssetReference(w, type, name, size, 8); };
			m_Assets.push_back(std::move(asset));
		}

		// A copy of a template material the base map stores, when its image table has a colour and a normal map and
		// links a neutral shared image in every other slot mapkit does not fill.
		bool CopyTemplate(const char* templateName, MaterialCopy& out, std::string& error) {
			const std::uint64_t name = HashName(templateName);
			for (std::size_t i = 0; i < m_Traced.list.assets.size(); ++i) {
				if (m_Traced.list.assets[i].type == 0x0A && m_Traced.list.assets[i].header == kPtrInline
					&& TracedAssetName(m_Traced.trace, m_Traced.zone.stream, i) == name) {
					if (!CopyMaterial(m_Traced.trace, m_Traced.zone.stream, m_Traced.list, i, out, error)) {
						return false;
					}
					if (!HasSemantic(out, kSemanticColor) || !HasSemantic(out, kSemanticNormal)) {
						error = "its image table has no colour map or no normal map";
						return false;
					}
					for (const MaterialCopy::TableImage& image : out.images) {
						const std::uint32_t semantic = image.Semantic();
						if (semantic == kSemanticColor || semantic == kSemanticNormal || semantic == kSemanticRoughness
							|| semantic == kSemanticMetal || semantic == kSemanticEmissive) {
							continue;
						}
						const auto neutral = std::ranges::find(kNeutralImages, semantic, &std::pair<std::uint32_t, std::uint64_t>::first);
						if (neutral == std::end(kNeutralImages) || (image.image & ~(1ull << 63)) != neutral->second) {
							error = std::format("its image slot {:08X} links {:016X}, not a neutral shared image", semantic, image.image);
							return false;
						}
					}
					return true;
				}
			}
			error = "the base map does not store it";
			return false;
		}

		static bool HasSemantic(const MaterialCopy& copy, std::uint32_t semantic) {
			return std::ranges::any_of(copy.images, [&](const auto& image) { return image.Semantic() == semantic; });
		}

		// The template of a kind, copied on first use (its techset then queued). False when the base map has none.
		bool Template(Look look) {
			if (m_TemplateTried[look]) {
				return m_Templates[look].has_value();
			}
			m_TemplateTried[look] = true;
			for (const char* name : kLookTemplates[look].materials) {
				MaterialCopy copy;
				std::string error;
				if (CopyTemplate(name, copy, error)) {
					Reference(0x0F, copy.techset, 168);
					m_Templates[look] = std::move(copy);
					return true;
				}
				std::printf("  own textures: the %s template %s cannot be copied (%s)\n", kLookTemplates[look].kind, name, error.c_str());
			}
			return false;
		}

		const TracedMap& m_Traced;
		std::string m_Map;
		fs::path m_SourceDir;
		std::array<std::optional<MaterialCopy>, kLookCount> m_Templates;
		std::array<bool, kLookCount> m_TemplateTried{};
		std::map<std::string, ReplacedImage> m_Solids;           // what -> mapkit's own 4 x 4 image
		std::map<std::string, std::uint64_t> m_Materials;       // colour|normal|roughness|metal|alpha -> material
		std::map<std::string, std::optional<ReplacedImage>> m_Images; // usage:texture -> image
		std::size_t m_MaterialCount = 0;
		std::size_t m_ImageCount = 0;
		std::vector<ZoneAsset> m_Assets;
	};

	// The model and the assets it links, in the order they must be written (every link before the model).
	// Empty when the source draws no brush.
	bool AddBrushModel(const MapKit::MkMap& source, const fs::path& sourceDir, const TracedMap& traced,
		std::vector<ZoneAsset>& out, std::uint64_t& modelName) {
		const std::vector<std::uint64_t> stored = StoredMaterials(traced);
		const std::uint64_t fallback = HashName(kDefaultBrushMaterial);
		if (!std::ranges::binary_search(stored, fallback)) {
			std::fprintf(stderr, "the default brush material %s is not in the base map\n", kDefaultBrushMaterial);
			return false;
		}

		StaticModel model;
		model.name = HashName("mapkit_" + source.name);
		model.meshName = HashName("mapkit_" + source.name + "_lod0");
		model.skeleton = HashName("mapkit_" + source.name + "_skeleton");
		model.collision = kBrushModelCollision;
		// Brushes sorted by material, so each material is one surface.
		std::vector<std::pair<std::uint64_t, const MapKit::MkBrush*>> drawn;
		std::size_t unknown = 0;
		for (const MapKit::MkBrush& brush : source.brushes) {
			if (!brush.rendered) {
				continue;
			}
			std::uint64_t material = brush.material.empty() ? fallback : HashName(brush.material);
			if (!std::ranges::binary_search(stored, material)) {
				std::printf("  brush %s: material '%s' is not in the base map, drawn with %s\n", brush.path.c_str(),
					brush.material.c_str(), kDefaultBrushMaterial);
				material = fallback;
				++unknown;
			}
			drawn.emplace_back(material, &brush);
		}
		std::ranges::stable_sort(drawn, {}, [](const auto& entry) { return entry.first; });
		for (const auto& [material, brush] : drawn) {
			AppendSurface(model, BrushSurface(*brush, material));
		}
		// The meshes (MkMesh): a surface's material when it names a game material the base map stores, else the
		// mesh's game_material when it has one, else the editor material's own look (its texture), else the default.
		OwnMaterials own(traced, source.name, sourceDir);
		std::size_t meshParts = 0;
		for (const MapKit::MkMesh& mesh : source.meshes) {
			std::map<std::string, std::string> fellBack; // editor material -> what it drew with
			std::size_t swappedSurfaces = 0;
			const bool meshMaterial = !mesh.gameMaterial.empty() && std::ranges::binary_search(stored, HashName(mesh.gameMaterial));
			for (const MapKit::MkMeshPart& part : mesh.parts) {
				++meshParts;
				for (const MapKit::MkMeshSurface& surface : part.surfaces) {
					std::uint64_t material = HashName(surface.material);
					if (surface.material.empty() || !std::ranges::binary_search(stored, material)) {
						const std::uint64_t textured = meshMaterial || surface.colorTexture.empty() ? 0 : own.For(surface);
						material = meshMaterial ? HashName(mesh.gameMaterial) : textured ? textured : fallback;
						if (!textured) {
							fellBack[surface.material.empty() ? "(none)" : surface.material] = meshMaterial ? mesh.gameMaterial
								: kDefaultBrushMaterial;
						}
					}
					bool swapped = false;
					const auto triangles = FrontClockwise(surface, swapped);
					swappedSurfaces += swapped ? 1 : 0;
					for (const StaticSurface& part_surface : MeshSurfaces(surface, triangles, material)) {
						AppendSurface(model, part_surface);
					}
				}
			}
			for (const auto& [editor, used] : fellBack) {
				std::printf("  mesh %s: material '%s' is no game material the base map has; drawn with %s\n", mesh.path.c_str(),
					editor.c_str(), used.c_str());
			}
			if (!mesh.gameMaterial.empty() && !std::ranges::binary_search(stored, HashName(mesh.gameMaterial))) {
				std::printf("  mesh %s: game_material '%s' is not in the base map\n", mesh.path.c_str(), mesh.gameMaterial.c_str());
			}
			if (swappedSurfaces) {
				std::printf("  mesh %s: %zu surface(s) wound the other way (by their normals); turned round\n", mesh.path.c_str(),
					swappedSurfaces);
			}
		}
		if (model.surfaces.empty()) {
			modelName = 0;
			return true;
		}

		std::size_t vertices = 0, triangles = 0;
		for (const StaticSurface& surface : model.surfaces) {
			vertices += surface.vertices.size();
			triangles += surface.triangles.size();
		}
		const StaticModelBounds bounds = ModelBounds(model);
		std::printf("  drawn: %zu brushes and %zu mesh parts in model mapkit_%s (%016llX), %zu surfaces, %zu vertices, %zu "
			"triangles, bounds (%.0f %.0f %.0f)..(%.0f %.0f %.0f)%s\n", drawn.size(), meshParts, source.name.c_str(),
			static_cast<unsigned long long>(model.name), model.surfaces.size(), vertices, triangles, bounds.mins[0],
			bounds.mins[1], bounds.mins[2], bounds.maxs[0], bounds.maxs[1], bounds.maxs[2],
			unknown ? std::format(", {} with the default material", unknown).c_str() : "");
		modelName = model.name;
		for (ZoneAsset& asset : own.Assets()) {
			out.push_back(std::move(asset));
		}
		AddStaticModelAssets(std::move(model), "brushes", out);
		return true;
	}

	// --- the sky (docs/mapkit-plan.md, "P4 sky and lighting") ------------------------------------------------------
	// Die Maschine's lighting state 0 (day, the one a mapkit map runs in) draws the dome skybox_zm_silver_override (43
	// vertices) with one material, 792207D7B4B8B185 (techset C84D8285C9443CAB). Its colour slot holds an equirectangular
	// panorama, i_mtl_skybox_zm_silver: 8192 x 4096 BC6H_UF16, one level, usage 0x13, the top row straight up and the middle
	// row the horizon (its sun is 27 degrees up; the state's sun, 25). The dome's own UVs do not map it: the shader works
	// out the direction. Constants +160 and +416 hold the brightness, 2^6.5 = 90.5 (the dark sky's 2^0.5, the black sky's
	// 2^-10). Read offline 2026-09-29 (ffinfo --mesh, --material, --dds).
	// mapkit's sky: the creator's panorama as a resident image, and a copy of that material under its own name with the
	// image in place of Die Maschine's. The override zone's copy wins the name, so the dome draws the creator's sky.
	constexpr std::uint64_t kSkyMaterial = 0x792207D7B4B8B185ull;
	constexpr std::size_t kSkyBrightnessOffsets[] = { 160, 416 };
	constexpr std::uint8_t kUsageSky = 0x13;

	void AddSky(const MapKit::MkMap& source, const fs::path& sourceDir, const TracedMap& traced, std::vector<ZoneAsset>& out) {
		if (source.sky.image.empty()) {
			return;
		}
		DdsImage dds;
		std::string error;
		if (!ReadDds(sourceDir / source.sky.image, dds, error)) {
			std::printf("  sky: %s; the base map's sky is kept\n", error.c_str());
			return;
		}
		std::optional<std::size_t> asset;
		for (std::size_t i = 0; i < traced.list.assets.size() && !asset; ++i) {
			if (traced.list.assets[i].type == 0x0A && traced.list.assets[i].header == kPtrInline
				&& TracedAssetName(traced.trace, traced.zone.stream, i) == kSkyMaterial) {
				asset = i;
			}
		}
		MaterialCopy copy;
		if (!asset || !CopyMaterial(traced.trace, traced.zone.stream, traced.list, *asset, copy, error)) {
			std::printf("  sky: the base map's sky material %016llX cannot be copied (%s); its sky is kept\n",
				static_cast<unsigned long long>(kSkyMaterial), asset ? error.c_str() : "the base map does not store it");
			return;
		}
		ResidentImage image;
		image.name = HashName(std::format("mapkit_{}_sky", source.name));
		// A panorama without HDR holds sRGB colours (BC7, or BC1 / RGBA8 when the editor could not compress it); one with
		// HDR is BC6H, as Die Maschine's.
		const std::pair<std::uint32_t, std::uint32_t> srgb[] = { { 71, 72 }, { 98, 99 }, { 28, 29 } };
		image.format = dds.format;
		for (const auto& [linear, gamma] : srgb) {
			image.format = image.format == linear ? gamma : image.format;
		}
		image.width = static_cast<std::uint16_t>(dds.width);
		image.height = static_cast<std::uint16_t>(dds.height);
		// One level, as Die Maschine's: the direction lookup wraps round at one column, where mip levels would draw a seam.
		image.levels = 1;
		image.usage = kUsageSky;
		dds.pixels.resize(ImageLevelBytes(image.format, dds.width, dds.height));
		image.pixels = std::move(dds.pixels);
		ReplaceMaterialImage(copy, kSemanticColor, ReplacedImage{ image.name, image.Id() });
		float brightness = 0.0f;
		for (const std::size_t offset : kSkyBrightnessOffsets) {
			if (copy.constants.size() >= offset + 4) {
				const float base = Get<float>(std::span<const std::uint8_t>(copy.constants), offset);
				brightness = base * std::max(source.sky.energy, 0.0f);
				PutAt(std::span<std::uint8_t>(copy.constants), offset, brightness);
			}
		}
		std::printf("  sky: %s, %u x %u %s -> mapkit_%s_sky, drawn by the base map's day sky material %016llX at brightness "
			"%g (energy %g)\n", source.sky.image.c_str(), image.width, image.height, image.format == 95 ? "BC6H (HDR)" : "sRGB",
			source.name.c_str(), static_cast<unsigned long long>(kSkyMaterial), brightness, source.sky.energy);

		ZoneAsset imageAsset;
		imageAsset.type = 0x10;
		imageAsset.label = "image (sky)";
		imageAsset.linkName = image.name;
		imageAsset.encode = [image = std::make_shared<ResidentImage>(std::move(image))](XWriter& w) { EncodeResidentImage(w, *image); };
		out.push_back(std::move(imageAsset));
		if (!std::ranges::any_of(out, [&](const ZoneAsset& a) { return a.type == 0x0F && a.linkName == copy.techset; })) {
			ZoneAsset techset;
			techset.type = 0x0F;
			techset.linkName = copy.techset;
			techset.byName = true;
			techset.encode = [name = copy.techset](XWriter& w) { EncodeAssetReference(w, 0x0F, name, 168, 8); };
			out.push_back(std::move(techset));
		}
		ZoneAsset material;
		material.type = 0x0A;
		material.label = "material (sky)";
		material.linkName = kSkyMaterial;
		// Under Die Maschine's name and with its material id (+16), so it takes the place of that material.
		const std::uint32_t materialId = Get<std::uint32_t>(std::span<const std::uint8_t>(copy.root), 16);
		material.encode = [copy = std::make_shared<MaterialCopy>(std::move(copy)), materialId](XWriter& w) {
			EncodeMaterialCopy(w, *copy, kSkyMaterial, materialId);
		};
		out.push_back(std::move(material));
	}

	// --- the lighting (docs/mapkit-plan.md, "P4 sky and lighting") ----------------------------------------------------
	// The base map's lighting asset copied whole (zonekit CopyLighting: its bytes, each link re-pointed at a by-name
	// reference), with the map's own sun and fog written into the daytime state (0) of every volume, the state a mapkit
	// map runs in. The copy keeps the base map's name, so it takes that asset's place (the gfx_map links it by name).
	// Since P6 step 2 every map carries its own copy, its states as they are when the map sets no sun or fog (`always`;
	// --library-world copies it only for a sun or fog, as before). The fields: world_assets.hpp (LoadLightingAsset).
	// The copy carries nothing of the base map's level that the map would show (docs/mapkit-plan.md, "The map's own
	// lighting"):
	//   - its lights go black (`keepLights` leaves them): the base map's lamps shone on the map wherever the base map has
	//     them (482 of Die Maschine's 948 lie within 1,500 units of where a map lands);
	//   - its baked shadow trees become empty ones (`keepBakedShadows` leaves them): the sun's shadow of the base map's
	//     own buildings, baked for the base map's sun, drawn in blocks of its texels wherever the shadow maps of the frame
	//     do not reach. `keyEdits` gets the stream keys whose copies must then name the empty tree's data
	//     (AddLibraryCopies); with the base map's zone loaded those keys are the base map's, so the trees stay.
	constexpr std::size_t kStateSunYaw = 0, kStateSunPitch = 4, kStateSunColor = 16, kStateSunIntensity = 28;
	constexpr std::size_t kStateSunPitchOffset = 1320, kStateSunYawOffset = 1324;
	constexpr std::size_t kStateFogStart = 1936, kStateFogBaseHeight = 1940, kStateFogHalfway = 1944,
		kStateFogHalfwayHeight = 1948, kStateFogHalfwayHeight2 = 1956, kStateFogOpacity = 1960, kStateFogColor = 1964,
		kStateFogOn = 1976;
	// A fog the same at every height: thins by half only this high up (the state has no switch for it).
	constexpr float kEvenFogHeight = 1.0e6f;

	bool AddLighting(const MapKit::MkMap& source, const std::array<float, 3>& at, const TracedMap& traced,
		std::vector<ZoneAsset>& out, bool always, bool keepLights, bool keepBakedShadows,
		std::vector<LightingCopy::KeyEdit>& keyEdits) {
		const bool edited = source.lighting && (source.lighting->sun || source.lighting->fog);
		if (!edited && !always) {
			return true;
		}
		std::optional<std::size_t> asset;
		for (std::size_t i = 0; i < traced.list.assets.size() && !asset; ++i) {
			if (traced.list.assets[i].type == 0xAA && traced.list.assets[i].header == kPtrInline) {
				asset = i;
			}
		}
		LightingCopy copy;
		std::string error;
		if (!asset || !CopyLighting(traced.trace, traced.zone.stream, traced.list, *asset, copy, error)) {
			std::printf("  lighting: the base map's cannot be copied (%s); it stays as it is\n",
				asset ? error.c_str() : "it stores none");
			return true;
		}
		// Nothing of the base map's level in it.
		std::string level;
		if (keepLights) {
			level = std::format("its {} lights as they are (--keep-lights)", copy.lightCount);
		}
		else {
			const std::size_t lit = copy.DarkenLights();
			level = std::format("its {} lights black ({} gave light)", copy.lightCount, lit);
		}
		if (keepBakedShadows) {
			level += std::format(", its {} baked shadow trees as they are", copy.shadowTrees.size());
		}
		else {
			std::size_t emptied = 0;
			std::vector<LightingCopy::KeyEdit> keys;
			if (!copy.EmptyShadowTrees(keys, emptied, error)) {
				std::fprintf(stderr, "  lighting: the base map's baked shadow trees cannot be emptied: %s (--keep-baked-shadows builds the "
					"map with them)\n", error.c_str());
				return false;
			}
			level += std::format(", {} of its {} baked shadow trees emptied (the rest are empty ones already)", emptied,
				copy.shadowTrees.size());
			keyEdits.insert(keyEdits.end(), keys.begin(), keys.end());
		}
		std::string what;
		for (std::size_t v = 0; edited && v < copy.volumeCount; ++v) {
			const std::span<std::uint8_t> state = copy.State(v, 0);
			if (state.empty()) {
				continue;
			}
			if (const auto& sun = source.lighting->sun) {
				const auto& d = sun->direction;
				const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				const float x = length > 0 ? d[0] / length : 0.0f, y = length > 0 ? d[1] / length : 0.0f,
					z = length > 0 ? d[2] / length : -1.0f;
				const float pitch = std::asin(std::clamp(-z, -1.0f, 1.0f)) * 57.29578f;
				const float yaw = std::atan2(y, x) * 57.29578f;
				const float intensity = Get<float>(std::span<const std::uint8_t>(state), kStateSunIntensity) * std::max(sun->energy, 0.0f);
				PutAt(state, kStateSunYaw, yaw);
				PutAt(state, kStateSunPitch, pitch);
				PutAt(state, kStateSunPitchOffset, 0.0f);
				PutAt(state, kStateSunYawOffset, 0.0f);
				for (std::size_t c = 0; c < 3; ++c) {
					PutAt(state, kStateSunColor + 4 * c, std::max(sun->color[c], 0.0f));
				}
				PutAt(state, kStateSunIntensity, intensity);
				if (v == 0) {
					what += std::format(", sun travelling toward yaw {:.1f} pitch {:.1f} (the sun {:.1f} up at yaw {:.1f}), colour "
						"{:.2f} {:.2f} {:.2f}, intensity {:g}", yaw, pitch, pitch, std::fmod(yaw + 540.0f, 360.0f), sun->color[0],
						sun->color[1], sun->color[2], intensity);
				}
			}
			if (const auto& fog = source.lighting->fog) {
				const float height = fog->halfwayHeight > 0 ? fog->halfwayHeight : kEvenFogHeight;
				PutAt(state, kStateFogStart, std::max(fog->start, 0.0f));
				PutAt(state, kStateFogBaseHeight, fog->baseHeight + at[2]);
				PutAt(state, kStateFogHalfway, std::max(fog->halfway, 1.0f));
				PutAt(state, kStateFogHalfwayHeight, height);
				PutAt(state, kStateFogHalfwayHeight2, height);
				PutAt(state, kStateFogOpacity, fog->enabled ? std::clamp(fog->opacity, 0.0f, 1.0f) : 0.0f);
				for (std::size_t c = 0; c < 3; ++c) {
					PutAt(state, kStateFogColor + 4 * c, std::max(fog->color[c], 0.0f) * 255.0f);
				}
				PutAt(state, kStateFogOn, fog->enabled ? 1.0f : 0.0f);
				if (v == 0) {
					what += !fog->enabled ? std::string(", no fog")
						: std::format(", fog from {:g} with half at {:g} further, thinning by half every {} up from z {:g}, colour "
							"{:.2f} {:.2f} {:.2f}, opacity {:g}", fog->start, fog->halfway,
							fog->halfwayHeight > 0 ? std::format("{:g}", fog->halfwayHeight) : std::string("(never)"),
							fog->baseHeight + at[2], fog->color[0], fog->color[1], fog->color[2], fog->opacity);
				}
			}
		}

		// Each asset it links, by name (after the zone's own entries of that name, if any).
		std::set<std::pair<std::uint64_t, std::uint64_t>> present;
		for (const ZoneAsset& a : out) {
			if (a.linkName) {
				present.emplace(a.type, a.linkName);
			}
		}
		std::vector<CopiedLink> links;
		std::size_t references = 0;
		for (const LightingCopy::Link& link : copy.links) {
			links.push_back({ link.at, link.type, link.name });
			if (!present.emplace(link.type, link.name).second) {
				continue;
			}
			auto reference = ByNameReference(link.type, link.name);
			if (!reference) {
				std::printf("  lighting: no by-name reference layout for %s; the base map's lighting stays as it is\n",
					std::string(XAssetTypeName(link.type)).c_str());
				keyEdits.clear();
				return true;
			}
			out.push_back(std::move(*reference));
			++references;
		}
		std::vector<CopiedPointer> pointers;
		for (const LightingCopy::Pointer& pointer : copy.pointers) {
			pointers.push_back({ pointer.at, pointer.target });
		}
		if (!edited) {
			what = ", its states as they are (the map sets no sun or fog)";
		}
		std::printf("  lighting: the base map's (%.2f MB, %u volumes, %zu links, %zu pointers into itself, %zu new by-name "
			"references) in its daytime state%s\n", copy.bytes.size() / (1024.0 * 1024.0), copy.volumeCount, copy.links.size(),
			copy.pointers.size(), references, what.c_str());
		std::printf("  lighting: %s\n", level.c_str());
		out.push_back(CopiedAsset(0xAA, edited ? "lighting (own sun and fog)" : "lighting (copied)",
			std::move(copy.bytes), {}, std::move(links), std::move(pointers)));
		return true;
	}

	// --- the level's other world assets (docs/mapkit-plan.md, "P6 step 2") --------------------------------------------
	// A map's world is more than the gfx_map, clip map, streamerworld and districts: the level also finds its terraingfx,
	// placed effects (staticlevelfxlist), glass, primary lights (com_map), occlusion data and navvolume by the map's
	// .d3dbsp name, and its path nodes (game_map) as the one asset of their pool. Until P6 they all came from <map>'s
	// zone. Now the map's zone holds its own (level_assets.hpp), each spliced from <map>'s (asset_record.hpp):
	//   - terraingfx: without its tiles (the drawn ground); its +224 block of map-wide textures stays, since the renderer
	//     binds them whatever the terrain (an empty one crashed every render thread on texture slot 105, 2026-09-26);
	//   - staticlevelfxlist: empty (<map>'s 2,482 effects played at <map>'s places);
	//   - glasses, cpu_occlusion_data, game_map, navvolume: copied whole;
	//   - com_map: copied with every light black (`keepLights` leaves them lit). The client game copies its lights into
	//     its own array when the level starts (Com_CopyPrimaryLightsToCG_cand 0x7FF7295F0520), and they are <map>'s lamps
	//     at <map>'s places. The list keeps its length: the lighting's volumes, and the com_map's own groups, name lights
	//     by their index in it.
	// They keep <map>'s names, so they take the place of <map>'s in the override swap, and what they link (images,
	// streamkeys) stays linked by name. --library-world leaves them all to <map>.
	struct LevelAssetPlan {
		std::uint64_t type = 0;
		std::string label;
		AssetRecord record;
		RecordSplice splice;
		std::vector<RecordLink> links;
		std::vector<std::string> strings;
		std::shared_ptr<const std::vector<std::uint64_t>> names; // splice.assetNames points into it
		std::string error; // set if the encode fails (the zone then does not read back and is not written)
	};

	bool AddLevelWorldAssets(const TracedMap& traced, const std::string& map, bool keepTerrain, bool keepLights,
		std::vector<ZoneAsset>& out) {
		const auto names = std::make_shared<const std::vector<std::uint64_t>>(TracedAssetNames(traced));
		const std::uint64_t bspName = HashName(MapBspName(map));
		std::set<std::pair<std::uint64_t, std::uint64_t>> present;
		for (const ZoneAsset& a : out) {
			if (a.linkName) {
				present.emplace(a.type, a.linkName);
			}
		}
		std::string summary;
		for (const auto& [type, label] : std::initializer_list<std::pair<std::uint64_t, const char*>>{
				{ 0xB1, keepTerrain ? "terraingfx" : "terraingfx (without tiles)" }, { 0x7F, "staticlevelfxlist (empty)" },
				{ 0x43, "glasses" }, { 0x19, keepLights ? "com_map" : "com_map (lights black)" },
				{ 0xA9, "cpu_occlusion_data" }, { 0x1A, "game_map" }, { 0x76, "navvolume" } }) {
			std::size_t index = traced.list.assets.size();
			for (std::size_t i = 0; i < traced.list.assets.size(); ++i) {
				if (traced.list.assets[i].type == type && (*names)[i] == bspName) {
					index = i;
					break;
				}
			}
			if (index == traced.list.assets.size()) {
				std::printf("  %s: %s has none named like its map; the level runs without one\n", label, map.c_str());
				continue;
			}
			auto plan = std::make_shared<LevelAssetPlan>();
			plan->type = type;
			plan->label = label;
			plan->names = names;
			XStream s(traced.zone.stream, 0);
			traced.Start(s, index);
			if (!LevelAssetReader(type)(s, traced.list.assets[index].header, plan->record)) {
				std::fprintf(stderr, "%s: %s\n", label, s.Error().c_str());
				return false;
			}
			RecordSplice& splice = plan->splice;
			splice.stream = traced.zone.stream;
			splice.record = &plan->record;
			// The XAsset array is the last thing block 4 holds before the first asset.
			splice.assetArrayPos = traced.trace.assets[0].pos[XBlockVirtual] - 16 * traced.list.assets.size();
			splice.assets = traced.list.assets;
			splice.assetNames = *names;
			splice.strings = traced.list.strings;
			splice.cuts = type == 0xB1 && !keepTerrain ? TerrainGfxWithoutTilesCuts()
				: type == 0x7F ? EmptyStaticLevelFxListCuts() : std::vector<RecordCut>{};
			std::erase_if(splice.cuts, [&](const RecordCut& cut) { return !plan->record.Subtree(cut.name); });
			if (const RecordSubtree* lights = type == 0x19 && !keepLights ? plan->record.Subtree("lights") : nullptr) {
				// Each light's colour, both copies of it (world_assets.hpp, kLightColor). The root's +12 counts the lights.
				const std::size_t count = Get<std::uint32_t>(std::span<const std::uint8_t>(traced.zone.stream), plan->record.rootAt + 12);
				for (std::size_t i = 0; i < count && lights->begin + kLightSize * (i + 1) <= lights->end; ++i) {
					for (const std::size_t field : { kLightColor, kLightColorAgain }) {
						splice.edits.emplace_back(lights->begin + kLightSize * i + field, std::vector<std::uint8_t>(12));
					}
				}
			}
			std::string error;
			if (!RecordLinks(splice, plan->links, plan->strings, error)) {
				std::fprintf(stderr, "%s: %s\n", label, error.c_str());
				return false;
			}
			std::size_t references = 0;
			for (const RecordLink& link : plan->links) {
				if (!present.emplace(link.type, link.name).second) {
					continue;
				}
				auto reference = ByNameReference(link.type, link.name);
				if (!reference) {
					std::fprintf(stderr, "%s: no by-name reference layout for %s\n", label,
						std::string(XAssetTypeName(link.type)).c_str());
					return false;
				}
				out.push_back(std::move(*reference));
				++references;
			}
			std::size_t cut = 0;
			for (const RecordCut& c : splice.cuts) {
				const RecordSubtree* subtree = plan->record.Subtree(c.name);
				cut += subtree->end - subtree->begin;
			}
			summary += std::format("{}{} {:.2f} MB ({} links, {} new by-name, {} script strings)", summary.empty() ? "" : "; ", label,
				(plan->record.end - plan->record.begin - cut) / (1024.0 * 1024.0), plan->links.size(), references,
				plan->strings.size());
			ZoneAsset asset;
			asset.type = type;
			asset.label = label;
			asset.scriptStrings = plan->strings;
			asset.encode = [plan](XWriter& w) {
				if (!EncodeRecordedAsset(w, plan->splice, LevelAssetReader(plan->type), plan->error)) {
					std::fprintf(stderr, "  %s: %s\n", plan->label.c_str(), plan->error.c_str());
				}
			};
			out.push_back(std::move(asset));
		}
		std::printf("  level world: copies of %s's, under its names: %s\n", map.c_str(), summary.c_str());
		return true;
	}

	// --- the streamed data's packages (docs/mapkit-plan.md, "P6 step 2b-2") ----------------------------------------------
	// Streamed data (a streamed image's larger mip levels, a streamed mesh's buffers) is read from packages: zone\<name>.xpak
	// and its .xsub files, one set per mode (zm, zm_postship, zm_sink) and for core, not one per map. Once a zone has loaded,
	// DB_OpenZonePackages_cand 0x7FF72928AB20 opens the ones its keyvaluepairs asset (named after the zone) lists under
	// "xpak_read". <map>'s lists the zm and zm_postship sets its world streams from, zm_common's only zm_sink and core. So a
	// map that leaves <map>'s zones out carries <map>'s list under its own name, or its copies of <map>'s streamed images
	// keep their smallest levels. A value <map> stores by pointer (its own world's name: no package has it) is left out.
	bool AddPackages(const TracedMap& traced, const std::string& map, const std::string& id, std::vector<ZoneAsset>& out) {
		const std::size_t index = traced.Find(0x4B);
		KeyValuePairsData kvp;
		XStream s(traced.zone.stream, 0);
		if (index == traced.list.assets.size()) {
			std::fprintf(stderr, "  packages: %s has no keyvaluepairs to take its package list from\n", map.c_str());
			return false;
		}
		traced.Start(s, index);
		if (!ReadKeyValuePairs(s, traced.list.assets[index].header, kvp)) {
			std::fprintf(stderr, "  packages: %s's keyvaluepairs: %s\n", map.c_str(), s.Error().c_str());
			return false;
		}
		std::vector<std::pair<std::uint32_t, std::string>> pairs;
		std::size_t byPointer = 0;
		std::string names;
		for (const KeyValuePairsData::Pair& pair : kvp.pairs) {
			if (pair.key != kKeyXPakRead) {
				continue;
			}
			if (pair.shared || pair.value.empty()) {
				++byPointer;
				continue;
			}
			names += " " + pair.value;
			pairs.emplace_back(kKeyXPakRead, pair.value);
		}
		if (pairs.empty()) {
			std::fprintf(stderr, "  packages: %s's keyvaluepairs lists none\n", map.c_str());
			return false;
		}
		std::printf("  packages: %s's list of %zu under the map's name (%zu stored by pointer left out):%s\n", map.c_str(),
			pairs.size(), byPointer, names.c_str());
		ZoneAsset asset;
		asset.type = 0x4B;
		asset.label = "keyvaluepairs (packages)";
		asset.encode = [name = HashName(id), pairs = std::move(pairs)](XWriter& w) { EncodeKeyValuePairs(w, name, pairs); };
		out.push_back(std::move(asset));
		return true;
	}

	// --- what the map's assets link from <map>'s zone (docs/mapkit-plan.md, "P6 step 2b") ------------------------------
	// The copies above link <map>'s images, streamkeys, sky domes and the like by name. Each one <map>'s zone holds itself
	// (not by name there too) is copied as well (library_assets.hpp), with what it links in turn, under <map>'s name: while
	// <map> still loads, the copy takes its place in the override swap; once <map> no longer loads, the copy is the asset.
	// A copy that shares data with an earlier asset of <map>'s (most models, meshes, skeletons and materials do: a fastfile
	// stores identical data once) needs that asset copied ahead of it, and points into its copy (RecordSplice::shared).
	// Everything goes at the front of the zone, each copy ahead of the by-name references to it, which keep linking it by
	// name: an override's own entry ends up holding the loser of the swap. An asset that another asset's load writes into
	// (LoadWritesInto: the lighting's streamed-texture keys) stays <map>'s while <map> loads: the swap would lose the write.
	// Without <map>'s zone (`library` false) nothing is swapped: those are copied too, the copies go after the zone's own
	// assets they link (the creator's sky material, which the copied sky domes draw with: Build puts it first), and an asset
	// left linked to <map>'s zone fails the build, since no loaded zone would hold it.
	struct LibraryCopy {
		std::uint64_t type = 0;
		AssetRecord record;
		RecordSplice splice;
		std::vector<std::string> strings;
		std::shared_ptr<const std::vector<std::uint64_t>> names; // splice.assetNames points into it
		std::string error; // set if the encode fails (the zone then does not read back and is not written)
	};

	// Where each copied chunk landed, looked up by the retail block and position (RecordSplice::shared).
	struct LibraryLandings {
		std::vector<RecordLanding> landings;
		std::map<std::pair<int, std::uint64_t>, std::size_t> byStart;
		std::size_t indexed = 0;

		std::optional<std::uint64_t> Find(std::uint64_t stored) {
			for (; indexed < landings.size(); ++indexed) {
				byStart[{ landings[indexed].block, landings[indexed].pos }] = indexed;
			}
			const std::uint64_t value = stored - 1;
			const int block = static_cast<int>(value >> 60);
			const std::uint64_t pos = value & 0x0FFFFFFFFFFFFFFFull;
			auto found = byStart.upper_bound({ block, pos });
			if (found == byStart.begin()) {
				return std::nullopt;
			}
			const RecordLanding& landing = landings[(--found)->second];
			if (landing.block != block || pos >= landing.pos + std::max<std::uint64_t>(landing.size, 1)) {
				return std::nullopt;
			}
			return XWriter::Reference(landing.newBlock, landing.newPos + (pos - landing.pos));
		}
	};

	// `stays` is what zm_common holds (LoadStayingAssets; empty with `library`), `modelNames` the source's model names by
	// hash, which name the copies that leave something linked to <map>'s zone. `keyEdits` are the stream keys whose copies
	// name other data in the packages than <map>'s (AddLighting: an emptied shadow tree's key names the empty tree's
	// data); a key of those that is not copied fails the build, since the lighting then says one thing and the key another.
	bool AddLibraryCopies(const TracedMap& traced, const std::string& map, const std::set<AssetKey>& stays,
		const std::map<std::uint64_t, std::string>& modelNames, std::vector<ZoneAsset>& out, bool library, bool acoustics,
		const std::vector<LightingCopy::KeyEdit>& keyEdits) {
		const auto names = std::make_shared<const std::vector<std::uint64_t>>(TracedAssetNames(traced));
		const std::size_t count = traced.list.assets.size();
		const std::span<const std::uint8_t> stream(traced.zone.stream);
		using Key = std::pair<std::uint64_t, std::uint64_t>; // {type, name}
		// <map>'s own assets (not by name), and those of the types mapkit copies.
		std::set<Key> whole;
		std::map<Key, std::size_t> own;
		for (std::size_t i = 0; i < count; ++i) {
			const XAssetEntry& entry = traced.list.assets[i];
			const std::size_t at = traced.trace.assets[i].offset + XAssetNameOffset(entry.type);
			if ((entry.header == kPtrInline || entry.header == kPtrInsert) && at + 8 <= stream.size()
				&& !(Get<std::uint64_t>(stream.subspan(at, 8), 0) >> 63)) {
				whole.emplace(entry.type, (*names)[i]);
				if (LibraryAssetReader(entry.type)) {
					own.emplace(Key{ entry.type, (*names)[i] }, i);
				}
			}
		}
		// What the zone holds itself under a name (the creator's sky material, say) is never replaced by <map>'s.
		std::set<Key> held;
		for (const ZoneAsset& a : out) {
			if (a.linkName && !a.byName) {
				held.emplace(a.type, a.linkName);
			}
		}

		const auto landings = std::make_shared<LibraryLandings>();
		std::vector<ZoneAsset> front;
		std::set<Key> referenced; // by-name references in `front`
		std::set<Key> heldLinked; // what the zone holds itself that a copy links
		std::map<std::size_t, bool> done; // asset index -> copied
		std::map<std::uint64_t, std::pair<std::size_t, std::uint64_t>> copied; // type -> {count, stream bytes}
		std::map<std::uint64_t, std::pair<std::size_t, std::uint64_t>> streamed; // type -> {count, block-7 bytes}
		std::map<std::string, std::size_t> failures;
		std::size_t sharers = 0;
		// fx links that only <map>'s zone holds, left to the game's stub: its default fx (g_defaultAssetNames[0x33]), when
		// zm_common holds it (a zbarrier's repair effects may be <map>'s own).
		constexpr std::uint64_t kFx = 0x33;
		constexpr std::uint64_t kDefaultFx = 0x4644FA5FD144D2D2ull;
		std::set<Key> stubbed;
		std::set<std::uint64_t> editedKeys; // the keyEdits a copy took
		std::function<bool(std::size_t)> copy = [&](std::size_t index) -> bool {
			if (const auto found = done.find(index); found != done.end()) {
				return found->second; // false while it is being copied, too: a cycle stays linked by name
			}
			done[index] = false;
			const std::uint64_t type = traced.list.assets[index].type;
			auto fail = [&](const std::string& why) {
				++failures[std::format("{}: {}", XAssetTypeName(type), why)];
				return false;
			};
			if (library && LoadWritesInto(type, stream.subspan(traced.trace.assets[index].offset))) {
				return fail("the asset linking it sets its owner while it loads, and the override swap would lose that");
			}
			auto plan = std::make_shared<LibraryCopy>();
			plan->type = type;
			plan->names = names;
			XStream s(traced.zone.stream, 0);
			traced.Start(s, index);
			if (!LibraryAssetReader(type)(s, traced.list.assets[index].header, plan->record)) {
				return fail(s.Error());
			}
			RecordSplice& splice = plan->splice;
			splice.stream = traced.zone.stream;
			splice.record = &plan->record;
			splice.assetArrayPos = traced.trace.assets[0].pos[XBlockVirtual] - 16 * count;
			splice.assets = traced.list.assets;
			splice.assetNames = *names;
			splice.strings = traced.list.strings;
			// A sound bank's acoustics (+104) go with --no-acoustics: a null one loads no Triton data (SND_AddBank
			// 0x7FF7292202E0 tests it first).
			if (type == 0x12 && !acoustics && plan->record.Subtree("sound acoustics")) {
				splice.cuts.push_back({ "sound acoustics", {} });
			}
			// A stream key that names other data than <map>'s: its +8 (what the packages hold the data under) and its +48
			// (u32, that data's size; the four bytes after it stay).
			std::optional<std::uint32_t> editedSize;
			if (type == 0xB8) {
				const auto edit = std::ranges::find_if(keyEdits, [&](const LightingCopy::KeyEdit& e) { return e.key == (*names)[index]; });
				if (edit != keyEdits.end()) {
					const std::uint64_t tail = Get<std::uint64_t>(stream.subspan(traced.trace.assets[index].offset, 56), 48);
					splice.rootEdits.push_back({ 8, edit->package });
					splice.rootEdits.push_back({ 48, (tail & ~0xFFFFFFFFull) | edit->size });
					editedSize = edit->size;
					editedKeys.insert(edit->key);
				}
			}
			std::vector<RecordLink> links;
			std::vector<std::uint64_t> shared;
			std::string error;
			if (!RecordLinks(splice, links, plan->strings, error, &shared)) {
				return fail(error);
			}
			// The assets whose data it shares go first, whatever else they are.
			std::set<std::size_t> owners;
			for (const std::uint64_t stored : shared) {
				owners.insert(TracedAssetOfData(traced.trace, stored));
			}
			for (const std::size_t owner : owners) {
				if (owner >= index || !LibraryAssetReader(traced.list.assets[owner].type)) {
					return fail("it shares data with an asset mapkit cannot copy");
				}
				if (!copy(owner)) {
					return fail(std::format("it shares data with a {} that cannot be copied", XAssetTypeName(traced.list.assets[owner].type)));
				}
			}
			sharers += !owners.empty();
			// What it links: <map>'s own ones copied ahead of it where they can be, each linked by name.
			for (const RecordLink& link : links) {
				const Key key{ link.type, link.name };
				if (held.contains(key)) {
					heldLinked.insert(key);
				}
				else if (const auto found = own.find(key); found != own.end()) {
					copy(found->second);
				}
				else if (!library && whole.contains(key) && !stays.contains(key)) {
					// Only <map>'s zone holds it, of a type mapkit has no reader for: by name it would find nothing. The game
					// would link a stub of the type's default asset in its place, or stop with an error for a type with no
					// default loaded (DB_LinkMissingReference 0x7FF727EC1860). An fx is the one kind let through: the
					// game's default fx is zm_common's, so the stub is certain (and only the effect is lost).
					if (link.type != kFx || !stays.contains({ kFx, kDefaultFx })) {
						return fail(std::format("it links a {} only {}'s zone holds, which mapkit cannot copy yet",
							XAssetTypeName(link.type), map));
					}
					stubbed.insert(key);
				}
				if (referenced.insert(key).second) {
					auto reference = ByNameReference(link.type, link.name);
					if (!reference) {
						return fail(std::format("no by-name reference layout for {}", XAssetTypeName(link.type)));
					}
					front.push_back(std::move(*reference));
				}
			}
			splice.shared = [landings](std::uint64_t stored) { return landings->Find(stored); };
			splice.landings = &landings->landings;
			ZoneAsset asset;
			asset.type = type;
			asset.label = std::format("{} (copied)", XAssetTypeName(type));
			asset.scriptStrings = plan->strings;
			asset.encode = [plan](XWriter& w) {
				if (!EncodeRecordedAsset(w, plan->splice, LibraryAssetReader(plan->type), plan->error)) {
					std::fprintf(stderr, "  copied %s: %s\n", std::string(XAssetTypeName(plan->type)).c_str(), plan->error.c_str());
				}
			};
			front.push_back(std::move(asset));
			auto& [n, bytes] = copied[type];
			++n;
			bytes += plan->record.end - plan->record.begin;
			// Data in block 7 streams from <map>'s packages by key (images, meshes, streamkeys): the copy only reserves it.
			std::uint64_t reserved = 0;
			for (const RecordChunk& chunk : plan->record.chunks) {
				reserved += chunk.block == XBlockStreamed ? editedSize ? *editedSize : chunk.size : 0;
			}
			if (reserved) {
				++streamed[type].first;
				streamed[type].second += reserved;
			}
			return done[index] = true;
		};

		// The copies that leave something linked to <map>'s zone are named: a model by its name where the source gives one.
		// What several share is counted against the first that reaches it.
		auto failed = [&] {
			std::size_t n = 0;
			for (const auto& [why, times] : failures) {
				n += times;
			}
			return n;
		};
		std::size_t seeds = 0, kept = 0;
		std::string blamed;
		for (const ZoneAsset& a : out) {
			if (!a.byName || held.contains({ a.type, a.linkName })) {
				continue;
			}
			if (const auto found = own.find({ a.type, a.linkName }); found != own.end()) {
				++seeds;
				const std::size_t before = failed();
				kept += !copy(found->second);
				if (failed() != before) {
					const auto name = a.type == 0x06 ? modelNames.find(a.linkName) : modelNames.end();
					blamed += name != modelNames.end() ? " " + name->second
						: std::format(" {} {:016X}", XAssetTypeName(a.type), a.linkName);
				}
			}
		}
		// The level's sound bank, which nothing links by name: DB_LinkSoundBank 0x7FF728A3E290 hands it to the sound system
		// when its zone loads (docs/mapkit-plan.md, "Ambient rooms"). It holds the level's rooms (the ones ambient
		// rooms name), its ambience and its own sounds, and its Triton acoustics. Without <map>'s zone the map has none of
		// that unless it carries a copy. A bank that cannot be copied leaves the map as it was before copies: said, not a
		// failed build.
		for (const auto& [key, index] : own) {
			if (library || key.first != 0x12) {
				continue;
			}
			const auto before = failures;
			const std::span<const std::uint8_t> root = stream.subspan(traced.trace.assets[index].offset);
			if (copy(index)) {
				const std::uint64_t triton = Get<std::uint64_t>(root, 104);
				const bool cut = !acoustics && (triton == kPtrInline || triton == kPtrInsert);
				std::printf("  sound bank %016llX: %s's copied (%u alias lists, %u rooms, %u ducks, %s): its rooms, ambience and level "
					"sounds\n", static_cast<unsigned long long>(key.second), map.c_str(), Get<std::uint32_t>(root, 32),
					Get<std::uint32_t>(root, 72), Get<std::uint32_t>(root, 56), !triton ? "no acoustics"
					: cut ? "its Triton acoustics left out (--no-acoustics)"
					: "with its Triton acoustics, baked from its own walls (--no-acoustics leaves them out)");
				continue;
			}
			std::string why;
			for (const auto& [what, n] : failures) {
				if (const auto was = before.find(what); was == before.end() || was->second != n) {
					why += (why.empty() ? "" : "; ") + what;
				}
			}
			failures = before;
			std::printf("  sound bank %016llX: %s's cannot be copied (%s), so the map has no level sound bank: no rooms, no ambience "
				"(--with-library loads %s's)\n", static_cast<unsigned long long>(key.second), map.c_str(), why.c_str(), map.c_str());
		}
		// Each copy goes ahead of every reference to it: the references `front` holds replace the zone's later ones. With
		// <map>'s zone loaded they go at the very front (a reference to what the zone holds itself finds <map>'s, which the
		// swap then fills); without it after the last asset of the zone's own that a copy links.
		auto insertPoint = [&] {
			std::size_t at = 0;
			for (std::size_t i = 0; !library && i < out.size(); ++i) {
				if (!out[i].byName && out[i].linkName && heldLinked.contains({ out[i].type, out[i].linkName })) {
					at = i + 1;
				}
			}
			return at;
		};
		// A reference of the zone's own ahead of that point, to an asset another zone holds, stays: an asset there links it
		// (the sky material its techset, which techset_<map> holds; zm_debug's first builds, 2026-10-03, had a copied
		// material on the same one), and a link finds the first entry of its name (XWriter::AssetEntry). `front` drops its
		// own. One to a copy cannot stay ahead of the copy: it goes, and the zone writer says if an asset there links it.
		auto wasCopied = [&](const Key& key) {
			const auto found = own.find(key);
			const auto was = found == own.end() ? done.end() : done.find(found->second);
			return was != done.end() && was->second;
		};
		std::set<Key> ahead;
		for (std::size_t i = 0, end = insertPoint(); i < end; ++i) {
			const Key key{ out[i].type, out[i].linkName };
			if (out[i].byName && referenced.contains(key) && !wasCopied(key)) {
				ahead.insert(key);
			}
		}
		std::erase_if(front, [&](const ZoneAsset& a) { return a.byName && ahead.contains({ a.type, a.linkName }); });
		std::erase_if(out, [&](const ZoneAsset& a) {
			return a.byName && referenced.contains({ a.type, a.linkName }) && !ahead.contains({ a.type, a.linkName });
		});
		const std::size_t insertAt = insertPoint();
		out.insert(out.begin() + static_cast<std::ptrdiff_t>(insertAt), std::make_move_iterator(front.begin()),
			std::make_move_iterator(front.end()));

		std::string summary;
		std::size_t total = 0;
		std::uint64_t totalBytes = 0;
		for (const auto& [type, numbers] : copied) {
			summary += std::format("{}{} {}", summary.empty() ? "" : ", ", numbers.first, XAssetTypeName(type));
			total += numbers.first;
			totalBytes += numbers.second;
		}
		std::printf("  library: %zu of %s's assets copied (%.2f MB: %s), %zu of them sharing data with an earlier one; %zu of the "
			"%zu it was asked for stay linked by name\n", total, map.c_str(), totalBytes / (1024.0 * 1024.0), summary.c_str(), sharers,
			kept, seeds);
		std::string streaming;
		for (const auto& [type, numbers] : streamed) {
			streaming += std::format("{}{} {} ({:.1f} MB)", streaming.empty() ? "" : ", ", numbers.first, XAssetTypeName(type),
				numbers.second / (1024.0 * 1024.0));
		}
		if (!streaming.empty()) {
			std::printf("    streamed from %s's packages, not stored: %s\n", map.c_str(), streaming.c_str());
		}
		if (!stubbed.empty()) {
			std::printf("    linked by name though only %s's zone holds them: %zu fx, which the game replaces with its default fx "
				"(mapkit cannot copy an fx yet)\n", map.c_str(), stubbed.size());
		}
		if (!keyEdits.empty()) {
			std::uint64_t bytes = 0;
			for (const LightingCopy::KeyEdit& edit : keyEdits) {
				bytes += editedKeys.contains(edit.key) ? edit.size : 0;
			}
			std::printf("    %zu of the copied streamkeys name an empty shadow tree's data (%.1f MB) in place of %s's baked ones\n",
				editedKeys.size(), bytes / (1024.0 * 1024.0), map.c_str());
			if (editedKeys.size() != keyEdits.size()) {
				std::fprintf(stderr, "  library: %zu stream key(s) of the lighting's emptied shadow trees are not among the copies: the "
					"lighting would say an empty tree and the key %s's baked one (--keep-baked-shadows leaves the trees as they are)\n",
					keyEdits.size() - editedKeys.size(), map.c_str());
				return false;
			}
		}
		std::size_t shown = 0;
		for (const auto& [why, n] : failures) {
			if (shown++ < 8) {
				std::printf("    not copied: %zu x %s\n", n, why.c_str());
			}
		}
		if (!blamed.empty()) {
			std::printf("    for:%s\n", blamed.c_str());
		}
		if (!library && !failures.empty()) {
			std::fprintf(stderr, "  library: what is not copied stays linked to %s's zone, which this map does not load: leave out what "
				"needs it, or build with --with-library (it loads that zone)\n", map.c_str());
			return false;
		}
		return true;
	}

	// --- the draw test row (--draw-test; docs/mapkit-plan.md P0) ------------------------------------------------
	// The retail model it is built around: p9_zm_ndu_door_metal_gray_rusted, a door Die Maschine places twice by
	// script_model. Resident LOD 0 with its own buffer, 16-B vertices, 2 surfaces, 78 vertices, opaque.
	constexpr std::uint64_t kDrawTestModel = 0x3ED36494E93F3CBDull;
	// The doors: this far in front of player 1's spawn, this far apart; the cubes this far behind it, this big.
	constexpr float kDrawTestDistance = 176.0f;
	constexpr float kDrawTestSpacing = 96.0f;
	constexpr float kDrawTestBehind = 112.0f;
	constexpr float kDrawTestCubeHalf = 24.0f;

	// A stored reference into the XAsset array (block 4, just before the first asset; +8 = the header field)
	// names the asset it points at (ffinfo's AssetOfReference).
	std::optional<std::size_t> AssetOfReference(const TracedMap& traced, std::uint64_t stored) {
		if (stored == kPtrNull || stored == kPtrInline || stored == kPtrInsert) {
			return std::nullopt;
		}
		const std::uint64_t value = stored - 1;
		const std::uint64_t offset = value & 0x0FFFFFFFFFFFFFFFull;
		const std::uint64_t tableEnd = traced.trace.assets[0].pos[XBlockVirtual];
		const std::uint64_t tableBegin = tableEnd - 16ull * traced.list.assets.size();
		if ((value >> 60) != XBlockVirtual || offset < tableBegin || offset >= tableEnd) {
			return std::nullopt;
		}
		return static_cast<std::size_t>((offset - tableBegin) / 16);
	}

	std::uint64_t AssetName(const TracedMap& traced, std::size_t index) {
		return Get<std::uint64_t>(std::span<const std::uint8_t>(traced.zone.stream).subspan(traced.trace.assets[index].offset, 8), 0)
			& ~(1ull << 63);
	}

	// The map's own navmesh (P5, zonekit/navmesh.hpp): the walkable polygons over the collision hulls, as <map>'s navmesh
	// asset. <map>'s navmesh files, fetched from its packages, are the templates. On failure nothing is added, and the
	// map keeps <map>'s navmesh. `obj` gets the polygons as an OBJ.
	bool AddNavMesh(const Options& options, const std::string& map, const std::string& mapId,
		std::span<const std::array<float, 3>> seeds, const std::vector<ConvexHull>& hulls, const TracedMap& traced,
		std::vector<ZoneAsset>& out, std::string& obj) {
		std::string error;
		NavPolygons polygons;
		NavGenerateReport generated;
		if (!GenerateNavPolygons(hulls, seeds, NavSettings{}, polygons, generated, error)) {
			std::fprintf(stderr, "  navmesh: %s\n", error.c_str());
			return false;
		}
		std::printf("  navmesh: %zu collision triangles -> %zu walkable polygons, %zu kept (reached from %zu of %zu seeds), "
			"%zu vertices, (%.0f %.0f %.0f)..(%.0f %.0f %.0f)\n", generated.triangles, generated.polygons, generated.kept,
			generated.seedsOnMesh, seeds.size(), polygons.vertices.size(), generated.mins[0], generated.mins[1], generated.mins[2],
			generated.maxs[0], generated.maxs[1], generated.maxs[2]);

		// <map>'s navmesh: the root, then its cells inline. The shared file's stream key is at +8, cell 0's at +40 of the
		// cell; a stream key's package key is its +8.
		const std::size_t index = traced.Find(0x75);
		const std::span<const std::uint8_t> stream(traced.zone.stream);
		if (index >= traced.list.assets.size() || traced.trace.assets[index].offset + 168 > stream.size()) {
			std::fprintf(stderr, "  navmesh: %s has no navmesh to use as the template\n", map.c_str());
			return false;
		}
		const std::span<const std::uint8_t> root = stream.subspan(traced.trace.assets[index].offset, 168);
		const auto packageKey = [&](std::uint64_t stored) -> std::optional<std::uint64_t> {
			const auto key = AssetOfReference(traced, stored);
			if (!key || traced.list.assets[*key].type != 0xB8 || traced.trace.assets[*key].offset + 56 > stream.size()) {
				return std::nullopt;
			}
			return Get<std::uint64_t>(stream.subspan(traced.trace.assets[*key].offset, 56), 8);
		};
		const auto sharedKey = packageKey(Get<std::uint64_t>(root, 8));
		const auto cellKey = packageKey(Get<std::uint64_t>(root, 104 + 40));
		if (Get<std::int32_t>(root, 32) < 1 || !sharedKey || !cellKey) {
			std::fprintf(stderr, "  navmesh: %s's navmesh does not point at its files as expected\n", map.c_str());
			return false;
		}
		Kapi::PackageIndex packages;
		std::vector<std::uint8_t> sharedPayload;
		std::vector<std::uint8_t> cellPayload;
		HkTagfile retailShared;
		HkTagfile retailCell;
		if (!packages.Open(options.gameDir / "zone", error) || !packages.Extract(*sharedKey, sharedPayload, error)
			|| !packages.Extract(*cellKey, cellPayload, error) || !ReadHkTagfile(NavPayloadTagfile(sharedPayload), retailShared, error)
			|| !ReadHkTagfile(NavPayloadTagfile(cellPayload), retailCell, error)) {
			std::fprintf(stderr, "  navmesh: %s's navmesh files: %s\n", map.c_str(), error.c_str());
			return false;
		}
		HkTagfile shared;
		HkTagfile cell;
		NavCellReport report;
		if (!BuildNavMeshShared(retailShared, shared, error) || !BuildNavMeshCell(retailCell, polygons, cell, report, error)) {
			std::fprintf(stderr, "  navmesh: %s\n", error.c_str());
			return false;
		}
		NavMeshAsset asset;
		asset.name = HashName(MapBspName(map));
		asset.sharedKey = HashName(std::format("mapkit/{}/navmesh_shared", mapId));
		asset.cellKey = HashName(std::format("mapkit/{}/navmesh_cell_0", mapId));
		asset.shared = NavPayload(WriteHkTagfile(shared));
		asset.cell = NavPayload(WriteHkTagfile(cell));
		asset.mins = report.mins;
		asset.maxs = report.maxs;
		asset.faceCount = static_cast<std::uint32_t>(report.faces);
		// What the engine gets must read as a tagfile again, and the cell hold together.
		HkTagfile check;
		if (!ReadHkTagfile(NavPayloadTagfile(asset.shared), check, error) || !ReadHkTagfile(NavPayloadTagfile(asset.cell), check, error)
			|| !CheckNavMeshCell(check, error)) {
			std::fprintf(stderr, "  navmesh: the written tagfiles do not read back whole: %s\n", error.c_str());
			return false;
		}
		std::printf("  navmesh: %zu faces, %zu edges (%zu on the boundary), %zu vertices, %zu clusters (%zu links); tagfiles: "
			"shared %.1f KB (%s's, tactical points emptied), cell %.1f KB\n", report.faces, report.edges, report.openEdges,
			report.vertices, report.clusters, report.clusterEdges, asset.shared.size() / 1024.0, map.c_str(),
			asset.cell.size() / 1024.0);
		ZoneAsset navmesh;
		navmesh.type = 0x75;
		navmesh.label = "navmesh (mapkit's own)";
		navmesh.encode = [asset = std::move(asset)](XWriter& w) { EncodeNavMesh(w, asset); };
		out.push_back(std::move(navmesh));
		obj = NavPolygonsObj(polygons);
		return true;
	}

	float HalfToFloat(std::uint16_t h) {
		const std::uint32_t sign = (h & 0x8000u) << 16;
		const std::uint32_t exponent = (h >> 10) & 0x1F;
		const std::uint32_t mantissa = h & 0x3FF;
		std::uint32_t bits;
		if (exponent == 0) {
			float f = std::ldexp(static_cast<float>(mantissa), -24);
			std::memcpy(&bits, &f, 4);
			bits |= sign;
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

	// 10:10:10:2 back to a vector (model_writer.cpp's PackUnitVector: n * 511 + 512).
	std::array<float, 3> UnpackUnitVector(std::uint32_t packed) {
		std::array<float, 3> v{};
		for (int axis = 0; axis < 3; ++axis) {
			v[axis] = (static_cast<float>((packed >> (10 * axis)) & 0x3FFu) - 512.0f) / 511.0f;
		}
		return v;
	}

	// A retail model of <map> as a StaticModel: its LOD 0's surfaces, vertices and materials, decoded from the
	// zone, so mapkit's writer can write it again. It must be resident with its own buffer and 16-B vertices.
	// What the writer does not carry is lost: the vertex colors (it writes white) and the other LODs.
	bool DecodeRetailModel(const TracedMap& traced, std::uint64_t name, StaticModel& out, std::string& error) {
		std::size_t index = traced.list.assets.size();
		for (std::size_t i = 0; i < traced.list.assets.size(); ++i) {
			if (traced.list.assets[i].type == 0x06 && traced.list.assets[i].header == kPtrInline && AssetName(traced, i) == name) {
				index = i;
				break;
			}
		}
		if (index == traced.list.assets.size()) {
			error = std::format("no xmodel {:016X} stored in this zone", name);
			return false;
		}
		const std::span<const std::uint8_t> root(traced.zone.stream.data() + traced.trace.assets[index].offset, 232);
		const auto lod = AssetOfReference(traced, Get<std::uint64_t>(root, 32));
		if (!lod || traced.list.assets[*lod].type != 0x09) {
			error = "its LOD 0 is not a reference to an earlier xmodelmesh";
			return false;
		}
		XStream meshStream(traced.zone.stream, 0);
		traced.Start(meshStream, *lod);
		XModelMeshData mesh;
		if (!ReadXModelMesh(meshStream, traced.list.assets[*lod].header, mesh) || !mesh.hasInfo) {
			error = "its LOD 0 does not decode: " + meshStream.Error();
			return false;
		}
		const MeshInfo& info = mesh.info;
		if (info.Streamed() || info.ExtendedVertices() || info.resident.size() < info.BufferSize()) {
			error = std::format("its LOD 0 is not resident with its own 16-B vertex buffer (flags {:#x})", info.Flags());
			return false;
		}

		// LOD 0's material handles: the model's own walk (model_assets.cpp LoadXModelBody) up to its first table.
		std::vector<std::uint64_t> handles;
		XStream s(traced.zone.stream, 0);
		traced.Start(s, index);
		LoadAssetHeader(s, traced.list.assets[index].header, 232, 8, [&](std::span<std::uint8_t> model) {
			s.Push(XBlockVirtual);
			LoadXSkeletonAsset(s, Get<std::uint64_t>(model, 8));
			LoadXCollisionAsset(s, Get<std::uint64_t>(model, 16));
			for (std::size_t l = 0; l < 8 && !s.Failed(); ++l) {
				LoadXModelMeshAsset(s, Get<std::uint64_t>(model, 32 + l * 8));
			}
			if (Get<std::uint64_t>(model, 96) && !s.Failed()) {
				s.Alloc(8);
				std::vector<std::uint8_t> tables(32ull * Get<std::uint16_t>(model, 112));
				std::vector<std::uint8_t> bytes;
				if (s.Load(tables.data(), tables.size()) && !tables.empty()
					&& LoadInline(s, Get<std::uint64_t>(tables, 8), 8ull * Get<std::uint16_t>(tables, 0), 8, "material handles", &bytes)) {
					for (std::size_t h = 0; h + 8 <= bytes.size(); h += 8) {
						handles.push_back(Get<std::uint64_t>(bytes, h));
					}
				}
			}
			s.Pop();
		});
		if (handles.size() != mesh.surfaces.size()) {
			error = std::format("{} material handles for {} surfaces{}", handles.size(), mesh.surfaces.size(),
				s.Failed() ? " (" + s.Error() + ")" : "");
			return false;
		}

		const std::span<const std::uint8_t> buffer(info.resident);
		for (std::size_t si = 0; si < mesh.surfaces.size(); ++si) {
			const MeshSurface& surface = mesh.surfaces[si];
			const auto material = AssetOfReference(traced, handles[si]);
			if (!material || traced.list.assets[*material].type != 0x0A) {
				error = std::format("surface {}'s material is not a reference to a material asset", si);
				return false;
			}
			StaticSurface decoded;
			decoded.material = AssetName(traced, *material);
			for (std::uint32_t v = 0; v < surface.VertexCount(); ++v) {
				const std::size_t vertex = std::size_t(surface.FirstVertex()) + v;
				const std::size_t at = info.PositionOffset() + vertex * 12;
				const std::size_t record = info.VertexOffset() + vertex * 16;
				if (at + 12 > buffer.size() || record + 16 > buffer.size()) {
					error = std::format("surface {} vertex {} lies outside the buffer", si, v);
					return false;
				}
				StaticVertex decodedVertex;
				for (int axis = 0; axis < 3; ++axis) {
					decodedVertex.position[axis] = Get<float>(buffer, at + 4 * axis);
				}
				decodedVertex.uv = { HalfToFloat(Get<std::uint16_t>(buffer, record + 4)), HalfToFloat(Get<std::uint16_t>(buffer, record + 6)) };
				decodedVertex.normal = UnpackUnitVector(Get<std::uint32_t>(buffer, record + 8));
				const std::uint32_t tangent = Get<std::uint32_t>(buffer, record + 12);
				decodedVertex.tangent = UnpackUnitVector(tangent);
				decodedVertex.bitangentSign = (tangent >> 30) == 0 ? -1.0f : 1.0f;
				decoded.vertices.push_back(decodedVertex);
			}
			for (std::uint32_t f = 0; f < surface.FaceCount(); ++f) {
				const std::size_t at = info.FaceOffset() + std::size_t(surface.FirstIndex()) * 2 + std::size_t(f) * 6;
				if (at + 6 > buffer.size()) {
					error = std::format("surface {} triangle {} lies outside the buffer", si, f);
					return false;
				}
				const std::array<std::uint16_t, 3> t = { Get<std::uint16_t>(buffer, at), Get<std::uint16_t>(buffer, at + 2),
					Get<std::uint16_t>(buffer, at + 4) };
				if (t[0] >= surface.VertexCount() || t[1] >= surface.VertexCount() || t[2] >= surface.VertexCount()) {
					error = std::format("surface {} triangle {} indexes past its vertices", si, f);
					return false;
				}
				decoded.triangles.push_back(t);
			}
			AppendSurface(out, decoded);
		}
		return true;
	}

	void SetEntityAngles(MapEntity& entity, const std::array<float, 3>& angles);

	// A script_model placing `model` at origin with the given yaw: a copy of <map>'s first script_model, cut to its
	// placement keys. id: the entity's number.
	std::optional<MapEntity> PlacedModel(const std::vector<MapEntity>& entities, std::uint64_t model, std::uint32_t id,
		const std::array<float, 3>& origin, float yaw) {
		const auto templ = std::ranges::find_if(entities, [](const MapEntity& entity) {
			return entity.Text("classname") == "script_model" && entity.Find("model") && entity.Find("origin")
				&& entity.Find("angles");
		});
		if (templ == entities.end()) {
			return std::nullopt;
		}
		MapEntity placed = *templ;
		std::erase_if(placed.keys, [](const EntityKey& key) {
			return key.key != "classname" && key.key != "model" && key.key != "origin" && key.key != "angles";
		});
		for (EntityKey& key : placed.keys) {
			if (key.key == "model") {
				key.hash = model;
				PutAt(std::span<std::uint8_t>(key.raw), 8, model);
			}
		}
		placed.id = id;
		SetEntityOrigin(placed, origin, std::nullopt);
		SetEntityAngles(placed, { 0.0f, yaw, 0.0f });
		return placed;
	}

	std::uint32_t NextEntityId(const std::vector<MapEntity>& entities) {
		std::uint32_t last = 0;
		for (const MapEntity& entity : entities) {
			last = std::max(last, entity.id);
		}
		return last + 1;
	}

	// The draw test row. In front of player 1's spawn, left to right as the player sees them, four doors:
	//   1 the retail model (kDrawTestModel), placed like the brush model: the control that the placement draws;
	//   2 the same model decoded and written again by mapkit's writer, with xmodel +208 = 8 as mapkit wrote it until
	//     2026-09-26;
	//   3 the same with +208 = 0 (StaticModel's default);
	//   4 the same with +208 = 0 and the door's own +221 and xmodelmesh +62 (2 and 2): the whole retail header.
	// None of mapkit's drew until the zone carried a bgcache listing them (BgCacheAsset); since then all four do, so
	// +208, +221 and +62 do not decide drawing.
	// Behind the spawn, two mapkit cubes (+208 = 0): left with the material of the door's biggest surface, right
	// with the default brush material. Each mapkit one has its own skeleton (<name>_skeleton) and the brush
	// model's collision.
	// Names: mapkit_<source>_clone8, _clone, _clone_full, _cube and _cube_default (the client's live dump compares
	// them with the retail door).
	bool AddDrawTestRow(const MapKit::MkMap& source, const TracedMap& traced, const MapEntity& player,
		std::vector<ZoneAsset>& assets, std::vector<MapEntity>& entities) {
		std::string error;
		StaticModel door;
		if (!DecodeRetailModel(traced, kDrawTestModel, door, error)) {
			std::fprintf(stderr, "draw test: the retail model %016llX: %s\n", static_cast<unsigned long long>(kDrawTestModel),
				error.c_str());
			return false;
		}
		door.collision = kBrushModelCollision;
		// The cube takes the material of the model's biggest surface (its first is glass on this door).
		const std::uint64_t retailMaterial = std::ranges::max_element(door.surfaces, {}, [](const StaticSurface& surface) {
			return surface.triangles.size();
		})->material;
		const std::string prefix = "mapkit_" + source.name;
		const auto named = [](StaticModel model, const std::string& name) {
			model.name = HashName(name);
			model.meshName = HashName(name + "_lod0");
			model.skeleton = HashName(name + "_skeleton");
			return model;
		};
		StaticModel clone8 = named(door, prefix + "_clone8");
		clone8.flags = 8;
		StaticModel clone = named(door, prefix + "_clone");
		StaticModel cloneFull = named(door, prefix + "_clone_full");
		cloneFull.modelByte221 = 2;
		cloneFull.meshWord62 = 2;

		const auto cube = [&](const std::string& name, std::uint64_t material) {
			MapKit::MkBrush brush;
			brush.center = { 0.0f, 0.0f, kDrawTestCubeHalf };
			brush.halfAxes = { { { kDrawTestCubeHalf, 0, 0 }, { 0, kDrawTestCubeHalf, 0 }, { 0, 0, kDrawTestCubeHalf } } };
			StaticModel model;
			model.collision = kBrushModelCollision;
			AppendSurface(model, BrushSurface(brush, material));
			return named(std::move(model), name);
		};
		StaticModel cubeRetail = cube(prefix + "_cube", retailMaterial);
		StaticModel cubeDefault = cube(prefix + "_cube_default", HashName(kDefaultBrushMaterial));

		constexpr float kDegrees = 3.14159265358979f / 180.0f;
		const float yaw = player.angles[1];
		const std::array<float, 2> forward = { std::cos(yaw * kDegrees), std::sin(yaw * kDegrees) };
		const std::array<float, 2> right = { forward[1], -forward[0] };
		struct Item {
			std::uint64_t model;
			float ahead; // along the player's facing
			float side;  // to the player's right
			const char* what;
		};
		const Item items[] = {
			{ kDrawTestModel, kDrawTestDistance, -1.5f * kDrawTestSpacing, "door 1: retail p9_zm_ndu_door_metal_gray_rusted" },
			{ clone8.name, kDrawTestDistance, -0.5f * kDrawTestSpacing, "door 2: mapkit's copy, +208 = 8" },
			{ clone.name, kDrawTestDistance, 0.5f * kDrawTestSpacing, "door 3: mapkit's copy, +208 = 0" },
			{ cloneFull.name, kDrawTestDistance, 1.5f * kDrawTestSpacing, "door 4: mapkit's copy, the whole retail header" },
			{ cubeRetail.name, -kDrawTestBehind, -0.5f * kDrawTestSpacing, "cube left (behind): the door's metal material" },
			{ cubeDefault.name, -kDrawTestBehind, 0.5f * kDrawTestSpacing, "cube right (behind): the default brush material" },
		};
		std::uint32_t id = NextEntityId(entities);
		for (const Item& item : items) {
			const std::array<float, 3> origin = { player.origin[0] + forward[0] * item.ahead + right[0] * item.side,
				player.origin[1] + forward[1] * item.ahead + right[1] * item.side, player.origin[2] };
			auto placed = PlacedModel(entities, item.model, id, origin, yaw);
			if (!placed) {
				std::fprintf(stderr, "draw test: no script_model to copy\n");
				return false;
			}
			std::printf("  draw test (entity %u, model %016llX) at %.0f %.0f %.0f: %s\n", id,
				static_cast<unsigned long long>(item.model), origin[0], origin[1], origin[2], item.what);
			entities.push_back(std::move(*placed));
			++id;
		}
		std::string materials;
		for (const StaticSurface& surface : door.surfaces) {
			materials += std::format(" {:016X} ({} triangles)", surface.material, surface.triangles.size());
		}
		std::printf("  draw test: the door's surfaces:%s; the cubes %016llX and %016llX\n", materials.c_str(),
			static_cast<unsigned long long>(retailMaterial), static_cast<unsigned long long>(HashName(kDefaultBrushMaterial)));
		AddStaticModelAssets(std::move(clone8), "draw test door, +208 = 8", assets);
		AddStaticModelAssets(std::move(clone), "draw test door", assets);
		AddStaticModelAssets(std::move(cloneFull), "draw test door, retail header", assets);
		AddStaticModelAssets(std::move(cubeRetail), "draw test cube", assets);
		AddStaticModelAssets(std::move(cubeDefault), "draw test cube, default material", assets);
		return true;
	}

	// How far under the source's lowest brush the graveyard is: below every floor, out of sight.
	constexpr float kGraveyardDepth = 4096.0f;
	// A wall buy's chalk hangs this high above the source's wall_buy origin (the foot of its wall): each of
	// zm_silver's 12 chalks is 38.5 to 40.8 units above the lockdown path node on the floor in front of it.
	constexpr float kWallBuyHeight = 40.0f;
	// The contents of a zone's info_volume, as zm_silver's player_volume ones store them (map_entities.hpp).
	constexpr std::uint32_t kTriggerContentsVolume = 0x80000001;
	// A perk machine's or box location's struct yaw, from its node's facing (PlaceGameplayObjects says why). The game
	// spawns the machine, or the box, at the struct's origin with the struct's angles.
	constexpr float kPerkStructYaw = 90.0f;
	constexpr float kBoxStructYaw = -90.0f;
	// How far in front of a perk machine or a box a player stands to use it: past the deepest machine's front (26 units
	// from its origin) by more than a player's radius.
	constexpr float kUseSpotDistance = 48.0f;

	float NormalizeYaw(float yaw) {
		yaw = std::fmod(yaw, 360.0f);
		return yaw < 0 ? yaw + 360.0f : yaw;
	}

	// Whether the source has a power switch (MkPowerSwitch). Without one the power is on from the start.
	bool HasPowerSwitch(const MapKit::MkMap& source) {
		return std::ranges::any_of(source.entities, [](const MapKit::MkEntity& entity) { return entity.cls == "power_switch"; });
	}

	// All three angles, in the entity's fields and its "angles" key (SetEntityOrigin sets only the yaw).
	void SetEntityAngles(MapEntity& entity, const std::array<float, 3>& angles) {
		entity.angles = angles;
		for (EntityKey& key : entity.keys) {
			if (key.type == EntityValueType::Vector && key.key == "angles") {
				key.vector = angles;
			}
		}
	}

	void SetEntityText(MapEntity& entity, std::string_view name, const std::string& text) {
		for (EntityKey& key : entity.keys) {
			if (key.type == EntityValueType::String && key.key == name) {
				key.text = text;
			}
		}
	}

	// The source's nodes of one class, in the source's order.
	std::vector<const MapKit::MkEntity*> SourceNodes(const MapKit::MkMap& source, std::string_view cls) {
		std::vector<const MapKit::MkEntity*> out;
		for (const MapKit::MkEntity& entity : source.entities) {
			if (entity.cls == cls) {
				out.push_back(&entity);
			}
		}
		return out;
	}

	// The <map> entities whose String key `key` is `value`, by index.
	std::vector<std::size_t> EntitiesWith(const std::vector<MapEntity>& entities, std::string_view key, std::string_view value) {
		std::vector<std::size_t> out;
		for (std::size_t i = 0; i < entities.size(); ++i) {
			if (entities[i].Text(key) == value) {
				out.push_back(i);
			}
		}
		return out;
	}

	// The perk machine struct of <map>'s that each of the source's machines takes: the first one of its perk that no
	// machine before it took, or entities.size() when <map> has no (other) machine of that perk. What moves the structs
	// (PlaceGameplayObjects) and what makes the machines solid (ModelHulls) both go by this.
	std::vector<std::size_t> PerkMachineStructs(const std::vector<const MapKit::MkEntity*>& machines,
		const std::vector<MapEntity>& entities) {
		const std::vector<std::size_t> structs = EntitiesWith(entities, "targetname", "zm_perk_machine");
		std::vector<char> taken(entities.size(), 0);
		std::vector<std::size_t> out;
		for (const MapKit::MkEntity* machine : machines) {
			const char* noteworthy = PerkNoteworthy(machine->Prop("perk"));
			const auto it = std::ranges::find_if(structs, [&](std::size_t i) {
				return noteworthy && !taken[i] && entities[i].Text("script_noteworthy") == noteworthy;
			});
			out.push_back(it == structs.end() ? entities.size() : *it);
			if (it != structs.end()) {
				taken[*it] = 1;
			}
		}
		return out;
	}

	// <map>'s Mystery Box locations in the order the source's take them: the ones the box may start at first.
	std::vector<std::size_t> BoxStructs(const std::vector<MapEntity>& entities) {
		std::vector<std::size_t> chests = EntitiesWith(entities, "content_key", "magicbox_zbarrier");
		std::ranges::stable_sort(chests, {}, [&](std::size_t i) { return entities[i].Text("start_exclude") != "0"; });
		return chests;
	}

	// The source's Mystery Box locations in the order they take <map>'s: the ones the box starts at first. The k-th
	// takes <map>'s k-th (BoxStructs); past <map>'s count they are left out.
	std::vector<const MapKit::MkEntity*> BoxNodes(const MapKit::MkMap& source) {
		std::vector<const MapKit::MkEntity*> boxes = SourceNodes(source, "mystery_box");
		std::ranges::stable_sort(boxes, {}, [](const MapKit::MkEntity* box) { return box->Prop("start_here") != "true"; });
		return boxes;
	}

	// <map>'s Zombies objects against the source's: each one the source places takes a retail struct of its kind
	// (moved, facing converted), and the rest are taken out. zm_silver expresses them this way:
	// - perk machines: script_struct zm_perk_machine, script_noteworthy talent_<perk>, one machine each. The
	//   machine's front faces the struct's right (-Y: zm_perks.gsc puts the buy trigger 20 units along
	//   anglestoright, and the models' buttons are on -Y), so the struct's yaw is the source's facing + 90;
	// - Mystery Box locations: content_struct magicbox_zbarrier, script_noteworthy <area>_chest, start_exclude "0"
	//   on the ones the box may start at. The box model is long along the struct's X and its front (the latch)
	//   faces the struct's +Y, so the struct's yaw is the source's facing - 90;
	// - wall buys: a content_struct wallbuy_chalk (script_noteworthy = the weapon, the chalk decal model) and a
	//   wallbuy_gun that targets it (the weapon model), both on the wall. The chalk's right side faces out of
	//   the wall (the lockdown path node stands 15-16 units that way) and the gun is turned round from it;
	// - ammo caches, Armor Stations (the Arsenal: MkArmorStation and MkArsenal), Wunderfizzes, crafting tables,
	//   Pack-a-Punch machines: one content_struct each (copied past <map>'s count), and the exfil: a content
	//   instance moved and turned whole with its helicopter paths (see below).
	// Taking out is safe for these: their scripts spawn one object per struct, whatever the count. One box
	// location when the source has none goes to the graveyard instead, since the box script looks one up. `kept` gets the ids of every <map> entity placed or parked here: the level's entity list keeps
	// those (ComposeLevelEntities) and leaves out the rest of <map>'s.
	template <typename Place>
	void PlaceGameplayObjects(const MapKit::MkMap& source, const std::string& map, const Place& place,
		const std::array<float, 3>& graveyard, std::vector<MapEntity>& entities, std::unordered_set<std::uint32_t>& kept) {
		std::vector<char> keep(entities.size(), 1);
		std::vector<char> placed(entities.size(), 0);
		const bool powerSwitch = HasPowerSwitch(source);
		const auto sourceOf = [&source](std::string_view cls) { return SourceNodes(source, cls); };
		const auto retail = [&entities](std::string_view key, std::string_view value) { return EntitiesWith(entities, key, value); };
		const auto dropUnplaced = [&](const std::vector<std::size_t>& found, std::string_view nameKey) {
			std::string names;
			for (const std::size_t i : found) {
				if (!placed[i]) {
					keep[i] = 0;
					names += " " + entities[i].Text(nameKey);
				}
			}
			return names;
		};

		// Perk machines.
		const std::vector<std::size_t> perks = retail("targetname", "zm_perk_machine");
		const std::vector<const MapKit::MkEntity*> machines = sourceOf("perk_machine");
		const std::vector<std::size_t> machineStructs = PerkMachineStructs(machines, entities);
		for (std::size_t k = 0; k < machines.size(); ++k) {
			const MapKit::MkEntity* machine = machines[k];
			const std::string perk = machine->Prop("perk");
			const std::size_t taken = machineStructs[k];
			if (taken >= entities.size()) {
				std::printf("  perk %s (%s): %s has no (other) machine of it, skipped\n", perk.c_str(), machine->path.c_str(),
					map.c_str());
				continue;
			}
			SetEntityOrigin(entities[taken], place(machine->origin), std::nullopt);
			SetEntityAngles(entities[taken], { 0.0f, NormalizeYaw(machine->angles[1] + kPerkStructYaw), 0.0f });
			placed[taken] = 1;
			std::printf("  perk %s: entity %u moved to %s\n", perk.c_str(), entities[taken].id, machine->path.c_str());
		}
		if (const std::string names = dropUnplaced(perks, "script_noteworthy"); !names.empty()) {
			std::printf("  perks taken out:%s\n", names.c_str());
		}

		// Mystery Box locations: the ones the box may start at are used first, for the source's start_here boxes.
		const std::vector<std::size_t> chests = BoxStructs(entities);
		const std::vector<const MapKit::MkEntity*> boxes = BoxNodes(source);
		const bool anyStart = !boxes.empty() && boxes[0]->Prop("start_here") == "true";
		for (std::size_t k = 0; k < boxes.size(); ++k) {
			if (k >= chests.size()) {
				std::printf("  mystery box %s: %s has only %zu box locations, skipped\n", boxes[k]->path.c_str(), map.c_str(),
					chests.size());
				continue;
			}
			MapEntity& chest = entities[chests[k]];
			const bool start = boxes[k]->Prop("start_here") == "true" || (!anyStart && k == 0);
			SetEntityOrigin(chest, place(boxes[k]->origin), std::nullopt);
			SetEntityAngles(chest, { 0.0f, NormalizeYaw(boxes[k]->angles[1] + kBoxStructYaw), 0.0f });
			SetEntityText(chest, "start_exclude", start ? "0" : "1");
			placed[chests[k]] = 1;
			std::printf("  mystery box %s: entity %u (%s) moved there%s\n", boxes[k]->path.c_str(), chest.id,
				chest.Text("script_noteworthy").c_str(), start ? ", the box starts here" : "");
		}
		if (boxes.empty() && !chests.empty()) {
			// The box script picks its start from these; with none at all it may stop the level's scripts.
			SetEntityOrigin(entities[chests[0]], graveyard, std::nullopt);
			SetEntityText(entities[chests[0]], "start_exclude", "0");
			placed[chests[0]] = 1;
			std::printf("  mystery box: the source has none; %s's %s goes to the graveyard\n", map.c_str(),
				entities[chests[0]].Text("script_noteworthy").c_str());
		}
		if (const std::string names = dropUnplaced(chests, "script_noteworthy"); !names.empty()) {
			std::printf("  box locations taken out:%s\n", names.c_str());
		}

		// Wall buys: the chalk (named after the weapon) and the gun that targets it.
		const std::vector<std::size_t> chalks = retail("content_key", "wallbuy_chalk");
		const std::vector<std::size_t> guns = retail("content_key", "wallbuy_gun");
		const auto gunOf = [&](std::size_t chalk) {
			const std::string name = entities[chalk].Text("targetname");
			const auto it = std::ranges::find_if(guns, [&](std::size_t i) { return !name.empty() && entities[i].Text("target") == name; });
			return it == guns.end() ? entities.size() : *it;
		};
		bool costNoted = false;
		for (const MapKit::MkEntity* buy : sourceOf("wall_buy")) {
			const std::string weapon = buy->Prop("weapon");
			const auto it = std::ranges::find_if(chalks, [&](std::size_t i) {
				return !placed[i] && entities[i].Text("script_noteworthy") == weapon;
			});
			if (it == chalks.end()) {
				std::string offered;
				for (const std::size_t i : chalks) {
					offered += " " + entities[i].Text("script_noteworthy");
				}
				std::printf("  wall buy %s (%s): %s has no (other) wall buy of it, skipped. It has:%s\n", weapon.c_str(),
					buy->path.c_str(), map.c_str(), offered.c_str());
				continue;
			}
			std::array<float, 3> at = place(buy->origin);
			at[2] += kWallBuyHeight;
			const float facing = buy->angles[1];
			SetEntityOrigin(entities[*it], at, std::nullopt);
			SetEntityAngles(entities[*it], { 0.0f, NormalizeYaw(facing + 90.0f), 0.0f });
			placed[*it] = 1;
			if (const std::size_t gun = gunOf(*it); gun < entities.size()) {
				SetEntityOrigin(entities[gun], at, std::nullopt);
				SetEntityAngles(entities[gun], { 0.0f, NormalizeYaw(facing - 90.0f), 0.0f });
				placed[gun] = 1;
			}
			std::printf("  wall buy %s: entities %u + gun moved to %s\n", weapon.c_str(), entities[*it].id, buy->path.c_str());
			if (!costNoted && !buy->Prop("cost").empty()) {
				std::printf("  (wall buy costs are the game's own for each weapon: the source's cost is not used yet)\n");
				costNoted = true;
			}
		}
		std::string wallNames;
		for (const std::size_t chalk : chalks) {
			if (!placed[chalk]) {
				keep[chalk] = 0;
				wallNames += " " + entities[chalk].Text("script_noteworthy");
				if (const std::size_t gun = gunOf(chalk); gun < entities.size()) {
					keep[gun] = 0;
				}
			}
		}
		if (!wallNames.empty()) {
			std::printf("  wall buys taken out:%s\n", wallNames.c_str());
		}

		// Entities added below (copies), appended at the end, numbered past every one of <map>'s.
		std::vector<MapEntity> added;
		std::uint32_t lastId = NextEntityId(entities) - 1;

		// Devices: the ammo caches, Armor Stations, Wunderfizzes, crafting tables and Pack-a-Punch machines. Each is
		// a content_struct of one content_key under one content_instance, and each struct spawns one device turned
		// as the struct is, its front on the struct's +X (the scripts put the use prompt 24 units along
		// anglestoforward), so the struct's yaw is the source's facing. Each source node takes one of <map>'s
		// structs of its kind (moved); past <map>'s count, copies of the first (same keys, so the same parent);
		// <map>'s others are taken out. zm_silver's Armor Station needs power (script_noteworthy "power": the script shows
		// "need power" until the "power_on" flag): the key stays when the source has a power switch and the station's
		// needs_power is on (the default), and is dropped otherwise, so the station works from the start.
		//
		// Pack-a-Punch is the weapon_machine content object (zm_common script_6fc2be37feeb317b): each
		// weapon_machine_spawn struct gets the machine p9_fxanim_zm_gp_pap_xmodel, whose menu
		// (sr_weapon_upgrade_menu) sells the Pack-a-Punch tiers (5000, 15000, 30000) and the ammo mods. It spawns at
		// start_zombie_round_logic unless level.var_ce45839f names a notify to wait for: zm_silver's Pack-a-Punch
		// quest sets it (#"pap_quest_completed"), mapkit's level script does not. The Arsenal is the armor_machine
		// (script_7a5293d92c61c788, sr_armor_menu: armor and weapon rarity), so MkArsenal builds an Armor Station.
		struct Device {
			const char* cls;
			const char* contentKey;
			const char* what;
			const char* alias = nullptr; // a second source class that builds the same device
		};
		for (const Device& device : { Device{ "ammo_cache", "ammo_cache_spawn", "ammo cache" },
				 Device{ "armor_station", "armor_machine", "arsenal (armor station)", "arsenal" },
				 Device{ "wunderfizz", "perk_machine_choice", "wunderfizz" },
				 Device{ "crafting_table", "crafting_table", "crafting table" },
				 Device{ "pack_a_punch", "weapon_machine_spawn", "pack-a-punch" } }) {
			const std::vector<std::size_t> found = retail("content_key", device.contentKey);
			std::vector<const MapKit::MkEntity*> wanted = sourceOf(device.cls);
			if (device.alias) {
				std::ranges::copy(sourceOf(device.alias), std::back_inserter(wanted));
			}
			if (!wanted.empty() && found.empty()) {
				std::printf("  %s: %s has no %s struct to move, %zu skipped\n", device.what, map.c_str(), device.contentKey,
					wanted.size());
				continue;
			}
			std::size_t copies = 0;
			for (std::size_t k = 0; k < wanted.size(); ++k) {
				MapEntity* at = nullptr;
				if (k < found.size()) {
					at = &entities[found[k]];
					placed[found[k]] = 1;
				}
				else {
					added.push_back(entities[found[0]]);
					at = &added.back();
					at->id = ++lastId;
					++copies;
				}
				SetEntityOrigin(*at, place(wanted[k]->origin), std::nullopt);
				SetEntityAngles(*at, { 0.0f, NormalizeYaw(wanted[k]->angles[1]), 0.0f });
				if (!powerSwitch || wanted[k]->Prop("needs_power") == "false") {
					std::erase_if(at->keys, [](const EntityKey& key) {
						return key.key == "script_noteworthy" && key.type == EntityValueType::String && key.text == "power";
					});
				}
			}
			if (!wanted.empty()) {
				std::printf("  %s: %zu placed (%zu of %s's moved, %zu copies)\n", device.what, wanted.size(),
					wanted.size() - copies, map.c_str(), copies);
			}
			if (const std::size_t out = std::ranges::count_if(found, [&](std::size_t i) { return !placed[i]; }); out) {
				dropUnplaced(found, "content_key");
				std::printf("  %s: %zu of %s's taken out\n", device.what, out, map.c_str());
			}
		}

		// Exfil. zm_silver's is one content_instance "exfil" whose structs are heli_spawn (where the helicopter
		// spawns), exfil_loc, landing_zone, smoke and the starts of the helicopter's three vehicle paths (in, a loop
		// round the landing point, the landing: getvehiclenode(<struct's targetname>, "target") finds each path's
		// first node), plus the radio struct exfil_radio outside the instance. The instance, its structs and the
		// three paths move and turn as one, so that the helicopter lands at the source's MkExfil facing its arrow
		// (zm_silver's lands facing exfil_loc's yaw). Vehicle nodes are made from the entity list at load
		// (VehNode_SpawnFromMapEnt_cand 0x7FF723D0A600), so the moved nodes are the path flown.
		const std::vector<const MapKit::MkEntity*> exfils = sourceOf("exfil");
		const std::vector<const MapKit::MkEntity*> radios = sourceOf("exfil_radio");
		std::vector<std::size_t> exfilSet; // the instance, its location, its structs
		std::size_t landing = entities.size(), loc = entities.size();
		for (std::size_t i = 0; i < entities.size(); ++i) {
			if (entities[i].Text("variantName") == "content_instance" && entities[i].Text("content_script_name") == "exfil") {
				exfilSet.push_back(i);
				const std::string name = entities[i].Text("targetname");
				for (std::size_t j = 0; j < entities.size(); ++j) {
					if (!name.empty() && entities[j].Text("target") == name && entities[j].Text("variantName") == "content_struct") {
						exfilSet.push_back(j);
						landing = entities[j].Text("content_key") == "landing_zone" ? j : landing;
						loc = entities[j].Text("content_key") == "exfil_loc" ? j : loc;
					}
					else if (entities[j].Text("targetname") == entities[i].Text("target")
						&& entities[j].Text("variantName") == "content_location") {
						exfilSet.push_back(j);
					}
				}
				break;
			}
		}
		if (!exfils.empty() && (landing == entities.size() || loc == entities.size())) {
			std::printf("  exfil %s: %s has no exfil (landing_zone and exfil_loc) to move, skipped\n", exfils[0]->path.c_str(),
				map.c_str());
		}
		else if (!exfils.empty()) {
			const MapKit::MkEntity& to = *exfils[0];
			const std::array<float, 3> from = entities[landing].origin;
			const std::array<float, 3> target = place(to.origin);
			const float turn = to.angles[1] - entities[loc].angles[1];
			const float radians = turn * 3.14159265358979f / 180.0f;
			const float c = std::cos(radians), s = std::sin(radians);
			const auto move = [&](MapEntity& entity) {
				const float dx = entity.origin[0] - from[0], dy = entity.origin[1] - from[1];
				SetEntityOrigin(entity, { target[0] + c * dx - s * dy, target[1] + s * dx + c * dy,
					target[2] + entity.origin[2] - from[2] }, std::nullopt);
				SetEntityAngles(entity, { entity.angles[0], NormalizeYaw(entity.angles[1] + turn), entity.angles[2] });
			};
			// The paths: from each struct, the vehicle node that targets it, then node to node by target.
			const auto isNode = [](const MapEntity& entity) { return entity.Text("classname").starts_with("info_vehicle_node"); };
			std::vector<char> path(entities.size(), 0);
			std::size_t nodes = 0;
			for (const std::size_t part : exfilSet) {
				const std::string name = entities[part].Text("targetname");
				for (std::size_t i = 0; i < entities.size(); ++i) {
					if (name.empty() || !isNode(entities[i]) || entities[i].Text("target") != name) {
						continue;
					}
					for (std::size_t at = i; at < entities.size() && !path[at];) {
						path[at] = 1;
						++nodes;
						const std::string next = entities[at].Text("target");
						at = static_cast<std::size_t>(std::ranges::find_if(entities, [&](const MapEntity& entity) {
							return isNode(entity) && !next.empty() && entity.Text("targetname") == next;
						}) - entities.begin());
					}
				}
			}
			for (std::size_t i = 0; i < entities.size(); ++i) {
				if (path[i]) {
					move(entities[i]);
				}
			}
			for (const std::size_t part : exfilSet) {
				move(entities[part]);
				placed[part] = 1;
			}
			std::printf("  exfil %s: zm_silver's (%zu structs, %zu helicopter path nodes) moved there, turned %.0f degrees%s\n",
				to.path.c_str(), exfilSet.size(), nodes, NormalizeYaw(turn), exfils.size() > 1 ? "; the other exfils are skipped" : "");
		}
		const std::vector<std::size_t> radio = retail("targetname", "exfil_radio");
		if (!radios.empty() && radio.empty()) {
			std::printf("  exfil radio: %s has no exfil_radio struct to move, skipped\n", map.c_str());
		}
		else if (!radios.empty()) {
			SetEntityOrigin(entities[radio[0]], place(radios[0]->origin), std::nullopt);
			SetEntityAngles(entities[radio[0]], { 0.0f, NormalizeYaw(radios[0]->angles[1]), 0.0f });
			placed[radio[0]] = 1;
			std::printf("  exfil radio %s: entity %u moved there%s\n", radios[0]->path.c_str(), entities[radio[0]].id,
				exfils.empty() ? " (the source has no exfil: it calls nothing)" : "");
		}
		else if (!exfils.empty()) {
			std::printf("  exfil: the source has no exfil radio, so players can't call it\n");
		}

		for (std::size_t i = 0; i < entities.size(); ++i) {
			if (placed[i]) {
				kept.insert(entities[i].id);
			}
		}

		std::size_t removed = 0;
		for (std::size_t i = entities.size(); i-- > 0;) {
			if (!keep[i]) {
				entities.erase(entities.begin() + static_cast<std::ptrdiff_t>(i));
				++removed;
			}
		}
		std::printf("  %zu of %s's entities taken out, %zu left\n", removed, map.c_str(), entities.size());
		std::ranges::move(added, std::back_inserter(entities));
	}

	// P1 step 2 (docs/mapkit-plan.md): the level's own entity and trigger lists, and its zone graph.
	//
	// Zones are volume zones. zm_zonemgr::zone_init finds a zone's volumes by targetname (info_volume or
	// trigger_box, in the trigger list) and its zombie spawn structs through the first volume's "target", and
	// zm_utility checks the play area against every volume with script_noteworthy "player_volume". Die
	// Maschine's zones are node zones instead: path nodes named after the zone. Nodes cannot be authored: the
	// node array is compiled into the game_map asset (with the navmesh), and a node_* entity in the entity list
	// only names the next node of that array, in order (G_SpawnMapEntity_cand, 0x7FF7230D13A0). With no node
	// named after a zone, every zone lookup of zm_common uses the volumes.
	struct LevelZone {
		std::string name;
		std::string path;
		bool start = false;
		std::array<float, 3> center{};   // world
		std::array<float, 3> halfSize{}; // along the zone's own axes
		float yaw = 0;
		std::array<float, 3> mins{};     // world bounds, turn included
		std::array<float, 3> maxs{};
	};

	struct LevelDoor {
		std::string path;
		std::string flag;                // set by zm_blockers when a player buys the door (its trigger's script_flag)
		std::vector<std::string> opens;
		std::string model;               // its look, a game name or #hash; "" = kDefaultDoorModel
		std::string cost;
		bool power = false;              // opens by itself when the power comes on (needs_power, with a power switch)
		std::array<float, 3> origin{};   // world: the node's floor point, where the model stands
		float yaw = 0;
		std::array<float, 3> center{};   // world: the box's middle
		std::array<float, 3> halfSize{}; // along the box's own axes
		std::array<float, 3> mins{};
		std::array<float, 3> maxs{};
	};

	// A volume entity's box in world space: its center, half size (own axes) and world bounds.
	template <typename Place>
	void VolumeBox(const MapKit::MkEntity& entity, const Place& place, std::array<float, 3>& center,
		std::array<float, 3>& halfSize, std::array<float, 3>& mins, std::array<float, 3>& maxs) {
		const MapKit::MkBounds bounds = entity.bounds.value_or(MapKit::MkBounds{});
		const float radians = entity.angles[1] * 3.14159265358979f / 180.0f;
		const float c = std::cos(radians), s = std::sin(radians);
		std::array<float, 3> local{};
		for (int k = 0; k < 3; ++k) {
			local[k] = (bounds.min[k] + bounds.max[k]) / 2;
			halfSize[k] = (bounds.max[k] - bounds.min[k]) / 2;
		}
		const std::array<float, 3> origin = place(entity.origin);
		center = { origin[0] + c * local[0] - s * local[1], origin[1] + s * local[0] + c * local[1], origin[2] + local[2] };
		const std::array<float, 3> extent = { std::fabs(c) * halfSize[0] + std::fabs(s) * halfSize[1],
			std::fabs(s) * halfSize[0] + std::fabs(c) * halfSize[1], halfSize[2] };
		for (int k = 0; k < 3; ++k) {
			mins[k] = center[k] - extent[k];
			maxs[k] = center[k] + extent[k];
		}
	}

	bool BoxesTouch(const std::array<float, 3>& aMins, const std::array<float, 3>& aMaxs, const std::array<float, 3>& bMins,
		const std::array<float, 3>& bMaxs, float margin) {
		for (int k = 0; k < 3; ++k) {
			if (aMins[k] > bMaxs[k] + margin || bMins[k] > aMaxs[k] + margin) {
				return false;
			}
		}
		return true;
	}

	// The source's zones and doors. False (and why) when the zones cannot make a level: none, no start zone, or
	// two with one name.
	template <typename Place>
	bool ReadZones(const MapKit::MkMap& source, const Place& place, std::vector<LevelZone>& zones, std::vector<LevelDoor>& doors) {
		for (const MapKit::MkEntity& entity : source.entities) {
			if (entity.cls == "zone") {
				LevelZone zone;
				zone.name = entity.Prop("zone_name");
				zone.path = entity.path;
				zone.start = entity.Prop("active_at_start") == "true";
				zone.yaw = entity.angles[1];
				VolumeBox(entity, place, zone.center, zone.halfSize, zone.mins, zone.maxs);
				if (zone.name.empty() || zone.name.find_first_of(" \"\\") != std::string::npos) {
					std::fprintf(stderr, "zone %s: zone_name '%s' is empty or has a space, quote or backslash\n", entity.path.c_str(),
						zone.name.c_str());
					return false;
				}
				if (std::ranges::any_of(zones, [&](const LevelZone& other) { return other.name == zone.name; })) {
					std::fprintf(stderr, "two zones are named '%s'\n", zone.name.c_str());
					return false;
				}
				if (entity.angles[0] != 0.0f || entity.angles[2] != 0.0f) {
					std::printf("  zone %s: only its yaw is used (pitch and roll are dropped)\n", zone.name.c_str());
				}
				zones.push_back(std::move(zone));
			}
			else if (entity.cls == "door") {
				LevelDoor door;
				door.path = entity.path;
				door.flag = std::format("mapkit_door_{}", doors.size());
				if (const auto it = entity.props.find("opens"); it != entity.props.end() && it->is_array()) {
					for (const auto& zone : *it) {
						if (zone.is_string()) {
							door.opens.push_back(zone.get<std::string>());
						}
					}
				}
				door.model = entity.Prop("model");
				door.cost = entity.Prop("cost");
				door.power = entity.Prop("needs_power") == "true";
				door.origin = place(entity.origin);
				door.yaw = entity.angles[1];
				VolumeBox(entity, place, door.center, door.halfSize, door.mins, door.maxs);
				doors.push_back(std::move(door));
			}
		}
		if (zones.empty()) {
			std::fprintf(stderr, "the source has no zone: add an MkZone around the start area, with active_at_start on\n");
			return false;
		}
		if (std::ranges::none_of(zones, [](const LevelZone& zone) { return zone.start; })) {
			std::fprintf(stderr, "no zone has active_at_start on\n");
			return false;
		}
		return true;
	}

	// A window barrier (MkBarrier): boards zombies tear down to climb in, and players rebuild. `name` is the targetname
	// and script_string its pieces share (AddBarriers), and what a barrier spawner's struct names (ComposeLevelEntities).
	struct LevelBarrier {
		std::string path;
		std::string zone;
		std::string kind;                // wood or concrete
		std::string name;                // mapkit_barrier_<n>
		std::array<float, 3> origin{};   // world: the foot of the opening
		float yaw = 0;                   // the node's facing: into the play area
	};

	template <typename Place>
	std::vector<LevelBarrier> ReadBarriers(const MapKit::MkMap& source, const Place& place, const std::vector<LevelZone>& zones) {
		std::vector<LevelBarrier> barriers;
		for (const MapKit::MkEntity& entity : source.entities) {
			if (entity.cls != "barrier") {
				continue;
			}
			LevelBarrier barrier;
			barrier.path = entity.path;
			barrier.zone = entity.Prop("zone");
			barrier.kind = entity.Prop("kind") == "concrete" ? "concrete" : "wood";
			barrier.name = std::format("mapkit_barrier_{}", barriers.size());
			barrier.origin = place(entity.origin);
			barrier.yaw = entity.angles[1];
			if (std::ranges::find(zones, barrier.zone, &LevelZone::name) == zones.end()) {
				std::printf("  barrier %s: zone '%s' does not exist, so no spawner uses it\n", entity.path.c_str(), barrier.zone.c_str());
			}
			barriers.push_back(std::move(barrier));
		}
		return barriers;
	}

	// The barrier a barrier spawner's zombies head for: the nearest one of its zone. Null when its zone has none.
	const LevelBarrier* SpawnerBarrier(const std::vector<LevelBarrier>& barriers, const std::string& zone,
		const std::array<float, 3>& at) {
		const LevelBarrier* best = nullptr;
		float bestDistance = FLT_MAX;
		for (const LevelBarrier& barrier : barriers) {
			if (barrier.zone != zone) {
				continue;
			}
			float distance = 0;
			for (int k = 0; k < 3; ++k) {
				distance += (barrier.origin[k] - at[k]) * (barrier.origin[k] - at[k]);
			}
			if (distance < bestDistance) {
				best = &barrier;
				bestDistance = distance;
			}
		}
		return best;
	}

	// The crafting table items a map may turn off, by their .mkmap name (MkCraftingTable.ITEMS): the game setting
	// the crafting menu checks for each (script_4ccfb58a9443a60b, names recovered from their hashes 2026-09-27).
	constexpr std::array<std::pair<std::string_view, std::string_view>, 19> kCraftingItems = { {
		{ "frag", "zmenablefraggrenade" }, { "semtex", "zmenablesemtex" }, { "molotov", "zmenablemolotov" },
		{ "hatchet", "zmenablehatchet" }, { "c4", "zmenablec4" }, { "decoy", "zmenabledecoygrenade" },
		{ "stun", "zmenablestungrenade" }, { "monkey", "zmenablecymbalmonkey" }, { "stimshot", "zmenablestimshot" },
		{ "self_revive", "zmenableselfrevive" }, { "turret", "zmenablescorestreakultimateturret" },
		{ "chopper_gunner", "zmenablescorestreakchoppergunner" }, { "death_machine", "zmenablescorestreakdeathmachine" },
		{ "flamethrower", "zmenablescorestreakflamethrower" }, { "bow", "zmenablescorestreakbow" },
		{ "napalm", "zmenablescorestreaknapalmstrike" }, { "pineapple_gun", "zmenablescorestreakpineapplegun" },
		{ "hand_cannon", "zmenablescorestreakhandcannon" }, { "rcxd", "zmenablescorestreakarcxd" },
	} };

	// The source's exfil (MkExfil), as the exfil script (script_7b1cd3908a825fdd) reads it from level fields.
	struct LevelExfil {
		std::string path;
		int holdSeconds = 90;
		int attackRadius = 600;
		int attackHeight = 200;
		std::vector<std::string> zones;  // players must reach one of these
		bool radioLiveAtStart = false;   // for testing: the radio works from the start, not after round 10
	};

	// The level-wide settings the source's objects make: the exfil, and the crafting items turned off.
	// The targetname of the props that are not solid (AddProps): the zones script's settings() calls notsolid on them.
	constexpr const char* kNonSolidProps = "mapkit_prop_nonsolid";

	struct LevelSettings {
		std::optional<LevelExfil> exfil;
		std::vector<std::string_view> settingsOff; // game settings (kCraftingItems) the level sets to 0
		bool nonSolidProps = false;                // a prop with solid off: settings() makes kNonSolidProps notsolid
		bool powerSwitch = false;                  // an MkPowerSwitch: the perk machines wait for the power
	};

	// Whether a zone's box holds a world point.
	bool ZoneHolds(const LevelZone& zone, const std::array<float, 3>& point) {
		const float radians = zone.yaw * 3.14159265358979f / 180.0f;
		const float c = std::cos(radians), s = std::sin(radians);
		const float dx = point[0] - zone.center[0], dy = point[1] - zone.center[1];
		const std::array<float, 3> local = { c * dx + s * dy, -s * dx + c * dy, point[2] - zone.center[2] };
		for (int k = 0; k < 3; ++k) {
			if (std::fabs(local[k]) > zone.halfSize[k]) {
				return false;
			}
		}
		return true;
	}

	template <typename Place>
	LevelSettings ReadLevelSettings(const MapKit::MkMap& source, const Place& place, const std::vector<LevelZone>& zones) {
		LevelSettings settings;
		settings.powerSwitch = HasPowerSwitch(source);
		std::vector<const MapKit::MkEntity*> tables;
		for (const MapKit::MkEntity& entity : source.entities) {
			if (entity.cls == "crafting_table") {
				tables.push_back(&entity);
			}
			else if (entity.cls == "prop" && entity.Prop("solid") == "false" && !entity.Prop("model").empty()) {
				settings.nonSolidProps = true;
			}
			else if (entity.cls == "exfil" && !settings.exfil) {
				LevelExfil exfil;
				exfil.path = entity.path;
				const auto number = [&entity](const char* name, int fallback) {
					const std::string text = entity.Prop(name);
					return text.empty() ? fallback : std::atoi(text.c_str());
				};
				exfil.radioLiveAtStart = entity.Prop("radio_live_at_start") == "true";
				exfil.holdSeconds = number("hold_seconds", exfil.holdSeconds);
				exfil.attackRadius = number("attack_radius", exfil.attackRadius);
				exfil.attackHeight = number("attack_height", exfil.attackHeight);
				if (const auto it = entity.props.find("zones"); it != entity.props.end() && it->is_array()) {
					for (const auto& zone : *it) {
						if (zone.is_string() && std::ranges::find(zones, zone.get<std::string>(), &LevelZone::name) != zones.end()) {
							exfil.zones.push_back(zone.get<std::string>());
						}
						else if (zone.is_string()) {
							std::printf("  exfil %s: zone '%s' doesn't exist; ignored\n", entity.path.c_str(), zone.get<std::string>().c_str());
						}
					}
				}
				if (exfil.zones.empty()) {
					std::array<float, 3> landing = place(entity.origin);
					landing[2] += 8.0f;
					for (const LevelZone& zone : zones) {
						if (ZoneHolds(zone, landing)) {
							exfil.zones.push_back(zone.name);
						}
					}
				}
				if (exfil.zones.empty()) {
					std::printf("  exfil %s: its landing point is in no zone and it names none, so no player can reach it\n",
						entity.path.c_str());
				}
				settings.exfil = std::move(exfil);
			}
		}
		// One list for the map: an item sells only if every table sells it.
		if (!tables.empty()) {
			for (const auto& [item, setting] : kCraftingItems) {
				const bool sold = std::ranges::all_of(tables, [item](const MapKit::MkEntity* table) {
					const auto it = table->props.find("items");
					return it == table->props.end() || !it->is_array()
						|| std::ranges::find(*it, nlohmann::json(std::string(item))) != it->end();
				});
				if (!sold) {
					settings.settingsOff.push_back(setting);
				}
			}
		}
		return settings;
	}

	// scripts/zm/zm_silver_zones.gsc for the source (mapkit/level/zm_silver/zm_silver.gsc calls it): the start
	// zones, every zone, and how they connect. Start zones connect to each other always; a zone a door opens
	// connects to the zones by that door (the ones its box touches, else the start zones) once the door's flag
	// is set. Then settings(): the level-wide settings of the source's objects (LevelSettings), and what names an asset of
	// <map>'s zone only while that zone loads (`library`).
	std::string ZonesScript(const MapKit::MkMap& source, const std::vector<LevelZone>& zones, const std::vector<LevelDoor>& doors,
		const LevelSettings& settings, bool library) {
		std::vector<std::string> starts;
		for (const LevelZone& zone : zones) {
			if (zone.start) {
				starts.push_back(zone.name);
			}
		}
		std::string text = std::format(
			"// Generated by cwlink build from '{}' ({}): the map's zones. Rebuild to change it; edits here are lost.\n"
			"// It replaces scripts/zm/zm_silver_zones.gsc, which zm_silver links on its own (script_using).\n\n"
			"#using scripts\\core_common\\flag_shared;\n"
			"#using scripts\\zm_common\\zm_zonemgr;\n\n"
			"#namespace mapkit_zones;\n\n"
			"// The zones active when the match starts (MkZone active_at_start).\n"
			"function start_zones()\n{{\n    return array( ",
			source.name, source.title);
		for (std::size_t i = 0; i < starts.size(); ++i) {
			text += std::format("{}\"{}\"", i ? ", " : "", starts[i]);
		}
		text += " );\n}\n\n"
			"// level.zone_manager_init_func: every zone, then the connections.\n"
			"function init()\n{\n"
			"    level flag::init( \"always_on\" );\n"
			"    level flag::set( \"always_on\" );\n";
		for (const LevelZone& zone : zones) {
			text += std::format("    zm_zonemgr::zone_init( \"{}\" );{}\n", zone.name, zone.start ? "   // active at start" : "");
		}
		for (std::size_t a = 0; a < starts.size(); ++a) {
			for (std::size_t b = a + 1; b < starts.size(); ++b) {
				text += std::format("    zm_zonemgr::add_adjacent_zone( \"{}\", \"{}\", \"always_on\", 0 );\n", starts[a], starts[b]);
			}
		}
		for (const LevelDoor& door : doors) {
			for (const std::string& opened : door.opens) {
				const auto target = std::ranges::find(zones, opened, &LevelZone::name);
				if (target == zones.end()) {
					std::printf("  door %s: opens '%s', which is no zone; ignored\n", door.path.c_str(), opened.c_str());
					continue;
				}
				std::vector<std::string> from;
				for (const LevelZone& zone : zones) {
					if (zone.name != opened && BoxesTouch(zone.mins, zone.maxs, door.mins, door.maxs, 16.0f)) {
						from.push_back(zone.name);
					}
				}
				const bool byDoor = !from.empty();
				if (!byDoor) {
					from = starts;
				}
				text += std::format("    // {}: opens {} from {} (the zones {})\n", door.path, opened,
					from.size() == 1 ? from[0] : "several", byDoor ? "its box touches" : "active at start: its box touches none");
				for (const std::string& zone : from) {
					if (zone != opened) {
						text += std::format("    zm_zonemgr::add_adjacent_zone( \"{}\", \"{}\", \"{}\", 0 );\n", zone, opened, door.flag);
					}
				}
			}
		}
		text += "}\n\n"
			"// The level's settings from the map's objects; the level script calls it before load::main.\n"
			"function settings()\n{\n";
		const auto line = [&text](const std::string& code, std::string_view comment) {
			text += std::format("    {:<42}{}// {}\n", code, code.size() >= 42 ? " " : "", comment);
		};
		if (library) {
			// zm_weapons::init_weapons loads the base table (core_common's) and then this one on top, with no check that it
			// exists (Scr_GetStringTableArg_cand 0x7FF729605590): only Die Maschine's zone holds it.
			text += "    // Die Maschine's own weapons (zm_weapons init_weapons), from its zone, which loads under this map.\n";
			line("level.var_d0ab70a2 = #\"hash_5e105c88ae5d540f\";", "Die Maschine's weapon spec table");
		}
		if (settings.exfil) {
			const LevelExfil& exfil = *settings.exfil;
			text += std::format("    // Exfil ({}), read by the exfil script (script_7b1cd3908a825fdd) when it starts.\n", exfil.path);
			// Its waves are the script bundle <prefix><n>, n from 1 to 4 by round, found with no check that it exists. Die
			// Maschine's exfil_silver_<n> are in its own zone; zm_common holds exfil_realm_1 to _5 (Outbreak's).
			line(library ? "level.var_dafeed10 = \"exfil_silver_\";" : "level.var_dafeed10 = \"exfil_realm_\";",
				library ? "its zombie waves: Die Maschine's exfil_silver_<n>, by round"
					: "its zombie waves: zm_common's exfil_realm_<n> (Outbreak's), by round");
			line(std::format("level.var_aaf7505f = {};", exfil.holdSeconds), "seconds to hold out");
			line(std::format("level.var_26ed6a07 = {};", exfil.attackRadius), "zombies attack from this far around the landing point");
			line(std::format("level.var_c86f12d4 = {};", exfil.attackHeight), "...and this far above or below it");
			line("level.var_baed3b8e = 1750;", "path search limit during the exfil, as Die Maschine");
			line("level.var_ac94c2b8 = 2;", "find-flesh service during the exfil, as Die Maschine");
			std::string zoneList;
			for (std::size_t i = 0; i < exfil.zones.size(); ++i) {
				zoneList += std::format("{}\"{}\"", i ? ", " : "", exfil.zones[i]);
			}
			line(std::format("level.var_ad5e81fe = array( {} );", zoneList), "the zones players must reach");
			if (exfil.radioLiveAtStart) {
				line("level thread exfil_radio_live();", "MkExfil radio_live_at_start: for testing");
			}
		}
		if (settings.powerSwitch) {
			text += "    // The map has a power switch (MkPowerSwitch): the perk machines wait for it (zm_perks\n"
				"    // get_perk_machine_start_state; Quick Revive still works alone).\n";
			line("level.vending_machines_powered_on_at_start = 0;", "the level script sets 1 before this");
		}
		if (!settings.settingsOff.empty()) {
			text += "    // Crafting table items turned off (MkCraftingTable items).\n";
			for (const std::string_view setting : settings.settingsOff) {
				text += std::format("    setgametypesetting( #\"{}\", 0 );\n", setting);
			}
		}
		if (settings.nonSolidProps) {
			text += std::format("    // Props with solid off (MkProp): players and zombies pass through them.\n"
				"    foreach ( prop in getentarray( \"{}\", \"targetname\" ) )\n    {{\n        prop notsolid();\n    }}\n",
				kNonSolidProps);
		}
		text += "}\n";
		if (settings.exfil && settings.exfil->radioLiveAtStart) {
			// zclassic sets the flag for 120 s after rounds 10, 15, 20...; the exfil script sets rbz_exfil_allowed once it
			// has spawned the radio, then makes the radio usable whenever the flag is set (script_7b1cd3908a825fdd).
			text += "\n// MkExfil radio_live_at_start (for testing): the radio can be used from the start, not only after rounds\n"
				"// 10, 15, 20...\n"
				"function private exfil_radio_live()\n{\n"
				"    level flag::wait_till( \"rbz_exfil_allowed\" );\n"
				"    level flag::set( \"rbz_exfil_beacon_active\" );\n"
				"}\n";
		}
		return text;
	}

	// An entity made from one of <map>'s: its keys cut to `keep` (classname, origin and angles always stay), then
	// `set` (a String key of the template keeps its stored bytes; a new one is written like the template's
	// classname key).
	MapEntity EntityFrom(const MapEntity& templ, std::uint32_t id, const std::array<float, 3>& origin, float yaw,
		std::initializer_list<std::string_view> keep, std::initializer_list<std::pair<std::string_view, std::string>> set) {
		MapEntity entity = templ;
		entity.id = id;
		std::erase_if(entity.keys, [&](const EntityKey& key) {
			return key.key != "classname" && key.key != "origin" && key.key != "angles"
				&& std::ranges::find(keep, std::string_view(key.key)) == keep.end();
		});
		const EntityKey* classname = templ.Find("classname");
		for (const auto& [name, text] : set) {
			auto it = std::ranges::find_if(entity.keys, [&](const EntityKey& key) { return key.key == name; });
			if (it == entity.keys.end()) {
				const auto from = std::ranges::find_if(templ.keys, [&](const EntityKey& key) {
					return key.key == name && key.type == EntityValueType::String;
				});
				EntityKey key;
				if (from != templ.keys.end()) {
					key = *from;
				}
				else {
					key.key = std::string(name);
					key.type = EntityValueType::String;
					key.flags = classname ? classname->flags : 1;
				}
				entity.keys.push_back(std::move(key));
				it = entity.keys.end() - 1;
			}
			it->text = text;
		}
		SetEntityOrigin(entity, origin, std::nullopt);
		SetEntityAngles(entity, { 0.0f, yaw, 0.0f });
		return entity;
	}

	// A String key set to `text`, added (flags as the classname key's) when the entity has none.
	void SetOrAddText(MapEntity& entity, std::string_view name, const std::string& text) {
		for (EntityKey& key : entity.keys) {
			if (key.key == name && key.type == EntityValueType::String) {
				key.text = text;
				return;
			}
		}
		const EntityKey* classname = entity.Find("classname");
		EntityKey key;
		key.key = std::string(name);
		key.type = EntityValueType::String;
		key.flags = classname ? classname->flags : 1;
		key.text = text;
		entity.keys.push_back(std::move(key));
	}

	// The first of <map>'s entities (or triggers) matching `match`, or null (and why).
	template <typename Match>
	const MapEntity* Template(const std::vector<MapEntity>& entities, const char* what, const Match& match) {
		const auto it = std::ranges::find_if(entities, match);
		if (it == entities.end()) {
			std::fprintf(stderr, "no %s in the base map's entities to copy\n", what);
			return nullptr;
		}
		return &*it;
	}

	// A map that leaves <map>'s zones out has only the AI types zm_common holds (and registers: its bgcache lists them, so
	// their anim trees load). An actor_spawner_<aitype> entity spawns that AI type, so each of <map>'s spawner templates
	// whose AI type would be gone takes zm_common's closest. For Die Maschine (aitype tables of the GSC dump, 2026-10-01):
	// its soldier zombies and their wall-pull variant (one more animation table) the plain spawner_zm_zombie, and its
	// armoured heavy Outbreak's spawner_bo5_zombie_sr_armor_heavy, with the same character, state machine, behavior tree
	// and score type. False when a template has neither its AI type nor a stand-in.
	constexpr std::pair<std::string_view, std::string_view> kStayingSpawners[] = {
		{ "spawner_zm_zombie_ndu", "spawner_zm_zombie" },
		{ "spawner_zm_zombie_ndu_wall_pull", "spawner_zm_zombie" },
		{ "spawner_bo5_zombie_zm_silver_armor_heavy", "spawner_bo5_zombie_sr_armor_heavy" },
	};

	bool UseStayingSpawners(std::vector<MapEntity>& entities, const std::set<AssetKey>& stays) {
		constexpr std::uint64_t kAiType = 0x38;
		constexpr std::string_view kPrefix = "actor_";
		std::map<std::string, std::pair<std::string, std::size_t>> replaced;
		std::size_t kept = 0;
		for (MapEntity& entity : entities) {
			const std::string classname = entity.Text("classname");
			if (!classname.starts_with("actor_spawner")) {
				continue;
			}
			const std::string aitype = classname.substr(kPrefix.size());
			if (stays.contains({ kAiType, HashName(aitype) })) {
				++kept;
				continue;
			}
			const auto stand = std::ranges::find(kStayingSpawners, aitype, &std::pair<std::string_view, std::string_view>::first);
			if (stand == std::end(kStayingSpawners) || !stays.contains({ kAiType, HashName(stand->second) })) {
				std::fprintf(stderr, "  spawner template %u (%s): its AI type is not in zm_common and has no stand-in\n", entity.id,
					classname.c_str());
				return false;
			}
			SetEntityText(entity, "classname", std::string(kPrefix) + std::string(stand->second));
			auto& [to, n] = replaced[aitype];
			to = stand->second;
			++n;
		}
		std::string list;
		for (const auto& [from, to] : replaced) {
			list += std::format("{}{} x{} -> {}", list.empty() ? "" : ", ", from, to.second, to.first);
		}
		std::printf("  spawner templates: %zu with zm_common's AI type already%s%s\n", kept, list.empty() ? "" : "; ", list.c_str());
		return true;
	}

	// The models a map's entities draw (script_model "model" keys) once <map>'s zones no longer load. G_SetModel_cand
	// 0x7FF723DA8F70 finds a model only in a loaded bgcache's model table, and <map>'s, which listed them, is gone: the
	// map's bgcache (`listed`) lists each one a zone that loads holds (zm_common, the map's own zone, or the map's zone
	// through a copy). Each of <map>'s own that zm_common lacks (the power switch's p9_zm_ndu_power_on_switch) gets a by-name
	// reference, which AddLibraryCopies turns into a copy, with what it links.
	bool AddEntityModels(const TracedMap& traced, const std::set<AssetKey>& stays, const std::vector<MapEntity>& entities,
		std::vector<ZoneAsset>& out, std::vector<std::uint64_t>& listed) {
		constexpr std::uint64_t kXModel = 0x06;
		const std::span<const std::uint8_t> stream(traced.zone.stream);
		std::set<std::uint64_t> own;
		for (std::size_t i = 0; i < traced.list.assets.size(); ++i) {
			if (traced.list.assets[i].type != kXModel) {
				continue;
			}
			const std::size_t at = traced.trace.assets[i].offset + XAssetNameOffset(kXModel);
			const std::uint64_t name = at + 8 <= stream.size() ? Get<std::uint64_t>(stream, at) : 0;
			if (name && name != kPtrInline && !(name >> 63)) {
				own.insert(name);
			}
		}
		std::size_t added = 0, unheld = 0;
		std::string copies;
		for (const MapEntity& entity : entities) {
			if (entity.Text("classname") != "script_model") {
				continue;
			}
			for (const EntityKey& key : entity.keys) {
				if (key.key != "model" || key.type != EntityValueType::Hash || !key.hash) {
					continue;
				}
				const std::uint64_t model = key.hash & ~(1ull << 63);
				const bool ours = std::ranges::any_of(out, [&](const ZoneAsset& a) {
					return a.type == kXModel && !a.byName && a.linkName == model; });
				const bool common = stays.contains({ kXModel, model });
				const bool library = own.contains(model);
				if (!ours && !common && !library) {
					++unheld;
					continue;
				}
				if (std::ranges::find(listed, model) == listed.end()) {
					listed.push_back(model);
					++added;
				}
				if (library && !common && !ours && std::ranges::none_of(out, [&](const ZoneAsset& a) {
						return a.byName && a.type == kXModel && a.linkName == model; })) {
					out.push_back(*ByNameReference(kXModel, model));
					copies += std::format(" {:016X}", model);
				}
			}
		}
		std::printf("  entity models: %zu listed in the map's bgcache (%zu held by no zone that loads, left as they were); "
			"copied from the base map:%s\n", added, unheld, copies.empty() ? " none" : copies.c_str());
		return true;
	}

	// The zbarriers a map's entities name, once <map>'s zones no longer load. When the level starts, G_RegisterZBarriers_cand
	// 0x7FF720912790 walks the entity list: an entity whose classname is zbarrier_<name> (a window barrier's boards,
	// AddBarriers), or a content_struct with a zbarrier key (the Mystery Box, when a content flag is set), names a zbarrier
	// asset (0x53) that it looks up by that name. zbarrier has no default asset (g_defaultAssetNames[0x53] is 0), so a name
	// no loaded zone holds stops the game at launch: ERR_DROP 0xDE8F2849, "Uniform 99 Divebomb Karma" (zm_debug's fourth
	// build, 2026-10-03: its concrete barrier's zmcore_basicwallbarrier_concrete_silver, which only zm_silver.ff holds; the
	// wood barrier's is zm_common's). Each of <map>'s own that zm_common lacks gets a by-name reference, which
	// AddLibraryCopies turns into a copy with what it links (its board models and fx). A board's four animations are names
	// the game looks up when the level starts (ZBarrier_InitAnimTree_cand 0x7FF72090D3A0), not links: one that only <map>'s
	// zone holds is said, since the game plays its default animation in its place.
	// The client finds each board's models in the bgcache model table when it sets a barrier's pieces up
	// (CG_ZBarrier_SetupPieces_cand 0x7FF727CE62D0, "model %s not precached."): an unlisted one leaves model index 0, a null
	// model, and the piece's DObj null, a crash two seconds into the level (zm_debug's fifth build). <map>'s bgcache listed
	// the copied ones, so the map's lists them (`listed`): the barrier's own model (+120, G_SetModel) and each board's three.
	//
	// Where each xmodel a zbarrier links is stored: {the field's offset in the 944-B root, the stream offset of the
	// model's 232-B root}, for the barrier's own (+120) and each board's three, in that order. A field that links no
	// model is left out.
	std::vector<std::pair<std::size_t, std::size_t>> ZBarrierModelRoots(const TracedMap& traced, std::size_t index) {
		AssetRecord record;
		XStream s(traced.zone.stream, 0);
		traced.Start(s, index);
		if (!LibraryAssetReader(0x53)(s, traced.list.assets[index].header, record) || record.rootAt == kNoStream) {
			return {};
		}
		const std::span<const std::uint8_t> stream(traced.zone.stream);
		// Models stored inline are nested 232-B roots in the temp block, in the loader's field order.
		std::vector<std::size_t> inlineRoots;
		for (std::size_t i = 1; i < record.chunks.size(); ++i) {
			if (record.chunks[i].block == XBlockTemp && record.chunks[i].size == 232 && record.chunks[i].at != kNoStream) {
				inlineRoots.push_back(record.chunks[i].at);
			}
		}
		std::vector<std::size_t> fields = { 120 };
		for (std::size_t b = 0; b < 6; ++b) {
			for (const std::size_t field : { 0, 8, 16 }) {
				fields.push_back(128 + 136 * b + field);
			}
		}
		std::vector<std::pair<std::size_t, std::size_t>> roots;
		std::size_t next = 0;
		for (const std::size_t field : fields) {
			const std::uint64_t stored = Get<std::uint64_t>(stream, record.rootAt + field);
			if (stored == kPtrInline || stored == kPtrInsert) {
				if (next < inlineRoots.size()) {
					roots.emplace_back(field, inlineRoots[next++]);
				}
			}
			else if (const auto asset = AssetOfReference(traced, stored);
				asset && traced.trace.assets[*asset].offset + 8 <= stream.size()) {
				roots.emplace_back(field, traced.trace.assets[*asset].offset);
			}
		}
		return roots;
	}

	// The names of those models, each once.
	std::vector<std::uint64_t> ZBarrierModels(const TracedMap& traced, std::size_t index) {
		const std::span<const std::uint8_t> stream(traced.zone.stream);
		std::vector<std::uint64_t> names;
		for (const auto& [field, root] : ZBarrierModelRoots(traced, index)) {
			const std::uint64_t name = Get<std::uint64_t>(stream, root) & ~(1ull << 63);
			if (name && std::ranges::find(names, name) == names.end()) {
				names.push_back(name);
			}
		}
		return names;
	}

	bool AddEntityZBarriers(const TracedMap& traced, const std::string& map, const std::set<AssetKey>& stays,
		const std::vector<MapEntity>& entities, std::vector<ZoneAsset>& out, std::vector<std::uint64_t>& listed) {
		constexpr std::uint64_t kZBarrier = 0x53;
		constexpr std::uint64_t kXAnim = 0x05;
		constexpr std::string_view kPrefix = "zbarrier_";
		auto lower = [](std::string text) {
			std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		};
		// Each name an entity gives, by hash.
		std::map<std::uint64_t, std::string> named;
		for (const MapEntity& entity : entities) {
			const std::string classname = entity.Text("classname");
			if (classname.starts_with(kPrefix)) {
				const std::string name = classname.substr(kPrefix.size());
				named.emplace(HashName(name), name);
				continue;
			}
			if (classname != "script_struct" || lower(entity.Text("variantName")) != "content_struct") {
				continue;
			}
			for (const EntityKey& key : entity.keys) {
				if (lower(key.key) != "zbarrier") {
					continue;
				}
				if (key.type == EntityValueType::String && !key.text.empty()) {
					named.emplace(HashName(key.text), key.text);
				}
				else if (key.type == EntityValueType::Hash && (key.hash & ~(1ull << 63))) {
					named.emplace(key.hash & ~(1ull << 63), std::format("{:016X}", key.hash & ~(1ull << 63)));
				}
			}
		}
		if (named.empty()) {
			return true;
		}
		// <map>'s own zbarriers (their roots) and xanims (names).
		const std::span<const std::uint8_t> stream(traced.zone.stream);
		std::map<std::uint64_t, std::size_t> own;
		std::set<std::uint64_t> ownAnims;
		for (std::size_t i = 0; i < traced.list.assets.size(); ++i) {
			const std::uint64_t type = traced.list.assets[i].type;
			if (type != kZBarrier && type != kXAnim) {
				continue;
			}
			const std::size_t at = traced.trace.assets[i].offset + XAssetNameOffset(type);
			const std::uint64_t name = at + 8 <= stream.size() ? Get<std::uint64_t>(stream, at) : 0;
			if (!name || name == kPtrInline || (name >> 63)) {
				continue;
			}
			if (type == kXAnim) {
				ownAnims.insert(name);
			}
			else if (traced.trace.assets[i].offset + 944 <= stream.size()) {
				own.emplace(name, i);
			}
		}
		std::string common, copied, unheld;
		for (const auto& [hash, name] : named) {
			if (stays.contains({ kZBarrier, hash })) {
				common += " " + name;
				continue;
			}
			const auto found = own.find(hash);
			if (found == own.end()) {
				unheld += " " + name;
				continue;
			}
			if (std::ranges::none_of(out, [&](const ZoneAsset& a) { return a.type == kZBarrier && a.linkName == hash; })) {
				out.push_back(*ByNameReference(kZBarrier, hash));
			}
			std::size_t models = 0;
			for (const std::uint64_t model : ZBarrierModels(traced, found->second)) {
				if (std::ranges::find(listed, model) == listed.end()) {
					listed.push_back(model);
					++models;
				}
			}
			// Its boards' animations (u32 @76 boards in use; four xanim names at +24 .. +48 of each 136-B board at +128).
			const std::span<const std::uint8_t> root = stream.subspan(traced.trace.assets[found->second].offset, 944);
			const std::size_t boards = static_cast<std::size_t>(std::clamp(Get<std::int32_t>(root, 76), 0, 6));
			std::size_t anims = 0, inCommon = 0, onlyMap = 0;
			for (std::size_t b = 0; b < boards; ++b) {
				for (std::size_t k = 0; k < 4; ++k) {
					const std::uint64_t anim = Get<std::uint64_t>(root, 128 + 136 * b + 24 + 8 * k) & ~(1ull << 63);
					if (!anim) {
						continue;
					}
					++anims;
					inCommon += stays.contains({ kXAnim, anim });
					onlyMap += !stays.contains({ kXAnim, anim }) && ownAnims.contains(anim);
				}
			}
			copied += std::format(" {} ({} boards, {} models listed in the map's bgcache, {} animations: {} in zm_common{})", name,
				boards, models, anims, inCommon,
				onlyMap ? std::format(", {} only in {}'s zone, which the game replaces with its default animation", onlyMap, map)
					: anims > inCommon ? std::format(", {} in neither", anims - inCommon) : "");
		}
		std::printf("  zbarriers the entities name: %s%s%s%s%s%s\n", common.empty() ? "" : "zm_common's", common.c_str(),
			copied.empty() ? "" : std::format("{}copied from {}:", common.empty() ? "" : "; ", map).c_str(), copied.c_str(),
			unheld.empty() ? "" : std::format("{}in no zone cwlink reads:", common.empty() && copied.empty() ? "" : "; ").c_str(),
			unheld.c_str());
		if (!unheld.empty()) {
			// core's zones stay loaded too, and cwlink does not read them: not a failed build, but the likeliest drop at launch.
			std::printf("    a zbarrier no loaded zone holds stops the game at launch (\"Uniform 99 Divebomb Karma\"): unless core's "
				"zones hold%s, take out what names it\n", unheld.c_str());
		}
		return true;
	}

	// The level's lists. The entity list keeps, of <map>'s: worldspawn, the AI spawner templates (actor_spawner_*),
	// the initial spawns (moved to the source's player spawns before this), the Zombies objects
	// PlaceGameplayObjects placed or parked (`kept`) with their content parents, and everything cwlink added
	// (device copies, the brush model, draw-test rows: ids above <map>'s). It adds per zone a player_respawn_point, per
	// zombie spawner a riser
	// struct, and the two minimap corners. The trigger list holds one info_volume per zone. Every other entity of
	// <map>'s is left out: its level script no longer runs, so nothing looks them up.
	template <typename Place>
	bool ComposeLevelEntities(const MapKit::MkMap& source, const Place& place, const std::vector<LevelZone>& zones,
		const std::vector<LevelBarrier>& barriers, const std::vector<MapEntity>& baseEntities, const std::vector<MapEntity>& baseTriggers,
		const std::unordered_set<std::uint32_t>& kept, std::uint32_t baseLastId, std::vector<MapEntity>& entities,
		std::vector<MapEntity>& triggers, TriggerShapes& shapes) {
		const MapEntity* respawnTemplate = Template(baseEntities, "player_respawn_point struct", [](const MapEntity& e) {
			return e.Text("targetname") == "player_respawn_point" && e.Text("target") == "initial_spawn_points";
		});
		const MapEntity* spawnTemplate = Template(baseEntities, "riser_location struct", [](const MapEntity& e) {
			return e.Text("classname") == "script_struct" && e.Text("script_noteworthy") == "riser_location";
		});
		const MapEntity* cornerTemplate = Template(baseEntities, "minimap_corner", [](const MapEntity& e) {
			return e.Text("targetname") == "minimap_corner";
		});
		const MapEntity* volumeTemplate = Template(baseTriggers, "player_volume info_volume", [](const MapEntity& e) {
			return e.Text("classname") == "info_volume" && e.Text("script_noteworthy") == "player_volume";
		});
		if (!respawnTemplate || !spawnTemplate || !cornerTemplate || !volumeTemplate) {
			return false;
		}
		// Copies: the templates point into the list about to be cut.
		const MapEntity respawn = *respawnTemplate, spawn = *spawnTemplate, corner = *cornerTemplate, volume = *volumeTemplate;

		// A kept object's content parents stay too. The box, wall buys and Arsenal are content structs that
		// content_manager.gsc reaches only from the top: content_destination <- content_location <- content_instance
		// <- content_struct, each linked by its `target` = the parent's targetname. A struct without its parents
		// spawns nothing. Every content_destination stays as well (zm_silver has two): content_manager.gsc looks
		// destinations up by name from the map's fields.
		std::unordered_set<std::uint32_t> keep = kept;
		for (const MapEntity& entity : entities) {
			if (entity.Text("variantName") == "content_destination") {
				keep.insert(entity.id);
			}
		}
		std::unordered_map<std::string, std::vector<const MapEntity*>> named;
		for (const MapEntity& entity : entities) {
			if (const std::string name = entity.Text("targetname"); !name.empty()) {
				named[name].push_back(&entity);
			}
		}
		std::vector<const MapEntity*> walk;
		for (const MapEntity& entity : entities) {
			if (kept.contains(entity.id) || entity.id > baseLastId) {
				walk.push_back(&entity);
			}
		}
		while (!walk.empty()) {
			const MapEntity* child = walk.back();
			walk.pop_back();
			const auto it = named.find(child->Text("target"));
			if (it == named.end()) {
				continue;
			}
			for (const MapEntity* parent : it->second) {
				if (parent->Text("variantName").starts_with("content_") && keep.insert(parent->id).second) {
					walk.push_back(parent);
				}
			}
		}
		const std::size_t parents = keep.size() - kept.size();

		// Every vehicle node stays too: the support items fly <map>'s paths (the chopper gunner's loop, the napalm
		// strike's runs), and the exfil flies the ones PlaceGameplayObjects moved.
		const std::size_t before = entities.size();
		std::erase_if(entities, [&](const MapEntity& entity) {
			const std::string classname = entity.Text("classname");
			return !(classname == "worldspawn" || classname.starts_with("actor_spawner") || classname.starts_with("info_vehicle_node")
				|| entity.id > baseLastId || keep.contains(entity.id) || entity.Text("targetname") == "initial_spawn_points");
		});
		if (entities.empty() || entities[0].Text("classname") != "worldspawn") {
			std::fprintf(stderr, "the base map's entity list does not start with worldspawn\n");
			return false;
		}
		std::size_t spawners = 0, vehicleNodes = 0;
		for (const MapEntity& entity : entities) {
			spawners += entity.Text("classname").starts_with("actor_spawner");
			vehicleNodes += entity.Text("classname").starts_with("info_vehicle_node");
		}
		std::printf("  entity list: %zu of the base map's entities kept (worldspawn, %zu AI spawner templates, %zu vehicle "
			"nodes, spawns, the objects placed, %zu content parents), %zu left out\n", entities.size(), spawners, vehicleNodes,
			parents, before - entities.size());

		std::uint32_t id = NextEntityId(entities);
		std::array<float, 3> lo = { FLT_MAX, FLT_MAX, FLT_MAX }, hi = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const LevelZone& zone : zones) {
			entities.push_back(EntityFrom(respawn, id++, { zone.center[0], zone.center[1], zone.mins[2] }, 0.0f,
				{ "client_server", "radius", "script_int" },
				{ { "targetname", "player_respawn_point" }, { "target", "initial_spawn_points" }, { "script_noteworthy", zone.name } }));

			MapEntity info = EntityFrom(volume, id++, zone.center, 0.0f, { "client_server" },
				{ { "targetname", zone.name }, { "script_noteworthy", "player_volume" }, { "target", zone.name + "_spawns" } });
			SetEntityAngles(info, { 0.0f, 0.0f, 0.0f });
			AddTriggerBox(shapes, kTriggerContentsVolume, zone.halfSize, zone.yaw);
			triggers.push_back(std::move(info));
			for (int k = 0; k < 3; ++k) {
				lo[k] = std::min(lo[k], zone.mins[k]);
				hi[k] = std::max(hi[k], zone.maxs[k]);
			}
		}

		std::size_t risers = 0, toBarriers = 0;
		for (const MapKit::MkEntity& spawner : source.entities) {
			if (spawner.cls != "zombie_spawner") {
				continue;
			}
			const std::string zone = spawner.Prop("zone");
			if (std::ranges::find(zones, zone, &LevelZone::name) == zones.end()) {
				std::printf("  zombie spawner %s: zone '%s' does not exist, skipped\n", spawner.path.c_str(), zone.c_str());
				continue;
			}
			// find_flesh: the zombie hunts players at once instead of looking for a barrier to tear down
			// (zm_spawner should_skip_teardown), as on all of Die Maschine's ground spawns. A barrier spawner's struct
			// names its barrier instead: zm_behavior findnodesservice sends the zombie to the exterior_goal of that
			// script_string, and it climbs in through the barrier_align of it once the boards are down.
			std::string goal = "find_flesh";
			if (spawner.Prop("kind") == "barrier") {
				if (const LevelBarrier* barrier = SpawnerBarrier(barriers, zone, place(spawner.origin))) {
					goal = barrier->name;
					++toBarriers;
					std::printf("  zombie spawner %s: its zombies head for barrier %s\n", spawner.path.c_str(), barrier->path.c_str());
				}
				else {
					std::printf("  zombie spawner %s: zone '%s' has no barrier, so its zombies hunt the players straight away\n",
						spawner.path.c_str(), zone.c_str());
				}
			}
			entities.push_back(EntityFrom(spawn, id++, place(spawner.origin), spawner.angles[1], { "client_server" },
				{ { "targetname", zone + "_spawns" }, { "script_noteworthy", "riser_location" }, { "script_string", goal } }));
			++risers;
		}

		// The minimap's corners (compass::setupminimap): the zones' bounds, a little wider.
		for (const std::array<float, 3>& at : { std::array<float, 3>{ hi[0] + 256, lo[1] - 256, 0.0f },
				std::array<float, 3>{ lo[0] - 256, hi[1] + 256, 0.0f } }) {
			entities.push_back(EntityFrom(corner, id++, at, corner.angles[1], { "client_server", "targetname" }, {}));
		}
		std::printf("  entity list: + %zu player respawn points, %zu zombie spawn structs (%zu to a barrier), 2 minimap corners = "
			"%zu entities\n", zones.size(), risers, toBarriers, entities.size());
		std::printf("  trigger list: %zu zone volumes (the base map's %zu triggers left out)\n", triggers.size(), baseTriggers.size());
		if (!risers) {
			std::printf("  (no zombie spawner: no zombie will spawn)\n");
		}
		return true;
	}

	// A door's look when the source names none: the rusted metal door zm_silver slides open between its upstairs
	// rooms (entity 41696). It has its own collision.
	constexpr const char* kDefaultDoorModel = "p9_zm_ndu_door_metal_gray_rusted";
	// The contents of zm_silver's use triggers (trigger_use_touch, trigger_use), as their trigger models store them.
	constexpr std::uint32_t kTriggerContentsUse = 0x82000000;
	// How far a door's buy trigger reaches past its box: in front and behind, and at the sides.
	constexpr float kDoorTriggerReach = 40.0f;
	constexpr float kDoorTriggerSide = 8.0f;

	// A model the source names: 16 hex digits after '#' (or "0x"), else a game name.
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

	// The xmodels <map>'s zone holds or links by name: the ones sure to be loaded with it.
	std::unordered_set<std::uint64_t> BaseModels(const TracedMap& traced) {
		std::unordered_set<std::uint64_t> models;
		const std::span<const std::uint8_t> stream(traced.zone.stream);
		for (std::size_t i = 0; i < traced.list.assets.size(); ++i) {
			if (traced.list.assets[i].type == 0x06 && traced.trace.assets[i].offset + 8 <= stream.size()) {
				models.insert(Get<std::uint64_t>(stream.subspan(traced.trace.assets[i].offset, 8), 0) & ~(1ull << 63));
			}
		}
		return models;
	}

	// Doors (zm_blockers.gsc door_init, door_think, door_activate). Each source door becomes two entities:
	// - a trigger_use_touch "zombie_door" in the trigger list, a copy of one of <map>'s (zm_silver entity 45392),
	//   over the door's box and kDoorTriggerReach past it on each side of its thin axis: the buy prompt. Its
	//   script_flag is the door's flag (the zones script connects the door's zones on it), zombie_cost its cost,
	//   and its target names the piece;
	// - the piece: a script_model of the door's model at the node, a copy of <map>'s door piece (zm_silver
	//   entity 41693) with script_noteworthy "model_clip" (the model's own collision blocks, as on zm_silver's
	//   doors) and script_string "move" with script_vector straight down by the box's height, so a bought door
	//   sinks into the floor.
	// The pieces' models go into `models`, for the level's bgcache (BG_Cache_Register_cand skips a name that is
	// already listed, so a model zm_silver lists is harmless).
	//
	// A door with needs_power opens by itself when the power comes on instead: its trigger gets script_noteworthy
	// "electric_door" (zm_blockers door_init: no price, waits for the door's power_on notify, which
	// zm_power::turn_power_on_and_open_doors sends; zm_silver entity 45397). Without a power switch it is bought as usual.
	bool AddDoors(const std::vector<LevelDoor>& doors, bool powerSwitch, const TracedMap& traced,
		const std::vector<MapEntity>& baseEntities, const std::vector<MapEntity>& baseTriggers, std::vector<MapEntity>& entities,
		std::vector<MapEntity>& triggers, TriggerShapes& shapes, std::vector<std::uint64_t>& models) {
		if (doors.empty()) {
			return true;
		}
		const MapEntity* triggerTemplate = Template(baseTriggers, "zombie_door trigger_use_touch", [](const MapEntity& e) {
			return e.Text("classname") == "trigger_use_touch" && e.Text("targetname") == "zombie_door" && !e.Find("script_noteworthy");
		});
		const MapEntity* pieceTemplate = Template(baseEntities, "door piece script_model", [](const MapEntity& e) {
			return e.Text("classname") == "script_model" && e.Text("script_noteworthy") == "model_clip"
				&& e.Text("script_string") == "slide_apart" && e.Find("model");
		});
		if (!triggerTemplate || !pieceTemplate) {
			return false;
		}
		// Copies: the templates point into the lists being added to.
		const MapEntity trigger = *triggerTemplate, piece = *pieceTemplate;
		const std::unordered_set<std::uint64_t> loaded = BaseModels(traced);
		std::uint32_t id = std::max(NextEntityId(entities), NextEntityId(triggers));
		for (const LevelDoor& door : doors) {
			const std::string modelName = door.model.empty() ? kDefaultDoorModel : door.model;
			const std::uint64_t model = ModelKey(modelName);
			std::array<float, 3> reach = door.halfSize;
			const std::size_t thin = door.halfSize[0] <= door.halfSize[1] ? 0 : 1;
			reach[thin] += kDoorTriggerReach;
			reach[1 - thin] += kDoorTriggerSide;
			const bool electric = door.power && powerSwitch;
			MapEntity use = EntityFrom(trigger, id++, door.center, 0.0f,
				{ "cursorhint", "TEAM_ALLIES", "TEAM_AXIS", "TEAM_NEUTRAL", "TEAM_THREE" },
				{ { "targetname", "zombie_door" }, { "target", door.flag }, { "script_flag", door.flag },
					{ "zombie_cost", door.cost.empty() ? "1000" : door.cost } });
			if (electric) {
				SetOrAddText(use, "script_noteworthy", "electric_door");
			}
			else if (door.power) {
				std::printf("  door %s: needs_power, but the map has no power switch: it is bought as usual\n", door.path.c_str());
			}
			SetEntityAngles(use, { 0.0f, 0.0f, 0.0f });
			AddTriggerBox(shapes, kTriggerContentsUse, reach, door.yaw);
			triggers.push_back(std::move(use));

			const float drop = 2 * door.halfSize[2] + 8;
			MapEntity part = EntityFrom(piece, id++, door.origin, door.yaw, { "client_server", "model", "script_sound" },
				{ { "targetname", door.flag }, { "script_noteworthy", "model_clip" }, { "script_string", "move" },
					{ "script_vector", std::format("0 0 {}", -drop) } });
			for (EntityKey& key : part.keys) {
				if (key.key == "model") {
					key.hash = model;
					PutAt(std::span<std::uint8_t>(key.raw), 8, model);
				}
			}
			entities.push_back(std::move(part));
			if (std::ranges::find(models, model) == models.end()) {
				models.push_back(model);
			}
			std::printf("  door %s: %s %s, opens %zu zone(s) on %s; sinks %.0f into the floor%s\n", door.path.c_str(),
				modelName.c_str(), electric ? "opens when the power comes on" : ("for " + (door.cost.empty() ? std::string("1000") : door.cost)).c_str(),
				door.opens.size(), door.flag.c_str(), drop,
				loaded.contains(model) ? "" : " (the model is not in the base map's zone: it shows only if zm_common or core loads it)");
		}
		return true;
	}

	// Moves an entity rigidly with a frame: from one origin and yaw to another (its offset turned, its yaw added to).
	void MoveWithFrame(MapEntity& entity, const std::array<float, 3>& fromOrigin, float fromYaw, const std::array<float, 3>& toOrigin,
		float toYaw) {
		const float turn = (toYaw - fromYaw) * 3.14159265358979f / 180.0f;
		const float c = std::cos(turn), s = std::sin(turn);
		const float dx = entity.origin[0] - fromOrigin[0], dy = entity.origin[1] - fromOrigin[1];
		SetEntityOrigin(entity, { toOrigin[0] + c * dx - s * dy, toOrigin[1] + s * dx + c * dy,
			toOrigin[2] + entity.origin[2] - fromOrigin[2] }, std::nullopt);
		SetEntityAngles(entity, { entity.angles[0], NormalizeYaw(entity.angles[1] + toYaw - fromYaw), entity.angles[2] });
	}

	// The zbarrier class of a barrier kind (MkBarrier `kind`).
	constexpr std::string_view kZBarrierClassPrefix = "zbarrier_";
	std::string BarrierClassname(const std::string& kind) {
		return std::string(kZBarrierClassPrefix)
			+ (kind == "concrete" ? "zmcore_basicwallbarrier_concrete_silver" : "zmcore_t8_basicwoodbarrier");
	}

	// The first of <map>'s barriers of a class that has an exterior goal and a rebuild point: what each of the source's
	// barriers of that kind is a copy of. `parts` gets the structs around it. Null when <map> has none.
	const MapEntity* BarrierGroup(const std::vector<MapEntity>& baseEntities, const std::string& classname,
		std::vector<const MapEntity*>* parts = nullptr) {
		for (const MapEntity& candidate : baseEntities) {
			if (candidate.Text("classname") != classname) {
				continue;
			}
			const std::string t = candidate.Text("targetname"), s = candidate.Text("script_string");
			std::vector<const MapEntity*> found;
			bool goal = false, rebuild = false;
			for (const MapEntity& entity : baseEntities) {
				if (&entity == &candidate || entity.Text("classname") != "script_struct") {
					continue;
				}
				const bool isGoal = entity.Text("targetname") == "exterior_goal" && entity.Text("target") == t;
				const bool isAlign = entity.Text("targetname") == "barrier_align" && entity.Text("script_string") == s;
				if (isGoal || isAlign || entity.Text("targetname") == t) {
					found.push_back(&entity);
					goal |= isGoal;
					rebuild |= entity.Text("script_noteworthy") == "trigger_location";
				}
			}
			if (!t.empty() && !s.empty() && goal && rebuild) {
				if (parts) {
					*parts = std::move(found);
				}
				return &candidate;
			}
		}
		return nullptr;
	}

	// The yaw of a barrier's zbarrier, from its node's facing: the node's arrow points into the play area, the
	// zbarrier's +X out of it.
	constexpr float kBarrierYaw = 180.0f;

	// Window barriers (MkBarrier; zm_blockers.gsc blocker_init, zm_behavior.gsc findnodesservice and the barricade
	// mocomps). zm_silver builds each of its 25 from one group, the same on all of them (2026-09-28): a zbarrier entity
	// (zbarrier_zmcore_t8_basicwoodbarrier, or _basicwallbarrier_concrete_silver: the boards, their tear and repair
	// animations; its +X points OUT of the play area) with targetname T and script_string S, and around it
	// - exterior_goal (target T, script_string S): 38 units out, 16 up, facing in. Where the zombie goes (the nearest
	//   navmesh point within 128) and tears the boards down from;
	// - attack_spots (targetname T): 38 out, 31 up; trigger_location (targetname T, radius 36, height 64): 9 in, 23 up,
	//   where players rebuild;
	// - barrier_align (script_string S), 1 unit in: what the zombie's climb-in animation lines up on, in noclip, so it
	//   gets through whatever the wall is (the concrete barrier has none; the zbarrier itself is used then);
	// - two path nodes, left out (they bind to compiled nodes; blocker_init only looks for negotiation nodes).
	// A zombie spawn struct whose script_string is S sends its zombies to that barrier (ComposeLevelEntities). Each source
	// barrier gets a copy of one such group, moved and turned as one so the zbarrier stands at the node facing away from
	// the node's arrow (the arrow points into the play area), T and S both = its name. The zbarrier asset, the board
	// models and animations come with zm_silver; without its zones, the wood barrier's zbarrier is zm_common's and the
	// concrete one's is copied (AddEntityZBarriers).
	bool AddBarriers(const std::vector<LevelBarrier>& barriers, const std::vector<MapEntity>& baseEntities,
		std::vector<MapEntity>& entities, std::vector<MapEntity>& triggers) {
		if (barriers.empty()) {
			return true;
		}
		std::uint32_t id = std::max(NextEntityId(entities), NextEntityId(triggers));
		for (const LevelBarrier& barrier : barriers) {
			const std::string classname = BarrierClassname(barrier.kind);
			std::vector<const MapEntity*> parts;
			const MapEntity* zbarrier = BarrierGroup(baseEntities, classname, &parts);
			if (!zbarrier) {
				std::printf("  barrier %s: the base map has no %s barrier to copy, skipped\n", barrier.path.c_str(), barrier.kind.c_str());
				continue;
			}
			const std::string t = zbarrier->Text("targetname"), s = zbarrier->Text("script_string");
			const float yaw = NormalizeYaw(barrier.yaw + kBarrierYaw);
			std::string what;
			for (const MapEntity* part : parts) {
				MapEntity copy = *part;
				copy.id = id++;
				MoveWithFrame(copy, zbarrier->origin, zbarrier->angles[1], barrier.origin, yaw);
				for (const char* key : { "targetname", "target" }) {
					if (copy.Text(key) == t) {
						SetEntityText(copy, key, barrier.name);
					}
				}
				if (copy.Text("script_string") == s) {
					SetEntityText(copy, "script_string", barrier.name);
				}
				what += " " + (copy.Text("script_noteworthy").empty() ? copy.Text("targetname") : copy.Text("script_noteworthy"));
				entities.push_back(std::move(copy));
			}
			MapEntity board = *zbarrier;
			board.id = id++;
			MoveWithFrame(board, zbarrier->origin, zbarrier->angles[1], barrier.origin, yaw);
			SetEntityText(board, "targetname", barrier.name);
			SetEntityText(board, "script_string", barrier.name);
			entities.push_back(std::move(board));
			std::printf("  barrier %s (%s, zone %s): %s's %s moved there with its%s\n", barrier.path.c_str(), barrier.kind.c_str(),
				barrier.zone.c_str(), classname.c_str(), s.c_str(), what.c_str());
		}
		return true;
	}

	// The power switch (MkPowerSwitch; zm_power.gsc electric_switch). zm_silver's (2026-09-28): a trigger_use
	// "use_elec_switch" (trigger list 45416) whose target names a struct with script_noteworthy "elec_switch_fx" (where the
	// sparks go) and, when an entity of that targetname has script_noteworthy "elec_switch", the lever, which the script
	// turns 90 degrees about its roll. zm_silver's console (script_model 41708, script_noteworthy elec_switch, front +X)
	// has no targetname, so nothing turns; mapkit places it the same way. Using the trigger turns the power on:
	// zm_power::turn_power_on_and_open_doors sets the "power_on" flag, powers the perk machines and opens the doors whose
	// trigger has script_noteworthy "electric_door". The trigger here is a box in front of the console (the node's +X):
	// kPowerSwitchReach deep, as wide as a person, from the floor up.
	constexpr std::array<float, 3> kPowerSwitchReach = { 24.0f, 32.0f, 40.0f }; // half sizes: forward, side, up
	constexpr const char* kPowerSwitchTarget = "mapkit_power_switch";

	// zm_silver's power switch console, the script_model the power switch copies. Null when it has none.
	const MapEntity* PowerSwitchConsole(const std::vector<MapEntity>& baseEntities) {
		const auto it = std::ranges::find_if(baseEntities, [](const MapEntity& e) {
			return e.Text("classname") == "script_model" && e.Text("script_noteworthy") == "elec_switch" && e.Find("model");
		});
		return it == baseEntities.end() ? nullptr : &*it;
	}

	template <typename Place>
	bool AddPowerSwitch(const MapKit::MkMap& source, const Place& place, const std::vector<MapEntity>& baseEntities,
		const std::vector<MapEntity>& baseTriggers, std::vector<MapEntity>& entities, std::vector<MapEntity>& triggers,
		TriggerShapes& shapes, std::vector<std::uint64_t>& models) {
		std::vector<const MapKit::MkEntity*> switches;
		for (const MapKit::MkEntity& entity : source.entities) {
			if (entity.cls == "power_switch") {
				switches.push_back(&entity);
			}
		}
		if (switches.empty()) {
			return true;
		}
		const MapEntity* console = PowerSwitchConsole(baseEntities);
		const MapEntity* trigger = Template(baseTriggers, "use_elec_switch trigger_use", [](const MapEntity& e) {
			return e.Text("classname") == "trigger_use" && e.Text("targetname") == "use_elec_switch";
		});
		const MapEntity* sparks = Template(baseEntities, "elec_switch_fx struct", [](const MapEntity& e) {
			return e.Text("classname") == "script_struct" && e.Text("script_noteworthy") == "elec_switch_fx";
		});
		if (!console || !trigger || !sparks) {
			if (!console) {
				std::fprintf(stderr, "no elec_switch script_model in the base map's entities to copy\n");
			}
			return false;
		}
		// Copies: the templates point into lists being added to.
		const MapEntity consoleTemplate = *console, triggerTemplate = *trigger, sparksTemplate = *sparks;
		std::uint32_t id = std::max(NextEntityId(entities), NextEntityId(triggers));
		const bool powerDoors = std::ranges::any_of(source.entities, [](const MapKit::MkEntity& e) {
			return e.cls == "door" && e.Prop("needs_power") == "true";
		});
		for (std::size_t n = 0; n < switches.size(); ++n) {
			const MapKit::MkEntity* at = switches[n];
			const std::string target = std::format("{}_{}", kPowerSwitchTarget, n);
			const std::array<float, 3> origin = place(at->origin);
			const float yaw = NormalizeYaw(at->angles[1]);
			MapEntity model = consoleTemplate;
			model.id = id++;
			SetEntityOrigin(model, origin, std::nullopt);
			SetEntityAngles(model, { 0.0f, yaw, 0.0f });
			if (const EntityKey* key = model.Find("model")) {
				const std::uint64_t name = key->hash & ~(1ull << 63);
				if (std::ranges::find(models, name) == models.end()) {
					models.push_back(name);
				}
			}
			entities.push_back(std::move(model));

			MapEntity fx = sparksTemplate;
			fx.id = id++;
			MoveWithFrame(fx, consoleTemplate.origin, consoleTemplate.angles[1], origin, yaw);
			SetEntityText(fx, "targetname", target);
			entities.push_back(std::move(fx));

			const float radians = yaw * 3.14159265358979f / 180.0f;
			const std::array<float, 3> center = { origin[0] + std::cos(radians) * kPowerSwitchReach[0],
				origin[1] + std::sin(radians) * kPowerSwitchReach[0], origin[2] + kPowerSwitchReach[2] };
			MapEntity use = EntityFrom(triggerTemplate, id++, center, 0.0f,
				{ "cursorhint", "TEAM_ALLIES", "TEAM_AXIS", "TEAM_NEUTRAL", "TEAM_THREE" },
				{ { "targetname", "use_elec_switch" }, { "target", target } });
			SetEntityAngles(use, { 0.0f, 0.0f, 0.0f });
			AddTriggerBox(shapes, kTriggerContentsUse, kPowerSwitchReach, yaw);
			triggers.push_back(std::move(use));
			std::printf("  power switch %s: zm_silver's console, its trigger and sparks moved there\n", at->path.c_str());
		}
		const bool powerStations = std::ranges::any_of(source.entities, [](const MapKit::MkEntity& e) {
			return (e.cls == "armor_station" || e.cls == "arsenal") && e.Prop("needs_power") != "false";
		});
		std::vector<std::string> waiting = { "the perk machines" };
		if (powerDoors) {
			waiting.push_back("the power doors");
		}
		if (powerStations) {
			waiting.push_back("the Armor Stations");
		}
		std::string list;
		for (std::size_t i = 0; i < waiting.size(); ++i) {
			list += (i == 0 ? "" : i + 1 == waiting.size() ? " and " : ", ") + waiting[i];
		}
		std::printf("  power: %s wait for %s\n", list.c_str(), switches.size() > 1 ? "any of the switches" : "the switch");
		return true;
	}

	// Ambient rooms (MkAmbientRoom): how an area sounds (its reverb and room tone). The client spawns every trigger_multiple
	// of the trigger list whose targetname is "ambient_package" as an ambient room (CG_SpawnClientTrigger 0x7FF727EF58D0,
	// IDB 2026-10-03), and only one whose spawnflags have 0x880 set. Its String key script_ambientroom names a room of the
	// level's sound bank (the engine keeps the FNV-1a hash of the name, lowercased), and script_ambientpriority (read as an
	// int: a String is atoi'd) decides between overlapping ones. Each frame the listener takes the room of the trigger it
	// stands in, and room 0 in none (CG_UpdateAmbientRooms_cand 0x7FF724D89090): the bank's default room. That was all a
	// mapkit map played before these: its trigger list holds none of <map>'s rooms. Each trigger here is a copy of one of
	// <map>'s ambient_package triggers, cut to its classname and spawnflags (and client_server), with a box shape. The rooms
	// <map>'s own triggers name are printed: those are rooms its sound bank has.
	// <map>'s reverb also comes from Triton acoustics baked from <map>'s own geometry (SND_Triton_LoadBankAcoustics
	// 0x7FF729992B50), which rooms do not replace.
	// The room names are <map>'s sound bank's, which is in <map>.ff: it loads only with `library` (--with-library or an
	// overlay). Without it the build says so: a map built without <map>'s zones has none of <map>'s rooms until its bank
	// is copied.
	constexpr const char* kAmbientRoomTargetname = "ambient_package";
	// The contents of zm_silver's trigger_multiple trigger models (map_entities.hpp).
	constexpr std::uint32_t kTriggerContentsMultiple = 0x82000000;

	struct LevelRoom {
		std::string path;
		std::string room;
		int priority = 1;
		std::array<float, 3> center{};   // world
		std::array<float, 3> halfSize{}; // along the room's own axes
		float yaw = 0;
	};

	// The rooms of <map>'s sound bank (IDB 2026-10-03): the bank's +80 table, 136-B records x u32 @72. A room's name hash
	// (HashName) is at +0, the bool defaultRoom at +8, OverrideTriton at +9, the reverb, nearVerb and farVerb at +16 .. +32
	// (hashes), the dry and wet levels, room occlusion and verb attenuation at +40 .. +52 (floats), the loop at +56 (the room
	// tone it plays), the duck at +64, three entity contexts at +72 .. +112 (ringoff_plr indoor or outdoor: the weapons'
	// tails) and a global context at +120 (field names: the schema at 0x7FF72A3C47E0). The engine looks a room up by its
	// hash in every loaded bank (SND_FindRoom 0x7FF729221920). A trigger's room it does not find, and no room at all, play
	// the default room: the first with defaultRoom set (SND_FindDefaultRoom 0x7FF729220F40, SND_ApplyListenerRoom
	// 0x7FF729538990).
	struct BankRooms {
		std::set<std::uint64_t> rooms;
		std::uint64_t defaultRoom = 0;
	};

	std::optional<BankRooms> ReadBankRooms(const TracedMap& traced) {
		const std::span<const std::uint8_t> stream(traced.zone.stream);
		for (std::size_t index = 0; index < traced.list.assets.size(); ++index) {
			const XAssetEntry& entry = traced.list.assets[index];
			if (entry.type != 0x12 || (entry.header != kPtrInline && entry.header != kPtrInsert)) {
				continue;
			}
			XStream s(traced.zone.stream, 0);
			traced.Start(s, index);
			AssetRecord record;
			const RecordSubtree* table = nullptr;
			if (!LibraryAssetReader(0x12)(s, entry.header, record) || !(table = record.Subtree("sound bank +80"))
				|| table->firstChunk >= table->endChunk) {
				continue;
			}
			const RecordChunk& chunk = record.chunks[table->firstChunk];
			BankRooms out;
			for (std::size_t at = 0; chunk.at != kNoStream && at + 136 <= chunk.size; at += 136) {
				const auto room = stream.subspan(chunk.at + at, 136);
				const std::uint64_t name = Get<std::uint64_t>(room, 0) & 0x7FFFFFFFFFFFFFFFull;
				out.rooms.insert(name);
				if (room[8] && !out.defaultRoom) {
					out.defaultRoom = name;
				}
			}
			return out;
		}
		return std::nullopt;
	}

	template <typename Place>
	bool AddAmbientRooms(const MapKit::MkMap& source, const Place& place, const std::string& map, bool library,
		const TracedMap& traced, const std::vector<MapEntity>& baseTriggers, std::vector<MapEntity>& entities,
		std::vector<MapEntity>& triggers, TriggerShapes& shapes) {
		std::map<std::string, std::size_t> baseRooms;
		for (const MapEntity& trigger : baseTriggers) {
			if (trigger.Text("targetname") == kAmbientRoomTargetname && !trigger.Text("script_ambientroom").empty()) {
				++baseRooms[trigger.Text("script_ambientroom")];
			}
		}
		std::string known;
		for (const auto& [room, count] : baseRooms) {
			known += std::format(" {} (x{})", room, count);
		}
		if (known.empty()) {
			known = " none";
		}

		std::vector<LevelRoom> rooms;
		for (const MapKit::MkEntity& entity : source.entities) {
			if (entity.cls != "ambient_room") {
				continue;
			}
			LevelRoom room;
			room.path = entity.path;
			room.room = entity.Prop("room");
			if (room.room.empty() || room.room.find_first_of(" \"\\") != std::string::npos) {
				std::fprintf(stderr, "ambient room %s: room '%s' is empty or has a space, quote or backslash (%s's rooms:%s)\n",
					entity.path.c_str(), room.room.c_str(), map.c_str(), known.c_str());
				return false;
			}
			const std::string priority = entity.Prop("priority");
			if (!priority.empty()) {
				room.priority = std::atoi(priority.c_str());
			}
			room.yaw = entity.angles[1];
			std::array<float, 3> mins{}, maxs{};
			VolumeBox(entity, place, room.center, room.halfSize, mins, maxs);
			rooms.push_back(std::move(room));
		}
		if (rooms.empty()) {
			std::printf("  ambient rooms: none placed, so the whole map sounds like %s's default room (%s's rooms:%s)\n",
				map.c_str(), map.c_str(), known.c_str());
			return true;
		}
		const std::optional<BankRooms> bank = ReadBankRooms(traced);
		const MapEntity* templ = Template(baseTriggers, "ambient_package trigger_multiple", [](const MapEntity& e) {
			return e.Text("classname") == "trigger_multiple" && e.Text("targetname") == kAmbientRoomTargetname;
		});
		if (!templ) {
			return false;
		}
		// A copy: the template points into a list being added to.
		const MapEntity trigger = *templ;
		std::uint32_t id = std::max(NextEntityId(entities), NextEntityId(triggers));
		for (const LevelRoom& room : rooms) {
			MapEntity ambient = EntityFrom(trigger, id++, room.center, 0.0f, { "spawnflags", "client_server", "script_ambientpriority" },
				{ { "targetname", kAmbientRoomTargetname }, { "script_ambientroom", room.room },
					{ "script_ambientpriority", std::to_string(room.priority) } });
			// The template may store the priority as a number rather than text.
			for (EntityKey& key : ambient.keys) {
				if (key.key == "script_ambientpriority" && key.type == EntityValueType::Int) {
					key.integer = room.priority;
				}
				else if (key.key == "script_ambientpriority" && key.type == EntityValueType::Float) {
					key.number = static_cast<float>(room.priority);
				}
			}
			SetEntityAngles(ambient, { 0.0f, 0.0f, 0.0f });
			AddTriggerBox(shapes, kTriggerContentsMultiple, room.halfSize, room.yaw);
			triggers.push_back(std::move(ambient));
			std::string note;
			if (bank && !bank->rooms.contains(HashName(room.room))) {
				note = std::format(" -- NOT a room of {}'s sound bank: unless another loaded bank has it, it plays the default room "
					"like the rest of the map. Use one of {}'s rooms (below)", map, map);
			}
			else if (!bank && !baseRooms.contains(room.room)) {
				note = std::format(" (not one of {}'s rooms: it works only if {}'s sound bank has it)", map, map);
			}
			std::printf("  ambient room %s: '%s', priority %d%s\n", room.path.c_str(), room.room.c_str(), room.priority, note.c_str());
		}
		std::printf("  ambient rooms: %zu placed; elsewhere the map sounds like %s's default room (%s's rooms:%s)\n", rooms.size(),
			map.c_str(), map.c_str(), known.c_str());
		if (bank) {
			std::string defaultRoom = bank->defaultRoom ? std::format("{:016X}", bank->defaultRoom) : std::string("none");
			for (const auto& [name, count] : baseRooms) {
				if (HashName(name) == bank->defaultRoom) {
					defaultRoom = name;
				}
			}
			std::printf("  ambient rooms: %s's sound bank has %zu rooms, the default one %s; its triggers name the %zu above, the "
				"others have no name to go by\n", map.c_str(), bank->rooms.size(), defaultRoom.c_str(), baseRooms.size());
		}
		if (!library) {
			std::printf("  ambient rooms: they are rooms of %s's sound bank, which is in %s.ff: the map carries a copy of it (\"sound "
				"bank\" below)\n", map.c_str(), map.c_str());
		}
		return true;
	}

	// Props (MkProp, P2 first step): a script_model each, of the source's model at the node, turned with all three
	// angles and scaled by modelscale. Each is a copy of one of <map>'s script_models: ServerSide, a float modelscale
	// and spawnflags 1 with DYNAMICPATH 1, as on zm_silver's debris pieces (39 of its 140 script_models, all of them
	// debris). A solid prop blocks with its model's own collision, as the door pieces do, and keeps DYNAMICPATH, so
	// zombies should path round it (a guess from the debris, not checked). A prop that is not solid drops both and
	// gets targetname kNonSolidProps. The models go into `models`, for the level's bgcache.
	template <typename Place>
	bool AddProps(const MapKit::MkMap& source, const Place& place, const TracedMap& traced,
		const std::vector<MapEntity>& baseEntities, const std::vector<MapEntity>& triggers, std::vector<MapEntity>& entities,
		std::vector<std::uint64_t>& models) {
		std::vector<const MapKit::MkEntity*> props;
		for (const MapKit::MkEntity& entity : source.entities) {
			if (entity.cls == "prop") {
				props.push_back(&entity);
			}
		}
		if (props.empty()) {
			return true;
		}
		const MapEntity* propTemplate = Template(baseEntities, "script_model with modelscale and DYNAMICPATH", [](const MapEntity& e) {
			const EntityKey* scale = e.Find("modelscale");
			return e.Text("classname") == "script_model" && e.Find("model") && scale && scale->type == EntityValueType::Float
				&& e.Text("client_server") == "ServerSide" && e.Text("spawnflags") == "1" && e.Text("DYNAMICPATH") == "1";
		});
		if (!propTemplate) {
			return false;
		}
		const MapEntity templ = *propTemplate;
		const std::unordered_set<std::uint64_t> loaded = BaseModels(traced);
		std::uint32_t id = std::max(NextEntityId(entities), NextEntityId(triggers));
		std::size_t placedCount = 0, solid = 0, unknown = 0;
		for (const MapKit::MkEntity* prop : props) {
			const std::string modelName = prop->Prop("model");
			if (modelName.empty()) {
				std::printf("  prop %s: no model; left out\n", prop->path.c_str());
				continue;
			}
			const std::uint64_t model = ModelKey(modelName);
			const std::string scaleText = prop->Prop("scale");
			const float scale = scaleText.empty() ? 1.0f : std::strtof(scaleText.c_str(), nullptr);
			const bool isSolid = prop->Prop("solid") != "false";
			MapEntity placed = isSolid
				? EntityFrom(templ, id++, place(prop->origin), 0.0f, { "client_server", "model", "modelscale", "spawnflags", "DYNAMICPATH" }, {})
				: EntityFrom(templ, id++, place(prop->origin), 0.0f, { "client_server", "model", "modelscale" }, { { "targetname", kNonSolidProps } });
			SetEntityAngles(placed, prop->angles);
			for (EntityKey& key : placed.keys) {
				if (key.key == "model") {
					key.hash = model;
				}
				else if (key.key == "modelscale") {
					key.number = scale > 0 ? scale : 1.0f;
				}
			}
			entities.push_back(std::move(placed));
			if (std::ranges::find(models, model) == models.end()) {
				models.push_back(model);
			}
			++placedCount;
			solid += isSolid ? 1 : 0;
			if (!loaded.contains(model)) {
				++unknown;
				std::printf("  prop %s: %s is not in the base map's zone: it shows only if zm_common or core loads it\n",
					prop->path.c_str(), modelName.c_str());
			}
		}
		std::printf("  props: %zu placed as script_models (%zu solid)%s\n", placedCount, solid,
			unknown ? "; see above for models that may not show" : "");
		return true;
	}

	// The model's X (forward), Y (left) and Z (up) in the world for the engine's angles (pitch down +, yaw, roll),
	// as AnglesToAxis makes them (Y = -right).
	std::array<std::array<float, 3>, 3> AxesFromAngles(const std::array<float, 3>& angles) {
		constexpr float kRadians = 3.14159265358979f / 180.0f;
		const float cp = std::cos(angles[0] * kRadians), sp = std::sin(angles[0] * kRadians);
		const float cy = std::cos(angles[1] * kRadians), sy = std::sin(angles[1] * kRadians);
		const float cr = std::cos(angles[2] * kRadians), sr = std::sin(angles[2] * kRadians);
		return { { { cp * cy, cp * sy, -sp },
			{ sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp },
			{ cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp } } };
	}

	// The solid props' collision (P2, 2026-09-28). A script_model collides only through its models' own xcollisions:
	// SP_ScriptModel_cand 0x7FF72848ECA0 ORs XCollision_GetContents 0x7FF7293EFBD0 (xcollision +88) over the entity's
	// models. Most scenery models' say only 0x1, and as entities those did not block (sandbags, ndu_ammo_crate_01 and a
	// debris piece were walk-through in the 2026-09-27 test): zm_silver collides with them through its streamed world
	// cells, which mapkit empties. So each solid prop also gets a hull in the world tree, as a brush does: the MeshHull
	// of its model's full-detail mesh, turned, scaled and moved as the prop (the model's bounds when its mesh does not
	// decode). The models come from <map> and, when its trace is next to <map>'s, zm_common: the zones the model picker
	// lists.
	//
	// The power switch's console (AddPowerSwitch) is a script_model too, and gets the same.
	//
	// So do the perk machines and the Mystery Box locations (2026-10-05: in zm_debug an unpowered machine and the box
	// were walk-through), unless `objects` is off:
	// - a machine is a script_model the perk script spawns with the perk's _off model and no clip of its own
	//   (zm_perks.gsc: `collision = undefined`), swapped for the powered model when the power comes on. Every _off
	//   model's xcollision says 0x11, with nothing in its upper half (q >> 26, the clip: player 0x10000, monster
	//   0x20000); the powered ones say 0x131651, which is why a machine blocked only once it had power;
	// - the box is a zbarrier (zmcore_magicbox) with no collision model at all (+120 null: SP_ZBarrier_cand
	//   0x7FF7287525D0 then gives the entity neither a model nor contents), and its pieces' xcollisions say 0x1.
	// A retail map blocks at both with clip in its own world. The hull is around the model the struct names (the powered
	// machine, the box), turned as the game turns it, at every placed machine and box location, whether the box is
	// there or not. These are obstacles, not floors: the navmesh never runs over their tops (ConvexHull::walkable).
	template <typename Place>
	bool ModelHulls(const Options& options, const std::string& map, const MapKit::MkMap& source, const Place& place,
		const std::vector<MapEntity>& baseEntities, bool objects, std::vector<ConvexHull>& hulls) {
		struct Solid {
			const MapKit::MkEntity* node;
			const char* what;            // prop, power switch, perk machine, box location
			std::string name;            // the model, as the source names it
			std::uint64_t model;
			float scale;
			std::array<float, 3> angles; // the model's, in the game
			bool walkable;
		};
		// The model a <map> entity names (a hash key, as zm_silver's are), or 0.
		const auto modelOf = [](const MapEntity& entity) -> std::uint64_t {
			const EntityKey* key = entity.Find("model");
			if (!key) {
				return 0;
			}
			return key->type == EntityValueType::String ? ModelKey(key->text) : key->hash & ~(1ull << 63);
		};
		std::vector<Solid> solid;
		for (const MapKit::MkEntity& entity : source.entities) {
			if (entity.cls == "prop" && entity.Prop("solid") != "false" && !entity.Prop("model").empty()) {
				const std::string scaleText = entity.Prop("scale");
				const float scale = scaleText.empty() ? 1.0f : std::strtof(scaleText.c_str(), nullptr);
				solid.push_back({ &entity, "prop", entity.Prop("model"), ModelKey(entity.Prop("model")), scale > 0 ? scale : 1.0f,
					entity.angles, true });
			}
			else if (entity.cls == "power_switch") {
				// The console stands upright, turned by the node's yaw only (AddPowerSwitch).
				if (const MapEntity* console = PowerSwitchConsole(baseEntities); console && modelOf(*console)) {
					const std::uint64_t model = modelOf(*console);
					solid.push_back({ &entity, "power switch", std::format("#{:016x}", model), model, 1.0f,
						{ 0.0f, entity.angles[1], 0.0f }, true });
				}
			}
		}
		if (objects) {
			// The same machines and box locations PlaceGameplayObjects moves <map>'s structs to.
			const std::vector<const MapKit::MkEntity*> machines = SourceNodes(source, "perk_machine");
			const std::vector<std::size_t> machineStructs = PerkMachineStructs(machines, baseEntities);
			for (std::size_t k = 0; k < machines.size(); ++k) {
				if (machineStructs[k] >= baseEntities.size()) {
					continue;
				}
				if (const std::uint64_t model = modelOf(baseEntities[machineStructs[k]])) {
					solid.push_back({ machines[k], "perk machine", std::format("#{:016x}", model), model, 1.0f,
						{ 0.0f, NormalizeYaw(machines[k]->angles[1] + kPerkStructYaw), 0.0f }, false });
				}
			}
			const std::vector<const MapKit::MkEntity*> boxes = BoxNodes(source);
			const std::vector<std::size_t> chests = BoxStructs(baseEntities);
			for (std::size_t k = 0; k < boxes.size() && k < chests.size(); ++k) {
				if (const std::uint64_t model = modelOf(baseEntities[chests[k]])) {
					solid.push_back({ boxes[k], "box location", std::format("#{:016x}", model), model, 1.0f,
						{ 0.0f, NormalizeYaw(boxes[k]->angles[1] + kBoxStructYaw), 0.0f }, false });
				}
			}
		}
		if (solid.empty()) {
			return true;
		}
		ModelLibrary library;
		library.SetPackageDir(options.gameDir / "zone");
		for (const std::string& zone : { map, std::string("zm_common") }) {
			const fs::path trace = zone == map ? options.trace : options.trace.parent_path() / (zone + ".mktrace");
			if (!fs::exists(trace)) {
				std::printf("  model collision: no trace of %s (%s), so objects of its models get none\n", zone.c_str(),
					trace.string().c_str());
				continue;
			}
			ModelLibraryStats stats;
			std::string error;
			if (!library.AddZone(options.gameDir / "zone" / (zone + ".ff"), trace, stats, error)) {
				std::fprintf(stderr, "model collision: %s: %s\n", zone.c_str(), error.c_str());
				return false;
			}
		}
		std::size_t planes = 0;
		std::map<std::string, std::pair<std::size_t, std::size_t>> counts; // what -> made, wanted
		for (const Solid& each : solid) {
			const MapKit::MkEntity* prop = each.node;
			const std::string& name = each.name;
			++counts[each.what].second;
			const LibraryModel* model = library.Find(each.model);
			std::vector<std::array<float, 3>> points;
			std::string why;
			if (!model) {
				why = "the model is in neither zone read";
			}
			else {
				ModelGeometry geometry;
				if (model->problem.empty() && library.Geometry(*model, 0, geometry, why)) {
					for (const GeometrySurface& surface : geometry.surfaces) {
						points.insert(points.end(), surface.positions.begin(), surface.positions.end());
					}
				}
				if (points.empty()) {
					// Its bounds, when the mesh does not decode.
					for (int corner = 0; corner < 8; ++corner) {
						points.push_back({ (corner & 1 ? model->maxs : model->mins)[0], (corner & 2 ? model->maxs : model->mins)[1],
							(corner & 4 ? model->maxs : model->mins)[2] });
					}
				}
			}
			ConvexHull hull;
			if (!points.empty()
				&& MeshHull(points, AxesFromAngles(each.angles), each.scale, place(prop->origin), kHullSolid, hull, why)) {
				hull.walkable = each.walkable;
				std::printf("  %s %s: hull (%.0f %.0f %.0f)..(%.0f %.0f %.0f), %zu corners, %zu extra planes\n", each.what,
					prop->path.c_str(), hull.mins[0], hull.mins[1], hull.mins[2], hull.maxs[0], hull.maxs[1], hull.maxs[2],
					hull.vertices.size(), hull.planes.size());
				planes += hull.planes.size();
				hulls.push_back(std::move(hull));
				++counts[each.what].first;
			}
			else {
				std::printf("  %s %s: %s gets no collision (%s)\n", each.what, prop->path.c_str(), name.c_str(), why.c_str());
			}
		}
		std::string summary;
		for (const auto& [what, count] : counts) {
			summary += std::format("{}{} of {} {}{}", summary.empty() ? "" : ", ", count.first, count.second, what,
				count.second == 1 ? "" : "s");
		}
		std::printf("  model hulls: %s get a hull around their model in the world tree (%zu extra planes)\n", summary.c_str(),
			planes);
		return true;
	}

	// A barrier's player clip (MkBarrier). The game gives a zbarrier entity its asset's collision model (+120, through
	// G_SetModel) and contents 0x2080 (SP_ZBarrier_cand 0x7FF7287525D0): what bullets and grenades hit, the boards' own
	// shapes (the wood barrier's is p8_zm_barricade_board_collision, the concrete one's
	// p8_zm_esc_wall_barrier_collision_col; their xcollisions say 0x1). None of it stops a player. A retail map puts
	// player clip in the opening, in its own world, which a map of its own has none of (2026-10-05: zm_debug's wood
	// barrier was walk-through). So each barrier gets a hull over its collision model's bounds, in the zbarrier's frame
	// and at least kBarrierClipDepth deep, of kHullPlayerClip: players stop at the boards, bullets and grenades pass,
	// and zombies climb in as before (their climb is in noclip). The navmesh goes round it as round any hull, so a
	// zombie's only way through the opening is the barrier.
	constexpr float kBarrierClipDepth = 8.0f;

	template <typename Place>
	void BarrierClips(const MapKit::MkMap& source, const Place& place, const TracedMap& traced,
		const std::vector<MapEntity>& baseEntities, std::vector<ConvexHull>& hulls) {
		const std::span<const std::uint8_t> stream(traced.zone.stream);
		for (const MapKit::MkEntity* barrier : SourceNodes(source, "barrier")) {
			const std::string kind = barrier->Prop("kind") == "concrete" ? "concrete" : "wood";
			const std::string classname = BarrierClassname(kind);
			const MapEntity* zbarrier = BarrierGroup(baseEntities, classname);
			if (!zbarrier) {
				continue; // AddBarriers says so, and places none
			}
			// The zbarrier asset's collision model, when <map>'s zone stores both.
			const std::string name = classname.substr(kZBarrierClassPrefix.size());
			std::optional<std::size_t> modelRoot;
			for (std::size_t i = 0; i < traced.list.assets.size() && !modelRoot; ++i) {
				if (traced.list.assets[i].type != 0x53 || traced.list.assets[i].header != kPtrInline
					|| traced.trace.assets[i].offset + 944 > stream.size() || AssetName(traced, i) != HashName(name)
					|| (Get<std::uint64_t>(stream, traced.trace.assets[i].offset) >> 63)) {
					continue;
				}
				for (const auto& [field, root] : ZBarrierModelRoots(traced, i)) {
					if (field == 120 && root + 232 <= stream.size() && !(Get<std::uint64_t>(stream, root) >> 63)) {
						modelRoot = root;
					}
				}
			}
			if (!modelRoot) {
				std::printf("  barrier %s (%s): no player clip (the base map's zone does not store %s with a collision model)\n",
					barrier->path.c_str(), kind.c_str(), name.c_str());
				continue;
			}
			// The model's bounds (xmodel +184, +196), in the zbarrier's own axes.
			std::array<float, 3> centre{}, half{};
			for (int k = 0; k < 3; ++k) {
				const float low = Get<float>(stream, *modelRoot + 184 + 4 * k), high = Get<float>(stream, *modelRoot + 196 + 4 * k);
				centre[k] = (low + high) / 2;
				half[k] = (high - low) / 2;
			}
			half[0] = std::max(half[0], kBarrierClipDepth / 2);
			const auto axes = AxesFromAngles({ zbarrier->angles[0], NormalizeYaw(barrier->angles[1] + kBarrierYaw), zbarrier->angles[2] });
			std::array<float, 3> at = place(barrier->origin);
			std::array<std::array<float, 3>, 3> halfAxes{};
			for (int axis = 0; axis < 3; ++axis) {
				for (int k = 0; k < 3; ++k) {
					at[k] += axes[axis][k] * centre[axis];
					halfAxes[axis][k] = axes[axis][k] * half[axis];
				}
			}
			ConvexHull hull;
			std::string why;
			if (!BoxHull(at, halfAxes, kHullPlayerClip, hull, why)) {
				std::printf("  barrier %s (%s): no player clip (%s)\n", barrier->path.c_str(), kind.c_str(), why.c_str());
				continue;
			}
			hull.walkable = false;
			std::printf("  barrier %s (%s): player clip (%.0f %.0f %.0f)..(%.0f %.0f %.0f), %.0f wide, %.0f high, from %.0f above "
				"the node\n", barrier->path.c_str(), kind.c_str(), hull.mins[0], hull.mins[1], hull.mins[2], hull.maxs[0],
				hull.maxs[1], hull.maxs[2], 2 * half[1], 2 * half[2], centre[2] - half[2]);
			hulls.push_back(std::move(hull));
		}
	}

	// mapkit/level/<map> of the repo this cwlink.exe was built in: the first parent of the exe's folder that has it.
	fs::path DefaultLevelDir(const std::string& map) {
		std::wstring exe(MAX_PATH, L'\0');
		const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
		if (length == 0 || length >= exe.size()) {
			return {};
		}
		exe.resize(length);
		std::error_code ec;
		for (fs::path dir = fs::path(exe).parent_path(); !dir.empty(); dir = dir.parent_path()) {
			if (fs::is_directory(dir / "mapkit" / "level" / map, ec)) {
				return dir / "mapkit" / "level" / map;
			}
			if (dir == dir.parent_path()) {
				break;
			}
		}
		return {};
	}

	// Runs ACTS (acts.exe on PATH) with `arguments`, in this console. Returns its exit code, or -1 when it cannot start.
	int RunActs(const std::wstring& arguments) {
		wchar_t found[MAX_PATH]{};
		if (!SearchPathW(nullptr, L"acts.exe", nullptr, MAX_PATH, found, nullptr)) {
			return -1;
		}
		std::wstring command = L"\"" + std::wstring(found) + L"\" " + arguments;
		// ACTS writes to the same console: what cwlink printed so far goes first.
		std::fflush(stdout);
		STARTUPINFOW startup{ sizeof(startup) };
		PROCESS_INFORMATION process{};
		if (!CreateProcessW(found, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
			return -1;
		}
		WaitForSingleObject(process.hProcess, INFINITE);
		DWORD code = 1;
		GetExitCodeProcess(process.hProcess, &code);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		return static_cast<int>(code);
	}

	// The map's level scripts (docs/mapkit-plan.md P1): every .gsc/.csc in the level folder, compiled by ACTS under
	// the stock name scripts/<mode>/<file> into <out>/scripts. The client's GSC loader loads that folder with the
	// map's zones and serves each file in place of the stock script of its name (replace only, never injected).
	// Which stock scripts a map links is set by its script_using assets ({name, gsc, csc}); for zm_silver the
	// level-design ones are zm_silver.gsc/.csc, zm_silver_ffotd.gsc/.csc and zm_silver_zones.gsc.
	// The folder's old .gscc/.cscc files go first: they are cwlink's own output. `generated` holds files cwlink
	// writes for this map (file name -> text); each is compiled in place of the level folder's file of that name,
	// from <map folder>/generated/, where it stays for reading.
	// A map of its own (`standalone`) starts under its id, and the game runs the level script of the map it starts
	// (scripts/<p>/<map>.gsc and .csc, built from the map name: sub_7FF71E76DFD0, IDB 2026-09-29): the level folder's
	// <map>.gsc/.csc are compiled under that name instead. While <map>'s zone loads under it (`library`), <map>'s own level
	// script is still linked (that zone's script_using lists it), so an empty stand-in goes under its name: its includes
	// (the quests) stay unlinked. The client serves a map's scripts whether or not a zone holds a stock one of the name.
	int WriteLevelScripts(const Options& options, const std::string& map, const std::string& id,
		const std::map<std::string, std::string>& generated = {}, bool standalone = false, bool library = true) {
		const fs::path out = (options.outDir.empty() ? options.gameDir / "cw-mod" / "maps" / id : options.outDir) / "scripts";
		std::error_code ec;
		std::size_t removed = 0;
		for (auto it = fs::directory_iterator(out, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
			const std::string ext = it->path().extension().string();
			if (ext == ".gscc" || ext == ".cscc") {
				std::error_code removeEc;
				removed += fs::remove(it->path(), removeEc) ? 1 : 0;
			}
		}
		if (!options.levelScripts) {
			std::printf("  level scripts: none (--no-level-scripts), %s's own run%s\n", map.c_str(),
				removed ? std::format("; {} old compiled script(s) removed from {}", removed, out.string()).c_str() : "");
			return 0;
		}
		const fs::path source = options.levelDir.empty() ? DefaultLevelDir(map) : options.levelDir;
		if (source.empty() || !fs::is_directory(source, ec)) {
			std::fprintf(stderr, "  level scripts: no folder %s (pass --level <dir>, or --no-level-scripts)\n",
				source.empty() ? ("mapkit\\level\\" + map).c_str() : source.string().c_str());
			return 1;
		}
		const std::string mode = map.substr(0, map.find('_'));
		std::vector<fs::path> files;
		for (auto it = fs::directory_iterator(source, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
			const std::string ext = it->path().extension().string();
			if (ext == ".gsc" || ext == ".csc") {
				files.push_back(it->path());
			}
		}
		std::ranges::sort(files);
		fs::create_directories(out, ec);
		int failed = 0;
		for (const auto& [name, text] : generated) {
			const fs::path file = out.parent_path() / "generated" / name;
			fs::create_directories(file.parent_path(), ec);
			if (!WriteWholeFile(file, std::vector<std::uint8_t>(text.begin(), text.end()))) {
				std::fprintf(stderr, "  could not write %s\n", file.string().c_str());
				return 1;
			}
			const auto it = std::ranges::find(files, name, [](const fs::path& path) { return path.filename().string(); });
			if (it != files.end()) {
				*it = file;
			}
			else {
				files.push_back(file);
			}
		}
		if (standalone && library) {
			for (const char* ext : { ".gsc", ".csc" }) {
				const std::string stub = std::format("// Written by cwlink: {0}'s level script is not run on a map of its own, which\n"
					"// loads {0}'s zones only as its asset library. {0}'s zone still links this script by name\n"
					"// (script_using), so this empty one stands in for it and its quests stay unlinked. The map's own\n"
					"// level script is scripts/{1}/{2}{3}.\n\n#namespace mapkit_library_stub;\n\nfunction autoexec __init__()\n{{\n}}\n",
					map, mode, id, ext);
				const fs::path file = out.parent_path() / "generated" / (map + "_stub" + ext);
				fs::create_directories(file.parent_path(), ec);
				if (!WriteWholeFile(file, std::vector<std::uint8_t>(stub.begin(), stub.end()))) {
					std::fprintf(stderr, "  could not write %s\n", file.string().c_str());
					return 1;
				}
				files.push_back(file);
			}
		}
		for (const fs::path& file : files) {
			const bool client = file.extension() == ".csc";
			// A map of its own: its level script under its own name, the stand-in under <map>'s (above).
			std::string stem = file.stem().string();
			if (standalone && stem == map) {
				stem = id;
			}
			else if (standalone && library && stem == map + "_stub") {
				stem = map;
			}
			const std::string name = "scripts/" + mode + "/" + stem + file.extension().string();
			const fs::path target = out / stem;
			const fs::path compiled = fs::path(target).replace_extension(client ? ".cscc" : ".gscc");
			const std::wstring arguments = std::format(L"-t gscc -g cw {} {} -o \"{}\" \"{}\"", client ? L"--name-client" : L"--name",
				fs::path(name).wstring(), target.wstring(), file.wstring());
			const int code = RunActs(arguments);
			if (code == -1) {
				std::fprintf(stderr, "  level scripts: ACTS (acts.exe) is not on PATH; pass --no-level-scripts to build without them\n");
				return 1;
			}
			if (code != 0 || !fs::exists(compiled, ec)) {
				std::fprintf(stderr, "  level script %s: ACTS failed (exit %d)\n", file.filename().string().c_str(), code);
				++failed;
				continue;
			}
			std::printf("  level script %-24s -> %s as %s (0x%016llX)\n", file.filename().string().c_str(),
				compiled.filename().string().c_str(), name.c_str(), static_cast<unsigned long long>(HashName(name)));
		}
		if (files.empty()) {
			std::fprintf(stderr, "  level scripts: %s has no .gsc or .csc\n", source.string().c_str());
			return 1;
		}
		return failed;
	}

	// A JSON string literal holding s.
	std::string JsonQuote(const std::string& s) {
		std::string out = "\"";
		for (const char c : s) {
			const auto u = static_cast<unsigned char>(c);
			if (c == '"' || c == '\\') {
				out += '\\';
				out += c;
			}
			else if (u < 0x20) {
				out += std::format("\\u{:04x}", static_cast<unsigned>(u));
			}
			else {
				out += c;
			}
		}
		return out + "\"";
	}

	// <out>/map.json, what makes the folder a listed custom map for the client (mapkit_loader.hpp): the title
	// CUSTOM MAPS shows, the retail map it is built on, and a description for the card. A map of its own (`standalone`)
	// also says so ("format": "standalone"), and, when not all of <map>'s zones load under it (`library` false), which of
	// them do ("library": only techset_<map>, for the sky's techset).
	int WriteMapJson(const Options& options, const std::string& id, const std::string& map, const MapKit::MkMap& source,
		bool standalone = false, bool library = true) {
		const fs::path out = (options.outDir.empty() ? options.gameDir / "cw-mod" / "maps" / id : options.outDir) / "map.json";
		const std::string title = source.title.empty() ? id : source.title;
		const std::string description = std::format("Built with mapkit{} on {}.",
			source.author.empty() ? std::string() : " by " + source.author, map);
		const std::string libraryZones = std::format("techset_{}", map);
		const std::string json = std::format("{{\n  \"title\": {},\n  \"base\": {},\n  \"description\": {},\n  \"author\": {}{}{}\n}}\n",
			JsonQuote(title), JsonQuote(map), JsonQuote(description), JsonQuote(source.author),
			standalone ? ",\n  \"format\": \"standalone\"" : "",
			standalone && !library ? std::format(",\n  \"library\": [ {} ]", JsonQuote(libraryZones)) : std::string());
		const std::vector<std::uint8_t> bytes(json.begin(), json.end());
		if (!WriteWholeFile(out, bytes)) {
			std::fprintf(stderr, "  could not write %s\n", out.string().c_str());
			return 1;
		}
		std::printf("  map.json: '%s', %s\n", title.c_str(), !standalone ? std::format("an overlay of {}", map).c_str()
			: library ? std::format("a map of its own, with the asset library of {}", map).c_str()
			: std::format("a map of its own, with only {} of {}'s zones", libraryZones, map).c_str());
		return 0;
	}

	// A map source laid over <map> (M4 step 2): the plane world, the source's solid brushes as hulls in the
	// world collision tree, and <map>'s player spawns and perk machines moved to the source's. The rest of
	// <map>'s entities and scripts stay, so its round logic runs.
	int Build(const Options& options, const std::string& map, const fs::path& sourcePath) {
		MapKit::MkMap source;
		std::string error;
		if (!MapKit::ReadMkMap(sourcePath, source, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		// The custom map's id: its folder and zone names. Never a retail name, or it would replace a retail zone.
		const std::string mapId = options.as.empty() ? source.name : options.as;
		if (!ValidMapName(mapId)) {
			std::fprintf(stderr, "'%s' is not a usable map id: lowercase letters, digits and '_' (at least one), at most 48 "
				"characters. Pass one with --as <id>.\n", mapId.c_str());
			return 1;
		}
		for (const char* prefix : { "", "ww_1080_", "ww_4k_" }) {
			if (mapId == map || fs::exists(options.gameDir / "zone" / (prefix + mapId + ".ff"))) {
				std::fprintf(stderr, "'%s' is a retail zone name; pass another id with --as <id>.\n", mapId.c_str());
				return 1;
			}
		}
		// The game finds a map's level scripts (scripts/<p>/<map>.gsc) and looks its world up (maps/<p>/<map>.d3dbsp)
		// under <p>, the text before the first '_' of the map it starts; the scripts are compiled under <map>'s.
		if (!options.overlay && mapId.substr(0, mapId.find('_')) != map.substr(0, map.find('_'))) {
			std::fprintf(stderr, "'%s' needs %s's mode prefix ('%s_'); pass another id with --as <id>.\n", mapId.c_str(),
				map.c_str(), map.substr(0, map.find('_')).c_str());
			return 1;
		}
		// Of <map>'s zones only techset_<map> loads under a map of its own (P6 step 2b-2: the sky's techset is still <map>'s), so
		// the map's zone holds the rest of what it takes from <map>. --with-library (and an overlay, laid over <map>) loads
		// them all, as stage 1 did.
		const bool library = options.withLibrary || options.overlay;
		if (!library && (options.libraryWorld || options.linkLibrary)) {
			std::fprintf(stderr, "--library-world and --link-library leave assets to %s's zone, which loads only with --with-library\n",
				map.c_str());
			return 1;
		}
		std::printf("build '%s' (%s, by %s) as custom map '%s': %zu brushes, %zu entities, over %s%s\n", source.name.c_str(),
			source.title.c_str(), source.author.c_str(), mapId.c_str(), source.brushes.size(), source.entities.size(), map.c_str(),
			library ? ", which loads under it as its asset library" : std::format(" (only techset_{} of its zones loads under it)",
				map).c_str());
		TracedMap traced;
		std::vector<MapEntity> entities;
		std::vector<MapEntity> baseTriggers;
		if (!LoadTracedMap(options, map, traced) || !ReadEntityList(traced, map, entities, &baseTriggers)) {
			return 1;
		}
		std::set<AssetKey> stays;
		if (!library && !LoadStayingAssets(options, map, stays)) {
			return 1;
		}
		const std::vector<MapEntity> baseEntities = entities;
		const std::uint32_t baseLastId = std::max(NextEntityId(entities), NextEntityId(baseTriggers)) - 1;

		// <map>'s initial spawns, by player number: the default place for the source's origin, and what the
		// source's player spawns move.
		std::vector<MapEntity*> spawns;
		for (MapEntity& entity : entities) {
			if (entity.Text("targetname") == "initial_spawn_points") {
				spawns.push_back(&entity);
			}
		}
		if (spawns.empty()) {
			std::fprintf(stderr, "%s has no initial_spawn_points\n", map.c_str());
			return 1;
		}
		std::ranges::sort(spawns, {}, [](const MapEntity* entity) { return entity->Text("script_noteworthy"); });
		std::array<float, 3> at{};
		if (options.at) {
			at = *options.at;
		}
		else {
			for (const MapEntity* spawn : spawns) {
				for (int k = 0; k < 3; ++k) {
					at[k] += spawn->origin[k] / spawns.size();
				}
			}
		}
		const auto place = [&at](const std::array<float, 3>& p) {
			return std::array<float, 3>{ p[0] + at[0], p[1] + at[1], p[2] + at[2] };
		};
		std::printf("  origin at %.1f %.1f %.1f (%s)\n", at[0], at[1], at[2], options.at ? "--at" : "the player spawns' center");

		// The collision: one hull per solid brush, in the world tree.
		std::vector<ConvexHull> hulls;
		float lowest = FLT_MAX;
		std::size_t drawn = 0;
		for (const MapKit::MkBrush& brush : source.brushes) {
			drawn += brush.rendered ? 1 : 0;
			if (!brush.solid) {
				continue;
			}
			ConvexHull hull;
			if (!BoxHull(place(brush.center), brush.halfAxes, 1, hull, error)) {
				std::fprintf(stderr, "brush %s: %s\n", brush.path.c_str(), error.c_str());
				return 1;
			}
			lowest = std::min(lowest, hull.mins[2]);
			hulls.push_back(std::move(hull));
		}
		const std::size_t brushHulls = hulls.size();
		MeshHulls(source, place, hulls, lowest);
		if (hulls.empty()) {
			std::fprintf(stderr, "the source has no solid brush or mesh to stand on\n");
			return 1;
		}
		if (!ModelHulls(options, map, source, place, baseEntities, options.objectClip, hulls)) {
			return 1;
		}
		if (options.objectClip) {
			BarrierClips(source, place, traced, baseEntities, hulls);
		}
		else {
			std::printf("  perk machines, box locations and barriers get no collision of their own (--no-object-clip)\n");
		}
		// The terrain is a safety floor under the whole map, as low as it can be stored.
		PlaneWorld plane;
		if (!MakePlaneWorld(traced, map, options.floor.value_or(lowest), plane) || !MakeGfxWorld(traced, map, options, plane)) {
			return 1;
		}
		CollisionTree& world = plane.clip->trees.at(0);
		const std::uint32_t before = Get<std::uint32_t>(world.raw, 32);
		if (!AppendHulls(world, hulls, 0, error)) {
			std::fprintf(stderr, "world collision: %s\n", error.c_str());
			return 1;
		}
		std::size_t planes = 0;
		for (std::size_t i = 0; i < brushHulls; ++i) {
			planes += hulls[i].planes.size();
		}
		std::printf("  brushes: %zu solid (%zu extra planes), %zu drawn (the model below); world tree hulls %u..%u: the brushes', "
			"then the meshes', the models' and the barriers' (above), surface 0\n", brushHulls, planes, drawn, before,
			before + static_cast<std::uint32_t>(hulls.size()) - 1);
		std::printf("  world tree: bounds (%.0f %.0f %.0f)..(%.0f %.0f %.0f), %u hulls, %u planes, %u vertices, contents %#x, "
			"face words %zu\n", Get<float>(world.raw, 0), Get<float>(world.raw, 4), Get<float>(world.raw, 8),
			Get<float>(world.raw, 12), Get<float>(world.raw, 16), Get<float>(world.raw, 20), Get<std::uint32_t>(world.raw, 32),
			Get<std::uint32_t>(world.raw, 24), Get<std::uint32_t>(world.raw, 28), Get<std::uint32_t>(world.raw, 36),
			world.arrays[5].size() / 8);

		// The entities: the level's own lists (ComposeLevelEntities), from the source's zones, spawners, player
		// spawns and Zombies objects, with <map>'s structs of each kind moved to them (PlaceGameplayObjects).
		std::vector<LevelZone> zones;
		std::vector<LevelDoor> doors;
		if (!ReadZones(source, place, zones, doors)) {
			return 1;
		}
		std::vector<const MapKit::MkEntity*> playerSpawns;
		std::map<std::string, std::size_t> notBuilt;
		for (const MapKit::MkEntity& entity : source.entities) {
			if (entity.cls == "player_spawn") {
				playerSpawns.push_back(&entity);
			}
			else if (entity.cls != "perk_machine" && entity.cls != "mystery_box" && entity.cls != "wall_buy"
				&& entity.cls != "arsenal" && entity.cls != "pack_a_punch" && entity.cls != "zone"
				&& entity.cls != "zombie_spawner" && entity.cls != "light" && entity.cls != "ammo_cache"
				&& entity.cls != "armor_station" && entity.cls != "wunderfizz" && entity.cls != "crafting_table"
				&& entity.cls != "exfil" && entity.cls != "exfil_radio" && entity.cls != "door" && entity.cls != "prop"
				&& entity.cls != "barrier" && entity.cls != "power_switch" && entity.cls != "ambient_room") {
				++notBuilt[entity.cls];
			}
		}
		if (playerSpawns.empty()) {
			std::fprintf(stderr, "the source has no player_spawn\n");
			return 1;
		}
		// <map>'s spawn n takes the source's spawn for player n, else the source's spawns in turn.
		for (std::size_t i = 0; i < spawns.size(); ++i) {
			const std::string noteworthy = spawns[i]->Text("script_noteworthy"); // player_<n>
			const std::string player = noteworthy.substr(noteworthy.rfind('_') + 1);
			const MapKit::MkEntity* target = playerSpawns[i % playerSpawns.size()];
			for (const MapKit::MkEntity* candidate : playerSpawns) {
				if (candidate->Prop("player") == player) {
					target = candidate;
				}
			}
			SetEntityOrigin(*spawns[i], place(target->origin), target->angles[1]);
			std::printf("  spawn %s (entity %u) -> %s\n", spawns[i]->Text("script_noteworthy").c_str(), spawns[i]->id,
				target->path.c_str());
		}
		if (!notBuilt.empty()) {
			std::string list;
			for (const auto& [cls, count] : notBuilt) {
				list += std::format(" {} x{}", cls, count);
			}
			std::printf("  not built yet, left out:%s\n", list.c_str());
		}
		// Far under the map: where <map>'s objects go that its scripts may still look up by name.
		const std::array<float, 3> graveyard = { at[0], at[1], lowest - kGraveyardDepth };
		std::unordered_set<std::uint32_t> kept;
		PlaceGameplayObjects(source, map, place, graveyard, entities, kept);
		if (!ApplyMoves(options, entities)) {
			return 1;
		}

		// The world assets keep <map>'s names in both forms: they replace <map>'s world in the override swap, since the
		// engine keeps one world loaded (mapkit_loader.hpp 7.: a world named after the id crashed the 19:04 run). Without
		// <map>'s zones they are the only ones of those names, and the client looks the world up by them (its alias).
		std::vector<ZoneAsset> replacements;
		// The stream keys of the lighting's emptied shadow trees, for the copies of those keys (AddLibraryCopies). With
		// <map>'s zone loaded the keys are <map>'s own (a copy would lose the owner the lighting sets: LoadWritesInto), so
		// the trees stay as they are.
		std::vector<LightingCopy::KeyEdit> keyEdits;
		if (!library) {
			// Nothing is swapped in later then, and an asset finds only what has loaded before it (a by-name reference to a
			// lighting no zone has loaded drops the game: DB_LinkMissingReference 0x7FF727EC1860). So the creator's sky
			// material goes first (the copied sky domes draw with it), then the lighting, which the empty streamerworld and
			// the gfx_map link.
			AddSky(source, sourcePath.parent_path(), traced, replacements);
			if (!AddLighting(source, at, traced, replacements, true, options.keepLights, options.keepBakedShadows, keyEdits)) {
				return 1;
			}
		}
		AddPlaneWorldAssets(map, plane, replacements);
		std::uint64_t brushModel = 0;
		if (!AddBrushModel(source, sourcePath.parent_path(), traced, replacements, brushModel)) {
			return 1;
		}
		if (library) {
			AddSky(source, sourcePath.parent_path(), traced, replacements);
			if (!AddLighting(source, at, traced, replacements, !options.libraryWorld, options.keepLights, true, keyEdits)) {
				return 1;
			}
		}
		if (!options.libraryWorld && !AddLevelWorldAssets(traced, map, options.keepTerrain, options.keepLights, replacements)) {
			return 1;
		}
		std::string navmeshObj;
		if (options.navmesh) {
			// Seeds: where players and zombies start and the objects players use. Not zones, ambient rooms, lights, props,
			// doors or barriers, which may float or stand in a wall. A perk machine or a box location fills its own
			// origin with its hull (ModelHulls), so its seed is where a player stands to use it: in front of it.
			std::vector<std::array<float, 3>> seeds;
			for (const MapKit::MkEntity& entity : source.entities) {
				if (entity.cls != "zone" && entity.cls != "ambient_room" && entity.cls != "light" && entity.cls != "prop"
					&& entity.cls != "door" && entity.cls != "barrier") {
					std::array<float, 3> seed = entity.origin;
					if (options.objectClip && (entity.cls == "perk_machine" || entity.cls == "mystery_box")) {
						const float facing = entity.angles[1] * 3.14159265358979f / 180.0f;
						seed[0] += kUseSpotDistance * std::cos(facing);
						seed[1] += kUseSpotDistance * std::sin(facing);
					}
					seeds.push_back(place(seed));
				}
			}
			if (!AddNavMesh(options, map, mapId, seeds, hulls, traced, replacements, navmeshObj)) {
				std::fprintf(stderr, "(--no-navmesh builds the map on %s's navmesh)\n", map.c_str());
				return 1;
			}
		}
		else {
			std::printf("  navmesh: %s's (--no-navmesh)\n", map.c_str());
		}
		if (brushModel) {
			// Placed by a script_model at the source's origin (a copy of one of <map>'s, cut to its placement keys).
			const std::uint32_t id = NextEntityId(entities);
			auto placed = PlacedModel(entities, brushModel, id, at, 0.0f);
			if (!placed) {
				std::fprintf(stderr, "%s has no script_model to copy for the brush model\n", map.c_str());
				return 1;
			}
			entities.push_back(std::move(*placed));
			std::printf("  brush model placed by script_model entity %u at %.1f %.1f %.1f\n", id, at[0], at[1], at[2]);
		}
		if (options.drawTest && !AddDrawTestRow(source, traced, *spawns.front(), replacements, entities)) {
			return 1;
		}
		std::vector<MapEntity> triggers;
		TriggerShapes shapes;
		const std::vector<LevelBarrier> barriers = ReadBarriers(source, place, zones);
		if (!ComposeLevelEntities(source, place, zones, barriers, baseEntities, baseTriggers, kept, baseLastId, entities, triggers,
			shapes)) {
			return 1;
		}
		std::vector<std::uint64_t> entityModels;
		if (!AddDoors(doors, HasPowerSwitch(source), traced, baseEntities, baseTriggers, entities, triggers, shapes, entityModels)
			|| !AddBarriers(barriers, baseEntities, entities, triggers)
			|| !AddPowerSwitch(source, place, baseEntities, baseTriggers, entities, triggers, shapes, entityModels)
			|| !AddAmbientRooms(source, place, map, library, traced, baseTriggers, entities, triggers, shapes)
			|| !AddProps(source, place, traced, baseEntities, triggers, entities, entityModels)) {
			return 1;
		}
		if (!library && (!UseStayingSpawners(entities, stays) || !AddEntityModels(traced, stays, entities, replacements, entityModels)
			|| !AddEntityZBarriers(traced, map, stays, entities, replacements, entityModels))) {
			return 1;
		}
		const std::string zonesScript = ZonesScript(source, zones, doors, ReadLevelSettings(source, place, zones), library);
		std::printf("  zones: %zu (%zu active at start), %zu door link(s); zm_silver_zones.gsc generated\n", zones.size(),
			static_cast<std::size_t>(std::ranges::count(zones, true, &LevelZone::start)), doors.size());
		replacements.push_back(BgCacheAsset(source.name, replacements, entityModels));
		std::printf("  %s: mapkit_%s_bgcache\n", replacements.back().label.c_str(), source.name.c_str());
		replacements.push_back(EntityListAsset(map, entities));
		replacements.push_back(TriggerListAsset(map, shapes, triggers));
		if (!library && !AddPackages(traced, map, mapId, replacements)) {
			return 1;
		}
		std::map<std::uint64_t, std::string> modelNames{ { ModelKey(kDefaultDoorModel), kDefaultDoorModel } };
		for (const MapKit::MkEntity& entity : source.entities) {
			if (const std::string model = entity.Prop("model"); !model.empty()) {
				modelNames.emplace(ModelKey(model), model);
			}
		}
		if (!options.libraryWorld && !options.linkLibrary
			&& !AddLibraryCopies(traced, map, stays, modelNames, replacements, library, options.acoustics, keyEdits)) {
			return 1;
		}
		const bool standalone = !options.overlay;
		int failed = standalone ? WriteMapZone(options, map, replacements, entities.size(), mapId, library)
			: WriteOverrideZones(options, map, replacements, entities.size(), mapId);
		failed += WriteLevelScripts(options, map, mapId, { { "zm_silver_zones.gsc", zonesScript } }, standalone, library);
		failed += WriteMapJson(options, mapId, map, source, standalone, library);
		if (!navmeshObj.empty()) {
			const fs::path obj = (options.outDir.empty() ? options.gameDir / "cw-mod" / "maps" / mapId : options.outDir) / "generated"
				/ "navmesh.obj";
			std::error_code ec;
			fs::create_directories(obj.parent_path(), ec);
			if (WriteWholeFile(obj, std::vector<std::uint8_t>(navmeshObj.begin(), navmeshObj.end()))) {
				std::printf("  navmesh: its polygons as %s\n", obj.string().c_str());
			}
		}
		if (failed) {
			std::printf("\nFAILED: the lines above say why; the map is not ready to play.\n");
		}
		else {
			std::printf("\nDone. In game: Zombies > Private > CUSTOM MAPS > '%s'.\n",
				source.title.empty() ? mapId.c_str() : source.title.c_str());
		}
		std::printf("The zones hold copies of your own game data for testing on this PC: never share them.\n");
		return failed ? 1 : 0;
	}

	int Clone(const Options& options, const std::string& map, const std::string& newMap) {
		if (!ValidMapName(newMap)) {
			std::fprintf(stderr, "'%s' is not a usable map name: lowercase letters, digits and '_', with a mode prefix "
				"like zm_, at most 48 characters.\n", newMap.c_str());
			return 1;
		}
		if (map.substr(0, map.find('_')) != newMap.substr(0, newMap.find('_'))) {
			std::fprintf(stderr, "'%s' and '%s' need the same mode prefix (the text before the first '_').\n",
				map.c_str(), newMap.c_str());
			return 1;
		}
		const fs::path zoneDir = options.gameDir / "zone";
		if (fs::exists(zoneDir / (newMap + ".ff"))) {
			std::fprintf(stderr, "'%s' is a retail zone name; pick a new one.\n", newMap.c_str());
			return 1;
		}
		const std::vector<std::string> prefixes = FindVariants(zoneDir, map);
		if (std::ranges::find(prefixes, std::string()) == prefixes.end()) {
			std::fprintf(stderr, "no %s.ff in %s\n", map.c_str(), zoneDir.string().c_str());
			return 1;
		}

		const fs::path out = options.outDir.empty() ? options.gameDir / "cw-mod" / "maps" / newMap : options.outDir;
		fs::create_directories(out);
		std::printf("clone %s -> %s (%zu zones) into %s\n", map.c_str(), newMap.c_str(), prefixes.size(), out.string().c_str());

		int failed = 0;
		for (const std::string& prefix : prefixes) {
			LoadedZone zone;
			std::string error;
			if (!LoadZone(zoneDir / (prefix + map + ".ff"), zone, error)) {
				std::fprintf(stderr, "  %s\n", error.c_str());
				++failed;
				continue;
			}
			const std::vector<HashRewrite> rewrites = RenameMapInStream(zone.stream, map, newMap);
			if (!WriteZone(zone, prefix + newMap, zone.stream, out)) {
				++failed;
				continue;
			}
			for (const HashRewrite& rewrite : rewrites) {
				if (rewrite.count) {
					std::printf("      %3zu x %2u-bit  %s -> %s\n", rewrite.count, rewrite.bits, rewrite.from.c_str(), rewrite.to.c_str());
				}
			}
		}

		std::printf("\n%s. This is a copy of your own game data for testing on this PC: never share it.\n",
			failed ? "FAILED" : "Done");
		return failed ? 1 : 0;
	}
}

int main(int argc, char** argv) {
	// Unbuffered, so what goes to stderr lands where it happens in a log of both (the editor's Build reads one pipe).
	std::setvbuf(stdout, nullptr, _IONBF, 0);
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
	try {
		if (options.args[0] == "clone") {
			return Clone(options, options.args[1], options.args[2]);
		}
		if (options.args[0] == "patch") {
			return Patch(options, options.args[1]);
		}
		if (options.args[0] == "plane") {
			return Plane(options, options.args[1]);
		}
		if (options.args[0] == "build") {
			return Build(options, options.args[1], options.rawArgs[2]);
		}
		return Replace(options, options.args[1]);
	}
	catch (const std::exception& e) {
		std::fprintf(stderr, "%s\n", e.what());
		return 1;
	}
}
