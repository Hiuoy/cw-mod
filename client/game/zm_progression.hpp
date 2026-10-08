#pragma once
// ZM progression outside LIVE: XP, weapon levels, crystals and upgrade tiers in LAN and offline
// matches, saved to local .cgp files.
//
// Why it is off in the first place (IDB-verified 2026-09-23, see dump_anchors.hpp):
//   1. LiveStorage_AreMatchStatsEnabled and its GSC sibling are false unless networkMode == 2, so
//      G_AddPlayerRankXp adds nothing, not even in memory, and the commit returns early.
//   2. The progression maps (5 common, 18 zm_stats, 19 zm_progression) live in dwuser storage,
//      which never loads without Demonware. The dwuser -> hdd redirect (PlayerData_ControllerStorageTick)
//      makes them local; it is extended to offline/LAN boots while this feature is on.
//   3. The commit refuses a buffer whose player_xuid is not the local XUID, and a DDL-default buffer
//      has 0. The engine's own stamp (sub_7FF72531A030) only runs on a DW sign-in, so we stamp it.
//
// What the engine then does by itself: copy the player's playerdata into the prematch transfer slot,
// hand it to the server, add XP during the match, send the final stats back, copy them into playerdata
// and queue the write. Nothing here computes a stat.
//
// Safety: the commit copies the WHOLE buffer. If the prematch copy ever fails, the match would start
// from DDL defaults and the commit would overwrite the real save with them. So a failed copy retries
// with the gate unforced (the match still loads) and marks that controller tainted: its commit is
// skipped for the match. The player folder is also backed up once per boot.
//
// Online boots (local backend), since 2026-10-07. The same detours run there, for what the backend
// does not do:
//   * The level and gun XP live in the ae_sync block, which only Demonware's AE service fills and
//     keeps. The sign-in creates it blank, so every boot started at level 1. It is filled from the same
//     local file the LAN stand-in uses (player/cwmod_ae_sync_<ctrl>.bin) and saved back at match end.
//   * The commit waits for a stats signature no backend sends, and for player_xuid to match.
//   * The two gates are forced there too; a no-op when the engine already answers true.
//
// Opt-out: "progression": false in cw-mod.json.

#include <cstdint>
#include <string>

namespace Client::Game::ZmProgression {
	// Resolves the engine calls (Arxan thunks), reads the opt-out marker, backs up the player folder.
	// Call once, after ArxanCall is initialised and before the hooks go in. Logs its decision.
	void Init(std::uintptr_t moduleBase, std::size_t imageSize);

	// On for this boot: "progression" not false, every engine call resolved.
	bool Enabled();

	// Build the Arxan thunks the five detours call their originals through. Call once, right after
	// the detours are installed (so m_Original is set) and before the game can call them.
	void BindHookThunks();

	// Enabled, the session is Zombies, and no fallback scope is suppressing it.
	// Asked by the two gate detours on every call, from the main and server threads.
	bool ShouldForceGate();

	// Record a gate call site whose answer we changed. Logged once per (gate, site).
	void NoteForcedSite(const char* gate, std::uintptr_t returnAddress);
	// The engine answered true by itself in a Zombies session. Logged once per (gate, session word),
	// so an online boot shows whether the gates needed forcing at all.
	void NoteEngineGate(const char* gate);

	// While alive on this thread, ShouldForceGate() is false: the gates answer as the engine would.
	struct SuppressScope {
		SuppressScope();
		~SuppressScope();
		SuppressScope(const SuppressScope&) = delete;
		SuppressScope& operator=(const SuppressScope&) = delete;
	};

	// --- BeginStatsTransfer ---------------------------------------------------------------------
	// Stamps player_xuid on the two source maps and describes what the prematch copy will read.
	// safeToForce is false when a source map is not loaded or the copy is predicted to fail.
	std::string PrepareBegin(int controller, bool& safeToForce);
	// Record fields after Begin, for the log.
	std::string DescribeRecord(int controller);
	// The record's source-0 prematch slot carries the local XUID: it was copied from the save (a blank
	// DDL instance has player_xuid 0). True after a Begin the engine ran with its own gate on.
	bool PrematchHoldsSave(int controller);
	void SetTainted(int controller, bool tainted);
	bool IsTainted(int controller);
	// New match: resets the XP transcript counters.
	void OnMatchStart();

	// --- CommitStatsTransfer --------------------------------------------------------------------
	// Every precondition the commit checks, in its order, plus our stamp if the buffer lost it.
	std::string PrepareCommit(int controller, int source);
	// Set record+498+source, as a final commit does before anything else, so the recap still runs
	// for a controller whose copy we withhold.
	void MarkCommitted(int controller, int source);

	// --- G_AddPlayerRankXp transcript -----------------------------------------------------------
	// Returns false once the per-match detail budget is spent (the caller then only counts).
	bool ShouldLogXpEvent();
	int ReadServerRankXp(int clientNum);   // svClient+54188, -1 if unreadable
	bool ServerStatsSlotReady(int clientNum);

	// --- The AE block: account XP / level / AAR (dump_anchors.hpp) ------------------------------
	// The server's XP counter starts from the client's AE blob. LAN never has one, so every match
	// started at 0 and overwrote the saved rankxp. Raises svClient+54188 to the saved rankxp when it
	// is below it, whatever the blob held. Server thread, before the original G_AddPlayerRankXp runs.
	void SeedServerRankXp(int clientNum);

