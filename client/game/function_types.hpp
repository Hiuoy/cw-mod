#pragma once
#include "common.hpp"
#include "engine/t9/Font_s.hpp"
#include "engine/t9/enums/LobbyNetworkMode.hpp"
#include "engine/t9/unknown/ContentManager.hpp"

namespace Client::Game::Functions {
	using BB_AlertT = void(const char* type, const char* msg);
	using CL_DisconnectT = void(int localClientNum, bool deactivateClient, const char* message);
	using CL_DrawTextPhysicalT = void(const char* text, int maxChars, T9::Font_s* font, float x, float y, float rotation, float xScale, float yScale, const float* color, int style, int padding);
	using Com_SessionMode_SetNetworkModeT = void(int mode);
	using Dvar_SetBoolFromSourceT = void(std::uintptr_t* dvar, bool value, int source);
	using Dvar_SetIntFromSourceT = void(std::uintptr_t* dvar, int value, int source);
	// value = 32-byte DvarValue, domain = dvar+32 (16 B). See kDump_Dvar_StringToValue.
	using Dvar_StringToValueT = void*(void* value, int type, const void* domain, const char* text);
	using Dvar_ApplyValueInternalT = void(std::uintptr_t* dvar, const void* value, unsigned int source);
	using Dvar_ShowOverStackT = void();
	// bdLogin flow/service selector; hooked to return 9 (studio auth). See dump_anchors.hpp.
	using Dw_GetLoginFlowT = std::uint64_t(void* loginConfig);
	// Login state-machine status setter; hooked read-only to mirror the login transcript. See
	// dump_anchors.hpp. Returns the ctx (opaque); we only read `status`.
	using Login_SetStatusT = std::uint64_t(void* ctx, const char* status, std::uint32_t code);
	// LiveUser_FirstPartyPresenceOk (nullary bool). The state-1 first-party gate; MinHooked with a
	// return-address guard so it returns 1 only when the login driver calls it. See dump_anchors.hpp.
	using LiveUser_FirstPartyPresenceOkT = bool();
	// FirstParty_GetSession_MayBeNull(mgr) -> *(void**)(mgr+24). Returns the first-party session,
	// which is NULL on every studio-auth boot and is dereferenced unchecked by all 15 call sites.
	using FirstParty_GetSessionT = void*(void* mgr);
	// FirstParty_GetLocalUserIndex_ViaSession (nullary u32) -> session+0xD0, the local user /
	// controller index. Measured to be the one and only consumer of the null session; hooked to
	// return 0 (= controller 0) so the studio-auth boot needs no synthesised session object.
	using FirstParty_GetLocalUserIndexT = std::uint32_t();
	// LiveUser_SignOut_BuildDropMessage(controller, const char** outMsg) -> bool. Tears the live user
	// down, then returns whether that sign-out should become an ERR_DROP; the message it writes to
	// outMsg is the "Boy 501 Gothic Missile" word-code. See dump_anchors.hpp.
	using LiveUser_SignOutBuildDropMessageT = char(std::uint32_t controller, const char** outMsg);
	// B3 LPC list diagnostics. See dump_anchors.hpp.
	using BdRemoteHttpTask_FinishRowT = std::uint64_t(void** task, void* response);
	using PublisherObjectsResource_ParseT = std::uint8_t(void* resource, void* response);
	using ObjectMetadata_ParseJsonT = std::uint8_t(void* metadata, void* json, std::uint32_t ownerType);
	using Lpc_ListCallbackT = std::uint64_t(std::uint32_t controller);
	using BdLobbyMsg_WriteHeaderT = std::uint64_t(void** buffer, std::uint8_t msgType, std::uint8_t serviceId);
	// B5 MtxSync transcript. See dump_anchors.hpp.
	using MtxSync_ControllerGateT = std::uint8_t(std::int32_t controller);
	using MtxSync_OnBnetTokenT = std::uint8_t(std::int32_t error, std::int64_t* token);
	// B6 lobby gate: DwFetch_GetStatus(controller, u32* got) -> all required fetch bits set. See dump_anchors.hpp.
	// The third argument (r8) is a 16-byte scratch struct DwFetch_IsDone passes; other callers leave r8
	// as whatever it was. Declared so the detour forwards r8 untouched instead of clobbering it.
	using DwFetch_GetStatusT = std::uint8_t(std::uint32_t controller, std::uint32_t* got, void* scratch);
	using DwFetch_IsDoneT = std::uint8_t(std::uint32_t controller);
	// LuaUtils.IsTrial's engine side. No arguments.
	using LiveUser_IsTrialT = std::uint8_t();
	// A Lua native (lua_CFunction): arguments on L's value stack, returns the result count.
	using LuaNativeT = std::int32_t(void* L);
	// DwLogin_BuildStudioToken(loginConfig, userObj, outBuf). Builds+signs the studio-auth JWT into
	// outBuf; returns 1 on success. Hooked (read-only diagnostics) to see why it fails. See dump_anchors.hpp.
	using DwLogin_BuildStudioTokenT = std::uint8_t(void* loginConfig, void* userObj, void* outBuf);
	// LuiError_ReportFatal(context, L). The engine's fatal handler for a Lua error that lua_pcall
	// caught - it reports and terminates. Hooked so the error text lands in our log, and so an
	// online boot survives it. See dump_anchors.hpp.
	using LuiError_ReportFatalT = std::uint64_t(const char* context, void* luaState);
	// LUI_RunFile(L, chunkName): load a LUI chunk from the luafile pool and pcall it; 1 on success.
	// Hooked so a missing ui/ffotd chunk is skipped instead of killing the process. See dump_anchors.hpp.
	using LUI_RunFileT = std::uint8_t(void* luaState, const char* name);
	// DecryptString(s): decrypts an engine string in place and returns it; plain text comes back as is.
	// Hooked to replace UI text ("ui_text" in cw-mod.json). See dump_anchors.hpp.
	using DecryptStringT = char*(char* s);
	// Scr_ConstructMessageString(inst, out, firstParam, count): an iprintln payload into out = {u32 length;
	// char text[1024]}. Hooked read-only to mirror script prints. See dump_anchors.hpp.
	using Scr_ConstructMessageStringT = void(int inst, std::uint32_t* out, std::uint32_t firstParam, int count);
	// bdCommonAddr::bdCommonAddr(this, localAddrs, publicAddr, natType, extra). See dump_anchors.hpp.
	using bdCommonAddr_CtorT = void*(void* self, void* localAddrs, const void* publicAddr, std::uint32_t natType,
		std::int32_t extra);
	// The openmenu replay (see kDump_g_luiCtx in dump_anchors.hpp). controller -1 = omit the
	// "controller" event parameter, which is what the console command passes.
	using LUI_DispatchAddMenuEventT = std::int64_t(const char* root, std::uint64_t menuHash,
		int controller, void* luaState);
	using LUI_GetRootNameT = const char*(int controller);          // "UIRoot0".. / "UIRootFull"
	using CL_LocalClientToControllerT = int(int localClient);      // -1 = no controller bound
	using UI_SetUiActiveT = void(std::int64_t localClient, bool active);
	using lua_createtableT = void(void* L, int narr, int nrec);          // pushes a new table
	using lua_setfieldT = void(void* L, int idx, const char* k);         // t[k] = top; pops it
	using LUI_ProtectedCallT = int(void* L, int nargs, int nresults, int errfunc);  // 0 = ok
	// lua_load: pushes the compiled chunk (0) or an error message (nonzero). See kDump_lua_load.
	using lua_ReaderT = const char*(void* L, void* data, std::size_t* size);
	using lua_loadT = int(void* L, lua_ReaderT* reader, void* data, const char* chunkname);
	// The two Battle.net fatal-error reporters. Both end in Com_Error(level 1024) = undismissable
	// dialog + exit to desktop; hooked to log-and-return while an online-mode test boot is active.
	// The unguarded one takes the BGS code by value, the latched one by pointer. See dump_anchors.hpp.
	using BnetError_ReportFatalUnguardedT = std::uint64_t(std::uint32_t bgsErrorCode);
	using BnetError_ReportFatalIfSignedInT = void(std::uintptr_t ctx, std::uint32_t* bgsErrorCode);
	// The two ways the first-party object enters STATE_ERROR (+56 = 4), which is what makes LUI raise
	// the BLZBNTBGS popup. Both return the object/opaque; callers ignore it. See dump_anchors.hpp.
	using FirstParty_SetErrorT = std::uint64_t(std::uintptr_t firstPartyObj, int code);
	using FirstParty_SetErrorStateT = std::uint64_t(std::uintptr_t firstPartyObj);
	using FirstParty_OnBgsDisconnectedT = std::uint64_t(std::uintptr_t firstPartyObj, std::uintptr_t unused,
		const std::uint32_t* bgsCode);
	// The queue LUI drains to render the "ERROR / EXIT TO DESKTOP" popup — the single choke point
	// every producer of that dialog goes through. Returns 1 = queued (caller stops), 0 = refused
	// (caller falls through to Com_Error / the pending store). See dump_anchors.hpp.
	using ErrorQueue_PushT = char(int level, const char* message, char flag);
	// The watchdog that actually produces the BLZBNTBGS dialog: signs both controllers out and calls
	// Com_Error(1024) itself, bypassing both the queue and the reporters. Returns the message it built
	// (callers ignore it). See dump_anchors.hpp.
	using LiveUser_ForceSignOutAndFatalT = char*();
	using LiveUser_GetUserDataForControllerT = std::uintptr_t*(int controllerIndex);
	using LobbyBase_SetNetworkModeT = void(T9::LobbyNetworkMode networkMode);
	// Stores the lobby UI's target menu and fires the Lua OnLobbySettings + OnUpdateUI events. It is
	// not a navigation: no Lobby.Core process runs, so no lobby is created (see Pointers::SetMode).
	using LobbyUI_SetTargetMenuAndNotifyT = void(int targetMenu, char fromLobbyState);
	using Unk_SetUsernameT = void(std::uintptr_t* _this, const char* username);

