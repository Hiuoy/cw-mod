#pragma once
// Every build-locked engine anchor, as DUMP-ABSOLUTE addresses.
//
// We let the compiler compute the RVA (dumpAbs - kDumpImagebase) so there is no hand arithmetic to
// get wrong, then add the live module base (see anchors.cpp). PROVISIONAL: build-locked to
// 1.34.0.15931218 (dump imagebase 0x7FF71CBC0000). TODO: convert to AOB signatures once the IDB is
// available, then delete the RVA path. See docs/phase3_netcode.md.
#include <cstddef>
#include <cstdint>

namespace Client::Game {
	constexpr std::uintptr_t kDumpImagebase = 0x7FF71CBC0000ULL;

	constexpr std::uintptr_t kDump_SV_DirectConnect        = 0x7FF723D4AB60ULL;
	constexpr std::uintptr_t kDump_SV_ReseatClientLoopback = 0x7FF723D49410ULL; // was 0x7FF71FD49410 (mid-function typo); IDB names this address
	constexpr std::uintptr_t kDump_g_svClients             = 0x7FF72DA75B00ULL;
	constexpr std::uintptr_t kDump_sv_migrationInProgress  = 0x7FF72D672950ULL;
	constexpr std::uintptr_t kDump_SV_StageConnectMessage  = 0x7FF72669E1F0ULL;
	constexpr std::uintptr_t kDump_SV_UnstageConnectMessage= 0x7FF72669E1A0ULL;
	constexpr std::uintptr_t kDump_Com_SessionMode_GetStr  = 0x7FF728D7BD60ULL;
	constexpr std::uintptr_t kDump_g_netFieldChecksum      = 0x7FF736EE2EA4ULL;
	constexpr std::uintptr_t kDump_qportCounter            = 0x7FF72D7C0AC4ULL;

	// Lever-3 host-launch capture (read-only diagnostics; see docs/phase3_netcode.md GSC/LobbyVM trigger).
	constexpr std::uintptr_t kDump_HostLaunchBlock         = 0x7FF73254EAC0ULL; // latched StartLaunch param block (0x140 bytes)
	constexpr std::uintptr_t kDump_g_hostLaunchPhase       = 0x7FF73254F5E8ULL; // host-launch FSM phase
	constexpr std::uintptr_t kDump_g_localPlayerCount      = 0x7FF7323AF2A8ULL; // local player count

	// LAN announce/search state (read-only; see DumpSessionState / docs/phase3_netcode.md LAN layer).
	constexpr std::uintptr_t kDump_g_netSessionManager     = 0x7FF73753F5C8ULL; // ptr to NetSession manager (null until session exists)
	constexpr std::uintptr_t kDump_g_netSessionLaunchState = 0x7FF72ADC7CA0ULL; // launch-state enum the pump drives

	// GSC-facing launch wrappers we hook to capture the clonable launch args.
	constexpr std::uintptr_t kDump_GScr_LaunchP2P          = 0x7FF727A99000ULL; // GScr_LobbyHost_LaunchPrivateP2P
	constexpr std::uintptr_t kDump_GScr_LaunchP2P_Named    = 0x7FF727A990F0ULL; // ..._Named
	constexpr std::uintptr_t kDump_GScr_LaunchP2P_Full     = 0x7FF727A99190ULL; // ..._Full

	// The native host-launch entrypoint we REPLAY through. LobbyHost_LaunchPrivateP2P_Plain builds the
	// two GSC istring/script-value handles and the pointer-laden session-config blob itself (via
	// sub_7FF729E07240 + sub_7FF728C18B00), so we only feed scalars + two persistent config-string
	// pointers captured from a real _Full call — no hand-fabricated script objects. -> _Resolve ->
	// HostSession_StartLaunch. See docs/phase3_netcode.md.
	constexpr std::uintptr_t kDump_LobbyHost_LaunchP2P_Plain = 0x7FF72614EE00ULL;

	// Lever-4 client-join chain (mirror of the host chain above). ClientSession_JoinKnownHost is the
	// one-shot trigger; the other two are the primitives it composes, hooked so a real join tells us
	// what a valid host candidate — above all the 16-byte netadr handle — actually contains.
	constexpr std::uintptr_t kDump_ClientSession_JoinKnownHost = 0x7FF727946AD0ULL;
	constexpr std::uintptr_t kDump_ClientSession_StartJoin     = 0x7FF727946640ULL;
	constexpr std::uintptr_t kDump_JoinCtx_AddHostCandidate    = 0x7FF727946520ULL;
	constexpr std::uintptr_t kDump_ClientSession_JoinKick      = 0x7FF727FC0470ULL;
	constexpr std::uintptr_t kDump_g_clientJoinCtx             = 0x7FF730430DA0ULL;
	constexpr std::uintptr_t kDump_cl_lobbyLaunchState         = 0x7FF73050A740ULL;

	// Lever-4 NATIVE path. The client join needs only the host XUID: QueryHostByXuid sends netmsg 0
	// (InfoRequest) addressed by XUID, the host answers netmsg 1 (InfoResponse) with the full
	// descriptor, the engine parses it into g_pendingJoinTarget_*, and JoinPendingTarget consumes
	// those globals. Nothing here is fabricated or captured.
	constexpr std::uintptr_t kDump_ClientSession_QueryHostByXuid   = 0x7FF727947190ULL;
	constexpr std::uintptr_t kDump_ClientSession_JoinPendingTarget = 0x7FF727946A30ULL;
	constexpr std::uintptr_t kDump_Session_GetSessionObject        = 0x7FF726F82D20ULL;

	// g_pendingJoinTarget_* — the descriptor ClientSession_JoinPendingTarget consumes (lever 4).
	constexpr std::uintptr_t kDump_pendingJoin_awaiting            = 0x7FF730437720ULL;
	constexpr std::uintptr_t kDump_pendingJoin_nonce               = 0x7FF730437724ULL;
	constexpr std::uintptr_t kDump_pendingJoin_xuid                = 0x7FF730437728ULL;
	constexpr std::uintptr_t kDump_pendingJoin_valid               = 0x7FF730437738ULL;
	constexpr std::uintptr_t kDump_pendingJoin_sessionId           = 0x7FF730437748ULL;
	constexpr std::uintptr_t kDump_pendingJoin_hostName            = 0x7FF730437750ULL;
	constexpr std::uintptr_t kDump_pendingJoin_slot                = 0x7FF730437774ULL;
	constexpr std::uintptr_t kDump_pendingJoin_secId               = 0x7FF73043778CULL;
	constexpr std::uintptr_t kDump_pendingJoin_secKey              = 0x7FF730437794ULL;
	constexpr std::uintptr_t kDump_pendingJoin_serializedAdr       = 0x7FF7304377A5ULL;

	// Lever-5 netmsg transcript. The wire went quiet on us for a reason we could not read: the
	// lobby handshake encrypts its application payload, so a capture shows two machines talking
	// and nothing about what they said. These anchors sit INSIDE the process, after decryption.
	constexpr std::uintptr_t kDump_NetMsg_Dispatch         = 0x7FF726BF9370ULL;
	constexpr std::uintptr_t kDump_NetMsg_SendJoinResponse = 0x7FF726BF82C0ULL;
	// The JoinLobby request parser. Hooked on the HOST so the incoming request's fields are readable
	// side by side with the verdict they produce — otherwise a refusal names a code but never the
	// value that earned it.
	constexpr std::uintptr_t kDump_Session_ParseJoinLobbyRequest = 0x7FF726BFB070ULL;
	// Progression / PlayerData — the actual ZM upgrade gate (see game.hpp for the causal chain).
	constexpr std::uintptr_t kDump_g_playerDataDefsById     = 0x7FF73037BA60ULL;
	constexpr std::uintptr_t kDump_g_playerDataStore        = 0x7FF730351A10ULL;
	constexpr int            kPdMaxDataMapId                = 0x2D;  // engine bound: id-1 <= 0x2C
	// PlayerData_ControllerStorageTick(controller) — the per-frame load driver, and the seam that
	// actually works. It is what submits each location, so its FIRST call is by construction before
	// location 0 has been submitted (submittedFlag at store + 86040*ctrl + 8 + loc is still clear).
	// Rewriting the defs there needs no race with init and no flag surgery: the location-0 pass that
	// this very call is about to make then picks the rewritten maps up along with the real hdd ones.
	constexpr std::uintptr_t kDump_PlayerData_ControllerStorageTick = 0x7FF7275C1E90ULL;
	// Per-location "already submitted" flags: one byte each, locations 0..4, immediately before the
	// entry array (which starts at +16). Only read to detect the case where we arrived too late.
	constexpr std::size_t    kPdStoreSubmittedFlagsOff = 8;
	// PlayerData_OnStorageOpComplete(controller, opKind, resultCode, entry) — THE writer of entry+120
	// on the load path, and the thing that actually decides when a data map becomes readable.
	//
	// This corrects a claim that cost a boot. PlayerData_ResetBufferToDefaults was believed to be the
	// only setter of entry+120 (an old IDB comment said so, and it was repeated here without being
	// checked). Measured instead: an online boot called ResetBufferToDefaults exactly ONCE, for map 1,
	// which was already loaded — while THIRTEEN maps went from unloaded to loaded in the same window.
	// ResetBufferToDefaults is the DEFAULTS path, for when there is nothing to read.
	//
	// The load path is asynchronous: SubmitLocationLoad queues reads, PlayerDataHdd_JobWorker runs
	// them off-thread, and each completion lands here and sets entry+120 on the entry it names. That
	// is why the LOADED set grows over seconds during boot, and why a frontend built too early sees a
	// nil for a map that is perfectly fine a moment later. Hooking this yields every completion in
	// order — including whether map 4's read ever lands, and with what result code.
	//
	// a4 is the ENTRY, not a job: a4[30] is entry+120 and *a4 is the dataMapId.
	constexpr std::uintptr_t kDump_PlayerData_OnStorageOpComplete = 0x7FF7275C1920ULL;
	// --- THE FAILURE BRANCH (decompiled 2026-07-29, after the dwuser->hdd redirect landed) --------
	// The redirect worked: 66 completions instead of 50, and map 4 finally appears as a real
	// storage=hdd(0) READ. But 14 of the 66 come back result=1 (failed) — every one of them among the
	// 18 maps we redirected, because those maps have only ever lived on Demonware and no local .cgp
	// exists yet. So the next question was what the engine does with a failed read, and the answer is
	// not what the transcript appeared to say.
	//
	//     // opKind 0 (READ), a3 != 0 (the read failed):
	//     a4[30] = 1;                                       // SETTLED — keep whatever is in the buffer
	//     if (a3 == 3 && def.storageLocation == 4)          // fastfile absent
	//         { a4[30] = 2; v13 = 0; }
	//   LABEL_40:
	//     v11 = &unk_7FF73037BBD0[4 * dataMapId];           // slots [0] and [1]
	//     if (*v11  && !(*v11 )(ctrl, id, slot, a3, ...)) { a4[30] = 0; v13 = 1; }
	//     if ( v28  && ! v28  (ctrl, id, slot, a3, ...)) { a4[30] = 0; v13 = 1; }
	//     if (v13) { ++a4[2*opKind + 33];                   // attempt counter
	//                a4[2*opKind + 34] = 1000 * min(attempt, dvarMax) * dvarInterval + now; }
	//
	// Two things follow. First, the engine ALREADY does the right thing on a failed read — entry+120
	// becomes 1, meaning "settled", and there is deliberately no defaults refill here. Second, a
	// per-map callback then rejects it and puts the flag back to 0, arming an exponential retry that
	// re-reads a file which will never exist. So the instrument's "entry+120 unchanged at 0" was
	// hiding a real 0 -> 1 -> 0: it only sampled before and after the original call, and an internal
	// set-then-veto is invisible from there. That wording is fixed in the impl.
	//
	// cw-mod's answer is to do both halves the engine would have done had the data ever existed:
	// fill the buffer from the map's DDL defaults up front (PlayerData_ResetBufferToDefaults, from
	// the tick, on the main thread — viable now only because the def says hdd, which IS available),
	// and then, for redirected maps only, undo the veto and clear the retry state so the defaults are
	// not thrown away by the next attempt. Stock maps keep stock behaviour, veto included.
	constexpr std::uintptr_t kDump_g_playerDataMapCallbacks = 0x7FF73037BBD0ULL;
	// PlayerData_ResetBufferToDefaults(controller, dataMapId, version). The engine's own buffer
	// initialiser: fills the buffer from the map's DDL defaults (sub_7FF72967CA70) and only then sets
	// entry+120 = 1, and only if PlayerDataStorage_IsAvailable agrees. Pass version -1 to mean "the def's
	// default version" (def+48) — the entry search matches on dataMapId AND version, so a wrong version
	// silently finds nothing.
	// Calling this is the CORRECTION to flipping entry+120 by hand: a buffer marked loaded but never
	// filled with DDL defaults is a structurally invalid DDL instance, which reads back as zeros/garbage.
	constexpr std::uintptr_t kDump_PlayerData_ResetBufferToDefaults = 0x7FF7275C2250ULL;
	// The packed session-mode word. gameMode = bits 0-3, networkMode = bits 4-7, matchType = bits 12-15.
	// Read-only here: progression keys off matchType, LAN discovery keys off networkMode == 1, and the
	// two must not be conflated.
	constexpr std::uintptr_t kDump_g_sessionModePacked           = 0x7FF73561C7F8ULL;

	// LAN browser "in match" flag, read the way Com_IsGameServerRunning (0x7FF722349350) decides it,
	// minus its Dvar_GetBool calls: sv_running's plaintext value AND NOT the frontend UI level. Both
	// read-only. p_dvar_svRunning holds the Dvar*; value block = *(dvar + 16), byte 0.
	constexpr std::uintptr_t kDump_p_dvar_svRunning              = 0x7FF72CCC1158ULL;
	constexpr std::uintptr_t kDump_g_uiLevelRunning              = 0x7FF72CCC2719ULL; // u8

	// LiveUser_GetObject(c) is literally this array indexed by c — the whole function is one load.
	constexpr std::uintptr_t kDump_g_liveUserObjects                     = 0x7FF72ADC3390ULL;

	// g_dvarAllowServerFlaggedWrites — WHY THE FORCE-RAW-BLOCK-COPY SET SILENTLY DID NOTHING.
	//
	// Measured in-game: Dvar_GetBool read 0 both before and after Dvar_SetBoolFromSource, on a
	// kind-1 / flags-0x400 dvar. The setter is not at fault — Dvar_SetBoolFromSource (dump
	// 0x7FF728C75580) handles kind 1 correctly, writing the plaintext check byte at value+0 and the
	// PEB+ImageBase-mixed word at value+24, which is exactly the layout Dvar_GetBool decodes. It
	// then hands off to Dvar_ApplyValueInternal (dump 0x7FF728C78DB0), which opens with:
	//
	//     if (!dvar || (dvar->hash & 0x7FFFFFFFFFFFFFFF) == 0 ||
	//         ((dvar->flags & 0x400) && Sys_IsMainThread() && !g_dvarAllowServerFlaggedWrites))
	//         return;                       // silent: no log, no return code, nothing to observe
	//
	// Flag 0x400 is "server-authoritative": writable only off the main thread, or while this latch
	// is held. Our overlay calls from the D3D12 present thread, which IS the main thread, so the set
	// returned normally and changed nothing. This is also why Dvar_CanSetValue looked innocent — it
	// only screens 0x10/0x40/0x80, and it is not even reached for source 0.
	//
	// The engine's own network path proves the intended usage: Dvar_ApplyServerDvarPacket brackets
	// its applies with Dvar_SetAllowServerFlaggedWrites(1) ... (0), and its "flags you may not OR in"
	// mask drops from 0x4D8 to 0xD8 while the latch is held — a difference of exactly 0x400.
	//
	// We bind the byte rather than the one-instruction setter (dump 0x7FF728C75A80, literally
	// "mov g_dvarAllowServerFlaggedWrites, cl; ret") so the bracket can save and restore whatever
	// the engine had, instead of unconditionally clearing it and stomping a packet apply in flight.
	constexpr std::uintptr_t kDump_g_dvarAllowServerFlaggedWrites         = 0x7FF73560D544ULL;

	// String dvar writes, the way Dvar_ApplyServerDvarPacket (0x7FF728C73A80) applies a PubVar:
	// Dvar_StringToValue(out32, dvar->type, &dvar->domain (+32, 16 B), text) builds the 32-byte value,
	// including the obfuscated check word at +24 that every getter decodes, then
	// Dvar_ApplyValueInternal(dvar, value, 0) copies a type-8 string into engine memory. Source 0
	// skips Dvar_CanSetValue; flag 0x400 still needs the latch above. Thunked (ArxanCall, 4/3 args).
	constexpr std::uintptr_t kDump_Dvar_StringToValue                     = 0x7FF728C7A580ULL;
	constexpr std::uintptr_t kDump_Dvar_ApplyValueInternal                = 0x7FF728C78DB0ULL;

