// ffinfo: inspect, unpack and repack Black Ops Cold War fastfiles.
//
//   ffinfo [--game <dir>] <zone|file.ff> [--stream <out.bin>] [--repack <out.ff>] [--assets] [--walk]
//          [--entities] [--entities-json <out.json>] [--trace <zone.mktrace>]
//   ffinfo [--game <dir>] --scan
//   ffinfo [--game <dir>] --walk-scan
//   ffinfo --hk <file>
//   ffinfo [--game <dir>] <level zone> --trace <zone.mktrace> --needs [--usage <file.mkuse>] [--map-zone <id.ff>]
//          [--keep <zone>[:<trace>]]... [--needs-list <file>]
//
// <dir> is the game folder (the one holding BlackOpsColdWar.exe, zone\ and oo2core_8_win64.dll).
// It defaults to %MAPKIT_GAME_DIR%, then the current folder. Nothing read from the game is ever
// written anywhere but the paths you pass.

#include <zonekit/asset_loaders.hpp>
#include <zonekit/asset_walk.hpp>
#include <zonekit/fastfile.hpp>
#include <zonekit/gfx_world.hpp>
#include <zonekit/hash.hpp>
#include <zonekit/havok_tagfile.hpp>
#include <zonekit/kapi.hpp>
#include <zonekit/level_assets.hpp>
#include <zonekit/library_assets.hpp>
#include <zonekit/model_assets.hpp>
#include <zonekit/map_entities.hpp>
#include <zonekit/oodle.hpp>
#include <zonekit/xasset_list.hpp>
#include <zonekit/zone.hpp>
#include <zonekit/world_assets.hpp>
#include <zonekit/zone_trace.hpp>
#include <zonekit/ref_graph.hpp>
#include <zonekit/usage_file.hpp>

#include <algorithm>
#include <regex>
#include <set>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace MapKit::Zone;

namespace {
	struct Options {
		fs::path gameDir;
		std::string zone;
		fs::path streamOut;
		fs::path repackOut;
		bool listAssets = false;
		bool scan = false;
		bool walk = false;
		bool walkScan = false;
		bool entities = false;
		fs::path entitiesJson;
		std::string entitiesMap; // --map: whose lists to look for (default: the zone's own map)
		fs::path trace;
		bool world = false;
		bool clipMap = false; // --clipmap
		bool gfx = false;     // --gfx
		bool levelAssets = false; // --level-assets
		bool libraryAssets = false; // --library-assets
		fs::path checkCopies;       // --check-copies <map zone .ff>
		std::size_t cell = SIZE_MAX; // --cell <n>: list that cell's models
		std::size_t meshAsset = SIZE_MAX;
		std::uint64_t meshName = 0; // --mesh #<hash>: the xmodel of that name
		bool residentMeshes = false; // --resident-meshes
		std::string material;        // --material <name|#hash>
		fs::path ddsDir;             // --dds <dir>: with --material, each image's stored mips as a .dds (game data)
		bool images = false;         // --images
		bool residentMaterials = false; // --resident-materials
		bool semantics = false;         // --semantics
		std::uint32_t semantic = 0;     // --semantic <hex>: with --semantics, that semantic per techset
		fs::path materialTable;         // --material-table <file>
		fs::path modelMaterials;        // --model-materials <file>
		std::vector<std::pair<std::size_t, std::size_t>> dumps; // --dump <asset|type name>:<bytes>
		fs::path objOut;
		fs::path hkFile;                // --hk <file>: a Havok tagfile (or a navmesh payload): read, write back, compare
		bool needs = false;             // --needs: what a map built on this level zone still takes from it (plan P6)
		fs::path usage;                 // --usage <file.mkuse>: the client's asset-usage census of a match
		fs::path usageBase;             // --usage-base <file.mkuse>: a census written before the match (the frontend's)
		fs::path mapZone;               // --map-zone <id.ff>: the map's own zone (its by-name references count too)
		std::vector<std::string> keeps; // --keep <zone>[:<trace>]: a zone that stays loaded without the library
		fs::path needsList;             // --needs-list <file>: every asset the count found, one per line
		fs::path needsGraph;            // --needs-graph <file>: the level zone's link graph, one asset per line
	};

	void Usage() {
		std::puts(
			"usage: ffinfo [--game <dir>] <zone|file.ff> [--assets] [--stream <out.bin>] [--repack <out.ff>] [--walk]\n"
			"       ffinfo [--game <dir>] --scan\n"
			"       ffinfo [--game <dir>] --walk-scan\n"
			"\n"
			"  --game <dir>       game folder (default: %MAPKIT_GAME_DIR%, then the current folder)\n"
			"  --assets           print every asset's index, type and stream offset of the asset table\n"
			"                     (with --walk: also each walked asset's stream range)\n"
			"  --stream <file>    write the current (patched) xfile stream\n"
			"  --repack <file>    write a standalone .ff (no .fd) holding the current stream, then\n"
			"                     re-read it and check the stream round-trips byte for byte\n"
			"  --scan             decode every zone in <game>\\zone and report any that fail\n"
			"  --walk             walk the stream asset by asset with mapkit's loaders and check the result\n"
			"  --walk-scan        walk every zone: which walk completely, and which missing loader stops\n"
			"                     the most zones\n"
			"  --entities         a level zone's entities (entity list + trigger list): counts by classname\n"
			"  --entities-json <file>  every entity with all its keys, as JSON\n"
			"  --map <name>       with --entities: the map whose lists to look for (a custom map's override zones\n"
			"                     hold its base map's, e.g. --map zm_silver)\n"
			"  --trace <file>     check a zone trace the cw-mod client recorded (cw-mod.json \"mapkit_trace\")\n"
			"                     against this zone, then decode every asset mapkit has a loader for from\n"
			"                     its recorded start and check it ends where the next one starts\n"
			"  --world            with --trace: decode the level's streamerworld and print its model list and\n"
			"                     cell records instead of the replay table\n"
			"  --clipmap          with --trace: decode the level's clip map (cells, collision trees)\n"
			"  --gfx              with --trace: decode the level's gfx_map: what it links to, its sub-arrays\n"
			"  --level-assets     with --trace: decode the level's other world assets (terraingfx, staticlevelfxlist,\n"
			"                     glasses, com_map, cpu_occlusion_data, game_map, navvolume): what each links to\n"
			"                     and whether mapkit can copy it (plan P6 step 2); then the lighting's lights and baked\n"
			"                     shadow trees, with what each tree's stream key names in the packages. Without --trace\n"
			"                     for a zone mapkit walks whole (a map's own <id>.ff)\n"
			"  --library-assets   with --trace: read every image, streamkey, xskeleton, xmodelmesh, xmodel, material,\n"
			"                     klf, winddef and sound type (bank, asset, duck, alias modifier, acoustics) the zone holds\n"
			"                     with mapkit's copy readers: how many decode exactly and how many a copy can carry whole,\n"
			"                     with what stops the rest (plan P6 step 2b)\n"
			"  --check-copies <ff>  with --trace: read back every copy a map's zone holds of this zone's assets (cwlink\n"
			"                     build) and compare it with the original: the same bytes but where a pointer or\n"
			"                     string was re-pointed, each saying the same thing (plan P6 step 2b)\n"
			"  --mesh <asset>     with --trace: decode one xmodelmesh (or an xmodel's LOD 0; its name or #<hash> names the xmodel),\n"
			"                     fetch its buffer from\n"
			"                     the .ff or the .xsub packages and print its geometry\n"
			"  --obj <file>       with --mesh: also write the geometry as OBJ (game data: keep it local)\n"
			"  --material <name|#hash>  with --trace: decode one material, its image table and each image's header\n"
			"  --dds <dir>        with --material or --images: write the mips each image stores in the zone as .dds, and with\n"
			"                     --material a streamed image's pixels from the packages too (_streamed.dds) (game data:\n"
			"                     keep it local)\n"
			"  --images           with --trace: every image asset stored here, by format, size and residency\n"
			"  --resident-materials  with --trace: the materials whose images are all resident, by techset\n"
			"  --semantics        with --trace: every image semantic the materials use, its formats, and the\n"
			"                     commonest semantic sets\n"
			"  --semantic <hex>   --semantics, plus that semantic's image formats and constants per techset\n"
			"  --material-table <file>  with --trace: one tab-separated line per material: its name, techset, images\n"
			"                     (semantic:format:usage or semantic:ref:name), root and constant buffer in hex\n"
			"  --model-materials <file>  with --trace: every xmodel's LOD 0 materials (the table's +8 and +16 lists)\n"
			"  --hk <file>        a Havok tagfile, bare or in a navmesh payload (0x120-byte header): its types and\n"
			"                     items, then check that mapkit writes it back byte for byte\n"
			"  --needs            with --trace, on a level zone: what a map built on it still takes from it (mapkit plan\n"
			"                     P6): the assets a match used, every asset they link to, by type and size\n"
			"  --usage <file>     with --needs: the client's census of a match (cw-mod.json \"mapkit_usage\"): what it\n"
			"                     looked up, and which zone holds each loaded asset\n"
			"  --usage-base <file>  with --usage: a census the same game wrote before the match (the frontend's\n"
			"                     level_<time>.mkuse; the census keeps counting from the game's start): the lookups it\n"
			"                     already had, and that were not made again since, are left out\n"
			"  --map-zone <file>  with --needs: the map's own zone (<id>.ff); its links by name count as used\n"
			"  --keep <zone>[:<trace>]  with --needs and no census: a zone that stays loaded without the library\n"
			"                     (zm_common:<its trace>); a zone mapkit walks completely needs no trace\n"
			"  --needs-list <file>  with --needs: every asset to copy, one per line (index, type, name hash, bytes)\n"
			"  --needs-graph <file>  with --needs: every asset of the level zone and the ones it links to, one per line\n"
			"                     (index, type, name hash, bytes, own or link, linked indices), for questions the report\n"
			"                     does not answer");
	}

