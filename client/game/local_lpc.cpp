// LPC playlists outside LIVE. See local_lpc.hpp for the why; dump_anchors.hpp for the addresses.
#include "common.hpp"
#include "game/local_lpc.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/arxan_call.hpp"
#include "game/boot_profile.hpp"
#include "game/dump_anchors.hpp"
#include "game/settings.hpp"
#include "game/function_types.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <regex>
#include <string>

namespace Client::Game::LocalLpc {
	namespace {
		namespace F = Functions;
		namespace fs = std::filesystem;
		using Clock = std::chrono::steady_clock;

		enum class Phase : int { Off, Mount, WaitIdle, Loading, Done, Failed };

		constexpr int kSearchPathPriority = 300;   // what Lpc_VerifyNextFile passes
		constexpr int kSearchPathDevice = 2;
		constexpr std::uint32_t kLangZoneBit = 0x8000;
		constexpr auto kLoadTimeout = std::chrono::seconds(60);

		std::atomic<Phase> g_Phase{ Phase::Off };
		std::atomic_bool g_LoadInFlight{ false };
		std::array<std::atomic<std::uint32_t>, 256> g_AllocsByType{};
		std::array<std::atomic_bool, 256> g_PoolEmptyLogged{};
		std::atomic_bool g_EntryListLogged{ false };
		Clock::time_point g_PhaseStart{};
		int g_LastBaseStatus = -2;
		int g_LastLangStatus = -2;
		bool g_WaitLogged = false;
		bool g_OffThreadLogged = false;
		// Slot 0 read 2 with no playlist behind it: session.cpp's B3 bypass wrote it (LIVE + backend).
		// Its state is then left at 2, because Com_ApplyGameModeSwitch spins until both slots are 2.
		bool g_SlotFaked = false;
		// Zone names = the file names without .ff, exactly as shipped: the signature covers them.
		std::string g_BaseZone;
		std::string g_LangZone;

		struct Api {
			F::IntFnT* GetTuVersion{};
			F::CStrFnT* GetBuildId{};
			F::IntFnT* GetLanguage{};
			F::Loc_GetLanguagePrefixT* LanguagePrefix{};
			F::FS_AddSearchPathT* AddSearchPath{};
			F::DB_ZoneFileExistsT* ZoneFileExists{};
			F::DB_LoadXAssetsT* LoadXAssets{};
			F::VoidFnT* SyncXAssets{};
			F::R_RemoteScreenUpdateAllowT* ScreenUpdateAllow{};
			F::VoidFnT* ScreenUpdateBegin{};
			F::VoidFnT* ScreenUpdateEnd{};
			F::OnlineContent_SlotCallbackT* PlaylistsLoaded{};
			const std::uint8_t* slotTable{};   // row 0 = playlists
			int* slotState{};                  // int[2]: 0 idle, 1 loading, 2 loaded
			const std::uint8_t* zoneRows{};
			const std::uint8_t* dbReady{};
			const std::uint64_t* dbSyncPending{};
			const std::uint64_t* mainThreadId{};
			void* const* playlistAsset{};
			const std::uint8_t* playlistValid{};
			void* const* entryFreeHead{};
			const std::uint8_t* entries{};
			const std::uint8_t* pools{};
		} g_Api;

		// [<lang>_]core_playlists_tu<N>_100_<16 hex>.ff
		const std::regex& PlaylistsName() {
			static const std::regex re(R"(^([a-z]{2}_)?core_playlists_tu(\d+)_100_([0-9a-f]{16})\.ff$)",
				std::regex::icase);
			return re;
		}

		fs::path SourceDir() {
			std::error_code ec;
			return fs::current_path(ec) / "cw-mod" / "lpc";
		}

		void SetPhase(Phase p) {
			g_Phase = p;
			g_PhaseStart = Clock::now();
		}

		bool OnMainThread() {
			std::uint64_t main = 0;
			return SafeRead(g_Api.mainThreadId, main) && main == GetCurrentThreadId();
		}

		// DB_GetZoneInfoByFlags, read in place: first row with exactly these flags; 0 = no row.
		int ZoneStatus(std::uint32_t flags) {
			for (int i = 0; i < kZoneInfoRowCount; ++i) {
				const std::uint8_t* row = g_Api.zoneRows + i * kZoneInfoRowSize;
				std::uint32_t f = 0;
				if (!SafeRead(row + 64, f)) return -1;
				if (f != flags) continue;
				int status = -1;
				SafeRead(row + 68, status);
				return status;
			}
			return 0;
		}