	constexpr std::uintptr_t kDump_g_netMsgNames           = 0x7FF72ADEF4E0ULL;
	constexpr std::uintptr_t kDump_g_netMsgHandlers        = 0x7FF72A392110ULL;
	constexpr std::uintptr_t kDump_g_joinResponseCode      = 0x7FF730437710ULL;
	constexpr std::uintptr_t kDump_g_expectedHostAdr       = 0x7FF730433E98ULL;
	// The Lua 5.1 C API. Named and verified in the .i64 by decompiling the lua_index2adr xref
	// cluster against Lua 5.1 source semantics. All of these are wrapped in ArxanCall thunks.
	constexpr std::uintptr_t kDump_lua_getfield     = 0x7FF729E3CC50ULL;
	constexpr std::uintptr_t kDump_lua_gettable     = 0x7FF729E3CD90ULL;
	constexpr std::uintptr_t kDump_lua_rawgeti      = 0x7FF729E3CE40ULL;
	constexpr std::uintptr_t kDump_lua_next         = 0x7FF729E3E310ULL;
	constexpr std::uintptr_t kDump_lua_pushvalue    = 0x7FF729E3B2E0ULL;
	constexpr std::uintptr_t kDump_lua_type         = 0x7FF729E3B860ULL;
	constexpr std::uintptr_t kDump_lua_tolstring    = 0x7FF729E3BD70ULL;
	constexpr std::uintptr_t kDump_lua_tonumber     = 0x7FF729E3EA90ULL;
	constexpr std::uintptr_t kDump_lua_getmetatable = 0x7FF729E3D150ULL;

	// --- The Lua debug API, for reading a Lua error's call stack WHILE IT STILL EXISTS ------------
	//
	// LuiError_ReportFatal is too late to answer "which menu, built with what?". By the time it runs
	// the lua_pcall has already returned, so luaD_throw has unwound every frame the question is about
	// and all that is left is the formatted message. That is exactly why the first online-boot error
	// ("attempt to index a nil value") named nothing useful: T9's chunks are compiled with local and
	// upvalue names stripped, so Lua cannot append its usual "(field 'x')" and the message is generic
	// no matter how interesting the failure is.
	//
	// luaL_traceback is the errfunc LUI installs around those pcalls - it is what appends
	// "stack traceback:" to the message - and an errfunc runs BEFORE the throw unwinds. Hooking it
	// puts us on the erroring stack with every frame, and every frame's arguments, still live.
	//
	// lua_getstack fills lua_Debug::i_ci (+316) with a packed frame locator:
	//     lo16 = (frame - L->stack) >> 3        (L->stack is the pointer at L+32)
	//     hi16 = the frame's slot count
	// so frame_base = *(TValue**)(L+32) + lo16, the running closure is frame_base[-1], and
	// frame_base[0 .. hi16) are that call's arguments and locals. Reading them as raw NaN-boxed
	// qwords sidesteps the stripped debug info entirely: we cannot ask Lua what the argument was
	// CALLED, but we can read what it IS - and for LUI.createMenu that argument is the tag -6 hashed
	// menu name we already know how to resolve.
	constexpr std::uintptr_t kDump_luaL_traceback   = 0x7FF729E48320ULL;
	constexpr std::uintptr_t kDump_lua_getstack     = 0x7FF729E47100ULL;
	constexpr std::uintptr_t kDump_lua_getinfo      = 0x7FF729E47E20ULL;

	// --- An engine bug that turns any hashed-name Lua error into a process death -----------------
	//
	// MEASURED, not theorised: the online frontend's first Lua error was followed immediately by
	//   0xC0000005 (reading 0x39) at BlackOpsColdWar.exe+0xD291273
	// which is luaO_pushvfstring's inline strlen of a "%s" argument, reached from luaL_traceback's
	//   sub_7FF729E3C720(B, " in function '%s'", ar.name)   [call site 0x7FF729E485B4]
	//
	// The chain, all confirmed by decompiling the three functions:
	//   luaG_getobjname, opcode-57 (hashed-name call) branch:
	//       if (*(BYTE*)(k + 8) != 5) return "xhashfunc";     <-- returns namewhat, NEVER writes
	//                                                              *nameOut
	//   lua_getinfo 'n' branch:
	//       ar.namewhat = result;
	//       if (!result) { ar.namewhat = ""; ar.name = 0; }   <-- ar.name only cleared when the
	//                                                              result is NULL, which this is not
	//   luaL_traceback:
	//       if (*ar.namewhat) addfstring(B, " in function '%s'", ar.name);
	//
	// So ar.name keeps whatever the caller's uninitialised stack slot held. Usually that is a
	// readable byte pattern and the traceback merely prints nonsense - which is exactly the
	// "in function '@&['" seen in earlier dumps, so this bug has been visible in our logs from the
	// start and was read as harmless cosmetic damage. When the slot instead holds a non-pointer
	// (0x39 here) the strlen faults and the process dies. Offline it is near-invisible because the
	// frontend does not raise; the online frontend raises on every menu it tries to build.
	//
	// Hooked to write a valid pointer on that one path. The two "xhashfunc" literals are distinct
	// addresses in .rdata and only the first is the non-writing return, so the detour can identify
	// the exact case by comparing the returned pointer rather than the string's text.
	constexpr std::uintptr_t kDump_luaG_getobjname  = 0x7FF729E476D0ULL;
	constexpr std::uintptr_t kDump_aXhashfuncNoName = 0x7FF72AFAD9B0ULL; // the non-writing return

	// --- Local Demonware backend (see memory cw-mod-demonware-backend) --------------------------
	// The two RSA-2048 public keys the client uses to authenticate Demonware. Both are 294-byte DER
	// SubjectPublicKeyInfo blobs baked into .rdata; we overwrite them in memory with our own keys so
	// a local server we control (whose private keys sign the replies) passes the client's checks.
	//   auth: DwAuth_VerifyReplySignature verifies the X-Signature header (RSASSA-PSS/SHA-256/salt=0)
	//         over the /auth/ reply body with this key.
	//   lsg : Lsg_BuildHandshake_ParseBDDATA guards the CLIENTCHAL/BDDATA handshake with this key.
	constexpr std::uintptr_t kDump_g_dwAuthSigPubKey_DER    = 0x7FF72A5381B0ULL; // RVA 0xD9781B0
	constexpr std::uintptr_t kDump_g_lsgHandshakePubKey_DER = 0x7FF72A537330ULL; // RVA 0xD977330
	constexpr std::size_t    kDwPubKeyDerLen                = 294;               // fixed RSA-2048 SPKI len

	// Dw_GetLoginFlow (bdLogin flow/service selector): returns *(uint*)(loginConfig+3124), with a
	// 1->4/8 remap. The login state machine sends flow 9 straight to the Demonware Auth task,
	// skipping the Battle.net first-party token step; every one of its 12 callers is login/auth, so
	// hooking it to return 9 selects "studio auth" with no collateral. RVA 0xE2BF90.
	constexpr std::uintptr_t kDump_Dw_GetLoginFlow          = 0x7FF729DEBF90ULL;

	// Login_SetStatus(ctx, const char* statusString, unsigned int code): the login state machine's
	// own status setter (sub_7FF729DECF80 calls it at every transition). It copies the status string
	// into ctx+24 and stores the code. Those strings ("Fetching First-Party Token", "Studio auth
	// login detected...", "Authenticating to Demonware", "Authenticated to Demonware", "Connecting to
	// LSG", "Login Complete", and every failure reason) go to the game's internal log, NOT our
	// console — so we hook it read-only to MIRROR the full login transcript into our console. This is
	// pure diagnostics: it always calls the original. RVA 0xE2C4D0.
	constexpr std::uintptr_t kDump_Login_SetStatus          = 0x7FF729DEC4D0ULL;

	// The master gate for the per-frame LiveUser update loop (LiveUserSystem_Tick, sub_7FF7262ACAA0):
	//   if (g_liveUserSystemActive && reentryGuard <= 0) { for c in 0..1 LiveUser_LoginDriver_Tick(c); }
	// If this byte is 0, the login driver never ticks at all — so nodw/flow-9/key-swap are never even
	// consulted. Read-only diagnostic anchor (DumpDwLoginState). RVA 0xDF2018D.
	constexpr std::uintptr_t kDump_g_liveUserSystemActive   = 0x7FF72AAE018DULL;

	// --- Login-driver state-1 gates -----------------------------------------------------------------
	// The state-1 connect path in LiveUser_LoginDriver_Tick only calls Dw_GetOrCreateConnection443
	// when ALL of these hold: g_liveUserSystemActive!=0, nodw==false, live_connect_mode==1, ctrl==0,
	// g_liveUserLoginAllowed!=0, and LiveUser_FirstPartyPresenceOk()==true. On a no-Battle.net launch
	// the first-party presence check fails, so login never begins; the presence hook fixes that, and
	// DumpDwLoginState reads the rest.
	//
	// live_connect_mode dvar SLOT (holds the dvar_t*, like p_dvar_nodw). Gate B: must read int 1.
	constexpr std::uintptr_t kDump_dvar_liveConnectMode     = 0x7FF733D20100ULL;
	// g_liveUserLoginAllowed byte (LiveUser_GetLoginAllowedFlag returns it). RVA 0x25B075D7.
	constexpr std::uintptr_t kDump_g_liveUserLoginAllowed   = 0x7FF7371D75D7ULL;
	// LiveUser_FirstPartyPresenceOk (bool()). THE WALL: with nodw==false it demands a signed-in
	// Battle.net first-party manager. We MinHook it and return 1 ONLY when the caller is inside
	// LiveUser_LoginDriver_Tick (return-address guard), so its ~29 other callers are untouched.
	constexpr std::uintptr_t kDump_LiveUser_FirstPartyPresenceOk = 0x7FF7296B79B0ULL;
	// LiveUser_LoginDriver_Tick base + size: the return-address window the guard accepts.
	constexpr std::uintptr_t kDump_LiveUser_LoginDriver_Tick     = 0x7FF727F39160ULL;
	constexpr std::size_t    kLiveUser_LoginDriver_TickSize      = 0xB98;
	// LiveUser_OnSigninStateChange_SetupIdentity (0x7FF729753500, 0xFC bytes). The ONE presence caller
	// the widened hook must never lie to: with presence true (and ctrl 0, no split-screen) it takes
	// the gamertag from FirstParty (sub_7FF7296B6430 = *(*(g_firstPartyManager+32)+80)), a manager
	// studio auth never signs in. Everywhere else it falls back to the local identity path.
	constexpr std::uintptr_t kDump_LiveUser_SetupIdentity        = 0x7FF729753500ULL;
	constexpr std::size_t    kLiveUser_SetupIdentitySize         = 0xFC;
	// The two account-id pickers, the same kind of caller (2026-09-24 22:58, site #15 at RVA 0xCAEB368):
	//   LiveUser_GetAccountId_cand(ctrl, u64* out)   0x7FF7296AB340, 0x62  <- match launch
	//   LiveUser_RefreshAccountId_cand(userData)      0x7FF7296AB2F0, 0x49  (writes userData+31824)
	// Both do `if (nodw || ctrl || !presence) id = the Demonware/local id (sub_7FF7296AB090); else
	// id = FirstParty_GetBnetAccountId_cand()` (0x7FF7296B6440 -> 0x7FF7297DEE70 -> GetAccount on
	// *(g_firstPartyManager+24)), and on studio auth that object is null: `mov rax,[rcx+58h]` with
	// rcx 0 at 0x7FF72978BA20, one frame after "Launching game". They sit back to back, so one window
	// covers both: 0x7FF7296AB2F0 .. 0x7FF7296AB3A2.
	constexpr std::uintptr_t kDump_LiveUser_AccountIdPickers     = 0x7FF7296AB2F0ULL;
	constexpr std::size_t    kLiveUser_AccountIdPickersSize      = 0xB2;

	// --- Peer addressing on LIVE (2026-09-24, the server browser join) ---------------------------
	// Every peer is reached through its bdCommonAddr, the 84-byte "serializedadr" in the join
	// descriptor (bdCommonAddr_Deserialize 0x7FF729D377F0): 5 x local addr (ip4 + port), public addr,
	// u8 NAT type, u32, u8 relay flag (+ a 42-byte relay route when set). This PC's own one is built by
	// bdNetImpl (start 0x7FF729DFF620 <- NetSession_Launch 0x7FF727AA4650, online = LobbyBase network
	// mode 2) through this constructor, from exactly three call sites:
	//   start, online == 0 (LAN)    public = empty,                 NAT = 1 (open)
	//   pump, preset public         public = net+64,                NAT = net+216
	//   pump, after STUN discovery  public = discovered, NAT = discovered   (ops4-stun.*.demonware.net)
	// Connecting then hinges on the public address: bdCommonAddr_IsSameAddr 0x7FF729D37AC0 calls a peer
	// "loopback" when its main address (public if valid, else local[0]) equals ours, and a peer behind
	// another public address needs Demonware NAT traversal or its relay, neither of which our backend
	// has. LAN's construction has neither problem and is the one proven across two PCs (2026-07-23), so
	// on a backend boot the detour builds the LAN shape: empty public, NAT open. The online state machine
	// (net status 2 -> g_netSessionLaunchState 8) does not look at either field.
	// g_bdAddrEmpty is the engine's own empty bdAddr, the one bdAddr_IsValid 0x7FF729E04330 compares with.
	constexpr std::uintptr_t kDump_bdCommonAddr_Ctor             = 0x7FF729D373D0ULL;
	constexpr std::uintptr_t kDump_g_bdAddrEmpty                 = 0x7FF73753F5E0ULL;
	constexpr std::uint32_t  kBdNatType_Open                     = 1;
	// g_firstPartyManager SLOT (off_7FF7371BF208). Read-only, so we can see whether the
	// first-party manager is even allocated when not signed in. RVA 0x25AFF208.
	constexpr std::uintptr_t kDump_g_firstPartyManager      = 0x7FF7371BF208ULL;

	// FirstParty_GetSession_MayBeNull(mgr) = *(void**)(mgr + 24), the first-party (Battle.net)
	// SESSION object hanging off that manager. Two instructions, exactly 5 bytes
	// (48 8B 41 18 / C3), so a MinHook jmp rel32 fits without spilling into the next function --
	// and because it ends in `ret` the detour can reproduce it outright instead of calling through
	// a trampoline. RVA 0xCC1F050.
	//
	// WHY IT MATTERS: the ctor (FirstParty_Manager_Ctor_NullsSessionAt24, 0x7FF7297DD5A0) sets +24
	// to 0 explicitly and NOTHING on the studio-auth path ever assigns it -- flow 9 skips Battle.net
	// first-party entirely, which is our own deliberate bypass. All 15 call sites dereference the
	// result unchecked. That is the null deref one second after "[status 27] Login Complete"
	// (0xC0000005 reading 0xD0, via 0x7FF7297D9EE0). It is a NEWLY EXPOSED gap, not a regression:
	// until the LSG lobby reply was accepted the flow never passed status 18, so no post-login code
	// ever asked for a session.
	constexpr std::uintptr_t kDump_FirstParty_GetSession    = 0x7FF7297DF050ULL;
	// The offset the getter reads out of the manager. Named so the detour is not a magic 24.
	constexpr std::size_t    kFirstPartyMgr_SessionOffset   = 24;

	// FirstParty_GetLocalUserIndex_ViaSession() -> *(uint*)(*(void**)(mgr+24) + 0xD0). Nullary, 7
	// instructions, 0x19 bytes, prologue `sub rsp,28h` — ample room for a 5-byte jmp rel32.
	// RVA 0xCC19EE0.
	//
	// MEASURED, one boot, 2026-08-02: this is the ONLY site in the process that asked for the session
	// while it was null, and it asked exactly ONCE, right after "[status 27] Login Complete". The
	// census hooked FirstParty_GetSession, handed back a zeroed stand-in, logged every distinct
	// return address, and the boot then reached the main menu — one line, RA 0x7FF7297D9EF1, which is
	// the return of the call at 0x7FF7297D9EEC inside this very function. The other 98 xrefs to the
	// getter never ran. The old "all 15 call sites dereference it unchecked" worry above was a static
	// count of what COULD run; what DOES run is this.
	//
	// And session+0xD0 is just the LOCAL USER / CONTROLLER INDEX. Its only consumer,
	// FirstParty_RefreshUserRequest (0x7FF7297E81B0), passes it straight into
	// BdUser_SubmitRequestForLocalUser (0x7FF729F3C084) as the user index — that function resolves a
	// per-user object from it and reports bdError 3027 to the request callback if it resolves to
	// nothing. So 0 is not merely survivable, it is the CORRECT answer for a single-local-user PC:
	// controller 0. Hooking here to return 0 makes synthesising a first-party session object
	// unnecessary and keeps every fake object out of the process.
	constexpr std::uintptr_t kDump_FirstParty_GetLocalUserIndex = 0x7FF7297D9EE0ULL;
	// The offset the session getter reads. Named so the detour is not a magic 0xD0.
	constexpr std::size_t    kFirstPartySession_LocalUserIndex  = 0xD0;
	// The first-party manager OBJECT itself (not the pointer slot kDump_g_firstPartyManager above --
	// that is a different global). FirstParty_GetManagerSingleton is a magic-statics wrapper that
	// always returns &this, so the detour reads the object directly and never has to call the
	// TLS-guarded initialiser. Reading it before construction is safe and correct: it is a zeroed
	// static, so +24 is 0, which is exactly the "no session" answer we want. RVA 0xA61A520.
	constexpr std::uintptr_t kDump_g_firstPartyManagerObj  = 0x7FF7371DA520ULL;

