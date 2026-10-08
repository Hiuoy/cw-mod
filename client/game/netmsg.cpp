// Phase 3 lever-5: the netmsg transcript detours.
#include "common.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/join_log.hpp"
#include "memory/minhook.hpp"

#include <atomic>

namespace Client::Game {
	// --- Phase 3 lever-5: the netmsg transcript ---------------------------------------------------
	// Why this exists. A two-PC capture proved the machines DO complete a transport handshake and then
	// exchange application frames — but those frames are ciphertext, so the wire can show *that* they
	// talked and never *what* they said. These detours read the same messages inside the process,
	// after decryption, on both sides. Everything below is read-only: we log and call through.
	//
	// This is also the instrument that settles the open question. The client-side JoinResponse handler
	// (NetMsg_Handle_JoinResponse, 0x7FF727946C90) can only leave the FSM in state 4 (accepted),
	// 2 (code 47, retry) or 5 (any other refusal). The observed transition is 3 -> 1, which is NONE of
	// those — so either msgId 17 never reaches the handler, or it is discarded by the source-address
	// comparison that guards it. The dispatch log distinguishes those two outright.
	namespace {
		using NetMsg_Dispatch_t         = char (*)(unsigned int, void*, void*, void*);
		using NetMsg_SendJoinResponse_t = bool (*)(int, int, int, void*, void*, void*);
		using ParseJoinLobbyRequest_t   = bool (*)(void*, void*);

		Client::Memory::MinHook<void>* g_netMsgDispatchHook = nullptr;
		Client::Memory::MinHook<void>* g_netMsgJoinRespHook = nullptr;
		Client::Memory::MinHook<void>* g_netMsgParseReqHook = nullptr;
		NetMsg_Dispatch_t              g_origNetMsgDispatch = nullptr;
		NetMsg_SendJoinResponse_t      g_origSendJoinResponse = nullptr;
		ParseJoinLobbyRequest_t        g_origParseJoinReq = nullptr;
		std::atomic<bool>              g_netMsgTranscriptOn{ false };

		// Latched at install time so the detours — which are free functions on the game thread — never
		// have to reach back through the Pointers instance while a message is in flight.
		void**        g_netMsgNamesTbl   = nullptr;
		std::uint8_t* g_netMsgJoinCtx    = nullptr;
		std::uint8_t* g_netMsgExpectedAdr= nullptr;
		int*          g_netMsgRespCode   = nullptr;

		const char* NetMsgName(int msgId) {
			if (!g_netMsgNamesTbl || msgId < 0 || msgId >= 35) {
				return "?";
			}
			const char* s = nullptr;
			if (!SafeRead(&g_netMsgNamesTbl[msgId], s) || !s) {
				return "?";
			}
			return s;
		}

		// Verdict labels from NetMsg_Handle_JoinLobby (0x7FF726BF6380) and the client-side
		// NetMsg_Handle_JoinResponse (0x7FF727946C90). 1 is the only success. Each code below is pinned
		// to a concrete comparison against a NAMED wire field — the parser (0x7FF726BFB070) gives the
		// field names, and every stack variable in the decision function maps onto one of them. Labels
		// still marked (?) guard a predicate whose meaning is not yet pinned to a named field.
		const char* NetMsgJoinResponseLabel(int code) {
			switch (code) {
			case 1:  return "ACCEPTED";
			case 9:  return "lobbytype > 2, or party host != session host (?)";
			case 10: return "client: candidate roster hit its 127 cap (?)";
			case 18: return "host: joiner is on a block/ignore list (?)";
			case 33: return "host: reservation commit failed (?)";
			case 36: return "host: request networkmode != 1, OR jointype not in {1,4}";
			case 38: return "host: playlistver HIGHER than host's";
			case 39: return "host: playlistver LOWER than host's";
			case 40: return "host: playlistchecksum mismatch";
			case 42: return "host: netchecksum mismatch";
			case 43: case 44: return "host: ffotdver mismatch";
			case 45: return "host: featurechecksum mismatch (featuretable_default differs)";
			case 47: return "host: post-accept step failed -> client RETRIES (state 2)";
			case 53: case 54: case 55: case 56: return "client: platform/crossplay refusal (?)";
			case 60: return "client: crossplay disabled for this network mode (?)";
			case 62: return "host: local-player-count / split-screen mismatch (?)";
			default: return "(unmapped refusal — read the branch in NetMsg_Handle_JoinLobby)";
			}
		}

