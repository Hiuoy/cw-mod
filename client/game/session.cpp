// Read-only inspector for the LAN net-session chain (lever 3, host side).
#include "common.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/dw_backend.hpp"
#include "game/dump_anchors.hpp"
#include <utility/nt.hpp>

#include <filesystem>
#include <fstream>

namespace Client::Game {
	// --- Online session mode -----------------------------------------------------------------------
	// Which frontends the game builds is a function of the session network mode, decided long before
	// the overlay exists. networkMode lives in the low nibbles of g_sessionModePacked:
	//   packed & 0xF   = gameMode,  (packed >> 4) & 0xF = networkMode (0 offline / 1 lan / 2 online),
	//   (packed >> 12) & 0xF = matchType.
	// Com_SessionMode_IsOnline() is literally (packed & 0xF0) == 32.
	namespace OnlineMode {
		void Tick() {
			if (!g_Pin.load(std::memory_order_relaxed) || !g_Requested.load(std::memory_order_relaxed)) {
				return;
			}
			if (!g_Pointers || !g_Pointers->m_g_sessionModePacked || !g_Pointers->m_Com_SessionMode_SetNetworkMode) {
				return;
			}
			// Plain global int — safe to read every frame. Only touch the engine when it has drifted.
			const std::uint32_t packed = *g_Pointers->m_g_sessionModePacked;
			if (((packed >> 4) & 0xF) != 2) {
				g_Pointers->m_Com_SessionMode_SetNetworkMode(2);
			}
		}
	}