	// LiveUser_SignOut_BuildDropMessage(controller, const char** outMsg) -> bool. RVA 0xCB93740.
	//
	// THE SOURCE OF "An error occurred: Boy 501 Gothic Missile", measured 2026-08-02 on the first
	// boot where online mode and the local backend ran together. The word-code is not an error name:
	// it is Com_FormatHash64 printing an error hash that has no localized string, which is what this
	// function falls back to.
	//
	// CORRECTED by B1 (2026-09-16): this is NOT a sign-out. IDB name now
	// LiveUser_PromoteSigninOnline_MaybeDrop. It runs on DW connect and PROMOTES the user
	// (signinState +5764: 0 none, 1 local, 2 online) to 2, notifying subsystems, then decides whether
	// to also drop to offline:
	//
	//     prev = *(u32*)(liveUser + 5764);
	//     ... identity setup, +5764 := 2, notify ...
	//     if ((prev & 0xFFFFFFFD) == 0                          // prev 0 or 2 -> no drop
	//         || (prev == 1 && !Com_SessionMode_IsOnline()))    // local, offline mode -> no drop
	//         return 0;
	//     if (UI string dvar non-empty || session top nibble == 6) return 0;
	//     *outMsg = <qword_7FF72AF57F38, "disconnected">; return 1;
	//
	// On 1 the caller, LiveUser_OnDwConnected (0x7FF729722670, entered on loginState -> 4), forces the
	// session mode offline (Com_SessionMode_ForceOfflineDropped, |0x6030) and ERR_DROPs. Our online
	// marker makes the online clause true, so a local user who connects gets promoted AND dropped.
	// Returning 0 keeps the promotion and skips only the drop.
	constexpr std::uintptr_t kDump_LiveUser_SignOutBuildDropMessage = 0x7FF729753740ULL;
	// The signin-state field the gate above reads. Named so the detour is not a magic 5764.
	// 0 none, 1 local / signed out, 2 signed in online (B1, 2026-09-16).
	constexpr std::size_t    kLiveUser_SigninStateOffset            = 5764;
	// Login-driver state. 4 = Demonware connected (set by LiveUser_OnDwLoginComplete_SetState4,
	// 0x7FF727F38F30); 0 = connection lost / retry.
	constexpr std::size_t    kLiveUser_LoginStateOffset             = 5768;
	constexpr int            kLiveUser_SigninState_Online           = 2;
	constexpr int            kLiveUser_LoginState_Connected         = 4;

	// --- Studio-auth JWT token build (the wall after login starts) -------------------------------
	// DwLogin_BuildStudioToken(loginConfig, userObj, outBuf6784). In the flow-9 studio-auth path the
	// login state machine calls this; on failure it prints "Studio auth login failed to build token"
	// (code 26) and never reaches the Demonware Auth POST. It fails because the JWT is signed with an
	// asymmetric key inline at loginConfig+5416 that the retail Battle.net build does not provision.
	// Hooked (read-only for now) to confirm the failure reason at runtime; later the injection point
	// where we substitute a token our own DW auth server accepts. RVA 0xE2CA90.
	constexpr std::uintptr_t kDump_DwLogin_BuildStudioToken = 0x7FF729DECA90ULL;
	// GetProviderIndex(userObj) = *(userObj+56084); must be <= 3 or BuildStudioToken returns early.
	constexpr std::size_t    kStudioUserObj_ProviderOffset  = 56084;

	// LuiError_ReportFatal(const char* context, lua_State* L) - THE reason opening some menu hashes
	// kills the game, and it is not a fault we can catch.
	//
	// LuiEvent_Dispatch (0x7FF727B8CA80) runs the menu builder under lua_pcall, so the builder's own
	// errors ARE caught by the engine. When pcall returns nonzero the engine comes here: this reads
	// the error message off the top of the Lua stack, hands it to LuiError_ReportAndDie
	// (0x7FF7222C04C0) - which files a telemetry record, busy-waits ~1s and terminates the process -
	// and then does settop(L, -2).
	//
	// So the death is deliberate engine behaviour rather than an access violation, which is exactly
	// why the __try around the dispatch never caught it. A menu whose builder wants data the current
	// UI state has not loaded raises, and the engine treats that as fatal. Hooking this and dropping
	// the report (after reading the message and popping it, as the original does) makes those menus
	// survivable and tells us WHY each one failed. RVA 0x5702340.
	constexpr std::uintptr_t kDump_LuiError_ReportFatal     = 0x7FF7222C2340ULL;

	// LUI_RunFile(lua_State* L, const char* name) -> bool. LuaFile_LoadAsset (0x7FF729642580), then
	// lua_pcall; a failure goes to LuiError_ReportFatal above. BUT a chunk missing from the luafile pool
	// never gets that far: DB_FindXAssetHeader treats a missing type-0x7C asset as fatal in itself,
	// sub_7FF7298101F0(0x3580ADA5, ",<hash>,luafile"), logged as BB_Alert sys_error, then exit.
	//
	// Two callers run ui/ffotd_tu<N>.lua (sub_7FF727B8CA30 builds "%s_tu%d.%s" with Com_GetTuVersion, so
	// ui/ffotd_tu34.lua, asset 0x005D4E9D4E4224CE) whenever OnlineContent_AllSlotsLoaded():
	//   LUI_Init_cand 0x7FF7258D72D0     (the UI re-init at map load, ret 0x7FF7258D806A)
	//   UI_RestartUILevel_cand 0x7FF7223495C0 (the UI level restart, i.e. back to the frontend)
	// On LIVE + backend session.cpp's B3 bypass reports both slots loaded but only the playlists zone
	// loads, so every match launch died 1 s after "Launching game" (22:45, 22:46). Offline/LAN the
	// slots are not loaded and the call never happens. No Arxan caller check; the prologue is a plain
	// 5-byte mov. RVA 0xAFCCCD0.
	constexpr std::uintptr_t kDump_LUI_RunFile              = 0x7FF727B8CCD0ULL;

	// DecryptString(char* s) -> char*. THE choke point for every localized UI string ("ui_text" in
	// cw-mod.json, hooks/impl/game/DecryptString_UiText.cpp). Verified 2026-09-25:
	//   - A localized string is asset type 0x1D, keyed by the 63-bit name hash; its header's +0 is the
	//     text, still encrypted. There is no central SEH_StringEd_GetString in this build: ~100 sites
	//     each do DB_FindXAssetHeader(0x1D, hash) and then DecryptString(header->value) themselves.
	//     Lua's Engine.Localize (60-bit 0xF9F1239CFD921FE, native 0x7FF71E938FB0 -> LuaNative_Localize
	//     0x7FF72534C140) is one of them.
	//   - `if ((*s & 0xC0) != 0x80) return s;` is the first statement. Otherwise it decrypts IN PLACE
	//     (the first byte is overwritten), so every later call on the same string is that early return.
	//     Plain text is therefore passed back untouched, which is what makes a substituted string safe.
	//   - No Arxan caller check: the prologue is plain register saves, and the only call before the
	//     body is a recursive spinlock (0x7FF726887CE0). RVA 0xC990AE0.
	//   - 2026-10-07 (the first SERVER BROWSER build ended the process on the Zombies main screen): that
	//     native, LuaNative_Localize_Impl, returns a STRING argument untouched only when its first byte is
	//     0x15, the form of its own results (Engine.LocalizeHash formats them "\x15%s\x14", the string at
	//     0x7FF72A37D740). Any other string is hashed as an entry NAME, the same as a hash argument. For
	//     an entry that does not exist it calls LUI_ReportUiErrorCode_cand (0x7FF7222C2390: code 100004,
	//     "UI Error 100004 <text>", a dialog, and it returns), and then DB_FindXAssetHeader(0x1D) ends the
	//     process: sys_error 0x3580ada5 "<hash>,localizeentry". DB_FindXAssetHeader's per-type table
	//     (0x7FF727EC2604, index type - 2) makes a missing localize entry fatal, so the native's null
	//     checks after the call never run. Hence the rule in ui_scripts.hpp for text handed to a stock
	//     widget: "\021text\020", or a hash the game's own menus use.
	constexpr std::uintptr_t kDump_DecryptString            = 0x7FF729550AE0ULL;

	// Scr_ConstructMessageString(inst, out, firstParam, count): builds the payload of iprintln and
	// iprintlnbold (function and player-method forms) from the script args, into out = {u32 length;
	// char text[1024]}. One segment per arg, each led by a code byte: 0x12 string (an entity arg gives
	// its name + "^7"), 0x10 a localized key that exists, 0x11 a hash with none, 0x13 int, 0x15 float.
	// Verified 2026-09-26 from GScr_IPrintLnBold (0x7FF720819D20, builtin hash 0x061F222E) ->
	// Scr_ConstructMessageString_Fmt (0x7FF728400010, va("%c \"%s\"", 51, text)) -> this. Plain
	// prologue, no Arxan caller check. Detoured read-only (ScriptPrint_Mirror.cpp) so script prints
	// reach client.log and the overlay while their on-screen text shows empty. RVA 0xB8400B0.
	constexpr std::uintptr_t kDump_Scr_ConstructMessageString = 0x7FF7284000B0ULL;

	// Opening a LUI menu by name or hash (overlay "LUI menus" tab, client/game/lui_menu.cpp).
	// A replay of the engine's own `openmenu` console command, Cmd_OpenMenu_f (0x7FF726FC37E0):
	//     controller = CL_LocalClientToController(localClient);
	//     UI_SetUiActive(localClient, true);
	//     LUI_DispatchAddMenuEvent(LUI_GetRootName(controller), FNV1a63(lower(name)), -1, g_luiCtx);
	// except that we pass `controller` instead of -1: with -1 builders get controller = nil and any
	// menu with a button prompt raises (see SafeDispatch in lui_menu.cpp).
	// None of the four is Arxan caller-guarded: the guarded Lua leaves they call check THEIR return
	// address, which lands inside the image. The builder runs under lua_pcall inside the dispatch, so
	// a raising menu ends in LuiError_ReportFatal above, which is suppressed for our dispatch only.
	// g_luiCtx IS the lua_State (LUI_BeginEvent is `*(void**)ev = luiCtx`). Re-verified 2026-09-23.
	constexpr std::uintptr_t kDump_g_luiCtx                   = 0x7FF7305B2F38ULL;
	constexpr std::uintptr_t kDump_LUI_DispatchAddMenuEvent   = 0x7FF727997770ULL;
	constexpr std::uintptr_t kDump_LUI_GetRootName            = 0x7FF7258D7050ULL;
	constexpr std::uintptr_t kDump_CL_LocalClientToController = 0x7FF728C18BD0ULL;
	constexpr std::uintptr_t kDump_UI_SetUiActive             = 0x7FF7258E72E0ULL;

	// Opening a menu WITH PARAMS, the way the game's own buttons do:
	//     CoD.BaseUtility.OpenOverlay(self, menuName, controller, { _sessionMode = ..., ... })
	// addmenu cannot do this - the root's handler builds the menu as LUI.createMenu[m](controller),
	// so a menu that reads self:getSessionMode() (ZMUpgrades -> ZMUpgradeUtil cache keyed by mode)
	// raises "table index is nil". Measured 2026-09-23.
	// `self` must be an open MENU (an element whose __index chain has .openMenu): OpenOverlay walks
	// up the parents to one and calls :openOverlay on it. The UI root has none - passing it raised
	// "attempt to index a nil value" in baseutility. OpenLuiOverlay takes it from root.m_references.
	//   lua_createtable(L, narr, nrec) - the IDB used to call this lua_call. It is luaH_new + push.
	//   lua_setfield(L, idx, k)        - t[k] = top, pops. Caller-guarded like every lapi entry.
	//   LUI_ProtectedCall(L, nargs, nresults, errfunc) -> status - the setjmp-guarded wrapper
	//     LuiEvent_Dispatch uses around T9's modified lua_pcall (0x7FF729E3DB70). NOT caller-guarded
	//     itself. Driving the pcall only through this wrapper keeps the stack layout identical to
	//     the engine's own callers (func, args...), whatever T9 changed inside.
	constexpr std::uintptr_t kDump_lua_createtable    = 0x7FF729E3CF30ULL;
	constexpr std::uintptr_t kDump_lua_setfield       = 0x7FF729E3D460ULL;
	constexpr std::uintptr_t kDump_LUI_ProtectedCall  = 0x7FF722293FA0ULL;

	// Our own menu Lua (game/ui_scripts.cpp): cw-mod/ui_scripts/*.lua run as SOURCE in the LUI state.
	// Verified 2026-09-26:
	//   lua_load(L, reader, data, chunkname) -> status: caller-guarded, straight into
	//     luaD_protectedparser(L, reader, data, chunkname, mode = 0) (0x7FF729E48B70) -> lj_cpparser
	//     (0x7FF729E49330), which takes bytecode (lj_bcread) OR text (lj_parse 0x7FF729E5D720): the full
	//     LuaJIT 2.1 parser is in the exe. LuaFile_LoadAsset (0x7FF729642580) is the engine's only caller.
	//     Thunked (4 args, the thunk's limit).
	//   T9's lexer (0x7FF729E552B0) adds a hash literal: @"text" (token 290) and @name (291) become a
	//     hashed name (type "xhash", tag -6), hashed by the callback at global_State+816 ("xhashcreatefn",
	//     uint64 fn(const char*, uint32 len)). The lexer ASSERTS it is set (CRT _wassert = abort); retail
	//     chunks are all bytecode, so nothing guarantees it is. Hashed names are interned by their exact
	//     64-bit value (0x7FF729E53730: buckets at G+16, mask G+24, node {next, u8 type 5 @+8, u64 @+16}),
	//     while the Lua dump prints only the low 60 bits. tostring() of one is "xhash:0x%016llx"
	//     (lj_strfmt_obj 0x7FF729E50E30; G+832 is an optional reverse lookup, null in retail).
	constexpr std::uintptr_t kDump_lua_load           = 0x7FF729E48AE0ULL;

	// --- Battle.net fatal-error reporters (the BLZBNTBGS wall in online session mode) -------------
	// Booting with networkMode == 2 makes the client bring the Battle.net/BGS layer up for real, and
	// with no reachable Battle.net it answers with a BGS error. That error is NOT a soft failure: it
	// is formatted as "BLZBNTBGS%08X" (aBlzbntbgs08x @ 0x7FF72A3DCBC8, via BnetError_FormatMessage
	// 0x7FF7297A5960) and handed to Com_Error at level 1024. Level 1024 falls through Com_Error's
	// dispatch to `a3 |= 2` and then the full shutdown chain, which is why the dialog cannot be
	// dismissed and the process exits to desktop.
	//
	// There are TWO reporters, and the difference matters:
	//   0x7FF7296B7CF0  guarded by g_bnetEverSignedInLatch — "we HAD a session and lost it".
	//   0x7FF7296B7C70  UNGUARDED — fires on any BGS error at all.
	// The latch is sticky (LiveFirstParty_Frame 0x7FF7296B5F00 ORs FirstParty_IsSignedIn_State3 into
	// it every tick and never clears it) and is never set on a launch that never reached Battle.net,
	// so the fatal we actually hit comes from the UNGUARDED one. Both are reached only through
	// callback slots (0x7FF7377338AC / 0x7FF7377338B8), never a direct call, so there is no call site
	// to gate — the functions themselves are the intervention point.
	//
	// Detoured to log-and-return, and ONLY while an online-mode test boot is active: outside that,
	// a genuine Battle.net failure should still be fatal exactly as it was. RVA 0x4AF7C70 / 0x4AF7CF0.
	constexpr std::uintptr_t kDump_BnetError_ReportFatalUnguarded = 0x7FF7296B7C70ULL;
	constexpr std::uintptr_t kDump_BnetError_ReportFatalIfSignedIn = 0x7FF7296B7CF0ULL;
	// g_bnetEverSignedInLatch — read-only here, purely so the diagnostic can say which reporter could
	// have fired. RVA 0x25AFF272.
	constexpr std::uintptr_t kDump_g_bnetEverSignedInLatch  = 0x7FF7371BF272ULL;

	// --- First-party error state: the ACTUAL source of the BLZBNTBGS dialog ------------------------
	// Suppressing the two Com_Error reporters above was not enough, and the reason is that the dialog
	// is not a Com_Error at all. Lua_FirstParty_GetErrorMessage (0x7FF71E91EF40) is a lua_CFunction
	// that pushes the formatted "BLZBNTBGS%08X" text for LUI:
	//     Lua_FirstParty_GetErrorMessage -> FirstParty_PushErrorMessageToLua (0x7FF72648AFA0)
	//       -> FirstParty_GetErrorMessage (0x7FF7296B6460) -> BnetError_FormatMessage(g_firstPartyManager + 64)
	// So the popup is a LUI menu QUERYING the stored error, which is why it also reappears on every
	// menu change: each new menu asks again, sees the error state still set, and re-raises.
	//
	// The state lives in the first-party object: +56 state (3 = signed in, 4 = STATE_ERROR), +60
	// has-error flag, +64 the BGS code. FirstParty_StateTick (0x7FF7297DE3A0) switches on +56 and
	// routes 2/3/4 to FirstParty_NotifyStateListeners (0x7FF7296BF680), which fans the state out to
	// every listener — that is how one error reaches both the LUI popup and the Com_Error reporters.
	//
	// Exactly two functions can put it in state 4, so they are the choke point (the 12 individual
	// SetError call sites are not worth gating one by one):
	constexpr std::uintptr_t kDump_FirstParty_SetError      = 0x7FF7297DF340ULL; // (obj, code): +60=1, +64=code, +56=4
	constexpr std::uintptr_t kDump_FirstParty_SetErrorState = 0x7FF7297DE380ULL; // (obj): +56=4 only, no code
	// ...and a third, reached only through the BGS callback slot 0x7FF7377368A0: the "disconnected"
	// callback. When the connection drops mid-sign-in (+56 == 2) it stores the error itself (+60 = 1,
	// +64 = code + 1000000, +56 = 4), notifies, and then always leaves +56 = 5 and +61 = 1 (disconnected).
	// This one fires on EVERY online + backend boot (BLZBNTBGS000003EA, raw 1002, about 10 s in, when
	// nodw goes false). director_lan never asked about it. The title screen polls +60 every 400 ms
	// (Engine[0x2FFC1C70C8D5B359], lua_dec 5bca1ea30c053ddd f272) and raises the dialog.
	constexpr std::uintptr_t kDump_FirstParty_OnBgsDisconnected = 0x7FF7297DF2E0ULL; // (obj, ?, const u32* bgsCode)

