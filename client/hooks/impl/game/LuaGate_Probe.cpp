#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/dump_anchors.hpp"
#include "game/game_internal.hpp"

#include <utility/nt.hpp>
#include <MinHook.h>

#include <atomic>
#include <cstdlib>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Mode-tile lock probe (B6 padlocks). The 07:08 transcript put the lobby menu at director_lan, which
// the tile action accepts, so the padlock is one of the tile datasource's lock branches, and three of
// those turn on values only a running game has: the content natives, the mode-available dvars, and
// Korea-minor for ZM (see dump_anchors.hpp). Each detour runs the original, then logs what Lua asked
// and what it got back. A (native, argument) pair is logged the first time and again whenever the
// answer changes, so a click on a tile shows up as the lobby test being asked.

namespace {
	using namespace Client;

	constexpr int kMaxLines = 1500;

	std::mutex g_Lock;
	int g_Lines = 0;
	std::map<std::string, std::string> g_LastAnswer;

	void Emit(std::string key, const std::string& answer) {
		std::lock_guard<std::mutex> lock(g_Lock);
		if (g_Lines >= kMaxLines) {
			return;
		}
		auto it = g_LastAnswer.find(key);
		if (it != g_LastAnswer.end() && it->second == answer) {
			return;
		}
		const bool changed = it != g_LastAnswer.end();
		LOG("LuaGate", INFO, "{} -> {}{}", key, answer, changed ? std::format("  (was {})", it->second) : "");
		g_LastAnswer[std::move(key)] = answer;
		if (++g_Lines == kMaxLines) {
			LOG("LuaGate", WARN, "cap of {} lines reached; the tile gate probe stops here.", kMaxLines);
		}
	}

	void Record(const char* native, void* L) {
		std::string arg, answer;
		Game::Pointers::LuaArgText(L, 1, arg);
		if (!Game::Pointers::LuaResultText(L, answer)) {
			answer = "<nothing pushed>";
		}
		Emit(std::format("{}({})", native, arg), answer);
	}

	// GetDvarInt is asked for dozens of dvars every frame; only the four mode-available ones matter.
	const char* ModeDvarLabel(void* L) {
		std::string arg;
		if (!Game::Pointers::LuaArgText(L, 1, arg) || arg.size() < 2 || arg[0] != '#') {
			return nullptr;
		}
		const std::uint64_t h = std::strtoull(arg.c_str() + 1, nullptr, 16) & 0x0FFFFFFFFFFFFFFFULL;
		if (h == Game::kModeDvar_ZM) return "GetDvarInt[ZM available]";
		if (h == Game::kModeDvar_MP) return "GetDvarInt[MP available]";
		if (h == Game::kModeDvar_WZ) return "GetDvarInt[WZ available]";
		if (h == Game::kModeDvar_CP) return "GetDvarInt[CP available]";
		return nullptr;
	}
}

// Why AreLocalFilesReady said false. PlayerData_AreLocalFilesReady re-evaluated from memory in the
// engine's order (dump_anchors.hpp has its shape), naming every (map, version) that fails and which of
// IsBufferReady's causes it is. It runs on the Lua thread right after the native answered, so both
// read the same state. If the reconstruction passes while the engine said false, it says so: the
// model is wrong, not the data.
namespace {
	using P = Client::Game::Pointers;
	using Client::Game::SafeRead;

	std::uintptr_t g_ModuleBase = 0;
	std::uintptr_t g_ModuleSize = 0;

	const std::uint8_t* Rt(std::uintptr_t dump) {
		const std::uintptr_t a = g_ModuleBase + (dump - Client::Game::kDumpImagebase);
		return g_ModuleBase && a >= g_ModuleBase && a < g_ModuleBase + g_ModuleSize
			? reinterpret_cast<const std::uint8_t*>(a) : nullptr;
	}

	template <typename T>
	T ReadOr(const void* p, T fallback) {
		T v{};
		return SafeRead(p, v) ? v : fallback;
	}