	// --- B5 diagnostics ------------------------------------------------------------------------------
	// Controller 0 only: the online frontend and the login driver run on controller 0. Plain in-module
	// globals plus the LiveUser object, all through SafeRead; nothing here calls into the engine.
	namespace EntitlementGates {
		void Tick() {
			if (!OnlineMode::g_Requested.load(std::memory_order_relaxed)) {
				return;
			}

			static std::uintptr_t s_ModBase = 0, s_ModSize = 0;
			if (!s_ModBase) {
				const Common::Utility::NT::Library game;
				s_ModBase = reinterpret_cast<std::uintptr_t>(game.GetPtr());
				s_ModSize = game.GetOptionalHeader()->SizeOfImage;
			}
			auto at = [](std::uintptr_t dumpAbs, std::ptrdiff_t offset = 0) -> const void* {
				const std::uintptr_t a = s_ModBase + (dumpAbs - kDumpImagebase) + offset;
				return (a >= s_ModBase && a < s_ModBase + s_ModSize) ? reinterpret_cast<const void*>(a) : nullptr;
			};

			int pubVars = -1, slot0 = -1, slot1 = -1, mtxSync = -1, invState = -1, invCount = -1;
			std::uint8_t invLoaded = 0xEE;
			SafeRead(at(kDump_g_pubVarsState), pubVars);
			SafeRead(at(kDump_g_onlineContentSlots), slot0);
			SafeRead(at(kDump_g_onlineContentSlots, 4), slot1);
			SafeRead(at(kDump_g_mtxSyncState), mtxSync);
			SafeRead(at(kDump_g_inventory, kInventory_State), invState);
			SafeRead(at(kDump_g_inventory, kInventory_ItemCount), invCount);
			SafeRead(at(kDump_g_inventory, kInventory_LoadedFlag), invLoaded);

			// B3 bypass (2026-09-16, option 2): mark both content slots loaded. A slot only counts as
			// installed when its core_playlists / core_ffotd pair is in the LPC manifest, and loading the
			// only files we have (TU35 renamed to tu34) ERR_DROPs in DB_AllocXAssetEntry, so the server
			// serves an empty list. With both slots at 2, OnlineContent_LoaderFrame's scan returns
			// before loading any zone; the cost is that the slot post-load callbacks never run, so there
			// is no playlist unless game/local_lpc.cpp loads it. Both must read 2 before any mode switch:
			// Com_ApplyGameModeSwitch spins until they do. Written only once PubVars is ready, which is
			// the loader's own precondition and long after its one-time slot reset.
			if (DwBackend::g_Enabled && pubVars == 4 && (slot0 != 2 || slot1 != 2)) {
				const bool ok = SafeWrite(const_cast<void*>(at(kDump_g_onlineContentSlots)), 2)
					&& SafeWrite(const_cast<void*>(at(kDump_g_onlineContentSlots, 4)), 2);
				LOG("Entitlements", INFO, "content slots {},{} -> 2,2 (bypass: no loadable TU34 LPC zones) {}",
					slot0, slot1, ok ? "written" : "WRITE FAILED");
				if (ok) {
					slot0 = slot1 = 2;
				}
			}

			// B7 (2026-09-24): ZM Start hosts its own game lobby. The Start handler targets
			// director_online_private (CreateGameLobbyNONMatchmaking: host the game lobby here, join our own
			// XUID) unless kDvar_ZmStartUsesDedicated is true, and the exe registers it true. The public
			// path waits for datacenter QoS pings, a relay token and an async matchmaking search, none of
			// which exist, so every Start failed after 12.5 s with "Failed to host lobby" (21:48-22:02).
			// Nothing native reads the dvar, so this only changes the Lua routing. Checked every frame in
			// case playlist rules set it back.
			if (DwBackend::g_Enabled && g_Pointers && g_Pointers->m_Dvar_FindVar) {
				static int s_Flips = 0;
				if (auto* const dvar = g_Pointers->m_Dvar_FindVar(kDvar_ZmStartUsesDedicated)) {
					const std::uint8_t* values = nullptr;
					std::uint8_t value = 0;
					if (SafeRead(reinterpret_cast<const std::uint8_t*>(dvar) + Pointers::kDvar_Values, values)
						&& values && SafeRead(values, value) && value) {
						g_Pointers->WriteDvarBool(dvar, false);
						std::uint8_t after = 0xEE;
						SafeRead(values, after);
						if (++s_Flips <= 5) {
							LOG("Entitlements", INFO, "ZM Start routing: dedicated-server dvar 1 -> {} (flip #{}{}), "
								"so Start opens director_online_private (self-hosted) instead of the DS matchmaking path.",
								after, s_Flips, s_Flips > 1 ? ": something set it back" : "");
						}
					}
				}
			}

			std::uint64_t field5776 = ~0ull;
			std::uint8_t active5760 = 0xEE;
			int signin = -1;
			std::uintptr_t user = 0;
			if (g_Pointers && g_Pointers->m_g_liveUserObjects
				&& SafeRead(reinterpret_cast<const void*>(g_Pointers->m_g_liveUserObjects), user) && user) {
				SafeRead(reinterpret_cast<const void*>(user + kLiveUser_Field5776), field5776);
				SafeRead(reinterpret_cast<const void*>(user + kLiveUser_ActiveFlag5760), active5760);
				SafeRead(reinterpret_cast<const void*>(user + kLiveUser_SigninState), signin);
			}

			// LiveUser_IsGuestLike_Flag20(0): controller 0 -> client index -> flags & 0x20. -1 = no mapping.
			int clientIdx = -1, clientFlags = -1;
			for (int row = 0; row < 2; ++row) {
				int key = -1, value = -1;
				if (SafeRead(at(kDump_g_controllerClientMap, row * 60 + 4), key) && key == 0
					&& SafeRead(at(kDump_g_controllerClientMap, row * 60), value)) {
					clientIdx = value;
					break;
				}
			}
			if (clientIdx >= 0 && clientIdx < 8) {
				SafeRead(at(kDump_g_clientFlags, 2084 * 4 * clientIdx), clientFlags);
			}

			// B6 padlocks: the lobby the mode tiles need. Per slot "active/state256" (Session_IsSlotActive
			// = active > 0; the tiles' Engine[0x10EA2BE00F49480D] also wants state256 == 2). t0 = type 0,
			// t1 = type 1 (the Lua lobbies). Plus the Korean PC-bang flag, which padlocks every non-MP
			// tile with no action (FirstParty_IsKoreanIGR_cand); '?' = unreadable.
			std::string lobbySlots;
			for (int type = 0; type < 2; ++type) {
				lobbySlots += std::format("{}t{}=", type ? " " : "", type);
				for (int idx = 0; idx < 3; ++idx) {
					const std::ptrdiff_t off = static_cast<std::ptrdiff_t>(
						idx * (type ? kSessionSlot_TypeNStride : kSessionSlot_Type0Stride));
					const std::uintptr_t slots = type ? kDump_g_sessionSlots_typeN : kDump_g_sessionSlots_type0;
					int active = -1, state = -1;
					SafeRead(at(slots, off + static_cast<std::ptrdiff_t>(kSessionSlot_Active)), active);
					SafeRead(at(slots, off + static_cast<std::ptrdiff_t>(kSessionSlot_State256)), state);
					lobbySlots += std::format("{}{}/{}", idx ? "," : "", active, state);
				}
			}
			int igr = -1;
			if (g_Pointers && g_Pointers->m_g_firstPartyManager) {
				std::uintptr_t mgr = 0, account = 0;
				std::uint8_t flag = 0;
				if (SafeRead(g_Pointers->m_g_firstPartyManager, mgr) && mgr
					&& SafeRead(reinterpret_cast<const void*>(mgr + kFirstPartyMgr_Account), account) && account
					&& SafeRead(reinterpret_cast<const void*>(account + kFirstPartyAccount_IgrFlag), flag)) {
					igr = flag;
				}
			}

			// Log on any change, plus a heartbeat so a long quiet stretch still shows the values.
			const auto key = std::make_tuple(pubVars, slot0, slot1, mtxSync, invState, invCount, invLoaded, field5776 != 0,
				active5760, signin, clientIdx, clientFlags, lobbySlots, igr);
			static std::remove_const_t<decltype(key)> s_Last{};
			static bool s_Logged = false;
			static std::uint64_t s_LastLogMs = 0;
			const std::uint64_t now = GetTickCount64();
			if (s_Logged && key == s_Last && now - s_LastLogMs < 30000) {
				return;
			}
			const bool changed = !s_Logged || key != s_Last;
			s_Last = key;
			s_Logged = true;
			s_LastLogMs = now;

			LOG("Entitlements", INFO,
				"{} pubVars={} (4=ready) contentSlots={},{} (2=loaded) mtxSync={} (0 idle/1 waiting Bnet ZEUS token/"
				"2 sync sent/3 done) inventory state={} (4=loaded) items={} loadedFlag={} liveUser+5776={} | MtxSync block: "
				"active5760={} (needs !=0) signin={} (needs 2) client={} flags=0x{:X} (0x20 = guest-like, skips MtxSync) | "
				"lobby slots active/state256: {} (tiles need t1 active) | KR IGR flag={}",
				changed ? "changed:" : "heartbeat:", pubVars, slot0, slot1, mtxSync, invState, invCount,
				static_cast<int>(invLoaded), field5776 == ~0ull ? "?" : (field5776 ? "set" : "0"),
				static_cast<int>(active5760), signin, clientIdx, static_cast<std::uint32_t>(clientFlags),
				lobbySlots, igr < 0 ? std::string("?") : std::to_string(igr));
		}
	}