	// ...and neither of the above is what actually put BLZBNTBGS000003EA on screen. Measured: with all
	// four detoured, the log showed the latched reporter suppressed AND the popup still appeared, with
	// the two state writers never firing at all. Tracing the dialog properly:
	//
	// ErrorQueue_Push(int level, const char* msg, char flag) is the ONE place the "ERROR / EXIT TO
	// DESKTOP" popup comes from. It appends {flag, level, text} to a 4-slot ring (1032-byte entries:
	// +0 flag, +4 level, +8 text) at g_errorQueue and bumps g_errorQueueCount; LUI drains the queue and
	// renders it. It returns 1 = queued, 0 = refused (ring full, or its gating dvar off) — and EVERY
	// caller treats 0 as "you display it then", falling through to Com_Error(level, ...) or to
	// ErrorQueue_StorePending. Level 1024 is the Battle.net/BGS class.
	//
	// THREE independent producers push level 1024, only one of which is a BnetError reporter:
	//   BnetError_ReportFatal_IfEverSignedIn  — hooked already
	//   LiveUser_HandleSignOut  0x7FF729752990 — NOT an error path at all, a sign-out path. It tears
	//     the LiveUser object down, asks LiveUser_BuildSignOutErrorMessage for a reason, and pushes
	//     whatever it gets. It never goes near FirstParty_SetError, which is exactly why hooking the
	//     state writers changed nothing. THIS is the one that got us.
	//
	// And LiveUser_BuildSignOutErrorMessage (0x7FF729753600) is the whole online/offline asymmetry:
	//     if (signinState == 1 || signinState == 2 && !Com_SessionMode_IsOnline()) return 0;
	// Offline, a sign-out with signinState 2 returns "no message" and the sign-out is silent. Online,
	// the same sign-out falls through, produces a message, and becomes an undismissable fatal. Setting
	// networkMode 2 is literally what converts a routine silent sign-out into the wall.
	//
	// So the correct choke point is the queue, not any producer: drop level 1024 there and return 1
	// (claiming we displayed it) and no caller falls through to Com_Error or the pending store.
	constexpr std::uintptr_t kDump_ErrorQueue_Push          = 0x7FF722349D60ULL;
	constexpr int            kErrorLevel_BattleNet          = 1024;

	// ...and that was still not it. Measured again: ErrorQueue_Push hooked and armed, and the hook
	// NEVER FIRED once, while the dialog appeared exactly as before. Three wrong choke points in a
	// row, all because each was found by reasoning backwards from the message text instead of forwards
	// from who is actually running.
	//
	// The real producer is the tail call of LiveFirstParty_Frame:
	//
	//   LiveFirstParty_Frame  (0x7FF7296B5F00, gated on !Dvar_GetBool(nodw) && byte_7FF7371BF1FF)
	//     -> if Dvar_GetBool(0x7FF733D20160) && LiveUser_IsSignedIn(0) && ...4 more predicates
	//       -> LiveUser_ForceSignOutAndFatal()
	//
	// It is a "you must stay signed in" watchdog and a dead end by construction:
	//   1. LiveUser_HandleSignOut(i) for both controllers
	//   2. forces loginState (+5768) to 9, then 10
	//   3. builds a localised message from the hash in qword_7FF72AF57F28
	//   4. sets g_pendingErrorLevel = 1024 / g_pendingErrorMessage = msg
	//   5. calls Com_Error(file, 0, 0x400 /* = 1024 */, msg) DIRECTLY
	//
	// Step 5 is the whole story: it never goes near ErrorQueue_Push and never goes near either
	// BnetError reporter, which is exactly why five detours changed nothing.
	constexpr std::uintptr_t kDump_LiveUser_ForceSignOutAndFatal = 0x7FF727F3A320ULL;

	// --- B5: why the marketplace inventory never loads (read-only; EntitlementGates::Tick) ---------
	// Engine.HasEntitlement -> Entitlement_IsOwned counts a product as owned only if it has quantity in
	// the per-controller marketplace inventory. Inventory_Frame (0x7FF727F1D5E0, end of
	// LiveUser_UpdateSigninState) fetches that inventory only once these all hold:
	//   PubVars state == 4          publisher variables (lobby svc 3 msgType 95) fetched
	//   both content slots == 2     OnlineContent_LoaderFrame finished (installed ff groups)
	//   MtxSync state in {2,3}      Battle.net 'ZEUS' token -> marketplace sync, in flight or done
	//   + LiveUser+5776 != 0, LiveStorage_AreOnlineDataMapsReady(c,2) (not read here)
	constexpr std::uintptr_t kDump_g_pubVarsState          = 0x7FF73407BAF4ULL; // 2 fetching, 3 failed/retry, 4 ready
	constexpr std::uintptr_t kDump_g_onlineContentSlots    = 0x7FF72FFFAD80ULL; // int[2]: 0 idle, 1 loading, 2 loaded
	constexpr std::uintptr_t kDump_g_mtxSyncState          = 0x7FF72B173420ULL; // int[2]: 0 idle, 1 waiting Bnet token, 2 sync sent, 3 done
	constexpr std::uintptr_t kDump_g_inventory             = 0x7FF733B79520ULL; // 801056 bytes per controller
	constexpr std::ptrdiff_t kInventory_LoadedFlag         = 944;               // byte, set when state reaches 4
	constexpr std::ptrdiff_t kInventory_ItemCount          = 400948;            // int
	constexpr std::ptrdiff_t kInventory_State              = 400956;            // 0 never reset, 1 request, 2 in flight, 3 next page, 4 loaded, 5 retry
	constexpr std::ptrdiff_t kLiveUser_Field5776           = 5776;              // qword, Inventory_Frame requires != 0

	// The better lever, now that the producer is known. LiveFirstParty_Frame's ENTIRE body — watchdog
	// included — is gated on `!Dvar_GetBool(nodw)`. Forcing nodw true before the frontend comes up
	// means the watchdog never runs, rather than running and having its output swallowed. main.cpp
	// already forces nodw=true when DwBackend is off; it just does it from a thread that waits on
	// Scr_Initialized, which lands ~6 seconds too late (measured: error at 13:48:14, nodw at 13:48:17).
	// Note this does NOT touch the session network mode, so the online frontends still get built.

	// --- B3: the LPC publisher-object list (read-only diagnostics) -------------------------------
	// Lpc_SyncFrame (0x7FF727B36860) asks objectstore for category tu<N>_<buildId> through the
	// lobby-tunnelled HTTP task. The reply reaches BdRemoteHttpTask_FinishRow, which calls the
	// resource's parser (PublisherObjectsResource_ParseResponse, which calls ObjectMetadata_ParseJson
	// once per object) and sets task state 2 (done) or 3 (failed). Done runs Lpc_WriteManifest,
	// failed runs Lpc_OnListFailed, which re-arms a 15 s retry. Boot 2026-09-16 22:56: the server
	// answered twice 15 s apart and .manifest was never written, so these five name the failing step.
	constexpr std::uintptr_t kDump_BdRemoteHttpTask_FinishRow         = 0x7FF729DDFDA0ULL;
	// The parser the LPC list actually uses (ownerType 1), NOT PublisherObjectsResource_ParseResponse
	// (0x7FF729D90170, ownerType 3), which never ran. It returns false unless nextPageToken is null or a
	// string, so a reply without the key fails after every object parsed (boot 2026-09-16 23:06).
	constexpr std::uintptr_t kDump_PublisherObjectsResource_Parse     = 0x7FF729D8B130ULL;
	constexpr std::uintptr_t kDump_ObjectMetadata_ParseJson           = 0x7FF729D70BD0ULL;
	constexpr std::uintptr_t kDump_Lpc_WriteManifest                  = 0x7FF727B360A0ULL;
	constexpr std::uintptr_t kDump_Lpc_OnListFailed                   = 0x7FF727B36070ULL;
	// BdLobbyMsg_Ctor(msg, msgType, serviceId, maxSize, bufSize): every outgoing lobby request is built
	// here. Census target: boot 2026-09-16 23:20 sent 'service 1 / msgType 8' (one UInt64 = 1) every
	// frame once the content slots reached 2,2, and its call sites are Arxan-obfuscated.
	// Boot 2026-09-17 19:10: hooking the Ctor caught msgTypes 38/255/27/67 but NOT service 1 or the
	// StructData requests, which build their header elsewhere. Every header goes through
	// BdLobbyMsg_WriteHeader(buf, msgType, serviceId) (8 callers incl. the Ctor), so the census moved there.
	constexpr std::uintptr_t kDump_BdLobbyMsg_WriteHeader             = 0x7FF729D98EC0ULL;

	// --- B5: why MtxSync never leaves 0 (read-only transcript, MtxSync_Transcript.cpp) -------------
	// Boot 2026-09-17 19:19: pubVars 4, content slots 2,2, service-1 blob answered, yet mtxSync stayed
	// 0 for a minute. LiveUser_UpdateSigninState starts it only when ShouldStart holds: dvar
	// off_7FF7371D6B18, IsOnlineReady, PubVars ready, state 0, backoff timer (unk_7FF72B173390 + 36*c).
	// RequestBnetTokenZEUS additionally skips on nodw. Inventory_Frame reaches IsDoneOrInFlight only
	// after its own gates (dvar, +5776, AreOnlineDataMapsReady, not LAN/offline), so a silent
	// IsDoneOrInFlight names those instead.
	constexpr std::uintptr_t kDump_MtxSync_ShouldStart                = 0x7FF71E74DE40ULL;
	constexpr std::uintptr_t kDump_MtxSync_RequestBnetTokenZEUS       = 0x7FF71E7506C0ULL;
	constexpr std::uintptr_t kDump_MtxSync_OnBnetToken                = 0x7FF71E750600ULL;
	constexpr std::uintptr_t kDump_MtxSync_IsDoneOrInFlight           = 0x7FF71E74DCB0ULL;
	constexpr std::uintptr_t kDump_g_mtxSyncBackoff                   = 0x7FF72B173390ULL; // 36 bytes per controller
	// Boot 2026-09-17 19:31: ShouldStart never ran, so LiveUser_UpdateSigninState returns before it.
	// Presence is already answered by the B1 detour; the other gates on that block are:
	//   LiveUser+5760 byte != 0 (active flag), LiveUser+5764 == 2 (signinState),
	//   !LiveUser_IsGuestLike_Flag20: client index = g_controllerClientMap lookup (15-dword rows, key
	//   at +4, value at +0), then g_clientFlags[2084*4*idx] & 0x20 (ignored when byte_7FF72CCC2719 + dvar).
	constexpr std::ptrdiff_t kLiveUser_ActiveFlag5760                 = 5760;
	constexpr std::ptrdiff_t kLiveUser_SigninState                    = 5764;
	constexpr std::uintptr_t kDump_g_controllerClientMap              = 0x7FF7356092C4ULL;
	constexpr std::uintptr_t kDump_g_clientFlags                      = 0x7FF72CD700E0ULL;

	// --- B6: the online frontend's lobby gate (read-only transcript, DwFetch_Transcript.cpp) -------
	// Lua's Lobby.ProcessNavigate.BeginLivePlay opens the online menu (and so builds the lobby the mode
	// tiles bind to) only when Engine.IsDemonwareFetchingDone && Engine.AreLocalFilesReady; otherwise
	// it shows the "connecting to online services" overlay. IsDemonwareFetchingDone (native hash
	// 0x603CD0351DA0D371, registered at 0x7FF71E914788) -> 0x7FF7262ACBE0 -> this function:
	//   DwFetch_GetStatus(controller, u32* got) -> (required & *got) == required
	// It ORs one bit per ready subsystem into *got. required = 0x17337FA, or 0x17B37FA while MtxSync is
	// not done (bit 0x80000 = "MtxSync not done", so MtxSync can never block it). Bit 0x10 and 0x20000
	// are waived under conditions. The engine renders the same mask as 25 letters (A = bit 0, '-' =
	// clear) in its connection-info string (0x7FF7262AD670 case 1).
	constexpr std::uintptr_t kDump_DwFetch_GetStatus                  = 0x7FF7262A4BA0ULL;
	// State behind the required bits, dumped by the transcript for whichever bits are clear (plus
	// kDump_g_pubVarsState and kDump_g_onlineContentSlots above; bits H/J/X/Y read slots 0/1/9/10).
	constexpr std::uintptr_t kDump_g_dwFetchFlag1000                 = 0x7FF72E7FA64FULL; // byte, bit 0x1000
	constexpr std::uintptr_t kDump_g_dwFetchState2000                 = 0x7FF73295357CULL; // int >= 4, bit 0x2000
	constexpr std::uintptr_t kDump_g_dwFetchState400000               = 0x7FF73082D838ULL; // int 2..3, bit 0x400000
	constexpr std::uintptr_t kDump_g_dwFetchFlag200000                = 0x7FF72EF33444ULL; // byte (if a dvar >= 2)
	constexpr std::uintptr_t kDump_g_dwFetchCtrl20000                 = 0x7FF733B79520ULL; // 801056/ctrl: +944 byte, +900 int == 3

	// --- B6 lobby waiver (DwFetch_LobbyWaiver.cpp; boot 2026-09-24 06:24) --------------------------
	// Measured: everything reached its bit except G, N, R, W, Y. Read from IDA the same day:
	//   G 0x40      slot 1 (core_ffotd, flag 0x800) listed in the LPC manifest. We serve no ffotd.
	//   Y 0x1000000 slot 10 = ingamestore_<lang>.json, fetched only after G. The in-game store.
	//   R 0x20000   the marketplace inventory loaded (needs the Battle.net ZEUS token, B5).
	//   N 0x2000    dedicated-server QoS (fed by Lua Lobby.MatchmakingAsync.EventQoSHosts).
	//   W 0x400000  SocketRouter relay bind (Lua PumpLocalClients -> SocketRouterBindToRelay).
	// All five are public matchmaking / commerce. The waiver answers true for these five ONLY, and only
	// to Lua (the call from Lua_IsDemonwareFetchingDone_Impl); the engine's own callers keep the truth.
	// DwFetch_IsDone(controller) is a wrapper that calls DwFetch_GetStatus(c, &got, &zeroed16).
	constexpr std::uintptr_t kDump_DwFetch_IsDone                     = 0x7FF7262ACBE0ULL;
	constexpr std::uintptr_t kDump_Lua_IsDemonwareFetchingDone_Impl   = 0x7FF72534AE20ULL;
	constexpr std::size_t    kLua_IsDemonwareFetchingDone_ImplSize    = 0x3D;
	// "Do you own the game" (LuaUtils.IsTrial = native 0x33698526482CB1F8 -> 0x7FF726933CF0 -> this).
	//   if (nodw) return 0;                                  <- why offline never shows the upsell
	//   if (!firstPartyMgr.licenses) return 1;               <- studio auth: no Battle.net licenses
	//   if (dvar 0x7FF733DDA4E0 || FirstParty_IsFlagged()) return 0;
	//   return !licenses.contains(0xCDE9);                   <- the full-game product id
	// A trial user gets the "you don't own the game" overlay on the mode tiles (IsModeAllowedForTrialUsers).
	constexpr std::uintptr_t kDump_LiveUser_IsTrial                   = 0x7FF728151A00ULL;