	const std::uint8_t* Def(int id) {
		void** const defs = Client::g_Pointers ? Client::g_Pointers->m_g_playerDataDefsById : nullptr;
		if (!defs || id < 1 || id > Client::Game::kPdMaxDataMapId) return nullptr;
		return ReadOr<const std::uint8_t*>(defs + id, nullptr);
	}

	// PlayerData_IsDefInScopeForController: scope 1 admits controller 0, 2 and 3 admit 0..1, anything
	// else admits nobody.
	int ScopeMax(const std::uint8_t* def) {
		const int scope = ReadOr<int>(def + P::kPdDef_Scope, 0);
		return scope == 1 ? 0 : (scope == 2 || scope == 3) ? 1 : -1;
	}

	bool InScope(const std::uint8_t* def, int ctrl) {
		return def && ctrl != -1 && ctrl <= ScopeMax(def);
	}

	std::uint64_t UserXuid(int ctrl) {
		std::uintptr_t* const users = Client::g_Pointers ? Client::g_Pointers->m_g_liveUserObjects : nullptr;
		if (!users || ctrl < 0) return 0;
		const auto obj = ReadOr<std::uintptr_t>(users + ctrl, 0);
		return obj ? ReadOr<std::uint64_t>(reinterpret_cast<const void*>(obj + Client::Game::kLiveUser_Xuid), 0) : 0;
	}

	// PlayerData_IsBufferReady's causes, as text. Empty means ready.
	std::string BufferCause(int ctrl, int id, int ver) {
		if (ctrl < 0) return "no controller";
		if (!ReadOr<std::uint8_t>(Rt(Client::Game::kDump_g_playerDataInitialized), 0)) return "store not initialised";
		const std::uint8_t* const def = Def(id);
		if (ver == -1) ver = def ? ReadOr<int>(def + P::kPdDef_DefaultVer, 0) : 0;
		const std::uint8_t* const store = Client::g_Pointers ? Client::g_Pointers->m_g_playerDataStore : nullptr;
		if (!store) return "store anchor unresolved";
		const std::uint8_t* const block = store + P::kPdStoreStride * static_cast<std::size_t>(ctrl);
		const int count = ReadOr<int>(block + P::kPdStoreCountOff, 0);
		if (count <= 0) return "store block empty";

		const std::uint8_t* entry = nullptr;
		for (int k = 0; k < count && k < static_cast<int>(P::kPdEntryCapacity); ++k) {
			const std::uint8_t* const e = block + P::kPdEntryBias + P::kPdEntryStride * k;
			if (ReadOr<int>(e + P::kPdEntry_DataMapId, -1) == id && ReadOr<int>(e + P::kPdEntry_Version, -1) == ver) {
				entry = e;
				break;
			}
		}
		if (!entry) return std::format("no entry for v{}", ver);

		const auto* const edef = ReadOr<const std::uint8_t*>(entry + P::kPdEntry_Def, nullptr);
		if (!edef || ctrl > ScopeMax(edef)) {
			return std::format("entry scope {} excludes controller {}", edef ? ReadOr<int>(edef + P::kPdDef_Scope, 0) : -1, ctrl);
		}
		const int loaded = ReadOr<int>(entry + P::kPdEntry_Loaded, -1);
		if (loaded != 1) return std::format("entry+120 = {} (not loaded)", loaded);
		const auto storeXuid = ReadOr<std::uint64_t>(block, 0);
		const auto userXuid = UserXuid(ctrl);
		if (storeXuid != userXuid) return std::format("store XUID {:X} != user XUID {:X}", storeXuid, userXuid);
		return {};
	}