		// Voice traffic is the only high-rate message in the table and it tells us nothing about a join,
		// so it is the one thing we drop. Heartbeats (10 / 5) are deliberately KEPT: at ~1 Hz they are
		// cheap, and a heartbeat arriving from the far machine is exactly the kind of signal the test
		// plan calls admissible — it cannot be produced locally.
		bool NetMsgWorthLogging(int msgId) {
			return msgId != 30 && msgId != 31;
		}

		char hkNetMsg_Dispatch(unsigned int localClient, void* netadr, void* a3, void* msg) {
			int msgId = -1;
			if (msg) {
				SafeRead(static_cast<const std::uint8_t*>(msg) + Pointers::kNetMsg_MsgIdOffset, msgId);
			}

			const bool log = NetMsgWorthLogging(msgId);
			std::uint8_t adr[16]{};
			int before = -1;
			if (log) {
				SafeCopy(adr, netadr, sizeof(adr));
				if (g_netMsgJoinCtx) SafeRead(g_netMsgJoinCtx, before);
			}

			const char result = g_origNetMsgDispatch
				? g_origNetMsgDispatch(localClient, netadr, a3, msg)
				: 0;

			if (log) {
				int after = -1;
				if (g_netMsgJoinCtx) SafeRead(g_netMsgJoinCtx, after);

				// result == 0 means the dispatcher fell off the end of its 14-entry table: the message
				// arrived and was DROPPED because nothing is registered for that msgId. Worth calling
				// out separately from "handled" — it is a different failure.
				std::string line = std::format(
					"[netmsg] {}  recv msgId={:2} {:<22} from {}  client={}  handled={}",
					WallClockNow(), msgId, NetMsgName(msgId), HexBytes(adr, sizeof(adr)),
					localClient, result ? "yes" : "NO (no handler registered)");

				if (before != after) {
					line += std::format("   joinState {} ({}) -> {} ({})",
						before, JoinStateLabel(before), after, JoinStateLabel(after));
				} else if (before >= 0) {
					line += std::format("   joinState {} ({})", before, JoinStateLabel(before));
				}

				// msgId 17 is the whole reason this hook exists: print the verdict the client just
				// stored, and — when the FSM did not move — the address comparison that most likely
				// ate it. g_expectedHostAdr is what NetMsg_Handle_JoinResponse checks the source against.
				if (msgId == 17) {
					int code = -1;
					if (g_netMsgRespCode && SafeRead(g_netMsgRespCode, code)) {
						line += std::format("\n[netmsg]            verdict={} ({})", code, NetMsgJoinResponseLabel(code));
					}
					// Informational only. A live run showed these two differing in the LAST DWORD while the
					// handler still ran and moved the FSM — so that dword is not part of the engine's
					// comparison (sub_7FF728FBDAA0). Never read a difference here as "the reply was
					// dropped"; `handled` and the joinState transition are the authority on that.
					std::uint8_t expect[16]{};
					if (g_netMsgExpectedAdr && SafeCopy(expect, g_netMsgExpectedAdr, sizeof(expect))) {
						const bool same = std::memcmp(expect, adr, sizeof(adr)) == 0;
						line += std::format("\n[netmsg]            expected host adr {}  ({})",
							HexBytes(expect, sizeof(expect)),
							same ? "identical to source" : "differs from source (FYI — last dword is not compared)");
					}
				}

				JcAppend(line);
			}
			return result;
		}