	// PlayerData_ResetBufferToDefaults, called by the TEMPORARY dwuser->hdd redirect hook.
	// `version` == -1 means "use the def's default version" (def+48).
	using PlayerData_ResetBufferToDefaultsT = std::int64_t(unsigned int controller, unsigned int dataMapId, int version);
	using RtlDispatchExceptionT = bool(PEXCEPTION_RECORD record, PCONTEXT ctx);

	// --- Phase 3 lever-1 (loopback co-op) ---
	// SV_DirectConnect takes ONE arg: a pointer to a 16-byte netadr. The connect userinfo is not an
	// argument — it is staged into the OOB message-redirect stack first (SV_StageConnectMessage),
	// read by SV_DirectConnect, then popped (SV_UnstageConnectMessage). See docs/phase3_netcode.md.
	using SV_DirectConnectT = std::int64_t(void* netadr);
	using SV_StageConnectMessageT = std::int64_t(const char* connectStr);
	using SV_UnstageConnectMessageT = std::int64_t();
	using Com_SessionMode_GetStringT = const char*();

	// --- Debug tab: dvars without a dev console -------------------------------------
	// Dvar_FindVar(nameHash) -> Dvar*, or null when nothing is registered under that hash. The hash
	// is Pointers::HashString (FNV-1a-64, lowercased, 63-bit) — the engine masks off the top bit
	// before comparing, so passing an unmasked hash works too. Takes an internal spinlock, so this
	// must run on the game thread like every other engine call we make.
	using Dvar_FindVarT = std::uintptr_t*(std::uint64_t nameHash);