	std::string ExplainLocalFiles(int ctrl) {
		std::vector<std::string> fails;
		auto check = [&](int c, int id, int ver, std::string_view what) {
			std::string cause = BufferCause(c, id, ver);
			if (!cause.empty()) fails.push_back(std::format("{}: {}", what, cause));
		};

		// 1. LiveUser_GetActiveFlag5760.
		std::uintptr_t* const users = Client::g_Pointers ? Client::g_Pointers->m_g_liveUserObjects : nullptr;
		const auto user = users && ctrl >= 0 ? ReadOr<std::uintptr_t>(users + ctrl, 0) : 0;
		if (!user || !ReadOr<std::uint8_t>(reinterpret_cast<const void*>(user + Client::Game::kLiveUser_ActiveFlag5760), 0)) {
			fails.push_back("LiveUser+5760 active flag is 0");
		}

		// 2. LiveStorage_AreOnlineDataMapsReady(ctrl, 1) is true for any mode but 2; nothing to test.

		// 3. PlayerData_AreDataMapGroupReady(ctrl, 1): a row with no def fails outright; otherwise
		//    only mode-1 rows are tested.
		if (const std::uint8_t* const groups = Rt(Client::Game::kDump_g_playerDataMapGroups)) {
			for (int g = 0; g < Client::Game::kPdMapGroupCount; ++g) {
				const std::uint8_t* const row = groups + Client::Game::kPdMapGroupStride * g;
				const int id = ReadOr<int>(row, 0);
				const int mode = ReadOr<int>(row + 4, 0);
				const std::uint8_t* const def = Def(id);
				if (!def) {
					fails.push_back(std::format("group map {} has no def", id));
					continue;
				}
				if (mode != 1 || !InScope(def, ctrl) || ReadOr<std::uint8_t>(def + Client::Game::kPdDef_SkipLocalCheck, 0)) continue;
				const int n = ReadOr<int>(def + Client::Game::kPdDef_VersionCount, 0);
				for (int v = 0; v < n; ++v) check(ctrl, id, v, std::format("group map {} v{}", id, v));
			}
		}

		// 4. PlayerData_IsPrimaryMap23Ready: map 23 for the primary local client's controller.
		const int idx = ReadOr<int>(Rt(Client::Game::kDump_g_primaryLocalClient), -1);
		int primary = -1;
		if (const std::uint8_t* const table = Rt(Client::Game::kDump_g_localClientControllers); table && idx >= 0 && idx < 8) {
			if (ReadOr<int>(table + 60 * idx + 4, -2) == idx) primary = ReadOr<int>(table + 60 * idx + 8, -1);
		}
		check(primary, Client::Game::kPdPrimaryCheckMapId, -1,
			std::format("primary map {} (local client {} -> controller {})", Client::Game::kPdPrimaryCheckMapId, idx, primary));

		// 5. Every version of every non-dwuser map in scope. The engine stops at the first failure;
		//    this lists all of them.
		for (int id = 1; id <= Client::Game::kPdMaxDataMapId; ++id) {
			const std::uint8_t* const def = Def(id);
			const int loc = def ? ReadOr<int>(def + P::kPdDef_StorageLoc, 5) : 5;
			if (loc == P::kPdLoc_DwUser || !InScope(def, ctrl)) continue;
			if (ReadOr<std::uint8_t>(def + Client::Game::kPdDef_SkipLocalCheck, 0)) continue;
			const int n = ReadOr<int>(def + Client::Game::kPdDef_VersionCount, 0);
			const bool redirected = Client::Game::PlayerDataRedirect::IsRedirected(static_cast<unsigned int>(id));
			for (int v = 0; v < n; ++v) {
				check(ctrl, id, v, std::format("map {} loc {}{} v{}", id, loc, redirected ? " (our dwuser->hdd)" : "", v));
			}
		}

		if (fails.empty()) {
			return "reconstruction passes every check, so it DISAGREES with the engine's false: the model in "
				"dump_anchors.hpp is wrong somewhere, not the data";
		}
		constexpr std::size_t kShown = 16;
		std::string out = std::format("{} failing check(s):", fails.size());
		for (std::size_t i = 0; i < fails.size() && i < kShown; ++i) out += "\n    " + fails[i];
		if (fails.size() > kShown) out += std::format("\n    ... and {} more", fails.size() - kShown);
		return out;
	}

