// ZM progression without Demonware's own. See zm_progression.hpp for the why; dump_anchors.hpp for the addresses.
#include "common.hpp"
#include "game/zm_progression.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/arxan_call.hpp"
#include "game/boot_profile.hpp"
#include "game/dump_anchors.hpp"
#include "game/settings.hpp"
#include "game/function_types.hpp"

#include <ShlObj.h>
#pragma comment(lib, "shell32.lib")

#include <algorithm>
#include <atomic>
#include <climits>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace Client::Game::ZmProgression {
	namespace {
		namespace F = Functions;

		std::atomic_bool g_Enabled{ false };
		std::atomic_bool g_AeAvailable{ false };
		std::atomic_bool g_SuppressLuiFatals{ false };
		std::atomic<int> g_BonusXp{ 0 };
		std::atomic_bool g_Tainted[2]{};
		std::atomic<int> g_XpDetailBudget{ 0 };
		thread_local int t_SuppressDepth = 0;

		// Resolved once in Init, read from any thread afterwards.
		struct Api {
			F::LiveUser_GetXuidIfSignedInT* GetXuid{};
			F::PlayerData_GetBufferT* GetBuffer{};
			F::PlayerData_IsBufferReadyT* IsBufferReady{};
			F::StatsMapIdT* GametypeMapId{};
			F::StatsMapIdT* PrimaryMapId{};
			F::StatsTransfer_IsValidForControllerT* RecordValid{};
			F::BoolFnT* DwTokensRequired{};
			F::SV_IsClientStatsSlotReadyT* SlotReady{};
			F::Dvar_GetBoolT* DvarGetBool{};
			F::LiveStorage_GetStatsDdlRootForSourceT* RootForSource{};
			F::Ddl_IsInstanceValidT* InstanceValid{};
			F::Ddl_InitRootStateT* InitRootState{};
			F::Ddl_MoveToMemberByHashT* MoveToMember{};
			F::Ddl_GetUInt64T* GetUInt64{};
			F::Ddl_SetUInt64T* SetUInt64{};
			F::Ddl_ParseHeaderT* ParseHeader{};
			// The AE block. Optional: missing ones only turn the AE stand-in off.
			F::Ddl_FindRootByHashT* FindRoot{};
			F::Ddl_InitInstanceT* InitInstance{};
			F::Ddl_GetValueT* GetValue{};
			F::Ddl_PushStateToLuaT* PushStateToLua{};
			F::LiveStats_GetXpT* GetAeXp{};
			F::LiveStats_SetXpT* SetAeXp{};
			F::LiveStats_SetAarModelFlagT* SetAarFlag{};
			F::Rank_GetLevelForXpT* LevelForXp{};
			F::Rank_GetMaxXpT* MaxRankXp{};
			F::SV_GetClientStatsInstanceT* SvStats{};
			F::Ddl_ScrambleInstanceBufferT* Scramble{};
			F::Ddl_UnscrambleInstanceBufferT* Unscramble{};
			std::uint8_t* statsTransfer{};
			void** integrityDvarSlot{};
		} g_Api;

		// The stand-in for LiveUser+40024: the ae_sync instance at +18328 over its 5120-byte buffer
		// at +13208, and the 32-byte root-state slot at +18392 that GetStatsBufferForUi fills.
		struct AeBlock {
			alignas(16) std::uint8_t buffer[kAeSyncBufferSize]{};
			alignas(16) std::uint8_t instance[64]{};
			alignas(16) std::uint8_t rootState[32]{};
			bool ready = false;
			int mirroredXp = -1;
			std::uint64_t lastRefreshMs = 0;
			// The engine's own block on an online boot (SyncEngineAe). The first two are also read
			// without the lock: the getter behind them is called for every stat read.
			std::atomic<void*> engineInst{ nullptr };   // the instance the local block was last copied into
			std::atomic<std::uint64_t> engineRefreshMs{ 0 };
			int engineXp = -1;               // the rankxp last written to it
			int engineHeld = 0;              // what it read back then
			bool engineForeign = false;      // it held XP before we touched it: a backend fills it
		};
		AeBlock g_Ae[2];
		// Back the transfer record's two source-2 instances (+176 prematch, +416 current).
		alignas(16) std::uint8_t g_AePrematchBuffer[2][kAeSyncBufferSize];
		alignas(16) std::uint8_t g_AeCurrentBuffer[2][kAeSyncBufferSize];
		// Backs the server's source-2 slot for a local player (OnServerClientStatsReady): gun XP and
		// camo challenges land here during the match, and the final commit copies it back.
		alignas(16) std::uint8_t g_SvAeBuffer[2][kAeSyncBufferSize];
		std::atomic<int> g_SvAeClient[2]{ -1, -1 };
		std::recursive_mutex g_AeLock;

		// The menus see LIVE while the engine stays LAN (LobbyRoot_SetNetworkModeModel detour).
		std::atomic_bool g_LiveMenus{ false };
		std::atomic_bool g_LanLobby{ false };
		// What TickMenuNetworkMode compares (dump_anchors.hpp); all three are plain globals.
		struct MenuModelAddrs {
			const int* engineMode{};
			const std::uint32_t* modelId{};
			const std::uint8_t* nodes{};
		} g_MenuModel;

		// The local ae_sync block on disk, next to the engine's .cgp saves (and backed up with them).
		constexpr char kAeFileMagic[4] = { 'C', 'W', 'A', 'E' };
		constexpr std::uint32_t kAeFileVersion = 1;
		struct AeFileHeader {
			char magic[4];
			std::uint32_t version;
			std::uint64_t rootHash;
			std::int32_t bufferSize;
			std::int32_t rootSize;   // root+60, so a changed ae_sync layout is refused, not misread
			std::uint64_t xuid;
		};

		std::filesystem::path DocumentsPlayerDir();

		// Return addresses of every known caller of the two gates (RVA = call site + 4 - imagebase).
		// Anything not in the table is logged by RVA and is worth a look.
		struct KnownSite { std::uintptr_t rva; const char* name; };
		constexpr KnownSite kKnownSites[] = {
			{ 0x5E41305, "GScr_AddRankXp (weapon XP)" },
			{ 0x5E4203B, "GSC native 0xF93152A5 (sub_7FF722A01F70)" },
			{ 0x5E4866C, "GScr_AddWeaponStat" },
			{ 0x5E48B5D, "GScr_MarkWeaponUsedForStats" },
			{ 0x5E48CB4, "GScr_ProcessUsedWeaponStats" },
			{ 0x7023946, "G_AddPlayerRankXp (all rank XP)" },
			{ 0x702F5DB, "LiveStats_RecordMatchStatsIfEnabled" },
			{ 0x8759F6E, "LiveStorage_CommitStatsTransferAndRecap" },
			{ 0x875A53D, "LiveStorage_BeginStatsTransfer (LIVE-only loop)" },
			{ 0x8760266, "LiveStorage_CommitStatsTransfer" },
			{ 0x8761831, "LiveStorage_BuildStatsTransferSlot (prematch copy)" },
			{ 0x5E49430, "GSC stat helper sub_7FF722A093F0" },
			{ 0x5E4AA08, "GSC stat helper sub_7FF722A0A800" },
			{ 0x5E4BA1B, "GSC stat helper sub_7FF722A0B850" },
			{ 0x5E4C624, "GSC stat helper sub_7FF722A0BFA0" },
			{ 0x5E4CEFE, "GSC stat helper sub_7FF722A0CD00" },
			{ 0x5E4DB6E, "GSC stat helper sub_7FF722A0D7B0" },
			{ 0x5E4E206, "GSC stat helper sub_7FF722A0E1E0" },
		};

		std::uintptr_t g_ModuleBase = 0;
		std::mutex g_SiteLock;
		std::vector<std::pair<const char*, std::uintptr_t>> g_SeenSites;

		const char* SiteName(std::uintptr_t rva) {
			for (const auto& s : kKnownSites)
				if (s.rva == rva) return s.name;
			return "UNKNOWN caller";
		}

		// Packed session word: gameMode bits 0-3, networkMode bits 4-7, matchType bits 12-15.
		std::uint32_t SessionWord() {
			std::uint32_t w = 0xFFFFFFFFu;
			if (auto* p = g_Pointers ? g_Pointers->m_g_sessionModePacked : nullptr)
				SafeRead(p, w);
			return w;
		}
		int GameMode(std::uint32_t w)    { return static_cast<int>(w & 0xF); }
		int NetworkMode(std::uint32_t w) { return static_cast<int>((w >> 4) & 0xF); }
		int MatchType(std::uint32_t w)   { return static_cast<int>((w >> 12) & 0xF); }

		std::uint8_t* Record(int controller) {
			if (!g_Api.statsTransfer || controller < 0 || controller > 1) return nullptr;
			return g_Api.statsTransfer + kStatsXfer_Stride * static_cast<std::size_t>(controller);
		}

		// A DDL accessor state, laid out as PlayerData_ReadBufferPlayerXuid builds it on its stack.
		struct alignas(16) DdlState {
			std::uint8_t bytes[64]{};
			void Reset() {
				std::memset(bytes, 0, sizeof(bytes));
				*reinterpret_cast<int*>(bytes + 8) = -1;
			}
		};

		// player_xuid of a playerdata instance. `found` says whether the map has the member at all.
		std::uint64_t ReadPlayerXuid(void* instance, bool& found) {
			found = false;
			if (!instance || !g_Api.InstanceValid(instance)) return 0;
			DdlState root, member;
			root.Reset();
			member.Reset();
			g_Api.InitRootState(root.bytes, instance);
			const std::uint64_t hash = kDdlMember_PlayerXuid;
			if (!g_Api.MoveToMember(root.bytes, member.bytes, &hash)) return 0;
			found = true;
			return g_Api.GetUInt64(member.bytes, instance);
		}

		// kDdlInst_WriteMode of an instance, -1 if unreadable.
		int InstanceWriteMode(void* instance) {
			int mode = -1;
			if (instance) SafeRead(static_cast<std::uint8_t*>(instance) + kDdlInst_WriteMode, mode);
			return mode;
		}

		// A progression map is read-only once its file has loaded (kDdlInst_WriteMode), and every DDL
		// setter refuses such an instance. It is opened for this one write the way the engine opens it for
		// a server stat delta: mode 1, write, the old mode back.
		bool WritePlayerXuid(void* instance, std::uint64_t xuid) {
			if (!instance || !g_Api.InstanceValid(instance)) return false;
			DdlState root, member;
			root.Reset();
			member.Reset();
			g_Api.InitRootState(root.bytes, instance);
			const std::uint64_t hash = kDdlMember_PlayerXuid;
			if (!g_Api.MoveToMember(root.bytes, member.bytes, &hash)) return false;
			auto* const modeAt = static_cast<std::uint8_t*>(instance) + kDdlInst_WriteMode;
			const int mode = InstanceWriteMode(instance);
			const bool reopen = mode != -1 && mode != kDdlWriteMode_Writable;
			if (reopen) SafeWrite(modeAt, kDdlWriteMode_Writable);
			const bool ok = g_Api.SetUInt64(member.bytes, instance, xuid);
			if (reopen) SafeWrite(modeAt, mode);
			return ok;
		}

		// PlayerStatsList.rankxp.statvalue of a zm_progression instance, -1 if unreadable.
		int ReadRankXp(void* instance) {
			if (!instance || !g_Api.InstanceValid(instance)) return -1;
			DdlState root, list, stat, value;
			root.Reset();
			list.Reset();
			stat.Reset();
			value.Reset();
			g_Api.InitRootState(root.bytes, instance);
			const std::uint64_t path[] = { kDdlMember_PlayerStatsList, kDdlMember_RankXp, kDdlMember_StatValue };
			if (!g_Api.MoveToMember(root.bytes, list.bytes, &path[0])) return -1;
			if (!g_Api.MoveToMember(list.bytes, stat.bytes, &path[1])) return -1;
			if (!g_Api.MoveToMember(stat.bytes, value.bytes, &path[2])) return -1;
			return static_cast<int>(g_Api.GetValue(value.bytes, instance));
		}

		int SavedRankXp(int controller) {
			if (!g_Api.IsBufferReady(controller, kMapId_ZmProgression, -1)) return -1;
			return ReadRankXp(g_Api.GetBuffer(controller, kMapId_ZmProgression, -1));
		}

		// LiveStats_SetXp keeps only the part above the top rank when xp exceeds it, so fill base_xp
		// to the top first. Returns what LiveStats_GetXp reads back.
		int WriteAeXp(void* instance, int xp) {
			const int top = g_Api.MaxRankXp();
			if (top > 0 && xp > top) g_Api.SetAeXp(instance, 0xFFFFFFFFu, top);
			g_Api.SetAeXp(instance, 0xFFFFFFFFu, xp);
			return static_cast<int>(g_Api.GetAeXp(instance, 0xFFFFFFFFu));
		}

		// A fresh ae_sync instance over `buffer`, the way LiveStats_CreateLiveUserStatsInstance makes one.
		bool InitAeInstance(void* buffer, void* instance) {
			void* root = g_Api.FindRoot(kDdlRoot_AeSync, 0);
			if (!root) return false;
			std::memset(buffer, 0, kAeSyncBufferSize);
			g_Api.InitInstance(buffer, kAeSyncBufferSize, root, instance, 0, 0, 0);
			return g_Api.InstanceValid(instance);
		}

		int AeRootSize() {
			int size = -1;
			if (void* root = g_Api.FindRoot(kDdlRoot_AeSync, 0))
				SafeRead(reinterpret_cast<std::uint8_t*>(root) + 60, size);
			return size;
		}

		std::size_t NonZeroBytes(const std::uint8_t* p, std::size_t n) {
			return static_cast<std::size_t>(std::count_if(p, p + n, [](std::uint8_t v) { return v != 0; }));
		}

		// A DDL instance's own fields (dump_anchors.hpp).
		struct InstView {
			std::uint8_t* buffer{};
			int size{};
			std::uint8_t* root{};
			std::uint8_t scrambled{};
			std::uint32_t key{};
		};

		bool ViewInstance(void* instance, InstView& v) {
			auto* const p = static_cast<std::uint8_t*>(instance);
			return p && SafeRead(p, v.buffer) && SafeRead(p + kDdlInst_Size, v.size)
				&& SafeRead(p + kDdlInst_Root, v.root) && SafeRead(p + kDdlInst_Scrambled, v.scrambled)
				&& SafeRead(p + kDdlInst_ScrambleKey, v.key) && v.buffer && v.root;
		}

		// An ae_sync instance over a full-size buffer: the engine's LiveUser block, or a local one.
		bool IsAeInstance(const InstView& v) {
			return v.size == kAeSyncBufferSize && v.root == g_Api.FindRoot(kDdlRoot_AeSync, 0);
		}

		// Runs `copy` over the instance's plain bytes: unscrambled in place first and scrambled again
		// with the same key after, the way Ddl_CopyInstanceToInstance reads a scrambled source.
		template <typename Fn>
		bool WithPlainBuffer(void* instance, const InstView& v, Fn&& copy) {
			auto* const flag = static_cast<std::uint8_t*>(instance) + kDdlInst_Scrambled;
			if (v.scrambled) {
				g_Api.Unscramble(instance, v.buffer, static_cast<unsigned int>(v.size));
				std::uint8_t still = 1;
				if (!SafeRead(flag, still) || still) return false;   // refused: the bytes are still scrambled
			}
			const bool ok = copy();
			if (v.scrambled) g_Api.Scramble(instance, v.buffer, static_cast<unsigned int>(v.size), v.key);
			return ok;
		}

		// The plain bytes of the engine's ae_sync block -> out[kAeSyncBufferSize], header included.
		// `out` is written only when the whole read worked.
		bool CopyOutInstance(void* instance, std::uint8_t* out) {
			InstView v;
			if (!ViewInstance(instance, v) || !IsAeInstance(v)) return false;
			std::uint8_t plain[kAeSyncBufferSize];
			if (!WithPlainBuffer(instance, v, [&] { return SafeCopy(plain, v.buffer, kAeSyncBufferSize); })) return false;
			std::memcpy(out, plain, kAeSyncBufferSize);
			return true;
		}

		// in[kAeSyncBufferSize] -> the engine's ae_sync block, everything after its header. The header
		// stays the engine's own: an import binds the root from it.
		bool CopyIntoInstance(void* instance, const std::uint8_t* in) {
			InstView v;
			int header = -1;
			if (!ViewInstance(instance, v) || !IsAeInstance(v)) return false;
			if (!SafeRead(v.root + kDdlRoot_HeaderBytes, header) || header < 0 || header > 64) return false;
			return WithPlainBuffer(instance, v, [&] {
				return SafeCopy(v.buffer + header, in + header, kAeSyncBufferSize - static_cast<std::size_t>(header));
			});
		}

		std::filesystem::path AeFilePath(int controller) {
			const auto dir = DocumentsPlayerDir();
			return dir.empty() ? dir : dir / std::format("cwmod_ae_sync_{}.bin", controller);
		}

		// Caller holds g_AeLock; b.instance was just initialised over b.buffer.
		void LoadAeFile(int controller, AeBlock& b) {
			const auto path = AeFilePath(controller);
			std::error_code ec;
			if (path.empty() || !std::filesystem::exists(path, ec)) {
				LOG("Progression", INFO, "AE file (controller {}): none yet ({}). Gun levels and camo progress "
					"start empty and are saved there after your first finished Zombies match.", controller,
					path.string());
				return;
			}
			AeFileHeader h{};
			std::vector<std::uint8_t> data(kAeSyncBufferSize);
			std::ifstream in(path, std::ios::binary);
			if (!in.read(reinterpret_cast<char*>(&h), sizeof(h)) ||
				!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
				LOG("Progression", ERROR, "AE file (controller {}): {} is too short; ignored, starting empty.",
					controller, path.string());
				return;
			}
			const int rootSize = AeRootSize();
			if (std::memcmp(h.magic, kAeFileMagic, sizeof(kAeFileMagic)) != 0 || h.version != kAeFileVersion ||
				h.rootHash != kDdlRoot_AeSync || h.bufferSize != kAeSyncBufferSize || h.rootSize != rootSize) {
				LOG("Progression", ERROR, "AE file (controller {}): {} does not match this build (version {}, root "
					"0x{:X}, buffer {}, root size {} vs {}); ignored, starting empty. The file is left as it is.",
					controller, path.string(), h.version, h.rootHash, h.bufferSize, h.rootSize, rootSize);
				return;
			}
			std::memcpy(b.buffer, data.data(), kAeSyncBufferSize);
			b.mirroredXp = -1;
			LOG("Progression", INFO, "AE file (controller {}): loaded {} ({} non-zero bytes of gun XP, camo and "
				"challenge progress).", controller, path.string(), NonZeroBytes(b.buffer, kAeSyncBufferSize));
		}

		// Caller holds g_AeLock. Written to a temp file, then renamed over the old one.
		bool SaveAeFile(int controller, const AeBlock& b) {
			const auto path = AeFilePath(controller);
			if (path.empty()) return false;
			auto tmp = path;
			tmp += L".tmp";
			AeFileHeader h{};
			std::memcpy(h.magic, kAeFileMagic, sizeof(kAeFileMagic));
			h.version = kAeFileVersion;
			h.rootHash = kDdlRoot_AeSync;
			h.bufferSize = kAeSyncBufferSize;
			h.rootSize = AeRootSize();
			h.xuid = g_Api.GetXuid(controller);
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				out.write(reinterpret_cast<const char*>(&h), sizeof(h));
				out.write(reinterpret_cast<const char*>(b.buffer), kAeSyncBufferSize);
				if (!out) {
					LOG("Progression", ERROR, "AE file (controller {}): could not write {}.", controller, tmp.string());
					return false;
				}
			}
			std::error_code ec;
			std::filesystem::rename(tmp, path, ec);
			if (ec) {
				LOG("Progression", ERROR, "AE file (controller {}): could not replace {} ({}).", controller,
					path.string(), ec.message());
				return false;
			}
			return true;
		}

		// Caller holds g_AeLock.
		bool EnsureAeBlock(int controller) {
			AeBlock& b = g_Ae[controller];
			if (b.ready) return true;
			static bool s_Logged[2]{};
			b.ready = InitAeInstance(b.buffer, b.instance);
			if (b.ready) {
				LOG("Progression", INFO, "AE block (controller {}): local ae_sync instance created. The level, "
					"Barracks and AAR now read the saved rankxp instead of an empty Demonware block.", controller);
				LoadAeFile(controller, b);
			}
			else if (!s_Logged[controller]) {
				s_Logged[controller] = true;
				LOG("Progression", WARN, "AE block (controller {}): ae_sync DDL root 0x{:X} not loaded yet; the "
					"level stays at 0 until it is (retrying on every read).", controller, kDdlRoot_AeSync);
			}
			return b.ready;
		}
		std::string EnsurePlayerXuid(int controller, unsigned int mapId, std::uint64_t xuid) {
			void* inst = g_Api.GetBuffer(controller, mapId, -1);
			bool found = false;
			const std::uint64_t before = ReadPlayerXuid(inst, found);
			if (!found) return "no player_xuid member (or instance invalid)";
			if (before == xuid) return std::format("player_xuid already 0x{:X}", before);
			if (xuid == 0) return std::format("player_xuid 0x{:X}, local XUID is 0 - left alone", before);
			const int mode = InstanceWriteMode(inst);
			const bool ok = WritePlayerXuid(inst, xuid);
			bool refound = false;
			const std::uint64_t after = ReadPlayerXuid(inst, refound);
			const char* const how = mode == kDdlWriteMode_Writable ? "writable"
				: "read-only once loaded, opened for this write";
			if (ok && after == xuid) {
				return std::format("player_xuid 0x{:X} -> 0x{:X} (stamped; write mode {}: {})", before, after, mode, how);
			}
			return std::format("player_xuid 0x{:X} -> 0x{:X} (*** STAMP FAILED ***: the setter returned {}, write mode {} "
				"({}), now {})", before, after, ok, mode, how, InstanceWriteMode(inst));
		}

		// The AE block the engine is about to copy into the record as source 2.
		std::string DescribeAeSource(int controller) {
			const bool engines = EngineAeInstance(controller) != nullptr;
			void* block = AeBlockFor(controller, true);
			InstView v;
			if (!block || !ViewInstance(block, v))
				return "no AE block (ae_sync root not loaded?): the engine skips source 2, gun XP is not recorded";
			return std::format("{} AE block: AE xp {}, {}{}", engines ? "the engine's own" : "the local stand-in",
				g_Api.GetAeXp(block, 0xFFFFFFFFu), v.scrambled ? "scrambled in memory" : "plain in memory",
				engines && g_Ae[controller].engineForeign ? ", filled by a backend (not ours to write)" : "");
		}

		// What Ddl_CopyInstanceToInstance will decide for this source, short of the import itself.
		std::string DescribeCopySource(int controller, int source, unsigned int mapId, bool& usable) {
			usable = false;
			const bool ready = g_Api.IsBufferReady(controller, mapId, -1);
			void* inst = g_Api.GetBuffer(controller, mapId, -1);
			const bool valid = inst && g_Api.InstanceValid(inst);
			if (!ready || !valid) {
				return std::format("map {}: ready={} instanceValid={} -> NOT usable (the redirect did not "
					"load it; the copy would be a silent no-op)", mapId, ready, valid);
			}

			void* root = nullptr;
			void* buffer = nullptr;
			SafeRead(reinterpret_cast<std::uint8_t*>(inst) + 16, root);
			SafeRead(inst, buffer);
			int rootSize = -1;
			std::uint8_t headerless = 0;
			if (root) {
				SafeRead(reinterpret_cast<std::uint8_t*>(root) + 60, rootSize);
				SafeRead(reinterpret_cast<std::uint8_t*>(root) + 83, headerless);
			}

			int capacity = -1;
			if (void* slotRoot = g_Api.RootForSource(source))
				SafeRead(reinterpret_cast<std::uint8_t*>(slotRoot) + 60, capacity);

			bool integrityOn = false;
			void* integrityDvar = nullptr;
			if (g_Api.integrityDvarSlot && SafeRead(g_Api.integrityDvarSlot, integrityDvar) && integrityDvar)
				integrityOn = g_Api.DvarGetBool(integrityDvar);

			alignas(16) std::uint8_t hdr[32]{};
			std::uint16_t magic = 0;
			std::uint8_t buildId = 0;
			std::uint64_t guid = 0;
			if (buffer) {
				g_Api.ParseHeader(hdr, buffer, static_cast<char>(headerless));
				std::memcpy(&magic, hdr + 2, sizeof(magic));
				buildId = hdr[4];
				std::memcpy(&guid, hdr + 8, sizeof(guid));
			}
			const bool headerOk = hdr[0] != 0 || (magic == 0x7376 && buildId == 7 && guid != 0);

			std::string verdict;
			if (capacity >= 0 && rootSize > capacity) verdict = "WILL FAIL: source larger than the slot";
			else if (integrityOn && !headerOk) verdict = "WILL FAIL: integrity check on and the header is not sealed";
			else { verdict = "ok up to the import"; usable = true; }

			return std::format("map {}: size {} vs slot {}, integrity dvar {}, header magic 0x{:04X} "
				"buildId {} guid 0x{:X}{} -> {}", mapId, rootSize, capacity, integrityOn ? "ON" : "off",
				magic, buildId, guid, headerless ? " (headerless root)" : "", verdict);
		}

		std::filesystem::path DocumentsPlayerDir() {
			PWSTR docs = nullptr;
			std::filesystem::path out;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs)
				out = std::filesystem::path(docs) / L"Call Of Duty Black Ops Cold War" / L"player";
			if (docs) CoTaskMemFree(docs);
			return out;
		}

		// One copy of the player folder per boot, newest 10 kept. The engine writes a main + backup
		// file itself; this guards against OUR logic ever writing a bad save over a good one.
		void BackupPlayerFolder() {
			std::error_code ec;
			const auto src = DocumentsPlayerDir();
			if (src.empty() || !std::filesystem::exists(src, ec)) {
				LOG("Progression", INFO, "no player folder to back up yet ({}).", src.string());
				return;
			}
			const auto root = std::filesystem::current_path(ec) / "cw-mod" / "backups";
			const std::time_t now = std::time(nullptr);
			std::tm tm{};
			localtime_s(&tm, &now);
			const auto dst = root / std::format("player-{:04}{:02}{:02}-{:02}{:02}{:02}", tm.tm_year + 1900,
				tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
			std::filesystem::create_directories(dst, ec);
			int copied = 0;
			for (const auto& e : std::filesystem::directory_iterator(src, ec)) {
				if (!e.is_regular_file(ec)) continue;
				const bool ours = e.path().extension() == ".bin" && e.path().filename().string().starts_with("cwmod_");
				if (e.path().extension() != ".cgp" && !ours) continue;
				if (std::filesystem::copy_file(e.path(), dst / e.path().filename(),
						std::filesystem::copy_options::overwrite_existing, ec))
					++copied;
			}

			std::vector<std::filesystem::path> olds;
			for (const auto& e : std::filesystem::directory_iterator(root, ec))
				if (e.is_directory(ec) && e.path().filename().string().starts_with("player-"))
					olds.push_back(e.path());
			std::sort(olds.begin(), olds.end());
			while (olds.size() > 10) {
				std::filesystem::remove_all(olds.front(), ec);
				olds.erase(olds.begin());
			}
			LOG("Progression", INFO, "backed up {} save file(s) (.cgp + cwmod_*.bin) from {} to {}", copied,
				src.string(), dst.string());
		}
	}

	void Init(std::uintptr_t moduleBase, std::size_t imageSize) {
		g_ModuleBase = moduleBase;
		const auto& boot = Boot::Current();
		const char* const kind = boot.IsOnline() ? "online" : boot.network == Boot::Network::Lan ? "LAN" : "offline";
		if (!Settings::Get().progression) {
			LOG("Progression", INFO, "\"progression\" is off in cw-mod.json: ZM progression OFF ({} boot).", kind);
			return;
		}
		if (!ArxanCall::Ready()) {
			LOG("Progression", ERROR, "no Arxan return gadget: the engine calls this needs would be "
				"silent no-ops, so ZM progression stays OFF.");
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
		thunk(g_Api.GetXuid, kDump_LiveUser_GetXuidIfSignedIn, "GetXuid");
		thunk(g_Api.GetBuffer, kDump_PlayerData_GetBuffer, "GetBuffer");
		thunk(g_Api.IsBufferReady, kDump_PlayerData_IsBufferReady, "IsBufferReady");
		thunk(g_Api.GametypeMapId, kDump_LiveStorage_GetGametypeStatsMapId, "GametypeMapId");
		thunk(g_Api.PrimaryMapId, kDump_LiveStorage_GetPrimaryStatsMapId, "PrimaryMapId");
		thunk(g_Api.RecordValid, kDump_StatsTransfer_IsValidForController, "RecordValid");
		thunk(g_Api.DwTokensRequired, kDump_Live_DwTokensRequired, "DwTokensRequired");
		thunk(g_Api.SlotReady, kDump_SV_IsClientStatsSlotReady, "SlotReady");
		thunk(g_Api.DvarGetBool, kDump_Dvar_GetBool, "Dvar_GetBool");
		thunk(g_Api.RootForSource, kDump_LiveStorage_GetStatsDdlRootForSource, "RootForSource");
		thunk(g_Api.InstanceValid, kDump_Ddl_IsInstanceValid, "InstanceValid");
		thunk(g_Api.InitRootState, kDump_Ddl_InitRootState, "InitRootState");
		thunk(g_Api.MoveToMember, kDump_Ddl_MoveToMemberByHash, "MoveToMember");
		thunk(g_Api.GetUInt64, kDump_Ddl_GetUInt64, "GetUInt64");
		thunk(g_Api.SetUInt64, kDump_Ddl_SetUInt64, "SetUInt64");
		thunk(g_Api.ParseHeader, kDump_Ddl_ParseHeader, "ParseHeader");
		g_Api.statsTransfer = static_cast<std::uint8_t*>(addr(kDump_g_statsTransfer));
		g_Api.integrityDvarSlot = static_cast<void**>(addr(kDump_dvarPtr_ddlCopyIntegrityCheck));
		if (!g_Api.statsTransfer) missing += " g_statsTransfer";
		if (!g_Pointers || !g_Pointers->m_g_sessionModePacked) missing += " g_sessionModePacked";

		if (!missing.empty()) {
			LOG("Progression", ERROR, "ZM progression OFF: unresolved{}.", missing);
			return;
		}

		std::string aeMissing;
		auto aeThunk = [&]<typename Fn>(Fn*& out, std::uintptr_t dumpAbs, const char* name) {
			void* a = addr(dumpAbs);
			out = a ? ArxanCall::MakeThunk<Fn>(a) : nullptr;
			if (!out) aeMissing += std::format(" {}", name);
		};
		aeThunk(g_Api.FindRoot, kDump_Ddl_FindRootByHash, "Ddl_FindRootByHash");
		// Direct, never thunked: seven arguments (see kDump_Ddl_InitInstance).
		g_Api.InitInstance = reinterpret_cast<F::Ddl_InitInstanceT*>(addr(kDump_Ddl_InitInstance));
		if (!g_Api.InitInstance) aeMissing += " Ddl_InitInstance";
		aeThunk(g_Api.GetValue, kDump_Ddl_GetValue, "Ddl_GetValue");
		aeThunk(g_Api.PushStateToLua, kDump_Ddl_PushStateToLua, "Ddl_PushStateToLua");
		aeThunk(g_Api.GetAeXp, kDump_LiveStats_GetXp, "LiveStats_GetXp");
		aeThunk(g_Api.SetAeXp, kDump_LiveStats_SetXp, "LiveStats_SetXp");
		aeThunk(g_Api.SetAarFlag, kDump_LiveStats_SetAarModelFlag, "LiveStats_SetAarModelFlag");
		aeThunk(g_Api.LevelForXp, kDump_Rank_GetLevelForXp, "Rank_GetLevelForXp");
		aeThunk(g_Api.MaxRankXp, kDump_Rank_GetMaxXp, "Rank_GetMaxXp");
		aeThunk(g_Api.SvStats, kDump_SV_GetClientStatsInstance, "SV_GetClientStatsInstance");
		aeThunk(g_Api.Scramble, kDump_Ddl_ScrambleInstanceBuffer, "Ddl_ScrambleInstanceBuffer");
		aeThunk(g_Api.Unscramble, kDump_Ddl_UnscrambleInstanceBuffer, "Ddl_UnscrambleInstanceBuffer");
		if (aeMissing.empty()) {
			g_AeAvailable = true;
		}
		else {
			LOG("Progression", ERROR, "AE block OFF (unresolved{}): XP still saves, but it will not accumulate "
				"across matches and the level/AAR stay empty.", aeMissing);
		}

		BackupPlayerFolder();
		g_Enabled = true;
		LOG("Progression", WARN, "ZM progression ON ({} boot). XP, weapon levels and upgrades will record in "
			"Zombies matches and save to the player folder.{} Opt out with \"progression\": false in cw-mod.json.",
			kind, boot.IsOnline() ? " The engine's own level block is filled from that save, because the local "
				"backend has no AE service to fill or keep it." : "");

		// Only with the AE block: the menus would otherwise lock items behind levels that never save.
		if (!g_AeAvailable) {
			LOG("Progression", WARN, "LIVE menus OFF: the AE block is unavailable, so gun levels would not save.");
		}
		else if (!Settings::Get().liveMenus) {
			LOG("Progression", INFO, "\"live_menus\" is off in cw-mod.json: {}", boot.lobbyLive
				? "a Zombies custom game keeps its own Create-a-Class rules (everything unlocked, no gun levels)."
				: "the menus stay in LAN mode (everything unlocked, no gun levels shown).");
		}
		else {
			g_MenuModel.engineMode = static_cast<const int*>(addr(kDump_g_lobbyNetworkMode));
			g_MenuModel.modelId = static_cast<const std::uint32_t*>(addr(kDump_g_lobbyRootModel_lobbyNetworkMode));
			g_MenuModel.nodes = static_cast<const std::uint8_t*>(addr(kDump_g_uiModelNodes));
			g_LanLobby = !boot.lobbyLive;
			g_LiveMenus = true;
			if (boot.lobbyLive) {
				LOG("Progression", INFO, "LIVE menus: the lobby is LIVE on this boot, so the menus show locks and gun "
					"levels by themselves. Only a Zombies custom game is still hidden from the two Create-a-Class "
					"progression gates. Opt out with \"live_menus\": false in cw-mod.json.");
			}
			else {
				LOG("Progression", WARN, "LIVE menus ON: the menus are told the lobby is LIVE (the engine stays LAN), "
					"so attachments and camos lock until earned and gun levels show. Opt out with "
					"\"live_menus\": false in cw-mod.json.{}", g_MenuModel.engineMode && g_MenuModel.modelId && g_MenuModel.nodes ? ""
						: " (the per-frame re-apply is OFF: a model address did not resolve)");
			}
		}
	}

	bool Enabled() { return g_Enabled.load(std::memory_order_relaxed); }

	// Any Zombies session. Where the engine already answers true (measured per boot by NoteEngineGate),
	// forcing changes nothing; where it does not (LAN, offline, a custom game), this is the feature.
	bool ShouldForceGate() {
		if (!Enabled() || t_SuppressDepth > 0) return false;
		const std::uint32_t w = SessionWord();
		return w != 0xFFFFFFFFu && GameMode(w) == 0;
	}

	void NoteEngineGate(const char* gate) {
		// Both gates are asked on every XP event: the repeat is answered without the lock.
		thread_local const char* t_Gate = nullptr;
		thread_local std::uint32_t t_Word = 0xFFFFFFFFu;
		const std::uint32_t w = SessionWord();
		if (t_Gate == gate && t_Word == w) return;
		t_Gate = gate;
		t_Word = w;
		static std::vector<std::pair<const char*, std::uint32_t>> s_Seen;
		{
			std::lock_guard lock(g_SiteLock);
			for (const auto& [g, word] : s_Seen)
				if (g == gate && word == w) return;
			s_Seen.emplace_back(gate, w);
		}
		LOG("Progression", INFO, "{}: the engine's own answer is TRUE (session 0x{:X}: gameMode {} network {} "
			"matchType {}); nothing to force.", gate, w, GameMode(w), NetworkMode(w), MatchType(w));
	}

	void NoteForcedSite(const char* gate, std::uintptr_t returnAddress) {
		const std::uintptr_t rva = returnAddress >= g_ModuleBase ? returnAddress - g_ModuleBase : returnAddress;
		{
			std::lock_guard lock(g_SiteLock);
			for (const auto& [g, r] : g_SeenSites)
				if (g == gate && r == rva) return;
			g_SeenSites.emplace_back(gate, rva);
		}
		const std::uint32_t w = SessionWord();
		LOG("Progression", INFO, "{}: forced TRUE for +0x{:X} {} (session 0x{:X}: gameMode {} network {} "
			"matchType {})", gate, rva, SiteName(rva), w, GameMode(w), NetworkMode(w), MatchType(w));
	}

	SuppressScope::SuppressScope() { ++t_SuppressDepth; }
	SuppressScope::~SuppressScope() { --t_SuppressDepth; }

	std::string PrepareBegin(int controller, bool& safeToForce) {
		const std::uint64_t xuid = g_Api.GetXuid(controller);
		const unsigned int gametypeMap = g_Api.GametypeMapId();
		const unsigned int primaryMap = g_Api.PrimaryMapId();
		bool ok0 = false, ok1 = false;
		std::string out = std::format("controller {} XUID 0x{:X}, DW tokens required {}\n", controller, xuid,
			g_Api.DwTokensRequired());
		out += std::format("    source 0 {}\n", EnsurePlayerXuid(controller, gametypeMap, xuid));
		out += std::format("    source 0 {}\n", DescribeCopySource(controller, 0, gametypeMap, ok0));
		out += std::format("    source 1 {}\n", EnsurePlayerXuid(controller, primaryMap, xuid));
		out += std::format("    source 1 {}", DescribeCopySource(controller, 1, primaryMap, ok1));
		// Source 2 exists only with networkMode 2: the engine then copies the AE block into the record
		// and uploads it. Synced now, so what it copies is the save.
		if (NetworkMode(SessionWord()) == 2) out += std::format("\n    source 2 {}", DescribeAeSource(controller));
		safeToForce = ok0 && ok1;
		if (!safeToForce) out += "\n    NOT SAFE TO FORCE: a source map is not usable";
		return out;
	}

	bool PrematchHoldsSave(int controller) {
		std::uint8_t* r = Record(controller);
		const std::uint64_t xuid = g_Api.GetXuid(controller);
		bool found = false;
		return r && xuid != 0 && ReadPlayerXuid(r + 16, found) == xuid;   // +16: source-0 prematch instance
	}

	std::string DescribeRecord(int controller) {
		std::uint8_t* r = Record(controller);
		if (!r) return "record unreadable";
		auto u8 = [&](std::size_t off) { std::uint8_t v = 0; SafeRead(r + off, v); return v; };
		auto i32 = [&](std::size_t off) { int v = -1; SafeRead(r + off, v); return v; };
		std::uint64_t xuid = 0;
		SafeRead(r + kStatsXfer_Xuid, xuid);
		return std::format("record: valid {} maps [{}, {}] gameMode {} network {} xuid 0x{:X} "
			"committed [{}, {}] signature [{}, {}] isValidForController {}", u8(kStatsXfer_Valid),
			i32(kStatsXfer_MapId), i32(kStatsXfer_MapId + 4), i32(kStatsXfer_GameMode),
			i32(kStatsXfer_NetworkMode), xuid, u8(kStatsXfer_Committed), u8(kStatsXfer_Committed + 1),
			u8(kStatsXfer_SignatureIn), u8(kStatsXfer_SignatureIn + 1),
			g_Api.RecordValid(static_cast<unsigned int>(controller)));
	}

	void SetTainted(int controller, bool tainted) {
		if (controller >= 0 && controller <= 1) g_Tainted[controller] = tainted;
	}
	bool IsTainted(int controller) {
		return controller >= 0 && controller <= 1 && g_Tainted[controller].load();
	}

	void OnMatchStart() { g_XpDetailBudget = 40; }

	std::string PrepareCommit(int controller, int source) {
		std::uint8_t* r = Record(controller);
		if (!r || source < 0 || source > 1) return std::format("source {} (not a playerdata source)", source);
		int mapId = -1;
		SafeRead(r + kStatsXfer_MapId + 4 * static_cast<std::size_t>(source), mapId);
		const std::uint64_t xuid = g_Api.GetXuid(controller);
		std::string out = std::format("controller {} source {} map {}: {}\n", controller, source, mapId,
			DescribeRecord(controller));
		if (mapId <= 0) return out + "    no map recorded for this source";

		const bool ready = g_Api.IsBufferReady(controller, static_cast<unsigned int>(mapId), -1);
		out += std::format("    buffer ready {}; {}", ready,
			EnsurePlayerXuid(controller, static_cast<unsigned int>(mapId), xuid));

		// No backend signs stats outside LIVE. Mark the signature as in so the write is not parked
		// waiting for one; the engine clears the flag itself right after queuing the write.
		if (g_Api.DwTokensRequired()) {
			std::uint8_t* sig = r + kStatsXfer_SignatureIn + static_cast<std::size_t>(source);
			std::uint8_t before = 0;
			SafeRead(sig, before);
			if (!before) SafeWrite(sig, std::uint8_t{ 1 });
			out += std::format("\n    DW tokens required: signature flag {} -> 1 (no backend will sign it)", before);
		}
		return out;
	}

	void MarkCommitted(int controller, int source) {
		std::uint8_t* r = Record(controller);
		if (r && source >= 0 && source <= 2)
			SafeWrite(r + kStatsXfer_Committed + static_cast<std::size_t>(source), std::uint8_t{ 1 });
	}

	bool ShouldLogXpEvent() {
		return g_XpDetailBudget.fetch_sub(1, std::memory_order_relaxed) > 0;
	}

	int ReadServerRankXp(int clientNum) {
		if (!g_Pointers || !g_Pointers->m_g_svClients || clientNum < 0) return -1;
		std::uint8_t* clients = nullptr;
		if (!SafeRead(g_Pointers->m_g_svClients, clients) || !clients) return -1;
		int xp = -1;
		SafeRead(clients + kSvClientStride * static_cast<std::size_t>(clientNum) + kSvClient_RankXp, xp);
		return xp;
	}

	bool ServerStatsSlotReady(int clientNum) {
		return g_Api.SlotReady && g_Api.SlotReady(static_cast<std::int16_t>(clientNum), 0);
	}

	void SeedServerRankXp(int clientNum) {
		if (!g_AeAvailable || clientNum < 0 || !ServerStatsSlotReady(clientNum)) return;
		// The engine writes the counter into slot 0's rankxp on every award, so the two only differ
		// before the first one: then slot 0 still holds the client's save, and the counter holds what
		// SV_ClientStatsReady read from the AE blob (nothing in LAN, a blank block if the sync failed).
		const int counter = ReadServerRankXp(clientNum);
		const int saved = ReadRankXp(g_Api.SvStats(static_cast<std::int16_t>(clientNum), 0));
		if (counter < 0 || saved <= counter) return;

		std::uint8_t* clients = nullptr;
		if (!SafeRead(g_Pointers->m_g_svClients, clients) || !clients) return;
		SafeWrite(clients + kSvClientStride * static_cast<std::size_t>(clientNum) + kSvClient_RankXp, saved);
		LOG("Progression", INFO, "XP seed: client {} starts this match at rankxp {} (level {}) from its save; the "
			"server's counter read {}. Without this the match total overwrites the saved rankxp.", clientNum, saved,
			g_Api.LevelForXp(saved), counter);
	}

	void OnServerClientStatsReady(std::uint8_t* cl) {
		if (!g_AeAvailable || !cl || !ShouldForceGate() || !g_Pointers || !g_Pointers->m_g_svClients) return;
		std::uint8_t* clients = nullptr;
		if (!SafeRead(g_Pointers->m_g_svClients, clients) || !clients || cl < clients) return;
		const int clientNum = static_cast<int>((cl - clients) / kSvClientStride);

		auto* state = cl + kSvClient_StatsSlotState + 4 * kStatsSource_Ae;
		auto* inst = cl + kSvClient_StatsInstances + kSvClient_StatsInstanceStride * kStatsSource_Ae;
		int st = -1;
		void* instBuf = nullptr;
		SafeRead(state, st);
		SafeRead(inst, instBuf);
		const bool ours = instBuf == g_SvAeBuffer[0] || instBuf == g_SvAeBuffer[1];
		if (!ours && st >= 2 && st <= 6 && g_Api.InstanceValid(inst)) {
			LOG("Progression", INFO, "Gun XP: client {} already has an AE slot the engine imported from its blob "
				"(state {}, AE xp {}); left alone. Its changes go back to that client's own block.", clientNum, st,
				g_Api.GetAeXp(inst, 0xFFFFFFFFu));
			return;
		}

		// Whose stats these are: source 0 carries the player_xuid our Begin hook stamped.
		bool found = false;
		const std::uint64_t xuid = ReadPlayerXuid(cl + kSvClient_StatsInstances, found);
		int controller = -1;
		for (int c = 0; c < 2 && xuid; ++c)
			if (g_Api.GetXuid(c) == xuid) { controller = c; break; }
		if (controller < 0) {
			LOG("Progression", INFO, "Gun XP: client {} (player_xuid 0x{:X}) is not a player on this PC; its gun "
				"XP and camo progress are not tracked here.", clientNum, xuid);
			return;
		}

		std::lock_guard lock(g_AeLock);
		if (!LocalAeInstance(controller, true)) {
			LOG("Progression", WARN, "Gun XP: client {}: no local AE block yet (ae_sync root not loaded?); gun XP "
				"is not recorded this match.", clientNum);
			return;
		}
		if (!InitAeInstance(g_SvAeBuffer[controller], inst)) {
			LOG("Progression", ERROR, "Gun XP: client {}: could not create the server AE slot; gun XP is not "
				"recorded this match.", clientNum);
			return;
		}
		std::memcpy(g_SvAeBuffer[controller], g_Ae[controller].buffer, kAeSyncBufferSize);
		SafeWrite(state, 2);
		g_SvAeClient[controller] = clientNum;
		LOG("Progression", INFO, "Gun XP: server client {} (controller {}) gets its AE slot from the local block "
			"({} non-zero bytes, AE xp {}). Gun XP and camo challenges record this match and are saved at the "
			"match-end commit.", clientNum, controller, NonZeroBytes(g_SvAeBuffer[controller], kAeSyncBufferSize),
			g_Api.GetAeXp(inst, 0xFFFFFFFFu));
	}

	bool LiveMenusEnabled() { return g_LiveMenus.load(std::memory_order_relaxed); }
	bool LogsNetworkModeQueries() { return LiveMenusEnabled() && g_LanLobby.load(std::memory_order_relaxed); }

	int MenuNetworkMode(int mode) {
		const int shown = LiveMenusEnabled() && mode == kLobbyNetworkMode_Lan ? kLobbyNetworkMode_Live : mode;
		static std::atomic<int> s_LastLogged{ INT_MIN };
		if (s_LastLogged.exchange(mode) != mode) {
			LOG("Progression", INFO, "LIVE menus: lobby network mode {} -> the menus see {}{}.", mode, shown,
				shown != mode ? " (LAN shown as LIVE; the engine stays LAN)" : "");
		}
		return shown;
	}

	void TickMenuNetworkMode() {
		if (!LiveMenusEnabled() || !g_MenuModel.engineMode || !g_MenuModel.modelId || !g_MenuModel.nodes) return;
		int engine = 0;
		std::uint32_t id = 0;
		if (!SafeRead(g_MenuModel.engineMode, engine) || engine != kLobbyNetworkMode_Lan) return;
		if (!SafeRead(g_MenuModel.modelId, id) || id == 0) return;   // lobbyRoot not created yet
		const std::uint8_t* node = g_MenuModel.nodes + kUiModelNode_Stride * id;
		std::int64_t value = 0;
		int type = 0;
		if (!SafeRead(node, value) || !SafeRead(node + kUiModelNode_Type, type)) return;
		if (type == kUiModelType_Int && value == kLobbyNetworkMode_Live) return;
		static std::atomic<int> s_Fixes{ 0 };
		if (s_Fixes.fetch_add(1) < 5) {
			LOG("Progression", INFO, "LIVE menus: the lobbyRoot.lobbyNetworkMode model held {} (type {}): set "
				"before our detour existed, or rebuilt. Re-applying it as LIVE.", value, type);
		}
		ApplyMenuNetworkMode(engine);
	}

	bool HideCustomGameFromCaller(void* L, int mode) {
		if (!LiveMenusEnabled() || mode != kGameMode_MatchmakingManual || !L || !g_Pointers) return false;
		const std::uint32_t w = SessionWord();
		if (w == 0xFFFFFFFFu || GameMode(w) != 0 || MatchType(w) != mode) return false;

		// The two gate closures, re-read at most once a second: the UI Lua state is rebuilt between
		// the frontend and a match, and a stale pointer only stops matching until the next read.
		static void* s_L = nullptr;
		static std::uint64_t s_Gates[2]{};
		static std::uint64_t s_ReadMs = 0;
		static bool s_LoggedRead = false;
		const std::uint64_t now = GetTickCount64();
		if (L != s_L || now - s_ReadMs >= 1000) {
			s_L = L;
			s_ReadMs = now;
			s_Gates[0] = g_Pointers->LuaCoDFunction(L, kLuaHash_CACUtility, kLuaHash_IsProgressionEnabled);
			s_Gates[1] = g_Pointers->LuaCoDFunction(L, kLuaHash_CACUtility, kLuaHash_IsProgressionEnabledAnyMode);
			if (!s_LoggedRead) {
				s_LoggedRead = true;
				LOG("Progression", INFO, "LIVE menus: CACUtility progression gates in this Lua state: "
					"IsProgressionEnabled 0x{:X}, its any-mode twin 0x{:X} (0 = not found).", s_Gates[0], s_Gates[1]);
			}
		}
		if (!s_Gates[0] && !s_Gates[1]) return false;

		std::uint64_t frames[2]{};
		for (int level = 0; level < 2; ++level) {
			frames[level] = g_Pointers->LuaFrameFunction(L, level);
			for (int i = 0; i < 2; ++i) {
				if (!frames[level] || frames[level] != s_Gates[i]) continue;
				static std::atomic_bool s_Logged[2]{};
				if (!s_Logged[i].exchange(true)) {
					LOG("Progression", INFO, "LIVE menus: {} asked 'custom game?' (this lobby is one) and was "
						"told no, so Create-a-Class shows gun levels and locks (Lua level {}).",
						i == 0 ? "IsProgressionEnabled" : "the any-mode progression gate", level);
				}
				return true;
			}
		}
		// Every other caller keeps the real answer. The first few are logged so a gate that never
		// matches (a wrong frame level) shows up next to what the stack did hold.
		static std::atomic<int> s_Passed{ 0 };
		if (s_Passed.fetch_add(1) < 3) {
			LOG("Progression", INFO, "LIVE menus: 'custom game?' from another Lua function (level 0 0x{:X}, level 1 "
				"0x{:X}): real answer kept.", frames[0], frames[1]);
		}
		return false;
	}

	void NoteNetworkModeQuery(void* L) {
		// A frame is "x64:<63-bit path hash>.lua:<line> <name it was called under>". The line is the
		// compiled one, not the decompiled file's; the file hash and the call name are what map it.
		constexpr int kMaxLogged = 250;
		static std::unordered_set<std::string> s_Seen;
		static int s_Logged = 0;
		if (!LiveMenusEnabled() || !L || !g_Pointers || s_Logged >= kMaxLogged) return;
		std::string key = g_Pointers->LuaFrameBrief(L, 1);
		key += " <- ";
		key += g_Pointers->LuaFrameBrief(L, 2);
		if (!s_Seen.insert(key).second) return;
		int engine = -1;
		if (g_MenuModel.engineMode) SafeRead(g_MenuModel.engineMode, engine);
		LOG("Progression", INFO, "LIVE menus: Engine.GetLobbyNetworkMode() = {} for {} <- {}{}", engine, key,
			g_Pointers->LuaFrameBrief(L, 3), ++s_Logged == kMaxLogged ? " (log cap reached)" : "");
	}

	void* LocalAeInstance(int controller, bool forceRefresh) {
		if (!g_AeAvailable || controller < 0 || controller > 1) return nullptr;
		std::lock_guard lock(g_AeLock);
		if (!EnsureAeBlock(controller)) return nullptr;
		AeBlock& b = g_Ae[controller];

		const std::uint64_t now = GetTickCount64();
		if (forceRefresh || now - b.lastRefreshMs >= 500) {
			b.lastRefreshMs = now;
			const int xp = SavedRankXp(controller);
			if (xp >= 0 && xp != b.mirroredXp) {
				const int readBack = WriteAeXp(b.instance, xp);
				LOG("Progression", INFO, "AE block (controller {}): base_xp {} -> {} (reads back {}, level {}).",
					controller, b.mirroredXp, xp, readBack, g_Api.LevelForXp(readBack));
				b.mirroredXp = xp;
			}
		}
		return b.instance;
	}

	void SyncEngineAe(int controller, void* real, bool forceRefresh) {
		if (!g_AeAvailable || !real || controller < 0 || controller > 1) return;
		AeBlock& b = g_Ae[controller];
		const std::uint64_t now = GetTickCount64();
		if (!forceRefresh && b.engineInst.load() == real && now - b.engineRefreshMs.load() < 500) return;
		std::lock_guard lock(g_AeLock);
		if (!EnsureAeBlock(controller)) return;
		b.engineRefreshMs = now;

		const int held = static_cast<int>(g_Api.GetAeXp(real, 0xFFFFFFFFu));
		// The engine made it again (a sign-in, the ffotd): the XP written here is gone. XP never drops
		// to 0 any other way. Not the scramble key: every engine copy FROM the block renews that.
		const bool first = b.engineInst.load() != real;
		const bool again = !first && !b.engineForeign && b.engineHeld > 0 && held == 0;
		if (first || again) {
			b.engineInst = real;
			b.engineXp = -1;
			b.engineHeld = 0;
			b.engineForeign = held != 0;
			InstView v;
			const bool viewed = ViewInstance(real, v);
			if (b.engineForeign) {
				LOG("Progression", WARN, "AE block (controller {}): the engine's own block already holds {} XP "
					"that this mod did not write (a backend with an AE service). It is left alone: the local "
					"save is not copied into it and its level is the backend's.", controller, held);
			}
			else if (CopyIntoInstance(real, b.buffer)) {
				LOG("Progression", INFO, "AE block (controller {}): {} ({} in memory). The local save ({} non-zero "
					"bytes of gun XP, camo and challenge progress) is copied into it; the level follows below.",
					controller, first ? "a Demonware sign-in made the engine's own block, blank"
						: "the engine made its block again, blank", viewed && v.scrambled ? "scrambled" : "plain",
					NonZeroBytes(b.buffer, kAeSyncBufferSize));
			}
			else {
				LOG("Progression", ERROR, "AE block (controller {}): could not copy the local save into the engine's "
					"own block (instance {}, size {}, ae_sync root {}). Gun levels stay empty this session; the "
					"level still follows the saved rankxp.", controller, viewed ? "readable" : "UNREADABLE",
					viewed ? v.size : -1, viewed && IsAeInstance(v) ? "matches" : "DIFFERS");
			}
		}
		if (b.engineForeign) return;

		const int xp = SavedRankXp(controller);
		if (xp >= 0 && xp != b.engineXp) {
			const int readBack = WriteAeXp(real, xp);
			LOG("Progression", INFO, "AE block (controller {}): the engine's base_xp {} -> {} (reads back {}, "
				"level {}).", controller, b.engineXp, xp, readBack, g_Api.LevelForXp(readBack));
			b.engineXp = xp;
			b.engineHeld = readBack;
		}
	}

	void* AeBlockFor(int controller, bool forceRefresh) {
		if (void* real = EngineAeInstance(controller)) {
			SyncEngineAe(controller, real, forceRefresh);
			return real;
		}
		return LocalAeInstance(controller, forceRefresh);
	}

	void TickEngineAe() {
		if (!Enabled() || !g_AeAvailable) return;
		static std::uint64_t s_NextMs = 0;
		const std::uint64_t now = GetTickCount64();
		if (now < s_NextMs) return;
		s_NextMs = now + 250;
		for (int controller = 0; controller < 2; ++controller)
			if (void* real = EngineAeInstance(controller)) SyncEngineAe(controller, real);
	}

	void* LocalAeRootStateSlot(int controller) {
		if (!LocalAeInstance(controller)) return nullptr;
		return g_Ae[controller].rootState;
	}

	bool PushLocalAeToLua(void* luaState, int controller) {
		void* inst = LocalAeInstance(controller);
		if (!inst || !luaState) return false;
		DdlState state;
		state.Reset();
		g_Api.InitRootState(state.bytes, inst);
		if (!state.bytes[0]) return false;
		g_Api.PushStateToLua(luaState, state.bytes, inst);
		return true;
	}

	void PrepareAeSlots(int controller) {
		std::uint8_t* r = Record(controller);
		if (!g_AeAvailable || !r) return;
		std::lock_guard lock(g_AeLock);
		void* pre = r + kStatsXfer_AePrematchInst;
		void* cur = r + kStatsXfer_AeCurrentInst;
		// LIVE copies the Demonware block into both; never overwrite a real one.
		if (g_Api.InstanceValid(pre) || g_Api.InstanceValid(cur)) {
			LOG("Progression", INFO, "AE slots (controller {}): the record already has AE instances, left alone.",
				controller);
			return;
		}
		const int xp = ReadRankXp(r + 16);   // source-0 prematch copy = the save at connect
		const bool okPre = InitAeInstance(g_AePrematchBuffer[controller], pre);
		const bool okCur = InitAeInstance(g_AeCurrentBuffer[controller], cur);
		// Both start as the saved block, so the AAR's before/after also covers gun XP.
		if (okPre && okCur && LocalAeInstance(controller)) {
			std::memcpy(g_AePrematchBuffer[controller], g_Ae[controller].buffer, kAeSyncBufferSize);
			std::memcpy(g_AeCurrentBuffer[controller], g_Ae[controller].buffer, kAeSyncBufferSize);
		}
		int preXp = -1;
		if (okPre && okCur && xp >= 0) {
			preXp = WriteAeXp(pre, xp);
			WriteAeXp(cur, xp);
		}
		LOG("Progression", INFO, "AE slots (controller {}): prematch +176 {} / current +416 {}, prematch rankxp {} "
			"(AE reads {}). {}", controller, okPre ? "ok" : "FAILED", okCur ? "ok" : "FAILED", xp, preXp,
			okPre && okCur ? "The match-end recap can now hand the AAR its before/after XP."
				: "The AAR will not get its data this match (ae_sync DDL root not loaded?).");
		LocalAeInstance(controller, true);
	}

	void OnFinalCommit(int controller, int source) {
		std::uint8_t* r = Record(controller);
		if (!g_AeAvailable || !r || source < 0 || source > 2) return;
		std::uint8_t committed[3]{};
		for (std::size_t i = 0; i < 3; ++i) SafeRead(r + kStatsXfer_Committed + i, committed[i]);
		if (!committed[0] || !committed[1]) return;   // a playerdata source is still to come

		std::lock_guard lock(g_AeLock);
		AeBlock& b = g_Ae[controller];
		void* cur = r + kStatsXfer_AeCurrentInst;
		void* const real = EngineAeInstance(controller);
		const int svClient = g_SvAeClient[controller].exchange(-1);
		// This match's gun XP and camo progress go into the local block, which is what the file holds
		// (and in LAN the AAR's "after"). base_xp follows map 19, which now holds the match result.
		std::string gun;
		void* block = nullptr;
		bool save = false;
		if (!EnsureAeBlock(controller)) {
			gun = "no local AE block (ae_sync root not loaded?): gun XP / camo progress not captured";
		}
		else if (real && b.engineForeign) {
			block = real;
			gun = "the engine's AE block is filled by a backend: nothing copied or saved here";
		}
		else {
			const std::size_t before = NonZeroBytes(b.buffer, kAeSyncBufferSize);
			if (svClient >= 0) {
				// We backed the server's AE slot (no blob reached it): it holds the result.
				std::memcpy(b.buffer, g_SvAeBuffer[controller], kAeSyncBufferSize);
				b.mirroredXp = -1;
				void* curBuf = nullptr;
				if (SafeRead(cur, curBuf) && curBuf == g_AeCurrentBuffer[controller])
					std::memcpy(g_AeCurrentBuffer[controller], g_SvAeBuffer[controller], kAeSyncBufferSize);
				block = LocalAeInstance(controller, true);
				if (real && block) {
					// An engine block the server never imported: readers use it, so it gets the result too.
					CopyIntoInstance(real, b.buffer);
					b.engineXp = -1;
					SyncEngineAe(controller, real, true);
					block = real;
				}
				save = block != nullptr;
				gun = std::format("server client {}'s AE slot copied back", svClient);
			}
			else if (real) {
				// networkMode 2: the server imported the engine's block and its stat deltas were applied
				// to that block during the match. The new rankxp goes in first, so the file holds it too.
				SyncEngineAe(controller, real, true);
				block = real;
				save = CopyOutInstance(real, b.buffer);
				b.mirroredXp = -1;
				gun = save ? "the engine's AE block, which the server updated during the match, copied to the local block"
					: "*** could not read the engine's AE block ***: this match's gun XP / camo progress is not saved";
			}
			else {
				// The local stand-in itself was the block the server's deltas (if any) were applied to.
				block = LocalAeInstance(controller, true);
				save = block != nullptr;
				gun = "no server AE slot of ours this match: the local block is saved as it stands";
			}
			gun += std::format(" ({} -> {} non-zero bytes)", before, NonZeroBytes(b.buffer, kAeSyncBufferSize));
			if (save) {
				gun += SaveAeFile(controller, b)
					? std::format(", saved to {}", AeFilePath(controller).string()) : ", SAVE FAILED (see above)";
			}
		}
		LOG("Progression", INFO, "Gun XP (controller {}, after source {}): {}.", controller, source, gun);
		const int xp = SavedRankXp(controller);
		int curXp = -1;
		if (xp >= 0 && g_Api.InstanceValid(cur)) curXp = WriteAeXp(cur, xp);
		const int preXp = g_Api.InstanceValid(r + kStatsXfer_AePrematchInst)
			? static_cast<int>(g_Api.GetAeXp(r + kStatsXfer_AePrematchInst, 0xFFFFFFFFu)) : -1;

		// LIVE raises these from the AE service's reply; nothing will reply here.
		for (const std::uint64_t flag : { kAarFlag_ProgressionReady, kAarFlag_Second, kAarFlag_LootContracts })
			g_Api.SetAarFlag(controller, flag, 1);
		g_SuppressLuiFatals = true;

		LOG("Progression", INFO, "AAR (controller {}): both playerdata sources committed, source 2 {}. The recap "
			"runs {} with AE before {} / after {} (level {} -> {}), AE block {}, record +416 {}. LUI fatals are "
			"suppressed from here on.", controller, committed[2] ? "too" : "still to come",
			committed[2] ? "next" : "once it is in", preXp, xp, preXp >= 0 ? g_Api.LevelForXp(preXp) : -1,
			xp >= 0 ? g_Api.LevelForXp(xp) : -1, block ? "ok" : "MISSING", curXp >= 0 ? "ok" : "MISSING");
	}

	bool SuppressLuiFatals() { return g_SuppressLuiFatals.load(std::memory_order_relaxed); }

	int QueueBonusXp(int amount) {
		if (amount <= 0) return g_BonusXp.load();
		// Capped so repeated clicks cannot overflow the engine's u32 gain; the rank table clamps anyway.
		int cur = g_BonusXp.load();
		int pending = 0;
		do {
			pending = static_cast<int>((std::min)(static_cast<long long>(cur) + amount, 100'000'000LL));
		} while (!g_BonusXp.compare_exchange_weak(cur, pending));
		LOG("Progression", INFO, "Bonus XP: +{} queued from the overlay ({} pending). It is added to your next "
			"XP event in a Zombies match.", amount, pending);
		return pending;
	}

	int PendingBonusXp() { return g_BonusXp.load(std::memory_order_relaxed); }

	int TakeBonusXp(int clientNum) {
		return clientNum == 0 ? g_BonusXp.exchange(0) : 0;
	}

	XpStatus ReadXpStatus() {
		XpStatus s;
		s.available = Enabled() && g_AeAvailable;
		if (!s.available) return s;
		s.saved = SavedRankXp(0);
		s.savedLevel = s.saved >= 0 ? g_Api.LevelForXp(s.saved) : -1;
		if (ServerStatsSlotReady(0)) s.match = ReadServerRankXp(0);
		s.tainted = IsTainted(0);
		return s;
	}
}