	// --- Lua 5.1 C API ------------------------------------------------------------------------
	// T9's LUI runs stock Lua 5.1 with NaN-boxed values (NOT LuaJIT - that string appears nowhere
	// in the image). The menu registry is a Lua table, not a native array, so enumerating menus
	// means walking the Lua state. The whole standard library's name strings are stripped, so
	// these were identified by decompiling the xref cluster around lua_index2adr and matching each
	// against Lua 5.1 source semantics; see docs and the .i64.
	//
	// EVERY one of these must be called through an ArxanCall thunk, never directly - the table
	// accessors check their return address and silently do nothing when it is outside the game
	// image. See arxan_call.hpp.
	//
	// The stack index is a 32-bit int (lua_index2adr takes `int`), so pseudo-indices are passed
	// as plain negative ints, not as the sign-confused 64-bit constants the decompiler prints.
	using lua_getfieldT   = void(void* L, int idx, const char* k);        // pushes t[k]
	using lua_gettableT   = void(void* L, int idx);                       // pops key, pushes t[key]
	using lua_rawgetiT    = void(void* L, int idx, int n);                // pushes t[n], no metamethods
	using lua_nextT       = int(void* L, int idx);                        // pops key, pushes key+value
	using lua_pushvalueT  = void(void* L, int idx);                       // pushes a copy of idx
	using lua_typeT       = int(void* L, int idx);                        // LUA_T* of the value at idx
	using lua_tolstringT  = const char*(void* L, int idx, std::size_t* len);
	using lua_tonumberT   = double(void* L, int idx);
	// Pushes the metatable of the value at idx and returns 1, or pushes nothing and returns 0.
	// Unlike the table accessors this one dispatches on the value's type internally (table and
	// userdata carry their own mt, everything else shares G(L)->mt[type]), so it is safe to call
	// on a value of ANY type - which is what makes it the way into the non-table LUI root objects.
	using lua_getmetatableT = int(void* L, int objindex);