	bool ParseArgs(int argc, char** argv, Options& options) {
		for (int i = 1; i < argc; ++i) {
			const std::string arg = argv[i];
			const bool hasValue = i + 1 < argc;
			if (arg == "--game" && hasValue) {
				options.gameDir = argv[++i];
			}
			else if (arg == "--stream" && hasValue) {
				options.streamOut = argv[++i];
			}
			else if (arg == "--repack" && hasValue) {
				options.repackOut = argv[++i];
			}
			else if (arg == "--assets") {
				options.listAssets = true;
			}
			else if (arg == "--scan") {
				options.scan = true;
			}
			else if (arg == "--walk") {
				options.walk = true;
			}
			else if (arg == "--walk-scan") {
				options.walkScan = true;
			}
			else if (arg == "--entities") {
				options.entities = true;
			}
			else if (arg == "--entities-json" && hasValue) {
				options.entities = true;
				options.entitiesJson = argv[++i];
			}
			else if (arg == "--map" && hasValue) {
				options.entitiesMap = argv[++i];
			}
			else if (arg == "--trace" && hasValue) {
				options.trace = argv[++i];
			}
			else if (arg == "--world") {
				options.world = true;
			}
			else if (arg == "--gfx") {
				options.gfx = true;
			}
			else if (arg == "--level-assets") {
				options.levelAssets = true;
			}
			else if (arg == "--library-assets") {
				options.libraryAssets = true;
			}
			else if (arg == "--check-copies" && hasValue) {
				options.checkCopies = argv[++i];
			}
			else if (arg == "--clipmap") {
				options.clipMap = true;
			}
			else if (arg == "--cell" && hasValue) {
				options.world = true;
				options.cell = std::stoull(argv[++i]);
			}
			else if (arg == "--dump" && hasValue) {
				const std::string value = argv[++i];
				const auto colon = value.find(':');
				if (colon == std::string::npos) {
					return false;
				}
				std::size_t asset = SIZE_MAX;
				const std::string what = value.substr(0, colon);
				if (!what.empty() && std::isdigit(static_cast<unsigned char>(what[0]))) {
					asset = std::stoull(what);
				}
				else if (const auto type = XAssetTypeFromName(what)) {
					asset = SIZE_MAX - 1 - static_cast<std::size_t>(*type); // resolved to the type's first asset later
				}
				options.dumps.emplace_back(asset, std::stoull(value.substr(colon + 1)));
			}
			else if (arg == "--mesh" && hasValue) {
				const std::string value = argv[++i];
				if (value.starts_with('#')) {
					options.meshName = std::stoull(value.substr(1), nullptr, 16) & ~(1ull << 63);
					options.meshAsset = SIZE_MAX - 1; // resolved by name later
				}
				else if (!value.empty() && std::isdigit(static_cast<unsigned char>(value[0]))) {
					options.meshAsset = std::stoull(value);
				}
				else {
					// An xmodel by its name (a name used to reach stoull and end the program).
					options.meshName = HashName(value);
					options.meshAsset = SIZE_MAX - 1;
				}
			}
			else if (arg == "--resident-meshes") {
				options.residentMeshes = true;
			}
			else if (arg == "--material" && hasValue) {
				options.material = argv[++i];
			}
			else if (arg == "--images") {
				options.images = true;
			}
			else if (arg == "--dds" && hasValue) {
				options.ddsDir = argv[++i];
			}
			else if (arg == "--resident-materials") {
				options.residentMaterials = true;
			}
			else if (arg == "--semantics") {
				options.semantics = true;
			}
			else if (arg == "--semantic" && hasValue) {
				options.semantics = true;
				options.semantic = static_cast<std::uint32_t>(std::stoul(argv[++i], nullptr, 16));
			}
			else if (arg == "--material-table" && hasValue) {
				options.materialTable = argv[++i];
			}
			else if (arg == "--model-materials" && hasValue) {
				options.modelMaterials = argv[++i];
			}
			else if (arg == "--obj" && hasValue) {
				options.objOut = argv[++i];
			}
			else if (arg == "--needs") {
				options.needs = true;
			}
			else if (arg == "--usage" && hasValue) {
				options.usage = argv[++i];
			}
			else if (arg == "--usage-base" && hasValue) {
				options.usageBase = argv[++i];
			}
			else if (arg == "--map-zone" && hasValue) {
				options.mapZone = argv[++i];
			}
			else if (arg == "--keep" && hasValue) {
				options.keeps.push_back(argv[++i]);
			}
			else if (arg == "--needs-list" && hasValue) {
				options.needsList = argv[++i];
			}
			else if (arg == "--needs-graph" && hasValue) {
				options.needsGraph = argv[++i];
			}
			else if (arg == "--hk" && hasValue) {
				options.hkFile = argv[++i];
			}
			else if (!arg.empty() && arg[0] != '-' && options.zone.empty()) {
				options.zone = arg;
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
		return options.scan || options.walkScan || !options.hkFile.empty() || !options.zone.empty();
	}

	fs::path ResolveZone(const Options& options) {
		fs::path direct = options.zone;
		if (direct.extension() == ".ff" && fs::exists(direct)) {
			return direct;
		}
		return options.gameDir / "zone" / (options.zone + ".ff");
	}

	std::string Hex(std::span<const std::uint8_t> bytes) {
		std::string out;
		char buffer[3];
		for (std::uint8_t b : bytes) {
			std::snprintf(buffer, sizeof(buffer), "%02x", b);
			out += buffer;
		}
		return out;
	}

	std::string Megabytes(std::size_t bytes) {
		char buffer[32];
		std::snprintf(buffer, sizeof(buffer), "%.2f MB", bytes / (1024.0 * 1024.0));
		return buffer;
	}

	void PrintZone(const LoadedZone& zone, const XAssetList& list, bool listAssets) {
		const FastFileHeader& h = zone.header;
		std::printf("%s%s\n", zone.name.c_str(), zone.patched ? "  (patched by its .fd)" : "");
		std::printf("  header name      %s\n", h.ZoneName().c_str());
		std::printf("  version          %u\n", h.Version());
		std::printf("  builder          %s\n", h.BuilderName().c_str());
		std::printf("  build checksum   %s\n", Hex(h.BuildChecksum()).c_str());
		std::printf("  flags            server=%u codec=%u checksum-mode=%u encrypted=%u\n",
			zone.baseHeader.Flag(FlagIndex::Server), zone.baseHeader.Flag(FlagIndex::Compression),
			h.Flag(FlagIndex::ChecksumMode), zone.baseHeader.Flag(FlagIndex::Encrypted));
		std::printf("  stream           %s (%zu bytes)", Megabytes(zone.stream.size()).c_str(), zone.stream.size());
		if (zone.patched) {
			std::printf(", launch build %s", Megabytes(zone.baseStreamSize).c_str());
		}
		std::printf(", %zu blocks in the .ff\n", zone.blocks.size());

		std::printf("  xblock sizes    ");
		const auto sizes = h.BlockSizes();
		for (std::size_t i = 0; i < sizes.size(); ++i) {
			if (sizes[i]) {
				std::printf(" [%zu] 0x%llx", i, static_cast<unsigned long long>(sizes[i]));
			}
		}
		if (const HeaderSection* second = h.Find(HeaderTag::BlockSizes2); second && second->data.size() >= 104) {
			std::printf("\n  xblock sizes 2  ");
			for (std::size_t i = 0; i < kXBlockCount; ++i) {
				const auto size = Get<std::uint64_t>(second->data, 8 * i);
				if (size) {
					std::printf(" [%zu] 0x%llx", i, static_cast<unsigned long long>(size));
				}
			}
		}
		std::printf("\n  script strings   %zu\n  assets           %zu (table at stream +0x%zx, bodies from +0x%zx)\n",
			list.strings.size(), list.assets.size(), list.assetsOffset, list.bodyOffset);

		std::map<std::uint64_t, std::size_t> byType;
		for (const XAssetEntry& entry : list.assets) {
			++byType[entry.type];
		}
		std::vector<std::pair<std::size_t, std::uint64_t>> sorted;
		for (const auto& [type, count] : byType) {
			sorted.emplace_back(count, type);
		}
		std::sort(sorted.rbegin(), sorted.rend());
		for (const auto& [count, type] : sorted) {
			std::printf("    %7zu  %-24s (0x%02llx)\n", count, std::string(XAssetTypeName(type)).c_str(),
				static_cast<unsigned long long>(type));
		}

		if (listAssets) {
			std::printf("\n  index  type\n");
			for (std::size_t i = 0; i < list.assets.size(); ++i) {
				const XAssetEntry& entry = list.assets[i];
				std::printf("  %5zu  %s%s\n", i, std::string(XAssetTypeName(entry.type)).c_str(),
					entry.header == kPtrInline ? "" : "  (header not inline)");
			}
		}
	}

	std::string LoaderList() {
		std::string out;
		for (std::uint64_t type : AssetLoaderTypes()) {
			out += " " + std::string(XAssetTypeName(type));
		}
		return out;
	}

	std::vector<fs::path> ZoneFiles(const Options& options) {
		std::vector<fs::path> zones;
		for (const auto& entry : fs::directory_iterator(options.gameDir / "zone")) {
			if (entry.path().extension() == ".ff") {
				zones.push_back(entry.path());
			}
		}
		std::sort(zones.begin(), zones.end());
		return zones;
	}

	std::string JsonString(std::string_view text) {
		std::string out = "\"";
		for (const char c : text) {
			if (c == '"' || c == '\\') {
				out += '\\';
				out += c;
			}
			else if (static_cast<unsigned char>(c) < 0x20) {
				char buffer[8];
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
				out += buffer;
			}
			else {
				out += c;
			}
		}
		return out + "\"";
	}

	std::string JsonVector(const std::array<float, 3>& v) {
		char buffer[96];
		std::snprintf(buffer, sizeof(buffer), "[%g, %g, %g]", v[0], v[1], v[2]);
		return buffer;
	}

	std::string JsonValue(const EntityKey& key) {
		switch (key.type) {
		case EntityValueType::String:
			return JsonString(key.text);
		case EntityValueType::Vector:
			return JsonVector(key.vector);
		case EntityValueType::Float:
		case EntityValueType::Int:
			return key.ValueText();
		default:
			return JsonString(key.ValueText());
		}
	}

	bool PrintEntities(const LoadedZone& zone, const fs::path& jsonOut, const std::string& mapName) {
		MapEntities found;
		std::string error;
		if (!mapName.empty()) {
			if (!FindMapEntities(zone.stream, mapName, found, error)) {
				std::fprintf(stderr, "%s\n", error.c_str());
				return false;
			}
		}
		else if (!FindMapEntities(zone.stream, zone.name, found, error)) {
			// A variant zone (cwlink's override zones ww_1080_/ww_4k_<map>) holds its map's entities.
			bool variant = false;
			for (const std::string_view prefix : { "ww_1080_", "ww_4k_", "1080_", "4k_", "ww_" }) {
				std::string unused;
				if (zone.name.starts_with(prefix) && FindMapEntities(zone.stream, zone.name.substr(prefix.size()), found, unused)) {
					variant = true;
					break;
				}
			}
			if (!variant) {
				std::fprintf(stderr, "%s\n", error.c_str());
				return false;
			}
		}
		std::map<std::pair<bool, std::string>, std::size_t> byClass;
		std::size_t triggers = 0;
		for (const MapEntity& entity : found.entities) {
			++byClass[{ entity.trigger, entity.Text("classname") }];
			triggers += entity.trigger;
		}
		std::printf("\nentities of %s: %zu in the entity list (stream +0x%zx, data ends +0x%zx), %zu in the trigger list "
			"(+0x%zx); %zu string references into other assets unresolved\n",
			MapBspName(zone.name).c_str(), found.entities.size() - triggers, found.entityList, found.entityListEnd, triggers,
			found.triggerList, found.unresolved);
		std::vector<std::pair<std::size_t, std::pair<bool, std::string>>> sorted;
		for (const auto& [key, count] : byClass) {
			sorted.push_back({ count, key });
		}
		std::ranges::sort(sorted, std::greater<>());
		for (const auto& [count, key] : sorted) {
			std::printf("  %6zu  %-8s %s\n", count, key.first ? "trigger" : "entity", key.second.c_str());
		}
		std::printf("  trigger shapes: %zu models, %zu hulls, %zu slabs (trigger-list entity i has model i)\n",
			found.shapes.models.size(), found.shapes.hulls.size(), found.shapes.slabs.size());
		if (jsonOut.empty()) {
			return true;
		}
		// A trigger-list entity's shape (TriggerShapes, map_entities.hpp): its model's hulls and their slabs.
		const auto shapeJson = [&found](std::size_t index) {
			const TriggerShapes& shapes = found.shapes;
			if (index >= shapes.models.size()) {
				return std::string();
			}
			const auto& model = shapes.models[index];
			const std::uint16_t hullCount = Get<std::uint16_t>(model, 4);
			const std::uint16_t firstHull = Get<std::uint16_t>(model, 6);
			std::string json = std::format(", \"shape\": {{\"model\": {}, \"contents\": {}, \"hulls\": [", index,
				Get<std::uint32_t>(model, 0));
			for (std::size_t h = firstHull; h < std::size_t(firstHull) + hullCount && h < shapes.hulls.size(); ++h) {
				const auto& hull = shapes.hulls[h];
				json += std::format("{}{{\"mid\": [{}, {}, {}], \"half\": [{}, {}, {}], \"contents\": {}, \"slabs\": [",
					h == firstHull ? "" : ", ", Get<float>(hull, 0), Get<float>(hull, 4), Get<float>(hull, 8), Get<float>(hull, 12),
					Get<float>(hull, 16), Get<float>(hull, 20), Get<std::uint32_t>(hull, 24));
				const std::uint16_t slabCount = Get<std::uint16_t>(hull, 28);
				const std::uint16_t firstSlab = Get<std::uint16_t>(hull, 30);
				for (std::size_t k = firstSlab; k < std::size_t(firstSlab) + slabCount && k < shapes.slabs.size(); ++k) {
					const auto& slab = shapes.slabs[k];
					json += std::format("{}[{}, {}, {}, {}, {}]", k == firstSlab ? "" : ", ", Get<float>(slab, 0), Get<float>(slab, 4),
						Get<float>(slab, 8), Get<float>(slab, 12), Get<float>(slab, 16));
				}
				json += "]}";
			}
			return json + "]}";
		};
		std::size_t triggerIndex = 0;
		std::string json = "[\n";
		for (std::size_t i = 0; i < found.entities.size(); ++i) {
			const MapEntity& entity = found.entities[i];
			json += "{\"list\": " + JsonString(entity.trigger ? "trigger" : "entity") + ", \"id\": " + std::to_string(entity.id)
				+ ", \"origin\": " + JsonVector(entity.origin) + ", \"angles\": " + JsonVector(entity.angles)
				+ (entity.trigger ? shapeJson(triggerIndex++) : std::string()) + ", \"keys\": [";
			for (std::size_t k = 0; k < entity.keys.size(); ++k) {
				const EntityKey& key = entity.keys[k];
				json += (k ? ", " : "") + std::string("[") + JsonString(key.key) + ", " + std::to_string(static_cast<int>(key.type))
					+ ", " + JsonValue(key) + "]";
			}
			json += i + 1 < found.entities.size() ? "]},\n" : "]}\n";
		}
		json += "]\n";
		if (!WriteWholeFile(jsonOut, std::vector<std::uint8_t>(json.begin(), json.end()))) {
			std::fprintf(stderr, "could not write %s\n", jsonOut.string().c_str());
			return false;
		}
		std::printf("wrote %s: every entity as {list, id, origin, angles, keys: [[key, type, value]...]}\n",
			jsonOut.string().c_str());
		return true;
	}

	// The script-string indices mapkit knows the place of (every gfx_map's and sanim's) must fall inside the
	// zone's own table: the engine converts them through it at load (DB_ResolveScriptStringIndex), and an index
	// past its end reads a stray pointer (Test G1 20:16: copied sanims kept zm_silver's indices, and the engine
	// interned a string from the pointer 0xA). Returns false when one does not.
	bool PrintScriptStringCheck(const LoadedZone& zone, const XAssetList& list, const WalkResult& walk) {
		std::size_t checked = 0, assets = 0;
		std::vector<std::string> bad;
		auto check = [&](std::size_t asset, std::size_t at, std::uint32_t index) {
			++checked;
			if (index && (index >= list.strings.size() || !list.strings[index])) {
				bad.push_back(std::format("asset {} ({}) at +0x{:X}: index {}", asset, XAssetTypeName(walk.assets[asset].type),
					at, index));
			}
		};
		for (std::size_t i = 0; i < walk.assets.size(); ++i) {
			const WalkedAsset& asset = walk.assets[i];
			const auto bytes = std::span<const std::uint8_t>(zone.stream).subspan(asset.begin, asset.end - asset.begin);
			if (bytes.empty()) {
				continue;
			}
			if (asset.type == 0x66) {
				const auto fields = SAnimScriptStrings(bytes);
				if (!fields) {
					bad.push_back(std::format("asset {} (sanim) does not read back on its own", i));
					continue;
				}
				++assets;
				for (const std::size_t at : *fields) {
					check(i, asset.begin + at, Get<std::uint32_t>(bytes, at));
				}
			}
			else if (asset.type == 0x1B) {
				XStream s(zone.stream, asset.begin);
				s.Detach();
				GfxWorld world;
				if (!ReadGfxWorld(s, kPtrInline, world)) {
					bad.push_back(std::format("asset {} (gfx_map) does not read back on its own: {}", i, s.Error()));
					continue;
				}
				++assets;
				for (const GfxWorldString& string : world.strings) {
					check(i, string.at, string.index);
				}
			}
		}
		std::printf("  script strings   %zu indices in %zu gfx_map/sanim assets: %s (table of %zu)\n", checked, assets,
			bad.empty() ? "all inside the zone's table" : std::format("{} BAD", bad.size()).c_str(), list.strings.size());
		for (std::size_t i = 0; i < bad.size() && i < 20; ++i) {
			std::printf("    %s\n", bad[i].c_str());
		}
		return bad.empty();
	}

	// Returns false when the walk hit a real error (not just a type without a loader).
	bool PrintWalk(const LoadedZone& zone, const XAssetList& list, bool listAssets) {
		const WalkResult walk = WalkStream(zone.stream);
		if (listAssets) {
			std::printf("\n  index  type                      stream range                     block 4 / block 6 at its start\n");
			for (std::size_t i = 0; i < walk.assets.size(); ++i) {
				const WalkedAsset& a = walk.assets[i];
				std::printf("  %5zu  %-24s  +0x%zx..+0x%zx (0x%zx)  0x%llx / 0x%llx\n", i,
					std::string(XAssetTypeName(a.type)).c_str(), a.begin, a.end, a.end - a.begin,
					static_cast<unsigned long long>(a.pos[4]), static_cast<unsigned long long>(a.pos[6]));
			}
		}
		std::printf("\nwalk (loaders:%s)\n", LoaderList().c_str());
		std::printf("  walked           %zu of %zu assets, stream +0x%zx of 0x%zx, %zu references checked\n",
			walk.assets.size(), walk.assetCount, walk.cursor, zone.stream.size(), walk.references);
		std::printf("  high water      ");
		for (std::size_t i = 0; i < walk.highWater.size(); ++i) {
			if (walk.highWater[i]) {
				std::printf(" [%zu] 0x%llx", i, static_cast<unsigned long long>(walk.highWater[i]));
			}
		}
		std::printf("\n");
		if (!walk.error.empty()) {
			std::printf("  ERROR            %s\n", walk.error.c_str());
			return false;
		}
		if (!walk.complete) {
			std::printf("  stopped          asset %zu is a %s: no loader yet\n", walk.assets.size(),
				std::string(XAssetTypeName(walk.blockedType)).c_str());
			return true;
		}
		const std::string blocks = CompareBlockSizes(walk, zone.header.BlockSizes());
		std::printf("  COMPLETE         the stream ends exactly after the last asset; block sizes %s\n",
			blocks.empty() ? "all match the header" : ("DIFFER:" + blocks).c_str());
		const std::uint64_t declared1 = zone.header.BlockSizes()[1];
		std::printf("  preload block 1  0x%llx as the lobby preload lays it out (XStream::PreloadRoot): %s the header's 0x%llx\n",
			static_cast<unsigned long long>(walk.preloadBlock1), walk.preloadBlock1 > declared1 ? "MORE than" : "within",
			static_cast<unsigned long long>(declared1));
		return PrintScriptStringCheck(zone, list, walk);
	}

	// Returns false when the trace does not fit the zone or a loader fails on a traced asset.
	bool PrintTrace(const LoadedZone& zone, const XAssetList& list, const fs::path& path) {
		ZoneTrace trace;
		std::string error;
		if (!ReadZoneTrace(path, trace, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		std::printf("\ntrace %s: zone '%s', flags 0x%X, %zu of %zu assets%s\n", path.string().c_str(), trace.zone.c_str(),
			trace.flags, trace.complete ? trace.assets.size() - 1 : trace.assets.size(), trace.assetCount,
			trace.complete ? " + end record" : " (INCOMPLETE: no end record)");
		const std::string mismatch = AlignZoneTrace(trace, zone.stream, list);
		if (!mismatch.empty()) {
			std::printf("  DOES NOT FIT     %s\n", mismatch.c_str());
			return false;
		}
		std::printf("  fits             every asset's type and header match the stream; offsets grow%s\n",
			trace.complete ? " and the end record lands on the stream's last byte" : "");
		if (trace.PreloadFamily()) {
			std::printf("  loader family    preload (flags & 0x6A0): blocks 0, 1 and 11 are not compared\n");
		}

		// Our walker's model of the stream against the engine's, where the first asset starts.
		const WalkResult walk = WalkStream(zone.stream);
		std::string differ;
		for (std::size_t b = 0; b < kXBlockCount; ++b) {
			if (trace.Comparable(b) && walk.bodyPositions[b] != trace.assets[0].pos[b]) {
				differ += std::format(" [{}] ours 0x{:X} engine 0x{:X}", b, walk.bodyPositions[b], trace.assets[0].pos[b]);
			}
		}
		std::printf("  first asset      block %d; block positions %s\n", trace.assets[0].block,
			differ.empty() ? "match mapkit's walk" : ("DIFFER:" + differ).c_str());
		std::size_t walkedMatch = 0;
		for (std::size_t i = 0; i < walk.assets.size(); ++i) {
			walkedMatch += walk.assets[i].begin == trace.assets[i].offset;
		}
		std::printf("  walked prefix    %zu of %zu walked assets start where the trace says\n", walkedMatch, walk.assets.size());

		struct TypeStats {
			std::size_t count = 0, bytes = 0, replayed = 0, failed = 0;
			std::vector<std::string> errors;
		};
		std::map<std::uint64_t, TypeStats> byType;
		const std::size_t last = trace.complete ? trace.assetCount : trace.assets.size() - 1;
		for (std::size_t i = 0; i < last; ++i) {
			TypeStats& t = byType[trace.assets[i].type];
			++t.count;
			t.bytes += trace.assets[i + 1].offset - trace.assets[i].offset;
			TraceReplay replay;
			if (ReplayTracedAsset(trace, zone.stream, i, replay)) {
				++t.replayed;
			}
			else if (!replay.error.empty()) {
				++t.failed;
				if (t.errors.size() < 3) {
					t.errors.push_back(std::format("asset {} at +0x{:X}: {}", i, trace.assets[i].offset, replay.error));
				}
			}
		}

		std::vector<std::pair<std::size_t, std::uint64_t>> sorted;
		for (const auto& [type, t] : byType) {
			sorted.emplace_back(t.bytes, type);
		}
		std::sort(sorted.rbegin(), sorted.rend());
		std::size_t failed = 0;
		std::printf("\n  %-24s %8s %12s  %s\n", "type", "assets", "stream", "replay (mapkit loader from the recorded start)");
		for (const auto& [bytes, type] : sorted) {
			const TypeStats& t = byType[type];
			failed += t.failed;
			std::string replay = t.replayed + t.failed == 0 ? "-" : std::format("{} ok, {} FAILED", t.replayed, t.failed);
			std::printf("  %-24s %8zu %12s  %s\n", std::string(XAssetTypeName(type)).c_str(), t.count, Megabytes(bytes).c_str(),
				replay.c_str());
			for (const std::string& e : t.errors) {
				std::printf("      %s\n", e.c_str());
			}
		}
		return failed == 0;
	}

	std::array<float, 13> Floats52(const std::array<std::uint8_t, 52>& raw) {
		std::array<float, 13> f{};
		std::memcpy(f.data(), raw.data(), sizeof(f));
		return f;
	}

	// The clip map, decoded from its traced start: what each part takes, the cells and their records, and the
	// world collision trees with their array sizes and vertex bounds.
	// The gfx_map as the splice writer sees it (gfx_world.hpp): every reference by what it points at, and the
	// sub-arrays hanging off the root.
	bool PrintGfx(const LoadedZone& zone, const XAssetList& list, const fs::path& tracePath) {
		ZoneTrace trace;
		std::string error;
		if (!ReadZoneTrace(tracePath, trace, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		if (const std::string mismatch = AlignZoneTrace(trace, zone.stream, list); !mismatch.empty()) {
			std::fprintf(stderr, "the trace does not fit: %s\n", mismatch.c_str());
			return false;
		}
		std::size_t index = list.assets.size();
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			if (list.assets[i].type == 0x1B) {
				index = i;
				break;
			}
		}
		if (index == list.assets.size()) {
			std::printf("\ngfx_map          none in this zone\n");
			return true;
		}
		const TracedAsset& start = trace.assets[index];
		XStream s(zone.stream, 0);
		s.Restore(start.offset, start.block, start.pos);
		GfxWorld world;
		const bool ok = ReadGfxWorld(s, list.assets[index].header, world);
		const std::size_t next = index + 1 < trace.assets.size() ? trace.assets[index + 1].offset : zone.stream.size();
		std::printf("\ngfx_map          asset %zu '%s' at +0x%zX, decoded to +0x%zX (next asset at +0x%zX)%s\n", index,
			world.name.c_str(), start.offset, s.Cursor(), next, ok && s.Cursor() == next ? ": exact" : "");
		if (!ok) {
			std::printf("  FAILED: %s\n", s.Error().c_str());
			return false;
		}
		// The XAsset array is the last thing block 4 holds before the first asset.
		const std::uint64_t arrayPos = trace.assets[0].pos[XBlockVirtual] - 16 * list.assets.size();
		// The innermost sub-array holding a stream offset ("root" for the root struct itself).
		auto innermost = [&](std::size_t at) -> std::string {
			const GfxWorldSubtree* best = nullptr;
			for (const GfxWorldSubtree& subtree : world.subtrees) {
				if (at >= subtree.begin && at < subtree.end && (!best || subtree.end - subtree.begin < best->end - best->begin)) {
					best = &subtree;
				}
			}
			return best ? best->name : "root";
		};
		std::map<std::uint64_t, std::size_t> byType;
		std::map<std::string, std::size_t> externalBy, internalBy, modelsBy;
		std::size_t internal = 0, external = 0, typeMismatch = 0;
		for (const GfxWorldRef& ref : world.refs) {
			const GfxWorldTarget target = ResolveGfxWorldRef(world, ref, arrayPos, list.assets.size());
			if (target.kind == GfxWorldTarget::Asset) {
				++byType[list.assets[target.asset].type];
				typeMismatch += ref.assetType != ~0ull && ref.assetType != list.assets[target.asset].type;
				if (list.assets[target.asset].type == 0x06) {
					++modelsBy[innermost(ref.at)];
				}
			}
			else if (target.kind == GfxWorldTarget::Internal) {
				++internal;
				++internalBy[innermost(ref.at) + " -> " + innermost(world.chunks[target.chunk].at)];
			}
			else {
				++external;
				++externalBy[innermost(ref.at)];
			}
		}
		for (const auto& [name, count] : externalBy) {
			std::printf("  into other assets' data: %5zu from %s\n", count, name.c_str());
		}
		for (const auto& [name, count] : internalBy) {
			std::printf("  into itself:             %5zu from %s\n", count, name.c_str());
		}
		for (const auto& [name, count] : modelsBy) {
			std::printf("  xmodel links:            %5zu from %s\n", count, name.c_str());
		}
		std::printf("  %zu chunks, %zu sub-arrays, %zu script strings, %zu nested assets inline\n", world.chunks.size(),
			world.subtrees.size(), world.strings.size(), world.inlineAssets);
		std::printf("  %zu references: %zu into itself, %zu into other assets' data, %zu wrong-typed links; links by type:",
			world.refs.size(), internal, external, typeMismatch);
		for (const auto& [type, count] : byType) {
			std::printf(" %s %zu", std::string(XAssetTypeName(type)).c_str(), count);
		}
		std::printf("\n  sub-arrays of the root and the draw struct (stream bytes, chunks, where in the stream):\n");
		for (const GfxWorldSubtree& subtree : world.subtrees) {
			if (subtree.field == kNoStream || subtree.field < world.rootAt || subtree.field >= world.rootAt + kGfxWorldRootSize) {
				continue;
			}
			std::printf("    +%-5zu %-26s %10zu %7zu  +0x%zX\n", subtree.field - world.rootAt, subtree.name.c_str(),
				subtree.end - subtree.begin, subtree.endChunk - subtree.firstChunk, subtree.begin);
		}
		return true;
	}

	// Where every asset of a zone starts: the client's trace of it, or, with no trace, a walk of it when mapkit's loaders
	// read the whole zone (a map's own zone, which the game need not have loaded yet).
	bool TraceOrWalk(const LoadedZone& zone, const XAssetList& list, const fs::path& tracePath, ZoneTrace& trace) {
		std::string error;
		if (!tracePath.empty()) {
			if (!ReadZoneTrace(tracePath, trace, error)) {
				std::fprintf(stderr, "%s\n", error.c_str());
				return false;
			}
			if (const std::string mismatch = AlignZoneTrace(trace, zone.stream, list); !mismatch.empty()) {
				std::fprintf(stderr, "the trace does not fit: %s\n", mismatch.c_str());
				return false;
			}
			return true;
		}
		const WalkResult walk = WalkStream(zone.stream);
		if (!walk.complete || walk.assets.size() != list.assets.size()) {
			std::fprintf(stderr, "no --trace, and mapkit cannot walk this zone whole (%s): record a trace of it (cw-mod.json "
				"\"mapkit_trace\")\n", walk.error.empty() ? "a type has no loader" : walk.error.c_str());
			return false;
		}
		trace = {};
		trace.zone = zone.name;
		trace.assetCount = list.assets.size();
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			TracedAsset asset;
			asset.type = list.assets[i].type;
			asset.header = list.assets[i].header;
			asset.offset = walk.assets[i].begin;
			asset.block = XBlockVirtual;
			asset.pos = walk.assets[i].pos;
			trace.assets.push_back(asset);
		}
		TracedAsset end;
		end.type = kTraceEndType;
		end.offset = zone.stream.size();
		end.block = XBlockVirtual;
		end.pos = walk.highWater;
		trace.assets.push_back(end);
		trace.complete = true;
		return true;
	}

	// The lighting asset's parts that belong to the level it was baked for (zonekit world_assets.hpp): its lights and its
	// baked shadow trees, each tree with what its stream key names in the packages.
	void PrintLighting(const LoadedZone& zone, const XAssetList& list, const ZoneTrace& trace, std::span<const std::uint64_t> names,
		const std::vector<bool>& byName) {
		const std::span<const std::uint8_t> stream(zone.stream);
		for (std::size_t index = 0; index < list.assets.size(); ++index) {
			if (list.assets[index].type != 0xAA || byName[index]) {
				continue;
			}
			LightingCopy copy;
			std::string error;
			const std::size_t next = index + 1 < trace.assets.size() ? trace.assets[index + 1].offset : stream.size();
			std::printf("\n  lighting #%016llX, asset %zu at +0x%zX: %zu bytes\n", static_cast<unsigned long long>(names[index]), index,
				trace.assets[index].offset, next - trace.assets[index].offset);
			if (!CopyLighting(trace, stream, list, index, copy, error)) {
				std::printf("    copy whole: no, %s\n", error.c_str());
				continue;
			}
			const std::span<const std::uint8_t> bytes(copy.bytes);
			std::size_t lit = 0;
			for (std::uint32_t i = 0; i < copy.lightCount; ++i) {
				bool colour = false;
				for (const std::size_t field : { kLightColor, kLightColorAgain }) {
					for (std::size_t c = 0; c < 3; ++c) {
						colour |= Get<float>(bytes, copy.lightsAt + kLightSize * i + field + 4 * c) != 0.0f;
					}
				}
				lit += colour;
			}
			std::printf("    copy whole: yes, %zu links, %zu pointers into itself; %u volumes\n", copy.links.size(), copy.pointers.size(),
				copy.volumeCount);
			std::printf("    lights: %u, %zu of them with a colour\n", copy.lightCount, lit);
			std::size_t baked = 0;
			for (const LightingCopy::ShadowTree& tree : copy.shadowTrees) {
				baked += Get<std::uint32_t>(bytes, tree.at + kShadowTreeNodeCount) != 1;
			}
			std::printf("    baked shadow trees: %zu stored, %zu of them baked and %zu empty, under %zu records\n", copy.shadowTrees.size(),
				baked, copy.shadowTrees.size() - baked, copy.shadowRecords.size());
			for (const LightingCopy::ShadowTree& tree : copy.shadowTrees) {
				// The stream key: this zone's own asset of the name the tree links (the link itself may go through a by-name
				// reference, in a map's zone).
				std::uint64_t key = tree.key, package = tree.package;
				std::uint32_t size = tree.size;
				const auto link = std::ranges::find_if(copy.links, [&](const LightingCopy::Link& l) { return l.at == tree.at; });
				for (std::size_t i = 0; !key && link != copy.links.end() && i < list.assets.size(); ++i) {
					if (list.assets[i].type == 0xB8 && names[i] == link->name && !byName[i] && trace.assets[i].offset + 56 <= stream.size()) {
						key = names[i];
						package = Get<std::uint64_t>(stream, trace.assets[i].offset + 8);
						size = Get<std::uint32_t>(stream, trace.assets[i].offset + 48);
					}
				}
				std::size_t records = 0;
				const LightingCopy::ShadowRecord* first = nullptr;
				for (const LightingCopy::ShadowRecord& record : copy.shadowRecords) {
					if (record.tree == tree.at) {
						++records;
						first = first ? first : &record;
					}
				}
				const std::size_t body = first ? first->body : 0;
				std::printf("      %-13s +0x%-7zX %8u nodes, %zu record(s): %g x %g tiles of %g a texel, baked for yaw %g pitch %g; "
					"key %016llX names %016llX (%u B)%s\n", first && first->region ? "shadow region" : "sun shadow", tree.at,
					Get<std::uint32_t>(bytes, tree.at + kShadowTreeNodeCount), records, Get<float>(bytes, body + kShadowRecordTiles),
					Get<float>(bytes, body + kShadowRecordTiles + 4), Get<float>(bytes, body + kShadowRecordTiles + 8),
					Get<float>(bytes, body + kShadowRecordTiles + 16), Get<float>(bytes, body + kShadowRecordTiles + 20),
					static_cast<unsigned long long>(key ? key : link != copy.links.end() ? link->name : 0),
					static_cast<unsigned long long>(package), size, key ? "" : " (the key is another zone's)");
			}
		}
	}

	// The level's other world assets (level_assets.hpp), read with their recording readers: what each links to, where
	// its pointers go, and whether a copy can carry them (asset_record.hpp). mapkit plan P6 step 2.
	bool PrintLevelAssets(const LoadedZone& zone, const XAssetList& list, const fs::path& tracePath) {
		ZoneTrace trace;
		if (!TraceOrWalk(zone, list, tracePath, trace)) {
			return false;
		}
		const std::size_t count = list.assets.size();
		std::vector<std::uint64_t> names(count);
		std::vector<bool> byName(count);
		for (std::size_t i = 0; i < count; ++i) {
			names[i] = TracedAssetName(trace, zone.stream, i);
			const std::size_t at = trace.assets[i].offset + XAssetNameOffset(list.assets[i].type);
			byName[i] = list.assets[i].header == kPtrInline && at + 8 <= zone.stream.size()
				&& (Get<std::uint64_t>(std::span<const std::uint8_t>(zone.stream).subspan(at, 8), 0) >> 63);
		}
		// The XAsset array is the last thing block 4 holds before the first asset.
		const std::uint64_t arrayPos = trace.assets[0].pos[XBlockVirtual] - 16 * count;
		std::printf("\nlevel world assets (mapkit plan P6 step 2: what a map's own copies must carry)\n");
		for (const std::uint64_t type : { 0xB1ull, 0x7Full, 0x43ull, 0x19ull, 0xA9ull, 0x1Aull, 0x76ull }) {
			const RecordReader read = LevelAssetReader(type);
			for (std::size_t index = 0; index < count; ++index) {
				if (list.assets[index].type != type) {
					continue;
				}
				const TracedAsset& start = trace.assets[index];
				XStream s(zone.stream, 0);
				s.Restore(start.offset, start.block, start.pos);
				AssetRecord record;
				const bool ok = read(s, list.assets[index].header, record);
				const std::size_t next = index + 1 < trace.assets.size() ? trace.assets[index + 1].offset : zone.stream.size();
				std::printf("\n  %s #%016llX, asset %zu at +0x%zX: %zu bytes, decoded %s\n", std::string(XAssetTypeName(type)).c_str(),
					static_cast<unsigned long long>(names[index]), index, start.offset, next - start.offset,
					!ok ? ("FAILED: " + s.Error()).c_str() : s.Cursor() == next ? "exactly" : "short of the next asset");
				if (!ok) {
					continue;
				}
				auto innermost = [&](std::size_t at) -> std::string {
					const RecordSubtree* best = nullptr;
					for (const RecordSubtree& subtree : record.subtrees) {
						if (at >= subtree.begin && at < subtree.end && (!best || subtree.end - subtree.begin < best->end - best->begin)) {
							best = &subtree;
						}
					}
					return best ? best->name : "root";
				};
				std::map<std::string, std::size_t> own, other, internal, external;
				for (const RecordRef& ref : record.refs) {
					const RecordTarget target = ResolveRecordRef(record, ref, arrayPos, count);
					if (target.kind == RecordTarget::Asset) {
						const std::string what = std::format("{} from {}", XAssetTypeName(list.assets[target.asset].type),
							innermost(ref.at));
						++(byName[target.asset] ? other : own)[what];
					}
					else if (target.kind == RecordTarget::Internal) {
						++internal[innermost(ref.at) + " -> " + innermost(record.chunks[target.chunk].at)];
					}
					else {
						++external[innermost(ref.at)];
					}
				}
				for (const auto& [what, n] : own) {
					std::printf("    links to %s's own:      %6zu %s\n", zone.name.c_str(), n, what.c_str());
				}
				for (const auto& [what, n] : other) {
					std::printf("    links by name elsewhere: %6zu %s\n", n, what.c_str());
				}
				for (const auto& [what, n] : internal) {
					std::printf("    into itself:             %6zu %s\n", n, what.c_str());
				}
				for (const auto& [what, n] : external) {
					std::printf("    into other assets' data: %6zu from %s\n", n, what.c_str());
				}
				std::size_t inlined = 0;
				std::map<std::string, std::size_t> inlineBy;
				for (const std::size_t field : record.inlineAssetFields) {
					++inlineBy[field == kNoStream ? std::string("?") : innermost(field)];
					++inlined;
				}
				for (const auto& [what, n] : inlineBy) {
					std::printf("    nested asset inline:     %6zu in %s\n", n, what.c_str());
				}
				std::printf("    %zu chunks, %zu sub-arrays, %zu script strings, %zu references, %zu nested assets inline\n",
					record.chunks.size(), record.subtrees.size(), record.strings.size(), record.refs.size(), inlined);
				if (const RecordSubtree* lights = type == 0x19 ? record.Subtree("lights") : nullptr) {
					// The lights the client game copies when a level starts (world_assets.hpp, kLightSize).
					const std::span<const std::uint8_t> stream(zone.stream);
					const std::size_t n = Get<std::uint32_t>(stream, record.rootAt + 12);
					std::size_t lit = 0;
					for (std::size_t i = 0; i < n && lights->begin + kLightSize * (i + 1) <= lights->end; ++i) {
						bool colour = false;
						for (const std::size_t field : { kLightColor, kLightColorAgain }) {
							for (std::size_t c = 0; c < 3; ++c) {
								colour |= Get<float>(stream, lights->begin + kLightSize * i + field + 4 * c) != 0.0f;
							}
						}
						lit += colour;
					}
					std::printf("    lights: %zu, %zu of them with a colour\n", n, lit);
				}
				// Whether the copy mapkit writes can carry it: whole, or with the planned cuts.
				RecordSplice splice;
				splice.stream = zone.stream;
				splice.record = &record;
				splice.assetArrayPos = arrayPos;
				splice.assets = list.assets;
				splice.assetNames = names;
				splice.strings = list.strings;
				const std::vector<RecordCut> planned = type == 0x7F ? EmptyStaticLevelFxListCuts()
					: type == 0xB1 ? TerrainGfxWithoutTilesCuts() : std::vector<RecordCut>{};
				for (const bool cut : { false, true }) {
					if (cut && planned.empty()) {
						continue;
					}
					splice.cuts = cut ? planned : std::vector<RecordCut>{};
					std::erase_if(splice.cuts, [&](const RecordCut& c) { return !record.Subtree(c.name); });
					std::vector<RecordLink> links;
					std::vector<std::string> strings;
					std::string why;
					const bool copyable = RecordLinks(splice, links, strings, why);
					std::size_t ownLinks = 0;
					for (const RecordLink& link : links) {
						for (std::size_t i = 0; i < count; ++i) {
							if (list.assets[i].type == link.type && names[i] == link.name) {
								ownLinks += !byName[i];
								break;
							}
						}
					}
					std::printf("    copy%s: %s", cut ? " with the planned cuts" : " whole",
						copyable ? std::format("yes, {} distinct links ({} to {}'s own assets), {} script strings", links.size(),
							ownLinks, zone.name, strings.size()).c_str() : ("no, " + why).c_str());
					std::printf("\n");
				}
			}
		}
		PrintLighting(zone, list, trace, names, byName);
		return true;
	}

	// The sub-array of a recorded asset that holds a stream offset, the innermost one ("root" for none).
	std::string InnermostSubtree(const AssetRecord& record, std::size_t at) {
		const RecordSubtree* best = nullptr;
		for (const RecordSubtree& subtree : record.subtrees) {
			if (at >= subtree.begin && at < subtree.end && (!best || subtree.end - subtree.begin < best->end - best->begin)) {
				best = &subtree;
			}
		}
		return best ? best->name : "root";
	}

	// The asset library's assets a map's copies link (library_assets.hpp): every one the zone holds itself, read with its
	// copy reader. Each must end where the next asset starts; then RecordLinks (asset_record.hpp) says whether a copy can
	// carry it whole, and what stops the rest. mapkit plan P6 step 2b.
	bool PrintLibraryAssets(const LoadedZone& zone, const XAssetList& list, const fs::path& tracePath) {
		ZoneTrace trace;
		std::string error;
		if (!ReadZoneTrace(tracePath, trace, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		if (const std::string mismatch = AlignZoneTrace(trace, zone.stream, list); !mismatch.empty()) {
			std::fprintf(stderr, "the trace does not fit: %s\n", mismatch.c_str());
			return false;
		}
		const std::size_t count = list.assets.size();
		std::vector<std::uint64_t> names(count);
		std::vector<bool> byName(count);
		for (std::size_t i = 0; i < count; ++i) {
			names[i] = TracedAssetName(trace, zone.stream, i);
			const std::size_t at = trace.assets[i].offset + XAssetNameOffset(list.assets[i].type);
			byName[i] = list.assets[i].header == kPtrInline && at + 8 <= zone.stream.size()
				&& (Get<std::uint64_t>(std::span<const std::uint8_t>(zone.stream).subspan(at, 8), 0) >> 63);
		}
		const std::uint64_t arrayPos = trace.assets[0].pos[XBlockVirtual] - 16 * count;
		// Error texts name offsets and counts: one line per kind of failure.
		auto general = [](const std::string& text) {
			return std::regex_replace(text, std::regex("(\\+?0x[0-9A-Fa-f]+)|(\\b[0-9]+\\b)"), "#");
		};
		std::printf("\nlibrary assets (mapkit plan P6 step 2b: what a map's own copies of its library's assets must carry)\n");
		for (const std::uint64_t type : { 0x10ull, 0xB8ull, 0x08ull, 0x09ull, 0x06ull, 0x0Aull, 0x35ull, 0xD3ull, 0x12ull, 0x13ull, 0x14ull,
				0x15ull, 0x16ull }) {
			const RecordReader read = LibraryAssetReader(type);
			std::size_t own = 0, exact = 0, copyable = 0, withOwners = 0, strings = 0, links = 0;
			std::uint64_t ownBytes = 0, copyableBytes = 0;
			std::map<std::string, std::size_t> failed, blocked, ownerKinds;
			std::map<std::string, std::pair<std::size_t, std::string>> examples; // the first asset of each kind, and its error
			for (std::size_t index = 0; index < count; ++index) {
				const std::uint64_t header = list.assets[index].header;
				if (list.assets[index].type != type || byName[index] || (header != kPtrInline && header != kPtrInsert)) {
					continue;
				}
				++own;
				const TracedAsset& start = trace.assets[index];
				const std::size_t next = index + 1 < trace.assets.size() ? trace.assets[index + 1].offset : zone.stream.size();
				ownBytes += next - start.offset;
				XStream s(zone.stream, 0);
				s.Restore(start.offset, start.block, start.pos);
				AssetRecord record;
				std::string why;
				if (!read(s, header, record)) {
					why = s.Error();
				}
				else if (s.Cursor() != next) {
					why = s.Cursor() < next ? "ends short of the next asset" : "runs into the next asset";
				}
				if (!why.empty()) {
					const std::string key = general(why);
					++failed[key];
					examples.try_emplace("failed: " + key, index, why);
					continue;
				}
				++exact;
				RecordSplice splice;
				splice.stream = zone.stream;
				splice.record = &record;
				splice.assetArrayPos = arrayPos;
				splice.assets = list.assets;
				splice.assetNames = names;
				splice.strings = list.strings;
				std::vector<RecordLink> recordLinks;
				std::vector<std::string> recordStrings;
				std::vector<std::uint64_t> shared;
				std::string blockedWhy;
				if (RecordLinks(splice, recordLinks, recordStrings, blockedWhy, &shared)) {
					strings += recordStrings.size();
					links += recordLinks.size();
					if (shared.empty()) {
						++copyable;
						copyableBytes += next - start.offset;
						continue;
					}
					// Copyable once the assets whose data it shares are copied ahead of it (RecordSplice::shared).
					++withOwners;
					std::set<std::size_t> owners;
					for (const std::uint64_t stored : shared) {
						owners.insert(TracedAssetOfData(trace, stored));
					}
					for (const std::size_t owner : owners) {
						++ownerKinds[owner == SIZE_MAX ? std::string("nothing traced")
							: std::string(XAssetTypeName(list.assets[owner].type)) + (LibraryAssetReader(list.assets[owner].type)
								? "" : " (no copy reader)")];
					}
					continue;
				}
				std::string key = record.inlineAssets ? "a nested asset stored inline" : general(blockedWhy);
				++blocked[key];
				examples.try_emplace("blocked: " + key, index, blockedWhy);
			}
			std::printf("\n  %-11s %6zu of its own (%s): %zu decode exactly; a copy carries %zu whole (%s), %zu more with what "
				"their data is shared with (%zu links, %zu script strings in all)\n", std::string(XAssetTypeName(type)).c_str(), own,
				Megabytes(ownBytes).c_str(), exact, copyable, Megabytes(copyableBytes).c_str(), withOwners, links, strings);
			for (const auto& [what, n] : ownerKinds) {
				std::printf("    shares  %6zu x data of a %s\n", n, what.c_str());
			}
			for (const auto& [what, n] : failed) {
				const auto& [asset, text] = examples["failed: " + what];
				std::printf("    FAILED  %6zu %s\n            first: asset %zu, %s\n", n, what.c_str(), asset, text.c_str());
			}
			for (const auto& [what, n] : blocked) {
				const auto& [asset, text] = examples["blocked: " + what];
				std::printf("    cannot  %6zu %s\n            first: asset %zu, %s\n", n, what.c_str(), asset, text.c_str());
			}
		}
		return true;
	}

	// The copies a map's zone holds of this zone's assets (cwlink build, mapkit plan P6 step 2b), each read back from the
	// map's zone and from this one with the same copy reader (library_assets.hpp). Their bytes must match except where a
	// pointer or a script string was re-pointed, and each of those must say the same thing: a link names the same asset (an
	// entry of the map's zone with this one's type and name), a string has the same text, a pointer into the asset itself
	// lands on the same piece of data and offset, and one into data shared with another asset on the same piece and offset
	// of that asset's copy.
	bool PrintCopyCheck(const LoadedZone& zone, const XAssetList& list, const fs::path& tracePath, const fs::path& mapPath) {
		ZoneTrace trace;
		std::string error;
		if (!ReadZoneTrace(tracePath, trace, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		if (const std::string mismatch = AlignZoneTrace(trace, zone.stream, list); !mismatch.empty()) {
			std::fprintf(stderr, "the trace does not fit: %s\n", mismatch.c_str());
			return false;
		}
		LoadedZone mapZone;
		XAssetList mapList;
		if (!LoadZone(mapPath, mapZone, error) || !ParseXAssetList(mapZone.stream, mapList, error)) {
			std::fprintf(stderr, "%s: %s\n", mapPath.string().c_str(), error.c_str());
			return false;
		}
		const WalkResult walk = WalkStream(mapZone.stream);
		if (!walk.complete || walk.assets.size() != mapList.assets.size()) {
			std::fprintf(stderr, "%s does not walk: %s\n", mapPath.string().c_str(), walk.error.c_str());
			return false;
		}
		using Key = std::pair<std::uint64_t, std::uint64_t>;
		// Each zone's names, which of its entries are by-name references, and where its XAsset array is (block 4, right
		// before the first asset).
		struct Side {
			std::span<const std::uint8_t> stream;
			const XAssetList* list = nullptr;
			std::vector<std::uint64_t> names;
			std::vector<bool> byName;
			std::uint64_t arrayPos = 0;
		};
		auto sideOf = [](std::span<const std::uint8_t> stream, const XAssetList& assets, auto&& startOf, std::uint64_t arrayPos) {
			Side side{ stream, &assets, std::vector<std::uint64_t>(assets.assets.size()), std::vector<bool>(assets.assets.size()), arrayPos };
			for (std::size_t i = 0; i < assets.assets.size(); ++i) {
				const std::size_t at = startOf(i) + XAssetNameOffset(assets.assets[i].type);
				const std::uint64_t raw = at + 8 <= stream.size() ? Get<std::uint64_t>(stream.subspan(at, 8), 0) : 0;
				side.names[i] = raw & ~(1ull << 63);
				side.byName[i] = assets.assets[i].header == kPtrInline && (raw >> 63);
			}
			return side;
		};
		const Side base = sideOf(zone.stream, list, [&](std::size_t i) { return trace.assets[i].offset; },
			trace.assets[0].pos[XBlockVirtual] - 16 * list.assets.size());
		const Side copy = sideOf(mapZone.stream, mapList, [&](std::size_t i) { return walk.assets[i].begin; },
			walk.bodyPositions[XBlockVirtual] - 16 * mapList.assets.size());
		auto readBase = [&](std::size_t i, AssetRecord& out) {
			XStream s(zone.stream, 0);
			s.Restore(trace.assets[i].offset, trace.assets[i].block, trace.assets[i].pos);
			return LibraryAssetReader(list.assets[i].type)(s, list.assets[i].header, out);
		};
		auto readCopy = [&](std::size_t i, AssetRecord& out) {
			XStream s(mapZone.stream, 0);
			s.Restore(walk.assets[i].begin, XBlockVirtual, walk.assets[i].pos);
			return LibraryAssetReader(mapList.assets[i].type)(s, mapList.assets[i].header, out);
		};
		// The asset of the map's zone whose data a stored pointer points into (as TracedAssetOfData for a trace).
		auto copyOwner = [&](std::uint64_t stored) {
			const std::uint64_t value = stored - 1;
			const int block = static_cast<int>(value >> 60);
			const std::uint64_t pos = value & 0x0FFFFFFFFFFFFFFFull;
			const auto after = std::upper_bound(walk.assets.begin(), walk.assets.end(), pos,
				[block](std::uint64_t p, const WalkedAsset& asset) { return p < asset.pos[block]; });
			return after == walk.assets.begin() ? SIZE_MAX : static_cast<std::size_t>(after - walk.assets.begin() - 1);
		};
		// Where a stream byte or a stored pointer sits in a record: {piece, offset}.
		auto pieceAt = [](const AssetRecord& record, std::size_t at) -> std::optional<std::pair<std::size_t, std::size_t>> {
			for (std::size_t k = 0; k < record.chunks.size(); ++k) {
				const RecordChunk& chunk = record.chunks[k];
				if (chunk.at != kNoStream && at >= chunk.at && at < chunk.at + chunk.size) {
					return std::pair{ k, at - chunk.at };
				}
			}
			return std::nullopt;
		};
		auto pieceOf = [](const AssetRecord& record, std::uint64_t stored) -> std::optional<std::pair<std::size_t, std::uint64_t>> {
			const std::uint64_t value = stored - 1;
			const int block = static_cast<int>(value >> 60);
			const std::uint64_t pos = value & 0x0FFFFFFFFFFFFFFFull;
			for (std::size_t k = 0; k < record.chunks.size(); ++k) {
				const RecordChunk& chunk = record.chunks[k];
				if (chunk.block == block && pos >= chunk.pos && pos < chunk.pos + std::max<std::uint64_t>(chunk.size, 1)) {
					return std::pair{ k, pos - chunk.pos };
				}
			}
			return std::nullopt;
		};

		std::map<Key, std::size_t> own;
		// What each of this zone's stream keys names in the packages: {its type (u8 @54), the data's key (+8), its size (+48)}.
		std::set<std::tuple<std::uint8_t, std::uint64_t, std::uint32_t>> keyData;
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			const std::uint64_t header = list.assets[i].header;
			if ((header == kPtrInline || header == kPtrInsert) && !base.byName[i] && LibraryAssetReader(list.assets[i].type)) {
				own.emplace(Key{ list.assets[i].type, base.names[i] }, i);
				if (list.assets[i].type == 0xB8 && trace.assets[i].offset + 56 <= zone.stream.size()) {
					const std::span<const std::uint8_t> root = std::span<const std::uint8_t>(zone.stream).subspan(trace.assets[i].offset, 56);
					keyData.emplace(root[54], Get<std::uint64_t>(root, 8), Get<std::uint32_t>(root, 48));
				}
			}
		}
		std::size_t checked = 0, links = 0, strings = 0, internal = 0, shared = 0, renamedData = 0;
		std::uint64_t bytes = 0;
		std::vector<std::string> problems;
		auto problem = [&](std::size_t mapIndex, const std::string& what) {
			problems.push_back(std::format("{} #{:016X} (asset {} of the map's zone): {}", XAssetTypeName(mapList.assets[mapIndex].type),
				copy.names[mapIndex], mapIndex, what));
		};
		for (std::size_t m = 0; m < mapList.assets.size(); ++m) {
			if (copy.byName[m] || !LibraryAssetReader(mapList.assets[m].type)) {
				continue;
			}
			const auto found = own.find({ mapList.assets[m].type, copy.names[m] });
			if (found == own.end()) {
				continue; // the map's own (a texture, its sky image), not a copy
			}
			const std::size_t b = found->second;
			++checked;
			AssetRecord original, copied;
			if (!readBase(b, original) || !readCopy(m, copied)) {
				problem(m, "does not read");
				continue;
			}
			// A stream key may name other data than its original: the key of a shadow tree that cwlink emptied names the
			// empty tree's data (zonekit LightingCopy::EmptyShadowTrees). Such a copy is its original but for its +8 (the
			// data's key in the packages) and its +48 (u32, that data's size), and a key of this zone, of its type, names
			// that data already.
			if (mapList.assets[m].type == 0xB8 && original.rootAt != kNoStream && copied.rootAt != kNoStream) {
				const std::span<const std::uint8_t> from = std::span<const std::uint8_t>(zone.stream).subspan(original.rootAt, 56);
				const std::span<const std::uint8_t> to = std::span<const std::uint8_t>(mapZone.stream).subspan(copied.rootAt, 56);
				if (Get<std::uint64_t>(from, 8) != Get<std::uint64_t>(to, 8) || Get<std::uint32_t>(from, 48) != Get<std::uint32_t>(to, 48)) {
					const bool restSame = std::equal(from.begin(), from.begin() + 8, to.begin())
						&& std::equal(from.begin() + 16, from.begin() + 48, to.begin() + 16)
						&& std::equal(from.begin() + 52, from.end(), to.begin() + 52);
					if (restSame && keyData.contains({ to[54], Get<std::uint64_t>(to, 8), Get<std::uint32_t>(to, 48) })) {
						++renamedData;
					}
					else {
						problem(m, "names other data in the packages than its original, which no key of this zone names");
					}
					continue;
				}
			}
			if (original.chunks.size() != copied.chunks.size()) {
				problem(m, std::format("loads {} pieces of data, the original {}", copied.chunks.size(), original.chunks.size()));
				continue;
			}
			bool same = true;
			for (std::size_t k = 0; k < original.chunks.size() && same; ++k) {
				same = original.chunks[k].block == copied.chunks[k].block && original.chunks[k].size == copied.chunks[k].size;
			}
			if (!same) {
				problem(m, "a piece of data differs in block or size");
				continue;
			}
			// What may differ: every recorded pointer (8 B) and script string (4 B), by piece and offset.
			std::set<std::pair<std::size_t, std::size_t>> repointed;
			for (const RecordRef& ref : original.refs) {
				if (const auto at = pieceAt(original, ref.at)) {
					for (std::size_t j = 0; j < 8; ++j) {
						repointed.insert({ at->first, at->second + j });
					}
				}
			}
			for (const RecordString& string : original.strings) {
				if (const auto at = pieceAt(original, string.at)) {
					for (std::size_t j = 0; j < 4; ++j) {
						repointed.insert({ at->first, at->second + j });
					}
				}
			}
			std::size_t differing = 0;
			for (std::size_t k = 0; k < original.chunks.size(); ++k) {
				const RecordChunk& from = original.chunks[k];
				const RecordChunk& to = copied.chunks[k];
				if (from.at == kNoStream || to.at == kNoStream) {
					continue;
				}
				bytes += from.size;
				for (std::size_t j = 0; j < from.size; ++j) {
					differing += zone.stream[from.at + j] != mapZone.stream[to.at + j] && !repointed.contains({ k, j });
				}
			}
			if (differing) {
				problem(m, std::format("{} bytes differ outside its pointers and strings", differing));
				continue;
			}
			// Each pointer, side by side.
			std::map<std::pair<std::size_t, std::size_t>, const RecordRef*> copiedRefs;
			for (const RecordRef& ref : copied.refs) {
				if (const auto at = pieceAt(copied, ref.at)) {
					copiedRefs[*at] = &ref;
				}
			}
			for (const RecordRef& ref : original.refs) {
				const auto at = pieceAt(original, ref.at);
				const auto other = at ? copiedRefs.find(*at) : copiedRefs.end();
				if (other == copiedRefs.end()) {
					problem(m, std::format("the pointer at original stream +0x{:X} has no counterpart", ref.at));
					break;
				}
				const RecordTarget a = ResolveRecordRef(original, ref, base.arrayPos, list.assets.size());
				const RecordTarget c = ResolveRecordRef(copied, *other->second, copy.arrayPos, mapList.assets.size());
				std::string wrong;
				if (a.kind == RecordTarget::Asset) {
					++links;
					if (c.kind != RecordTarget::Asset || list.assets[a.asset].type != mapList.assets[c.asset].type
						|| base.names[a.asset] != copy.names[c.asset]) {
						wrong = "links another asset";
					}
				}
				else if (a.kind == RecordTarget::Internal) {
					++internal;
					if (c.kind != RecordTarget::Internal || a.chunk != c.chunk || a.offset != c.offset) {
						wrong = "points elsewhere in itself";
					}
				}
				else {
					++shared;
					const std::size_t baseOwner = TracedAssetOfData(trace, ref.stored);
					const std::size_t copyOwner_ = c.kind == RecordTarget::External ? copyOwner(other->second->stored) : SIZE_MAX;
					AssetRecord ownerOriginal, ownerCopy;
					if (baseOwner == SIZE_MAX || copyOwner_ == SIZE_MAX || list.assets[baseOwner].type != mapList.assets[copyOwner_].type
						|| base.names[baseOwner] != copy.names[copyOwner_] || !readBase(baseOwner, ownerOriginal)
						|| !readCopy(copyOwner_, ownerCopy)) {
						wrong = "shares data with another asset";
					}
					else if (pieceOf(ownerOriginal, ref.stored) != pieceOf(ownerCopy, other->second->stored)
						|| !pieceOf(ownerOriginal, ref.stored)) {
						wrong = "shares other data of that asset";
					}
				}
				if (!wrong.empty()) {
					problem(m, std::format("the pointer at original stream +0x{:X} {}", ref.at, wrong));
					break;
				}
			}
			// Each script string, side by side.
			std::map<std::pair<std::size_t, std::size_t>, std::uint32_t> copiedStrings;
			for (const RecordString& string : copied.strings) {
				if (const auto at = pieceAt(copied, string.at)) {
					copiedStrings[*at] = string.index;
				}
			}
			for (const RecordString& string : original.strings) {
				const auto at = pieceAt(original, string.at);
				const auto other = at ? copiedStrings.find(*at) : copiedStrings.end();
				auto text = [](const XAssetList& assets, std::uint32_t index) {
					return index < assets.strings.size() && assets.strings[index] ? *assets.strings[index] : std::string();
				};
				++strings;
				if (other == copiedStrings.end() || text(list, string.index) != text(mapList, other->second)) {
					problem(m, std::format("the script string at original stream +0x{:X} reads differently", string.at));
					break;
				}
			}
		}
		std::printf("\ncopies of %s's assets in %s (mapkit plan P6 step 2b)\n  %zu checked (%s): %zu links, %zu pointers into "
			"themselves, %zu into shared data, %zu script strings; %zu problems\n", zone.name.c_str(), mapPath.filename().string().c_str(),
			checked, Megabytes(bytes).c_str(), links, internal, shared, strings, problems.size());
		if (renamedData) {
			std::printf("  %zu of them are stream keys that name another key's data in the packages (an emptied shadow tree's: the empty "
				"tree's)\n", renamedData);
		}
		for (std::size_t i = 0; i < problems.size() && i < 20; ++i) {
			std::printf("    %s\n", problems[i].c_str());
		}
		return problems.empty();
	}

	bool PrintClipMap(const LoadedZone& zone, const XAssetList& list, const fs::path& tracePath) {
		ZoneTrace trace;
		std::string error;
		if (!ReadZoneTrace(tracePath, trace, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		if (const std::string mismatch = AlignZoneTrace(trace, zone.stream, list); !mismatch.empty()) {
			std::fprintf(stderr, "the trace does not fit: %s\n", mismatch.c_str());
			return false;
		}
		std::size_t index = list.assets.size();
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			if (list.assets[i].type == 0x18) {
				index = i;
				break;
			}
		}
		if (index == list.assets.size()) {
			std::printf("\nclip map         none in this zone\n");
			return true;
		}
		const TracedAsset& start = trace.assets[index];
		XStream s(zone.stream, 0);
		s.Restore(start.offset, start.block, start.pos);
		ClipMap map;
		const bool ok = ReadClipMap(s, list.assets[index].header, map);
		const std::size_t next = index + 1 < trace.assets.size() ? trace.assets[index + 1].offset : zone.stream.size();
		std::printf("\nclip map         asset %zu at +0x%zX, decoded to +0x%zX (next asset at +0x%zX)%s\n", index, start.offset,
			s.Cursor(), next, ok && s.Cursor() == next ? ": exact" : "");
		if (!ok) {
			std::printf("  FAILED: %s\n", s.Error().c_str());
		}
		for (const auto& [name, bytes] : map.parts) {
			std::printf("  %-28s %10zu bytes\n", name.c_str(), bytes);
		}
		std::printf("  root counts      cells %u, +428 %u, +432 %u, trees %u, xcollisions %u, +440 %u, +444 %u, dynents %u/%u, "
			"+448 %d, +452 %d\n",
			Get<std::uint16_t>(map.root, 462), Get<std::uint32_t>(map.root, 428), Get<std::uint32_t>(map.root, 432),
			Get<std::uint32_t>(map.root, 436), Get<std::uint32_t>(map.root, 424), Get<std::uint32_t>(map.root, 440),
			Get<std::uint32_t>(map.root, 444), Get<std::uint16_t>(map.root, 88), Get<std::uint16_t>(map.root, 90),
			Get<std::int32_t>(map.root, 448), Get<std::int32_t>(map.root, 452));
		std::printf("  root +0 name %016llX, +456 %08X, +460 %04X, +464..471 %016llX\n",
			static_cast<unsigned long long>(Get<std::uint64_t>(map.root, 0)), Get<std::uint32_t>(map.root, 456),
			Get<std::uint16_t>(map.root, 460), static_cast<unsigned long long>(Get<std::uint64_t>(map.root, 464)));
		{
			// The +96 records: the map's own dynents first (a def at +0), then the empty slots the game spawns
			// debris into at runtime (DynEnt_Create_cand 0x7FF72406D7A0: slot + @460).
			std::size_t withDef = 0, firstEmpty = map.dynEnts.size(), dirtyEmpty = 0;
			for (std::size_t i = 0; i < map.dynEnts.size(); ++i) {
				const auto& record = map.dynEnts[i];
				if (Get<std::uint64_t>(record, 0) != kPtrNull) {
					++withDef;
					continue;
				}
				firstEmpty = std::min(firstEmpty, i);
				if (std::ranges::any_of(record, [](std::uint8_t b) { return b != 0; })) {
					++dirtyEmpty;
				}
			}
			std::printf("  +96 dynents      %zu records: %zu with a def, first without one at %zu, %zu of those not all zero\n",
				map.dynEnts.size(), withDef, firstEmpty, dirtyEmpty);
			if (firstEmpty < map.dynEnts.size()) {
				const auto& first = map.dynEnts[firstEmpty];
				std::size_t same = 0;
				for (std::size_t i = firstEmpty; i < map.dynEnts.size(); ++i) {
					same += map.dynEnts[i] == first;
				}
				std::printf("    empty slot %zu (%zu of the empty ones identical to it):", firstEmpty, same);
				for (std::size_t at = 0; at < 96; at += 4) {
					if (const auto v = Get<std::uint32_t>(first, at)) {
						std::printf(" +%zu=%08X", at, v);
					}
				}
				std::printf("\n    own dynent 0:");
				for (std::size_t at = 0; at < 96; at += 4) {
					if (const auto v = Get<std::uint32_t>(map.dynEnts[0], at)) {
						std::printf(" +%zu=%08X", at, v);
					}
				}
				std::printf("\n");
			}
		}

		auto range = [](const std::vector<std::uint8_t>& data, std::size_t width) {
			std::uint32_t lo = UINT32_MAX, hi = 0;
			for (std::size_t at = 0; at + width <= data.size(); at += width) {
				const std::uint32_t v = width == 4 ? Get<std::uint32_t>(data, at) : Get<std::uint16_t>(data, at);
				lo = std::min(lo, v);
				hi = std::max(hi, v);
			}
			return data.empty() ? std::string("none") : std::format("{} values {}..{}", data.size() / width, lo, hi);
		};
		std::printf("  +16 cell index   %s\n  +72 (u32)        %s\n  +80 (u16)        %s\n", range(map.cellIndex, 4).c_str(),
			range(map.array72, 4).c_str(), range(map.array80, 2).c_str());
		if (map.hasTreeIndex) {
			std::printf("  +56 tree index   u32 @24 %u, @28 %u; arrays %zu/%zu/%zu B; +0 as u32 %s\n",
				Get<std::uint32_t>(map.treeIndex, 24), Get<std::uint32_t>(map.treeIndex, 28), map.treeIndexArrays[0].size(),
				map.treeIndexArrays[1].size(), map.treeIndexArrays[2].size(), range(map.treeIndexArrays[0], 4).c_str());
		}

		auto floats = [](std::span<const std::uint8_t> raw, std::size_t from, std::size_t count) {
			std::string text;
			for (std::size_t i = 0; i < count; ++i) {
				text += std::format("{}{:.3g}", i ? " " : "", Get<float>(raw, from + 4 * i));
			}
			return text;
		};
		auto words = [](std::span<const std::uint8_t> raw, std::size_t from, std::size_t to) {
			std::string text;
			for (std::size_t at = from; at + 4 <= to; at += 4) {
				text += std::format("{}{:08X}", at == from ? "" : " ", Get<std::uint32_t>(raw, at));
			}
			return text;
		};

		for (std::size_t c = 0; c < map.cells.size(); ++c) {
			const ClipMapCell& cell = map.cells[c];
			std::printf("  cell %2zu  %s | blob %zu B, %zu records\n", c, words(cell.raw, 0, 64).c_str(), cell.blob.size(),
				cell.records.size());
			for (std::size_t r = 0; r < std::min<std::size_t>(2, cell.records.size()); ++r) {
				std::printf("    record %zu   %s\n               floats +8: %s\n", r, words(cell.records[r], 0, 80).c_str(),
					floats(cell.records[r], 8, 15).c_str());
			}
		}

		if (map.hasWorld) {
			std::printf("  world (+160)     %s\n", words(map.worldHead, 0, 72).c_str());
			std::size_t recordCount = 0, indexBytes = 0, blobBytes = 0, listCount = 0;
			for (const ClipMapWorldEntry& entry : map.worldEntries) {
				recordCount += entry.records.size();
				listCount += entry.list.size();
				for (const ClipMapWorldRecord& record : entry.records) {
					indexBytes += record.indices.size();
					blobBytes += record.blob.size();
				}
			}
			std::printf("  world entries    %zu, records %zu (indices %zu B, blobs %zu B), list items %zu\n", map.worldEntries.size(),
				recordCount, indexBytes, blobBytes, listCount);
			for (std::size_t e = 0; e < std::min<std::size_t>(3, map.worldEntries.size()); ++e) {
				const ClipMapWorldEntry& entry = map.worldEntries[e];
				std::printf("  world entry %zu  %s\n                 floats +32: %s\n", e, words(entry.raw, 0, 176).c_str(),
					floats(entry.raw, 32, 12).c_str());
				for (std::size_t r = 0; r < std::min<std::size_t>(3, entry.records.size()); ++r) {
					const ClipMapWorldRecord& record = entry.records[r];
					std::printf("    record %zu     %s\n                 floats +0: %s | indices %zu B, bits %zu B, blob %zu B\n", r,
						words(record.raw, 0, 80).c_str(), floats(record.raw, 0, 8).c_str(), record.indices.size(),
						record.bits.size(), record.blob.size());
					if (!record.blob.empty()) {
						std::printf("                 blob: %s\n", words(record.blob, 0, std::min<std::size_t>(96, record.blob.size())).c_str());
					}
				}
				for (std::size_t k = 0; k < std::min<std::size_t>(2, entry.list.size()); ++k) {
					std::printf("    list %zu       %s\n", k, words(entry.list[k], 0, entry.list[k].size()).c_str());
				}
				// Heights against the record's z range: which records are sloped, and the u16 range they use.
				std::size_t shown = 0;
				for (std::size_t r = 0; r < entry.records.size() && shown < 8; ++r) {
					const ClipMapWorldRecord& record = entry.records[r];
					const float z0 = Get<float>(record.raw, 8), z1 = Get<float>(record.raw, 20);
					if (record.indices.size() < 2 || z0 == z1) {
						continue;
					}
					std::uint16_t lo = 0xFFFF, hi = 0;
					for (std::size_t k = 0; k + 2 <= record.indices.size(); k += 2) {
						lo = std::min(lo, Get<std::uint16_t>(record.indices, k));
						hi = std::max(hi, Get<std::uint16_t>(record.indices, k));
					}
					std::printf("    heights %4zu  z %.3f..%.3f  u16 %u..%u  first %u %u %u %u\n", r, z0, z1, lo, hi,
						Get<std::uint16_t>(record.indices, 0), Get<std::uint16_t>(record.indices, 2),
						Get<std::uint16_t>(record.indices, 4), Get<std::uint16_t>(record.indices, 6));
					++shown;
				}
				std::map<std::uint16_t, std::size_t> flatHeights;
				for (const ClipMapWorldRecord& record : entry.records) {
					if (record.indices.size() >= 2 && Get<float>(record.raw, 8) == Get<float>(record.raw, 20)) {
						flatHeights[Get<std::uint16_t>(record.indices, 0)]++;
					}
				}
				for (const auto& [value, count] : flatHeights) {
					std::printf("    flat u16 %u: %zu records\n", value, count);
				}
				// +38 picks how the terrain trace finds a tile's quads (0 none, 1 all, else the +48 bits).
				std::map<std::uint16_t, std::array<std::size_t, 3>> quadModes; // [+48 null, inline, reference]
				for (const ClipMapWorldRecord& record : entry.records) {
					const std::uint64_t bits = Get<std::uint64_t>(record.raw, 48);
					quadModes[Get<std::uint16_t>(record.raw, 38)][bits == kPtrNull ? 0 : (bits == kPtrInline ? 1 : 2)]++;
				}
				for (const auto& [mode, kinds] : quadModes) {
					std::printf("    +38 = %u: %zu records (+48 null %zu, inline %zu, reference %zu)\n", mode,
						kinds[0] + kinds[1] + kinds[2], kinds[0], kinds[1], kinds[2]);
				}
			}
		}
		for (std::size_t m = 0; m < std::min<std::size_t>(2, map.models.size()); ++m) {
			std::printf("  992-B %zu        %s\n                 +896 %s\n", m, words(map.models[m], 0, 96).c_str(),
				words(map.models[m], 896, 992).c_str());
		}

		std::size_t totals[kCollisionTreeArrayCount] = {};
		for (const CollisionTree& tree : map.trees) {
			for (std::size_t a = 0; a < kCollisionTreeArrayCount; ++a) {
				totals[a] += tree.arrays[a].size();
			}
		}
		std::printf("  trees            %zu; bytes per array:", map.trees.size());
		for (std::size_t a = 0; a < kCollisionTreeArrayCount; ++a) {
			std::printf(" [%zu] %zu", a, totals[a]);
		}
		std::printf("\n");
		for (std::size_t t = 0; t < std::min<std::size_t>(4, map.trees.size()); ++t) {
			const CollisionTree& tree = map.trees[t];
			std::printf("  tree %zu root   %s\n", t, words(tree.raw, 0, 64).c_str());
			std::printf("               +160 %s\n", words(tree.raw, 160, 184).c_str());
			std::printf("               floats +0: %s\n", floats(tree.raw, 0, 6).c_str());
			const auto& vertices = tree.arrays[1];
			if (vertices.size() >= 12) {
				float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
				for (std::size_t v = 0; v + 12 <= vertices.size(); v += 12) {
					for (int k = 0; k < 3; ++k) {
						const float f = Get<float>(vertices, v + 4 * k);
						lo[k] = std::min(lo[k], f);
						hi[k] = std::max(hi[k], f);
					}
				}
				std::printf("               array 1 (12 B) as floats: %zu, bounds (%.1f %.1f %.1f)..(%.1f %.1f %.1f)\n",
					vertices.size() / 12, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
			}
			for (std::size_t a = 0; a < kCollisionTreeArrayCount; ++a) {
				if (!tree.arrays[a].empty()) {
					std::printf("               [%2zu] %7zu B: %s\n", a, tree.arrays[a].size(),
						words(tree.arrays[a], 0, std::min<std::size_t>(32, tree.arrays[a].size())).c_str());
				}
			}
		}
		return ok;
	}

	// The streamerworld, decoded from its traced start: its model list and the cells' records. Model
	// fields are references; the ones into the XAsset array (block 4, right before the first asset) name
	// the asset they point at.
	bool PrintWorld(const LoadedZone& zone, const XAssetList& list, const fs::path& tracePath, std::size_t listCell) {
		ZoneTrace trace;
		std::string error;
		if (!ReadZoneTrace(tracePath, trace, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		if (const std::string mismatch = AlignZoneTrace(trace, zone.stream, list); !mismatch.empty()) {
			std::fprintf(stderr, "the trace does not fit: %s\n", mismatch.c_str());
			return false;
		}
		std::size_t index = list.assets.size();
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			if (list.assets[i].type == 0xAC) {
				index = i;
				break;
			}
		}
		if (index == list.assets.size()) {
			std::printf("\nworld            no streamerworld in this zone\n");
			return true;
		}
		const TracedAsset& start = trace.assets[index];
		XStream s(zone.stream, 0);
		s.Restore(start.offset, start.block, start.pos);
		StreamerWorld world;
		if (!ReadStreamerWorld(s, list.assets[index].header, world)) {
			std::printf("\nworld            asset %zu FAILED: %s\n", index, s.Error().c_str());
			return false;
		}

		const std::uint64_t tableEnd = trace.assets[0].pos[XBlockVirtual];
		const std::uint64_t tableBegin = tableEnd - 16ull * list.assets.size();
		std::map<std::string, std::size_t> targets;
		auto resolve = [&](std::uint64_t stored) -> std::string {
			if (stored == kPtrInline || stored == kPtrInsert || stored == kPtrNull) {
				return stored == kPtrNull ? "null" : "inline";
			}
			const std::uint64_t value = stored - 1;
			const int block = static_cast<int>(value >> 60);
			const std::uint64_t offset = value & 0x0FFFFFFFFFFFFFFFull;
			if (block == XBlockVirtual && offset >= tableBegin && offset < tableEnd) {
				const std::size_t asset = static_cast<std::size_t>((offset - tableBegin) / 16);
				return std::format("asset {} ({}) +{}", asset, XAssetTypeName(list.assets[asset].type), (offset - tableBegin) % 16);
			}
			return std::format("block {} +0x{:X}", block, offset);
		};
		auto kind = [&](std::uint64_t stored) {
			const std::string r = resolve(stored);
			const auto at = r.find('(');
			return at == std::string::npos ? r.substr(0, r.find(' ')) : r.substr(at, r.find(')') - at + 1) + r.substr(r.find(')') + 1);
		};

		std::printf("\nworld            streamerworld asset %zu at +0x%zX, decoded to +0x%zX\n", index, start.offset, s.Cursor());
		std::printf("  root counts     ");
		for (const std::size_t field : { 16, 32, 48, 64, 80, 96, 112, 176, 192 }) {
			std::printf(" +%zu=%llu", field, static_cast<unsigned long long>(Get<std::uint64_t>(world.root, field)));
		}
		std::printf("\n  root refs        +208 %s, +216 %s\n", resolve(Get<std::uint64_t>(world.root, 208)).c_str(),
			resolve(Get<std::uint64_t>(world.root, 216)).c_str());

		for (std::uint64_t m : world.models) {
			++targets[kind(m)];
		}
		std::printf("  model list       %zu:", world.models.size());
		for (const auto& [k, n] : targets) {
			std::printf(" %zu x %s;", n, k.c_str());
		}
		std::printf("\n");
		for (std::size_t i = 0; i < std::min<std::size_t>(3, world.models.size()); ++i) {
			std::printf("      [%zu] %s\n", i, resolve(world.models[i]).c_str());
		}

		std::size_t cellModels = 0, entries = 0, records = 0, axisOk = 0;
		std::array<std::size_t, 8> perGroup{};
		float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
		float scaleLo = 1e30f, scaleHi = -1e30f;
		targets.clear();
		for (const StreamerWorldCell& cell : world.cells) {
			cellModels += cell.models.size();
			for (const auto& m : cell.models) {
				++targets[kind(Get<std::uint64_t>(m, 0))];
			}
			for (std::size_t g = 0; g < 8; ++g) {
				perGroup[g] += cell.groups[g].size();
				entries += cell.groups[g].size();
				for (const StreamerWorldGroupEntry& e : cell.groups[g]) {
					for (const auto& raw : e.records) {
						const auto f = Floats52(raw);
						++records;
						bool ok = true;
						for (int row = 0; row < 3; ++row) {
							const float x = f[3 + row * 3], y = f[4 + row * 3], z = f[5 + row * 3];
							ok &= std::abs(std::sqrt(x * x + y * y + z * z) - 1.0f) < 0.01f;
						}
						axisOk += ok;
						for (int k = 0; k < 3; ++k) {
							lo[k] = std::min(lo[k], f[k]);
							hi[k] = std::max(hi[k], f[k]);
						}
						scaleLo = std::min(scaleLo, f[12]);
						scaleHi = std::max(scaleHi, f[12]);
					}
				}
			}
		}
		std::printf("  cells            %zu, %zu cell models:", world.cells.size(), cellModels);
		for (const auto& [k, n] : targets) {
			std::printf(" %zu x %s;", n, k.c_str());
		}
		std::printf("\n  group entries    %zu (per group:", entries);
		for (std::size_t n : perGroup) {
			std::printf(" %zu", n);
		}
		std::printf("), 52-B records %zu\n", records);
		if (records) {
			std::printf("  as placements    %zu of %zu have three unit-length axis rows; origin x %.1f..%.1f y %.1f..%.1f "
				"z %.1f..%.1f; scale %.3f..%.3f\n", axisOk, records, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], scaleLo, scaleHi);
		}

		auto dump = [](const char* label, const std::uint8_t* raw, std::size_t size) {
			std::printf("%s u32:", label);
			for (std::size_t o = 0; o + 4 <= size; o += 4) {
				std::uint32_t u;
				std::memcpy(&u, raw + o, 4);
				std::printf(" %08X", u);
			}
			std::printf("\n%*s f32:", static_cast<int>(std::strlen(label)), "");
			for (std::size_t o = 0; o + 4 <= size; o += 4) {
				float f;
				std::memcpy(&f, raw + o, 4);
				std::printf(" %.3f", f);
			}
			std::printf("\n");
		};
		for (std::size_t i = 0; i < std::min<std::size_t>(4, world.records40.size() / 40); ++i) {
			dump(std::format("  records40[{}]  ", i).c_str(), world.records40.data() + i * 40, 40);
		}
		// Streamkeys the cells point at: asset root (56 B) at its traced offset, +32 data, +48 size, +55 flags.
		auto describeKey = [&](std::uint64_t stored) -> std::string {
			const std::string where = resolve(stored);
			if (stored == kPtrNull || stored == kPtrInline || stored == kPtrInsert) {
				return where;
			}
			const std::uint64_t offset = (stored - 1) & 0x0FFFFFFFFFFFFFFFull;
			if (((stored - 1) >> 60) != XBlockVirtual || offset < tableBegin || offset >= tableEnd) {
				return where;
			}
			const std::size_t asset = static_cast<std::size_t>((offset - tableBegin) / 16);
			const std::span<const std::uint8_t> root(zone.stream.data() + trace.assets[asset].offset, 56);
			return std::format("{}: size {} flags 0x{:02X} package key {:016X}", where, Get<std::uint32_t>(root, 48), root[55],
				Get<std::uint64_t>(root, 8));
		};
		for (std::size_t c = 0; c < world.cells.size(); ++c) {
			const StreamerWorldCell& cell = world.cells[c];
			std::printf("  cell %2zu keys     models: %s\n", c, describeKey(Get<std::uint64_t>(cell.raw, 176 + 40)).c_str());
			for (std::size_t k = 0; k < cell.keyed.size(); ++k) {
				if (k < 3 || k + 1 == cell.keyed.size()) {
					dump(std::format("      keyed {:3}   ", k).c_str(), cell.keyed[k].data(), 48);
					std::printf("                   -> %s\n", describeKey(Get<std::uint64_t>(cell.keyed[k], 8)).c_str());
				}
			}
			std::printf("      %zu keyed records\n", cell.keyed.size());
		}
		if (listCell < world.cells.size()) {
			const StreamerWorldCell& cell = world.cells[listCell];
			for (std::size_t m = 0; m < cell.models.size(); ++m) {
				std::printf("  cell %zu model %-4zu %s  +8 %016llX\n", listCell, m, resolve(Get<std::uint64_t>(cell.models[m], 0)).c_str(),
					static_cast<unsigned long long>(Get<std::uint64_t>(cell.models[m], 8)));
			}
		}
		if (world.cells.size() > 1) {
			const std::uint64_t stored = Get<std::uint64_t>(world.cells[1].raw, 176 + 40);
			const std::uint64_t offset = (stored - 1) & 0x0FFFFFFFFFFFFFFFull;
			const std::size_t asset = static_cast<std::size_t>((offset - tableBegin) / 16);
			dump("  cell 1 key root ", zone.stream.data() + trace.assets[asset].offset, 56);
		}
		std::size_t lists = 0, listRecords = 0, gridVectors = 0;
		for (const StreamerWorldCell& cell : world.cells) {
			lists += cell.lists.size();
			for (const auto& r : cell.listRecords) {
				listRecords += r.size();
			}
			gridVectors += cell.gridVectors.size();
		}
		std::printf("  cell lists       %zu lists, %zu 36-B records; grid vectors %zu\n", lists, listRecords, gridVectors);
		for (std::size_t c = 0; c < std::min<std::size_t>(2, world.cells.size()); ++c) {
			const StreamerWorldCell& cell = world.cells[c];
			for (std::size_t l = 0; l < std::min<std::size_t>(2, cell.lists.size()); ++l) {
				dump(std::format("  cell {} list {}  ", c, l).c_str(), cell.lists[l].data(), 24);
				for (std::size_t r = 0; r < std::min<std::size_t>(3, cell.listRecords[l].size()); ++r) {
					dump(std::format("    record {}     ", r).c_str(), cell.listRecords[l][r].data(), 36);
				}
			}
			for (std::size_t v = 0; v < std::min<std::size_t>(3, cell.gridVectors.size()); ++v) {
				dump(std::format("  cell {} gridvec {}", c, v).c_str(), cell.gridVectors[v].data(), 12);
			}
		}

		for (std::size_t c = 0; c < std::min<std::size_t>(2, world.cells.size()); ++c) {
			const StreamerWorldCell& cell = world.cells[c];
			std::printf("  cell %zu           %zu models, first: %s\n", c, cell.models.size(),
				cell.models.empty() ? "-" : resolve(Get<std::uint64_t>(cell.models[0], 0)).c_str());
			std::printf("      raw +0..+8   %016llX   +224..+256", static_cast<unsigned long long>(Get<std::uint64_t>(cell.raw, 0)));
			for (std::size_t o = 224; o < 256; o += 4) {
				std::printf(" %g", Get<float>(cell.raw, o));
			}
			std::printf("\n");
			for (std::size_t i = 0; i < std::min<std::size_t>(2, cell.modelInfo.size()); ++i) {
				std::printf("      model %zu      +8 %016llX  info", i,
					static_cast<unsigned long long>(Get<std::uint64_t>(cell.models[i], 8)));
				for (std::size_t o = 0; o < 32; o += 4) {
					std::printf(" %g", Get<float>(cell.modelInfo[i], o));
				}
				std::printf("\n");
			}
			for (std::size_t g = 0; g < 8; ++g) {
				for (std::size_t e = 0; e < std::min<std::size_t>(1, cell.groups[g].size()); ++e) {
					const StreamerWorldGroupEntry& entry = cell.groups[g][e];
					std::printf("      group %zu[0]   u32:", g);
					for (std::size_t o = 0; o < 56; o += 4) {
						std::printf(" %u", Get<std::uint32_t>(entry.header, o));
					}
					std::printf("  (%zu records)\n", entry.records.size());
					for (std::size_t r = 0; r < std::min<std::size_t>(2, entry.records.size()); ++r) {
						const auto f = Floats52(entry.records[r]);
						std::printf("        record %zu  ", r);
						for (float v : f) {
							std::printf(" %.3f", v);
						}
						std::printf("\n");
					}
				}
			}
		}
		return true;
	}

	// A stored reference into the XAsset array (block 4, just before the first asset; +8 = the header
	// field) names the asset it points at.
	std::optional<std::size_t> AssetOfReference(const ZoneTrace& trace, const XAssetList& list, std::uint64_t stored) {
		if (stored == kPtrNull || stored == kPtrInline || stored == kPtrInsert) {
			return std::nullopt;
		}
		const std::uint64_t value = stored - 1;
		const std::uint64_t offset = value & 0x0FFFFFFFFFFFFFFFull;
		const std::uint64_t tableEnd = trace.assets[0].pos[XBlockVirtual];
		const std::uint64_t tableBegin = tableEnd - 16ull * list.assets.size();
		if ((value >> 60) != XBlockVirtual || offset < tableBegin || offset >= tableEnd) {
			return std::nullopt;
		}
		return static_cast<std::size_t>((offset - tableBegin) / 16);
	}

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
				float f = std::ldexp(static_cast<float>(mantissa), -24);
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

	// One LOD's geometry, decoded from its traced start: the mesh info, its buffer (resident in the .ff or
	// from the .xsub packages by the LOD's stream key) and the vertex bounds. Optionally an OBJ, which is
	// game data: write it only to a local folder, never into the repo.
	// How a mesh's stored tangent frames sit against its UVs: the tangent along +dP/du, and the bitangent (the
	// tangent's 2-bit sign * cross(normal, tangent)) along +dP/dv or -dP/dv. Which way a normal map's green points.
	struct TangentFrames {
		std::size_t triangles = 0, tangentAlong = 0, tangentAgainst = 0, bitangentAlong = 0, bitangentAgainst = 0;
	};

	void FrameCheck(const std::vector<std::uint8_t>& buffer, std::size_t positions, std::size_t records, std::uint32_t first,
		std::array<std::uint16_t, 3> corners, TangentFrames& frames) {
		using Vec = std::array<float, 3>;
		auto unpack = [](std::uint32_t packed) {
			Vec v{};
			for (int axis = 0; axis < 3; ++axis) {
				v[axis] = (static_cast<float>((packed >> (10 * axis)) & 0x3FFu) - 512.0f) / 511.0f;
			}
			return v;
		};
		Vec p[3];
		float u[3], v[3];
		for (int k = 0; k < 3; ++k) {
			const std::size_t vertex = std::size_t(first) + corners[k];
			std::memcpy(p[k].data(), buffer.data() + positions + vertex * 12, 12);
			u[k] = HalfToFloat(Get<std::uint16_t>(buffer, records + vertex * 16 + 4));
			v[k] = HalfToFloat(Get<std::uint16_t>(buffer, records + vertex * 16 + 6));
		}
		const float du1 = u[1] - u[0], dv1 = v[1] - v[0], du2 = u[2] - u[0], dv2 = v[2] - v[0];
		const float det = du1 * dv2 - du2 * dv1;
		if (std::abs(det) < 1e-8f) {
			return;
		}
		Vec dPdu{}, dPdv{};
		for (int k = 0; k < 3; ++k) {
			const float e1 = p[1][k] - p[0][k], e2 = p[2][k] - p[0][k];
			dPdu[k] = (e1 * dv2 - e2 * dv1) / det;
			dPdv[k] = (e2 * du1 - e1 * du2) / det;
		}
		const std::size_t vertex = std::size_t(first) + corners[0];
		const Vec n = unpack(Get<std::uint32_t>(buffer, records + vertex * 16 + 8));
		const std::uint32_t packedTangent = Get<std::uint32_t>(buffer, records + vertex * 16 + 12);
		const Vec t = unpack(packedTangent);
		const float sign = (packedTangent >> 30) == 0 ? -1.0f : 1.0f;
		const Vec b = { sign * (n[1] * t[2] - n[2] * t[1]), sign * (n[2] * t[0] - n[0] * t[2]), sign * (n[0] * t[1] - n[1] * t[0]) };
		auto dot = [](const Vec& x, const Vec& y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
		++frames.triangles;
		++(dot(t, dPdu) >= 0 ? frames.tangentAlong : frames.tangentAgainst);
		++(dot(b, dPdv) >= 0 ? frames.bitangentAlong : frames.bitangentAgainst);
	}

	bool PrintMesh(const Options& options, const LoadedZone& zone, const XAssetList& list) {
		ZoneTrace trace;
		std::string error;
		if (!ReadZoneTrace(options.trace, trace, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		if (const std::string mismatch = AlignZoneTrace(trace, zone.stream, list); !mismatch.empty()) {
			std::fprintf(stderr, "the trace does not fit: %s\n", mismatch.c_str());
			return false;
		}
		std::size_t index = options.meshAsset;
		if (options.meshName) {
			for (std::size_t i = 0; i < list.assets.size(); ++i) {
				const std::size_t at = trace.assets[i].offset;
				if (list.assets[i].type == 0x06 && list.assets[i].header == kPtrInline && at + 8 <= zone.stream.size()
					&& (Get<std::uint64_t>(std::span<const std::uint8_t>(zone.stream).subspan(at, 8), 0) & ~(1ull << 63)) == options.meshName) {
					index = i;
					break;
				}
			}
			if (index >= list.assets.size()) {
				std::fprintf(stderr, "no xmodel %016llX stored in this zone\n", static_cast<unsigned long long>(options.meshName));
				return false;
			}
		}
		if (index >= list.assets.size()) {
			std::fprintf(stderr, "asset %zu: the zone has %zu\n", index, list.assets.size());
			return false;
		}
		if (list.assets[index].type == 0x06) {
			const std::span<const std::uint8_t> model(zone.stream.data() + trace.assets[index].offset, 232);
			const auto lod = AssetOfReference(trace, list, Get<std::uint64_t>(model, 32));
			std::printf("\nxmodel %zu         name %016llX, LOD 0 %s\n", index,
				static_cast<unsigned long long>(Get<std::uint64_t>(model, 0)),
				lod ? std::format("= asset {}", *lod).c_str() : "is not a reference to an earlier asset");
			// +16: the model's own collision (an xcollision asset), null when the model has none.
			const std::uint64_t collision = Get<std::uint64_t>(model, 16);
			const auto collisionAsset = collision ? AssetOfReference(trace, list, collision) : std::nullopt;
			std::printf("  collision (+16)  %s\n", !collision ? "none (null)"
				: collisionAsset ? std::format("asset {} ({}, name {:016X})", *collisionAsset,
					XAssetTypeName(list.assets[*collisionAsset].type), Get<std::uint64_t>(std::span<const std::uint8_t>(zone.stream)
						.subspan(trace.assets[*collisionAsset].offset, 8), 0)).c_str()
				: std::format("{:016X}, not a reference to an earlier asset", collision).c_str());
			if (collisionAsset && list.assets[*collisionAsset].type == 0x07) {
				// xcollision (96 B, Load_XCollision): +8 skeleton, +16 stream key (streamed shapes), +24 the shapes
				// stored in the zone (40 B), +40 physpreset, +48 trigger actions, +56 physconstraints.
				const std::span<const std::uint8_t> c(zone.stream.data() + trace.assets[*collisionAsset].offset, 96);
				std::printf("  xcollision       +8 %016llX  +16 streamkey %016llX  +24 inline %016llX  +40 %016llX  +48 %016llX  "
					"+56 %016llX\n", static_cast<unsigned long long>(Get<std::uint64_t>(c, 8)),
					static_cast<unsigned long long>(Get<std::uint64_t>(c, 16)), static_cast<unsigned long long>(Get<std::uint64_t>(c, 24)),
					static_cast<unsigned long long>(Get<std::uint64_t>(c, 40)), static_cast<unsigned long long>(Get<std::uint64_t>(c, 48)),
					static_cast<unsigned long long>(Get<std::uint64_t>(c, 56)));
				std::printf("  xcollision raw  ");
				for (std::size_t i = 0; i < 96; i += 4) {
					std::printf(" %08X", Get<std::uint32_t>(c, i));
				}
				std::printf("\n");
			}
			if (!lod) {
				return false;
			}
			index = *lod;
		}
		if (list.assets[index].type != 0x09) {
			std::fprintf(stderr, "asset %zu is a %s, not an xmodel or xmodelmesh\n", index,
				std::string(XAssetTypeName(list.assets[index].type)).c_str());
			return false;
		}

		const TracedAsset& start = trace.assets[index];
		XStream s(zone.stream, 0);
		s.Restore(start.offset, start.block, start.pos);
		XModelMeshData mesh;
		if (!ReadXModelMesh(s, list.assets[index].header, mesh)) {
			std::printf("xmodelmesh %zu FAILED: %s\n", index, s.Error().c_str());
			return false;
		}
		const MeshInfo& info = mesh.info;
		std::printf("xmodelmesh %zu     name %016llX, stream key %016llX, %zu surfaces\n", index,
			static_cast<unsigned long long>(mesh.Name()), static_cast<unsigned long long>(mesh.StreamKey()), mesh.surfaces.size());
		if (!mesh.hasInfo) {
			std::printf("  mesh info is a reference to an earlier asset's (not followed yet)\n");
			return false;
		}
		std::printf("  mesh info       flags 0x%02X (%s%s), %u vertices, %u faces, buffer %u bytes; offsets positions %u "
			"vertices %u faces %u weights %u\n", info.Flags(), info.Streamed() ? "streamed" : "resident",
			info.ExtendedVertices() ? ", 24-B vertices" : "", info.VertexCount(), info.FaceCount(), info.BufferSize(),
			info.PositionOffset(), info.VertexOffset(), info.FaceOffset(), info.WeightOffset());

		std::vector<std::uint8_t> buffer;
		if (info.Streamed()) {
			Kapi::PackageIndex packages;
			if (!packages.Open(options.gameDir / "zone", error)) {
				std::fprintf(stderr, "%s\n", error.c_str());
				return false;
			}
			std::printf("  packages        %zu entries in %zu .xsub files\n", packages.Size(), packages.FileCount());
			if (const Kapi::Entry* e = packages.Find(mesh.StreamKey())) {
				std::printf("  package entry   %s +0x%llX, %u bytes stored\n", packages.File(e->file).filename().string().c_str(),
					static_cast<unsigned long long>(e->offset), e->size);
			}
			if (!packages.Extract(mesh.StreamKey(), buffer, error)) {
				std::printf("  EXTRACT FAILED  %s\n", error.c_str());
				return false;
			}
		}
		else {
			buffer = info.resident;
			if (buffer.empty()) {
				const auto data = Get<std::uint64_t>(info.raw, 32);
				std::printf("  resident data   pointer %016llX (block %llu +0x%llX)\n", static_cast<unsigned long long>(data),
					static_cast<unsigned long long>((data - 1) >> 60), static_cast<unsigned long long>((data - 1) & 0x0FFFFFFFFFFFFFFFull));
			}
		}
		std::printf("  buffer          %zu bytes (%s the declared %u)\n", buffer.size(),
			buffer.size() == info.BufferSize() ? "matches" : "DIFFERS from", info.BufferSize());

		const std::size_t vertexStride = info.ExtendedVertices() ? 24 : 16;
		TangentFrames frames;
		float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
		std::size_t badIndices = 0, vertices = 0, triangles = 0;
		std::string obj;
		std::size_t objBase = 1;
		for (std::size_t si = 0; si < mesh.surfaces.size(); ++si) {
			const MeshSurface& surface = mesh.surfaces[si];
			obj += std::format("o surface_{}\n", si);
			for (std::uint32_t v = 0; v < surface.VertexCount(); ++v) {
				const std::size_t at = info.PositionOffset() + (std::size_t(surface.FirstVertex()) + v) * 12;
				const std::size_t uvAt = info.VertexOffset() + (std::size_t(surface.FirstVertex()) + v) * vertexStride + 4;
				if (at + 12 > buffer.size() || uvAt + 4 > buffer.size()) {
					std::printf("  surface %zu vertex %u lies outside the buffer\n", si, v);
					return false;
				}
				float p[3];
				std::memcpy(p, buffer.data() + at, 12);
				for (int k = 0; k < 3; ++k) {
					lo[k] = std::min(lo[k], p[k]);
					hi[k] = std::max(hi[k], p[k]);
				}
				const float u = HalfToFloat(Get<std::uint16_t>(buffer, uvAt));
				const float w = HalfToFloat(Get<std::uint16_t>(buffer, uvAt + 2));
				obj += std::format("v {} {} {}\nvt {} {}\n", p[0], p[1], p[2], u, 1.0f - w);
			}
			for (std::uint32_t f = 0; f < surface.FaceCount(); ++f) {
				const std::size_t at = info.FaceOffset() + std::size_t(surface.FirstIndex()) * 2 + std::size_t(f) * 6;
				if (at + 6 > buffer.size()) {
					std::printf("  surface %zu face %u lies outside the buffer\n", si, f);
					return false;
				}
				const std::uint16_t a = Get<std::uint16_t>(buffer, at), b = Get<std::uint16_t>(buffer, at + 2),
					c = Get<std::uint16_t>(buffer, at + 4);
				badIndices += (a >= surface.VertexCount()) + (b >= surface.VertexCount()) + (c >= surface.VertexCount());
				obj += std::format("f {0}/{0} {1}/{1} {2}/{2}\n", objBase + a, objBase + b, objBase + c);
				if (vertexStride == 16 && a < surface.VertexCount() && b < surface.VertexCount() && c < surface.VertexCount()) {
					FrameCheck(buffer, info.PositionOffset(), info.VertexOffset(), surface.FirstVertex(), { a, b, c }, frames);
				}
			}
			objBase += surface.VertexCount();
			vertices += surface.VertexCount();
			triangles += surface.FaceCount();
			std::printf("  surface %-3zu     %u vertices from %u, %u triangles from index %u\n", si, surface.VertexCount(),
				surface.FirstVertex(), surface.FaceCount(), surface.FirstIndex());
		}
		std::printf("  geometry        %zu vertices, %zu triangles, %zu indices out of range; bounds x %.1f..%.1f "
			"y %.1f..%.1f z %.1f..%.1f\n", vertices, triangles, badIndices, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
		if (frames.triangles) {
			std::printf("  tangent frames  %zu triangles with UVs: tangent along +dP/du %zu, against %zu; sign * cross(n, t) "
				"along +dP/dv %zu, against %zu\n", frames.triangles, frames.tangentAlong, frames.tangentAgainst,
				frames.bitangentAlong, frames.bitangentAgainst);
		}
		if (!options.objOut.empty()) {
			if (!WriteWholeFile(options.objOut, std::vector<std::uint8_t>(obj.begin(), obj.end()))) {
				std::fprintf(stderr, "could not write %s\n", options.objOut.string().c_str());
				return false;
			}
			std::printf("  wrote           %s (local only: game geometry)\n", options.objOut.string().c_str());
			// The raw parts next to it, for working out the vertex format: .buf the mesh buffer, .info the 464-B mesh
			// info, .lod the xmodelmesh root, .surf the 48-B surfaces.
			std::vector<std::uint8_t> surfaces;
			for (const MeshSurface& surface : mesh.surfaces) {
				surfaces.insert(surfaces.end(), surface.raw.begin(), surface.raw.end());
			}
			const fs::path base = options.objOut;
			WriteWholeFile(fs::path(base).concat(".buf"), buffer);
			WriteWholeFile(fs::path(base).concat(".info"), std::vector<std::uint8_t>(info.raw.begin(), info.raw.end()));
			WriteWholeFile(fs::path(base).concat(".lod"), std::vector<std::uint8_t>(mesh.root.begin(), mesh.root.end()));
			WriteWholeFile(fs::path(base).concat(".surf"), surfaces);
		}
		return badIndices == 0;
	}

	// Every xmodel whose LOD 0 mesh is resident (kept in the .ff, not streamed): the candidates for a model
	// mapkit writes itself. "own data" = the buffer is stored inline in this LOD, not a reference into another's.
	bool PrintResidentMeshes(const Options& options, const LoadedZone& zone, const XAssetList& list) {
		ZoneTrace trace;
		std::string error;
		if (options.trace.empty() || !ReadZoneTrace(options.trace, trace, error) || !AlignZoneTrace(trace, zone.stream, list).empty()) {
			std::fprintf(stderr, "--resident-meshes needs a trace that fits: %s\n", error.c_str());
			return false;
		}
		std::size_t resident = 0, own = 0;
		// xmodelmesh +62 (u16, not understood yet), resident and streamed LOD 0s apart.
		std::map<std::pair<bool, std::uint16_t>, std::size_t> mesh62;
		std::printf("\n xmodel   name              lod0    surf verts  faces  flags buffer  own-data mesh+62 root +208..+231\n");
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			if (list.assets[i].type != 0x06 || list.assets[i].header != kPtrInline) {
				continue;
			}
			const std::span<const std::uint8_t> model(zone.stream.data() + trace.assets[i].offset, 232);
			const auto lod = AssetOfReference(trace, list, Get<std::uint64_t>(model, 32));
			if (!lod || list.assets[*lod].type != 0x09) {
				continue;
			}
			XStream s(zone.stream, 0);
			s.Restore(trace.assets[*lod].offset, trace.assets[*lod].block, trace.assets[*lod].pos);
			XModelMeshData mesh;
			if (!ReadXModelMesh(s, list.assets[*lod].header, mesh) || !mesh.hasInfo) {
				continue;
			}
			++mesh62[{ mesh.info.Streamed(), Get<std::uint16_t>(mesh.root, 62) }];
			if (mesh.info.Streamed()) {
				continue;
			}
			++resident;
			own += mesh.info.resident.empty() ? 0 : 1;
			if (!mesh.info.resident.empty()) {
				std::string tail;
				for (std::size_t b = 208; b < 232; ++b) {
					tail += std::format("{:02X}", model[b]);
				}
				std::printf(" %-8zu %016llX  %-7zu %-4zu %-6u %-6u 0x%02X  %-7u %-8s 0x%04X  %s\n", i,
					static_cast<unsigned long long>(Get<std::uint64_t>(model, 0)), *lod, mesh.surfaces.size(),
					mesh.info.VertexCount(), mesh.info.FaceCount(), mesh.info.Flags(), mesh.info.BufferSize(),
					mesh.info.resident.empty() ? "ref" : "yes", Get<std::uint16_t>(mesh.root, 62), tail.c_str());
			}
		}
		std::printf("%zu xmodels with a resident LOD 0, %zu of them storing their own buffer\n", resident, own);
		for (const auto& [key, count] : mesh62) {
			std::printf("  LOD 0 mesh+62 = 0x%04X: %zu %s\n", key.second, count, key.first ? "streamed" : "resident");
		}
		return true;
	}

	std::string HexWords(std::span<const std::uint8_t> bytes, std::size_t limit = SIZE_MAX) {
		std::string out;
		for (std::size_t i = 0; i + 4 <= bytes.size() && i < limit; i += 4) {
			out += std::format("{}{:08X}", i && i % 32 == 0 ? "\n                   " : " ", Get<std::uint32_t>(bytes, i));
		}
		return out;
	}

	std::string DxgiName(std::uint32_t format) {
		switch (format) {
		case 2: return "R32G32B32A32_FLOAT";
		case 10: return "R16G16B16A16_FLOAT";
		case 24: return "R10G10B10A2_UNORM";
		case 26: return "R11G11B10_FLOAT";
		case 28: return "R8G8B8A8_UNORM";
		case 29: return "R8G8B8A8_UNORM_SRGB";
		case 41: return "R32_FLOAT";
		case 49: return "R8G8_UNORM";
		case 54: return "R16_FLOAT";
		case 56: return "R16_UNORM";
		case 61: return "R8_UNORM";
		case 71: return "BC1_UNORM";
		case 72: return "BC1_UNORM_SRGB";
		case 74: return "BC2_UNORM";
		case 77: return "BC3_UNORM";
		case 78: return "BC3_UNORM_SRGB";
		case 80: return "BC4_UNORM";
		case 81: return "BC4_SNORM";
		case 83: return "BC5_UNORM";
		case 84: return "BC5_SNORM";
		case 87: return "B8G8R8A8_UNORM";
		case 91: return "B8G8R8A8_UNORM_SRGB";
		case 95: return "BC6H_UF16";
		case 96: return "BC6H_SF16";
		case 98: return "BC7_UNORM";
		case 99: return "BC7_UNORM_SRGB";
		default: return std::format("dxgi {}", format);
		}
	}

	// One image's header: format, size, residency, and each mip's record (package key, size field, dimensions).
	void PrintImageHeader(const ImageData& image, const char* indent) {
		std::printf("%sname %016llX  %ux%u  %s  flags 0x%08X (%s)  %u mips  +152 size %u  +164 %u\n", indent,
			static_cast<unsigned long long>(image.Name()), image.Width(), image.Height(), DxgiName(image.Format()).c_str(),
			image.Flags(), image.Flags() & 0x10 ? "streamed" : "resident", image.MipCount(), Get<std::uint32_t>(image.root, 152),
			image.root[164]);
		for (std::size_t m = 0; m + 32 <= image.mips.size(); m += 32) {
			const std::uint32_t size = Get<std::uint32_t>(image.mips, m + 24);
			std::printf("%s  mip %zu  key %016llX  +8 %016llX +16 %016llX  size field 0x%08X (%u B)  %ux%u\n", indent, m / 32,
				static_cast<unsigned long long>(Get<std::uint64_t>(image.mips, m)),
				static_cast<unsigned long long>(Get<std::uint64_t>(image.mips, m + 8)),
				static_cast<unsigned long long>(Get<std::uint64_t>(image.mips, m + 16)), size, (size >> 4) & 0x1FFFFFFF,
				Get<std::uint16_t>(image.mips, m + 28), Get<std::uint16_t>(image.mips, m + 30));
		}
		std::printf("%spixels %s, data %s\n", indent,
			image.pixels.empty() ? "not stored here" : std::format("{} B stored here", image.pixels.size()).c_str(),
			image.data.empty() ? "not stored here" : std::format("{} B stored here", image.data.size()).c_str());
		std::printf("%sroot %s\n", indent, HexWords(image.root).c_str());
	}

	// Bytes of one w x h level in a DXGI format (block formats pad to 4 x 4 blocks); 0 = a format not handled here.
	std::size_t LevelBytes(std::uint32_t format, std::uint32_t w, std::uint32_t h) {
		const std::size_t blocks = std::size_t((std::max(w, 1u) + 3) / 4) * ((std::max(h, 1u) + 3) / 4);
		switch (format) {
		case 71: case 72: case 80: case 81: return blocks * 8;
		case 74: case 77: case 78: case 83: case 84: case 95: case 96: case 98: case 99: return blocks * 16;
		case 24: case 28: case 29: case 87: case 91: return std::size_t(std::max(w, 1u)) * std::max(h, 1u) * 4;
		case 10: return std::size_t(std::max(w, 1u)) * std::max(h, 1u) * 8;
		default: return 0;
		}
	}

	// A .dds with a DX10 header: `levels` levels of a w x h image in a DXGI format, back to back from level 0.
	void WriteDds(std::uint32_t format, std::uint32_t w, std::uint32_t h, std::uint32_t levels, std::span<const std::uint8_t> bytes,
		const fs::path& path) {
		std::vector<std::uint8_t> file(4 + 124 + 20);
		auto put = [&](std::size_t at, std::uint32_t v) { std::memcpy(file.data() + at, &v, 4); };
		std::memcpy(file.data(), "DDS ", 4);
		put(4, 124);
		put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);
		put(12, h);
		put(16, w);
		put(20, static_cast<std::uint32_t>(LevelBytes(format, w, h)));
		put(28, levels);
		put(76, 32);
		put(80, 0x4);
		std::memcpy(file.data() + 84, "DX10", 4);
		put(108, 0x1000 | 0x400000 | 0x8);
		put(128, format);
		put(132, 3);
		put(140, 1);
		file.insert(file.end(), bytes.begin(), bytes.end());
		WriteWholeFile(path, file);
		std::printf("                  dds: %s (%ux%u, %u levels)\n", path.string().c_str(), w, h, levels);
	}

	// The levels an image stores in the zone as a .dds (DX10 header): all of a resident image's pixels, or the small
	// chain a streamed image keeps at +8 (from 32 on the long side down to 1 x 1). Game data: write it only locally.
	void WriteImageDds(const ImageData& image, const fs::path& path) {
		const bool streamed = image.Flags() & 0x10;
		const std::vector<std::uint8_t>& bytes = streamed ? image.data : image.pixels;
		std::uint32_t w = image.Width(), h = image.Height();
		while (streamed && std::max(w, h) > 32) {
			w = std::max(w >> 1, 1u);
			h = std::max(h >> 1, 1u);
		}
		std::uint32_t levels = 0;
		std::size_t total = 0;
		for (std::uint32_t lw = w, lh = h; total < bytes.size(); lw = std::max(lw >> 1, 1u), lh = std::max(lh >> 1, 1u)) {
			const std::size_t size = LevelBytes(image.Format(), lw, lh);
			if (!size) {
				break;
			}
			total += size;
			++levels;
			if (lw == 1 && lh == 1) {
				break;
			}
		}
		if (bytes.empty() || total != bytes.size()) {
			std::printf("                  dds: skipped (%zu stored bytes, %zu from %ux%u in %s)\n", bytes.size(), total, w, h,
				DxgiName(image.Format()).c_str());
			return;
		}
		WriteDds(image.Format(), w, h, levels, bytes, path);
	}

	// A streamed image's pixels, fetched from the packages: the mip record with a package key and the largest size holds
	// the levels from the full size down (Die Maschine's sky: one 8192 x 4096 level; skybox_default_black: all eight
	// from 128 x 64, the zone keeping those from 64 x 32). Game data: write it only locally.
	void WriteStreamedImageDds(const Kapi::PackageIndex& packages, const ImageData& image, const fs::path& path) {
		std::uint64_t key = 0;
		std::uint32_t largest = 0;
		for (std::size_t m = 0; m + 32 <= image.mips.size(); m += 32) {
			const std::uint64_t mipKey = Get<std::uint64_t>(image.mips, m);
			const std::uint32_t size = (Get<std::uint32_t>(image.mips, m + 24) >> 4) & 0x1FFFFFFF;
			if (mipKey && size > largest) {
				key = mipKey;
				largest = size;
			}
		}
		std::vector<std::uint8_t> bytes;
		std::string error;
		if (!key || !packages.Extract(key, bytes, error)) {
			std::printf("                  dds: streamed pixels not fetched (%s)\n", key ? error.c_str() : "no package key");
			return;
		}
		std::uint32_t levels = 0;
		std::size_t total = 0;
		for (std::uint32_t w = image.Width(), h = image.Height(); total < bytes.size(); w = std::max(w >> 1, 1u), h = std::max(h >> 1, 1u)) {
			const std::size_t size = LevelBytes(image.Format(), w, h);
			if (!size) {
				break;
			}
			total += size;
			++levels;
			if (w == 1 && h == 1) {
				break;
			}
		}
		if (total != bytes.size()) {
			std::printf("                  dds: streamed pixels skipped (%zu bytes fetched, %zu from %ux%u in %s)\n", bytes.size(),
				total, image.Width(), image.Height(), DxgiName(image.Format()).c_str());
			return;
		}
		WriteDds(image.Format(), image.Width(), image.Height(), levels, bytes, path);
	}

	// An image asset of this zone, decoded from its traced start.
	bool ReadTracedImage(const ZoneTrace& trace, const XAssetList& list, const LoadedZone& zone, std::size_t asset, ImageData& out) {
		XStream s(zone.stream, 0);
		s.Restore(trace.assets[asset].offset, trace.assets[asset].block, trace.assets[asset].pos);
		return ReadImage(s, list.assets[asset].header, out);
	}

	// One material: its root, constant buffer, image table (each image's header, followed to its asset) and the
	// small arrays, to learn what a material writer must fill.
	bool PrintMaterial(const Options& options, const LoadedZone& zone, const XAssetList& list) {
		ZoneTrace trace;
		std::string error;
		if (options.trace.empty() || !ReadZoneTrace(options.trace, trace, error) || !AlignZoneTrace(trace, zone.stream, list).empty()) {
			std::fprintf(stderr, "--material needs a trace that fits: %s\n", error.c_str());
			return false;
		}
		const std::uint64_t name = options.material.starts_with('#')
			? std::stoull(options.material.substr(1), nullptr, 16) & ~(1ull << 63) : HashName(options.material);
		std::size_t index = SIZE_MAX;
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			const std::size_t at = trace.assets[i].offset;
			if (list.assets[i].type == 0x0A && list.assets[i].header == kPtrInline && at + 8 <= zone.stream.size()
				&& (Get<std::uint64_t>(std::span<const std::uint8_t>(zone.stream).subspan(at, 8), 0) & ~(1ull << 63)) == name) {
				index = i;
				break;
			}
		}
		if (index == SIZE_MAX) {
			std::fprintf(stderr, "no material %016llX stored in this zone\n", static_cast<unsigned long long>(name));
			return false;
		}
		XStream s(zone.stream, 0);
		s.Restore(trace.assets[index].offset, trace.assets[index].block, trace.assets[index].pos);
		MaterialData material;
		if (!ReadMaterial(s, list.assets[index].header, material)) {
			std::printf("material %zu FAILED: %s\n", index, s.Error().c_str());
			return false;
		}
		const auto& root = material.root;
		std::printf("\nmaterial %zu  name %016llX, %zu bytes in the stream\n", index,
			static_cast<unsigned long long>(material.Name()), s.Cursor() - trace.assets[index].offset);
		std::printf("  root            %s\n", HexWords(root).c_str());
		const std::uint64_t techset = Get<std::uint64_t>(root, 40);
		const auto techsetAsset = AssetOfReference(trace, list, techset);
		std::printf("  techset (+40)   %s\n", techsetAsset
			? std::format("asset {} ({}), name {:016X}", *techsetAsset, XAssetTypeName(list.assets[*techsetAsset].type),
				Get<std::uint64_t>(std::span<const std::uint8_t>(zone.stream).subspan(trace.assets[*techsetAsset].offset, 8), 0)).c_str()
			: std::format("{:016X}", techset).c_str());
		std::printf("  +128 block      %zu records stored here, constant buffer %zu B (+144 says %llu)\n", material.records.size(),
			material.constants.size(), static_cast<unsigned long long>(Get<std::uint64_t>(root, 128 + 144)));
		std::vector<std::uint8_t> constants = material.constants;
		const std::uint64_t constantsSize = Get<std::uint64_t>(root, 128 + 144);
		if (constants.empty() && constantsSize) {
			std::string resolveError;
			if (ResolveStoredData(trace, zone.stream, Get<std::uint64_t>(root, 128 + 152), static_cast<std::size_t>(constantsSize),
				constants, resolveError)) {
				std::printf("  constants       shared with an earlier asset, resolved:\n");
			}
			else {
				std::printf("  constants       not resolved: %s\n", resolveError.c_str());
			}
		}
		if (!constants.empty()) {
			std::printf("  constants       %s\n", HexWords(constants, 512).c_str());
		}
		std::printf("  images (+48)    %u (u8 @328), +329 %u, +330..+335 %s\n", root[328], root[329],
			HexWords(std::span<const std::uint8_t>(root).subspan(328, 8)).c_str());
		std::size_t inlineAt = 0;
		std::optional<Kapi::PackageIndex> packages; // opened for the first streamed image --dds writes
		for (std::size_t e = 0; e + 24 <= material.imageTable.size(); e += 24) {
			const std::uint64_t image = Get<std::uint64_t>(material.imageTable, e);
			std::printf("  image %-2zu        semantic %08X  +12 %08X +16 %08X +20 %08X  ", e / 24,
				Get<std::uint32_t>(material.imageTable, e + 8), Get<std::uint32_t>(material.imageTable, e + 12),
				Get<std::uint32_t>(material.imageTable, e + 16), Get<std::uint32_t>(material.imageTable, e + 20));
			ImageData data;
			bool have = false;
			if (image == kPtrInline || image == kPtrInsert) {
				std::printf("stored inside the material\n");
				if (inlineAt < material.inlineImages.size()) {
					data = material.inlineImages[inlineAt++];
					have = true;
				}
			}
			else if (const auto asset = AssetOfReference(trace, list, image); asset && list.assets[*asset].type == 0x10) {
				std::printf("= image asset %zu\n", *asset);
				have = ReadTracedImage(trace, list, zone, *asset, data);
			}
			else {
				std::printf("reference %016llX\n", static_cast<unsigned long long>(image));
			}
			if (have) {
				PrintImageHeader(data, "                  ");
				if (!options.ddsDir.empty()) {
					fs::create_directories(options.ddsDir);
					const std::string stem = std::format("{:08X}_{:016X}", Get<std::uint32_t>(material.imageTable, e + 8), data.Name());
					WriteImageDds(data, options.ddsDir / (stem + ".dds"));
					if (data.Flags() & 0x10) {
						if (!packages) {
							packages.emplace();
							if (!packages->Open(options.gameDir / "zone", error)) {
								std::fprintf(stderr, "%s\n", error.c_str());
							}
						}
						WriteStreamedImageDds(*packages, data, options.ddsDir / (stem + "_streamed.dds"));
					}
				}
			}
		}
		if (!material.list56.empty()) {
			std::printf("  +56 list        %s\n", HexWords(material.list56).c_str());
		}
		for (const MaterialData::TextureSet& set : material.sets) {
			std::printf("  texture set     root %s\n                  data %s\n                  entries %s\n",
				HexWords(set.root).c_str(), HexWords(set.data, 128).c_str(), HexWords(set.entries, 128).c_str());
		}
		if (!material.tail296.empty()) {
			std::printf("  +296 array      %s\n", HexWords(material.tail296, 256).c_str());
		}
		if (!material.tail312.empty()) {
			std::printf("  +312 array      %s\n", HexWords(material.tail312, 256).c_str());
		}
		std::printf("  local techset   %s\n", material.localTechset ? "stored here" : "none");
		for (std::size_t r = 0; r < material.records.size(); ++r) {
			std::printf("  record %-2zu       %s\n", r, HexWords(material.records[r]).c_str());
		}
		return true;
	}

	// Every image asset stored in this zone: counts by format and residency, then the resident ones (the pixels
	// a zone can carry itself, the kind mapkit would write).
	bool PrintImages(const Options& options, const LoadedZone& zone, const XAssetList& list) {
		ZoneTrace trace;
		std::string error;
		if (options.trace.empty() || !ReadZoneTrace(options.trace, trace, error) || !AlignZoneTrace(trace, zone.stream, list).empty()) {
			std::fprintf(stderr, "--images needs a trace that fits: %s\n", error.c_str());
			return false;
		}
		std::map<std::tuple<std::uint32_t, bool, std::uint32_t>, std::size_t> kinds;
		std::size_t total = 0, failed = 0, shown = 0, idIsName = 0;
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			if (list.assets[i].type != 0x10 || list.assets[i].header != kPtrInline) {
				continue;
			}
			ImageData image;
			if (!ReadTracedImage(trace, list, zone, i, image)) {
				++failed;
				continue;
			}
			++total;
			// Texture sets name images by the +148 id: is it the name's low 32 bits (what mapkit's own images use)?
			idIsName += Get<std::uint32_t>(image.root, 148) == static_cast<std::uint32_t>(image.Name()) ? 1 : 0;
			const bool streamed = image.Flags() & 0x10;
			++kinds[{ image.Format(), streamed, image.Flags() }];
			// By-name references (format 0) are not shown. One line each: size, format, flags, stored bytes, and the
			// header bytes +144..+183 (whose meaning a writer needs).
			if (!streamed && image.Format()) {
				if (!shown++) {
					std::printf("\n resident images: asset  size  format  flags  +152 size  root +148..+183 as bytes\n");
				}
				std::string bytes;
				for (std::size_t b = 148; b < 184; ++b) {
					bytes += std::format("{}{:02X}", b % 4 == 0 ? " " : "", image.root[b]);
				}
				std::printf(" %-7zu %5ux%-5u %-20s 0x%02X %8u %s\n", i, image.Width(), image.Height(),
					DxgiName(image.Format()).c_str(), image.Flags(), Get<std::uint32_t>(image.root, 152), bytes.c_str());
				if (!options.ddsDir.empty()) {
					fs::create_directories(options.ddsDir);
					WriteImageDds(image, options.ddsDir / std::format("{}_{:016X}.dds", i, image.Name()));
				}
			}
		}
		std::printf("\n%zu images stored here (%zu failed to decode); +148 id = the name's low 32 bits on %zu\n", total, failed,
			idIsName);
		std::printf("by format / residency / flags:\n");
		for (const auto& [kind, count] : kinds) {
			std::printf("  %6zu  %-22s %-9s flags 0x%08X\n", count, DxgiName(std::get<0>(kind)).c_str(),
				std::get<1>(kind) ? "streamed" : "resident", std::get<2>(kind));
		}
		return failed == 0;
	}

	// Materials whose image table holds only resident images (pixels stored in the zone): the models for a
	// material mapkit writes with its own images. Per material: techset, images (semantic:format), texture sets.
	bool PrintResidentMaterials(const Options& options, const LoadedZone& zone, const XAssetList& list) {
		ZoneTrace trace;
		std::string error;
		if (options.trace.empty() || !ReadZoneTrace(options.trace, trace, error) || !AlignZoneTrace(trace, zone.stream, list).empty()) {
			std::fprintf(stderr, "--resident-materials needs a trace that fits: %s\n", error.c_str());
			return false;
		}
		const std::span<const std::uint8_t> stream(zone.stream);
		auto nameAt = [&](std::size_t asset) {
			return Get<std::uint64_t>(stream.subspan(trace.assets[asset].offset, 8), 0) & ~(1ull << 63);
		};
		std::size_t total = 0, shown = 0;
		std::map<std::uint64_t, std::size_t> techsets;
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			if (list.assets[i].type != 0x0A || list.assets[i].header != kPtrInline) {
				continue;
			}
			XStream s(zone.stream, 0);
			s.Restore(trace.assets[i].offset, trace.assets[i].block, trace.assets[i].pos);
			MaterialData material;
			if (!ReadMaterial(s, list.assets[i].header, material) || material.imageTable.empty()) {
				continue;
			}
			std::string images;
			bool allResident = true;
			for (std::size_t e = 0; e + 24 <= material.imageTable.size() && allResident; e += 24) {
				const auto asset = AssetOfReference(trace, list, Get<std::uint64_t>(material.imageTable, e));
				if (!asset || list.assets[*asset].type != 0x10) {
					allResident = false;
					break;
				}
				const auto root = stream.subspan(trace.assets[*asset].offset, 208);
				const std::uint32_t format = Get<std::uint32_t>(root, 156);
				// A by-name reference (format 0) is an image of another zone (a shared default): allowed.
				allResident = !(Get<std::uint32_t>(root, 144) & 0x10);
				images += format ? std::format(" {:08X}:{}x{}:{}", Get<std::uint32_t>(material.imageTable, e + 8),
					Get<std::uint16_t>(root, 160), Get<std::uint16_t>(root, 162), DxgiName(format))
					: std::format(" {:08X}:ref:{:016X}", Get<std::uint32_t>(material.imageTable, e + 8), nameAt(*asset));
			}
			if (!allResident) {
				continue;
			}
			++total;
			const auto techset = AssetOfReference(trace, list, Get<std::uint64_t>(material.root, 40));
			const std::uint64_t techsetName = techset ? nameAt(*techset) : 0;
			++techsets[techsetName];
			if (shown++ < 60) {
				std::printf(" material %-7zu %016llX techset %016llX sets %zu consts %zu%s\n", i,
					static_cast<unsigned long long>(material.Name()), static_cast<unsigned long long>(techsetName),
					material.sets.size(), material.constants.size(), images.c_str());
			}
		}
		std::printf("%zu materials use only resident images; by techset:\n", total);
		for (const auto& [name, count] : techsets) {
			std::printf("  %016llX  %zu\n", static_cast<unsigned long long>(name), count);
		}
		return true;
	}

	// Every image semantic the materials here use: how many materials, the image formats behind it, and the
	// commonest semantic sets (a techset's inputs) with a material of each.
	bool PrintSemantics(const Options& options, const LoadedZone& zone, const XAssetList& list) {
		ZoneTrace trace;
		std::string error;
		if (options.trace.empty() || !ReadZoneTrace(options.trace, trace, error) || !AlignZoneTrace(trace, zone.stream, list).empty()) {
			std::fprintf(stderr, "--semantics needs a trace that fits: %s\n", error.c_str());
			return false;
		}
		const std::span<const std::uint8_t> stream(zone.stream);
		std::map<std::uint32_t, std::map<std::uint32_t, std::size_t>> formats; // semantic -> format -> images
		std::map<std::uint32_t, std::size_t> materials;                       // semantic -> materials
		std::map<std::vector<std::uint32_t>, std::pair<std::size_t, std::uint64_t>> sets; // set -> count, a material
		struct TechsetUse {
			std::map<std::uint32_t, std::size_t> formats;
			std::map<std::string, std::size_t> constants;
			std::uint64_t example = 0;
		};
		std::map<std::uint64_t, TechsetUse> bySemantic; // --semantic: techset -> its use of that semantic
		std::size_t total = 0, noTable = 0;
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			if (list.assets[i].type != 0x0A || list.assets[i].header != kPtrInline) {
				continue;
			}
			XStream s(zone.stream, 0);
			s.Restore(trace.assets[i].offset, trace.assets[i].block, trace.assets[i].pos);
			MaterialData material;
			if (!ReadMaterial(s, list.assets[i].header, material)) {
				continue;
			}
			++total;
			if (material.imageTable.empty()) {
				++noTable;
				continue;
			}
			std::vector<std::uint32_t> semantics;
			for (std::size_t e = 0; e + 24 <= material.imageTable.size(); e += 24) {
				const std::uint32_t semantic = Get<std::uint32_t>(material.imageTable, e + 8);
				semantics.push_back(semantic);
				++materials[semantic];
				const auto asset = AssetOfReference(trace, list, Get<std::uint64_t>(material.imageTable, e));
				std::uint32_t format = 0xFFFFFFFF; // not an image asset of this zone
				if (asset && list.assets[*asset].type == 0x10) {
					format = Get<std::uint32_t>(stream.subspan(trace.assets[*asset].offset, 208), 156);
				}
				++formats[semantic][format];
			}
			if (options.semantic) {
				// --semantic <hex>: that semantic's image formats per techset, and the start of each constant buffer
				for (std::size_t e = 0; e + 24 <= material.imageTable.size(); e += 24) {
					if (Get<std::uint32_t>(material.imageTable, e + 8) != options.semantic) {
						continue;
					}
					const auto image = AssetOfReference(trace, list, Get<std::uint64_t>(material.imageTable, e));
					const std::uint32_t format = image && list.assets[*image].type == 0x10
						? Get<std::uint32_t>(stream.subspan(trace.assets[*image].offset, 208), 156) : 0xFFFFFFFF;
					const auto techset = AssetOfReference(trace, list, Get<std::uint64_t>(material.root, 40));
					const std::uint64_t techsetName = techset
						? Get<std::uint64_t>(stream.subspan(trace.assets[*techset].offset, 8), 0) & ~(1ull << 63) : 0;
					auto& entry = bySemantic[techsetName];
					++entry.formats[format];
					if (!entry.example) {
						entry.example = material.Name();
					}
					std::string head;
					for (std::size_t k = 0; k + 4 <= material.constants.size() && k < 16; k += 4) {
						head += std::format(" {:08X}", Get<std::uint32_t>(material.constants, k));
					}
					++entry.constants[head.empty() ? " (shared or none)" : head];
				}
			}
			std::sort(semantics.begin(), semantics.end());
			auto& set = sets[semantics];
			if (set.first++ == 0) {
				set.second = material.Name();
			}
		}
		std::printf("%zu materials (%zu without an image table here)\n\nsemantic  materials  formats (0 = by name)\n",
			total, noTable);
		for (const auto& [semantic, count] : materials) {
			std::string text;
			for (const auto& [format, images] : formats[semantic]) {
				text += format == 0xFFFFFFFF ? std::format(" ?:{}", images) : std::format(" {}:{}", DxgiName(format), images);
			}
			std::printf("%08X  %9zu %s\n", semantic, count, text.c_str());
		}
		std::vector<std::pair<std::size_t, const std::vector<std::uint32_t>*>> order;
		for (const auto& [semantics, set] : sets) {
			order.emplace_back(set.first, &semantics);
		}
		std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		std::printf("\n%zu semantic sets; the commonest:\n", order.size());
		for (std::size_t k = 0; k < order.size() && k < 40; ++k) {
			std::string text;
			for (const std::uint32_t semantic : *order[k].second) {
				text += std::format(" {:08X}", semantic);
			}
			std::printf("  %5zu  e.g. %016llX :%s\n", order[k].first,
				static_cast<unsigned long long>(sets[*order[k].second].second), text.c_str());
		}
		if (options.semantic) {
			std::printf("\nsemantic %08X by techset: image formats, a material, the first constants (16 B) and how often\n",
				options.semantic);
			for (const auto& [techset, use] : bySemantic) {
				std::string text;
				for (const auto& [format, images] : use.formats) {
					text += format == 0xFFFFFFFF ? std::format(" ?:{}", images) : std::format(" {}:{}", DxgiName(format), images);
				}
				std::printf("  techset %016llX  e.g. %016llX %s\n", static_cast<unsigned long long>(techset),
					static_cast<unsigned long long>(use.example), text.c_str());
				std::size_t shown = 0;
				for (const auto& [head, count] : use.constants) {
					if (shown++ < 4) {
						std::printf("      %5zu x%s\n", count, head.c_str());
					}
				}
			}
		}
		return true;
	}

	// Every material as one tab-separated line, for comparing materials offline: asset, name, techset, images
	// (semantic:format:usage, or semantic:ref:name for a by-name image), root, constant buffer, +56 list, +296 and +312
	// arrays (hex).
	bool WriteMaterialTable(const Options& options, const LoadedZone& zone, const XAssetList& list) {
		ZoneTrace trace;
		std::string error;
		if (options.trace.empty() || !ReadZoneTrace(options.trace, trace, error) || !AlignZoneTrace(trace, zone.stream, list).empty()) {
			std::fprintf(stderr, "--material-table needs a trace that fits: %s\n", error.c_str());
			return false;
		}
		const std::span<const std::uint8_t> stream(zone.stream);
		auto hex = [](std::span<const std::uint8_t> bytes) {
			std::string text;
			text.reserve(bytes.size() * 2);
			for (const std::uint8_t b : bytes) {
				text += std::format("{:02x}", b);
			}
			return text;
		};
		std::string out = "asset\tname\ttechset\timages\troot\tconstants\tlist56\ttail296\ttail312\n";
		std::size_t count = 0;
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			if (list.assets[i].type != 0x0A || list.assets[i].header != kPtrInline) {
				continue;
			}
			XStream s(zone.stream, 0);
			s.Restore(trace.assets[i].offset, trace.assets[i].block, trace.assets[i].pos);
			MaterialData material;
			if (!ReadMaterial(s, list.assets[i].header, material)) {
				continue;
			}
			std::string images;
			for (std::size_t e = 0; e + 24 <= material.imageTable.size(); e += 24) {
				const std::uint32_t semantic = Get<std::uint32_t>(material.imageTable, e + 8);
				const auto asset = AssetOfReference(trace, list, Get<std::uint64_t>(material.imageTable, e));
				if (!images.empty()) {
					images += ' ';
				}
				if (asset && list.assets[*asset].type == 0x10) {
					const auto root = stream.subspan(trace.assets[*asset].offset, 208);
					const std::uint32_t format = Get<std::uint32_t>(root, 156);
					images += format ? std::format("{:08X}:{}:{}", semantic, format, root[180])
						: std::format("{:08X}:ref:{:016X}", semantic, Get<std::uint64_t>(root, 0) & ~(1ull << 63));
				}
				else {
					images += std::format("{:08X}:?", semantic);
				}
			}
			const auto techset = AssetOfReference(trace, list, Get<std::uint64_t>(material.root, 40));
			const std::uint64_t techsetName = techset
				? Get<std::uint64_t>(stream.subspan(trace.assets[*techset].offset, 8), 0) & ~(1ull << 63) : 0;
			out += std::format("{}\t{:016X}\t{:016X}\t{}\t{}\t{}\t{}\t{}\t{}\n", i, material.Name(), techsetName, images,
				hex(material.root), hex(material.constants), hex(material.list56), hex(material.tail296), hex(material.tail312));
			++count;
		}
		if (!WriteWholeFile(options.materialTable, std::vector<std::uint8_t>(out.begin(), out.end()))) {
			std::fprintf(stderr, "could not write %s\n", options.materialTable.string().c_str());
			return false;
		}
		std::printf("wrote %zu materials to %s\n", count, options.materialTable.string().c_str());
		return true;
	}

	// Every xmodel's LOD 0 materials, one line per surface: model, the material of the table's +8 handles and the
	// one of its +16 handles (0 when that list is null).
	bool WriteModelMaterials(const Options& options, const LoadedZone& zone, const XAssetList& list) {
		ZoneTrace trace;
		std::string error;
		if (options.trace.empty() || !ReadZoneTrace(options.trace, trace, error) || !AlignZoneTrace(trace, zone.stream, list).empty()) {
			std::fprintf(stderr, "--model-materials needs a trace that fits: %s\n", error.c_str());
			return false;
		}
		const std::span<const std::uint8_t> stream(zone.stream);
		auto nameOf = [&](const XModelData::Material& material) -> std::uint64_t {
			if (material.name) {
				return material.name;
			}
			const auto asset = AssetOfReference(trace, list, material.stored);
			return asset ? Get<std::uint64_t>(stream.subspan(trace.assets[*asset].offset, 8), 0) & ~(1ull << 63) : 0;
		};
		std::string out = "model\tsurface\tmaterial\tmaterial16\n";
		std::size_t models = 0;
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			if (list.assets[i].type != 0x06 || list.assets[i].header != kPtrInline) {
				continue;
			}
			XStream s(zone.stream, 0);
			s.Restore(trace.assets[i].offset, trace.assets[i].block, trace.assets[i].pos);
			XModelData model;
			if (!ReadXModel(s, list.assets[i].header, model) || model.materials.empty()) {
				continue;
			}
			++models;
			for (std::size_t k = 0; k < model.materials[0].size(); ++k) {
				const bool has16 = !model.materials16.empty() && k < model.materials16[0].size();
				out += std::format("{:016X}\t{}\t{:016X}\t{:016X}\n", model.Name(), k, nameOf(model.materials[0][k]),
					has16 ? nameOf(model.materials16[0][k]) : 0);
			}
		}
		if (!WriteWholeFile(options.modelMaterials, std::vector<std::uint8_t>(out.begin(), out.end()))) {
			std::fprintf(stderr, "could not write %s\n", options.modelMaterials.string().c_str());
			return false;
		}
		std::printf("wrote %zu models' LOD 0 materials to %s\n", models, options.modelMaterials.string().c_str());
		return true;
	}

	// An asset's root struct as stored: the first bytes at its traced start (every shipped asset header is
	// inline, so the root is read right there), as u64 words with their offsets.
	bool PrintDumps(const Options& options, const LoadedZone& zone, const XAssetList& list) {
		ZoneTrace trace;
		std::string error;
		if (!ReadZoneTrace(options.trace, trace, error) || !AlignZoneTrace(trace, zone.stream, list).empty()) {
			std::fprintf(stderr, "--dump needs a trace that fits: %s\n", error.c_str());
			return false;
		}
		for (auto [asset, bytes] : options.dumps) {
			if (asset >= list.assets.size() && asset != SIZE_MAX) {
				const std::uint64_t type = SIZE_MAX - 1 - asset;
				asset = SIZE_MAX;
				for (std::size_t i = 0; i < list.assets.size(); ++i) {
					if (list.assets[i].type == type) {
						asset = i;
						break;
					}
				}
			}
			if (asset >= list.assets.size()) {
				std::fprintf(stderr, "--dump: no such asset\n");
				return false;
			}
			const std::size_t at = trace.assets[asset].offset;
			bytes = std::min(bytes, zone.stream.size() - at);
			std::printf("\nasset %zu (%s) root at +0x%zX, %zu bytes:\n", asset,
				std::string(XAssetTypeName(list.assets[asset].type)).c_str(), at, bytes);
			for (std::size_t o = 0; o + 8 <= bytes; o += 8) {
				const auto v = Get<std::uint64_t>(std::span(zone.stream.data() + at, bytes), o);
				if (v) {
					std::printf("  +%-5zu %016llX\n", o, static_cast<unsigned long long>(v));
				}
			}
		}
		return true;
	}

	int WalkScan(const Options& options) {
		const std::vector<fs::path> zones = ZoneFiles(options);
		std::size_t complete = 0, failed = 0;
		std::map<std::uint64_t, std::size_t> blockedBy;
		std::vector<std::string> errors, mismatches;
		for (std::size_t i = 0; i < zones.size(); ++i) {
			std::printf("\r[%zu/%zu] %-48s", i + 1, zones.size(), zones[i].stem().string().c_str());
			std::fflush(stdout);
			LoadedZone zone;
			std::string error;
			if (!LoadZone(zones[i], zone, error)) {
				errors.push_back(zones[i].stem().string() + ": " + error);
				++failed;
				continue;
			}
			const WalkResult walk = WalkStream(zone.stream);
			if (!walk.error.empty()) {
				errors.push_back(zone.name + ": " + walk.error);
				++failed;
			}
			else if (!walk.complete) {
				++blockedBy[walk.blockedType];
			}
			else {
				++complete;
				const std::string blocks = CompareBlockSizes(walk, zone.header.BlockSizes());
				if (!blocks.empty()) {
					mismatches.push_back(zone.name + ":" + blocks);
				}
			}
		}

		std::printf("\n\nloaders:%s\n%zu zones: %zu walk completely (%zu of them with block sizes that differ from "
			"the header), %zu errors\n", LoaderList().c_str(), zones.size(), complete, mismatches.size(), failed);
		for (const std::string& line : errors) {
			std::printf("  ERROR %s\n", line.c_str());
		}
		for (const std::string& line : mismatches) {
			std::printf("  BLOCKS %s\n", line.c_str());
		}
		std::vector<std::pair<std::size_t, std::uint64_t>> sorted;
		for (const auto& [type, count] : blockedBy) {
			sorted.emplace_back(count, type);
		}
		std::sort(sorted.rbegin(), sorted.rend());
		std::printf("first missing loader, by how many zones it stops:\n");
		for (const auto& [count, type] : sorted) {
			std::printf("    %5zu  %s\n", count, std::string(XAssetTypeName(type)).c_str());
		}
		return failed ? 1 : 0;
	}

	// --hk: a tagfile's summary and mapkit's round trip of it.
	int CheckHkFile(const fs::path& path) {
		std::vector<std::uint8_t> bytes;
		if (!ReadWholeFile(path, bytes)) {
			std::fprintf(stderr, "could not read %s\n", path.string().c_str());
			return 1;
		}
		std::span<const std::uint8_t> tagfile = NavPayloadTagfile(bytes);
		const bool payload = !tagfile.empty();
		if (!payload) {
			tagfile = bytes;
		}
		HkTagfile file;
		std::string error;
		if (!ReadHkTagfile(tagfile, file, error)) {
			std::fprintf(stderr, "%s: %s\n", path.string().c_str(), error.c_str());
			return 1;
		}
		std::size_t refs = 0;
		for (const HkItem& item : file.items) {
			refs += item.refs.size();
		}
		std::printf("%s: %s%zu bytes, SDK %.*s, %zu types, %zu items (%zu references), root %s\n", path.filename().string().c_str(),
			payload ? "navmesh payload, tagfile " : "", tagfile.size(), static_cast<int>(file.sdkVersion.size()),
			reinterpret_cast<const char*>(file.sdkVersion.data()), file.types.size(), file.items.size() - 1, refs,
			file.types[file.items[1].type].name.c_str());
		const std::vector<std::uint8_t> written = WriteHkTagfile(file);
		if (written.size() == tagfile.size() && std::equal(written.begin(), written.end(), tagfile.begin())) {
			std::printf("  round trip: identical (%zu bytes)\n", written.size());
			return 0;
		}
		std::size_t first = 0;
		while (first < std::min(written.size(), tagfile.size()) && written[first] == tagfile[first]) {
			++first;
		}
		std::printf("  round trip: DIFFERS: %zu bytes written vs %zu read, first difference at +0x%zX\n", written.size(),
			tagfile.size(), first);
		return 1;
	}

	int Scan(const Options& options) {
		const fs::path zoneDir = options.gameDir / "zone";
		std::vector<fs::path> zones;
		for (const auto& entry : fs::directory_iterator(zoneDir)) {
			if (entry.path().extension() == ".ff") {
				zones.push_back(entry.path());
			}
		}
		std::sort(zones.begin(), zones.end());

		const auto start = std::chrono::steady_clock::now();
		std::size_t failed = 0, patched = 0, assets = 0;
		std::uint64_t streamBytes = 0;
		std::map<std::uint64_t, std::size_t> byType;
		for (std::size_t i = 0; i < zones.size(); ++i) {
			LoadedZone zone;
			XAssetList list;
			std::string error;
			if (!LoadZone(zones[i], zone, error) || !ParseXAssetList(zone.stream, list, error)) {
				++failed;
				std::printf("FAIL %s: %s\n", zones[i].filename().string().c_str(), error.c_str());
				continue;
			}
			patched += zone.patched;
			assets += list.assets.size();
			streamBytes += zone.stream.size();
			for (const XAssetEntry& entry : list.assets) {
				++byType[entry.type];
			}
			std::printf("\r[%zu/%zu] %-48s", i + 1, zones.size(), zone.name.c_str());
			std::fflush(stdout);
		}

		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::printf("\n\n%zu zones, %zu failed, %zu patched by a .fd, %zu assets, %s of xfile streams, %.0f s\n",
			zones.size(), failed, patched, assets, Megabytes(static_cast<std::size_t>(streamBytes)).c_str(), seconds);
		std::printf("asset types present: %zu of %zu\n", byType.size(), kXAssetTypeCount);
		for (const auto& [type, count] : byType) {
			std::printf("    %8zu  %s\n", count, std::string(XAssetTypeName(type)).c_str());
		}
		return failed ? 1 : 0;
	}

	// --- --needs: what a map built on a level zone still takes from it (mapkit plan P6 step 1) --------------------------

	// The engine's 40 bgcache tables and the asset type of their names (g_bgCacheTables 0x7FF72AADB4F0, +0 name, +8
	// type; 0xDD = names with no asset), read from the IDB 2026-09-30. A census carries its own copy.
	constexpr std::array<std::uint8_t, 40> kBgCacheTableTypes = {
		0xDD, 0x4C, 0x06, 0x38, 0x39, 0x3A, 0x21, 0x1E, 0x1F, 0x53, 0x59, 0x67, 0x68, 0x6C, 0x04, 0x79, 0x7A, 0x05, 0x66, 0x57,
		0xAD, 0x20, 0xD4, 0xDB, 0x0A, 0xDD, 0xDD, 0x0A, 0xDD, 0xDD, 0xB6, 0xDD, 0x33, 0xDD, 0xDD, 0xDD, 0xDD, 0x33, 0x34, 0xB6 };
	constexpr std::array<const char*, 40> kBgCacheTableNames = {
		"(none)", "vehicle", "model", "aitype", "character", "xmodelalias", "weapon", "gesture", "gesturetable", "zbarrier",
		"rumble", "shellshock", "statuseffect", "xcam", "destructible", "streamerhint", "flowgraph", "xanim", "sanim",
		"scriptbundle", "talent", "cinematicmotion", "vehicleassembly", "execution", "statusicon", "locationselector", "menu",
		"material", "string", "eventstring", "moviefile", "objective", "fx", "lui_menu_data", "lui_elem", "radiant_exploder",
		"soundalias", "client_fx", "client_tagfxset", "client_lui_elem" };

	struct AssetKey {
		std::uint64_t type = 0;
		std::uint64_t name = 0;
		bool operator==(const AssetKey&) const = default;
	};
	struct AssetKeyHash {
		std::size_t operator()(const AssetKey& k) const noexcept { return k.name ^ (k.type * 0x9E3779B97F4A7C15ull); }
	};

	// Which zones hold an asset once the level has loaded.
	enum Holder : std::uint8_t {
		kHeldByLibrary = 1,   // the level zone or one of its variants (en_, ww_, 1080_/4k_, techset_...)
		kHeldByLevelZone = 2, // the level zone itself
		kHeldElsewhere = 4,   // a zone that stays without the library: zm_common, core, the map's own
	};

	// The level zone's family: the zones DB_ExpandZoneVariants adds for it.
	bool IsLibraryZone(std::string_view zone, std::string_view level) {
		if (zone == level || zone == std::string(level) + "_patch") return true;
		return zone.size() > level.size() + 1 && zone.ends_with(level) && zone[zone.size() - level.size() - 1] == '_';
	}

	struct NeedsResult {
		std::map<std::uint64_t, std::pair<std::size_t, std::size_t>> copy; // type -> assets, stream bytes (level zone)
		std::map<std::uint64_t, std::size_t> variants;                     // type -> assets only the library's variants hold
		std::map<std::uint64_t, std::size_t> unplaced;                     // type -> links by name nobody said who holds
		std::size_t kept = 0;      // held by a zone that stays
		std::size_t missing = 0;   // held by no loaded zone (census only)
		std::size_t nested = 0;    // in the level zone, inside another asset (census only)
		std::vector<std::uint32_t> assets;
		std::vector<AssetKey> variantKeys;
	};

	// Every name of every bgcache the level zone stores itself: what its level lets scripts and entities use.
	std::vector<std::pair<std::uint8_t, std::uint64_t>> LevelBgCacheNames(const LoadedZone& zone, const XAssetList& list,
		const ZoneTrace& trace, const std::vector<std::uint8_t>& byName) {
		std::vector<std::pair<std::uint8_t, std::uint64_t>> out;
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			if (list.assets[i].type != 0x6D || byName[i]) continue;
			// Load_BgcacheAsset: a 24-byte root {name, entries, count @16}, then the 24-byte entries {u8 table; name @8}.
			const std::size_t at = trace.assets[i].offset;
			const auto entries = Get<std::uint64_t>(zone.stream, at + 8);
			const auto count = Get<std::int32_t>(zone.stream, at + 16);
			if (entries != kPtrInline || count <= 0) continue;
			for (std::int32_t e = 0; e < count; ++e) {
				const std::size_t entry = at + 24 + 24 * static_cast<std::size_t>(e);
				if (entry + 24 > zone.stream.size()) break;
				out.emplace_back(zone.stream[entry], Get<std::uint64_t>(zone.stream, entry + 8) & ~(1ull << 63));
			}
		}
		return out;
	}

	// The own assets (not links by name) of a zone that stays loaded, from its trace or, without one, a full walk.
	bool KeptZoneNames(const Options& options, const std::string& spec, std::vector<AssetKey>& out, std::string& error) {
		const std::size_t colon = spec.find(':', 2);
		const std::string name = spec.substr(0, colon);
		Options zoneOptions = options;
		zoneOptions.zone = name;
		LoadedZone kept;
		XAssetList keptList;
		if (!LoadZone(ResolveZone(zoneOptions), kept, error) || !ParseXAssetList(kept.stream, keptList, error)) return false;
		std::vector<std::size_t> starts;
		if (colon != std::string::npos) {
			ZoneTrace trace;
			if (!ReadZoneTrace(spec.substr(colon + 1), trace, error)) return false;
			if (const std::string mismatch = AlignZoneTrace(trace, kept.stream, keptList); !mismatch.empty()) {
				error = std::format("{}: the trace does not fit: {}", name, mismatch);
				return false;
			}
			for (std::size_t i = 0; i < keptList.assets.size(); ++i) starts.push_back(trace.assets[i].offset);
		}
		else {
			const WalkResult walk = WalkStream(kept.stream);
			if (!walk.complete) {
				error = std::format("{}: mapkit cannot walk it ({}); give its trace: --keep {}:<trace>", name,
					walk.error.empty() ? std::format("no loader for {}", XAssetTypeName(walk.blockedType)) : walk.error, name);
				return false;
			}
			for (const WalkedAsset& a : walk.assets) starts.push_back(a.begin);
		}
		for (std::size_t i = 0; i < keptList.assets.size(); ++i) {
			const std::uint64_t type = keptList.assets[i].type;
			const auto raw = Get<std::uint64_t>(kept.stream, starts[i] + XAssetNameOffset(type));
			if (!(raw >> 63)) out.push_back({ type, raw });
		}
		return true;
	}

	bool PrintNeeds(const Options& options, const LoadedZone& zone, const XAssetList& list) {
		ZoneTrace trace;
		std::string error;
		if (options.trace.empty() || !ReadZoneTrace(options.trace, trace, error)) {
			std::fprintf(stderr, "--needs needs the level zone's trace (--trace <file>) %s\n", error.c_str());
			return false;
		}
		if (const std::string mismatch = AlignZoneTrace(trace, zone.stream, list); !mismatch.empty() || !trace.complete) {
			std::fprintf(stderr, "--needs: the trace does not fit: %s\n", mismatch.empty() ? "it has no end record" : mismatch.c_str());
			return false;
		}
		const std::string level = zone.name;
		const std::size_t count = list.assets.size();

		const auto started = std::chrono::steady_clock::now();
		const RefGraph graph = BuildRefGraph(trace, zone.stream, list);
		if (!graph.error.empty()) {
			std::fprintf(stderr, "--needs: %s\n", graph.error.c_str());
			return false;
		}
		std::size_t links = 0, inner = 0;
		for (const auto& l : graph.links) links += l.size();
		for (const auto& l : graph.inner) inner += l.size();
		std::printf("\nwhat a map built on %s takes from it (mapkit plan P6)\n", level.c_str());
		std::printf("  links             %zu between its %zu assets (%zu found through the asset list, %zu to the start of "
			"another asset's data), %.1f s\n", links, count, graph.entryLinks, graph.rootLinks,
			std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
		std::printf("  not followed      %zu pointers into the middle of another asset's data (%zu asset pairs): mostly data "
			"stored once and shared, not a use of the asset around it (ref_graph.hpp)\n", graph.innerPointers, inner);

		// Each asset's name, and whether it is a link by name to an asset of another zone (its root holds only the name).
		std::vector<std::uint64_t> names(count);
		std::vector<std::uint8_t> byName(count);
		std::unordered_map<AssetKey, std::uint32_t, AssetKeyHash> own;
		std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> typesOfName;
		for (std::size_t i = 0; i < count; ++i) {
			const std::uint64_t type = list.assets[i].type;
			const auto raw = Get<std::uint64_t>(zone.stream, trace.assets[i].offset + XAssetNameOffset(type));
			names[i] = raw & ~(1ull << 63);
			byName[i] = static_cast<std::uint8_t>(raw >> 63);
			if (!byName[i]) own.emplace(AssetKey{ type, names[i] }, static_cast<std::uint32_t>(i));
			auto& types = typesOfName[names[i]];
			if (std::ranges::find(types, type) == types.end()) types.push_back(type);
		}
		if (!options.needsGraph.empty()) {
			std::ofstream f(options.needsGraph, std::ios::trunc);
			f << std::format("# {}'s link graph: '<index> <type> #<name hash> <stream bytes> own|link <linked indices...>', "
				"~<index> for a pointer into the middle of that asset's data (not a link)\n", level);
			for (std::size_t i = 0; i < count; ++i) {
				f << std::format("{} {} #{:016x} {} {}", i, XAssetTypeName(list.assets[i].type), names[i], graph.bytes[i],
					byName[i] ? "link" : "own");
				for (const std::uint32_t j : graph.links[i]) f << ' ' << j;
				for (const std::uint32_t j : graph.inner[i]) f << " ~" << j;
				f << '\n';
			}
			std::printf("  graph written to %s\n", options.needsGraph.string().c_str());
		}

		// Who holds what: the census knows every loaded asset's zone. Without one, only the level zone's own assets
		// and those of the zones given with --keep are known.
		std::unordered_map<AssetKey, std::uint8_t, AssetKeyHash> holders;
		UsageFile usage;
		const bool haveUsage = !options.usage.empty();
		if (haveUsage) {
			if (!ReadUsageFile(options.usage, usage, error)) {
				std::fprintf(stderr, "--usage: %s\n", error.c_str());
				return false;
			}
			std::array<std::uint8_t, 256> kind{};
			std::string library, others;
			for (const UsageZone& z : usage.zones) {
				const std::string name(z.name, strnlen(z.name, sizeof(z.name)));
				kind[z.index & 0xFF] = name == level ? (kHeldByLibrary | kHeldByLevelZone)
					: IsLibraryZone(name, level) ? kHeldByLibrary : kHeldElsewhere;
				((kind[z.index & 0xFF] & kHeldByLibrary) ? library : others) += " " + name;
			}
			for (const UsageEntry& e : usage.entries) holders[{ e.type, e.name }] |= kind[e.zone];
			const std::time_t written = static_cast<std::time_t>(usage.header.time);
			char when[32]{};
			std::tm local{};
			localtime_s(&local, &written);
			std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &local);
			std::printf("  census            %s: map '%s', written %s; %zu loaded assets in %zu zones\n",
				options.usage.filename().string().c_str(), usage.Map().c_str(), when, usage.entries.size(), usage.zones.size());
			std::printf("  library zones    %s\n  zones that stay  %s\n", library.c_str(), others.c_str());
		}
		else {
			for (const auto& [key, index] : own) holders[key] |= kHeldByLibrary | kHeldByLevelZone;
		}
		for (const std::string& keep : options.keeps) {
			std::vector<AssetKey> keys;
			if (!KeptZoneNames(options, keep, keys, error)) {
				std::fprintf(stderr, "--keep %s: %s\n", keep.c_str(), error.c_str());
				return false;
			}
			for (const AssetKey& key : keys) holders[key] |= kHeldElsewhere;
			std::printf("  kept zone         %s: %zu assets of its own\n", keep.substr(0, keep.find(':', 2)).c_str(), keys.size());
		}

		// The map's own zone stays loaded: what it holds itself (its world, under the library's names) is not the
		// library's to give. Its links by name are what it uses; each carries the type its loader read it as.
		std::vector<AssetKey> mapLinks;
		std::vector<AssetKey> mapOwn;
		if (!options.mapZone.empty()) {
			LoadedZone mapZone;
			XAssetList mapList;
			if (!LoadZone(options.mapZone, mapZone, error) || !ParseXAssetList(mapZone.stream, mapList, error)) {
				std::fprintf(stderr, "--map-zone: %s\n", error.c_str());
				return false;
			}
			std::vector<XStream::RootRecord> roots;
			const WalkResult mapWalk = WalkStream(mapZone.stream, &roots);
			if (!mapWalk.complete) {
				std::fprintf(stderr, "--map-zone: mapkit cannot walk %s: %s\n", options.mapZone.string().c_str(),
					mapWalk.error.empty() ? "a type has no loader" : mapWalk.error.c_str());
				return false;
			}
			for (std::size_t i = 0; i < mapWalk.assets.size(); ++i) {
				const std::uint64_t type = mapList.assets[i].type;
				const auto raw = Get<std::uint64_t>(mapZone.stream, mapWalk.assets[i].begin + XAssetNameOffset(type));
				if (!(raw >> 63)) {
					holders[{ type, raw }] |= kHeldElsewhere;
					mapOwn.push_back({ type, raw });
				}
			}
			std::printf("  map zone          %s: %zu assets of its own; links by name:\n", options.mapZone.filename().string().c_str(), mapOwn.size());
			std::unordered_set<AssetKey, AssetKeyHash> once;
			for (const XStream::RootRecord& root : roots) {
				const std::uint64_t name = ByNameReference(std::span<const std::uint8_t>(mapZone.stream).subspan(root.cursor, root.size));
				if (!name) continue;
				std::vector<std::uint64_t> types;
				if (root.type != XStream::kNoRootType) types.push_back(root.type);
				else if (const auto it = typesOfName.find(name); it != typesOfName.end()) types = it->second;
				for (const std::uint64_t type : types) {
					if (!once.insert({ type, name }).second) continue;
					mapLinks.push_back({ type, name });
					const bool levelOwn = own.contains({ type, name });
					std::printf("    %-20s #%016llx  %s\n", std::string(XAssetTypeName(type)).c_str(), static_cast<unsigned long long>(name),
						levelOwn ? std::format("{}'s own", level).c_str() : typesOfName.contains(name) ? "a link of the library's too"
							: "not in the library");
				}
				if (types.empty()) std::printf("    %-20s #%016llx  type unknown, not in the library\n", "?", static_cast<unsigned long long>(name));
			}
		}

		auto held = [&](const AssetKey& key) -> std::uint8_t {
			const auto it = holders.find(key);
			return it == holders.end() ? 0 : it->second;
		};

		// Walks from what the map uses through every link, stopping at what a zone that stays holds.
		auto walk = [&](const std::vector<AssetKey>& start) {
			NeedsResult r;
			std::vector<std::uint8_t> seen(count, 0);
			std::unordered_set<AssetKey, AssetKeyHash> seenKeys;
			std::vector<std::uint32_t> queue;
			auto visit = [&](std::uint32_t j) {
				if (seen[j]) return;
				seen[j] = 1;
				const AssetKey key{ list.assets[j].type, names[j] };
				const std::uint8_t h = held(key);
				if (h & kHeldElsewhere) {
					++r.kept;
				}
				else if (!byName[j]) {
					queue.push_back(j);
				}
				else if (h & kHeldByLibrary) {
					++r.variants[key.type];
					r.variantKeys.push_back(key);
				}
				else if (haveUsage) {
					++r.missing;
				}
				else {
					++r.unplaced[key.type];
				}
			};
			for (const AssetKey& key : start) {
				if (!seenKeys.insert(key).second) continue;
				if (const auto it = own.find(key); it != own.end()) {
					visit(it->second);
					continue;
				}
				const std::uint8_t h = held(key);
				if (h & kHeldElsewhere) ++r.kept;
				else if (h & kHeldByLevelZone) ++r.nested;
				else if (h & kHeldByLibrary) {
					++r.variants[key.type];
					r.variantKeys.push_back(key);
				}
				else if (haveUsage) ++r.missing;
				else ++r.unplaced[key.type];
			}
			while (!queue.empty()) {
				const std::uint32_t i = queue.back();
				queue.pop_back();
				auto& t = r.copy[list.assets[i].type];
				++t.first;
				t.second += graph.bytes[i];
				r.assets.push_back(i);
				for (const std::uint32_t j : graph.links[i]) visit(j);
			}
			return r;
		};

		auto print = [&](const NeedsResult& r) {
			std::size_t assets = 0, bytes = 0;
			std::vector<std::pair<std::size_t, std::uint64_t>> byBytes;
			for (const auto& [type, t] : r.copy) {
				assets += t.first;
				bytes += t.second;
				byBytes.emplace_back(t.second, type);
			}
			std::sort(byBytes.rbegin(), byBytes.rend());
			std::printf("    to copy from %s.ff: %zu assets, %s of its stream (of %s)\n", level.c_str(), assets,
				Megabytes(bytes).c_str(), Megabytes(zone.stream.size()).c_str());
			std::printf("      %-24s %8s %12s  %s\n", "type", "assets", "stream", "mapkit");
			for (const auto& [typeBytes, type] : byBytes) {
				std::printf("      %-24s %8zu %12s  %s\n", std::string(XAssetTypeName(type)).c_str(), r.copy.at(type).first,
					Megabytes(typeBytes).c_str(), HasAssetLoader(type) ? "reads it" : "no loader yet");
			}
			if (!r.variants.empty()) {
				std::size_t total = 0;
				std::string line;
				for (const auto& [type, n] : r.variants) {
					total += n;
					line += std::format(" {} {},", XAssetTypeName(type), n);
				}
				line.pop_back();
				std::printf("    held only by %s's other zones (techset_, en_, ww_, 1080_/4k_): %zu:%s\n", level.c_str(), total,
					line.c_str());
			}
			if (!r.unplaced.empty()) {
				std::size_t total = 0;
				std::string line;
				for (const auto& [type, n] : r.unplaced) {
					total += n;
					line += std::format(" {} {},", XAssetTypeName(type), n);
				}
				line.pop_back();
				std::printf("    links by name to zones not given (no census; zm_common, core or %s's variants): %zu:%s\n",
					level.c_str(), total, line.c_str());
			}
			std::printf("    held by zones that stay: %zu", r.kept);
			if (haveUsage) std::printf("; held by no loaded zone: %zu; inside another %s asset: %zu", r.missing, level.c_str(), r.nested);
			std::printf("\n");
		};

		// The ceiling: every name the level's own bgcache lists, as if the map used all of it.
		const auto listed = LevelBgCacheNames(zone, list, trace, byName);
		{
			std::vector<AssetKey> start;
			std::size_t nameOnly = 0;
			for (const auto& [table, name] : listed) {
				const std::uint32_t type = haveUsage ? usage.TableType(table) : table < kBgCacheTableTypes.size() ? kBgCacheTableTypes[table] : kBgCacheNoAsset;
				if (type == kBgCacheNoAsset) ++nameOnly;
				else start.push_back({ type, name });
			}
			std::printf("\n  if a map used every name %s's bgcache lists (%zu names; %zu of them are not assets: sound aliases, "
				"strings, menus):\n", level.c_str(), listed.size(), nameOnly);
			print(walk(start));
		}

		// What the map used: the census's lookups (not the level load's registration of every listed name) and the map
		// zone's own links by name. The renderer's stream-ins are sorted out after the rest.
		std::vector<AssetKey> start;
		std::vector<std::uint32_t> startSource; // each name's source (Source::index)
		struct Source {
			std::size_t names = 0;
			std::size_t own = 0; // of them the level zone's own
			std::uint32_t index = 0;
		};
		std::map<std::string, Source> bySource;
		std::map<std::string, std::size_t> nameOnly;
		std::size_t registration = 0, fromOutside = 0, beforeMatch = 0;
		std::vector<std::uint32_t> streamedIn; // level zone assets the renderer streamed in
		auto add = [&](const AssetKey& key, const std::string& source) {
			const auto [it, added] = bySource.try_emplace(source);
			if (added) it->second.index = static_cast<std::uint32_t>(bySource.size() - 1);
			start.push_back(key);
			startSource.push_back(it->second.index);
			++it->second.names;
			if (own.contains(key) && !(held(key) & kHeldElsewhere)) ++it->second.own;
		};
		if (haveUsage) {
			// The census counts from the game's start: a lookup an earlier census (the frontend's) already had, not made
			// again since, is not the match's.
			using LookupKey = std::tuple<std::uint32_t, std::uint64_t, std::uint64_t>;
			std::map<LookupKey, std::uint32_t> baseFinds, baseCaches;
			if (!options.usageBase.empty()) {
				UsageFile base;
				if (!ReadUsageFile(options.usageBase, base, error)) {
					std::fprintf(stderr, "--usage-base: %s\n", error.c_str());
					return false;
				}
				for (const UsageLookup& l : base.finds) baseFinds[{ l.kind, l.name, l.caller }] = l.count;
				for (const UsageLookup& l : base.caches) baseCaches[{ l.kind, l.name, l.caller }] = l.count;
			}
			auto before = [](const std::map<LookupKey, std::uint32_t>& base, const UsageLookup& l) {
				const auto it = base.find(LookupKey{ l.kind, l.name, l.caller });
				return it != base.end() && l.count <= it->second;
			};
			std::unordered_set<AssetKey, AssetKeyHash> once;
			for (const UsageLookup& l : usage.caches) {
				if (IsRegistrationCaller(l.caller)) {
					++registration;
					continue;
				}
				if (before(baseCaches, l)) {
					++beforeMatch;
					continue;
				}
				const std::uint32_t type = usage.TableType(l.kind);
				if (type == kBgCacheNoAsset) {
					++nameOnly[usage.TableName(l.kind)];
					continue;
				}
				if (once.insert({ type, l.name }).second) add({ type, l.name }, "bgcache " + usage.TableName(l.kind));
			}
			for (const UsageLookup& l : usage.finds) {
				if (IsRegistrationCaller(l.caller)) {
					++registration;
					continue;
				}
				if (before(baseFinds, l)) {
					++beforeMatch;
					continue;
				}
				if (!l.caller) {
					++fromOutside;
					continue;
				}
				if (IsStreamInCaller(l.caller)) {
					const AssetKey key{ l.kind, l.name };
					if (const auto it = own.find(key); it != own.end() && !(held(key) & kHeldElsewhere)) streamedIn.push_back(it->second);
					continue;
				}
				if (once.insert({ l.kind, l.name }).second) add({ l.kind, l.name }, std::format("lookup {}", XAssetTypeName(l.kind)));
			}
		}
		for (const AssetKey& key : mapLinks) add(key, "map zone link");
		if (start.empty() && streamedIn.empty()) {
			std::printf("\n  (no census and no map zone: only the ceiling above. Run a match with \"mapkit_usage\": true in cw-mod.json,"
				" then --usage <file>.)\n");
			return true;
		}

		// The stream-ins count only when nothing else explains them. One that only the library's world links to (the
		// assets the map zone replaces with its own of the same type and name, and all they reach) was streamed because
		// that world is loaded: zm_navtest, 2026-09-30, streamed 6,720 of zm_silver's world meshes, images and streamkeys
		// that the map neither links to nor names anywhere in its zone.
		std::ranges::sort(streamedIn);
		streamedIn.erase(std::ranges::unique(streamedIn).begin(), streamedIn.end());
		std::size_t streamCounted = 0;
		std::map<std::uint64_t, std::size_t> streamReplaced, streamUnexplained; // type -> assets
		if (!streamedIn.empty()) {
			std::vector<std::uint8_t> counted(count, 0);
			for (const std::uint32_t i : walk(start).assets) counted[i] = 1;
			std::vector<std::uint8_t> replaced(count, 0);
			std::vector<std::uint32_t> stack;
			for (const AssetKey& key : mapOwn) {
				if (const auto it = own.find(key); it != own.end() && !replaced[it->second]) {
					replaced[it->second] = 1;
					stack.push_back(it->second);
				}
			}
			while (!stack.empty()) {
				const std::uint32_t i = stack.back();
				stack.pop_back();
				for (const std::uint32_t j : graph.links[i]) {
					if (!replaced[j]) {
						replaced[j] = 1;
						stack.push_back(j);
					}
				}
			}
			for (const std::uint32_t i : streamedIn) {
				const std::uint64_t type = list.assets[i].type;
				if (counted[i]) {
					++streamCounted;
				}
				else if (replaced[i]) {
					++streamReplaced[type];
				}
				else {
					++streamUnexplained[type];
					add({ type, names[i] }, std::format("stream-in {}", XAssetTypeName(type)));
				}
			}
		}
		auto byType = [](const std::map<std::uint64_t, std::size_t>& counts) {
			std::size_t total = 0;
			std::string line;
			for (const auto& [type, n] : counts) {
				total += n;
				line += std::format("{}{} {}", line.empty() ? "" : ", ", XAssetTypeName(type), n);
			}
			return std::format("{}{}", total, line.empty() ? "" : " (" + line + ")");
		};

		std::printf("\n  what the map used: %zu names", start.size());
		if (haveUsage) {
			std::printf(" (%zu lookups the level load made for every listed name left out, %zu made by cw-mod itself", registration, fromOutside);
			if (!options.usageBase.empty()) std::printf(", %zu made before the match", beforeMatch);
			std::printf(")");
		}
		std::printf("\n");
		if (!streamedIn.empty()) {
			std::printf("    the renderer streamed %zu of %s's own assets in: %zu are counted through what links to them; %s only %s's\n"
				"    world links to, the world the map zone replaces (streamed while %s.ff is loaded, not counted); %s nothing else\n"
				"    explains (counted, as 'stream-in')\n", streamedIn.size(), level.c_str(), streamCounted, byType(streamReplaced).c_str(),
				level.c_str(), level.c_str(), byType(streamUnexplained).c_str());
		}

		// What each source alone brings in: the assets the count would lose without its names.
		const NeedsResult used = walk(start);
		std::vector<std::pair<std::size_t, std::size_t>> alone(bySource.size()); // assets, stream bytes
		for (std::uint32_t s = 0; s < alone.size(); ++s) {
			std::vector<AssetKey> without;
			for (std::size_t k = 0; k < start.size(); ++k) {
				if (startSource[k] != s) without.push_back(start[k]);
			}
			std::vector<std::uint8_t> reached(count, 0);
			for (const std::uint32_t i : walk(without).assets) reached[i] = 1;
			for (const std::uint32_t i : used.assets) {
				if (!reached[i]) {
					++alone[s].first;
					alone[s].second += graph.bytes[i];
				}
			}
		}
		std::printf("    %-28s %8s %6s %8s %10s   (own: %s's own names; only it: the assets to copy that no other source reaches)\n",
			"from", "names", "own", "only it", "its stream", level.c_str());
		for (const auto& [source, s] : bySource) {
			std::printf("    %-28s %8zu %6zu %8zu %10s\n", source.c_str(), s.names, s.own, alone[s.index].first,
				Megabytes(alone[s.index].second).c_str());
		}
		if (!nameOnly.empty()) {
			std::string line;
			for (const auto& [table, n] : nameOnly) line += std::format(" {} {},", table, n);
			line.pop_back();
			std::printf("    names that are not assets:%s\n", line.c_str());
		}
		print(used);

		// The largest single assets to copy.
		std::vector<std::uint32_t> largest = used.assets;
		std::ranges::sort(largest, [&](std::uint32_t a, std::uint32_t b) { return graph.bytes[a] > graph.bytes[b]; });
		std::printf("    largest:");
		for (std::size_t k = 0; k < std::min<std::size_t>(largest.size(), 12); ++k) {
			const std::uint32_t i = largest[k];
			std::printf("%s %s #%016llx %s", k ? "," : "", std::string(XAssetTypeName(list.assets[i].type)).c_str(),
				static_cast<unsigned long long>(names[i]), Megabytes(graph.bytes[i]).c_str());
		}
		std::printf("\n");

		if (!options.needsList.empty()) {
			std::ofstream f(options.needsList, std::ios::trunc);
			f << std::format("# what a map built on {} takes from it: 'copy <index> <type> <name hash> <stream bytes>' for the "
				"level zone's own assets, 'variant <type> <name hash>' for those only its other zones hold\n", level);
			std::vector<std::uint32_t> sorted = used.assets;
			std::ranges::sort(sorted);
			for (const std::uint32_t i : sorted) {
				f << std::format("copy {} {} #{:016x} {}\n", i, XAssetTypeName(list.assets[i].type), names[i], graph.bytes[i]);
			}
			for (const AssetKey& key : used.variantKeys) {
				f << std::format("variant {} #{:016x}\n", XAssetTypeName(key.type), key.name);
			}
			std::printf("    list written to %s\n", options.needsList.string().c_str());
		}
		return true;
	}
}

int main(int argc, char** argv) {
	Options options;
	if (!ParseArgs(argc, argv, options)) {
		Usage();
		return 2;
	}

	if (!options.hkFile.empty()) {
		return CheckHkFile(options.hkFile);
	}
	std::string error;
	if (!Oodle::Load(options.gameDir, error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	if (options.scan) {
		return Scan(options);
	}
	if (options.walkScan) {
		return WalkScan(options);
	}

	LoadedZone zone;
	XAssetList list;
	const fs::path path = ResolveZone(options);
	if (!LoadZone(path, zone, error) || !ParseXAssetList(zone.stream, list, error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	PrintZone(zone, list, options.listAssets);
	if (options.walk && !PrintWalk(zone, list, options.listAssets)) {
		return 1;
	}
	if (options.entities && !PrintEntities(zone, options.entitiesJson, options.entitiesMap)) {
		return 1;
	}
	const bool traceReport = !options.world && !options.clipMap && !options.gfx && !options.levelAssets && !options.libraryAssets
		&& options.checkCopies.empty()
		&& options.meshAsset == SIZE_MAX
		&& !options.residentMeshes && options.material.empty() && !options.images && !options.residentMaterials
		&& !options.semantics && !options.needs;
	if (!options.trace.empty() && traceReport && !PrintTrace(zone, list, options.trace)) {
		return 1;
	}
	if (options.world && !PrintWorld(zone, list, options.trace, options.cell)) {
		return 1;
	}
	if (options.clipMap && !PrintClipMap(zone, list, options.trace)) {
		return 1;
	}
	if (options.gfx && !PrintGfx(zone, list, options.trace)) {
		return 1;
	}
	if (options.levelAssets && !PrintLevelAssets(zone, list, options.trace)) {
		return 1;
	}
	if (options.libraryAssets && !PrintLibraryAssets(zone, list, options.trace)) {
		return 1;
	}
	if (!options.checkCopies.empty() && !PrintCopyCheck(zone, list, options.trace, options.checkCopies)) {
		return 1;
	}
	if (!options.dumps.empty() && !PrintDumps(options, zone, list)) {
		return 1;
	}
	if (options.meshAsset != SIZE_MAX && !PrintMesh(options, zone, list)) {
		return 1;
	}
	if (options.residentMeshes && !PrintResidentMeshes(options, zone, list)) {
		return 1;
	}
	if (!options.material.empty() && !PrintMaterial(options, zone, list)) {
		return 1;
	}
	if (options.images && !PrintImages(options, zone, list)) {
		return 1;
	}
	if (options.residentMaterials && !PrintResidentMaterials(options, zone, list)) {
		return 1;
	}
	if (options.semantics && !PrintSemantics(options, zone, list)) {
		return 1;
	}
	if (!options.materialTable.empty() && !WriteMaterialTable(options, zone, list)) {
		return 1;
	}
	if (!options.modelMaterials.empty() && !WriteModelMaterials(options, zone, list)) {
		return 1;
	}
	if (options.needs && !PrintNeeds(options, zone, list)) {
		return 1;
	}

	if (!options.streamOut.empty()) {
		if (!WriteWholeFile(options.streamOut, zone.stream)) {
			std::fprintf(stderr, "could not write %s\n", options.streamOut.string().c_str());
			return 1;
		}
		std::printf("\nwrote the stream to %s\n", options.streamOut.string().c_str());
	}

	if (!options.repackOut.empty()) {
		std::vector<std::uint8_t> packed;
		if (!WriteFastFile(zone.header, zone.stream, packed, error) || !WriteWholeFile(options.repackOut, packed)) {
			std::fprintf(stderr, "repack failed: %s\n", error.empty() ? "could not write the file" : error.c_str());
			return 1;
		}

		FastFileHeader reread;
		std::vector<std::uint8_t> stream;
		if (!ReadFastFile(packed, reread, stream, nullptr, error)) {
			std::fprintf(stderr, "repacked file does not read back: %s\n", error.c_str());
			return 1;
		}
		const bool same = stream == zone.stream;
		std::printf("\nrepacked to %s (%s): stream round-trip %s\n", options.repackOut.string().c_str(),
			Megabytes(packed.size()).c_str(), same ? "OK" : "MISMATCH");
		if (!same) {
			return 1;
		}
	}
	return 0;
}