	// Once: every def the loops look at, so the fix can be chosen without another boot.
	void LogDefTableOnce(int ctrl) {
		static std::atomic<bool> s_Done{ false };
		if (s_Done.exchange(true)) return;
		std::string out;
		for (int id = 1; id <= Client::Game::kPdMaxDataMapId; ++id) {
			const std::uint8_t* const def = Def(id);
			if (!def) continue;
			const int n = ReadOr<int>(def + Client::Game::kPdDef_VersionCount, 0);
			int ready = 0;
			for (int v = 0; v < n; ++v) ready += BufferCause(ctrl, id, v).empty() ? 1 : 0;
			out += std::format("\n    map {:2}: loc {} scope {} skip {} versions {} default v{} ready {}/{}{}", id,
				ReadOr<int>(def + P::kPdDef_StorageLoc, -1), ReadOr<int>(def + P::kPdDef_Scope, -1),
				ReadOr<std::uint8_t>(def + Client::Game::kPdDef_SkipLocalCheck, 0), n,
				ReadOr<int>(def + P::kPdDef_DefaultVer, -1), ready, n,
				Client::Game::PlayerDataRedirect::IsRedirected(static_cast<unsigned int>(id)) ? " (our dwuser->hdd)" : "");
		}
		LOG("LuaGate", INFO, "player-data defs as AreLocalFilesReady sees them (controller {}; loc 1 = dwuser, skipped):{}", ctrl, out);
	}

	void ExplainAreLocalFilesReady(void* L) {
		std::string answer;
		if (!Client::Game::Pointers::LuaResultText(L, answer) || answer != "false") return;
		std::string arg;
		Client::Game::Pointers::LuaArgText(L, 1, arg);
		const int ctrl = std::atoi(arg.c_str());
		LogDefTableOnce(ctrl);
		Emit(std::format("AreLocalFilesReady({}) is false because", arg), ExplainLocalFiles(ctrl));
	}
}

// The start gates: the natives Lobby.ProcessNavigate.BeginLivePlay (and the title screen that loops
// PressStart into it) tests before it will navigate to the online menu and create the party. Every
// early return there is silent, so these answers are the only way to name the one that fires.
// Same Record as above; one indexed detour per entry instead of a named HookPlate type each.
namespace {
	struct StartGate {
		const char* label;
		std::uintptr_t dump;
	};

	constexpr StartGate kStartGates[] = {
		{ "ConnectionInfo",               Client::Game::kDump_LuaNative_ConnectionInfo },
		{ "ConnErrorGate[0x545A16E7]",    Client::Game::kDump_LuaNative_ConnErrorGate },
		{ "GetPlayerQueueInfo",           Client::Game::kDump_LuaNative_GetPlayerQueueInfo },
		{ "QueueGate[0x0F62AEC4]",        Client::Game::kDump_LuaNative_QueueGate },
		{ "IsPlayerQueued",               Client::Game::kDump_LuaNative_IsPlayerQueued },
		{ "UniversalAccountA[0x7CE050EC]", Client::Game::kDump_LuaNative_UniversalAccountA },
		{ "UniversalAccountB[0x21E94361]", Client::Game::kDump_LuaNative_UniversalAccountB },
		{ "AreLocalFilesReady",           Client::Game::kDump_LuaNative_AreLocalFilesReady },
		{ "ForceOfflineGate[0x3573048F]", Client::Game::kDump_LuaNative_ForceOfflineGate },
		{ "IsSignedInToLive",             Client::Game::kDump_LuaNative_IsSignedInToLive },
		{ "FirstParty.HasError",          Client::Game::kDump_LuaNative_FirstPartyHasError },
		{ "FirstParty.Disconnected",      Client::Game::kDump_LuaNative_FirstPartyDisconnected },
		{ "GetLobbyNav",                  Client::Game::kDump_LuaNative_GetLobbyNav },
	};
	constexpr std::size_t kStartGateCount = sizeof(kStartGates) / sizeof(kStartGates[0]);

