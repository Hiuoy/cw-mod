#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/arxan_call.hpp"
#include "game/dump_anchors.hpp"
#include "game/zm_progression.hpp"

#include <atomic>
#include <intrin.h>

// The detours behind ZM progression. The reasoning is in game/zm_progression.hpp;
// the addresses and what each engine function checks are in game/dump_anchors.hpp.
//
// Every original is re-entered through an ArxanCall thunk. Several of these may carry the Arxan
// caller check, and a detour's plain m_Original call returns into our module, which that check
// answers by silently skipping the body (see memory: hooking Arxan-guarded functions). The thunk
// costs nothing on functions without the check.

namespace {
	namespace Zm = Client::Game::ZmProgression;

	template <typename Fn>
	Fn* Thunked(Fn* trampoline, const char* name) {
		auto* const thunk = Client::Game::ArxanCall::MakeThunk<Fn>(reinterpret_cast<void*>(trampoline));
		if (!thunk) {
			LOG("Progression", ERROR, "{}: no Arxan thunk for the trampoline; calling it directly, which a "
				"caller-guarded function would silently skip.", name);
		}
		return thunk ? thunk : trampoline;
	}

	std::atomic<int> g_XpEvents{ 0 };
	std::atomic<long long> g_XpAwarded{ 0 };
	std::atomic<int> g_MidMatchCommitLogs{ 0 };
	constexpr int kMaxMidMatchCommitLogs = 20;

	using H = Client::Hook::Hooks;
	// Built once, at install time, on the installing thread. MakeThunk briefly re-protects the whole
	// thunk arena, so building one lazily from a gameplay thread could fault another thread that is
	// executing an existing thunk at that instant.
	decltype(H::HK_LiveStorage_AreMatchStatsEnabled::m_Original) g_AreMatchStatsEnabled{};
	decltype(H::HK_GScr_AreStatWritesEnabled::m_Original) g_AreStatWritesEnabled{};
	decltype(H::HK_LiveStorage_BeginStatsTransfer::m_Original) g_BeginStatsTransfer{};
	decltype(H::HK_LiveStorage_CommitStatsTransfer::m_Original) g_CommitStatsTransfer{};
	decltype(H::HK_G_AddPlayerRankXp::m_Original) g_AddPlayerRankXp{};
	decltype(H::HK_LiveStats_GetLiveUserStatsInstance::m_Original) g_GetLiveUserStatsInstance{};
	decltype(H::HK_LiveStats_GetAeRootStateSlot::m_Original) g_GetAeRootStateSlot{};
	decltype(H::HK_Lua_PushAeSyncBuffer::m_Original) g_PushAeSyncBuffer{};
	decltype(H::HK_SV_ClientStatsReady::m_Original) g_ClientStatsReady{};
	decltype(H::HK_LobbyRoot_SetNetworkModeModel::m_Original) g_SetNetworkModeModel{};
	decltype(H::HK_Lua_GameModeIsMode_Impl::m_Original) g_GameModeIsMode{};
	decltype(H::HK_Lua_Engine_GetLobbyNetworkMode::m_Original) g_GetLobbyNetworkMode{};

	// (g_sessionModePacked as int16) >> 12 is -8..7, so the original pushes false for this.
	constexpr int kNeverAGameMode = 0x7FFF;
}