	// A local ae_sync instance per controller, standing in for the Demonware one. Its base_xp
	// mirrors zm_progression's saved rankxp (re-read at most every 500 ms, or now if forced).
	// nullptr while the DDL root or the save is not loaded yet.
	void* LocalAeInstance(int controller, bool forceRefresh = false);
	void* LocalAeRootStateSlot(int controller);
	// Engine.GetAESyncBuffer with the local block: pushes it and returns true, or false to let the
	// original push nil. Lua thread.
	bool PushLocalAeToLua(void* luaState, int controller);
	// The engine's own answer (the detour's original): non-null means a Demonware sign-in made the
	// engine's block, and readers get that one. Defined in ZmProgression_Hooks.cpp.
	void* EngineAeInstance(int controller);
	// Online boots: the engine's block is blank and no backend fills it. Copies the local block into
	// it once per creation (the engine makes it again on a sign-in and when the ffotd loads), then
	// keeps its base_xp at the saved rankxp. Left alone when it already holds XP we did not put there
	// (a backend with an AE service). At most every 500 ms unless forced.
	void SyncEngineAe(int controller, void* engineInstance, bool forceRefresh = false);
	// The block every reader gets: the engine's own (synced first), else the local stand-in.
	void* AeBlockFor(int controller, bool forceRefresh = false);
	// Game thread, every frame: syncs the engine's block as soon as it exists, because some readers
	// take it from LiveUser+40024 directly and never pass a detour.
	void TickEngineAe();

	// BeginStatsTransfer succeeded: give the record the two AE instances LIVE would have copied
	// (source-2 prematch at +176, current at +416), so the match-end recap reaches the AAR Lua.
	void PrepareAeSlots(int controller);
	// A final commit landed and both playerdata sources are in: take the match's gun XP and camo
	// progress into the local block (from the server slot we backed, or from the engine's block the
	// server updated), re-read the new rankxp into it and the record's +416, save the file, and raise
	// the AAR's ready flags, before CommitStatsTransferAndRecap calls the LUI recap. Runs again for a
	// later final commit of the same match (source 2 on an online boot).
	void OnFinalCommit(int controller, int source);

	// --- Gun XP and camo challenges (dump_anchors.hpp) -------------------------------------------
	// They are written to the server's source-2 (AE) slot, which only LIVE fills. SV_ClientStatsReady
	// detour, before the original: for a player on this PC, back that slot with a copy of the local
	// ae_sync block (loaded from player/cwmod_ae_sync_<ctrl>.bin). OnFinalCommit copies it back and
	// saves the file. On an online boot the engine imported the slot itself and it is left alone.
	// Server thread.
	void OnServerClientStatsReady(std::uint8_t* svClient);

	// --- LIVE menus (dump_anchors.hpp) --------------------------------------------------------------
	// On with progression + the AE block, unless "live_menus" is false in cw-mod.json.
	bool LiveMenusEnabled();
	// The lobby is LAN on this boot, so Engine.GetLobbyNetworkMode's askers are worth logging.
	bool LogsNetworkModeQueries();
	// The value the lobbyRoot.lobbyNetworkMode model gets: LAN becomes LIVE while enabled.
	int MenuNetworkMode(int mode);
	// Game thread, every frame: our own boot sets LAN before the detour exists and nothing sets it
	// again, so re-apply when the model disagrees with MenuNetworkMode(engine value).
	void TickMenuNetworkMode();
	// Calls the model setter's original with MenuNetworkMode(mode). Defined in ZmProgression_Hooks.cpp.
	void ApplyMenuNetworkMode(int mode);
	// Engine.GameModeIsMode(mode) as asked by `luaState`'s running Lua (Lua_GameModeIsMode_Impl
	// detour). True = answer "no": the caller is one of the two CACUtility progression gates, the
	// question is "custom game?", and this is a ZM LAN custom lobby. Every other caller (loadout
	// set choice, party size, HUD) keeps the real answer. Lua thread.
	bool HideCustomGameFromCaller(void* luaState, int mode);
	// Engine.GetLobbyNetworkMode was just answered for `luaState`'s running Lua: logs each distinct
	// asker (the calling Lua frame and the two above it) once, up to a cap. Read-only. Lua thread.
	void NoteNetworkModeQuery(void* luaState);

	// Set once the LAN AAR path has been armed this session: LUI fatals are then logged and
	// suppressed (the AAR Lua was written for a LIVE AE reply we only imitate).
	bool SuppressLuiFatals();

	// --- Overlay: bonus XP ----------------------------------------------------------------------
	// XP queued from the overlay for the host's player (client 0). Nothing calls the engine from the
	// render thread: the G_AddPlayerRankXp detour adds the pending amount to that player's next XP
	// event (any kill) on the server thread, so it takes the engine's own path (multiplier, table
	// clamp, level-up notification) and the match-end commit saves it like any other XP.
	int QueueBonusXp(int amount);      // any thread; returns the new pending total
	int PendingBonusXp();
	int TakeBonusXp(int clientNum);    // detour side: 0 for anyone but client 0

	struct XpStatus {
		bool available = false;   // progression on and the AE calls resolved
		int saved = -1;           // zm_progression rankxp in playerdata, -1 if not loaded
		int savedLevel = -1;      // Rank_GetLevelForXp, 0-based (in-game level = +1)
		int match = -1;           // svClient+54188 while the server holds client 0's stats, else -1
		bool tainted = false;     // this match started blank: its XP will not be saved
	};
	XpStatus ReadXpStatus();          // game thread
}
