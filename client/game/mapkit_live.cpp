// mapkit live-state dump. See mapkit_live.hpp for what and why; layouts are zonekit's (model_writer.hpp,
// model_assets.cpp, gfx_world.cpp, world_assets.hpp).
#include "common.hpp"
#include "game/mapkit_live.hpp"
#include "game/game_internal.hpp"
#include "game/dump_anchors.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Client::Game::MapKit::Live {
	namespace {
		namespace fs = std::filesystem;

		// Struct sizes (zonekit).
		constexpr std::size_t kXModelSize = 232;
		constexpr std::size_t kXModelMeshSize = 64;
		constexpr std::size_t kMeshInfoSize = 464;
		constexpr std::size_t kSurfaceSize = 48;
		constexpr std::size_t kBlock96Size = 96;
		constexpr std::size_t kMaterialTableSize = 32;
		constexpr std::size_t kMaterialSize = 344;
		constexpr std::size_t kTechsetSize = 168;
		constexpr std::size_t kImageTableEntry = 24;
		constexpr std::size_t kImageSize = 208;
		constexpr std::size_t kGfxWorldSize = 7976;
		constexpr std::size_t kClipMapSize = 472;
		constexpr std::size_t kBufferHead = 256;
		constexpr std::size_t kMaxLods = 8;
		constexpr std::size_t kMaxSurfaces = 64;
		constexpr std::size_t kMaxMaterials = 8;
		constexpr std::size_t kMaxDiffLines = 24;
		constexpr auto kSecondDumpDelay = std::chrono::seconds(20);

		std::uintptr_t g_ModuleBase = 0;
		std::atomic<std::int64_t> g_SecondDumpAt{ 0 }; // steady_clock ticks; 0 = none pending
		std::mutex g_Lock;                              // guards the strings below and serialises dumps
		// Comma-separated lists; ours[i] is compared with retail[i], or with the last retail one. Defaults: the draw
		// test row of the Godot test map (cwlink build --draw-test) and the brush model, against the row's retail door.
		std::string g_Ours = "mapkit_zm_test_clone8,mapkit_zm_test_clone,mapkit_zm_test_clone_full,mapkit_zm_test_cube,mapkit_zm_test";
		std::string g_Retail = "0x3ED36494E93F3CBD";    // p9_zm_ndu_door_metal_gray_rusted
		std::string g_LastDump;

		struct Part {
			std::string label;
			std::uint64_t addr = 0;
			std::vector<std::uint8_t> bytes; // empty = null or unreadable
		};

		std::uintptr_t Abs(std::uintptr_t dump) { return g_ModuleBase + (dump - kDumpImagebase); }

		template <typename T>
		T Read(std::uint64_t addr) {
			T value{};
			if (addr) SafeRead(reinterpret_cast<const void*>(addr), value);
			return value;
		}

		std::uint64_t NameHash(std::string_view name) {
			std::uint64_t h = 0xCBF29CE484222325ull;
			for (const char c : name) {
				h = (h ^ static_cast<std::uint8_t>(std::tolower(static_cast<unsigned char>(c)))) * 0x100000001B3ull;
			}
			return h & 0x7FFFFFFFFFFFFFFFull;
		}

		// "0x<hex>" or "#<hex>" is a name hash, anything else a name.
		std::uint64_t ParseName(std::string_view text) {
			if (text.starts_with("0x") || text.starts_with("0X") || text.starts_with("#")) {
				text.remove_prefix(text[0] == '#' ? 1 : 2);
				std::uint64_t value = 0;
				std::from_chars(text.data(), text.data() + text.size(), value, 16);
				return value & 0x7FFFFFFFFFFFFFFFull;
			}
			return NameHash(text);
		}

		// Every live item of a pool whose header's first qword (the name) is `name`. A free item's +0 points back
		// into the pool, which is how it is told apart.
		std::vector<std::uint64_t> FindInPool(std::size_t type, std::uint64_t name) {
			std::vector<std::uint64_t> found;
			const std::uint64_t pool = Abs(kDump_g_xassetPools) + 32 * type;
			const auto items = Read<std::uint64_t>(pool);
			const auto size = Read<std::uint32_t>(pool + 8);
			const auto count = Read<std::int32_t>(pool + 12);
			if (!items || size < 8 || count <= 0) return found;
			const std::uint64_t end = items + static_cast<std::uint64_t>(size) * static_cast<std::uint64_t>(count);
			for (std::uint64_t item = items; item < end; item += size) {
				std::uint64_t head = 0;
				if (!SafeRead(reinterpret_cast<const void*>(item), head) || (head >= items && head < end)) continue;
				if (name == ~0ull || (head & 0x7FFFFFFFFFFFFFFFull) == name) found.push_back(item);
			}
			return found;
		}

		// Where `name` sits in the engine's bgcache "model" table (dump_anchors.hpp kDump_g_bgCacheTables), the only
		// place a map entity's model is looked up: an unlisted model's entity spawns with no model.
		std::string BgCacheModel(std::uint64_t name) {
			const std::uint64_t table = Abs(kDump_g_bgCacheTables) + 64 * kBgCacheTableModel;
			const auto first = Read<std::uint32_t>(table + 48);
			const auto count = Read<std::int32_t>(table + 52);
			if (count <= 1 || count > 0x10000) return std::format("bgcache model table unreadable (count {})", count);
			std::vector<std::uint64_t> names(2 * static_cast<std::size_t>(count));
			if (!SafeCopy(names.data(), reinterpret_cast<const void*>(Abs(kDump_g_bgCacheNames) + 16ull * first), 8 * names.size())) {
				return "bgcache model table unreadable";
			}
			for (std::int32_t i = 1; i < count; ++i) {
				if ((names[2 * i] & 0x7FFFFFFFFFFFFFFFull) == name) {
					return std::format("bgcache model #{} of {} (xmodel {:#x})", i, count - 1, names[2 * i + 1]);
				}
			}
			return std::format("NOT in the bgcache model table ({} names)", count - 1);
		}

		// Every loaded bgcache asset: name, entries, zone and that zone's flags. BG_Cache_RegisterAll_cand reads one
		// only from a zone whose flags meet its masks (0x3F, 0x7800, 0x3C0) and lack 8.
		std::vector<std::string> BgCaches() {
			std::vector<std::string> out;
			std::vector<std::uint32_t> used(kXAssetEntryUsedWords);
			if (!SafeCopy(used.data(), reinterpret_cast<const void*>(Abs(kDump_g_xassetEntryUsedBits)), 4 * used.size())) {
				out.push_back("bgcache: the xasset entry bits are unreadable");
				return out;
			}
			for (std::size_t w = 0; w < used.size(); ++w) {
				for (std::uint32_t bits = used[w]; bits;) {
					const int k = std::countl_zero(bits);
					bits &= ~(0x80000000u >> k);
					const std::uint64_t entry = Abs(kDump_g_xassetEntries) + 512 * w + 16 * static_cast<std::uint64_t>(k);
					if (Read<std::uint8_t>(entry + 14) != kXAssetTypeBgCache) continue;
					const auto header = Read<std::uint64_t>(entry);
					const auto zone = Read<std::uint8_t>(entry + 15);
					out.push_back(std::format("bgcache {:016X}: {} entries, zone {} flags {:#x}",
						Read<std::uint64_t>(header) & 0x7FFFFFFFFFFFFFFFull, Read<std::int32_t>(header + 16), zone,
						Read<std::uint32_t>(Abs(kDump_g_zoneInfos) + kZoneInfoSize * zone)));
				}
			}
			return out;
		}

		void Add(std::vector<Part>& parts, std::string label, std::uint64_t addr, std::size_t size) {
			Part part{ std::move(label), addr, {} };
			if (addr && size) {
				part.bytes.resize(size);
				if (!SafeCopy(part.bytes.data(), reinterpret_cast<const void*>(addr), size)) part.bytes.clear();
			}
			parts.push_back(std::move(part));
		}

		// The renderer's view of one xmodelmesh: its index, its bit in each streaming bit array, its dword.
		std::string MeshState(std::uint64_t mesh) {
			const auto base = Read<std::uint64_t>(Abs(kDump_g_xmodelMeshIndexBase));
			const auto globals = Read<std::uint64_t>(Abs(kDump_g_rendererGlobals));
			if (!mesh || !base || !globals || mesh < base) return "no index";
			const std::uint64_t index = (mesh - base) >> 6;
			const std::uint64_t word = 4 * (index >> 5);
			const std::uint32_t bit = 1u << (index & 31);
			std::string bits;
			for (const std::size_t array : kRendererMeshBitArrays) {
				const auto words = Read<std::uint64_t>(globals + array);
				bits += (words && (Read<std::uint32_t>(words + word) & bit)) ? '1' : '0';
			}
			const bool inlineBit = Read<std::uint32_t>(globals + kRendererMeshBitsInline + word) & bit;
			return std::format("index {} bits {} (+2DDC858 +868 +870 +878 +890 +8E8 +8F0 +8F8) inline {} dword {:#x}", index,
				bits, inlineBit ? 1 : 0, Read<std::uint32_t>(globals + kRendererMeshDwords + 4 * index));
		}

		// xmodel layout (zonekit model_writer.hpp): +32 LODs (8 x ptr), +96 material tables (32 B per LOD: u16
		// count, +8 material handles), +104 a 96-B block, +112 u16 LOD count. LOD (xmodelmesh, 64 B): +8 surfaces
		// (48 B x u16 @60), +16 mesh info (464 B: +32 buffer, +40 its size), +24 a 96-B block. Material (344 B):
		// +40 techset, +48 image table (24 B x u8 @328, image at +0).
		void AddModel(std::vector<Part>& parts, std::vector<std::string>& notes, const std::string& side, std::uint64_t model) {
			Add(parts, side + ".xmodel", model, kXModelSize);
			Add(parts, side + ".xmodel_block96", Read<std::uint64_t>(model + 104), kBlock96Size);
			const std::size_t lods = std::clamp<std::size_t>(Read<std::uint16_t>(model + 112), 1, kMaxLods);
			const auto tables = Read<std::uint64_t>(model + 96);
			std::vector<std::uint64_t> materials;
			for (std::size_t l = 0; l < lods; ++l) {
				const std::string lod = std::format("{}.lod{}", side, l);
				const auto mesh = Read<std::uint64_t>(model + 32 + 8 * l);
				Add(parts, lod, mesh, kXModelMeshSize);
				const auto info = Read<std::uint64_t>(mesh + 16);
				Add(parts, lod + ".info", info, kMeshInfoSize);
				const std::size_t surfaces = std::min<std::size_t>(Read<std::uint16_t>(mesh + 60), kMaxSurfaces);
				Add(parts, lod + ".surfaces", Read<std::uint64_t>(mesh + 8), kSurfaceSize * surfaces);
				Add(parts, lod + ".block96", Read<std::uint64_t>(mesh + 24), kBlock96Size);
				const std::size_t size = Read<std::uint32_t>(info + 40);
				Add(parts, lod + ".buffer_head", Read<std::uint64_t>(info + 32), std::min(size, kBufferHead));
				notes.push_back(std::format("{} mesh {:#x}: {}", lod, mesh, MeshState(mesh)));

				const std::uint64_t table = tables ? tables + kMaterialTableSize * l : 0;
				Add(parts, lod + ".mattable", table, kMaterialTableSize);
				const std::size_t count = std::min<std::size_t>(Read<std::uint16_t>(table), kMaxSurfaces);
				const auto handles = Read<std::uint64_t>(table + 8);
				for (std::size_t m = 0; m < count; ++m) {
					const auto material = Read<std::uint64_t>(handles + 8 * m);
					if (material && std::ranges::find(materials, material) == materials.end() && materials.size() < kMaxMaterials) {
						materials.push_back(material);
					}
				}
			}
			for (std::size_t m = 0; m < materials.size(); ++m) {
				const std::string label = std::format("{}.material{}", side, m);
				const std::uint64_t material = materials[m];
				Add(parts, label, material, kMaterialSize);
				Add(parts, label + ".techset", Read<std::uint64_t>(material + 40), kTechsetSize);
				const std::size_t images = Read<std::uint8_t>(material + 328);
				const auto imageTable = Read<std::uint64_t>(material + 48);
				Add(parts, label + ".imagetable", imageTable, kImageTableEntry * images);
				Add(parts, label + ".image0", images ? Read<std::uint64_t>(imageTable) : 0, kImageSize);
			}
		}

		// A heap pointer in this process (0x1C2..., 0x1EF..., 0x24B...). Small packed words (u16 counts, GPU handles
		// with flag bits) fall below 1 TB or are unaligned, so they are listed.
		bool LooksLikePointer(std::uint64_t v) { return v >= 0x10000000000ull && v < 0x800000000000ull && (v & 3) == 0; }

		std::vector<std::string> SplitList(const std::string& list) {
			std::vector<std::string> out;
			std::size_t start = 0;
			while (start <= list.size()) {
				const std::size_t comma = std::min(list.find(',', start), list.size());
				std::string item = list.substr(start, comma - start);
				std::erase_if(item, [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
				if (!item.empty()) out.push_back(std::move(item));
				start = comma + 1;
			}
			return out;
		}

		// ours vs retail for one part: every 8-B word that differs, pointer pairs counted but not listed.
		void LogDiff(const std::string& name, const Part& ours, const Part& retail) {
			if (ours.bytes.empty() || retail.bytes.empty()) {
				LOG("MapKit", INFO, "live diff {}: ours {}, retail {}", name, ours.bytes.empty() ? "null" : "present",
					retail.bytes.empty() ? "null" : "present");
				return;
			}
			const std::size_t n = std::min(ours.bytes.size(), retail.bytes.size());
			std::size_t words = 0, pointers = 0, listed = 0;
			std::vector<std::string> lines;
			for (std::size_t off = 0; off + 8 <= n; off += 8) {
				std::uint64_t a = 0, b = 0;
				std::memcpy(&a, ours.bytes.data() + off, 8);
				std::memcpy(&b, retail.bytes.data() + off, 8);
				if (a == b) continue;
				++words;
				if (LooksLikePointer(a) && LooksLikePointer(b)) {
					++pointers;
					continue;
				}
				if (listed++ < kMaxDiffLines) lines.push_back(std::format("+{:03}: ours {:016x} retail {:016x}", off, a, b));
			}
			LOG("MapKit", INFO, "live diff {} ({} vs {} B): {} words differ, {} of them pointer pairs{}", name,
				ours.bytes.size(), retail.bytes.size(), words, pointers, listed > kMaxDiffLines ? " (first 24 listed)" : "");
			for (const std::string& line : lines) LOG("MapKit", INFO, "  {}", line);
		}

		std::string Stamp() {
			SYSTEMTIME t{};
			GetLocalTime(&t);
			return std::format("{:02}{:02}{:02}", t.wHour, t.wMinute, t.wSecond);
		}

		std::string SafeTag(std::string tag) {
			for (char& c : tag) {
				if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
			}
			return tag;
		}
	}

	namespace {
		// What the AI walks on (dump_anchors.hpp kDump_g_aiNav): whose navmesh, and whether its tagfiles loaded.
		std::string NavMeshStatus() {
			const auto nav = Read<std::uint64_t>(Abs(kDump_g_aiNav));
			if (!nav) return "navmesh: no nav world";
			const auto asset = Read<std::uint64_t>(nav + 72);
			if (!asset) return "navmesh: no navmesh asset";
			const auto sharedKey = Read<std::uint64_t>(asset + 8);
			const bool inlineKey = sharedKey && (Read<std::uint8_t>(sharedKey + 55) & 2);
			const auto cells = Read<std::int32_t>(asset + 32);
			std::string status = std::format("navmesh: {:016X} ({}), shared file {}, {} cell(s)", Read<std::uint64_t>(asset),
				inlineKey ? "inline: mapkit's" : "streamed: the base map's", Read<std::uint64_t>(nav + 80) ? "loaded" : "NOT loaded", cells);
			const auto cellArray = Read<std::uint64_t>(asset + 40);
			for (std::int32_t i = 0; i < std::min(cells, 4); ++i) {
				const auto cell = Read<std::uint64_t>(cellArray + 64ull * i + 56);
				const auto mesh = cell ? Read<std::uint64_t>(cell + 32) : 0;
				status += mesh ? std::format("; cell {}: {} faces, {} edges, {} vertices", i, Read<std::int32_t>(mesh + 32),
					Read<std::int32_t>(mesh + 48), Read<std::int32_t>(mesh + 64)) : std::format("; cell {} NOT loaded", i);
			}
			status += std::format("; {} instance(s) in the world", Read<std::int32_t>(nav + 64));
			return status;
		}
	}

	void Init(std::uintptr_t moduleBase) { g_ModuleBase = moduleBase; }

	void Dump(const std::string& tag) {
		if (!g_ModuleBase) return;
		std::lock_guard lock(g_Lock);
		const std::vector<std::string> oursList = SplitList(g_Ours);
		const std::vector<std::string> retailList = SplitList(g_Retail);

		// Each model once, its first pool item: ours<i>, retail<j>.
		std::vector<Part> parts;
		std::vector<std::string> notes;
		std::string found;
		const auto addList = [&](const std::vector<std::string>& list, const char* side) {
			for (std::size_t i = 0; i < list.size(); ++i) {
				const std::uint64_t name = ParseName(list[i]);
				const auto items = FindInPool(kXAssetTypeXModel, name);
				found += std::format("{}{}{} {} ({:016X}): {} in the pool, {}", found.empty() ? "" : "; ", side, i, list[i], name,
					items.size(), BgCacheModel(name));
				if (!items.empty()) AddModel(parts, notes, std::format("{}{}", side, i), items.front());
			}
		};
		addList(oursList, "ours");
		addList(retailList, "retail");
		for (std::string& bgCache : BgCaches()) notes.push_back(std::move(bgCache));
		notes.push_back(NavMeshStatus());

		// The world roots, for file-vs-live offline.
		const auto globals = Read<std::uint64_t>(Abs(kDump_g_rendererGlobals));
		Add(parts, "world.gfx_map", globals ? Read<std::uint64_t>(globals + kRendererWorldOffset) : 0, kGfxWorldSize);
		const auto clipMaps = FindInPool(kXAssetTypeClipMap, ~0ull);
		for (std::size_t i = 0; i < clipMaps.size(); ++i) Add(parts, std::format("world.clip_map{}", i), clipMaps[i], kClipMapSize);

		std::error_code ec;
		const fs::path dir = fs::current_path(ec) / "cw-mod" / "mapkit" / "live" / (Stamp() + "_" + SafeTag(tag));
		fs::create_directories(dir, ec);
		std::ofstream manifest(dir / "manifest.txt");
		manifest << found << "\n";
		for (const std::string& note : notes) manifest << "# " << note << "\n";
		for (const Part& part : parts) {
			manifest << std::format("{} {:#x} {}\n", part.label, part.addr, part.bytes.size());
			if (part.bytes.empty()) continue;
			std::ofstream bin(dir / (part.label + ".bin"), std::ios::binary);
			bin.write(reinterpret_cast<const char*>(part.bytes.data()), static_cast<std::streamsize>(part.bytes.size()));
		}
		g_LastDump = dir.string();

		LOG("MapKit", INFO, "live dump '{}': {}; {} parts -> {}", tag, found, parts.size(), dir.string());
		for (const std::string& note : notes) LOG("MapKit", INFO, "live {}", note);
		// ours<i> against retail<i> (or the last retail), part by part (same suffix).
		for (std::size_t i = 0; i < oursList.size() && !retailList.empty(); ++i) {
			const std::size_t j = std::min(i, retailList.size() - 1);
			const std::string ours = std::format("ours{}.", i), retail = std::format("retail{}.", j);
			LOG("MapKit", INFO, "live diff {} vs {}:", oursList[i], retailList[j]);
			for (const Part& part : parts) {
				if (!part.label.starts_with(ours)) continue;
				const std::string suffix = part.label.substr(ours.size());
				const auto other = std::ranges::find(parts, retail + suffix, &Part::label);
				if (other != parts.end()) LogDiff(suffix, part, *other);
			}
		}
	}

	void OnWorldStart(bool ownWorld) {
		if (!ownWorld) return;
		Dump("world_start");
		g_SecondDumpAt = (std::chrono::steady_clock::now() + kSecondDumpDelay).time_since_epoch().count();
	}

	void Tick() {
		const std::int64_t at = g_SecondDumpAt.load(std::memory_order_relaxed);
		if (at && std::chrono::steady_clock::now().time_since_epoch().count() >= at && g_SecondDumpAt.exchange(0) == at) {
			Dump("match_20s");
		}
	}

	void SetModels(std::string ours, std::string retail) {
		std::lock_guard lock(g_Lock);
		g_Ours = std::move(ours);
		g_Retail = std::move(retail);
	}

	std::pair<std::string, std::string> Models() {
		std::lock_guard lock(g_Lock);
		return { g_Ours, g_Retail };
	}

	std::string LastDump() {
		std::lock_guard lock(g_Lock);
		return g_LastDump;
	}
}