void Client::Game::ZmProgression::BindHookThunks() {
	g_AreMatchStatsEnabled = Thunked(H::HK_LiveStorage_AreMatchStatsEnabled::m_Original, "AreMatchStatsEnabled");
	g_AreStatWritesEnabled = Thunked(H::HK_GScr_AreStatWritesEnabled::m_Original, "GScr_AreStatWritesEnabled");
	g_BeginStatsTransfer = Thunked(H::HK_LiveStorage_BeginStatsTransfer::m_Original, "BeginStatsTransfer");
	g_CommitStatsTransfer = Thunked(H::HK_LiveStorage_CommitStatsTransfer::m_Original, "CommitStatsTransfer");
	g_AddPlayerRankXp = Thunked(H::HK_G_AddPlayerRankXp::m_Original, "G_AddPlayerRankXp");
	g_GetLiveUserStatsInstance = Thunked(H::HK_LiveStats_GetLiveUserStatsInstance::m_Original,
		"LiveStats_GetLiveUserStatsInstance");
	g_GetAeRootStateSlot = Thunked(H::HK_LiveStats_GetAeRootStateSlot::m_Original, "LiveStats_GetAeRootStateSlot");
	g_PushAeSyncBuffer = Thunked(H::HK_Lua_PushAeSyncBuffer::m_Original, "Lua_PushAeSyncBuffer");
	g_ClientStatsReady = Thunked(H::HK_SV_ClientStatsReady::m_Original, "SV_ClientStatsReady");
	g_SetNetworkModeModel = Thunked(H::HK_LobbyRoot_SetNetworkModeModel::m_Original,
		"LobbyRoot_SetNetworkModeModel");
	// Installed only with LIVE menus on.
	if (H::HK_Lua_GameModeIsMode_Impl::m_Original) {
		g_GameModeIsMode = Thunked(H::HK_Lua_GameModeIsMode_Impl::m_Original, "Lua_GameModeIsMode_Impl");
	}
	if (H::HK_Lua_Engine_GetLobbyNetworkMode::m_Original) {
		g_GetLobbyNetworkMode = Thunked(H::HK_Lua_Engine_GetLobbyNetworkMode::m_Original,
			"Lua_Engine_GetLobbyNetworkMode");
	}
}

// Gun XP / camo challenges: back the server's AE slot for a local player before the engine reads it
// (the XP seed and the per-weapon unlock tables come from that slot).
template <>
char Client::Hook::Hooks::HK_SV_ClientStatsReady::hkCallback(std::uint8_t* svClient) {
	auto* const original = g_ClientStatsReady ? g_ClientStatsReady : m_Original;
	Zm::OnServerClientStatsReady(svClient);
	return original(svClient);
}

void Client::Game::ZmProgression::ApplyMenuNetworkMode(int mode) {
	auto* const original = g_SetNetworkModeModel ? g_SetNetworkModeModel
		: H::HK_LobbyRoot_SetNetworkModeModel::m_Original;
	if (original) original(MenuNetworkMode(mode));
}

// The menus' copy of the lobby network mode. Only the model changes; the engine stays LAN.
template <>
std::uint64_t Client::Hook::Hooks::HK_LobbyRoot_SetNetworkModeModel::hkCallback(int mode) {
	auto* const original = g_SetNetworkModeModel ? g_SetNetworkModeModel : m_Original;
	return original(Zm::MenuNetworkMode(mode));
}

// Engine.GameModeIsMode. A hidden custom game is answered through the engine's own push, by asking
// about a mode no session can be in.
template <>
std::uint64_t Client::Hook::Hooks::HK_Lua_GameModeIsMode_Impl::hkCallback(void* luaState, int mode) {
	auto* const original = g_GameModeIsMode ? g_GameModeIsMode : m_Original;
	return original(luaState, Zm::HideCustomGameFromCaller(luaState, mode) ? kNeverAGameMode : mode);
}

// Engine.GetLobbyNetworkMode. Pass-through: the real answer, and who asked goes to the log.
template <>
std::uint64_t Client::Hook::Hooks::HK_Lua_Engine_GetLobbyNetworkMode::hkCallback(void* luaState) {
	auto* const original = g_GetLobbyNetworkMode ? g_GetLobbyNetworkMode : m_Original;
	const std::uint64_t pushed = original(luaState);
	Zm::NoteNetworkModeQuery(luaState);
	return pushed;
}

void* Client::Game::ZmProgression::EngineAeInstance(int controller) {
	auto* const original = g_GetLiveUserStatsInstance ? g_GetLiveUserStatsInstance
		: H::HK_LiveStats_GetLiveUserStatsInstance::m_Original;
	return original ? original(controller) : nullptr;
}

// The AE block. The engine's own is the one readers get whenever it exists (a Demonware sign-in made
// it), filled from the local save first; only a null gets the local stand-in. See dump_anchors.hpp
// for why every level/AAR reader needs it.
template <>
void* Client::Hook::Hooks::HK_LiveStats_GetLiveUserStatsInstance::hkCallback(int controller) {
	return Zm::AeBlockFor(controller);
}