	// --- The Lua debug API ---------------------------------------------------------------------
	// Same Arxan caller check as the accessors above, so same rule: thunk or nothing.
	//
	// lua_getstack(L, level, ar) -> 1 while `level` addresses a live frame. It fills only ar->i_ci;
	// everything else stays untouched until lua_getinfo is asked for it.
	using lua_getstackT = int(void* L, int level, void* ar);
	// lua_getinfo(L, what, ar, extra). FOUR arguments on this build - stock Lua 5.1 has three. The
	// extra one only gates the 'u' branch's nparams/isvararg writes, so passing 0 is the stock
	// behaviour. Only ask for "nSl": those three fill pointers and ints in ar, whereas 'f' and 'L'
	// PUSH onto the Lua stack, which an error path must not do.
	using lua_getinfoT  = int(void* L, const char* what, void* ar, int extra);
	// luaL_traceback(B, L, msg, level) - the errfunc LUI wraps its pcalls in, and our hook point for
	// reading an error's call stack before it unwinds. See dump_anchors.hpp.
	using luaL_tracebackT = std::uint64_t(void* B, void* L, const char* msg, int level);

	// PlayerData_OnStorageOpComplete(controller, opKind, resultCode, entry) -> opaque.
	// opKind 0 = read completion, 1 = write completion. `entry` is the playerdata entry itself
	// (entry+0 is the dataMapId, entry+120 the LOADED flag this function writes), NOT a job handle.
	// Runs on the storage job thread as well as the main thread — anything hooking it must be
	// thread-safe and must not call into Lua.
	using PlayerData_OnStorageOpCompleteT = void*(unsigned int controller, int opKind,
		unsigned int resultCode, unsigned int* entry);

	// PlayerData_AllocateStoreAndBuildEntries() — takes nothing, returns nothing we use. Its only
	// value to us is WHEN it runs: PlayerData_Init calls it exactly once, straight after the def
	// loop that would have applied the force-local-storage rewrite. See kDump_* for why that is the
	// only moment a dwuser->hdd rewrite can still be picked up by the load driver.
	using PlayerData_AllocateStoreAndBuildEntriesT = void*();

	// PlayerData_ControllerStorageTick(controller) — the per-frame load driver. Return value is a
	// "did something / bail out early" byte we neither read nor alter.
	using PlayerData_ControllerStorageTickT = char(unsigned int controller);

	// PlayerData_PushDdlInstanceToLua(L, controller, dataMapId, version) — the body behind the LUI
	// native the online frontend's director menu raises on. Confirmed against the decompiler.
	//
	// Hooked here rather than at the lua_CFunction wrapper (0x7FF71E9450A0) because the wrapper is a
	// one-argument lua_CFunction whose real arguments are still on the Lua value stack; by this point
	// the engine has already decoded them into registers.
	//
	// The return is the Lua return COUNT and is always 1, on both branches. A detour learns nothing
	// from it — the branch has to be read off the value the callee pushed. See the impl.
	using PlayerData_PushDdlInstanceToLuaT = std::uint64_t(void* L, unsigned int controller,
		unsigned int dataMapId, unsigned int version);