		std::string AllocSummary() {
			std::string out;
			for (int t = 0; t < 256; ++t) {
				const std::uint32_t n = g_AllocsByType[t].load(std::memory_order_relaxed);
				if (!n) continue;
				const std::uint8_t* pool = g_Api.pools + 32 * t;
				int count = -1, used = -1;
				SafeRead(pool + 12, count);
				SafeRead(pool + 20, used);
				out += std::format(" 0x{:02X}x{} (pool {}/{})", t, n, used, count);
			}
			return out.empty() ? " none" : out;
		}

		// Picks the playlists pair for this exe's build id in cw-mod/lpc: the base zone and the current
		// language's twin (else en_, else any language). Nothing is copied or renamed.
		bool PickFiles(const std::string& buildId, const std::string& langPrefix, std::string& report) {
			std::error_code ec;
			fs::path base, wantLang, anyLang;
			for (const auto& e : fs::directory_iterator(SourceDir(), ec)) {
				if (!e.is_regular_file(ec)) continue;
				const std::string name = e.path().filename().string();
				std::smatch m;
				if (!std::regex_match(name, m, PlaylistsName())) continue;
				if (_stricmp(m[3].str().c_str(), buildId.c_str()) != 0) {
					report += std::format(" skipped {} (build id {}, this exe {});", name, m[3].str(), buildId);
					continue;
				}
				const std::string lang = m[1].str();
				if (lang.empty()) base = e.path();
				else if (_stricmp(lang.c_str(), langPrefix.c_str()) == 0) wantLang = e.path();
				else if (anyLang.empty() || _stricmp(lang.c_str(), "en_") == 0) anyLang = e.path();
			}
			if (wantLang.empty() && !anyLang.empty()) {
				report += std::format(" no {}core_playlists file, using {};", langPrefix, anyLang.filename().string());
				wantLang = anyLang;
			}
			if (base.empty() || wantLang.empty()) {
				report += std::format(" missing {}", base.empty() ? "core_playlists_tu*_100_<id>.ff"
					: "a <lang>_core_playlists_tu*_100_<id>.ff");
				return false;
			}
			g_BaseZone = base.stem().string();
			g_LangZone = wantLang.stem().string();
			return true;
		}

		void Fail(const std::string& why) {
			g_LoadInFlight = false;
			SetPhase(Phase::Failed);
			LOG("LPC", ERROR, "LPC playlists: {} The game keeps running without a playlist, as before.", why);
		}

		void DoMount() {
			const int tu = g_Api.GetTuVersion();
			const char* idRaw = g_Api.GetBuildId();
			const std::string buildId = idRaw ? idRaw : "";
			const char* prefixRaw = g_Api.LanguagePrefix(static_cast<unsigned int>(g_Api.GetLanguage()));
			const std::string langPrefix = prefixRaw ? prefixRaw : "";
			if (buildId.size() != 16) {
				Fail(std::format("the engine reported build id '{}'.", buildId));
				return;
			}

			std::string report;
			std::string dir;
			bool picked = false;
			try {
				picked = PickFiles(buildId, langPrefix, report);
				dir = SourceDir().string();
				// The first 2026-09-23 build staged renamed (tu34) copies here; renamed, they fail their signature.
				std::error_code ec;
				fs::remove_all(SourceDir() / "mount", ec);
			}
			catch (const std::exception& e) {
				report += std::format(" exception: {}", e.what());
				picked = false;
			}
			if (!picked) {
				Fail(std::format("no usable files (build {}, language '{}'):{}", buildId, langPrefix, report));
				return;
			}

			g_Api.AddSearchPath(dir.c_str(), kSearchPathPriority, kSearchPathDevice, 0);
			const bool baseFound = g_Api.ZoneFileExists(g_BaseZone.c_str());
			const bool langFound = g_Api.ZoneFileExists(g_LangZone.c_str());
			LOG("LPC", INFO, "LPC playlists: search path '{}' added (this exe tu{}, build {}, language '{}'):{} the "
				"zone loader {} '{}' and {} '{}'. They load under these names (the zone signature covers the "
				"name, so the tu stays as shipped).", dir, tu, buildId, langPrefix, report,
				baseFound ? "finds" : "does NOT find", g_BaseZone, langFound ? "finds" : "does NOT find", g_LangZone);
			if (!baseFound || !langFound) {
				Fail("the zone loader cannot see the files through the new search path.");
				return;
			}
			SetPhase(Phase::WaitIdle);
		}

		bool HavePlaylist() {
			void* playlist = nullptr;
			std::uint8_t valid = 0;
			SafeRead(g_Api.playlistAsset, playlist);
			SafeRead(g_Api.playlistValid, valid);
			return valid && playlist;
		}