template <>
void* Client::Hook::Hooks::HK_LiveStats_GetAeRootStateSlot::hkCallback(int controller) {
	auto* const original = g_GetAeRootStateSlot ? g_GetAeRootStateSlot : m_Original;
	if (void* engine = Zm::EngineAeInstance(controller)) Zm::SyncEngineAe(controller, engine);
	if (void* real = original(controller)) return real;
	return Zm::LocalAeRootStateSlot(controller);
}

// Engine.GetAESyncBuffer: its body reads LiveUser+40024 inline, so it has to be answered here too.
template <>
std::uint64_t Client::Hook::Hooks::HK_Lua_PushAeSyncBuffer::hkCallback(void* luaState, int controller) {
	auto* const original = g_PushAeSyncBuffer ? g_PushAeSyncBuffer : m_Original;
	if (void* engine = Zm::EngineAeInstance(controller)) Zm::SyncEngineAe(controller, engine);
	else if (Zm::PushLocalAeToLua(luaState, controller)) return 1;
	return original(luaState, controller);
}

template <>
bool Client::Hook::Hooks::HK_LiveStorage_AreMatchStatsEnabled::hkCallback(
	std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) {

	auto* const original = g_AreMatchStatsEnabled ? g_AreMatchStatsEnabled : m_Original;
	if (!Zm::ShouldForceGate()) return original(a, b, c, d);
	if (!original(a, b, c, d))
		Zm::NoteForcedSite("AreMatchStatsEnabled", reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
	else
		Zm::NoteEngineGate("AreMatchStatsEnabled");
	return true;
}

template <>
bool Client::Hook::Hooks::HK_GScr_AreStatWritesEnabled::hkCallback(
	std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) {

	auto* const original = g_AreStatWritesEnabled ? g_AreStatWritesEnabled : m_Original;
	if (!Zm::ShouldForceGate()) return original(a, b, c, d);
	if (!original(a, b, c, d))
		Zm::NoteForcedSite("GScr_AreStatWritesEnabled", reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
	else
		Zm::NoteEngineGate("GScr_AreStatWritesEnabled");
	return true;
}

template <>
char Client::Hook::Hooks::HK_LiveStorage_BeginStatsTransfer::hkCallback(int controller) {
	auto* const original = g_BeginStatsTransfer ? g_BeginStatsTransfer : m_Original;
	if (!Zm::Enabled()) return original(controller);

	// Tainted until proven otherwise: only a Begin that copied the real save into the prematch slot
	// may be committed. Anything else leaves a blank slot, and a forced commit of a blank-based result
	// would overwrite the save. This includes a Begin that runs before the session says Zombies.
	Zm::SetTainted(controller, true);
	if (!Zm::ShouldForceGate()) {
		const char r = original(controller);
		LOG("Progression", INFO, "BeginStatsTransfer(ctrl {}) outside a Zombies session: engine answer, "
			"returned {}; nothing of ours runs for this match.", controller, static_cast<int>(r));
		return r;
	}

	Zm::OnMatchStart();
	bool safe = false;
	const std::string prep = Zm::PrepareBegin(controller, safe);

	if (safe) {
		const char r = original(controller);
		if (r) {
			Zm::SetTainted(controller, false);
			LOG("Progression", INFO, "BeginStatsTransfer: prematch slot COPIED from the save.\n    {}\n    {}",
				prep, Zm::DescribeRecord(controller));
			Zm::PrepareAeSlots(controller);
			return r;
		}
		LOG("Progression", ERROR, "BeginStatsTransfer: the forced prematch copy FAILED (the engine would "
			"drop this connect). Rerunning it unforced so the match loads; this controller's commit will "
			"be skipped so the save is not overwritten.\n    {}", prep);
	}
	else {
		LOG("Progression", WARN, "BeginStatsTransfer: not forcing the prematch copy.\n    {}", prep);
	}

	char r = 0;
	{
		Zm::SuppressScope unforced;
		r = original(controller);
	}
	// Where the engine's own gate is on, "unforced" still copies. The slot says which it was.
	if (r && Zm::PrematchHoldsSave(controller)) {
		Zm::SetTainted(controller, false);
		LOG("Progression", INFO, "BeginStatsTransfer: unforced run returned {} and the prematch slot holds the "
			"save (the engine's own gate is on): it will be committed.\n    {}", static_cast<int>(r),
			Zm::DescribeRecord(controller));
		Zm::PrepareAeSlots(controller);
		return r;
	}
	LOG("Progression", WARN, "BeginStatsTransfer: unforced run returned {} (blank prematch slot; XP this "
		"match will show in the log but will NOT be saved).\n    {}", static_cast<int>(r),
		Zm::DescribeRecord(controller));
	return r;
}

template <>
std::uint64_t Client::Hook::Hooks::HK_LiveStorage_CommitStatsTransfer::hkCallback(
	int controller, int source, unsigned int checksum, char final) {

	auto* const original = g_CommitStatsTransfer ? g_CommitStatsTransfer : m_Original;
	if (!Zm::ShouldForceGate()) return original(controller, source, checksum, final);

	if (Zm::IsTainted(controller)) {
		// Mark it committed the way the engine would, so CommitStatsTransferAndRecap still runs the
		// recap once every source is in; only the whole-buffer copy over the save is withheld.
		if (final) Zm::MarkCommitted(controller, source);
		LOG("Progression", WARN, "CommitStatsTransfer(ctrl {}, source {}, final {}) SKIPPED: this match "
			"started from a blank prematch slot, so its result would overwrite the save.", controller,
			source, static_cast<int>(final));
		return 0;
	}

	const std::string prep = Zm::PrepareCommit(controller, source);
	const std::uint64_t r = original(controller, source, checksum, final);
	// Every final commit, and the first mid-match ones (the server may send those on a timer).
	if (final || g_MidMatchCommitLogs.fetch_add(1) < kMaxMidMatchCommitLogs) {
		LOG("Progression", INFO, "CommitStatsTransfer(ctrl {}, source {}, checksum 0x{:X}, final {}):\n    {}\n"
			"    after: {}\n    A queued write shows up below as 'storage WRITE ... storage=hdd(0)' once the "
			"frontend is back.", controller, source, checksum, static_cast<int>(final), prep,
			Zm::DescribeRecord(controller));
	}
	// CommitStatsTransferAndRecap runs the recap right after this returns, once every source is in.
	if (final) Zm::OnFinalCommit(controller, source);
	return r;
}

template <>
std::uint64_t Client::Hook::Hooks::HK_G_AddPlayerRankXp::hkCallback(
	std::int16_t clientNum, std::int64_t xp, std::int64_t xpType, std::uint64_t eventHash) {

	auto* const original = g_AddPlayerRankXp ? g_AddPlayerRankXp : m_Original;
	int bonus = 0;
	if (Zm::ShouldForceGate()) {
		Zm::SeedServerRankXp(clientNum);
		// Overlay bonus XP rides this event, so it gets the same multiplier/clamp/level-up path.
		if (Zm::ServerStatsSlotReady(clientNum)) bonus = Zm::TakeBonusXp(clientNum);
	}
	const int before = Zm::ReadServerRankXp(clientNum);
	const std::uint64_t r = original(clientNum, xp + bonus, xpType, eventHash);
	const int after = Zm::ReadServerRankXp(clientNum);
	if (bonus > 0) {
		LOG("Progression", INFO, "Bonus XP: +{} added to client {}'s event (xp {} type {}) -> server rankxp {} -> "
			"{}. Saved at match end.", bonus, clientNum, xp, xpType, before, after);
	}

	const int n = g_XpEvents.fetch_add(1) + 1;
	if (after > before && before >= 0) g_XpAwarded += after - before;
	if (Zm::ShouldLogXpEvent() || n % 50 == 0) {
		LOG("Progression", INFO, "XP #{}: client {} xp {} type {} event 0x{:X} -> server rankxp {} -> {} "
			"(slot ready {}, gate forced {}; {} awarded this boot)", n, clientNum, xp, xpType, eventHash,
			before, after, Zm::ServerStatsSlotReady(clientNum), Zm::ShouldForceGate(), g_XpAwarded.load());
	}
	return r;
}