	// luaG_getobjname(L, Proto*, Instruction* pc, unsigned reg, const char** nameOut)
	//
	// Returns the "namewhat" string ("local"/"global"/"field"/"method"/"upvalue"/"xhashfunc"), or
	// null when it cannot work out what the register holds, and writes the name itself through
	// nameOut. Hooked ONLY to repair one engine bug on one of those returns - see the detour in
	// hooks/impl/game/luaL_traceback.cpp. No Arxan caller guard on this one (checked in IDA), so the
	// trampoline can be called directly.
	using luaG_getobjnameT = const char*(void* L, void* proto, void* pc, unsigned int reg,
		const char** nameOut);

	// lua_Debug field offsets for this build. Recovered by cross-reading lua_getinfo's writes
	// against luaL_traceback's reads of the same struct - the traceback gates " in function '%s'"
	// on +16 and prints +8, which is Lua 5.1's `if (*ar.namewhat) ... ar.name` and pins the pair.
	inline constexpr std::size_t kLuaDebug_Name       = 8;    // const char*, may be null
	inline constexpr std::size_t kLuaDebug_NameWhat   = 16;   // const char*, "" when unknown
	inline constexpr std::size_t kLuaDebug_What       = 24;   // "Lua" / "C" / "main"
	inline constexpr std::size_t kLuaDebug_Source     = 32;   // const char*
	inline constexpr std::size_t kLuaDebug_CurrentLine = 40;  // int
	inline constexpr std::size_t kLuaDebug_LineDefined = 48;  // int
	inline constexpr std::size_t kLuaDebug_ShortSrc   = 56;   // char[]
	inline constexpr std::size_t kLuaDebug_ICI        = 316;  // int, packed (hi16 slots, lo16 base)
	// The 'u' branch writes +320/+324, so the struct is at least 328 bytes. We allocate far more
	// than that: guessing this size low means lua_getinfo writes past the end of our buffer.
	inline constexpr std::size_t kLuaDebugSize        = 512;

	// Lua pseudo-indices.
	inline constexpr int kLuaGlobalsIndex = -10002;
	inline constexpr int kLuaRegistryIndex = -10000;

	// --- Session start / teardown -----------------------------------------------------------------
	// Session_StartSession(slot, descriptor) -> bool. The only nonzero writer of slot+68, which IS
	// Session_IsSlotActive's whole test. Returns 0 on either of its two early exits (descriptor
	// already matches an active session, or NetSession_BuildLocalNetadr failed) and 1 having set
	// +68 = 2.
	using Session_StartSessionT = char(void* slot, void* descriptor);

	// LobbyVM_RequestStartSession(actionId, sessionType). The Lua-side ask: reached from the LUI
	// native at 0x7FF71E936910, which pulls both ints off the value stack. It gathers slot-0's
	// descriptor plus the local player count and game mode and forwards to the dispatcher that runs
	// LobbyVM_Action_StartSession. Hooked here rather than at the native because the arguments are
	// already in registers.
	using LobbyVM_RequestStartSessionT = std::uint64_t(int actionId, std::uint64_t sessionType);

	// Session_EndSession(slot). ONE argument — the "ending session to start a new one" string at the
	// call site inside Session_StartSession is left in rdx and never read. Clears +68 and sets
	// +72 |= 6 among much else.
	using Session_EndSessionT = void(void* slot);

	// Session_ResetSlot(type, idx, _). Indexes the two slot arrays itself rather than taking a slot
	// pointer. Clears +68 and sets +72 |= 7 — the same +72 bits Session_StartSession sets, which is
	// exactly why the two cannot be told apart from a memory dump.
	using Session_ResetSlotT = std::uint64_t(int type, unsigned int idx, std::uint64_t unused);