	constexpr std::size_t StartGateIndex(std::uintptr_t dump) {
		for (std::size_t i = 0; i < kStartGateCount; ++i) {
			if (kStartGates[i].dump == dump) return i;
		}
		return kStartGateCount;
	}

	template <std::size_t I>
	struct StartGateHook {
		static inline Client::Game::Functions::LuaNativeT* m_Original{};
		static std::int32_t hkCallback(void* L) {
			const std::int32_t ret = m_Original(L);
			Record(kStartGates[I].label, L);
			if constexpr (I == StartGateIndex(Client::Game::kDump_LuaNative_AreLocalFilesReady)) {
				ExplainAreLocalFilesReady(L);
			}
			return ret;
		}
	};

	template <std::size_t... I>
	int InstallStartGates(std::uintptr_t moduleBase, std::uintptr_t moduleSize, std::index_sequence<I...>) {
		int armed = 0;
		auto one = [&](std::uintptr_t dump, void* callback, void** original) {
			const std::uintptr_t a = moduleBase + (dump - Client::Game::kDumpImagebase);
			if (a < moduleBase || a >= moduleBase + moduleSize) return;
			void* const target = reinterpret_cast<void*>(a);
			if (MH_CreateHook(target, callback, original) == MH_OK && MH_EnableHook(target) == MH_OK) {
				++armed;
			}
		};
		(one(kStartGates[I].dump, reinterpret_cast<void*>(&StartGateHook<I>::hkCallback),
			reinterpret_cast<void**>(&StartGateHook<I>::m_Original)), ...);
		return armed;
	}
}

int Client::Hook::InstallStartGateProbe() {
	const Common::Utility::NT::Library game;
	const auto base = reinterpret_cast<std::uintptr_t>(game.GetPtr());
	const std::uintptr_t size = game.GetOptionalHeader()->SizeOfImage;
	g_ModuleBase = base;
	g_ModuleSize = size;
	const int armed = InstallStartGates(base, size, std::make_index_sequence<kStartGateCount>{});
	if (armed == static_cast<int>(kStartGateCount)) {
		LOG("Hooks", INFO, "Start-gate probe armed ({} BeginLivePlay/title-screen natives -> (LuaGate) lines).", armed);
	}
	else {
		LOG("Hooks", ERROR, "Start-gate probe only {}/{} detours live; missing (LuaGate) lines prove nothing.",
			armed, kStartGateCount);
	}
	return armed;
}

template <>
std::int32_t Client::Hook::Hooks::HK_LuaNative_ContentIsFullyInstalled::hkCallback(void* L) {
	const std::int32_t ret = m_Original(L);
	Record("Content.IsFullyInstalled", L);
	return ret;
}

template <>
std::int32_t Client::Hook::Hooks::HK_LuaNative_ContentIsPlayable::hkCallback(void* L) {
	const std::int32_t ret = m_Original(L);
	Record("Content.IsPlayable", L);
	return ret;
}

template <>
std::int32_t Client::Hook::Hooks::HK_LuaNative_ContentIsInstalling::hkCallback(void* L) {
	const std::int32_t ret = m_Original(L);
	Record("Content.IsInstalling", L);
	return ret;
}

template <>
std::int32_t Client::Hook::Hooks::HK_LuaNative_GetDvarInt::hkCallback(void* L) {
	const std::int32_t ret = m_Original(L);
	if (const char* label = ModeDvarLabel(L)) {
		Record(label, L);
	}
	return ret;
}

template <>
std::int32_t Client::Hook::Hooks::HK_LuaNative_IsKoreaMinor::hkCallback(void* L) {
	const std::int32_t ret = m_Original(L);
	Record("IsKoreaMinor", L);
	return ret;
}

template <>
std::int32_t Client::Hook::Hooks::HK_LuaNative_IsLobbySlotLive::hkCallback(void* L) {
	const std::int32_t ret = m_Original(L);
	Record("IsLobbySlotLive", L);
	return ret;
}