	// --- B6 padlocks after the trial fix (LuaPrint_Transcript.cpp; read 2026-09-24) ---------------
	// With IsTrial false the tiles stay padlocked and a click only plays a sound. The mode tile's own
	// action, CoD.LobbyUtility[0x7CF8330D78EB9E5], does nothing unless the lobby process queue is
	// empty AND the current lobby menu is the online/LAN select menu, and the "mode unavailable"
	// branch returns early unless Engine[0x10EA2BE00F49480D](private lobby) is true. So both need the
	// type-1 lobby (the fork's "padlocks = missing lobby"). Which step of PressStart -> BeginLivePlay
	// -> LobbyVM.OnGoForward -> Lobby.Actions LobbyClientStart stops online is not knowable
	// statically: the Lua narrates each one through Engine.PrintInfo, which retail compiles out.
	// Each print native checks its args (luaL_checkinteger(L,1) channel, lua_tolstring(L,2) text) and
	// tail-jumps to a 3-byte `xor eax, eax; ret` stub with live code right after it, so the stubs
	// cannot take a 5-byte detour. The WRAPPERS are hooked; they are unique to their native.
	constexpr std::uintptr_t kDump_LuaNative_PrintInfo                = 0x7FF71E93C050ULL; // native 0x28C5711DAACC99F4
	constexpr std::uintptr_t kDump_LuaNative_PrintWarning             = 0x7FF71E93C0C0ULL; // native 0x65DF86CF48135674
	constexpr std::uintptr_t kDump_LuaNative_PrintError               = 0x7FF71E93C000ULL; // native 0x4458FE92FEB39D4E
	// The 07:08 transcript: the lobby menu is director_lan (id 10), and the tile action accepts it
	// (GetLanSelectMenu), so the menu is not the click gate. The padlock is one of the datasource's
	// lock branches (5bca1ea30c053ddd f322), and three of them turn on runtime data:
	//   installing && !playable                 -> locked, action nil (silent click)
	//   playable && !GameModeAvailable(mode)    -> locked, action = "unavailable" dialog, which
	//                                              returns silently without the private lobby
	//   !installing && !playable                -> locked, action = the install popup
	// GameModeAvailable = mode dvar != 0 (ZM also !IsKoreaMinor). Ruled out by decompile: the
	// feature check's Engine[0x4BE6354DE2AFA1FC] and the content natives' Engine[0x72DA54CF5D6B7F02]
	// both push a constant false. The wrappers below are logged per (argument -> answer) in
	// LuaGate_Probe.cpp; all six share the print wrappers' hookable `push rbx; sub rsp,20h` prologue.
	constexpr std::uintptr_t kDump_LuaNative_ContentIsFullyInstalled  = 0x7FF71E933020ULL; // 0x5541E6CA5E529182 -> Content_IsModeFullyInstalled_cand
	constexpr std::uintptr_t kDump_LuaNative_ContentIsPlayable        = 0x7FF71E9330A0ULL; // 0x06CD1640D0F93036 -> Content_IsModePlayable_cand
	constexpr std::uintptr_t kDump_LuaNative_ContentIsInstalling      = 0x7FF71E933050ULL; // 0x5242D2DE58485C35 -> Content_IsModeInstalling_cand
	constexpr std::uintptr_t kDump_LuaNative_GetDvarInt               = 0x7FF71E9219A0ULL; // 0x622EAAB59AA27E9B (CoDShared.IsIntDvarNonZero)
	constexpr std::uintptr_t kDump_LuaNative_IsKoreaMinor             = 0x7FF71E9315E0ULL; // 0x45405A6484A88367 -> FirstParty_IsKoreaMinor_cand
	constexpr std::uintptr_t kDump_LuaNative_IsLobbySlotLive          = 0x7FF71E931700ULL; // 0x10EA2BE00F49480D -> Session_IsSlotActiveAndState2_cand
	// The four mode-available dvars (sub_7FF726A8C070 registers them, default 1), 60-bit as Lua sees them.
	constexpr std::uint64_t  kModeDvar_ZM = 0x00D9394548AC1DBAULL, kModeDvar_MP = 0x0639D4DB415CDB92ULL,
	                         kModeDvar_WZ = 0x0FA38645FB7CFB66ULL, kModeDvar_CP = 0x04F97324DFFF1C0CULL;
	// Bool, registered default TRUE at 0x7FF727DB1B11 (dvar_zmStartUsesDedicated_cand), Lua 60-bit
	// 0x720058BE2417E91. The ZM Start handler (lua_dec 2cbd14a5e53ed76b f269) goes to
	// director_online_public (dedicated servers) when it is true, else director_online_private
	// (a game lobby hosted here). No native reader: its only xref is the registration.
	constexpr std::uint64_t  kDvar_ZmStartUsesDedicated = 0x7720058BE2417E91ULL;
	// The padlock is `not privateClient.isHost` (no party). The party is made only by the director ->
	// select-menu step that PressStart -> Lobby.ProcessNavigate.BeginLivePlay runs (lua_dec
	// 1b5c8f89e079db53). The 07:44 boot reached "PressStart - BeginLivePlay" and then looped it about
	// 4x/s for 30 s. The title screen's 400 ms sign-in timer calls start while IsSignedInToLive, and
	// every early return in BeginLivePlay is silent. These are the natives those returns test, in
	// order; the probe logs each (argument -> answer) the same way. Same `push rbx; sub rsp,20h`
	// prologue as above.
	constexpr std::uintptr_t kDump_LuaNative_ConnectionInfo       = 0x7FF71E92D690ULL; // 0x5451F40A2FEDDF88 -> table, .connectionState
	constexpr std::uintptr_t kDump_LuaNative_ConnErrorGate        = 0x7FF71E92F700ULL; // 0x545A16E755DB1D5C (read when state 263/264)
	constexpr std::uintptr_t kDump_LuaNative_GetPlayerQueueInfo   = 0x7FF71E928B20ULL; // "GetPlayerQueueInfo" -> table (closed/queued/disabled)
	constexpr std::uintptr_t kDump_LuaNative_QueueGate            = 0x7FF71E931830ULL; // 0x0F62AEC4075B0105
	constexpr std::uintptr_t kDump_LuaNative_IsPlayerQueued       = 0x7FF71E9325F0ULL; // "IsPlayerQueued"
	constexpr std::uintptr_t kDump_LuaNative_UniversalAccountA    = 0x7FF71E933940ULL; // 0x7CE050ECDC5CFD1D
	constexpr std::uintptr_t kDump_LuaNative_UniversalAccountB    = 0x7FF71E933920ULL; // 0x21E94361FF6923EA
	constexpr std::uintptr_t kDump_LuaNative_AreLocalFilesReady   = 0x7FF71E917CC0ULL; // "AreLocalFilesReady"
	constexpr std::uintptr_t kDump_LuaNative_ForceOfflineGate     = 0x7FF71E9330D0ULL; // 0x3573048F8D3B4E25 (CoDShared.ForceOffline)
	constexpr std::uintptr_t kDump_LuaNative_IsSignedInToLive     = 0x7FF71E933150ULL; // "IsSignedInToLive" (the title timer's test)
	constexpr std::uintptr_t kDump_LuaNative_FirstPartyHasError   = 0x7FF71E92E8A0ULL; // 0x2FFC1C70C8D5B359 -> g_firstPartyManager+60
	constexpr std::uintptr_t kDump_LuaNative_FirstPartyDisconnected = 0x7FF71E92E800ULL; // 0x34D07729ED48730D -> g_firstPartyManager+61
	constexpr std::uintptr_t kDump_LuaNative_GetLobbyNav          = 0x7FF71E926230ULL; // 0x69882F293C327557
	// The 20:16 boot: every gate above passes once login completes, and BeginLivePlay then stops at
	// `IsDemonwareFetchingDone && AreLocalFilesReady` (the waiver answers the first, the second is
	// false). The else branch opens the "Connecting to online services" overlay, and the title timer
	// re-runs start, so the overlay flashes. The native -> PlayerData_AreLocalFilesReady(ctrl):
	//   LiveUser[ctrl]+5760 != 0
	//   && LiveStorage_AreOnlineDataMapsReady(ctrl, 1)          (true for any mode but 2)
	//   && PlayerData_AreDataMapGroupReady(ctrl, 1)            (mode-1 rows of the group table)
	//   && IsBufferReady(primaryController, 23, -1)
	//   && for id 1..45 with storageLocation != 1 (dwuser), in scope for ctrl, def+70 clear:
	//        IsBufferReady(ctrl, id, v) for every v < def+44
	// "Local" means not dwuser. Our dwuser -> hdd rewrite pulls those 18 maps into the last loop.
	// LuaGate_Probe.cpp re-evaluates all of it from memory whenever the native answers false.
	constexpr std::uintptr_t kDump_PlayerData_AreLocalFilesReady  = 0x7FF7262A31D0ULL;
	constexpr std::uintptr_t kDump_g_playerDataInitialized        = 0x7FF730351A06ULL; // byte, IsBufferReady's first test
	constexpr std::uintptr_t kDump_g_playerDataMapGroups          = 0x7FF72ADC7BB0ULL; // 10 x 24: +0 dataMapId, +4 mode (1 or 2)
	constexpr int            kPdMapGroupCount                     = 10;
	constexpr std::size_t    kPdMapGroupStride                    = 24;
	// sub_7FF728C18B00: controller of the primary local client, or -1. Entry = table + 60*idx; valid
	// only when entry+4 == idx, and then entry+8 is the controller.
	constexpr std::uintptr_t kDump_g_primaryLocalClient           = 0x7FF72AF2E788ULL; // int idx
	constexpr std::uintptr_t kDump_g_localClientControllers       = 0x7FF7356092C0ULL; // 60-byte entries
	constexpr int            kPdPrimaryCheckMapId                 = 23;
	constexpr std::ptrdiff_t kLiveUser_Xuid                       = 31824; // u64, what IsBufferReady compares store+0 against
	constexpr std::size_t    kPdDef_VersionCount                  = 44;    // int, versions the loops walk
	constexpr std::size_t    kPdDef_SkipLocalCheck                = 70;    // byte, excludes the map from both loops
	// Session_IsSlotActive(type, idx<3) = slot+68 > 0. Engine[0x10EA2BE00F49480D] (the tiles' lobby
	// test) additionally wants slot+256 == 2. Type 1 is the Lua lobby; type-0 slot 0 is the one
	// LobbyVM_RequestStartSession takes its descriptor from.
	constexpr std::uintptr_t kDump_g_sessionSlots_type0               = 0x7FF73007ABD0ULL; // 3 x 237728
	constexpr std::uintptr_t kDump_g_sessionSlots_typeN               = 0x7FF730043A50ULL; // 3 x 75216
	constexpr std::size_t    kSessionSlot_Type0Stride                 = 237728;
	constexpr std::size_t    kSessionSlot_TypeNStride                 = 75216;
	constexpr std::size_t    kSessionSlot_Active                      = 68;
	constexpr std::size_t    kSessionSlot_State256                    = 256;
	// The other two tile locks that set action = nil, both read off FirstParty_* the same day:
	//   Korean PC-bang (IGR), FirstParty_IsKoreanIGR_cand 0x7FF7296B7AE0: every non-MP tile.
	//     = !nodw && presenceOk && (dvar || *(u8*)(*(firstPartyMgr + 48) + 49))
	//   Korea && age < 18 (FirstParty_IsKoreaMinor_cand 0x7FF7296B78B0): every tile. Not ours.
	constexpr std::size_t    kFirstPartyMgr_Account                   = 48;
	constexpr std::size_t    kFirstPartyAccount_IgrFlag               = 49;

	// --- ZM progression outside LIVE (zm_progression.cpp; decompiled 2026-09-23) -----------------
	// The match-stats gate. `forceDvar || (mp/wz ? eligible : zm && networkMode == 2 && matchType != 1)`.
	// 11 callers: G_AddPlayerRankXp (all rank XP), GScr_AddRankXp (weapon XP), GScr_AddWeaponStat,
	// the weapon-used natives, LiveStats_RecordMatchStatsIfEnabled, BuildStatsTransferSlot (copy the
	// player's playerdata into the prematch slot vs a blank DDL instance), BeginStatsTransfer (LIVE
	// only), CommitStatsTransfer and CommitStatsTransferAndRecap. False in ZM LAN/offline.
	constexpr std::uintptr_t kDump_LiveStorage_AreMatchStatsEnabled   = 0x7FF7275C3DA0ULL;
	// Its sibling, same force dvar plus a kill switch, for the 7 GSC stat-write helpers
	// (sub_7FF722A093F0 .. sub_7FF722A0E1E0): `(force || modecheck) && !dvar(off_7FF72CCC1308)`.
	constexpr std::uintptr_t kDump_GScr_AreStatWritesEnabled          = 0x7FF722A0E2D0ULL;
	// LiveStorage_BeginStatsTransfer(ctrl) -> bool, from CL_ConnectionlessPacket on connect. Builds
	// the transfer record g_statsTransfer[528*ctrl] (source 0 = gametype map, 1 = primary map 5) and
	// sets record+492 (valid), +504 gameMode, +508 networkMode, +520 xuid. A 0 return is fatal to
	// the connect: this is where "West 683 Winning Clover" came from on 2026-07-28, when the force
	// dvar made BuildStatsTransferSlot take its Ddl_CopyInstanceToInstance branch and the copy failed.
	constexpr std::uintptr_t kDump_LiveStorage_BeginStatsTransfer     = 0x7FF72531A310ULL;
	// LiveStorage_CommitStatsTransfer(ctrl, source, checksum, final): copies the server's final stats
	// instance for `source` into the playerdata buffer and queues its write. Silent returns on: already
	// committed (+498+src), invalid record, gate false, buffer player_xuid != LiveUser XUID (checked
	// BEFORE the copy), buffer not ready, copy failed; the write is deferred while Live_DwTokensRequired
	// and the signature (+501+src) has not arrived. The copy is WHOLE-BUFFER, not a delta.
	constexpr std::uintptr_t kDump_LiveStorage_CommitStatsTransfer    = 0x7FF725320000ULL;
	// G_AddPlayerRankXp(clientNum, xp, xpType, eventHash): every GSC XP source lands here. Gates:
	// !scr_disableSetDStat, AreMatchStatsEnabled, G_AreStatWritesAllowed_cand, SV_IsClientStatsSlotReady.
	// Writes svClient(70864*n)+54188 (rankxp) and the server stats instance.
	constexpr std::uintptr_t kDump_G_AddPlayerRankXp                  = 0x7FF723BE38F0ULL;
	constexpr std::size_t    kSvClientStride                          = 70864;
	constexpr std::size_t    kSvClient_RankXp                         = 54188;

	// Called through ArxanCall thunks (read-mostly; the one writer is Ddl_SetUInt64 below).
	constexpr std::uintptr_t kDump_LiveUser_GetXuidIfSignedIn         = 0x7FF7296AB130ULL; // u64(int ctrl)
	constexpr std::uintptr_t kDump_PlayerData_GetBuffer               = 0x7FF7275C0DF0ULL; // inst*(int ctrl, unsigned id, int ver)
	constexpr std::uintptr_t kDump_PlayerData_IsBufferReady           = 0x7FF7275C1620ULL; // bool(int ctrl, unsigned id, int ver)
	constexpr std::uintptr_t kDump_LiveStorage_GetGametypeStatsMapId  = 0x7FF7275C3D40ULL; // unsigned(), 19 for ZM
	constexpr std::uintptr_t kDump_LiveStorage_GetPrimaryStatsMapId   = 0x7FF7275C3C50ULL; // unsigned(), 5 (25 in CP)
	constexpr std::uintptr_t kDump_StatsTransfer_IsValidForController = 0x7FF72531DAF0ULL; // bool(unsigned ctrl)
	constexpr std::uintptr_t kDump_Live_DwTokensRequired              = 0x7FF7266A9370ULL; // bool(), int dvar >= 1
	constexpr std::uintptr_t kDump_SV_IsClientStatsSlotReady          = 0x7FF723BD93D0ULL; // bool(short client, int source)
	constexpr std::uintptr_t kDump_Dvar_GetBool                       = 0x7FF71E628F50ULL; // bool(dvar*)
	// Ddl_CopyInstanceToInstance's integrity switch: when set, the SOURCE buffer's header must read
	// magic 0x7376, buildId 7 and a non-zero guid (the DDL root's own guid, root+24 — written by the
	// engine's header init, NOT a XUID). Slot holding the dvar_t*.
	constexpr std::uintptr_t kDump_dvarPtr_ddlCopyIntegrityCheck      = 0x7FF72E7FA088ULL;
	constexpr std::uintptr_t kDump_LiveStorage_GetStatsDdlRootForSource = 0x7FF7275C3740ULL; // root*(int source)
	// The DDL accessor chain PlayerData_ReadBufferPlayerXuid uses, plus the setter the engine's own
	// ownership claim (sub_7FF72531A030) uses to stamp player_xuid. State = 32 bytes:
	// {+0 u8 valid, +4 int index, +8 int -1, +16 member*, +24 root*}.
	constexpr std::uintptr_t kDump_Ddl_IsInstanceValid                = 0x7FF72967B200ULL; // bool(inst*)
	constexpr std::uintptr_t kDump_Ddl_InitRootState                  = 0x7FF72967B900ULL; // state*(state*, inst*)
	constexpr std::uintptr_t kDump_Ddl_MoveToMemberByHash             = 0x7FF72967C890ULL; // bool(in*, out*, const u64* hash)
	constexpr std::uintptr_t kDump_Ddl_GetUInt64                      = 0x7FF72967BBC0ULL; // u64(state*, inst*)
	constexpr std::uintptr_t kDump_Ddl_SetUInt64                      = 0x7FF72967D180ULL; // bool(state*, inst*, u64)
	// Header parse: out = {+0 headerless, +2 u16 magic, +4 u8 buildId, +8 u64 guid, ...} (32 bytes).
	constexpr std::uintptr_t kDump_Ddl_ParseHeader                    = 0x7FF729692160ULL; // out*(out*, buf*, char headerless)
	constexpr std::uint64_t  kDdlMember_PlayerXuid                    = 0x10CBB4E93D94E4F3ULL; // fnv "player_xuid"
	// g_statsTransfer: 528 bytes per controller. Offsets named from Begin/Build/Commit/IsValid.
	constexpr std::uintptr_t kDump_g_statsTransfer                    = 0x7FF72DE2E650ULL;
	constexpr std::size_t    kStatsXfer_Stride                        = 528;
	constexpr std::size_t    kStatsXfer_MapId                         = 480; // int[2], per source
	constexpr std::size_t    kStatsXfer_Valid                         = 492; // u8
	constexpr std::size_t    kStatsXfer_Committed                     = 498; // u8[2]
	constexpr std::size_t    kStatsXfer_SignatureIn                   = 501; // u8[2]
	constexpr std::size_t    kStatsXfer_GameMode                      = 504; // int
	constexpr std::size_t    kStatsXfer_NetworkMode                   = 508; // int
	constexpr std::size_t    kStatsXfer_Xuid                          = 520; // u64

