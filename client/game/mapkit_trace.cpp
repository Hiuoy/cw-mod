// mapkit zone trace. See mapkit_trace.hpp for the why; dump_anchors.hpp ("mapkit zone trace") for the addresses.
#include "common.hpp"
#include "game/mapkit_trace.hpp"
#include "game/dump_anchors.hpp"
#include "game/game_internal.hpp"
#include "game/settings.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace Client::Game::MapKitTrace {
	namespace {
		namespace fs = std::filesystem;

		constexpr std::size_t kAssetEntrySize = 16;     // XAsset {type, header}
		constexpr std::size_t kListCountOffset = 24;    // XAssetList: i32 asset count
		constexpr std::size_t kListAssetsOffset = 32;   // XAssetList: XAsset* assets

		std::vector<std::string> g_Zones; // written once by Init
		bool g_AllZones = false;
		bool g_Enabled = false;

		struct Api {
			const std::int32_t* posIndex{};
			const std::uint64_t* posArray{};
			const std::uint64_t* pos{};
			const std::int32_t* stackIndex{};
		} g_Api;

		// The thread loading the traced zone; 0 = no trace running. Everything below it is touched only by
		// that thread while it is set.
		std::atomic<DWORD> g_Thread{ 0 };
		std::uint64_t g_Stream = 0;
		std::string g_Zone;
		const std::uint8_t* g_List = nullptr;
		const std::uint8_t* g_Blocks = nullptr;
		std::array<std::uint64_t, kXBlockCount> g_Base{}; // read at the first asset: allocated after the zone begins
		int g_Flags = 0;
		std::vector<TraceRecord> g_Records;
		std::uint32_t g_AssetCount = 0;
		std::uint32_t g_LastIndex = 0;
		TraceRecord g_End{};
		bool g_HaveEnd = false;

		fs::path TraceDir() {
			std::error_code ec;
			return fs::current_path(ec) / "cw-mod" / "mapkit" / "trace";
		}

		std::string Lower(std::string s) {
			std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		bool Wanted(const std::string& zone) {
			return g_AllZones || std::ranges::find(g_Zones, zone) != g_Zones.end();
		}

		// The engine's stream state right now, as offsets from each block's base.
		void Capture(TraceRecord& r) {
			const int current = *g_Api.posIndex;
			r.block = static_cast<std::uint32_t>(current);
			r.depth = static_cast<std::uint32_t>(*g_Api.stackIndex);
			r.stream = g_Stream;
			for (int b = 0; b < kXBlockCount; ++b) {
				const std::uint64_t at = b == current ? *g_Api.pos : g_Api.posArray[b];
				r.pos[b] = at - g_Base[b];
			}
		}

		void Reset() {
			g_Thread = 0;
			g_Stream = 0;
			g_Zone.clear();
			g_List = nullptr;
			g_Blocks = nullptr;
			g_Base = {};
			g_Records.clear();
			g_Records.shrink_to_fit();
			g_AssetCount = 0;
			g_LastIndex = 0;
			g_HaveEnd = false;
		}

		void Write() {
			const bool complete = g_HaveEnd && g_AssetCount != 0 && g_LastIndex + 1 == g_AssetCount;
			if (complete) {
				g_End.index = g_AssetCount;
				g_End.type = kEndRecordType;
				g_End.header = 0;
				g_Records.push_back(g_End);
			}

			std::string file;
			for (char c : g_Zone) file += std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ? c : '_';
			std::error_code ec;
			fs::create_directories(TraceDir(), ec);
			const fs::path path = TraceDir() / (file + ".mktrace");

			TraceFileHeader header{};
			std::memcpy(header.magic, "MKTR", 4);
			header.version = kTraceVersion;
			header.recordSize = sizeof(TraceRecord);
			header.assetCount = g_AssetCount;
			header.recordCount = static_cast<std::uint32_t>(g_Records.size());
			header.flags = static_cast<std::uint32_t>(g_Flags);
			std::memcpy(header.zone, g_Zone.data(), std::min(g_Zone.size(), sizeof(header.zone) - 1));
			std::ranges::copy(g_Base, header.blockBase);

			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out.write(reinterpret_cast<const char*>(&header), sizeof(header));
			out.write(reinterpret_cast<const char*>(g_Records.data()),
				static_cast<std::streamsize>(g_Records.size() * sizeof(TraceRecord)));
			if (!out) {
				LOG("MapKit", ERROR, "Trace of '{}': could not write {}.", g_Zone, path.string());
				return;
			}
			const std::size_t traced = complete ? g_Records.size() - 1 : g_Records.size();
			LOG("MapKit", INFO, "Trace of '{}': {} of {} assets, {} stream bytes, wrote {} ({:.1f} MB).{}", g_Zone, traced,
				g_AssetCount, g_Stream, path.string(), (sizeof(header) + g_Records.size() * sizeof(TraceRecord)) / 1048576.0,
				complete ? "" : " INCOMPLETE: the load stopped before its last asset (no end record).");
		}
	}

	bool Init(std::uintptr_t moduleBase, std::size_t imageSize) {
		for (const std::string& zone : Settings::Get().mapkitTrace) {
			if (zone == "*") g_AllZones = true;
			else g_Zones.push_back(zone);
		}
		if (!g_AllZones && g_Zones.empty()) {
			return false;
		}

		auto addr = [&](std::uintptr_t dumpAbs) -> const void* {
			const std::uintptr_t a = moduleBase + (dumpAbs - kDumpImagebase);
			return (a >= moduleBase && a < moduleBase + imageSize) ? reinterpret_cast<const void*>(a) : nullptr;
		};
		g_Api.posIndex = static_cast<const std::int32_t*>(addr(kDump_g_streamPosIndex));
		g_Api.posArray = static_cast<const std::uint64_t*>(addr(kDump_g_streamPosArray));
		g_Api.pos = static_cast<const std::uint64_t*>(addr(kDump_g_streamPos));
		g_Api.stackIndex = static_cast<const std::int32_t*>(addr(kDump_g_streamPosStackIndex));
		if (!g_Api.posIndex || !g_Api.posArray || !g_Api.pos || !g_Api.stackIndex) {
			LOG("MapKit", ERROR, "Zone trace OFF: the stream position globals did not resolve.");
			return false;
		}

		std::string list;
		for (const std::string& zone : g_Zones) list += " " + zone;
		LOG("MapKit", WARN, "Zone trace ON for{}{}: each load writes cw-mod/mapkit/trace/<zone>.mktrace. Turn it off "
			"with \"mapkit_trace\": [] in cw-mod.json.", g_AllZones ? " every zone" : "", list);
		g_Enabled = true;
		return true;
	}

	bool Enabled() { return g_Enabled; }

	void OnZoneBegin(const char* zoneName, void* assetList, void* blocks, int flags) {
		if (!zoneName || !assetList || !blocks) return;
		if (g_Thread.load() != 0) {
			// The previous traced load never returned to us (an ERR_DROP longjmp): drop what it recorded.
			LOG("MapKit", WARN, "Trace of '{}' dropped: its load did not finish.", g_Zone);
			Reset();
		}
		const std::string zone = Lower(std::string(zoneName, strnlen(zoneName, 64)));
		if (!Wanted(zone)) return;

		g_Zone = zone;
		g_List = static_cast<const std::uint8_t*>(assetList);
		g_Blocks = static_cast<const std::uint8_t*>(blocks);
		g_Flags = flags;
		g_Stream = 0;
		g_Thread = GetCurrentThreadId();
		LOG("MapKit", INFO, "Trace of '{}' started (flags 0x{:X}).", zone, static_cast<std::uint32_t>(flags));
	}

	void OnZoneEnd() {
		if (g_Thread.load() != GetCurrentThreadId()) return;
		try {
			Write();
		}
		catch (const std::exception& e) {
			LOG("MapKit", ERROR, "Trace of '{}' not written: {}.", g_Zone, e.what());
		}
		Reset();
	}

	void OnAssetBegin(const std::uint8_t* asset) {
		if (g_Thread.load(std::memory_order_relaxed) != GetCurrentThreadId()) return;
		try {
			if (g_AssetCount == 0) {
				std::int32_t count = 0;
				std::memcpy(&count, g_List + kListCountOffset, sizeof(count));
				g_AssetCount = count > 0 ? static_cast<std::uint32_t>(count) : 0;
				g_Records.reserve(g_AssetCount + 1);
				// DB_InitStreams has run by now (it takes the same array), so the bases are set.
				for (int b = 0; b < kXBlockCount; ++b) {
					std::memcpy(&g_Base[b], g_Blocks + b * kXBlockStride, sizeof(std::uint64_t));
				}
			}
			const std::uint8_t* table = nullptr;
			std::memcpy(&table, g_List + kListAssetsOffset, sizeof(table));
			std::uint32_t index = static_cast<std::uint32_t>(g_Records.size());
			if (table && asset >= table && asset < table + std::size_t(g_AssetCount) * kAssetEntrySize) {
				index = static_cast<std::uint32_t>((asset - table) / kAssetEntrySize);
			}

			TraceRecord r{};
			r.index = index;
			std::uint64_t type = 0;
			std::memcpy(&type, asset, sizeof(type));
			r.type = static_cast<std::uint32_t>(type);
			std::memcpy(&r.header, asset + 8, sizeof(r.header));
			Capture(r);
			g_Records.push_back(r);
			g_LastIndex = index;
		}
		catch (const std::exception&) {
			// Out of memory for a 25 MB vector: give the trace up, never the zone load.
			g_Records.clear();
			g_Thread = 0;
		}
	}

	void OnAssetEnd() {
		if (g_Thread.load(std::memory_order_relaxed) != GetCurrentThreadId()) return;
		Capture(g_End);
		g_HaveEnd = true;
	}

	void OnStreamBytes(std::int64_t count) {
		const DWORD thread = g_Thread.load(std::memory_order_relaxed);
		if (thread != 0 && thread == GetCurrentThreadId() && count > 0) {
			g_Stream += static_cast<std::uint64_t>(count);
		}
	}

	void OnCrash() {
		if (g_Thread.load() != GetCurrentThreadId() || g_Records.empty()) return;
		const TraceRecord& last = g_Records.back();
		TraceRecord now{};
		Capture(now);
		// Once its loader has allocated the root, the asset's entry holds the root's address: its first words.
		std::string root = "not readable";
		const std::uint8_t* table = nullptr;
		std::memcpy(&table, g_List + kListAssetsOffset, sizeof(table));
		std::uint64_t header = 0;
		if (table && last.index < g_AssetCount && SafeRead(table + std::size_t(last.index) * kAssetEntrySize + 8, header)) {
			root = std::format("at 0x{:X}", header);
			for (std::size_t i = 0; i < 4; ++i) {
				std::uint64_t word = 0;
				if (!SafeRead(reinterpret_cast<const std::uint8_t*>(header) + 8 * i, word)) break;
				root += std::format(" {:016X}", word);
			}
		}
		LOG("MapKit", ERROR, "Trace of '{}': the load faulted in asset {} of {} (type 0x{:X}), which began {} stream bytes in "
			"({} now; block {} now, block 4 at +0x{:X}). Its root {}. Block bases: 0 0x{:X}, 4 0x{:X}, 6 0x{:X}, 12 0x{:X}. "
			"Writing what the trace recorded.", g_Zone, last.index, g_AssetCount, last.type, last.stream, g_Stream, now.block,
			now.pos[4], root, g_Base[0], g_Base[4], g_Base[6], g_Base[12]);
		try {
			Write();
		}
		catch (const std::exception&) {
		}
		Reset();
	}
}
