// Phase 3 lever-4: the client (PC2) join - FSM inspection, the host descriptor PC1 emits,
// and the probe-less join that consumes it.
#include "common.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/join_log.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace Client::Game {
	// Read-only. Decodes the join FSM context: state, candidate count/index, and the host currently being
	// tried. Offsets are all verified against the StartJoin / AddHostCandidate decompiles (see game.hpp).
	std::string Pointers::DumpClientJoinState() const {
		if (!this->m_g_clientJoinCtx) {
			return "g_clientJoinCtx unresolved (build mismatch?). Nothing to dump.";
		}
		const std::uint8_t* ctx = this->m_g_clientJoinCtx;

		int state = 0, actionId = 0, controllerIdx = 0, srcLobby = 0, dstLobby = 0, count = 0, idx = 0;
		if (!SafeRead(ctx + kJoinCtx_State, state)) {
			return "g_clientJoinCtx read faulted — address resolved but not mapped (build mismatch?).";
		}
		SafeRead(ctx + kJoinCtx_ActionId, actionId);
		SafeRead(ctx + kJoinCtx_ControllerIdx, controllerIdx);
		SafeRead(ctx + kJoinCtx_SrcLobby, srcLobby);
		SafeRead(ctx + kJoinCtx_DstLobby, dstLobby);
		SafeRead(ctx + kJoinCtx_HostCount, count);
		SafeRead(ctx + kJoinCtx_HostIdx, idx);

		const int launchState = this->m_cl_lobbyLaunchState ? *this->m_cl_lobbyLaunchState : -1;

		std::string out = std::format(
			"g_clientJoinCtx @0x{:X}\n"
			"  state         = {} ({})\n"
			"  actionId      = 0x{:X}\n"
			"  controllerIdx = {}   srcLobby = {}   dstLobby = {}\n"
			"  candidates    = {} of max {}   (FSM is on index {})\n"
			"  cl_lobbyLaunchState = {}  (33/34/35 = connecting; 40/41 = fail)\n",
			reinterpret_cast<std::uintptr_t>(ctx), state, JoinStateLabel(state),
			static_cast<unsigned>(actionId), controllerIdx, srcLobby, dstLobby,
			count, kJoinHost_MaxCount, idx, launchState);

		if (count <= 0) {
			out += "  (no host candidates — nothing discovered, and nothing injected)\n";
			return out;
		}

		// Decode the candidate list, clamped to the engine's own cap so a garbage count can't walk us
		// off the array.
		int n = count;
		if (n > static_cast<int>(kJoinHost_MaxCount)) n = static_cast<int>(kJoinHost_MaxCount);
		for (int i = 0; i < n; ++i) {
			const std::uint8_t* e = ctx + kJoinCtx_Candidates + static_cast<std::size_t>(i) * kJoinHost_Stride;
			char name[37] = {};
			char type[37] = {};
			std::uint8_t secKey[16] = {};
			std::uint8_t secId[8] = {};
			std::uint8_t serAdr[84] = {};
			SafeCopy(name,   e + kJoinHost_Name, 36);
			SafeCopy(type,   e + kJoinHost_HostType, 36);
			SafeCopy(secId,  e + kJoinHost_SecId, sizeof(secId));
			SafeCopy(secKey, e + kJoinHost_SecKey, sizeof(secKey));
			SafeCopy(serAdr, e + kJoinHost_SerializedAdr, sizeof(serAdr));
			std::int64_t sessionId = 0;
			SafeRead(e + kJoinHost_SessionId, sessionId);
			out += std::format("  host[{}] '{}' ({})  sessionId=0x{:X}\n"
				"            secid={}  seckey={}\n            serAdr[0:32]={}\n",
				i, name, type, static_cast<std::uint64_t>(sessionId),
				HexBytes(secId, sizeof(secId)), HexBytes(secKey, sizeof(secKey)),
				HexBytes(serAdr, 32));
		}

		// The 248-byte working copy of whichever candidate the FSM is actually trying.
		std::uint8_t cur[kJoinHost_Stride] = {};
		if (SafeCopy(cur, ctx + kJoinCtx_CurrentHost, sizeof(cur))) {
			char curName[37] = {};
			std::memcpy(curName, cur + kJoinHost_Name, 36);
			out += std::format("  current host slot: '{}'  seckey={}\n", curName,
				HexBytes(cur + kJoinHost_SecKey, 16));
		}
		return out;
	}

	namespace {
		// Guards the watcher so two joins cannot arm two samplers on the same FSM.
		std::atomic<bool> g_joinWatchRunning{ false };
	}

	void Pointers::StartJoinWatch() {
		if (!this->m_g_clientJoinCtx) {
			JcAppend("[join-watch] g_clientJoinCtx unresolved (build mismatch?) — cannot watch.");
			return;
		}
		bool expected = false;
		if (!g_joinWatchRunning.compare_exchange_strong(expected, true)) {
			JcAppend("[join-watch] already running — not arming a second sampler.");
			return;
		}
		const int* statePtr = reinterpret_cast<const int*>(this->m_g_clientJoinCtx + kJoinCtx_State);
		const int* countPtr = reinterpret_cast<const int*>(this->m_g_clientJoinCtx + kJoinCtx_HostCount);

		// Same shape as StartPhaseWatch: bounded, detached, self-terminating, read-only on plain globals.
		// 2ms sampling, not 20: at 20ms every transition landed exactly one sample apart, which measured
		// the sampler rather than the FSM and could not separate "host replied" from "abandoned locally".
		// Wall-clock stamps so PC1 and PC2 transcripts can be interleaved.
		std::thread([statePtr, countPtr] {
			const auto t0 = std::chrono::steady_clock::now();
			int last = -1, highest = 0;
			bool sawComplete = false;
			JcAppend(std::format("[join-watch] armed (2ms sampling, 15s window) at {}.", WallClockNow()));
			while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(15)) {
				const int s = *statePtr;
				if (s != last) {
					const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - t0).count();
					JcAppend(std::format("[join-watch] {}  +{:7}us  state {} -> {} ({})  candidates={}",
						WallClockNow(), us, last, s, JoinStateLabel(s), *countPtr));
					if (s == 6) sawComplete = true;
					if (s > highest && s < 6) highest = s;
					last = s;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			JcAppend(std::format("[join-watch] window closed. reached 6 (join complete)={}; highest pre-terminal "
				"state={} ({}); final state={} ({}); candidates={}. "
				"[stuck at 1 with 0 candidates = nothing to join; stuck at 3 = host never answered JoinLobby]",
				sawComplete ? "YES" : "no", highest, JoinStateLabel(highest),
				*statePtr, JoinStateLabel(*statePtr), *countPtr));
			g_joinWatchRunning.store(false);
		}).detach();
	}

	// --- Phase 3 lever-4, THE HOST DESCRIPTOR -----------------------------------------------------
	// PC1 reads its own live session object and emits a descriptor; PC2 pastes it. The XUID-probe
	// path that used to sit here is gone: addressing a netmsg by XUID needs a Demonware XUID->address
	// binding with no LAN fallback, so it can never work across two machines.
	namespace {
		using Session_GetSessionObject_t        = std::uint8_t*(__fastcall*)(int, unsigned);
		using ClientSession_JoinPendingTarget_t = char(__fastcall*)(std::int64_t, int, std::int64_t, int);
	}

	// PC1. The whole point of this is the XUID line: that number is the only thing PC2 needs.
	std::string Pointers::DumpMyHostIdentity() const {
		if (!this->m_Session_GetSessionObject) {
			return "Host identity: Session_GetSessionObject unresolved (build mismatch?).";
		}
		auto getObj = reinterpret_cast<Session_GetSessionObject_t>(this->m_Session_GetSessionObject);

		std::string out = "Session objects — 2 types x 3 slots. A slot is live when memberCount > 0.\n";
		int live = 0;
		for (int type = 0; type < 2; ++type) {
			for (unsigned idx = 0; idx < 3; ++idx) {
				std::uint8_t* obj = getObj(type, idx);
				if (!obj) {
					continue;
				}
				int members = 0;
				if (!SafeRead(obj + kSessObj_MemberCount, members)) {
					out += std::format("  [type {} slot {}] @{}  UNREADABLE\n", type, idx,
						static_cast<void*>(obj));
					continue;
				}
				if (members <= 0) {
					continue;
				}
				++live;

				std::int64_t xuid = 0;
				char  name[37]   = {};
				std::uint8_t secId[8]  = {};
				std::uint8_t secKey[16] = {};
				std::uint8_t adr[84]   = {};
				SafeRead(obj + kSessObj_HostXuid, xuid);
				SafeCopy(name,   obj + kSessObj_HostName, 36);
				SafeCopy(secId,  obj + kSessObj_SecId, sizeof(secId));
				SafeCopy(secKey, obj + kSessObj_SecKey, sizeof(secKey));
				SafeCopy(adr,    obj + kSessObj_SerializedAdr, sizeof(adr));

				out += std::format("  [type {} slot {}] @{}  members={}  name='{}'\n"
					"      >>> HOST XUID = 0x{:016X}  <<< (this is what PC2 types in)\n"
					"      secid  = {}\n      seckey = {}\n      serializedadr[0:32] = {}\n",
					type, idx, static_cast<void*>(obj), members, name,
					static_cast<std::uint64_t>(xuid),
					HexBytes(secId, sizeof(secId)), HexBytes(secKey, sizeof(secKey)),
					HexBytes(adr, 32));

				// The full descriptor, as one copy-pasteable token. If the XUID probe goes unanswered
				// PC2 can paste this instead and skip the round trip entirely — it carries by hand
				// exactly the eight fields ClientSession_JoinPendingTarget reads.
				out += std::format("      >>> CWJOIN1 blob (paste into PC2 if the XUID probe times out):\n{}\n",
					BuildJoinDescriptorBlob(static_cast<std::uint64_t>(xuid), static_cast<int>(idx),
						name, secId, secKey, adr));
			}
		}
		if (live == 0) {
			out += "  (no live session slots — you are not hosting. Launch the systemlink host first.)\n";
		}
		return out;
	}

	bool Pointers::ReadAdvertisedSession(JoinDescriptor& d, int& members, const std::uint8_t** objOut) const {
		if (!this->m_Session_GetSessionObject) {
			return false;
		}
		auto getObj = reinterpret_cast<Session_GetSessionObject_t>(this->m_Session_GetSessionObject);

		for (int slot = 2; slot >= 0; --slot) {
			for (int type = 0; type < 2; ++type) {
				const std::uint8_t* obj = getObj(type, static_cast<unsigned>(slot));
				int n = 0;
				if (!obj || !SafeRead(obj + kSessObj_MemberCount, n) || n <= 0) {
					continue;
				}
				// This is the object the engine would describe. If it is unreadable or has no XUID,
				// there is nothing joinable to advertise; do not fall through to a lower slot, the
				// client would never pick that one.
				JoinDescriptor out{};
				std::int64_t xuid = 0;
				if (!SafeRead(obj + kSessObj_HostXuid, xuid) || xuid == 0
					|| !SafeCopy(out.name, obj + kSessObj_HostName, 36)
					|| !SafeCopy(out.secId, obj + kSessObj_SecId, sizeof(out.secId))
					|| !SafeCopy(out.secKey, obj + kSessObj_SecKey, sizeof(out.secKey))
					|| !SafeCopy(out.adr, obj + kSessObj_SerializedAdr, sizeof(out.adr))) {
					return false;
				}
				out.name[36] = '\0';
				out.xuid = static_cast<std::uint64_t>(xuid);
				out.slot = slot;
				d = out;
				members = n;
				if (objOut) {
					*objOut = obj;
				}
				return true;
			}
		}
		return false;
	}

	namespace {
		// A NUL-terminated engine string of at most n bytes, or "" if it cannot be read.
		std::string ReadEngineString(const std::uint8_t* p, std::size_t n) {
			char buf[128] = {};
			if (!p || n >= sizeof(buf) || !SafeCopy(buf, p, n)) {
				return {};
			}
			buf[n] = '\0';
			return buf;
		}
	}

	Pointers::LobbyDetails Pointers::ReadLobbyDetails(const std::uint8_t* obj) const {
		LobbyDetails out;
		if (!obj) {
			return out;
		}

		int maxClients = 0;
		if (SafeRead(obj + kSessObj_MaxClients, maxClients) && maxClients > 0 && maxClients < kSessObj_ClientCount) {
			out.maxClients = maxClients;
		}

		for (int i = 0; i < kSessObj_ClientCount; ++i) {
			const std::uint8_t* slot = obj + kSessObj_ClientSlots + static_cast<std::size_t>(i) * kSessObj_ClientStride;
			std::uint64_t present = 0;
			const std::uint8_t* client = nullptr;
			if (!SafeRead(slot + kClientSlot_Present, present) || !present
				|| !SafeRead(slot + kClientSlot_Client, client) || !client) {
				continue;
			}
			std::string name = ReadEngineString(client + kLobbyClient_Gamertag, 64);
			out.players.push_back(name.empty() ? std::string("?") : std::move(name));
		}

		// The settings block exists in both session types; which one the lobby fills is not pinned
		// down (Session_GetMapName asks type 1). Take the advertised object first, then any live one.
		std::vector<const std::uint8_t*> sources{ obj };
		if (this->m_Session_GetSessionObject) {
			auto getObj = reinterpret_cast<Session_GetSessionObject_t>(this->m_Session_GetSessionObject);
			for (int type = 1; type >= 0; --type) {
				for (int slot = 2; slot >= 0; --slot) {
					const std::uint8_t* o = getObj(type, static_cast<unsigned>(slot));
					int n = 0;
					if (o && o != obj && SafeRead(o + kSessObj_MemberCount, n) && n > 0) {
						sources.push_back(o);
					}
				}
			}
		}
		for (const std::uint8_t* o : sources) {
			if (out.map.empty())      out.map = ReadEngineString(o + kSessObj_MapName, 64);
			if (out.gametype.empty()) out.gametype = ReadEngineString(o + kSessObj_Gametype, 32);
		}
		return out;
	}

	std::string Pointers::DumpPendingJoinTarget() const {
		if (!this->m_pendingJoin_valid) {
			return "Pending join target: unresolved (build mismatch?).";
		}
		bool awaiting = false, valid = false;
		int nonce = 0;
		SafeRead(this->m_pendingJoin_awaiting, awaiting);
		SafeRead(this->m_pendingJoin_valid, valid);
		SafeRead(this->m_pendingJoin_nonce, nonce);

		std::string out = std::format("g_pendingJoinTarget @{}\n  awaiting InfoResponse = {}\n"
			"  nonce                 = {}\n  descriptor valid      = {}\n",
			static_cast<void*>(this->m_pendingJoin_valid), awaiting ? "yes" : "no", nonce,
			valid ? "YES — host answered" : "no");
		if (valid) {
			char name[37] = {};
			std::uint8_t adr[84] = {};
			if (this->m_pendingJoin_hostName)      SafeCopy(name, this->m_pendingJoin_hostName, 36);
			if (this->m_pendingJoin_serializedAdr) SafeCopy(adr, this->m_pendingJoin_serializedAdr, sizeof(adr));
			out += std::format("  host name             = '{}'\n  serializedadr[0:32]   = {}\n",
				name, HexBytes(adr, 32));
			out += "  -> ready: JoinPendingTarget can be fired against this.\n";
		} else if (awaiting) {
			out += "  -> probe outstanding; the host has not answered yet.\n";
		} else {
			out += "  -> idle: no probe sent, or the last one timed out.\n";
		}
		return out;
	}

	// ---- Probe-less join: carry the descriptor by hand -------------------------------------------
	//
	// Why this exists. The XUID probe path is correct — it joins fine on one machine — but it depends
	// on resolving a XUID to an address, which is Demonware's job and has no LAN fallback. Across two
	// PCs the InfoRequest is built and handed to the transport with nowhere to go, and because
	// Session_SendInfoRequestMsg returns 1 unconditionally we never even hear about it.
	//
	// The round trip's only product is g_pendingJoinTarget_*, and ClientSession_JoinPendingTarget
	// reads exactly eight of those fields. All eight are readable on PC1 straight out of its live
	// session object (Session_BuildSendInfoResponse copies the very same bytes onto the wire). So we
	// carry them by hand. The join itself is addressed by serializedadr — a real LAN address — so it
	// never needs XUID resolution.
	namespace {
		constexpr char kBlobPrefix[] = "CWJOIN1.";

		void HexAppend(std::string& s, const std::uint8_t* p, std::size_t n) {
			static constexpr char kDigits[] = "0123456789ABCDEF";
			for (std::size_t i = 0; i < n; ++i) {
				s.push_back(kDigits[p[i] >> 4]);
				s.push_back(kDigits[p[i] & 0xF]);
			}
		}

		// Pulls exactly n bytes off the cursor. Returns false (and leaves out untouched) on any
		// non-hex digit or short read, so a truncated paste fails instead of half-filling the struct.
		bool HexTake(const std::string& s, std::size_t& pos, std::uint8_t* out, std::size_t n) {
			if (pos + n * 2 > s.size()) {
				return false;
			}
			for (std::size_t i = 0; i < n; ++i) {
				int hi = -1, lo = -1;
				for (int k = 0; k < 2; ++k) {
					const char c = s[pos + i * 2 + k];
					int v = -1;
					if (c >= '0' && c <= '9') v = c - '0';
					else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
					else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
					else return false;
					(k == 0 ? hi : lo) = v;
				}
				out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
			}
			pos += n * 2;
			return true;
		}
	}

	std::string Pointers::BuildJoinDescriptorBlob(std::uint64_t xuid, int slot, const char* name,
			const std::uint8_t* secId, const std::uint8_t* secKey, const std::uint8_t* adr) {
		std::uint8_t xuidBytes[8]{};
		std::memcpy(xuidBytes, &xuid, sizeof(xuidBytes));
		std::uint8_t slotByte = static_cast<std::uint8_t>(slot & 0xFF);
		std::uint8_t nameBytes[36]{};
		std::memcpy(nameBytes, name, 36);   // callers pass a char[37] whose first 36 came from the engine

		std::string s = kBlobPrefix;
		HexAppend(s, xuidBytes, sizeof(xuidBytes));
		HexAppend(s, &slotByte, 1);
		HexAppend(s, nameBytes, sizeof(nameBytes));
		HexAppend(s, secId, 8);
		HexAppend(s, secKey, 16);
		HexAppend(s, adr, 84);
		return s;
	}

	std::string Pointers::DecodeJoinDescriptorBlob(const std::string& blob, JoinDescriptor& d) {
		// Strip whitespace, then the prefix if present. Users paste out of a log window, so newlines
		// and stray spaces are expected.
		std::string s;
		s.reserve(blob.size());
		for (const char c : blob) {
			if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
				s.push_back(c);
			}
		}
		const std::size_t plen = sizeof(kBlobPrefix) - 1;
		if (s.size() >= plen && s.compare(0, plen, kBlobPrefix) == 0) {
			s.erase(0, plen);
		}

		// Parse fully into a local, and hand it out only once every check has passed. A bad paste must
		// not leave a half-filled descriptor behind.
		JoinDescriptor out{};
		std::uint8_t xuidBytes[8]{}, slotByte = 0;
		std::size_t pos = 0;
		if (!HexTake(s, pos, xuidBytes, sizeof(xuidBytes)) || !HexTake(s, pos, &slotByte, 1)
			|| !HexTake(s, pos, reinterpret_cast<std::uint8_t*>(out.name), 36)
			|| !HexTake(s, pos, out.secId, sizeof(out.secId))
			|| !HexTake(s, pos, out.secKey, sizeof(out.secKey)) || !HexTake(s, pos, out.adr, sizeof(out.adr))) {
			return std::format("malformed blob — expected {} hex chars after the '{}' prefix, got {}. "
				"Copy the whole CWJOIN1 line from PC1.", (8 + 1 + 36 + 8 + 16 + 84) * 2, kBlobPrefix, s.size());
		}
		if (pos != s.size()) {
			return std::format("blob has {} trailing characters — copy exactly one CWJOIN1 line.", s.size() - pos);
		}
		std::memcpy(&out.xuid, xuidBytes, sizeof(out.xuid));
		if (out.xuid == 0) {
			return "XUID in the blob is 0 — PC1 was not hosting when it emitted this.";
		}
		if (slotByte > 2) {
			return std::format("slot index {} is out of range (0..2).", slotByte);
		}
		out.slot = slotByte;
		out.name[36] = '\0';
		d = out;
		return {};
	}

	// joinType is the wire field `jointype`, NOT a lobby type. It reaches the host as request+8 and is
	// gated there: NetMsg_Handle_JoinLobby answers verdict 36 unless it is 1 or 4.
	std::string Pointers::JoinHostByDescriptor(const std::string& blob, int joinCtxId, int controllerIdx,
			int joinType) {
		if (!this->m_ClientSession_JoinPendingTarget) {
			return "Join by descriptor: ClientSession_JoinPendingTarget unresolved (build mismatch?).";
		}
		if (!this->m_pendingJoin_valid || !this->m_pendingJoin_xuid || !this->m_pendingJoin_sessionId
			|| !this->m_pendingJoin_hostName || !this->m_pendingJoin_slot || !this->m_pendingJoin_secId
			|| !this->m_pendingJoin_secKey || !this->m_pendingJoin_serializedAdr) {
			return "Join by descriptor: g_pendingJoinTarget_* unresolved — cannot fill the struct.";
		}
		if (!this->m_Com_SessionMode_SetNetworkMode) {
			return "Join by descriptor: SetNetworkMode unresolved — cannot force LAN mode.";
		}

		// Decode BEFORE touching a single engine global.
		JoinDescriptor d;
		if (const std::string err = DecodeJoinDescriptorBlob(blob, d); !err.empty()) {
			return "Join by descriptor: " + err;
		}
		const auto xuid = static_cast<std::int64_t>(d.xuid);

		int state = 0;
		if (SafeRead(this->m_g_clientJoinCtx + kJoinCtx_State, state) && state != 0) {
			return std::format("Join by descriptor: a join is already in progress (state={} {}). Back "
				"out to the frontend first.", state, JoinStateLabel(state));
		}

		this->m_Com_SessionMode_SetNetworkMode(1);

		// Fill exactly what ClientSession_JoinPendingTarget reads. sessionId gets the XUID too: the
		// host fills that slot (descriptor entry+8) from sessionObj+168, which is the host XUID.
		*this->m_pendingJoin_xuid      = xuid;
		*this->m_pendingJoin_sessionId = xuid;
		*this->m_pendingJoin_slot      = d.slot;
		std::memcpy(this->m_pendingJoin_hostName,      d.name,   36);
		std::memcpy(this->m_pendingJoin_secId,         d.secId,  sizeof(d.secId));
		std::memcpy(this->m_pendingJoin_secKey,        d.secKey, sizeof(d.secKey));
		std::memcpy(this->m_pendingJoin_serializedAdr, d.adr,    sizeof(d.adr));

		// The awaiting flag belongs to the probe path; clear it so a late InfoResponse (if the XUID
		// ever did resolve) cannot overwrite what we just wrote.
		if (this->m_pendingJoin_awaiting) {
			*this->m_pendingJoin_awaiting = false;
		}
		*this->m_pendingJoin_valid = true;

		std::string out = std::format("Join by descriptor: filled g_pendingJoinTarget from blob.\n"
			"  host xuid = 0x{:016X}\n  host name = '{}'\n  slot      = {}\n  adr[0:16] = {}\n",
			d.xuid, d.name, d.slot, HexBytes(d.adr, 16));

		auto join = reinterpret_cast<ClientSession_JoinPendingTarget_t>(this->m_ClientSession_JoinPendingTarget);
		const char ok = join(joinCtxId, controllerIdx, 0, joinType);
		out += std::format("  JoinPendingTarget(ctx={}, pad={}, jointype={}{}) -> {}\n", joinCtxId,
			controllerIdx, joinType,
			(joinType == 1 || joinType == 4) ? "" : " <-- HOST WILL REFUSE 36, must be 1 or 4",
			ok ? "accepted — watch the FSM march 1 -> 6" : "REFUSED");
		if (ok) {
			this->StartJoinWatch();
		}
		return out;
	}
}