	// --- The AE block: account XP / level outside LIVE (zm_progression.cpp; decompiled 2026-09-23) --
	// BOCW's player level is NOT zm_progression's rankxp. Every reader (frontend rank, AAR, the
	// server's XP counter) reads `ae_sync.progression.base_xp`, a DDL instance Demonware fills. It
	// lives at LiveUser+40024 -> +18328 and is only created on a DW sign-in
	// (LiveUser_AttachLiveStats 0x7FF7274B3690 <- LiveUser_UpdateSigninState when IsOnlineReady), so
	// offline/LAN it is null, the level reads 0, and:
	//   * SV_ClientStatsReady (0x7FF723C18230) seeds svClient+54188 from the client's source-2 (AE)
	//     blob only. No blob in LAN => every match starts at 0 and G_AddPlayerRankXp then writes that
	//     per-match total over PlayerStatsList.rankxp.statvalue: the save never accumulates.
	//   * The match-end recap (LiveStorage_CommitStatsTransferAndRecap) DOES run in LAN, because
	//     PlayerData_IsStorageLocationUsable(2) is false there and SerializeStatsTransfer pre-marks
	//     committed[2]. But its LUI call (LiveStorage_LuiAarRecap 0x7FF7253213C0) needs all six
	//     instances valid, and two are AE: record+176 (source-2 prematch, copied only when LIVE) and
	//     LiveStats_GetStatsSourceBlock (the LiveUser block, or record+416 behind two dvars). So the
	//     AAR Lua never gets its data and no XP screen opens.
	constexpr std::uintptr_t kDump_LiveStats_GetLiveUserStatsInstance = 0x7FF726A7D380ULL; // inst*(int ctrl); LiveUser+40024 -> +18328
	constexpr std::uintptr_t kDump_LiveStats_GetAeRootStateSlot       = 0x7FF726A7D3F0ULL; // state*(int ctrl); LiveUser+40024 -> +18392 (32 B)
	constexpr std::uintptr_t kDump_Lua_PushAeSyncBuffer               = 0x7FF726A7E500ULL; // (L, ctrl), Engine.GetAESyncBuffer's body; inlines the +40024 read
	constexpr std::uintptr_t kDump_LiveStats_GetXp                    = 0x7FF726A7D710ULL; // u32(inst*, u32 season=-1)
	constexpr std::uintptr_t kDump_LiveStats_SetXp                    = 0x7FF726A7F100ULL; // bool(inst*, u32 season=-1, int xp)
	constexpr std::uintptr_t kDump_LiveStats_SetAarModelFlag          = 0x7FF72531F590ULL; // (ctrl, member hash, bool) on model 0x749D1EE50313ACCA
	constexpr std::uintptr_t kDump_Rank_GetLevelForXp                 = 0x7FF727904DC0ULL; // int(int xp)
	// XP of the top rank (rank table +16). LiveStats_SetXp puts anything above it in xp[season]
	// instead of base_xp, and GetXp only adds xp[season] once base_xp has reached it.
	constexpr std::uintptr_t kDump_Rank_GetMaxXp                      = 0x7FF727904820ULL; // int()
	constexpr std::uintptr_t kDump_SV_GetClientStatsInstance          = 0x7FF723BD9720ULL; // inst*(short client, int source); svClient+53808+64*src
	constexpr std::uintptr_t kDump_Ddl_FindRootByHash                 = 0x7FF72967B6D0ULL; // root*(u64 hash, int)
	// (buf*, int size, root*, inst*, onChange, onChangeCtx, copyFrom). SEVEN arguments, so it can NOT
	// go through an ArxanCall thunk (register args only): the thunk shifts the frame by 0x30 and args
	// 5-7 read our stack. Arg 5 lands at inst+32, the change callback every DDL write calls; garbage
	// there = "executing <heap address>" on the next SetXp (the 2026-09-23 map-load crash). Arg 7
	// garbage faulted in Ddl_IsInstanceValid. It has no caller check (sub rsp / 6 movs / call), so
	// call it directly. Instance layout: +0 buf, +8 size, +16 root, +28 =1, +32 onChange, +40 ctx.
	constexpr std::uintptr_t kDump_Ddl_InitInstance                   = 0x7FF72967CA70ULL;
	constexpr std::uintptr_t kDump_Ddl_GetValue                       = 0x7FF72967BD50ULL; // u64(state*, inst*), any scalar type
	constexpr std::uintptr_t kDump_Ddl_PushStateToLua                 = 0x7FF72966E790ULL; // (L, state*, inst*) -> ddl userdata
	constexpr std::uint64_t  kDdlRoot_AeSync                          = 0x367E27B5078BB9EDULL; // LiveStats_CreateLiveUserStatsInstance
	constexpr int            kAeSyncBufferSize                        = 5120;  // ditto; the root is 0x99A0 bits
	constexpr std::size_t    kStatsXfer_AePrematchInst                = 176;   // source 2, +80*2+16
	constexpr std::size_t    kStatsXfer_AeCurrentInst                 = 416;   // source 2, +240+80*2+16
	constexpr unsigned int   kMapId_ZmProgression                     = 19;
	// PlayerStatsList.rankxp.statvalue (the path sub_7FF723C18100 builds for G_AddPlayerRankXp).
	constexpr std::uint64_t  kDdlMember_PlayerStatsList               = 0x1D59E8BFAC78A33BULL;
	constexpr std::uint64_t  kDdlMember_RankXp                        = 0x08928A12A20A9D67ULL;
	constexpr std::uint64_t  kDdlMember_StatValue                     = 0x03BF77799B56C06CULL;
	// The AAR waits on these three model flags (AARUtility 0xE8ED9434883E9C2) up to 10 s. LIVE sets
	// them from the AE service reply (sub_7FF726A81230); PreMatch clears them.
	constexpr std::uint64_t  kAarFlag_ProgressionReady                = 0x38F507EC98555AE6ULL;
	constexpr std::uint64_t  kAarFlag_Second                          = 0x64C1B98160F5CB89ULL;
	constexpr std::uint64_t  kAarFlag_LootContracts                   = 0x476A8B88C674B3E9ULL;

	// --- Gun XP and camo challenges outside LIVE (zm_progression.cpp; decompiled 2026-09-23) --------
	// BOCW gun levels are NOT zm_progression's ranked_item_stats (those stay 0 in every save). The
	// server's GScr_AddRankXp writes gun XP to ae_sync `progression.weapons[weapon].xp`
	// (LiveStats_PathProgressionWeaponXp 0x7FF7275C4D10), and the GSC stat helpers write
	// weapon_challenges / attachment_challenges (camos) to the same root, all on
	// SV_GetClientStatsInstance(client, 2): the server's copy of the client's AE blob.
	// SV_ImportClientStatsBlobs (0x7FF7275C0420) fills a slot only when its storage location is usable,
	// and location 2 is usable only when LIVE. So in LAN slot 2 is null and every write is dropped.
	// SV_ClientStatsReady (0x7FF723C18230) runs once per connect after the import; it reads slot 2 for
	// the XP seed and builds the server's per-weapon unlock tables (svClient+54364) from it. We fill
	// slot 2 just before it runs. Slot layout: state int[5] at +53788 (2..6 = usable, import sets 2),
	// 64-byte instance per source at +53808. Import passes onChange = the dirty-bit marker for the
	// client delta sync; our slot uses none (the client is this process, we copy at the commit).
	constexpr std::uintptr_t kDump_SV_ClientStatsReady                = 0x7FF723C18230ULL; // char(svClient*)
	constexpr std::size_t    kSvClient_StatsSlotState                 = 53788; // int[5]
	constexpr std::size_t    kSvClient_StatsInstances                 = 53808; // 64 B per source
	constexpr std::size_t    kSvClient_StatsInstanceStride            = 64;
	constexpr int            kStatsSource_Ae                          = 2;

	// --- The AE block on an online boot (zm_progression.cpp; decompiled 2026-10-07) -----------------
	// A Demonware sign-in makes the engine's own block (LiveStats_CreateLiveUserStatsInstance
	// 0x7FF726A7C600: Ddl_InitInstance, blank, no change callback), and LiveUser_RecreateAeSyncInstance_cand
	// (0x7FF7274B35C0, e.g. from OnlineContent_OnFfotdLoaded) makes it again, blank. Retail fills it from
	// the AE service reply (LiveStats_OnAeProgressionReply); the local backend has no AE service, so it
	// stays blank and the level reads 1 at every boot.
	// With networkMode 2 the source-2 slot is usable: BeginStatsTransfer copies the block into the
	// record (+176, +416), the server imports it as its slot 2, and the server's stat deltas are applied
	// straight back to the client's block during the match (CL_ApplyServerStatDelta_cand 0x7FF71EEED600
	// -> LiveStorage_GetCommitSourceInstance(ctrl, 2), which is the LiveUser block unless dvar
	// 0x7FF72E7F9838). So gun XP rises for the session and is lost at exit: CommitStatsTransfer only
	// marks source 2 committed, saving it is Demonware's job. Rank XP reaches it only behind dvar
	// 0x7FF72E7F97E0 (G_AddPlayerRankXp).
	// Measured 2026-10-07 17:32: an online Zombies private match passes both stats gates by itself
	// (session 0x20: gameMode 0, network 2, matchType 0), the engine's block is scrambled in memory, and
	// the server imports it as its slot 2 (AE xp 4047815). The match's gun XP reached the file (29 -> 35
	// non-zero bytes) and loaded again on the next boot.
	// The buffer can be scrambled in memory (dvar 0x7FF72E7FA078): an XOR pad over everything after the
	// header, made from each byte's own address and the key. The accessors read through it; a raw copy
	// has to unscramble first, the way Ddl_CopyInstanceToInstance does. Both take the instance's own
	// (buffer, size) or do nothing.
	constexpr std::uintptr_t kDump_Ddl_ScrambleInstanceBuffer         = 0x7FF729680420ULL; // (inst*, buf*, u32 size, u32 key)
	constexpr std::uintptr_t kDump_Ddl_UnscrambleInstanceBuffer       = 0x7FF7296804C0ULL; // (inst*, buf*, u32 size)
	constexpr std::size_t    kDdlInst_Size                            = 8;   // int
	constexpr std::size_t    kDdlInst_Root                            = 16;
	// The write mode. Ddl_InitInstance sets 1. Every setter ends in Ddl_WriteBits (0x7FF729680C50), which
	// returns 0 unless it is 1 (Ddl_IsInstanceWritable 0x7FF72967C390); the getters do not look at it.
	// LiveStorage_OnStatsMapRead_cand (0x7FF725321910), the read callback of every playerdata map, sets
	// 2 on maps 5, 8, 13 and 19 (the progression maps) after a read that did not fail. So a progression
	// map is read-only from the moment its file has loaded. The engine itself changes it only through the
	// commit's whole-buffer copy and through LiveStorage_ApplyStatDeltaMsg_cand (0x7FF72531FE60), which
	// sets 1, writes and puts the old value back.
	// Measured 2026-10-07 17:32 (online boot, a save made under another XUID): our player_xuid stamp
	// read back unchanged at match start and at the commit, CommitStatsTransfer returned at its
	// player_xuid check, and maps 19 and 5 were never written. The stamp had worked on 2026-09-23, on a
	// boot with no save file yet; the callback skips everything for read result 1, which would leave such
	// a buffer writable (read from the code, not measured).
	constexpr std::size_t    kDdlInst_WriteMode                       = 28;  // int
	constexpr int            kDdlWriteMode_Writable                   = 1;
	constexpr int            kDdlWriteMode_ReadOnly                   = 2;
	constexpr std::size_t    kDdlInst_Scrambled                       = 52;  // u8
	constexpr std::size_t    kDdlInst_ScrambleKey                     = 56;  // u32, new on every scramble
	constexpr std::size_t    kDdlRoot_SizeBytes                       = 60;  // int
	// Ddl_InitHeader zeroes this many bytes, then writes magic 0x7376, buildId 7, the root's guid and a
	// checksum of the blank body. 22 for ae_sync. An import binds the root from this header, so a copy
	// into the engine's block leaves it alone.
	constexpr std::size_t    kDdlRoot_HeaderBytes                     = 76;  // int

	// --- "unlock_all" (unlock_all.cpp; decompiled 2026-10-07) ---------------------------------------
	// Where the game decides that something is locked. Each is a predicate, so each is answered in a
	// detour; nothing is written to a save, and nothing is asked of a backend.
	//
	// 1. Levels. The unlockables module (0x7FF72412F280 - 0x7FF72413C6B2) holds every level rule: item
	//    locks, "is purchased", unlock levels, the "new" breadcrumbs, unlock tokens. All 26 of its rule
	//    sites carry the same test: the rules are OFF when (!ui_execdemo || ui_execdemo_cp) and
	//    Com_SessionMode_IsProgressionExemptContext(), which is matchType 0 on a playlist whose name hash
	//    is one of two exempt ones. Answering that one function "yes" to callers inside the module turns
	//    the level rules off the engine's own way. Its other 54 callers (GScr_AddRankXp, LiveStorage,
	//    matchmaking) keep the real answer.
	constexpr std::uintptr_t kDump_Com_SessionMode_IsProgressionExemptContext = 0x7FF728D7C2D0ULL; // bool()
	constexpr std::uintptr_t kDump_Unlockables_Begin                  = 0x7FF72412F280ULL;
	constexpr std::uintptr_t kDump_Unlockables_End                    = 0x7FF72413C6B2ULL;
	// 2. The rules that test is not part of. All return true/1 for LOCKED.
	//    Progression_IsItemLocked: Engine.IsItemLocked. Level rule, or with dvar 0x77DF33B965FDFC29 the
	//      item's loot id in the inventory.
	//    Progression_IsAttachmentLockedInBlock: Engine.IsItemAttachmentLocked and the GSC twin
	//      (0x7FF724139170). After the weapon's own level rule: required gun XP > progression.weapons[w].xp.
	//      Also 1 for a slot the weapon does not have (slot >= row+84), which stays locked.
	//    Progression_IsAttachmentSlotLocked: Engine.IsAttachmentSlotLocked, the slot's gun level.
	//    Progression_IsItemOptionLockedCore: camos and reticles (weapon options). The option's challenge
	//      stat against its tier target, plus an entitlement mask. Engine[0x6F1FD722970FDBA3] reaches it
	//      through Progression_IsItemOptionLocked (0x7FF72413AC20). It calls itself for a prerequisite.
	constexpr std::uintptr_t kDump_Progression_IsItemLocked           = 0x7FF7241395C0ULL; // bool(int mode, uint ctrl, int item)
	constexpr std::uintptr_t kDump_Progression_IsAttachmentLockedInBlock = 0x7FF7241391E0ULL; // char(int mode, block*, uint item, int slot, char)
	constexpr std::uintptr_t kDump_Progression_IsAttachmentSlotLocked = 0x7FF724138F10ULL; // bool(uint mode, uint ctrl, uint item, int slot)
	constexpr std::uintptr_t kDump_Progression_IsItemOptionLockedCore = 0x7FF72413AC90ULL; // char(uint mode, uint ctrl, uint item, uint option, char skipPrereq)
	// Unlockables_GetItemRow (0x7FF724132DD0): table + 409832 * mode + 320 * item, valid when byte +23 has
	// bit 4; the row is entry + 16 and its attachment count is the byte at row + 84.
	constexpr std::uintptr_t kDump_g_unlockableItemsByMode            = 0x7FF72D89CB90ULL;
	constexpr std::size_t    kUnlockables_ModeStride                  = 409832;
	constexpr std::size_t    kUnlockables_ItemStride                  = 320;
	constexpr std::size_t    kUnlockables_ItemFlags                   = 23;   // u8, & 4 = valid
	constexpr std::size_t    kUnlockables_ItemRow                     = 16;
	constexpr std::size_t    kUnlockableRow_AttachmentCount           = 84;   // u8
	// 3. Ownership: blueprints, bundles, battle pass rewards, operators, challenge-reward weapons. It is
	//    all "quantity of item id N in the Demonware marketplace inventory", which never loads here
	//    (measured: inventory state 1, 0 items, on every backend boot).
	//    Inventory_GetItemQuantity: 21 call sites, every one an ownership read (see the IDB comment).
	//      Items 500000-500002 are the trial state (Inventory_ComputeTrialState) and keep their 0.
	//    Loot_GetItemQuantity: the same behind loot_enabled && DwFetch_IsInventoryReady.
	//    Entitlement_IsOwned: Engine.HasEntitlement and the entitlement-locked weapon options. The Lua
	//      subtracts a pack's item from an owned item unless the pack's entitlement is owned too.
	//    DwFetch_IsInventoryReady: 13 loot functions answer 0 / nil until it is true. DwFetch_GetStatus
	//      (bit R of the lobby gate) also asks, and keeps the real answer.
	constexpr std::uintptr_t kDump_Inventory_GetItemQuantity          = 0x7FF727F1B3E0ULL; // u32(int ctrl, u32 itemId)
	constexpr std::uintptr_t kDump_Loot_GetItemQuantity               = 0x7FF727901C60ULL; // u32(uint ctrl, u32 itemId, -, -)
	constexpr std::uintptr_t kDump_Entitlement_IsOwned                = 0x7FF728513340ULL; // char(int ctrl, u64 nameHash)
	constexpr std::uintptr_t kDump_DwFetch_IsInventoryReady           = 0x7FF727F1B990ULL; // bool(uint ctrl)
	constexpr std::size_t    kDwFetch_GetStatus_Size                  = 0x44B;
	constexpr std::uint32_t  kInventory_TrialItemFirst                = 500000;
	constexpr std::uint32_t  kInventory_TrialItemLast                 = 500002;
	// 4. The battle pass: 8 bytes per season in the loot block (LiveUser user data + 40024: owned u8
	//    +0x9AE34, tier u8 +0x9AE35, xp u32 +0x9AE38), written only by a Demonware AE event. The menus read
	//    the UI model Loot_UpdateBattlePassModels fills from these getters; the engine calls it on
	//    inventory and AE events, which never arrive here, so the tick calls it.
	constexpr std::uintptr_t kDump_Loot_GetBattlePassOwned            = 0x7FF727901460ULL; // char(uint ctrl, int season)
	constexpr std::uintptr_t kDump_Loot_GetBattlePassRank             = 0x7FF7279014F0ULL; // u8(uint ctrl, int season)
	constexpr std::uintptr_t kDump_Loot_UpdateBattlePassModels        = 0x7FF727F1DD00ULL; // (int ctrl)
	constexpr std::uintptr_t kDump_LiveStorage_AreStatsReadable       = 0x7FF72531F360ULL; // bool(uint ctrl)
	constexpr int            kBattlePass_TopTier                      = 100;
	// Not covered, because the menus decide them in Lua from save data and not through a native: seven
	// Zombies items read from zm_progression (CoD[0x433DF93283838C0][0x48F022EAF4F53A3]).