	// Read-only LAN-session inspector. Chases g_netSessionManager -> session(+672) -> state(+680) plus the
	// announce(+688)/search(+696) handler states (field offsets verified from NetSession_NegotiateTick /
	// SessAnnounce_GetState +320 / SessSearch_GetState +468). Tells us on ONE PC whether the host lobby is
	// advertising over LAN. Pure pointer-chase reads with null guards; runs on the game thread (same thread as
	// the ~1Hz pump), so nothing mutates the chain mid-read. See docs/phase3_netcode.md.
	std::string Pointers::DumpSessionState() const {
		std::string out;
		auto line = [&](const std::string& s) { out += s; out += '\n'; };

		// A live C++ engine object's first qword is a vtable pointer into the module's .rdata. If the object
		// was freed and its memory reused, that qword is heap junk (not in-module). This is how we tell a
		// LIVE session from a dangling pointer left in manager+672 after teardown.
		const Common::Utility::NT::Library game;
		const std::uintptr_t modBase = reinterpret_cast<std::uintptr_t>(game.GetPtr());
		const std::uintptr_t modEnd  = modBase + game.GetOptionalHeader()->SizeOfImage;
		auto vtableInModule = [&](const void* obj) -> bool {
			std::uintptr_t vt = 0;
			if (!SafeRead(obj, vt)) return false;
			return vt >= modBase && vt < modEnd;
		};

		const int phase = this->m_g_hostLaunchPhase ? *this->m_g_hostLaunchPhase : -1;
		const int lstate = this->m_g_netSessionLaunchState ? *this->m_g_netSessionLaunchState : -1;
		line(std::format("g_hostLaunchPhase = {}   g_netSessionLaunchState = {}", phase, lstate));

		if (!this->m_g_netSessionManager) {
			line("g_netSessionManager anchor unresolved (build mismatch?).");
			return out;
		}
		// Every deref below goes through SafeRead: the manager/session struct layout is from RE, not verified
		// against this exact build, so a wrong offset (or a stale/torn-down object) must produce a diagnostic
		// line, never an access violation. A "FAULTED" line pinpoints exactly which read is bad.
		std::uint8_t* manager = nullptr;
		if (!SafeRead(this->m_g_netSessionManager, manager)) {
			line("FAULTED reading *g_netSessionManager — the anchor address itself is bad (build mismatch?).");
			return out;
		}
		if (!manager) {
			line("NetSession manager = NULL  =>  no net-session manager exists (no LAN/online session ever started).");
			return out;
		}
		line(std::format("NetSession manager @ 0x{:X}  ({})", reinterpret_cast<std::uintptr_t>(manager),
			vtableInModule(manager) ? "vtable in-module = LIVE object"
			                        : "vtable NOT in-module = manager itself looks freed/invalid"));

		std::uint8_t* session = nullptr;
		if (!SafeRead(manager + 672, session)) {
			line(std::format("FAULTED reading manager+672 (session ptr) — +672 offset wrong or manager @0x{:X} is not the real object.",
				reinterpret_cast<std::uintptr_t>(manager)));
			return out;
		}
		if (!session) {
			line("Active session = NULL  =>  manager exists but there is NO active session (nothing advertising on LAN).");
			return out;
		}
		// Offsets +680/+688/+696 are VERIFIED from NetSession_SetState / NetSession_NegotiateTick. So if the
		// reads below look like junk, the session OBJECT is stale, not the offsets. Check its vtable to prove it.
		if (!vtableInModule(session)) {
			line(std::format("Active session ptr = 0x{:X} but its vtable is NOT in-module  =>  DANGLING POINTER: the",
				reinterpret_cast<std::uintptr_t>(session)));
			line("  LAN session was created + launched (g_netSessionLaunchState reached 8) then TORN DOWN. manager+672");
			line("  still holds the freed pointer. This is why the host isn't discoverable and the UI stayed on the ZM");
			line("  menu. The session does not persist after launch — that's the lifetime bug to fix next, not an offset.");
			return out;
		}
		line(std::format("Active session @ 0x{:X}  (vtable in-module = LIVE object)",
			reinterpret_cast<std::uintptr_t>(session)));

		// The IDB-verified offsets (+680 state, +688/+696 handlers) read junk on the LIVE object even though the
		// object is provably alive. So the running layout differs from the dump at these offsets. Rather than
		// trust a fixed offset, dump the raw bytes and AUTO-LOCATE the real fields empirically:
		//  - state = a lone dword valued 1 (negotiating) or 2 (published) — a small int amid mostly-zero/pointer words
		//  - announce/search handlers = qwords that are either NULL or a heap pointer into the same arena as `session`
		const std::uintptr_t sess = reinterpret_cast<std::uintptr_t>(session);
		auto looksLikeHeapPtr = [&](std::uintptr_t v) {
			// same high-32-bit heap region as the session, 8-byte aligned — a plausible sibling allocation.
			return v > 0x10000 && (v & 7) == 0 && (v >> 32) == (sess >> 32);
		};

		// Raw qword dump of the window that should hold state + the two handler pointers (session +0x2A0..+0x2C8),
		// so we can eyeball the true layout, plus an auto-scan over a wider window for state/handler candidates.
		line("  --- raw session bytes (offset : qword) ---");
		for (unsigned off = 0x2A0; off <= 0x2C8; off += 8) {
			std::uintptr_t q = 0;
			if (SafeRead(session + off, q))
				line(std::format("    +0x{:03X} (={:4}) : 0x{:016X}", off, off, q));
			else
				line(std::format("    +0x{:03X} (={:4}) : <faulted>", off, off));
		}
		line("  --- auto-scan session +0x100..+0x400 ---");
		std::string stateHits, ptrHits;
		for (unsigned off = 0x100; off <= 0x400; off += 4) {
			unsigned d = 0;
			if (SafeRead(session + off, d) && (d == 1 || d == 2))
				stateHits += std::format(" +0x{:X}(={})", off, d);
		}
		for (unsigned off = 0x100; off <= 0x400; off += 8) {
			std::uintptr_t q = 0;
			if (SafeRead(session + off, q) && looksLikeHeapPtr(q) && vtableInModule(reinterpret_cast<void*>(q)))
				ptrHits += std::format(" +0x{:X}->0x{:X}", off, q);
		}
		line(std::format("  candidate state offsets (dword==1|2):{}", stateHits.empty() ? " none" : stateHits));
		line(std::format("  candidate handler ptrs (heap+vtable):{}", ptrHits.empty() ? " none" : ptrHits));
		return out;
	}