		void DoLoading();

		void DoWaitIdle() {
			if (HavePlaylist()) {
				LOG("LPC", INFO, "LPC playlists: the engine already has a playlist; not loading ours.");
				SetPhase(Phase::Done);
				return;
			}
			int slot0 = -1;
			SafeRead(g_Api.slotState, slot0);
			if (slot0 == 1) {
				LOG("LPC", INFO, "LPC playlists: content slot 0 is loading (the engine's loader has it); "
					"not loading ours.");
				SetPhase(Phase::Done);
				return;
			}
			g_SlotFaked = slot0 == 2;
			std::uint8_t ready = 0;
			std::uint64_t pending = 1;
			SafeRead(g_Api.dbReady, ready);
			SafeRead(g_Api.dbSyncPending, pending);
			const int level = ZoneStatus(kZoneFlag_Level);
			if (!ready || pending || (level >= 1 && level <= 4)) {
				if (!g_WaitLogged && Clock::now() - g_PhaseStart > std::chrono::seconds(20)) {
					g_WaitLogged = true;
					LOG("LPC", WARN, "LPC playlists: still waiting for an idle DB after 20 s (dbReady {}, sync "
						"pending {}, level zone status {}).", ready, pending != 0, level);
				}
				return;
			}

			// OnlineContent_LoadSlotZones, with the shipped names instead of ones built from this exe's tu.
			// DB_SyncXAssets blocks until both zones are in, so log first: a drop inside would hide it.
			LOG("LPC", INFO, "LPC playlists: loading '{}' and '{}' ({}; blocks until loaded).", g_LangZone, g_BaseZone,
				g_SlotFaked ? "content slot 0 already reads 2 with no playlist, i.e. the B3 bypass; left at 2"
					: "content slot 0 -> 1");
			for (auto& n : g_AllocsByType) n.store(0, std::memory_order_relaxed);
			F::XZoneInfo zones[2]{
				{ g_LangZone.c_str(), kZoneFlag_Playlists | kLangZoneBit, 0 },
				{ g_BaseZone.c_str(), kZoneFlag_Playlists, 0 },
			};
			g_LoadInFlight = true;
			g_Api.ScreenUpdateAllow(true);
			g_Api.ScreenUpdateBegin();
			g_Api.LoadXAssets(zones, 2, 0);
			g_Api.SyncXAssets();
			g_Api.ScreenUpdateEnd();
			g_Api.ScreenUpdateAllow(false);
			if (!g_SlotFaked) SafeWrite(g_Api.slotState, 1);
			SetPhase(Phase::Loading);
			// The sync above normally leaves both zones loaded: finish in this tick, so no mode switch
			// can land between here and the callback.
			DoLoading();
		}

		void DoLoading() {
			const int base = ZoneStatus(kZoneFlag_Playlists);
			const int lang = ZoneStatus(kZoneFlag_Playlists | kLangZoneBit);
			if (base != g_LastBaseStatus || lang != g_LastLangStatus) {
				g_LastBaseStatus = base;
				g_LastLangStatus = lang;
				LOG("LPC", INFO, "LPC playlists: zone status base {} / language {} (4 or 9 = loaded).", base, lang);
			}

			// Decided by the playlist, not slot 0: on LIVE the B3 bypass can write 2 over our 1.
			if (HavePlaylist()) {
				g_LoadInFlight = false;
				SetPhase(Phase::Done);
				LOG("LPC", INFO, "LPC playlists: the engine's own loader finished slot 0; its callback ran. "
					"Allocations by asset type:{}", AllocSummary());
				return;
			}

			if (base == 4 || base == 9) {
				g_LoadInFlight = false;
				SafeWrite(g_Api.slotState, 2);
				g_Api.PlaylistsLoaded();
				void* playlist = nullptr;
				std::uint8_t valid = 0;
				SafeRead(g_Api.playlistAsset, playlist);
				SafeRead(g_Api.playlistValid, valid);
				SetPhase(Phase::Done);
				if (valid && playlist) {
					LOG("LPC", INFO, "LPC playlists: zones loaded, slot callback ran: playlist asset {}, "
						"g_playlistValid {}. Allocations by asset type:{}", playlist, valid, AllocSummary());
				}
				else {
					LOG("LPC", ERROR, "LPC playlists: zones loaded and the slot callback ran, but there is still no "
						"playlist (asset {}, g_playlistValid {}). Allocations by asset type:{}", playlist, valid,
						AllocSummary());
				}
				return;
			}

			if (Clock::now() - g_PhaseStart > kLoadTimeout) {
				Fail(std::format("the zones did not load within 60 s (status base {} / language {}). "
					"Allocations by asset type:{}", base, lang, AllocSummary()));
			}
		}
	}