		// Host side. Fires just before the verdict is decided, with every field the decision reads.
		// The gates are all equality tests against these values, so printing them next to the resulting
		// code is the difference between "refused with 36" and "refused because networkmode was 0".
		bool hkSession_ParseJoinLobbyRequest(void* block, void* msg) {
			const bool ok = g_origParseJoinReq ? g_origParseJoinReq(block, msg) : false;
			if (!ok || !block) {
				JcAppend(std::format("[netmsg] {}  recv JoinLobby request — PARSE FAILED (malformed or truncated)",
					WallClockNow()));
				return ok;
			}

			const auto* b = static_cast<const std::uint8_t*>(block);
			auto i32 = [&](std::size_t off) { int v = -1; SafeRead(b + off, v); return v; };

			const int jointype    = i32(Pointers::kJoinReq_JoinType);
			const int networkmode = i32(Pointers::kJoinReq_NetworkMode);

			JcAppend(std::format(
				"[netmsg] {}  recv JoinLobby request:\n"
				"[netmsg]            targetlobby={} sourcelobby={} jointype={} membercount={} splitscreen={}\n"
				"[netmsg]            networkmode={} playlistid={} playlistver={} playlistchecksum={}\n"
				"[netmsg]            ffotdver={} netchecksum={} featurechecksum={} tuVersion={} protocol={} changelist={}",
				WallClockNow(),
				i32(Pointers::kJoinReq_TargetLobby), i32(Pointers::kJoinReq_SourceLobby), jointype,
				i32(Pointers::kJoinReq_MemberCount), i32(Pointers::kJoinReq_SplitScreen),
				networkmode, i32(Pointers::kJoinReq_PlaylistId), i32(Pointers::kJoinReq_PlaylistVer),
				i32(Pointers::kJoinReq_PlaylistChecksum), i32(Pointers::kJoinReq_FfotdVer),
				i32(Pointers::kJoinReq_NetChecksum), i32(Pointers::kJoinReq_FeatureChecksum),
				i32(Pointers::kJoinReq_TuVersion), i32(Pointers::kJoinReq_Protocol),
				i32(Pointers::kJoinReq_Changelist)));

			// Evaluate the verdict-36 predicate here rather than making it a reading exercise later.
			// This is the gate the last two-PC run actually tripped.
			if (networkmode != 1 || (jointype != 4 && jointype != 1)) {
				JcAppend(std::format(
					"[netmsg]            -> WILL REFUSE 36 (when host networkMode==1, i.e. LAN): {}{}",
					networkmode != 1 ? std::format("networkmode={} but host requires 1. ", networkmode) : "",
					(jointype != 4 && jointype != 1)
						? std::format("jointype={} but host requires 1 or 4.", jointype) : ""));
			}
			return ok;
		}

		// Host side. Fires exactly once per join attempt, and its 6th argument is the 96-byte response
		// struct whose first dword is the verdict. This is the cheapest possible read of "why the host
		// said no" — one argument, one dword, no reconstruction.
		bool hkNetMsg_SendJoinResponse(int a1, int a2, int a3, void* netadr, void* a5, void* resp) {
			int  code = -1;
			char name[37]{};
			int  lobbyType = -1, networkMode = -1, mainMode = -1;
			std::uint8_t adr[16]{};

			if (resp) {
				const auto* r = static_cast<const std::uint8_t*>(resp);
				SafeRead(r + Pointers::kJoinResp_Code, code);
				SafeCopy(name, r + Pointers::kJoinResp_Name, 36);
				SafeRead(r + Pointers::kJoinResp_LobbyType, lobbyType);
				SafeRead(r + Pointers::kJoinResp_NetworkMode, networkMode);
				SafeRead(r + Pointers::kJoinResp_MainMode, mainMode);
			}
			SafeCopy(adr, netadr, sizeof(adr));

			JcAppend(std::format(
				"[netmsg] {}  SEND msgId=17 JoinResponse -> {}\n"
				"[netmsg]            verdict={} ({})\n"
				"[netmsg]            name='{}' lobbyType={} networkMode={} mainMode={}",
				WallClockNow(), HexBytes(adr, sizeof(adr)),
				code, NetMsgJoinResponseLabel(code),
				name, lobbyType, networkMode, mainMode));

			return g_origSendJoinResponse
				? g_origSendJoinResponse(a1, a2, a3, netadr, a5, resp)
				: false;
		}
	}

