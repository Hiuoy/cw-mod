// Custom map zones from cw-mod/maps. See mapkit_loader.hpp for the why; dump_anchors.hpp for the addresses.
#include "common.hpp"
#include "game/mapkit_loader.hpp"
#include "game/mapkit_live.hpp"
#include "game/mapkit_usage.hpp"
#include "game/game_internal.hpp"
#include "game/arxan_call.hpp"
#include "game/dump_anchors.hpp"
#include "game/settings.hpp"
#include "game/function_types.hpp"
#include "scripting/scripting.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace Client::Game::MapKit {
	namespace {
		namespace F = Functions;
		namespace fs = std::filesystem;

		enum class Phase : int { Off, Mount, Mounted };

		constexpr std::size_t kMaxEvents = 40;
		constexpr std::size_t kMapNameSize = 64;
		constexpr std::uint32_t kMaxRedirectZones = 8;   // DB_LoadXAssets level loads pass 1 or 2
		constexpr std::uint32_t kZoneFlags_Level = 0x300; // 0x100 map load, 0x200 lobby preload
		constexpr std::uint32_t kZoneFlag_Preload = 0x200;
		constexpr int kMaxExpandedZones = 32;             // both DB_ExpandZoneVariants callers pass 32
		// The override zones `cwlink patch` / `cwlink plane` write: the region texture-tier variants.
		constexpr std::array<const char*, 2> kOverridePrefixes = { "ww_1080_", "ww_4k_" };

		std::atomic<Phase> g_Phase{ Phase::Off };

		// A listed overlay map: the base's two override zone names and the map's own.
		struct Overlay {
			std::string id;
			std::string base;                  // the retail map zone, e.g. zm_silver
			std::array<std::string, 2> from;   // ww_1080_<base>, ww_4k_<base>
			std::array<std::string, 2> to;     // ww_1080_<id>, ww_4k_<id>
		};

		// Written once by Init, before the hooks exist; read without a lock from the DB threads. AddActiveMap
		// hands the engine pointers into g_Overlays, so it never changes after Init.
		std::unordered_set<std::string> g_CustomZones;
		std::unordered_map<std::string, std::string> g_OverrideZones; // override zone -> its (retail) map zone
		std::vector<Overlay> g_Overlays;
		std::uintptr_t g_ModuleBase = 0;

		std::atomic<int> g_ActiveOverlay{ -1 }; // index into g_Overlays of the active map, -1 = none

		// A map of its own (mapkit_loader.hpp 7.): <id>.ff, started under its own name, <library>'s zones under it.
		struct OwnMap {
			std::string id;
			std::string library;
			bool wholeLibrary = true;       // every zone of the library loads under it; else only `keep` (P6 2b-2)
			std::vector<std::string> keep;  // map.json "library": the library's zones that load
			std::uint64_t ownBsp = 0;       // maps/<p>/<id>.d3dbsp: the name the engine looks the level's world up by
			std::uint64_t libraryBsp = 0;   // maps/<p>/<library>.d3dbsp: the name the map's world has (7.)
			std::uint64_t idHash = 0;       // the maptable's key for the map (MapTable_FindEntryByHash)
			std::uint64_t libraryHash = 0;
		};

		// What a map of its own's library contributes, for the log: "zm_silver's zones" or "only techset_zm_silver of zm_silver".
		std::string LibraryText(const OwnMap& map) {
			if (map.wholeLibrary) return std::format("{}'s zones", map.library);
			std::string list;
			for (const std::string& zone : map.keep) list += (list.empty() ? "" : ", ") + zone;
			return std::format("only {} of {}'s zones", list.empty() ? std::string("none") : list, map.library);
		}
		// Written once by Init, before the hooks exist; read without a lock from any thread.
		std::vector<OwnMap> g_OwnMaps;
		std::atomic<int> g_LevelOwnMap{ -1 };   // the map of its own this PC loads or plays, -1 = none
		std::atomic<int> g_PickedOwnMap{ -1 };  // the CUSTOM MAPS pick when it is a map of its own (the host)
		std::atomic<bool> g_ReloadScripts{ false }; // the level map changed: Tick reloads the served scripts
		std::array<std::atomic<std::uint32_t>, 8> g_AliasLogged{}; // world types already logged as aliased, 256 bits
		std::string g_LastZoneList;             // the last changed zone list logged (guarded by g_Lock)
		std::string g_LastLobbyMap;             // the last lobby map logged (guarded by g_Lock)
		std::string g_LastHostMap;              // the last map a member checked it can load (guarded by g_Lock)
		constexpr std::uint64_t kNameMask63 = 0x7FFFFFFFFFFFFFFFull;
		thread_local std::array<F::XZoneInfo, 16> t_Libraries{};

		std::mutex g_Lock; // guards everything below
		std::vector<MapFolder> g_Folders;
		std::string g_RedirectFrom;
		std::string g_RedirectTo;
		std::string g_Active;           // the listed map picked in the CUSTOM MAPS tab, "" = none
		bool g_RedirectByPick = false;  // the redirect is the active clone map's, not the Maps tab's
		std::deque<std::string> g_Events;
		std::unordered_set<std::string> g_HiddenLogged;

		// The names handed to the engine stay valid after the call: it may keep the pointer.
		std::array<char, kMapNameSize> g_StartName{};
		std::array<char, kMapNameSize> g_PreloadName{};
		std::string g_PreloadLogged; // "from>to" of the last logged preload redirect (it runs every frame)

		// DB_LoadXAssets copies the names before it returns, so a per-thread copy outlives the call.
		thread_local std::array<F::XZoneInfo, kMaxRedirectZones> t_Zones{};
		thread_local std::array<char, kMapNameSize> t_ZoneName{};

		struct Api {
			F::FS_AddSearchPathT* AddSearchPath{};
			F::DB_ZoneFileExistsT* ZoneFileExists{};
			F::IntFnT* GetLanguage{};
			F::Loc_GetLanguagePrefixT* LanguagePrefix{};
			F::Session_SetMapNameT* SetMapName{};         // through our detour
			F::Session_PickActiveSlotT* PickActiveSlot{};
			char* zoneSigName{};
			const std::uint64_t* mainThreadId{};
			char* preloadName{};
			const std::uint8_t* zoneRows{};
		} g_Api;

		std::string Lower(std::string s) {
			std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		fs::path GameDir() {
			std::error_code ec;
			return fs::current_path(ec);
		}

		void Event(std::string line) {
			LOG("MapKit", INFO, "{}", line);
			std::lock_guard lock(g_Lock);
			g_Events.push_back(std::move(line));
			while (g_Events.size() > kMaxEvents) g_Events.pop_front();
		}

		bool OnMainThread() {
			std::uint64_t main = 0;
			return SafeRead(g_Api.mainThreadId, main) && main == GetCurrentThreadId();
		}

		// Every name DB_ExpandZoneVariants produces for a level zone (flags 0x100): the zone itself, the
		// language, region, texture-tier and techset variants. Both tiers, since the player can switch.
		std::vector<std::string> ExpectedZones(const std::string& map, const std::string& langPrefix) {
			std::vector<std::string> out = { map, "techset_" + map, "ww_" + map, "1080_" + map, "4k_" + map,
				"ww_1080_" + map, "ww_4k_" + map };
			if (!langPrefix.empty()) out.push_back(langPrefix + map);
			return out;
		}

		std::string ZoneName(const F::XZoneInfo& zone) {
			return zone.name ? Lower(std::string(zone.name, strnlen(zone.name, kMapNameSize))) : std::string();
		}

		// The engine's asset name hash: FNV-1a 64 over the lowercased text, top bit cleared.
		std::uint64_t HashName(std::string_view text) {
			std::uint64_t h = 0xCBF29CE484222325ull;
			for (const char c : text) {
				h ^= static_cast<std::uint8_t>(std::tolower(static_cast<unsigned char>(c)));
				h *= 0x100000001B3ull;
			}
			return h & kNameMask63;
		}

		// Com_GetMapBspName: maps/<p>/<map>.d3dbsp, <p> being the text before the first '_'.
		std::string BspName(const std::string& map) {
			return std::format("maps/{}/{}.d3dbsp", map.substr(0, map.find('_')), map);
		}

		int FindOwnMap(std::string_view name) {
			for (std::size_t i = 0; i < g_OwnMaps.size(); ++i) {
				if (_strnicmp(g_OwnMaps[i].id.c_str(), name.data(), name.size()) == 0 && g_OwnMaps[i].id.size() == name.size()) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		// A zone DB_ExpandZoneVariants makes of `map`: <prefix>_<map> (en_, ww_, 1080_, 4k_, ww_1080_, ww_4k_, techset_)
		// or <map>_patch.
		bool IsVariantOf(const std::string& zone, const std::string& map) {
			return zone.size() > map.size() && (zone.ends_with("_" + map) || zone == map + "_patch");
		}

		// Sets the host's lobby map through Session_SetMapName (so through our detour, which then passes it on), on the
		// session slot the playlist apply uses. Main thread.
		void PushLobbyMap(const std::string& map) {
			if (!g_Api.SetMapName || !g_Api.PickActiveSlot) {
				Event(std::format("lobby map '{}' NOT set: Session_SetMapName did not resolve.", map));
				return;
			}
			const int slot = g_Api.PickActiveSlot(0);
			g_Api.SetMapName(static_cast<std::uint32_t>(slot), map.c_str());
			Event(std::format("lobby map set to '{}' (session slot {}): the lobby preloads it and sends it to every member.",
				map, slot));
		}

		// A member's lobby map comes from the host. A PC that cannot load it drops at the launch with 0x48342502
		// (Com_LoadLevelFastFiles: the map's .ff header does not read, "Zed 453 Kinetic Devil"), which says nothing about
		// why. Says it, once per map, when the host sets it.
		void CheckHostMap(const std::string& map) {
			{
				std::lock_guard lock(g_Lock);
				if (map == g_LastHostMap) return;
				g_LastHostMap = map;
			}
			if (map.empty() || FindOwnMap(map) >= 0) return;
			std::error_code ec;
			if (fs::exists(GameDir() / "zone" / (map + ".ff"), ec)) return; // a retail map
			std::string title;
			bool known = false;
			{
				std::lock_guard lock(g_Lock);
				for (const MapFolder& folder : g_Folders) {
					if (folder.name != map) continue;
					if (std::ranges::find(folder.zones, map) != folder.zones.end()) return; // <map>.ff is here: it loads
					known = true;
					title = folder.title;
				}
			}
			LOG("MapKit", WARN, "{}", known
				? std::format("the host's map '{}' is a map of its own, but cw-mod/maps/{} ('{}') on this PC is an older build "
					"of it with no {}.ff: the match will not load here (0x48342502). Install the host's build of the map.",
					map, map, title, map)
				: std::format("the host's map '{}' is not installed on this PC (neither a retail zone nor a folder in "
					"cw-mod/maps): the match will not load here (0x48342502).", map));
		}

		// Zone_FindByName, read in place: the status of the loaded zone with this name, -1 = no row.
		int ZoneStatus(const std::string& zone) {
			for (int i = 0; i < kZoneInfoRowCount; ++i) {
				const std::uint8_t* row = g_Api.zoneRows + i * kZoneInfoRowSize;
				char name[kMapNameSize]{};
				if (!SafeCopy(name, row, sizeof(name) - 1)) return -1;
				if (_stricmp(name, zone.c_str()) != 0) continue;
				int status = -1;
				SafeRead(row + 68, status);
				return status;
			}
			return -1;
		}

		// The redirect target for mapName, or "" when none applies or it is refused (the reason is then in
		// refusal, for the caller to log).
		std::string RedirectTarget(const char* mapName, std::string& refusal) {
			if (!mapName) return {};
			std::string from, to;
			std::vector<std::string> missing;
			bool known = false;
			bool overlay = false;
			{
				std::lock_guard lock(g_Lock);
				from = g_RedirectFrom;
				to = g_RedirectTo;
				for (const MapFolder& folder : g_Folders) {
					if (folder.name == to) {
						known = true;
						overlay = folder.listed && folder.overlay;
						missing = folder.missing;
					}
				}
			}
			if (to.empty() || _stricmp(mapName, from.c_str()) != 0) return {};
			if (!known || g_Phase.load() != Phase::Mounted) {
				refusal = std::format("redirect to '{}' refused, that map is not mounted", to);
				return {};
			}
			if (overlay) {
				// It has no zone of its own to start: it is laid over its base (pick it in CUSTOM MAPS).
				refusal = std::format("redirect to '{}' refused, it is an overlay map with no {}.ff", to, to);
				return {};
			}
			if (!missing.empty()) {
				std::string list;
				for (const std::string& z : missing) list += " " + z;
				refusal = std::format("redirect to '{}' refused, its zone set is missing{}", to, list);
				return {};
			}
			return to;
		}

		void DoMount() {
			const char* prefixRaw = g_Api.LanguagePrefix(static_cast<unsigned int>(g_Api.GetLanguage()));
			const std::string langPrefix = prefixRaw ? prefixRaw : "";

			std::vector<MapFolder> folders;
			{
				std::lock_guard lock(g_Lock);
				folders = g_Folders;
			}
			for (MapFolder& folder : folders) {
				g_Api.AddSearchPath(folder.dir.c_str(), kMapKitSearchPathPriority, kMapKitSearchPathDevice, 0);
			}

			// Checked after every folder is mounted: a variant may come from the retail zone dir (a replaced
			// retail map keeps its retail variants) or, in principle, from another folder.
			for (MapFolder& folder : folders) {
				folder.missing.clear();
				// An overlay map loads only its two zones, over its base's whole set; a map of its own loads its one zone
				// after its asset library's whole set, or after the zones of it its map.json lists.
				std::vector<std::string> expected = folder.listed && folder.overlay
					? std::vector<std::string>{ folder.base, "ww_1080_" + folder.name, "ww_4k_" + folder.name }
					: folder.listed && folder.standalone ? std::vector<std::string>{ folder.base, folder.name }
					: ExpectedZones(folder.name, langPrefix);
				if (folder.listed && folder.standalone && folder.partialLibrary) {
					expected = folder.library;
					expected.push_back(folder.name);
				}
				for (const std::string& zone : expected) {
					if (!g_Api.ZoneFileExists(zone.c_str())) folder.missing.push_back(zone);
				}
				std::string zones;
				for (const std::string& z : folder.zones) zones += " " + z;
				std::string missing;
				for (const std::string& z : folder.missing) missing += " " + z;
				const std::string listed = !folder.listed ? std::string()
					: folder.standalone ? std::format(", '{}', a map of its own with {}{}'s asset library, listed in CUSTOM "
						"MAPS", folder.title, folder.partialLibrary ? "part of " : "", folder.base)
					: std::format(", '{}', {} of {}, live only when picked in CUSTOM MAPS", folder.title,
						folder.overlay ? "an overlay" : "a clone", folder.base);
				Event(std::format("mounted '{}' (search path {}, priority {}{}{}): zones{}. {}", folder.name, folder.dir,
					kMapKitSearchPathPriority, folder.replacesRetail ? ", replaces the retail zone" : "", listed, zones,
					folder.missing.empty() ? std::string("Zone set complete.")
						: std::format("MISSING{} - starting this map would drop with 0xC21445D4.", missing)));
			}

			{
				std::lock_guard lock(g_Lock);
				g_Folders = std::move(folders);
			}
			g_Phase = Phase::Mounted;
		}

		// <folder>/map.json, what `cwlink build` writes: {"title", "base", "description", "playlist"}. Listed
		// only when it parses, the folder is not named like a retail map, its base is one, and it ships its own
		// zones: ww_1080_<name> and ww_4k_<name> (an overlay) or <name>.ff (a clone). Logs why not.
		void ReadMapJson(MapFolder& folder, const fs::path& retailZoneDir) {
			const fs::path path = fs::path(folder.dir) / "map.json";
			std::error_code ec;
			if (!fs::exists(path, ec)) return;
			std::string why;
			try {
				std::stringstream text;
				text << std::ifstream(path, std::ios::binary).rdbuf();
				const nlohmann::json j = nlohmann::json::parse(text.str());
				folder.title = j.value("title", folder.name);
				folder.base = Lower(j.value("base", std::string()));
				folder.description = j.value("description", std::string());
				folder.playlist = j.value("playlist", 0);
				folder.standalone = j.value("format", std::string()) == "standalone";
				if (const auto library = j.find("library"); library != j.end() && library->is_array()) {
					folder.partialLibrary = true;
					for (const auto& zone : *library) {
						if (zone.is_string()) folder.library.push_back(Lower(zone.get<std::string>()));
					}
				}
			}
			catch (const std::exception& e) {
				why = std::format("it does not parse: {}", e.what());
			}
			const auto ships = [&](const std::string& zone) { return std::ranges::find(folder.zones, zone) != folder.zones.end(); };
			const bool clone = ships(folder.name);
			if (why.empty() && folder.standalone && !clone) {
				why = std::format("it is a map of its own (\"format\": \"standalone\") with no {}.ff", folder.name);
			}
			if (why.empty() && folder.replacesRetail) {
				why = "the folder is named like a retail map";
			}
			if (why.empty() && (folder.base.empty() || !fs::exists(retailZoneDir / (folder.base + ".ff"), ec))) {
				why = std::format("its \"base\" '{}' is not a retail map", folder.base);
			}
			if (why.empty() && !clone && (!ships("ww_1080_" + folder.name) || !ships("ww_4k_" + folder.name))) {
				why = std::format("it ships neither {}.ff nor both ww_1080_{}.ff and ww_4k_{}.ff", folder.name, folder.name,
					folder.name);
			}
			if (!why.empty()) {
				LOG("MapKit", WARN, "cw-mod/maps/{}/map.json ignored, {}: the map is not listed in CUSTOM MAPS.", folder.name, why);
				return;
			}
			folder.listed = true;
			folder.overlay = !clone;
		}
	}

	void Init(std::uintptr_t moduleBase, std::size_t imageSize) {
		if (!Settings::Get().customMaps) {
			LOG("MapKit", INFO, "\"custom_maps\" is off in cw-mod.json: custom maps OFF.");
			return;
		}

		const fs::path root = GameDir() / "cw-mod" / "maps";
		const fs::path retailZoneDir = GameDir() / "zone";
		std::vector<MapFolder> folders;
		std::error_code ec;
		if (fs::is_directory(root, ec)) {
			for (const auto& entry : fs::directory_iterator(root, ec)) {
				if (!entry.is_directory(ec)) continue;
				MapFolder folder;
				folder.name = Lower(entry.path().filename().string());
				folder.dir = entry.path().string();
				for (const auto& file : fs::directory_iterator(entry.path(), ec)) {
					if (Lower(file.path().extension().string()) == ".ff") {
						folder.zones.push_back(Lower(file.path().stem().string()));
					}
				}
				if (folder.zones.empty()) continue;
				std::ranges::sort(folder.zones);
				folder.replacesRetail = fs::exists(retailZoneDir / (folder.name + ".ff"), ec);
				ReadMapJson(folder, retailZoneDir);
				folders.push_back(std::move(folder));
			}
		}
		if (folders.empty()) {
			LOG("MapKit", INFO, "Custom maps OFF: no cw-mod/maps/<map>/*.ff (nothing is hooked).");
			return;
		}
		if (!ArxanCall::Ready()) {
			LOG("MapKit", ERROR, "Custom maps OFF: no Arxan return gadget for the engine calls.");
			return;
		}

		std::string missing;
		auto addr = [&](std::uintptr_t dumpAbs) -> void* {
			const std::uintptr_t a = moduleBase + (dumpAbs - kDumpImagebase);
			return (a >= moduleBase && a < moduleBase + imageSize) ? reinterpret_cast<void*>(a) : nullptr;
		};
		auto thunk = [&]<typename Fn>(Fn*& out, std::uintptr_t dumpAbs, const char* name) {
			void* a = addr(dumpAbs);
			out = a ? ArxanCall::MakeThunk<Fn>(a) : nullptr;
			if (!out) missing += std::format(" {}", name);
		};
		thunk(g_Api.AddSearchPath, kDump_FS_AddSearchPath, "FS_AddSearchPath");
		thunk(g_Api.ZoneFileExists, kDump_DB_ZoneFileExists, "DB_ZoneFileExists");
		thunk(g_Api.GetLanguage, kDump_Loc_GetLanguage, "Loc_GetLanguage");
		thunk(g_Api.LanguagePrefix, kDump_Loc_GetLanguagePrefix, "Loc_GetLanguagePrefix");
		thunk(g_Api.SetMapName, kDump_Session_SetMapName, "Session_SetMapName");
		thunk(g_Api.PickActiveSlot, kDump_Session_PickActiveSlot, "Session_PickActiveSlot");
		g_Api.zoneSigName = static_cast<char*>(addr(kDump_g_zoneSigName));
		g_Api.mainThreadId = static_cast<const std::uint64_t*>(addr(kDump_g_mainThreadId));
		g_Api.preloadName = static_cast<char*>(addr(kDump_g_mapPreloadName));
		g_Api.zoneRows = static_cast<const std::uint8_t*>(addr(kDump_g_zoneInfoRows));
		if (!g_Api.zoneSigName || !g_Api.mainThreadId) missing += " g_zoneSigName/g_mainThreadId";
		if (!g_Api.preloadName || !g_Api.zoneRows) missing += " g_mapPreloadName/g_zoneInfoRows";
		if (!missing.empty()) {
			LOG("MapKit", ERROR, "Custom maps OFF: unresolved{}.", missing);
			return;
		}

		g_ModuleBase = moduleBase;
		Live::Init(moduleBase);
		std::string list;
		for (const MapFolder& folder : folders) {
			for (const std::string& zone : folder.zones) g_CustomZones.insert(zone);
			list += std::format(" {} ({} zone{}{})", folder.name, folder.zones.size(), folder.zones.size() == 1 ? "" : "s",
				!folder.listed ? std::string() : folder.standalone
					? std::format(", listed: '{}', its own map on {}", folder.title, folder.base)
					: std::format(", listed: '{}' over {}", folder.title, folder.base));
			if (folder.listed && folder.standalone) {
				OwnMap map;
				map.id = folder.name;
				map.library = folder.base;
				map.wholeLibrary = !folder.partialLibrary;
				map.keep = folder.library;
				map.ownBsp = HashName(BspName(folder.name));
				map.libraryBsp = HashName(BspName(folder.base));
				map.idHash = HashName(folder.name);
				map.libraryHash = HashName(folder.base);
				// cwlink writes <id>.ff alone. A variant next to it is left over from an older build of the map: the
				// ww_1080_/ww_4k_ zones of an overlay hold a second world under the library's names, and loading it
				// beside <id>.ff's overflows the world's asset pools (0xE554F200 on the DB thread, a fatal sys_error,
				// 2026-09-29 22:05). TrimOwnMapVariants drops them all.
				std::string leftovers;
				for (const std::string& zone : folder.zones) {
					if (IsVariantOf(zone, folder.name)) leftovers += std::format(" {}.ff", zone);
				}
				if (!leftovers.empty()) {
					LOG("MapKit", WARN, "cw-mod/maps/{} also has{}: zones of an older build of the map. A map of its own never "
						"loads them; delete them.", folder.name, leftovers);
				}
				g_OwnMaps.push_back(std::move(map));
				continue;
			}
			if (folder.listed && folder.overlay) {
				// Its zones stand in for the base's two override zones, so they link after the base as those would.
				Overlay overlay{ folder.name, folder.base, {}, {} };
				for (std::size_t k = 0; k < kOverridePrefixes.size(); ++k) {
					overlay.from[k] = kOverridePrefixes[k] + folder.base;
					overlay.to[k] = kOverridePrefixes[k] + folder.name;
					g_OverrideZones.emplace(overlay.to[k], folder.base);
				}
				g_Overlays.push_back(std::move(overlay));
				continue;
			}
			// An override folder leaves the map's own zone retail. A folder that ships <map>.ff (a replaced or
			// cloned map) keeps the engine's order: its variants are the ones the map itself references.
			if (!folder.replacesRetail || std::ranges::find(folder.zones, folder.name) != folder.zones.end()) continue;
			for (const char* prefix : kOverridePrefixes) {
				const std::string zone = prefix + folder.name;
				if (std::ranges::find(folder.zones, zone) != folder.zones.end()) g_OverrideZones.emplace(zone, folder.name);
			}
		}
		{
			std::lock_guard lock(g_Lock);
			g_Folders = std::move(folders);
		}
		g_Phase = Phase::Mount;
		LOG("MapKit", WARN, "Custom maps ON:{}. Their zones load unsigned from cw-mod/maps; retail zones are "
			"still verified. Opt out with \"custom_maps\": false in cw-mod.json.", list);
		for (const auto& [zone, map] : g_OverrideZones) {
			LOG("MapKit", INFO, "override zone '{}': loads and links after '{}' (the engine lists it first).", zone, map);
		}
		for (const OwnMap& map : g_OwnMaps) {
			LOG("MapKit", INFO, "map of its own '{}': loads after {}, its world has {}'s names{} (a world lookup by {:016X} goes "
				"to {:016X}), maptable entry {}'s.", map.id, LibraryText(map), map.library,
				map.wholeLibrary ? " and replaces that map's world" : "", map.ownBsp, map.libraryBsp, map.library);
		}
	}

	bool Enabled() { return g_Phase.load(std::memory_order_relaxed) != Phase::Off; }

	void LogWorldInit() {
		// gfx_map offsets (zonekit/gfx_world.cpp): +8 name, +1008 draw model groups (u64), +1120 terrain, +1128
		// group LOD entries (u64), +7448 lighting, +7456 streamerworld (+0 name hash, +80 cells, +176 models).
		auto at = [](std::uint64_t base, std::size_t offset) { return reinterpret_cast<const void*>(base + offset); };
		std::uint64_t globals = 0, world = 0;
		if (!SafeRead(reinterpret_cast<const void*>(g_ModuleBase + (kDump_g_rendererGlobals - kDumpImagebase)), globals)
			|| !SafeRead(at(globals, kRendererWorldOffset), world) || !world) {
			LOG("MapKit", WARN, "world start: no gfx_map in the renderer globals");
			return;
		}
		std::uint64_t namePtr = 0, groups = 0, terrain = 0, lods = 0, lighting = 0, streamer = 0;
		SafeRead(at(world, 8), namePtr);
		SafeRead(at(world, 1008), groups);
		SafeRead(at(world, 1120), terrain);
		SafeRead(at(world, 1128), lods);
		SafeRead(at(world, 7448), lighting);
		SafeRead(at(world, 7456), streamer);
		std::string name;
		for (std::size_t i = 0; i < 63 && namePtr; ++i) {
			char c = 0;
			if (!SafeRead(at(namePtr, i), c) || !c) break;
			name += c;
		}
		std::uint64_t streamerName = 0, cells = 0, models = 0;
		SafeRead(at(streamer, 0), streamerName);
		SafeRead(at(streamer, 80), cells);
		SafeRead(at(streamer, 176), models);
		const bool own = groups == 0 && terrain == 0;
		LOG("MapKit", INFO, "world start: gfx_map '{}' ({}): {} model groups, terrain {}, {} group LOD entries, lighting {:#x}; "
			"its streamerworld {:016X}: {} models, {} cells{}", name, own ? "mapkit's own" : "the map's own", groups,
			terrain ? "yes" : "none", lods, lighting, streamerName, models, cells,
			own ? "" : " (Die Maschine's buildings draw from its model groups)");
		Live::OnWorldStart(own);
		MapKitUsage::OnWorldStart(ActiveMap());
	}

	void Tick() {
		Live::Tick();
		MapKitUsage::Tick();
		const Phase phase = g_Phase.load(std::memory_order_relaxed);
		if (phase == Phase::Off || !OnMainThread()) return;
		if (phase == Phase::Mount) {
			DoMount();
			return;
		}
		// The map this PC loads changed (a lobby preload or a level load, WithLibraries): its level scripts are the ones
		// served from now on. The lobby preloads long before the launch, so this runs well before the level asks for them.
		if (g_ReloadScripts.exchange(false) && Client::Scripting::LoaderActive()) {
			const std::string map = ActiveMap();
			Event(std::format("level scripts: {}. {}", map.empty() ? std::string("no custom map's")
				: std::format("maps/{}/scripts served", map), Client::Scripting::ReloadScripts()));
		}
	}

	bool SkipSignatureCheck() {
		char raw[kMapNameSize]{};
		if (!SafeCopy(raw, g_Api.zoneSigName, sizeof(raw))) return false;
		std::string name = Lower(std::string(raw, strnlen(raw, sizeof(raw))));
		if (name.ends_with(".ff")) name.resize(name.size() - 3);
		if (!g_CustomZones.contains(name)) return false;

		// What the original does once it has checked: clear the name, the signature copy and the accumulator.
		std::memset(g_Api.zoneSigName, 0, kZoneSigStateBytes);
		Event(std::format("zone '{}' loaded from cw-mod/maps: signature check skipped.", name));
		return true;
	}

	bool HidesPatchFile(const char* path) {
		if (!path) return false;
		const std::size_t length = strnlen(path, 512);
		if (length < 4 || _stricmp(path + length - 3, ".fd") != 0) return false;
		const char* slash = path + length;
		while (slash > path && slash[-1] != '/' && slash[-1] != '\\') --slash;
		const std::string stem = Lower(std::string(slash, path + length - 3));
		if (!g_CustomZones.contains(stem)) return false;

		bool first;
		{
			std::lock_guard lock(g_Lock);
			first = g_HiddenLogged.insert(stem).second;
		}
		if (first) Event(std::format("'{}.fd' hidden: a custom zone is never patched.", stem));
		return true;
	}

	const char* RedirectPreload(const char* mapName) {
		std::string refusal;
		const std::string to = RedirectTarget(mapName, refusal);
		const std::string key = to.empty() ? std::string() : std::format("{}>{}", mapName, to);
		if (key != g_PreloadLogged) {
			// Logged once per change: the lobby calls this every frame while the redirect is active.
			g_PreloadLogged = key;
			if (!to.empty()) {
				Event(std::format("lobby preload '{}' -> '{}': the lobby loads the custom zone.", mapName, to));
			}
		}
		if (!refusal.empty()) Event(std::format("lobby preload '{}': {}.", mapName, refusal));
		if (to.empty()) return mapName;

		// MapPreload_Frame passes g_mapPreloadName itself; renamed in place, its later checks
		// (MapPreload_IsPreloaded for a kind 2 start) all see the target.
		if (mapName == g_Api.preloadName) {
			std::snprintf(g_Api.preloadName, kMapPreloadNameSize, "%s", to.c_str());
			return g_Api.preloadName;
		}
		std::snprintf(g_PreloadName.data(), g_PreloadName.size(), "%s", to.c_str());
		return g_PreloadName.data();
	}

	F::XZoneInfo* RedirectZones(F::XZoneInfo* zones, std::uint32_t count, int freeFlags, std::uintptr_t caller) {
		if (!zones || count == 0 || g_Phase.load(std::memory_order_relaxed) != Phase::Mounted) return zones;
		{
			std::lock_guard lock(g_Lock);
			if (g_RedirectTo.empty()) return zones;
		}

		std::string list;
		bool level = false;
		int renamed = -1;
		std::string to;
		for (std::uint32_t i = 0; i < count; ++i) {
			F::XZoneInfo zone{};
			if (!SafeRead(&zones[i], zone)) return zones;
			char name[kMapNameSize]{};
			if (zone.name) SafeCopy(name, zone.name, sizeof(name) - 1);
			list += std::format("{}{} 0x{:X}", i ? ", " : "", name, zone.flags);
			if (!(zone.flags & kZoneFlags_Level)) continue;
			level = true;
			std::string refusal;
			const std::string target = RedirectTarget(name, refusal);
			if (!refusal.empty()) list += std::format(" ({})", refusal);
			if (target.empty() || renamed >= 0) continue;
			renamed = static_cast<int>(i);
			to = target;
			list += std::format(" -> {}", target);
		}
		// Only level loads are worth a line: the lobby preload and the launch (a handful per match).
		if (!level) return zones;

		const std::uintptr_t rva = caller - g_ModuleBase;
		if (renamed >= 0 && count > kMaxRedirectZones) {
			Event(std::format("zone load [{}] free 0x{:X} from +0x{:X} (dump 0x{:X}): NOT redirected, {} zones is more "
				"than {}.", list, freeFlags, rva, rva + kDumpImagebase, count, kMaxRedirectZones));
			return zones;
		}
		Event(std::format("zone load [{}] free 0x{:X} from +0x{:X} (dump 0x{:X}).", list, freeFlags, rva,
			rva + kDumpImagebase));
		if (renamed < 0) return zones;

		for (std::uint32_t i = 0; i < count; ++i) SafeRead(&zones[i], t_Zones[i]);
		std::snprintf(t_ZoneName.data(), t_ZoneName.size(), "%s", to.c_str());
		t_Zones[renamed].name = t_ZoneName.data();
		return t_Zones.data();
	}

	F::XZoneInfo* WithLibraries(F::XZoneInfo* zones, int& count) {
		if (g_OwnMaps.empty() || !zones || count <= 0 || g_Phase.load(std::memory_order_relaxed) != Phase::Mounted) {
			return zones;
		}
		// The map a level load or lobby preload is for: [{map, 0x200}] (preload), [{map, 0x100}] (the preloaded map at
		// launch), [{<mode>_common, 0x10}, {map, 0x100}] (Com_LoadLevelFastFiles). Any other list leaves it as it was.
		if (count <= 2 && (zones[count - 1].flags & kZoneFlags_Level)) {
			const std::string map = ZoneName(zones[count - 1]);
			const int own = FindOwnMap(map);
			const int previous = g_LevelOwnMap.exchange(own);
			if (previous != own) {
				g_ReloadScripts = true;
				Event(own >= 0
					? (g_OwnMaps[own].wholeLibrary
						? std::format("loading '{}', a map of its own: {}'s zones load first as its asset library, its world "
							"replaces theirs, its level scripts are its own.", map, g_OwnMaps[own].library)
						: std::format("loading '{}', a map of its own: {} load first, its zone holds the rest of what it takes "
							"from {} and is the level zone, its level scripts are its own.", map, LibraryText(g_OwnMaps[own]),
							g_OwnMaps[own].library))
					: std::format("loading '{}': no map of its own{}.", map,
						previous >= 0 ? std::format(" ('{}' no longer loads)", g_OwnMaps[previous].id) : std::string()));
			}
			if (own < 0 && g_Api.ZoneFileExists && !map.empty() && !g_Api.ZoneFileExists(map.c_str())) {
				Event(std::format("the lobby's map '{}' is not installed on this PC: it is neither a retail zone nor a folder in "
					"cw-mod/maps. Loading it will fail.", map));
			}
		}
		int inserted = 0;
		for (int i = 0; i < count; ++i) {
			if ((zones[i].flags & kZoneFlags_Level) && FindOwnMap(ZoneName(zones[i])) >= 0) ++inserted;
		}
		if (inserted == 0) return zones;
		if (count + inserted > static_cast<int>(t_Libraries.size())) {
			Event(std::format("a map of its own in a load of {} zones: NOT given its asset library (at most {}).", count,
				t_Libraries.size()));
			return zones;
		}
		int n = 0;
		for (int i = 0; i < count; ++i) {
			const int own = (zones[i].flags & kZoneFlags_Level) ? FindOwnMap(ZoneName(zones[i])) : -1;
			if (own >= 0) {
				t_Libraries[n] = zones[i];
				t_Libraries[n].name = g_OwnMaps[own].library.c_str();
				++n;
			}
			t_Libraries[n++] = zones[i];
		}
		count = n;
		return t_Libraries.data();
	}

	int TrimOwnMapVariants(F::XZoneInfo* zones, int count) {
		if (g_OwnMaps.empty() || !zones || count <= 0 || count > kMaxExpandedZones
			|| g_Phase.load(std::memory_order_relaxed) != Phase::Mounted) {
			return count;
		}
		// The maps of their own this list loads: of a library their map.json lists zones of, only those stay.
		std::vector<const OwnMap*> loading;
		for (int i = 0; i < count; ++i) {
			const std::string name = ZoneName(zones[i]);
			for (const OwnMap& map : g_OwnMaps) {
				if (name == map.id) loading.push_back(&map);
			}
		}
		bool changed = false;
		std::string list;
		int kept = 0;
		for (int i = 0; i < count; ++i) {
			const std::string name = ZoneName(zones[i]);
			bool drop = false;
			for (const OwnMap& map : g_OwnMaps) {
				if (name == map.id) {
					// Over its whole library it is an override zone (its world replaces the library's in the swap); over
					// part of it, the level zone, whose world the renderer loads (R_BeginLoadWorld: flags 0x140, none of
					// 0x1278000).
					if (map.wholeLibrary) zones[i].flags |= kOverrideZoneFlags;
					changed = true;
					break;
				}
				if (IsVariantOf(name, map.id)) {
					drop = true;
					changed = true;
					break;
				}
			}
			for (const OwnMap* map : loading) {
				if (!drop && !map->wholeLibrary && (name == map->library || IsVariantOf(name, map->library))
					&& std::ranges::find(map->keep, name) == map->keep.end()) {
					drop = true;
					changed = true;
				}
			}
			if (drop) continue;
			list += std::format("{}{} 0x{:X}", list.empty() ? "" : ", ", name, zones[i].flags);
			zones[kept++] = zones[i];
		}
		if (changed) {
			bool first;
			{
				std::lock_guard lock(g_Lock);
				first = g_LastZoneList != list;
				if (first) g_LastZoneList = list;
			}
			if (first) Event(std::format("zone list for a map of its own: [{}]", list));
		}
		return kept;
	}

	const char* HostLobbyMap(std::uint32_t slot, const char* mapName, std::uintptr_t caller) {
		if (!mapName || g_Phase.load(std::memory_order_relaxed) != Phase::Mounted) return mapName;
		const char* result = mapName;
		std::string why;
		const int picked = g_PickedOwnMap.load(std::memory_order_relaxed);
		const std::uintptr_t apply = g_ModuleBase + (kDump_Lobby_ApplySessionSettings_cand - kDumpImagebase);
		const bool member = caller >= apply && caller < apply + kLobby_ApplySessionSettingsSize;
		if (picked >= 0 && !member && _stricmp(mapName, g_OwnMaps[picked].library.c_str()) == 0) {
			result = g_OwnMaps[picked].id.c_str();
			why = " (the CUSTOM MAPS pick in place of its asset library, the playlist's map)";
		}
		const std::string line = std::format("lobby map '{}'{}{} (slot {}, {})", result,
			result != mapName ? std::format(" <- '{}'", mapName) : std::string(), why, slot,
			member ? "from the host's settings" : std::format("caller +0x{:X}", caller - g_ModuleBase));
		bool first;
		{
			std::lock_guard lock(g_Lock);
			first = g_LastLobbyMap != line;
			if (first) g_LastLobbyMap = line;
		}
		if (first) Event(line);
		if (member) CheckHostMap(Lower(mapName));
		return result;
	}

	std::uint64_t WorldAssetName(std::uint32_t type, std::uint64_t name) {
		const int level = g_LevelOwnMap.load(std::memory_order_relaxed);
		if (level < 0) return name;
		const OwnMap& map = g_OwnMaps[static_cast<std::size_t>(level)];
		if ((name & kNameMask63) != map.ownBsp || type >= 256) return name;
		const std::uint32_t bit = 1u << (type & 31);
		if (!(g_AliasLogged[type >> 5].fetch_or(bit, std::memory_order_relaxed) & bit)) {
			LOG("MapKit", INFO, "world asset 0x{:X} of '{}' looked up under {}'s name (the map's zone replaces the ones it "
				"ships).", type, map.id, map.library);
		}
		return (name & ~kNameMask63) | map.libraryBsp;
	}

	std::uint64_t MapTableHash(std::uint64_t mapHash) {
		for (const OwnMap& map : g_OwnMaps) {
			if ((mapHash & kNameMask63) == map.idHash) {
				static std::atomic<bool> s_Logged{ false };
				if (!s_Logged.exchange(true)) {
					LOG("MapKit", INFO, "maptable: '{}' found by hash as {}'s entry (getmapfields, the loading screen); logged once.",
						map.id, map.library);
				}
				return (mapHash & ~kNameMask63) | map.libraryHash;
			}
		}
		return mapHash;
	}

	const char* MapTableName(const char* mapName) {
		if (!mapName) return mapName;
		for (const OwnMap& map : g_OwnMaps) {
			if (_stricmp(mapName, map.id.c_str()) == 0) {
				static std::atomic<bool> s_Logged{ false };
				if (!s_Logged.exchange(true)) {
					LOG("MapKit", INFO, "maptable: '{}' flags read as {}'s; logged once.", map.id, map.library);
				}
				return map.library.c_str();
			}
		}
		return mapName;
	}

	int AddActiveMap(F::XZoneInfo* zones, int count, int max) {
		const int active = g_ActiveOverlay.load(std::memory_order_relaxed);
		if (active < 0 || !zones || count < 1 || count > max || max > kMaxExpandedZones
			|| g_Phase.load(std::memory_order_relaxed) != Phase::Mounted) {
			return count;
		}
		const Overlay& map = g_Overlays[static_cast<std::size_t>(active)];

		// The map's zones go right after the base's own zone, which the engine lists after its variants: they
		// link after it (their by-name references into the map resolve), load after everything the base loads,
		// and so are freed first. The base's own override zones stay: the lobby may already have preloaded
		// them, and zone memory is a stack freed in list order (see dump_anchors.hpp "Zone memory").
		int base = -1;
		std::array<F::XZoneInfo, 2> add{};
		std::array<std::size_t, 2> addTier{};
		std::size_t adds = 0;
		for (int i = 0; i < count; ++i) {
			const std::string name = ZoneName(zones[i]);
			if (name == Lower(map.base)) base = i;
			for (std::size_t k = 0; k < map.from.size(); ++k) {
				if (name != map.from[k]) continue;
				// A lobby preload of the base stays stock: the pick may still change before the match starts,
				// and a preloaded zone the launch does not ask for is never freed in order. The launch adds them.
				if (zones[i].flags & kZoneFlag_Preload) continue;
				add[adds] = zones[i];
				add[adds].name = map.to[k].c_str();
				addTier[adds++] = k;
			}
		}
		if (adds == 0) return count;
		if (base < 0 || count + static_cast<int>(adds) > max) {
			Event(std::format("custom map '{}': NOT loaded, {}.", map.id, base < 0
				? std::format("'{}' is not in this load", map.base)
				: std::format("{} zones is the most this load takes", max)));
			return count;
		}
		const int at = base + 1;
		std::memmove(&zones[at + adds], &zones[at], static_cast<std::size_t>(count - at) * sizeof(F::XZoneInfo));
		for (std::size_t j = 0; j < adds; ++j) {
			zones[at + static_cast<int>(j)] = add[j];
			Event(std::format("custom map '{}': '{}' (flags 0x{:X}) loads after '{}', over its '{}'.", map.id,
				map.to[addTier[j]], add[j].flags, map.base, map.from[addTier[j]]));
		}
		return count + static_cast<int>(adds);
	}

	void OrderOverrideZones(F::XZoneInfo* zones, int count) {
		// Only once mounted: before that the retail zone of that name is the one that loads.
		if (!zones || count < 2 || count > kMaxExpandedZones || g_OverrideZones.empty()
			|| g_Phase.load(std::memory_order_relaxed) != Phase::Mounted) {
			return;
		}
		for (int i = 0; i < count; ++i) {
			const std::string name = ZoneName(zones[i]);
			const auto it = g_OverrideZones.find(name);
			if (it == g_OverrideZones.end()) continue;
			int map = -1;
			for (int j = i + 1; j < count && map < 0; ++j) {
				if (ZoneName(zones[j]) == it->second) map = j;
			}
			if (map < 0) continue; // already after the map, or the map is not in this load

			const F::XZoneInfo moved = zones[i];
			std::memmove(&zones[i], &zones[i + 1], static_cast<std::size_t>(map - i) * sizeof(F::XZoneInfo));
			zones[map] = moved;
			Event(std::format("zone order: '{}' (flags 0x{:X}) now links after '{}', so its references into the map "
				"resolve.", name, moved.flags, it->second));
			--i; // zones[i] is the next entry now
		}
	}

	const char* RedirectMap(const char* mapName, std::uint32_t kind) {
		std::string refusal;
		const std::string to = RedirectTarget(mapName, refusal);
		if (!refusal.empty()) Event(std::format("map start '{}': {}.", mapName, refusal));
		if (to.empty()) return mapName;

		// Kinds 1 and 2 (the lobby launch) start the zone the lobby preloaded and load nothing themselves.
		// Only RedirectPreload can have put the target there; without it, starting the target would die on
		// its first asset lookup, so the retail map starts instead.
		if (kind == kStartMapKindLoaded || kind == kStartMapKindPreloaded) {
			const int status = ZoneStatus(to);
			if (status <= 0 || status == kZoneStatusFailed) {
				Event(std::format("map start '{}' (kind {}): redirect to '{}' refused, the lobby did not preload it "
					"(zone status {}). Set the redirect in the Maps tab BEFORE opening the lobby; starting '{}'.",
					mapName, kind, to, status, mapName));
				return mapName;
			}
			Event(std::format("map start '{}' (kind {}): '{}' is preloaded (zone status {}).", mapName, kind, to, status));
		}

		std::snprintf(g_StartName.data(), g_StartName.size(), "%s", to.c_str());
		Event(std::format("map start '{}' -> '{}' (kind {}).", mapName, to, kind));
		return g_StartName.data();
	}

	std::vector<MapFolder> Folders() {
		std::lock_guard lock(g_Lock);
		return g_Folders;
	}

	bool Mounted() { return g_Phase.load(std::memory_order_relaxed) == Phase::Mounted; }

	std::string RedirectFrom() {
		std::lock_guard lock(g_Lock);
		return g_RedirectFrom;
	}

	std::string RedirectTo() {
		std::lock_guard lock(g_Lock);
		return g_RedirectTo;
	}

	void SetRedirect(const std::string& from, const std::string& to) {
		{
			std::lock_guard lock(g_Lock);
			g_RedirectFrom = Lower(from);
			g_RedirectTo = Lower(to);
		}
		Event(to.empty() ? std::string("map redirect off.") : std::format("map redirect: '{}' starts '{}'.", from, to));
	}

	std::vector<std::string> RecentEvents() {
		std::lock_guard lock(g_Lock);
		return { g_Events.begin(), g_Events.end() };
	}

	bool SetActiveMap(const std::string& rawId) {
		const std::string id = Lower(rawId);
		std::string refusal;
		MapFolder picked;
		bool redirectOff = false;
		{
			std::lock_guard lock(g_Lock);
			if (id == g_Active) return true;
			if (!id.empty()) {
				const auto it = std::ranges::find_if(g_Folders, [&](const MapFolder& f) { return f.listed && f.name == id; });
				if (it == g_Folders.end()) refusal = "it is not a listed map in cw-mod/maps";
				else if (!it->missing.empty()) refusal = "its zone set is incomplete (the overlay's Maps tab lists what is missing)";
				else picked = *it;
			}
			// A refused pick leaves no custom map active: the lobby then starts the stock one, never the last pick.
			g_Active = refusal.empty() ? id : std::string();
			redirectOff = g_RedirectByPick;
			g_RedirectByPick = picked.listed && !picked.overlay && !picked.standalone;
		}

		int overlay = -1;
		if (picked.listed && picked.overlay) {
			for (std::size_t i = 0; i < g_Overlays.size(); ++i) {
				if (g_Overlays[i].id == picked.name) overlay = static_cast<int>(i);
			}
		}
		g_ActiveOverlay = overlay;
		if (redirectOff) SetRedirect("", "");
		if (picked.listed && !picked.overlay && !picked.standalone) SetRedirect(picked.base, picked.name);

		// A map of its own becomes the lobby's map (every member then loads it by name); leaving one gives the lobby its
		// asset library back, the map the playlist names (the playlist may be the same, and is then not applied again).
		const int own = picked.listed && picked.standalone ? FindOwnMap(picked.name) : -1;
		const int wasOwn = g_PickedOwnMap.exchange(own);
		if (own >= 0) {
			PushLobbyMap(g_OwnMaps[own].id);
		}
		else if (wasOwn >= 0) {
			PushLobbyMap(g_OwnMaps[wasOwn].library);
		}

		if (!refusal.empty()) {
			Event(std::format("CUSTOM MAPS pick '{}' refused, {}: the stock map starts.", id, refusal));
		}
		else if (id.empty()) {
			Event("CUSTOM MAPS: none picked, every map starts stock.");
		}
		else if (own >= 0) {
			Event(std::format("CUSTOM MAPS: '{}' ('{}') is active: the lobby starts it under its own name, with {} under it.",
				id, picked.title, LibraryText(g_OwnMaps[own])));
		}
		else {
			Event(std::format("CUSTOM MAPS: '{}' ('{}') is active: {} starts with it{}.", id, picked.title, picked.base,
				picked.overlay ? std::format(" (ww_1080_{0} / ww_4k_{0} in place of {1}'s)", id, picked.base)
					: " (the redirect)"));
		}
		return refusal.empty();
	}

	std::string ActiveMap() {
		const int level = g_LevelOwnMap.load(std::memory_order_relaxed);
		if (level >= 0) return g_OwnMaps[static_cast<std::size_t>(level)].id;
		// A map of its own serves its scripts only while it loads, never on the pick alone (a client never picks).
		if (g_PickedOwnMap.load(std::memory_order_relaxed) >= 0) return {};
		std::lock_guard lock(g_Lock);
		return g_Active;
	}

	std::string PickedMap() {
		std::lock_guard lock(g_Lock);
		return g_Active;
	}

	std::vector<MapFolder> ListedMaps() {
		std::lock_guard lock(g_Lock);
		std::vector<MapFolder> out;
		for (const MapFolder& folder : g_Folders) {
			if (folder.listed) out.push_back(folder);
		}
		return out;
	}
}