	// Read-only: WHY does Demonware login never start? Login_SetStatus (hooked, mirrors every login state
	// transition) stays silent, which means the DW login state machine never runs — i.e. the login
	// controller at LiveUserObj+24 is never created/pumped. This snapshots the whole precondition chain so
	// we can see exactly where it stops: the LiveUser system master gate, the nodw dvar, and per-controller
	// the sign-in state (+5764==2 means signed in), the login-driver state (+5768, ==4 means online enabled),
	// the DW login controller ptr (+24), and the stats container (+40024). All SEH-guarded; no engine calls.
	std::string Pointers::DumpDwLoginState() const {
		std::string out;
		auto line = [&](const std::string& s) { out += s; out += '\n'; };

		// Master gate: LiveUserSystem_Tick only ticks the login drivers when this byte is set.
		if (this->m_g_liveUserSystemActive) {
			std::uint8_t active = 0xEE;
			if (SafeRead(this->m_g_liveUserSystemActive, active))
				line(std::format("g_liveUserSystemActive = {}  ({})", active,
					active ? "LiveUser update loop RUNS -> login drivers tick"
					       : "LiveUser update loop is OFF -> login drivers NEVER tick (nodw/flow-9 never consulted)"));
			else
				line("g_liveUserSystemActive: <faulted>");
		} else {
			line("g_liveUserSystemActive anchor unresolved (build mismatch?).");
		}

		// nodw dvar current value. *m_Dvar_NoDW is the dvar_t*; the bool value lives at the value block
		// (dvar+16 -> block, first dword) — we also print the inline dword at dvar+24 as a cross-check.
		if (this->m_Dvar_NoDW) {
			std::uintptr_t dvar = 0;
			if (SafeRead(this->m_Dvar_NoDW, dvar) && dvar) {
				std::uintptr_t valBlock = 0; int inlineVal = -1, blockVal = -1;
				SafeRead(reinterpret_cast<const void*>(dvar + kDvar_Values), valBlock);
				SafeRead(reinterpret_cast<const void*>(dvar + 24), inlineVal);
				if (valBlock) SafeRead(reinterpret_cast<const void*>(valBlock), blockVal);
				line(std::format("nodw dvar @0x{:X}: value(block)={} value(+24)={}  (true => login driver refuses to connect)",
					dvar, blockVal, inlineVal));
			} else {
				line("nodw dvar: <faulted or null>");
			}
		}

		// Force-login-attempt preconditions we could not see before. g_liveUserLoginAllowed is the byte
		// LiveUser_GetLoginAllowedFlag returns; live_connect_mode is gate B (must read 1); the first-party
		// manager tells us whether a client-side spoof of "signed in" would even have an object to write.
		if (this->m_g_liveUserLoginAllowed) {
			std::uint8_t la = 0xEE;
			if (SafeRead(this->m_g_liveUserLoginAllowed, la))
				line(std::format("g_liveUserLoginAllowed = {}  ({})", la, la ? "login permitted" : "login NOT permitted -> state-1 bails"));
			else
				line("g_liveUserLoginAllowed: <faulted>");
		}
		if (this->m_Dvar_LiveConnectMode) {
			std::uintptr_t dvar = 0;
			if (SafeRead(this->m_Dvar_LiveConnectMode, dvar) && dvar) {
				std::uintptr_t valBlock = 0; int blockVal = -1;
				SafeRead(reinterpret_cast<const void*>(dvar + kDvar_Values), valBlock);
				if (valBlock) SafeRead(reinterpret_cast<const void*>(valBlock), blockVal);
				line(std::format("live_connect_mode dvar @0x{:X}: value={}  (gate B needs 1)", dvar, blockVal));
			} else {
				line("live_connect_mode dvar: <faulted or null>");
			}
		}
		if (this->m_g_firstPartyManager) {
			std::uintptr_t mgr = 0;
			if (SafeRead(this->m_g_firstPartyManager, mgr)) {
				if (mgr) {
					int state = -1;
					SafeRead(reinterpret_cast<const void*>(mgr + 56), state);
					line(std::format("g_firstPartyManager = 0x{:X}  state(+56)={} ({})", mgr, state,
						state == 3 ? "SIGNED IN" : "not signed in"));
				} else {
					line("g_firstPartyManager = NULL  (no first-party manager object exists at all)");
				}
			} else {
				line("g_firstPartyManager: <faulted>");
			}
		}

		if (!this->m_g_liveUserObjects) {
			line("g_liveUserObjects anchor unresolved — cannot read per-controller state (open a map first, or build mismatch).");
			return out;
		}

		for (int c = 0; c < 2; ++c) {
			std::uintptr_t obj = 0;
			if (!SafeRead(reinterpret_cast<const void*>(
					reinterpret_cast<std::uintptr_t>(this->m_g_liveUserObjects) + c * sizeof(void*)), obj)) {
				line(std::format("ctrl {}: <faulted reading g_liveUserObjects[{}]>", c, c));
				continue;
			}
			if (!obj) {
				line(std::format("ctrl {}: LiveUser object = NULL  =>  no user context at all for this controller.", c));
				continue;
			}
			int signin = -1, loginState = -1;
			std::uint8_t flag5761 = 0xEE;
			std::uintptr_t dwCtrl = 0, container = 0;
			SafeRead(reinterpret_cast<const void*>(obj + kLiveUser_SigninState), signin);
			SafeRead(reinterpret_cast<const void*>(obj + kLiveUser_OnlineState), loginState);
			SafeRead(reinterpret_cast<const void*>(obj + kLiveUser_Flag5761), flag5761);
			SafeRead(reinterpret_cast<const void*>(obj + 24), dwCtrl);            // the bdLogin controller
			SafeRead(reinterpret_cast<const void*>(obj + kLiveUser_StatsContainer), container);
			line(std::format(
				"ctrl {}: obj=0x{:X}  signin(+5764)={} ({})  loginState(+5768)={}  dwLoginCtrl(+24)=0x{:X} ({})  "
				"container(+40024)=0x{:X}  flag5761={}",
				c, obj, signin, signin == 2 ? "SIGNED IN" : "not signed in", loginState,
				dwCtrl, dwCtrl ? "EXISTS -> should be pumped/emit status" : "NULL -> login never created",
				container, flag5761));
		}

		line("--- read: signin==2 => a user is signed in; loginState 0 idle / 1 begin / 4 online-enabled;");
		line("    dwLoginCtrl!=0 => the DW login state machine is live (Login_SetStatus would fire).");
		return out;
	}

}