	bool Pointers::NetMsgTranscriptInstalled() const {
		return g_netMsgTranscriptOn.load();
	}

	std::string Pointers::InstallNetMsgTranscript() {
		if (g_netMsgTranscriptOn.load()) {
			return "Netmsg transcript already installed.";
		}
		if (!this->m_NetMsg_Dispatch || !this->m_NetMsg_SendJoinResponse) {
			return "Netmsg transcript anchors unresolved (build mismatch?). Nothing installed.";
		}

		// Latch the read-only globals the detours use. Any of these may legitimately be null on a
		// build mismatch; the detours degrade to "?" / omitted fields rather than faulting.
		g_netMsgNamesTbl    = this->m_g_netMsgNames;
		g_netMsgJoinCtx     = this->m_g_clientJoinCtx;
		g_netMsgExpectedAdr = this->m_g_expectedHostAdr;
		g_netMsgRespCode    = this->m_g_joinResponseCode;

		if (!g_netMsgDispatchHook) {
			g_netMsgDispatchHook = new Client::Memory::MinHook<void>(this->m_NetMsg_Dispatch);
		}
		g_netMsgDispatchHook->Hook(&hkNetMsg_Dispatch, &g_origNetMsgDispatch);
		if (!g_origNetMsgDispatch) {
			return "MinHook failed to detour NetMsg_Dispatch (transcript not active).";
		}

		if (!g_netMsgJoinRespHook) {
			g_netMsgJoinRespHook = new Client::Memory::MinHook<void>(this->m_NetMsg_SendJoinResponse);
		}
		g_netMsgJoinRespHook->Hook(&hkNetMsg_SendJoinResponse, &g_origSendJoinResponse);

		if (this->m_Session_ParseJoinLobbyRequest) {
			if (!g_netMsgParseReqHook) {
				g_netMsgParseReqHook = new Client::Memory::MinHook<void>(this->m_Session_ParseJoinLobbyRequest);
			}
			g_netMsgParseReqHook->Hook(&hkSession_ParseJoinLobbyRequest, &g_origParseJoinReq);
		}

		g_netMsgTranscriptOn.store(true);
		JcAppend(std::format("[netmsg] ==== transcript installed at {} ====", WallClockNow()));
		LOG("Pointers", INFO, "netmsg transcript installed (dispatch + JoinResponse).");

		return std::format(
			"Netmsg transcript INSTALLED.\n"
			"  NetMsg_Dispatch         @0x{:X}\n"
			"  NetMsg_SendJoinResponse @0x{:X}\n"
			"Every inbound lobby message is now logged post-decryption, plus the host's verdict.\n"
			"Install on BOTH PCs, run the join, then diff the two client_join_args.txt files.",
			reinterpret_cast<std::uintptr_t>(this->m_NetMsg_Dispatch),
			reinterpret_cast<std::uintptr_t>(this->m_NetMsg_SendJoinResponse));
	}

	std::string Pointers::RemoveNetMsgTranscript() {
		if (!g_netMsgTranscriptOn.load()) {
			return "Netmsg transcript is not installed.";
		}
		if (g_netMsgDispatchHook) g_netMsgDispatchHook->Unhook();
		if (g_netMsgJoinRespHook) g_netMsgJoinRespHook->Unhook();
		if (g_netMsgParseReqHook) g_netMsgParseReqHook->Unhook();
		g_origNetMsgDispatch   = nullptr;
		g_origSendJoinResponse = nullptr;
		g_origParseJoinReq     = nullptr;
		g_netMsgTranscriptOn.store(false);

		JcAppend(std::format("[netmsg] ==== transcript removed at {} ====", WallClockNow()));
		LOG("Pointers", INFO, "netmsg transcript removed (stock dispatch restored).");
		return "Netmsg transcript REMOVED (stock NetMsg_Dispatch restored).";
	}
}