	// --- LIVE menus in LAN (zm_progression.cpp; decompiled 2026-09-23) ----------------------------
	// The progression UI (CACUtility.IsProgressionEnabled -> LobbyUtility 0x5855F4927E8631F: the
	// lock/level checks on attachments, camos, weapon levels) reads the MODEL lobbyRoot.lobbyNetworkMode,
	// not the engine value. Its only writer is this setter, called only by LobbyBase_SetNetworkMode
	// (<- Lua Engine.SetLobbyNetworkMode). Enum.LobbyNetworkMode: 1 LAN (and 'local'), 2 LIVE.
	// Detouring the setter changes what the menus see and nothing else: g_lobbyNetworkMode,
	// Engine.GetLobbyNetworkMode and the session network mode stay LAN.
	constexpr std::uintptr_t kDump_LobbyRoot_SetNetworkModeModel      = 0x7FF727DAF360ULL; // (int mode)
	constexpr int            kLobbyNetworkMode_Lan                    = 1;
	constexpr int            kLobbyNetworkMode_Live                   = 2;
	// Measured 2026-09-23: in an offline boot the only SetLobbyNetworkMode call is OUR Boot::ApplyEarly,
	// ~2 s before the progression detours go in, and the Lua never sets it again. So the model is also
	// checked every frame on the game thread and re-applied when it disagrees. Plain globals:
	constexpr std::uintptr_t kDump_g_lobbyNetworkMode                 = 0x7FF7323AF2A8ULL; // int, LobbyBase_GetNetworkMode
	constexpr std::uintptr_t kDump_g_lobbyRootModel_lobbyNetworkMode  = 0x7FF732BC93F0ULL; // u32 model id, 0 until created
	// UI model nodes, 48 B each, indexed by model id: +0 value (i64), +8 type (3 = int). UIModel_SetInt_cand.
	constexpr std::uintptr_t kDump_g_uiModelNodes                     = 0x7FF7340DC250ULL;
	constexpr std::size_t    kUiModelNode_Stride                      = 48;
	constexpr std::size_t    kUiModelNode_Type                        = 8;
	constexpr int            kUiModelType_Int                         = 3;
	// The model alone was not enough (measured 2026-09-23: lobby header went LIVE, Create-a-Class still
	// showed gun level 1 with everything unlocked). Both CACUtility progression gates,
	// IsProgressionEnabled (cacutility 0xB6DBF70ADB092E9) and its twin 0x30EECDDACAB2025, also return
	// false when Engine.GameModeIsMode(MODE_GAME_MATCHMAKING_MANUAL): a custom game, which every LAN lobby
	// is (session matchType 1). Off means CAC hard-codes gun level 1 and never asks the engine about
	// locks. The engine side has no such bypass: Progression_IsAttachmentLockedInBlock (0x7FF7241391E0)
	// compares required XP with progression.weapons[w].xp in the stats block, and
	// Progression_IsItemLocked (0x7FF7241395C0) only exempts matchType 0 playlists.
	// Lua_GameModeIsMode_Impl is Engine.GameModeIsMode's body; its only caller is the Lua wrapper
	// 0x7FF71E91D9D0. It pushes (g_sessionModePacked >> 12) == mode and returns 1.
	constexpr std::uintptr_t kDump_Lua_GameModeIsMode_Impl            = 0x7FF72648AA40ULL; // (L, int mode)
	constexpr int            kGameMode_MatchmakingManual              = 1;  // LuiEnum_Register_eGameModes
	constexpr std::uint64_t  kLuaHash_CACUtility                      = 0x064F9E885836078AULL;
	constexpr std::uint64_t  kLuaHash_IsProgressionEnabled            = 0x0B6DBF70ADB092E9ULL;
	constexpr std::uint64_t  kLuaHash_IsProgressionEnabledAnyMode     = 0x030EECDDACAB2025ULL;
	// Step 2 (playlist tiles, Challenges) reads the ENGINE value through Engine.GetLobbyNetworkMode, whose
	// body pushes LobbyBase_GetNetworkMode() and returns 1 (no caller check; its only caller is the Lua
	// wrapper 0x7FF71E926080). The same native also feeds lobby, launch and storage decisions (luautils:
	// global-character storage file, stats upload; the lobby process files), so it can't be flipped for
	// everyone. The ZM director's LAN/LIVE branches live in DirectorUtility (ui/utility/directorutility,
	// chunk x64:4be06f4309b61a6f.lua), which is missing from the Lua dump. For now the detour only logs
	// who asks.
	constexpr std::uintptr_t kDump_Lua_Engine_GetLobbyNetworkMode      = 0x7FF727A8E790ULL; // (L)

	// --- LPC playlists outside LIVE (local_lpc.cpp; decompiled 2026-09-23) ---------------------------
	// The playlist asset is ONLY loaded by the LPC content loader: slot 0's callback runs
	// Playlist_LoadFromAssets once zone 'core_playlists_tu<N>_100_<buildId>' (flags 0x2000, plus its
	// '<lang>' twin with |0x8000) is up. OnlineContent_LoaderFrame never gets that far offline/LAN: it
	// needs PubVars ready (a Demonware fetch) and refuses while the frontend UI level is running unless
	// networkMode is 2. The zone file is found through a search path: the engine adds the LPC dir
	// (priority 300, device 2) only once every manifest file passed MD5, which also needs Demonware.
	// So we add our own dir the same way and load slot 0's zones ourselves.
	// The retail TU35 files carry this exe's build content id (DB_ValidateFastfileHeader compares the
	// archive checksum of LPC zones to it), so only the tu number in the name differs (this exe: 34).
	// They must still be loaded under their OWN name (tu35): the zone's RSA signature covers the name it
	// is loaded under (DB_Signature_InitForZone seeds the signed buffer with it), and a failed check does
	// not error, it corrupts the asset-entry free list (see DB_TamperResponse below). Renaming them to
	// tu34 is what crashed boot on 2026-09-16 and 2026-09-23.
	constexpr std::uintptr_t kDump_Com_GetTuVersion                   = 0x7FF7295EFA70ULL; // int(); BuildKv "tu_version"
	constexpr std::uintptr_t kDump_Lpc_GetBuildContentIdString        = 0x7FF7292ADE60ULL; // const char*(); "%08x%08x" of the content id
	constexpr std::uintptr_t kDump_Loc_GetLanguage                    = 0x7FF728A57A60ULL; // int(); language dvar
	constexpr std::uintptr_t kDump_Loc_GetLanguagePrefix              = 0x7FF728A57CE0ULL; // const char*(lang), e.g. "en_"
	constexpr std::uintptr_t kDump_FS_AddSearchPath                   = 0x7FF729646DC0ULL; // (path, priority, device, 0); indexes the dir now
	// OnlineContent_LoadSlotZones (0x7FF726E8D740) is these five calls with names built from THIS exe's tu.
	constexpr std::uintptr_t kDump_DB_LoadXAssets                     = 0x7FF727EC4700ULL; // (XZoneInfo {name, u32 flags, pad}[], count, 0)
	constexpr std::uintptr_t kDump_DB_SyncXAssets                     = 0x7FF727EC50D0ULL; // blocks until the queued zones are in
	constexpr std::uintptr_t kDump_R_RemoteScreenUpdateAllow          = 0x7FF7281B59C0ULL; // (bool)
	constexpr std::uintptr_t kDump_R_BeginRemoteScreenUpdate          = 0x7FF7281B0940ULL;
	constexpr std::uintptr_t kDump_R_EndRemoteScreenUpdate            = 0x7FF7281BA620ULL;
	constexpr std::uintptr_t kDump_OnlineContent_OnPlaylistsLoaded    = 0x7FF726E8A570ULL; // slot 0 callback: Playlist_LoadFromAssets + dvar overrides
	constexpr std::uintptr_t kDump_DB_ZoneFileExists                  = 0x7FF727EC2020ULL; // bool(name): "<zone dir>/<name>.ff" through the search paths
	constexpr std::uintptr_t kDump_g_onlineContentSlotTable           = 0x7FF72A395F28ULL; // {u32 flags, pad, fn} x2: 0x2000 playlists, 0x800 ffotd
	constexpr std::uintptr_t kDump_g_zoneInfoRows                     = 0x7FF733B6FA7CULL; // 76 B {name[64], flags, status} x63
	constexpr std::size_t    kZoneInfoRowSize                         = 76;
	constexpr int            kZoneInfoRowCount                        = 63;
	constexpr std::uint32_t  kZoneFlag_Level                          = 0x100;
	constexpr std::uint32_t  kZoneFlag_Playlists                      = 0x2000;
	constexpr std::uintptr_t kDump_g_dbReady                          = 0x7FF735619A2BULL; // u8; DB_IsIdle_cand = this && !syncPending
	constexpr std::uintptr_t kDump_g_dbSyncPending                    = 0x7FF733B77998ULL; // u64
	constexpr std::uintptr_t kDump_g_mainThreadId                     = 0x7FF73561A0C0ULL; // u64, Sys_IsMainThread compares to it
	constexpr std::uintptr_t kDump_g_playlistAsset                    = 0x7FF730043470ULL; // asset type 0x8F, set by Playlist_LoadFromAssets
	constexpr std::uintptr_t kDump_g_playlistValid                    = 0x7FF73004348CULL; // u8
	// DB_AllocXAssetEntry(type, zone) drops with 0x3F6FDE09 when the global entry list is empty and
	// 0xE554F200 when g_xassetPools[type] has no free item. The entries are one fixed array right after
	// the free-list head, addressed by index, so the list cannot be grown.
	// DB_TamperResponse_CorruptEntryFreeList (0x7FF727EC1410) adds the array size to the head when a
	// zone's signature fails at the end of its load; the next zone pops one bogus entry and then drops
	// 0x3F6FDE09. A head outside the array therefore means "signature failed", not "out of entries".
	constexpr std::uintptr_t kDump_DB_AllocXAssetEntry                = 0x7FF727EC1310ULL;
	constexpr std::uintptr_t kDump_g_xassetEntryFreeHead              = 0x7FF732FD5318ULL;
	constexpr std::uintptr_t kDump_g_xassetEntries                    = 0x7FF732FD5320ULL; // 16 B each
	constexpr std::size_t    kXAssetEntryArrayBytes                   = 0xB40000;              // 737,280 entries
	constexpr std::uintptr_t kDump_g_xassetPools                      = 0x7FF72EA10670ULL; // 32 B {items, u32 size, i32 count, u8 singleton, i32 used, freeHead}
	// Which of the entries above are live, MSB first: entry 32*w + k is used when word w has bit 0x80000000 >> k
	// (DB_ForEachXAssetEntry_cand 0x7FF727EBF560). An entry's +14 u8 is its type, +15 u8 its zone.
	constexpr std::uintptr_t kDump_g_xassetEntryUsedBits              = 0x7FF733B2BB20ULL; // 23040 u32
	constexpr std::size_t    kXAssetEntryUsedWords                    = 23040;
	// Per loaded zone, 76 B: +0 u32 zone flags (what DB_EnumXAssetsInZones_cand 0x7FF727EC1F00 masks).
	constexpr std::uintptr_t kDump_g_zoneInfos                        = 0x7FF733B6FA70ULL;
	constexpr std::size_t    kZoneInfoSize                            = 76;
	// The bgcache name tables (zonekit world_writer.hpp EncodeBgCache), filled at level load by
	// BG_Cache_RegisterAll_cand 0x7FF726213850 from every loaded bgcache asset. 40 tables of 64 B: +0 name string,
	// +8 u8 asset type, +12 capacity, +48 u32 first slot in the names array, +52 i32 count (slot 0 unused),
	// +60 u8 sorted. Names: 16 B {u64 name, header}. Table 2 is "model": the only place G_SetModel_cand
	// (0x7FF723DA8F70) finds a map entity's model.
	constexpr std::uintptr_t kDump_g_bgCacheTables                    = 0x7FF72AADB4F0ULL;
	constexpr std::uintptr_t kDump_g_bgCacheNames                     = 0x7FF72E428C50ULL;
	constexpr std::size_t    kBgCacheTableModel                       = 2;
	// The asset-usage census (mapkit_usage.hpp, plan P6; read 2026-09-30). Scripts and map entities reach a
	// bgcache-listed asset through BG_Cache_FindIndex_cand(table, name): 0 = not listed, else the slot whose
	// header BG_Cache_Register_cand looked up when the level loaded (DB_FindXAssetHeader(table type, name, 1, -1)
	// for every name of every loaded bgcache). Leaf, plain prologue (mov [rsp+8],rbx ...), no caller check.
	// The 40 tables: 1 vehicle, 2 model, 3 aitype, 4 character, 5 xmodelalias, 6 weapon, 7 gesture, 8 gesturetable,
	// 9 zbarrier, 10 rumble, 11 shellshock, 12 statuseffect, 13 xcam, 14 destructible, 15 streamerhint, 16 flowgraph,
	// 17 xanim, 18 sanim, 19 scriptbundle, 20 talent, 21 cinematicmotion, 22 vehicleassembly, 23 execution,
	// 24 statusicon, 25 locationselector, 26 menu, 27 material, 28 string, 29 eventstring, 30 moviefile, 31 objective,
	// 32 fx, 33 lui_menu_data, 34 lui_elem, 35 radiant_exploder, 36 soundalias, 37 client_fx, 38 client_tagfxset,
	// 39 client_lui_elem. A table's +8 type 0xDD holds names with no asset (strings, sound aliases, menus).
	constexpr std::uintptr_t kDump_BG_Cache_FindIndex_cand            = 0x7FF726212E90ULL; // i64(u8 table, u64 name)
	constexpr std::uintptr_t kDump_BG_Cache_Register_cand             = 0x7FF726213DB0ULL; // size 0x24D
	constexpr std::size_t    kBgCacheTableCount                       = 40;
	// XAssetEntry (16 B, g_xassetEntries above): +0 header, +8 u64 {bits 0-23 next in the hash bucket, bits 24-47 the
	// next OVERRIDE (DB_InsertOverrideEntry 0x7FF727EC1210: an asset of the same name from another zone, sorted by zone
	// priority; only the head has its used bit)}, +14 type, +15 zone. Zone z is g_zoneInfoRows[z - 1] (DB_LinkXAssetEntry
	// reads the name at 0x7FF733B6FA30 + 76*z and the flags at g_zoneInfos + 76*z).