	// NetSession_BuildLocalNetadr(outNetadr, outSerialized, descAddr) -> bool. Session_StartSession's
	// gate. Measured to be a non-issue online (the net session publishes fine), but traced anyway:
	// this is the link the previous theory died on, and an unwatched link is how it gets resurrected.
	using NetSession_BuildLocalNetadrT = char(void* outNetadr, void* outSerialized, void* descAddr);

	// --- ZM progression outside LIVE (zm_progression.cpp). See dump_anchors.hpp. ------------------
	// Both gates take four register args they only forward to Dvar_GetBool; kept so the detour passes
	// the registers through untouched.
	using StatsGateT = bool(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
	using LiveStorage_BeginStatsTransferT = char(int controller);
	using LiveStorage_CommitStatsTransferT = std::uint64_t(int controller, int source, unsigned int checksum, char final);
	using G_AddPlayerRankXpT = std::uint64_t(std::int16_t clientNum, std::int64_t xp, std::int64_t xpType, std::uint64_t eventHash);

	using LiveUser_GetXuidIfSignedInT = std::uint64_t(int controller);
	using PlayerData_GetBufferT = void*(std::int64_t controller, unsigned int dataMapId, int version);
	using PlayerData_IsBufferReadyT = bool(int controller, unsigned int dataMapId, int version);
	using StatsMapIdT = unsigned int();
	using StatsTransfer_IsValidForControllerT = bool(unsigned int controller);
	using BoolFnT = bool();
	using SV_IsClientStatsSlotReadyT = bool(std::int16_t clientNum, int source);
	using Dvar_GetBoolT = bool(void* dvar);
	using LiveStorage_GetStatsDdlRootForSourceT = void*(int source);
	using Ddl_IsInstanceValidT = bool(void* instance);
	using Ddl_InitRootStateT = void*(void* state, void* instance);
	using Ddl_MoveToMemberByHashT = bool(void* inState, void* outState, const std::uint64_t* hash);
	using Ddl_GetUInt64T = std::uint64_t(void* state, void* instance);
	using Ddl_SetUInt64T = bool(void* state, void* instance, std::uint64_t value);
	using Ddl_ParseHeaderT = void*(void* out, const void* buffer, char headerless);

	// The AE block (account XP) outside LIVE. See dump_anchors.hpp.
	using LiveStats_GetLiveUserStatsInstanceT = void*(int controller);
	using LiveStats_GetAeRootStateSlotT = void*(int controller);
	using Lua_PushAeSyncBufferT = std::uint64_t(void* luaState, int controller);
	using LiveStats_GetXpT = unsigned int(void* instance, unsigned int season);
	using LiveStats_SetXpT = bool(void* instance, unsigned int season, int xp);
	using LiveStats_SetAarModelFlagT = std::uint64_t(std::int64_t controller, std::uint64_t member, std::uint64_t value);
	using Rank_GetLevelForXpT = int(int xp);
	using Rank_GetMaxXpT = int();
	using SV_GetClientStatsInstanceT = void*(std::int16_t clientNum, int source);
	using Ddl_FindRootByHashT = void*(std::uint64_t hash, int unused);
	using Ddl_InitInstanceT = std::uint64_t(void* buffer, int size, void* root, void* instance,
		std::uint64_t, std::uint64_t, std::uint64_t);
	using Ddl_GetValueT = std::uint64_t(void* state, void* instance);
	using Ddl_PushStateToLuaT = std::uint64_t(void* luaState, void* state, void* instance);
	using Ddl_ScrambleInstanceBufferT = void(void* instance, void* buffer, unsigned int size, unsigned int key);
	using Ddl_UnscrambleInstanceBufferT = void(void* instance, void* buffer, unsigned int size);
	// Gun XP / camo challenges (the server's AE slot) and the LIVE menu switch. See dump_anchors.hpp.
	using SV_ClientStatsReadyT = char(std::uint8_t* svClient);
	using LobbyRoot_SetNetworkModeModelT = std::uint64_t(int mode);
	using Lua_GameModeIsMode_ImplT = std::uint64_t(void* luaState, int mode);
	using Lua_Engine_GetLobbyNetworkModeT = std::uint64_t(void* luaState);

	// "unlock_all" (unlock_all.cpp). See dump_anchors.hpp. The lock predicates return true/1 for LOCKED.
	using Progression_IsItemLockedT = bool(int mode, unsigned int controller, int itemIndex);
	using Progression_IsAttachmentLockedInBlockT = char(int mode, void* statsBlock, unsigned int itemIndex,
		int slot, char a5);
	using Progression_IsAttachmentSlotLockedT = bool(unsigned int mode, unsigned int controller,
		unsigned int itemIndex, int slot);
	using Progression_IsItemOptionLockedCoreT = char(unsigned int mode, unsigned int controller,
		unsigned int itemIndex, unsigned int optionIndex, char skipPrerequisite);
	using Com_SessionMode_IsProgressionExemptContextT = bool();
	using Inventory_GetItemQuantityT = std::uint64_t(int controller, unsigned int itemId);
	// The last two are only forwarded to Dvar_GetBool; kept so the detour passes the registers on.
	using Loot_GetItemQuantityT = std::uint64_t(unsigned int controller, std::uint64_t itemId, std::uint64_t,
		std::uint64_t);
	using Entitlement_IsOwnedT = char(int controller, std::uint64_t nameHash);
	using DwFetch_IsInventoryReadyT = bool(unsigned int controller);
	using Loot_GetBattlePassOwnedT = char(unsigned int controller, int season);
	using Loot_GetBattlePassRankT = std::uint64_t(unsigned int controller, int season);
	using Loot_UpdateBattlePassModelsT = void(int controller);
	using LiveStorage_AreStatsReadableT = bool(unsigned int controller);

	// Local LPC playlists (local_lpc.cpp). See dump_anchors.hpp.
	using IntFnT = int();
	using CStrFnT = const char*();
	using Loc_GetLanguagePrefixT = const char*(unsigned int language);
	using FS_AddSearchPathT = void(const char* path, int priority, int device, std::uint64_t unused);
	using DB_ZoneFileExistsT = bool(const char* zoneName);
	struct XZoneInfo {
		const char* name;
		std::uint32_t flags;
		std::uint32_t pad;
	};
	using DB_LoadXAssetsT = void(XZoneInfo* zones, std::uint32_t count, int freeFlags);
	using VoidFnT = void();
	using R_RemoteScreenUpdateAllowT = void(bool allow);
	using OnlineContent_SlotCallbackT = std::uint64_t();
	using DB_AllocXAssetEntryT = void*(std::uint64_t type, std::uint64_t zone);

	// mapkit custom maps (mapkit_loader.cpp). See dump_anchors.hpp.
	using DB_Signature_VerifyZoneT = void();
	using FS_OpenFileReadT = void*(const char* path, int mode, int device);
	using SV_StartMapT = std::uint64_t(std::uint32_t a1, const char* mapName, std::uint32_t kind, std::uint8_t a4,
		std::uint32_t a5);
	using MapPreload_StartZoneReadT = std::uint64_t(const char* mapName, std::uint64_t a2, std::uint64_t a3,
		std::uint64_t a4);
	using DB_ExpandZoneVariantsT = int(XZoneInfo* zones, int count, char* names, XZoneInfo* out, int max);
	using R_InitWorldT = void();
	// A map of its own (mapkit_loader.cpp).
	using Session_SetMapNameT = char(std::uint32_t slot, const char* mapName);
	using Session_PickActiveSlotT = int(int type);
	using MapTable_FindEntryByHashT = void*(std::uint64_t mapHash);
	using MapTable_GetMapFlagsT = std::uint32_t(const char* mapName);
	using BG_Cache_FindIndexT = std::int64_t(std::uint8_t table, std::uint64_t name);

	// mapkit zone trace (mapkit_trace.cpp). See dump_anchors.hpp.
	using DB_LoadXFile_InternalT = std::int64_t(std::int64_t a1, std::int64_t a2, const char* zoneName, int a4,
		void* assetList, void* blocks, std::uint64_t a7, int a8, int flags);
	using Load_XAssetT = std::int64_t(char atStreamStart, std::uint8_t* asset);
	using DB_ReadXFileT = void(void* dst, int size);
	using DB_ReadXFileStringT = void(std::uint8_t* dst, std::uint32_t* count);
}
