// mapkit asset-usage census. See mapkit_usage.hpp for what and why; the engine layouts are in dump_anchors.hpp
// (g_xassetEntries, g_zoneInfoRows, g_bgCacheTables).
#include "common.hpp"
#include "game/mapkit_usage.hpp"
#include "game/game_internal.hpp"
#include "game/dump_anchors.hpp"
#include "game/settings.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Client::Game::MapKitUsage {
	namespace {
		namespace fs = std::filesystem;
		using Clock = std::chrono::steady_clock;

		constexpr auto kFirstWriteDelay = std::chrono::seconds(45);
		constexpr auto kWriteInterval = std::chrono::seconds(120);
		constexpr std::uint64_t kNameMask = 0x7FFFFFFFFFFFFFFFull;
		constexpr std::uint8_t kTypeBgCache = 0x6D; // {u64 name; entries @8; i32 count @16}, entries 24 B
		constexpr std::size_t kBgCacheEntrySize = 24;
		constexpr std::size_t kEntrySize = 16;
		constexpr int kMaxChain = 256;              // an override chain is one entry per zone that has the asset

		std::atomic<bool> g_Enabled{ false };
		std::uintptr_t g_ModuleBase = 0;
		std::size_t g_ImageSize = 0;

		struct Key {
			std::uint64_t name;
			std::uint32_t kind;
			std::uint64_t caller;
			bool operator==(const Key&) const = default;
		};
		struct KeyHash {
			std::size_t operator()(const Key& k) const noexcept {
				return k.name ^ (static_cast<std::uint64_t>(k.kind) << 48) ^ (k.caller * 0x9E3779B97F4A7C15ull);
			}
		};
		using Counts = std::unordered_map<Key, std::uint32_t, KeyHash>;

		std::mutex g_Lock; // everything below
		Counts g_Finds;
		Counts g_Caches;
		std::uint64_t g_Recorded = 0;        // distinct (kind, name, caller) recorded so far
		std::uint64_t g_RecordedAtWrite = 0; // ... when the last write started
		std::string g_File;                  // this level's file, "" before a world starts
		std::string g_Map;
		Clock::time_point g_NextWrite{};
		bool g_WriteDue = false;
		std::string g_LastStatus;
		std::atomic<bool> g_Writing{ false };

		std::uintptr_t Abs(std::uintptr_t dump) { return g_ModuleBase + (dump - kDumpImagebase); }

		// The return address as an IDB address, so a caller can be looked up in IDA as it is.
		std::uint64_t Caller(const void* returnAddress) {
			const auto a = reinterpret_cast<std::uintptr_t>(returnAddress);
			return (a >= g_ModuleBase && a < g_ModuleBase + g_ImageSize) ? a - g_ModuleBase + kDumpImagebase : 0;
		}

		void Record(Counts& counts, std::uint32_t kind, std::uint64_t name, const void* returnAddress) {
			const Key key{ name & kNameMask, kind, Caller(returnAddress) };
			std::lock_guard lock(g_Lock);
			const auto [it, added] = counts.try_emplace(key, 0);
			++it->second;
			if (added) ++g_Recorded;
		}

		std::vector<UsageLookup> Flatten(const Counts& counts) {
			std::vector<UsageLookup> out;
			out.reserve(counts.size());
			for (const auto& [key, count] : counts) {
				out.push_back({ key.name, key.kind, count, key.caller });
			}
			return out;
		}

		// Where a type's root keeps its name hash: DB_GetXAssetName's getter for it, `mov rax, [rcx+N]` (decoded from the
		// exe 2026-09-30; zonekit xasset_list.cpp XAssetNameOffset has the same table). +0 for all other types.
		std::size_t NameOffset(std::uint8_t type) {
			switch (type) {
			case 0x05: return 112;  // xanim
			case 0x11: return 8;    // sound
			case 0x1D: return 8;    // localizeentry
			case 0x27: return 48;   // weapontunables
			case 0x35: return 16;   // klf (+0 is a string)
			case 0x66: return 8;    // sanim
			case 0x6E: return 432;  // flametable
			case 0x71: return 8;    // maptableentry
			case 0x89: return 32;   // gametypetableentry
			case 0x8C: return 8;    // unlockableitem
			case 0x8F: return 16;   // playlists
			case 0x90: return 8;    // playlistglobalsettings
			case 0x91: return 8;    // playlistschedule
			case 0x97: return 2976; // ragdoll
			case 0x98: return 8;    // storagefile
			case 0x9C: return 8;    // storecategory
			case 0xB5: return 8;    // dlogevent
			case 0xBD: return 8;    // arenaseasons
			case 0xC4: return 16;   // crafticon
			case 0xC6: return 16;   // craftweaponsticker
			case 0xC8: return 8;    // craftbackground
			case 0xCA: return 16;   // craftmaterial
			case 0xCC: return 8;    // craftcategory
			case 0xD1: return 224;  // dynmodel
			default: return 0;
			}
		}

		std::uint64_t ReadName(std::uint64_t header, std::uint8_t type) {
			const std::size_t at = NameOffset(type);
			std::uint64_t name = 0;
			if (header) SafeRead(reinterpret_cast<const void*>(header + at), name);
			return name & kNameMask;
		}

		std::string ReadString(std::uint64_t address, std::size_t max) {
			std::string out;
			for (std::size_t i = 0; address && i < max; ++i) {
				char c = 0;
				if (!SafeRead(reinterpret_cast<const void*>(address + i), c) || !c) break;
				out += c;
			}
			return out;
		}

		template <typename T>
		void Put(std::ofstream& f, const std::vector<T>& items) {
			if (!items.empty()) f.write(reinterpret_cast<const char*>(items.data()), static_cast<std::streamsize>(sizeof(T) * items.size()));
		}

		// Runs on its own thread: reads only, every read guarded.
		std::string WriteFile(const fs::path& path, const std::string& map) {
			std::vector<UsageLookup> finds;
			std::vector<UsageLookup> caches;
			{
				std::lock_guard lock(g_Lock);
				finds = Flatten(g_Finds);
				caches = Flatten(g_Caches);
			}

			std::vector<UsageZone> zones;
			std::vector<std::uint8_t> rows(kZoneInfoRowSize * kZoneInfoRowCount);
			if (!SafeCopy(rows.data(), reinterpret_cast<const void*>(Abs(kDump_g_zoneInfoRows)), rows.size())) {
				return "not written: the zone rows are unreadable";
			}
			for (int r = 0; r < kZoneInfoRowCount; ++r) {
				const std::uint8_t* row = rows.data() + kZoneInfoRowSize * r;
				if (!row[0]) continue;
				UsageZone zone{};
				zone.index = static_cast<std::uint32_t>(r + 1);
				std::memcpy(zone.name, row, sizeof(zone.name) - 1);
				std::memcpy(&zone.flags, row + 64, 4);
				std::memcpy(&zone.status, row + 68, 4);
				zones.push_back(zone);
			}

			// Every live entry, and every override chained behind it (those have no used bit of their own).
			std::vector<std::uint32_t> used(kXAssetEntryUsedWords);
			std::vector<std::uint8_t> table(kXAssetEntryArrayBytes);
			if (!SafeCopy(used.data(), reinterpret_cast<const void*>(Abs(kDump_g_xassetEntryUsedBits)), 4 * used.size())
				|| !SafeCopy(table.data(), reinterpret_cast<const void*>(Abs(kDump_g_xassetEntries)), table.size())) {
				return "not written: the asset entries are unreadable";
			}
			const std::size_t entryCount = table.size() / kEntrySize;
			std::vector<UsageEntry> entries;
			entries.reserve(800000);
			std::vector<std::pair<std::uint64_t, std::uint8_t>> bgcaches; // header, zone
			for (std::size_t w = 0; w < used.size(); ++w) {
				for (std::uint32_t bits = used[w]; bits;) {
					const int k = std::countl_zero(bits);
					bits &= ~(0x80000000u >> k);
					std::size_t index = 32 * w + static_cast<std::size_t>(k);
					bool head = true;
					for (int step = 0; step < kMaxChain && index < entryCount; ++step) {
						const std::uint8_t* e = table.data() + kEntrySize * index;
						std::uint64_t header = 0, link = 0;
						std::memcpy(&header, e, 8);
						std::memcpy(&link, e + 8, 8);
						UsageEntry entry{};
						entry.type = e[14];
						entry.zone = e[15];
						entry.head = head ? 1 : 0;
						entry.name = ReadName(header, entry.type);
						entries.push_back(entry);
						if (entry.type == kTypeBgCache) bgcaches.emplace_back(header, entry.zone);
						index = static_cast<std::size_t>((link >> 24) & 0xFFFFFF);
						head = false;
						if (!index) break;
					}
				}
			}

			std::vector<UsageTable> tables;
			for (std::size_t t = 0; t < kBgCacheTableCount; ++t) {
				const std::uint64_t record = Abs(kDump_g_bgCacheTables) + 64 * t;
				std::uint64_t name = 0;
				std::uint8_t type = 0;
				SafeRead(reinterpret_cast<const void*>(record), name);
				SafeRead(reinterpret_cast<const void*>(record + 8), type);
				UsageTable entry{};
				entry.index = static_cast<std::uint32_t>(t);
				entry.type = type;
				const std::string text = ReadString(name, sizeof(entry.name) - 1);
				std::memcpy(entry.name, text.data(), text.size());
				tables.push_back(entry);
			}

			std::vector<UsageListEntry> lists;
			for (const auto& [header, zone] : bgcaches) {
				std::uint64_t list = 0;
				std::int32_t count = 0;
				if (!SafeRead(reinterpret_cast<const void*>(header + 8), list) || !SafeRead(reinterpret_cast<const void*>(header + 16), count)
					|| !list || count <= 0 || count > 1000000) {
					continue;
				}
				std::vector<std::uint8_t> raw(kBgCacheEntrySize * static_cast<std::size_t>(count));
				if (!SafeCopy(raw.data(), reinterpret_cast<const void*>(list), raw.size())) continue;
				for (std::int32_t i = 0; i < count; ++i) {
					const std::uint8_t* e = raw.data() + kBgCacheEntrySize * static_cast<std::size_t>(i);
					UsageListEntry entry{};
					entry.table = e[0];
					std::memcpy(&entry.name, e + 8, 8);
					entry.name &= kNameMask;
					entry.zone = zone;
					lists.push_back(entry);
				}
			}

			UsageFileHeader header{};
			std::memcpy(header.magic, "MKUS", 4);
			header.version = kUsageVersion;
			header.zoneCount = static_cast<std::uint32_t>(zones.size());
			header.entryCount = static_cast<std::uint32_t>(entries.size());
			header.tableCount = static_cast<std::uint32_t>(tables.size());
			header.listCount = static_cast<std::uint32_t>(lists.size());
			header.findCount = static_cast<std::uint32_t>(finds.size());
			header.cacheCount = static_cast<std::uint32_t>(caches.size());
			header.moduleBase = g_ModuleBase;
			header.time = static_cast<std::uint64_t>(std::time(nullptr));
			std::memcpy(header.map, map.data(), std::min(map.size(), sizeof(header.map) - 1));

			std::error_code ec;
			fs::create_directories(path.parent_path(), ec);
			fs::path temp = path;
			temp += ".part";
			{
				std::ofstream f(temp, std::ios::binary | std::ios::trunc);
				if (!f) return std::format("not written: cannot open {}", temp.string());
				f.write(reinterpret_cast<const char*>(&header), sizeof(header));
				Put(f, zones);
				Put(f, entries);
				Put(f, tables);
				Put(f, lists);
				Put(f, finds);
				Put(f, caches);
				if (!f) return std::format("not written: writing {} failed", temp.string());
			}
			fs::rename(temp, path, ec);
			if (ec) return std::format("not written: {} -> {}: {}", temp.string(), path.string(), ec.message());
			return std::format("{} lookups and {} bgcache lookups, {} assets in {} zones, {} bgcache names -> {}",
				finds.size(), caches.size(), entries.size(), zones.size(), lists.size(), path.string());
		}

		fs::path FileFor(const std::string& map) {
			const std::time_t now = std::time(nullptr);
			std::tm local{};
			localtime_s(&local, &now);
			char stamp[32]{};
			std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
			std::error_code ec;
			return fs::current_path(ec) / "cw-mod" / "mapkit" / "usage" / std::format("{}_{}.mkuse", map.empty() ? "level" : map, stamp);
		}

		void StartWrite(std::string file, std::string map) {
			if (g_Writing.exchange(true)) return;
			std::thread([file = std::move(file), map = std::move(map)] {
				const std::string status = WriteFile(file, map);
				LOG("MapKit", INFO, "asset usage: {}", status);
				{
					std::lock_guard lock(g_Lock);
					g_LastStatus = status;
				}
				g_Writing.store(false);
			}).detach();
		}
	}

	void Init(std::uintptr_t moduleBase, std::size_t imageSize) {
		g_ModuleBase = moduleBase;
		g_ImageSize = imageSize;
		if (!Settings::Get().mapkitUsage) return;
		g_Enabled.store(true);
		LOG("MapKit", WARN, "asset usage census ON (\"mapkit_usage\" in cw-mod.json): every asset and bgcache lookup is "
			"recorded, and each level writes cw-mod/mapkit/usage/<map>_<time>.mkuse. It costs a little on every lookup; "
			"turn it off when the count is done.{}", Settings::Get().scripts ? ""
				: " \"scripts\" is false, so the DB_FindXAssetHeader half (the script loader's detour) records nothing.");
	}

	bool Enabled() { return g_Enabled.load(std::memory_order_relaxed); }

	void OnFind(std::uint32_t type, std::uint64_t name, const void* returnAddress) {
		if (!g_Enabled.load(std::memory_order_relaxed)) return;
		Record(g_Finds, type, name, returnAddress);
	}

	void OnCacheFind(std::uint8_t table, std::uint64_t name, const void* returnAddress) {
		if (!g_Enabled.load(std::memory_order_relaxed)) return;
		Record(g_Caches, table, name, returnAddress);
	}

	void OnWorldStart(const std::string& map) {
		if (!Enabled()) return;
		std::lock_guard lock(g_Lock);
		g_Map = map;
		g_File = FileFor(map).string();
		g_NextWrite = Clock::now() + kFirstWriteDelay;
		g_WriteDue = true;
		g_RecordedAtWrite = 0;
		LOG("MapKit", INFO, "asset usage: world start{}, the first write in {} s to {}", map.empty() ? "" : std::format(" of '{}'", map),
			std::chrono::duration_cast<std::chrono::seconds>(kFirstWriteDelay).count(), g_File);
	}

	void Tick() {
		if (!Enabled() || g_Writing.load(std::memory_order_relaxed)) return;
		std::string file, map;
		{
			std::lock_guard lock(g_Lock);
			if (!g_WriteDue || g_File.empty() || Clock::now() < g_NextWrite) return;
			g_NextWrite = Clock::now() + kWriteInterval;
			// Nothing new since the last write: that file already says it all.
			if (g_RecordedAtWrite != 0 && g_Recorded == g_RecordedAtWrite) return;
			g_RecordedAtWrite = g_Recorded;
			file = g_File;
			map = g_Map;
		}
		StartWrite(std::move(file), std::move(map));
	}

	std::string WriteNow() {
		if (!Enabled()) return "The census is off: set \"mapkit_usage\": true in cw-mod.json and restart the game.";
		std::lock_guard lock(g_Lock);
		if (g_File.empty()) g_File = FileFor("manual").string();
		g_WriteDue = true;
		g_NextWrite = Clock::now();
		g_RecordedAtWrite = 0;
		return std::format("Writing {} ...", g_File);
	}

	std::string LastStatus() {
		std::lock_guard lock(g_Lock);
		return g_LastStatus;
	}
}