	// --- mapkit: custom map zones from cw-mod/maps (mapkit_loader.cpp; decompiled 2026-09-25) -------
	// Zone files are opened as "<zone dir>/<name>.ff" / ".fd" and FS_ResolvePathThroughSearchPaths
	// (0x7FF7296469A0) maps that to whichever search path indexes the file: the LOWEST priority value
	// wins. The retail zone dir is priority 400, device 0 (read from the dump's g_fsSearchPaths), so a
	// maps folder added at 200 overrides it, name for name.
	// DB_ExpandZoneVariants adds en_/ww_/1080_|4k_/ww_1080_|ww_4k_/techset_ for a level zone WITHOUT
	// checking that they exist, and DB_LoadZone (0x7FF727EC4370) errors 0xC21445D4 on a missing one
	// (unless the flags carry 0x80000): a custom map ships every variant.
	// A custom zone has no .fd. Both readers of <zone>.fd (DB_ReadZoneFileHeader and DB_OpenPatchFile)
	// open it through FS_OpenFileRead, which would otherwise fall through to a retail .fd of the same
	// name and fail its base-header check (ERR_DROP 0x91DFC002).
	// The zone signature is checked once, after the zone is linked: DB_LoadXFile_Finish ->
	// DB_Signature_VerifyZone, which reads the name it was seeded with from g_zoneSigName and clears
	// kZoneSigStateBytes from there when done. A failure trips DB_TamperResponse (see above).
	constexpr std::uintptr_t kDump_DB_Signature_VerifyZone            = 0x7FF7292AD410ULL; // void(); no caller check (prologue checked)
	constexpr std::uintptr_t kDump_g_zoneSigName                      = 0x7FF7363D71A0ULL; // char[64]: basename of the zone name being verified
	constexpr std::size_t    kZoneSigStateBytes                       = 27392;
	constexpr std::uintptr_t kDump_FS_OpenFileRead                    = 0x7FF7293FC9A0ULL; // handle(path, mode, device); 3-arg wrapper, 13 callers
	// SV_StartMap is where the map name enters: SV_SpawnServer, CL_MapLoading, the mapname dvar and
	// Com_LoadLevelFastFiles all take it from here. 5 args and no caller check in the prologue, so the
	// detour calls the original directly (the ArxanCall thunk forwards 4 register args only).
	constexpr std::uintptr_t kDump_SV_StartMap                        = 0x7FF723D484F0ULL;
	// SV_StartMap(controller, name, kind, flag, 0), from SV_Map_f "map <name> <kind> <flag>". Only kinds 0
	// and 3 run Com_LoadLevelFastFiles. Kind 1 (what the LAN lobby launch passes, logged 2026-09-25) loads
	// NOTHING: the lobby preload already loaded the level zone. Kind 2 loads [{g_mapPreloadName, 0x100}]
	// (free 0x100) through MapPreload_LoadPreloaded (0x7FF728B37610), only if MapPreload_IsPreloaded(name).
	// The lobby preload has two drivers, and both queue the map through MapPreload_StartZoneRead(name):
	//   - MapPreload_Frame (0x7FF728B37700): config string 17739 ("mspreload <map>") -> g_mapPreloadName,
	//     then StartZoneRead(g_mapPreloadName) (the pointer to the global itself).
	//   - MapPreload_SessionFrame (0x7FF727B33430): in lobby state 14, StartZoneRead(Session_GetMapName(1))
	//     whenever that differs from the last loaded request. The LAN ZM lobby uses THIS one: at launch
	//     g_mapPreload was still state 0 with an empty name (2026-09-25 06:29).
	// MapPreload_LoadQueued (0x7FF727B32B30) then allocates the preload stream buffer
	// (MapPreload_AllocStreamBuffer, its only caller) and loads DB_LoadXAssets([{map, 0x200}], 1, free 0x280).
	// DB_LoadZone streams every 0x200 zone into that buffer. So a 0x200 load anywhere else (the 06:27 build
	// did it in SV_StartMap, after the lobby had released the buffer) reads into address 0 and dies with
	// sys_error 0xC134097A ",error" from the async read. Renaming only the SV_StartMap argument dies too:
	// the target zone was never loaded (sys_error clip_map of maps/zm/zm_mapkit.d3dbsp, 2026-09-25 x2).
	// So the redirect goes in MapPreload_StartZoneRead: the lobby then preloads the target itself, and the
	// SV_StartMap rename only starts it. No caller check in its prologue (read 2026-09-25).
	// Harmless side effect: SessionFrame now sees "loaded request != session map" and calls StartZoneRead
	// every frame. That returns at once while the target zone is present. Its release path is keyed on
	// DB_AnyPreloadZoneFailed (0x7FF727EC13E0: a 0x6A0 zone in status 9), not on the name, so it never loops.
	// The preload alone is not enough. At launch the CLIENT loads the map itself, before SV_StartMap:
	// sub_7FF7228D7690 does "MapPreload_IsPreloaded(name) ? MapPreload_LoadPreloaded : Com_LoadLevelFastFiles(name)"
	// with the LOBBY's name, i.e. DB_LoadXAssets([{common, 0x10}, {zm_silver, 0x100}], 2, 0x150|0x170).
	// DB_LoadXAssets ORs in the return of MapPreload_OnLoadXAssets (0x7FF727B33190). When a level load (0x150
	// bits) names another map than the loaded preload request, that frees the preload stream buffer and adds
	// free flags that unload the preloaded zone. In retail that load is a no-op (same name); with the preload
	// redirected it dropped zm_mapkit and loaded zm_silver (2026-09-25 06:42; the SV_StartMap guard then
	// refused the redirect). So DB_LoadXAssets is detoured too: a level (0x100) or preload (0x200) entry named
	// like the redirect source is renamed. No caller check in its prologue (read 2026-09-25).
	constexpr std::uintptr_t kDump_MapPreload_StartZoneRead           = 0x7FF727B32E60ULL; // (name, 3 unused pass-through args)
	constexpr std::uintptr_t kDump_g_mapPreloadState                  = 0x7FF7355622F0ULL; // i32: 0 idle, 1 parsed, 2 loading, 3 preloaded
	constexpr std::uintptr_t kDump_g_mapPreloadName                   = 0x7FF7355623F8ULL; // char[128]
	constexpr std::size_t    kMapPreloadNameSize                      = 128;
	// g_zoneInfoRows row status (DB_LoadZone): 2 loading, 3 loaded, 5 loaded (0x6A0 flags), 9 failed.
	constexpr int            kZoneStatusFailed                        = 9;
	constexpr std::uint32_t  kStartMapKindLoaded                      = 1;
	constexpr std::uint32_t  kStartMapKindPreloaded                   = 2;
	constexpr int            kMapKitSearchPathPriority                = 200;
	constexpr int            kMapKitSearchPathDevice                  = 0;  // the retail zone dir's device
	// Link order (decompiled 2026-09-25 after the D2 plane test dropped at launch). DB_ExpandZoneVariants
	// emits a level zone X as [X_patch], en_X, ww_X, 4k_X|1080_X, ww_4k_X|ww_1080_X, techset_X, X: the base
	// zone comes LAST. DB_LoadXAssets queues that list as is, and DB_ProcessLoadQueue (0x7FF727EC3E50) links
	// a preloaded zone in queue order. So a mapkit override zone (ww_4k_<map>) linked before its map, and a
	// by-name reference into the map (the empty streamerworld's lighting) found nothing. DB_LinkMissingReference
	// (0x7FF727EC1860) then substitutes the type's default asset, and lighting has none: ERR_DROP 0xDE8F2849
	// "Uniform 99 Divebomb Karma". A retail zm_silver asset never references the retail ww_4k_zm_silver
	// (its one keyvaluepairs, 0x3A3D083D3CE2FEB9, checked 2026-09-25), so the override may link after the map.
	// Returns the entry count in eax; 5 args, no caller check in the prologue (read 2026-09-25).
	// Zone memory (decompiled 2026-09-26 after the first CUSTOM MAPS match died returning to the menu). Each of
	// 22 memory regions ("db map A", ...; names at 0x7FF72ADBE0B0) is a STACK of per-zone records {name hash,
	// 10 block sizes}. DB_QueueZoneLoads_cand (0x7FF727EC1B40) appends each new zone to g_loadedZones (360 B
	// each, count at 0x7FF733B765C8, 63 at most) in queue order, and a zone the lobby preloaded keeps its earlier
	// entry. DB_UnloadMarkedZones (0x7FF727EC2760) frees the marked ones LAST ENTRY FIRST through
	// DB_ZoneMem_FreeZone (0x7FF726BFFDD0), and a zone that is not on top of a region it allocated in is a
	// sys_error: "0x0a646198,<zone name hash>,<region>". So the list must stay in allocation order. The lobby
	// preloads a map when its playlist is set, BEFORE the CUSTOM MAPS pick: renaming ww_4k_zm_silver to the
	// custom zone at launch left the preloaded retail one unlinked and unfreed above 4k_zm_silver
	// (0x4f2a0075b6d0cb68), and the return to the menu died on it. A custom zone is therefore ADDED after the
	// base, never swapped in, and never part of a preload.
	constexpr std::uintptr_t kDump_DB_ExpandZoneVariants              = 0x7FF7295F2490ULL; // int(zones, count, names[64 each], out[], max)
	// World start (read 2026-09-25 for test G1, mapkit's own gfx_map). R_LoadWorldFrame (0x7FF727F65100) finds the
	// level's gfx_map by its .d3dbsp name (DB_FindXAssetHeader(0x1B)) and keeps it in the renderer globals at
	// +0x2DDF580, then calls R_InitWorld_cand once: the streamer starts from gfx_map +7456 (the streamerworld) and
	// Streamer_InitDistricts. By then the override swap has run (the empty districts of D2 took effect there), so
	// the gfx_map it holds is the one that draws. void(), no caller check (prologue read 2026-09-25), one caller.
	constexpr std::uintptr_t kDump_R_InitWorld_cand                  = 0x7FF727F64E50ULL; // void()
	// A map of its own (read 2026-09-29, IDB renamed): it starts under its own name, and the lobby carries that name.
	//   - The lobby map is a string: Session_SetMapName (sub_7FF727B272A0, 2 args) copies it to the type-0 session object
	//     +9176 (36 chars) with its lowercase FNV hash at +9216 and marks the session dirty; the lobby-state packers
	//     (LobbyMsgRW_PackageLobbyStateA/B) send it to every member as the string field "map". The host's playlist apply
	//     (sub_7FF726F4C700) sets it from the entry's map; a member's apply (Lobby_ApplySessionSettings_cand) sets it from
	//     the host's state. Plain prologue, no caller check.
	//   - The level script is found by the started map's name: sub_7FF71E76DFD0 builds "scripts/<p>/<map>.gsc|.csc",
	//     and every script_using asset of every loaded zone is linked too (DB_EnumXAssetsInZones 0x46 + the mode's).
	//   - World assets are looked up by the started map's .d3dbsp name through DB_FindXAssetHeader: clip_map
	//     (CM_LoadMap), com_map (sub_7FF7295F04A0), navmesh (AI_LoadNavmeshForMap and sub_7FF7225856F0), navvolume,
	//     the entity lists, and the client game's world (World_LoadByMapName_cand 0x7FF72904A850: gfx_map and
	//     cpu_occlusion_data). But the engine keeps ONE world (read after the 19:04 crash): CM_LoadMap drops what it
	//     finds and uses g_clipMap, the clip_map pool's first slot; and the renderer loads the gfx_map named after the
	//     level zone that finishes loading (DB_PostLoadFrame_ApplyOverrides 0x7FF727EC4A30 calls R_BeginLoadWorld
	//     0x7FF727F64DB0 for a zone with flag 0x140 and none of 0x1278000: zm_silver, not a map of its own's zone with
	//     the override flags), then districts by the gfx_map's name. So a map of its own names its world after its
	//     asset library and replaces the library's in the override swap; a lookup by its own name is aliased.
	//   - getmapfields (GSC builtin 0x7FF7262D7E00) finds the maptable entry through MapTable_FindEntryByHash (by the
	//     map name's hash) and returns undefined for an unknown map, and content_manager reads a field of the result
	//     unchecked. MapTable_GetMapFlags (djb2 of the name, binary search) is its second lookup. Both plain leaf
	//     functions, 1 arg.
	//   - Session_PickActiveSlot(type): the slot index of the type's live session (reads two globals, no guard).
	constexpr std::uintptr_t kDump_Session_SetMapName                = 0x7FF727B272A0ULL; // char(u32 slot, const char* map)
	constexpr std::uintptr_t kDump_Session_PickActiveSlot            = 0x7FF726F827C0ULL; // BOOL(int type)
	constexpr std::uintptr_t kDump_Lobby_ApplySessionSettings_cand   = 0x7FF7274B5540ULL; // a member applies the host's settings
	constexpr std::size_t    kLobby_ApplySessionSettingsSize         = 0x411;
	constexpr std::uintptr_t kDump_MapTable_FindEntryByHash          = 0x7FF728D7B040ULL; // entry*(u64 map name hash)
	constexpr std::uintptr_t kDump_MapTable_GetMapFlags              = 0x7FF728D7B1D0ULL; // u32(const char* map), ~0 = none
	// DB_GetZonePriority 0x7FF727EC2F50: a level zone is 7, +20 with the region flag. A map of its own's zone gets the
	// flags an override zone had (0x1040100 / 0x1040200, proven by every overlay build), so its same-name overrides of
	// its asset library's assets (its world, the sky material) win.
	constexpr std::uint32_t  kOverrideZoneFlags                      = 0x1040000;
	constexpr std::uintptr_t kDump_g_rendererGlobals                 = 0x7FF736474930ULL; // -> the renderer's globals
	constexpr std::size_t    kRendererWorldOffset                    = 0x2DDF580;         // the gfx_map it draws
	// The xasset pools (kDump_g_xassetPools above): 32 B per type {+0 items, +8 u32 item size, +12 i32 item
	// count, ...}; a free item's +0 points back into the pool. The same table the script loader finds by
	// signature ('xassetpool', resolved at +0x11E50670 on 2026-09-26). The live dump (mapkit_live.cpp) reads the
	// xmodel and clip map pools.
	constexpr std::size_t    kXAssetTypeXModel                       = 6;
	constexpr std::size_t    kXAssetTypeClipMap                      = 0x18;
	constexpr std::size_t    kXAssetTypeBgCache                      = 0x6D;
	// The renderer's per-xmodelmesh state (mapkit_live.cpp, read 2026-09-26). A mesh's index is
	// (mesh - *g_xmodelMeshIndexBase) >> 6 (XModelMesh_GetIndex 0x7FF7262E94C0: the xmodelmesh pool, 64-B items).
	// The streaming code keeps one bit per mesh in bit arrays hung off the renderer globals, and one dword per mesh
	// inline at +0x3A0010. XModelMesh_IsReadyToDraw_cand (0x7FF728FE5EB0) returns true for every RESIDENT mesh
	// without reading them; a streamed one needs the +0x2DDC8F8 bit set and the +0x2DDC8E8 / +0x2DDC8F0 bits clear.
	constexpr std::uintptr_t kDump_g_xmodelMeshIndexBase             = 0x7FF72EA0FFB8ULL; // -> xmodelmesh pool items
	constexpr std::size_t    kRendererMeshBitArrays[]                = { 0x2DDC858, 0x2DDC868, 0x2DDC870, 0x2DDC878,
		0x2DDC890, 0x2DDC8E8, 0x2DDC8F0, 0x2DDC8F8 };                                           // each -> u32[]
	constexpr std::size_t    kRendererMeshBitsInline                 = 0x2DDC900;         // u32[] inline
	constexpr std::size_t    kRendererMeshDwords                     = 0x3A0010;          // u32 per mesh inline
	// The AI nav world (P5, read 2026-09-29; AI_InitNavWorld_cand 0x7FF723BAD970 builds it at map load). The global
	// holds a pointer to {+40 hkaiWorld*, +56 hkaiNavMeshInstance*[], +64 i32 count, +72 the navmesh asset, +80 the
	// shared tagfile's object (Runtime_NavMeshAssetShared)}. The asset: +8 shared stream key (flags at +55, bit 2 =
	// inline: mapkit's), +32 cell count, +40 cells (64 B: +56 the cell's Runtime_NavMeshCell*, whose +32 is the
	// hkaiNavMesh: faces +24/+32, edges +40/+48, vertices +56/+64). Null objects = the tagfile was not loaded.
	constexpr std::uintptr_t kDump_g_aiNav                           = 0x7FF72D791D50ULL; // -> the nav world

	// --- mapkit zone trace (mapkit_trace.cpp; decompiled 2026-09-25) ------------------------------------
	// One zone load = DB_LoadZone -> DB_LoadXFile_Internal(a1, a2, zoneName, a4, XAssetList* list, XBlock*
	// blocks, a7, a8, flags). It reads the 40-B XAssetList itself (not through DB_ReadXFile), then
	// DB_InitStreams(blocks), Push(4), the script strings, the asset table, and Load_XAsset(0, &asset[i])
	// for each of list->count (i32 @+24) 16-B entries {type, header}. Every stream byte after the list goes
	// through DB_ReadXFile(dst, size) (the stored blocks of DB_LoadXFileData) or DB_ReadXFileString(dst,
	// &count) (Load_XStringInline), in both the plain and the .fd-patched mode. So the byte count of those
	// two, per zone, is the position in the patched stream that mapkit reads offline.
	// Stream position state (DB_PushStreamPos 0x7FF72966EEB0): g_streamPosIndex = the current block,
	// g_streamPos = its live position, g_streamPosArray[13] = every other block's saved position (block 0's
	// entry is 0 until block 0 is first left). Positions are absolute: offset = pos - blocks[b].base, the base
	// being the first qword of each 24-B XBlock (DB_InitStreams 0x7FF72966EBA0). None of the four targets
	// has a caller check in its prologue (read 2026-09-25).
	constexpr std::uintptr_t kDump_DB_LoadXFile_Internal              = 0x7FF72769FC00ULL; // 9 args, see above
	constexpr std::uintptr_t kDump_Load_XAsset                        = 0x7FF71E7F7560ULL; // (atStreamStart, XAsset*)
	// With flags & 0x6A0 (the lobby preload's 0x200, among others) DB_LoadXFile_Internal runs a complete twin
	// loader family instead: the same stream grammar, but asset roots go to block 1 instead of temp block 0,
	// and every non-null pointer field logs an 8-byte fixup in block 11 (DB_PushPointerFixup 0x7FF72966F010).
	// Seen 2026-09-25: the first zm_silver trace (a lobby preload) recorded 0 assets through Load_XAsset alone.
	// The block bases are allocated inside DB_LoadXFile_Internal, so they are read at the first asset.
	constexpr std::uintptr_t kDump_Load_XAsset_Preload                = 0x7FF71E867350ULL; // same signature
	constexpr std::uintptr_t kDump_DB_ReadXFile                       = 0x7FF7276A07D0ULL; // (dst, int size)
	constexpr std::uintptr_t kDump_DB_ReadXFileString                 = 0x7FF72769F470ULL; // (dst, u32* count incl. NUL)
	constexpr std::uintptr_t kDump_g_streamPosIndex                   = 0x7FF736EE30C8ULL; // i32
	constexpr std::uintptr_t kDump_g_streamPosArray                   = 0x7FF736EE30D0ULL; // u64[13]
	constexpr std::uintptr_t kDump_g_streamPos                        = 0x7FF736EE3140ULL; // u64
	constexpr std::uintptr_t kDump_g_streamPosStackIndex              = 0x7FF736EE3148ULL; // i32
	constexpr int            kXBlockCount                             = 13;
	constexpr std::size_t    kXBlockStride                            = 24;
}