	void Init(std::uintptr_t moduleBase, std::size_t imageSize) {
		// LIVE + our backend: the backend lists no LPC objects, so the engine never gets a playlist and
		// the online director has no modes or maps (measured 21:27). Same load as offline.
		if (Boot::Current().IsOnline() && !Boot::Current().backend) {
			LOG("LPC", INFO, "LIVE boot without the local backend: LPC content is left to the engine.");
			return;
		}
		std::error_code ec;
		const fs::path src = SourceDir();
		if (!Settings::Get().localPlaylists) {
			LOG("LPC", INFO, "\"local_playlists\" is off in cw-mod.json: LPC playlists OFF.");
			return;
		}
		int candidates = 0;
		try {
			for (const auto& e : fs::directory_iterator(src, ec)) {
				if (e.is_regular_file(ec) && std::regex_match(e.path().filename().string(), PlaylistsName())) ++candidates;
			}
		}
		catch (const std::exception& e) {
			LOG("LPC", ERROR, "LPC playlists OFF: reading cw-mod/lpc failed: {}", e.what());
			return;
		}
		if (!candidates) {
			LOG("LPC", INFO, "LPC playlists OFF: no core_playlists_*.ff in cw-mod/lpc. Copy the retail "
				"core_playlists_tu*_100_<id>.ff and <lang>_core_playlists_tu*_100_<id>.ff there to load them.");
			return;
		}
		if (!ArxanCall::Ready()) {
			LOG("LPC", ERROR, "LPC playlists OFF: no Arxan return gadget for the engine calls.");
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
		auto data = [&]<typename T>(T*& out, std::uintptr_t dumpAbs, const char* name) {
			out = static_cast<T*>(addr(dumpAbs));
			if (!out) missing += std::format(" {}", name);
		};
		thunk(g_Api.GetTuVersion, kDump_Com_GetTuVersion, "Com_GetTuVersion");
		thunk(g_Api.GetBuildId, kDump_Lpc_GetBuildContentIdString, "Lpc_GetBuildContentIdString");
		thunk(g_Api.GetLanguage, kDump_Loc_GetLanguage, "Loc_GetLanguage");
		thunk(g_Api.LanguagePrefix, kDump_Loc_GetLanguagePrefix, "Loc_GetLanguagePrefix");
		thunk(g_Api.AddSearchPath, kDump_FS_AddSearchPath, "FS_AddSearchPath");
		thunk(g_Api.ZoneFileExists, kDump_DB_ZoneFileExists, "DB_ZoneFileExists");
		thunk(g_Api.LoadXAssets, kDump_DB_LoadXAssets, "DB_LoadXAssets");
		thunk(g_Api.SyncXAssets, kDump_DB_SyncXAssets, "DB_SyncXAssets");
		thunk(g_Api.ScreenUpdateAllow, kDump_R_RemoteScreenUpdateAllow, "R_RemoteScreenUpdateAllow");
		thunk(g_Api.ScreenUpdateBegin, kDump_R_BeginRemoteScreenUpdate, "R_BeginRemoteScreenUpdate");
		thunk(g_Api.ScreenUpdateEnd, kDump_R_EndRemoteScreenUpdate, "R_EndRemoteScreenUpdate");
		thunk(g_Api.PlaylistsLoaded, kDump_OnlineContent_OnPlaylistsLoaded, "OnlineContent_OnPlaylistsLoaded");
		data(g_Api.slotTable, kDump_g_onlineContentSlotTable, "g_onlineContentSlotTable");
		data(g_Api.slotState, kDump_g_onlineContentSlots, "g_onlineContentSlots");
		data(g_Api.zoneRows, kDump_g_zoneInfoRows, "g_zoneInfoRows");
		data(g_Api.dbReady, kDump_g_dbReady, "g_dbReady");
		data(g_Api.dbSyncPending, kDump_g_dbSyncPending, "g_dbSyncPending");
		data(g_Api.mainThreadId, kDump_g_mainThreadId, "g_mainThreadId");
		data(g_Api.playlistAsset, kDump_g_playlistAsset, "g_playlistAsset");
		data(g_Api.playlistValid, kDump_g_playlistValid, "g_playlistValid");
		data(g_Api.entryFreeHead, kDump_g_xassetEntryFreeHead, "g_xassetEntryFreeHead");
		data(g_Api.entries, kDump_g_xassetEntries, "g_xassetEntries");
		data(g_Api.pools, kDump_g_xassetPools, "g_xassetPools");
		if (!missing.empty()) {
			LOG("LPC", ERROR, "LPC playlists OFF: unresolved{}.", missing);
			return;
		}

		// The slot table row must be the playlists slot and name the callback we thunked; anything else
		// means the anchors are off for this build.
		std::uint32_t flags = 0;
		void* callback = nullptr;
		SafeRead(g_Api.slotTable, flags);
		SafeRead(g_Api.slotTable + 8, callback);
		if (flags != kZoneFlag_Playlists || callback != addr(kDump_OnlineContent_OnPlaylistsLoaded)) {
			LOG("LPC", ERROR, "LPC playlists OFF: content slot 0 is flags 0x{:X} / callback {}, expected 0x{:X} / "
				"{} (anchor mismatch).", flags, callback, kZoneFlag_Playlists,
				addr(kDump_OnlineContent_OnPlaylistsLoaded));
			return;
		}

		SetPhase(Phase::Mount);
		LOG("LPC", WARN, "LPC playlists ON ({} file(s) in cw-mod/lpc): they load at the first idle frame. Opt "
			"out with \"local_playlists\": false in cw-mod.json.", candidates);
	}

	bool Enabled() { return g_Phase.load(std::memory_order_relaxed) != Phase::Off; }

	void Tick() {
		const Phase phase = g_Phase.load(std::memory_order_relaxed);
		if (phase == Phase::Off || phase == Phase::Done || phase == Phase::Failed) return;
		// The loader and the slot callback expect the main thread (both check Sys_IsMainThread).
		if (!OnMainThread()) {
			if (!g_OffThreadLogged) {
				g_OffThreadLogged = true;
				std::uint64_t main = 0;
				SafeRead(g_Api.mainThreadId, main);
				LOG("LPC", WARN, "LPC playlists: the tick runs on thread {}, not the main thread {}; waiting.",
					GetCurrentThreadId(), main);
			}
			return;
		}
		switch (phase) {
		case Phase::Mount: DoMount(); break;
		case Phase::WaitIdle: DoWaitIdle(); break;
		case Phase::Loading: DoLoading(); break;
		default: break;
		}
	}

	bool LoadInFlight() { return g_LoadInFlight.load(std::memory_order_relaxed); }

	void BeforeAssetAlloc(std::uint8_t type) {
		g_AllocsByType[type].fetch_add(1, std::memory_order_relaxed);

		// The entries are a fixed array; a head outside it is the engine's answer to a failed zone
		// signature (DB_TamperResponse_CorruptEntryFreeList), which drops on the very next allocation.
		void* entryHead = nullptr;
		if (!g_EntryListLogged.load(std::memory_order_relaxed) && SafeRead(g_Api.entryFreeHead, entryHead)) {
			const auto head = reinterpret_cast<std::uintptr_t>(entryHead);
			const auto first = reinterpret_cast<std::uintptr_t>(g_Api.entries);
			if (head && (head < first || head >= first + kXAssetEntryArrayBytes) && !g_EntryListLogged.exchange(true)) {
				LOG("LPC", ERROR, "LPC playlists: a zone FAILED its signature check: the engine's tamper response "
					"moved the asset-entry free list out of the entry array ({} not in {}..+0x{:X}). The next "
					"allocation drops 0x3F6FDE09 and the game closes. Allocating type 0x{:02X}; by asset type:{}",
					entryHead, static_cast<const void*>(g_Api.entries), kXAssetEntryArrayBytes, type, AllocSummary());
			}
			else if (!head && !g_EntryListLogged.exchange(true)) {
				LOG("LPC", ERROR, "LPC playlists: the global asset-entry list is EMPTY (allocating type 0x{:02X}); the "
					"engine ERR_DROPs 0x3F6FDE09 now. Allocations by asset type:{}", type, AllocSummary());
			}
		}

		const std::uint8_t* const pool = g_Api.pools + 32 * type;
		std::uint8_t singleton = 0;
		void* items = nullptr;
		void* freeHead = nullptr;
		SafeRead(pool + 16, singleton);
		SafeRead(pool, items);
		SafeRead(pool + 24, freeHead);
		if ((singleton ? items : freeHead) || g_PoolEmptyLogged[type].exchange(true)) return;
		int count = -1, used = -1;
		SafeRead(pool + 12, count);
		SafeRead(pool + 20, used);
		LOG("LPC", ERROR, "LPC playlists: asset type 0x{:02X} pool is {} ({} of {} used); the engine ERR_DROPs "
			"0xE554F200 now.", type, singleton ? "a singleton with no item" : "FULL", used, count);
	}
}
