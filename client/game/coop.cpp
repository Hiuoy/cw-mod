// Phase 3 lever-1: the in-process loopback second-player seat, and the client-slot
// inspector used to verify it.
#include "common.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"

#include <cstdio>

namespace Client::Game {
	// The connect-userinfo key hash SV_DirectConnect's Info_ValueForKey (sub_7FF72919B7D0) uses.
	// It is the SAME engine-wide name hash as dvars, LUI menu names and GSC #"..." - one function
	// (Com_HashString, RVA 0x1D56710), one algorithm. Kept as a named alias so the call sites below
	// still read as "info key", but there is only one implementation.
	namespace {
		std::uint64_t InfoKeyHash(const char* s) {
			return Pointers::HashString(s);
		}
	}

	// *g_svClients — the heap allocation holding the client slots. NULL while no server is running.
	// The global slot itself is in-module (safe to deref); the pointer it holds may be null.
	void* Pointers::ClientSlotArrayBase() const {
		if (!this->m_g_svClients) {
			return nullptr;
		}
		return *this->m_g_svClients;
	}

	// Read-only diagnostic. Reads only the first 0x40 bytes of each slot (well within the 70864-byte
	// stride of an allocated slot array), so it can't run off the allocation. No engine calls.
	// Purpose: find the connected-state field empirically (recon: SV_DirectConnect sets state = 3)
	// by diffing an occupied slot vs a free one during a live ZM session.
	std::string Pointers::DumpClientSlots() const {
		void* base = this->ClientSlotArrayBase();
		const int cap = this->m_g_svMaxClients ? *this->m_g_svMaxClients : -1;
		if (!base || cap <= 0) {
			return std::format("g_svClients base={} cap={} — no server running (start a map first).",
				base, cap);
		}

		int n = cap;
		if (n > 64) n = 64; // paranoia clamp; domain is [1,64]

		std::string out = std::format("g_svClients base=0x{:X} cap={} stride=0x{:X}\n",
			reinterpret_cast<std::uintptr_t>(base), cap, kClientSlotStride);

		for (int i = 0; i < n; ++i) {
			const std::uint8_t* slot = reinterpret_cast<const std::uint8_t*>(base)
				+ static_cast<std::size_t>(i) * kClientSlotStride;
			auto rd = [&](std::size_t off) {
				return *reinterpret_cast<const std::uint32_t*>(slot + off);
			};
			out += std::format("  slot[{}] @0x{:X}  +00={:#010x} +04={:#010x} +08={:#010x} +0C={:#010x}\n",
				i, reinterpret_cast<std::uintptr_t>(slot), rd(0), rd(4), rd(8), rd(0xC));
			for (std::size_t off = 0; off < 0x40; off += 4) {
				if (rd(off) == 3) {
					out += std::format("      state==3 candidate at +0x{:X}\n", off);
				}
			}
		}
		return out;
	}

	// Lever 1, Option B (surgical): seat a 2nd local client in-process, replicating the engine's own
	// loopback recipe (from SV_Migration_ReseatClient_Loopback) without the host-migration baggage.
	// MUST be called on the game thread with a server running. See docs/phase3_netcode.md Step 7.
	bool Pointers::SeatSecondPlayerLoopback() {
		if (!this->CoopAnchorsResolved()) {
			LOG("Pointers", ERROR, "Seat-2nd-player: coop anchors unresolved (build mismatch?). Aborting.");
			return false;
		}
		// A server must be running (a map loaded): SV_DirectConnect walks g_svClients up to the cap.
		const int cap = this->m_g_svMaxClients ? *this->m_g_svMaxClients : 0;
		if (cap <= 0 || !this->ClientSlotArrayBase()) {
			LOG("Pointers", ERROR, "Seat-2nd-player: no running server (cap={}). Load a Zombies map first.", cap);
			return false;
		}

		// Live values SV_DirectConnect validates. netfieldchk is a plain global (safe read); sessionmode
		// is the current mode string; qport is a fresh value from the engine counter (must be unique so
		// NET_CompareAdr doesn't alias an existing loopback slot). All read/called on the game thread.
		const int netfieldchk = *this->m_g_netFieldChecksum;
		const char* sessionmode = reinterpret_cast<Functions::Com_SessionMode_GetStringT*>(
			this->m_Com_SessionMode_GetString)();
		if (!sessionmode) sessionmode = "";
		const std::uint16_t qport = ++(*this->m_qportCounter);

		// Build the fresh-connect userinfo with hashed keys. migrating=0 -> fresh path; xuid=0 -> clean
		// free slot; challenge unread; password skipped for a local addr. Wrapped as `connect "..."`
		// like the engine; challenge is placed LAST so the closing quote lands on an unvalidated field.
		auto appendField = [](std::string& out, const char* key, const std::string& value) {
			char hex[24];
			std::snprintf(hex, sizeof(hex), "%llx", static_cast<unsigned long long>(InfoKeyHash(key)));
			out += '\\';
			out += hex;
			out += '\\';
			out += value;
		};
		std::string info = "connect \"";
		appendField(info, "protocol", "2426");
		appendField(info, "netfieldchk", std::to_string(netfieldchk));
		appendField(info, "sessionmode", sessionmode);
		appendField(info, "migrating", "0");
		appendField(info, "xuid", "0");
		appendField(info, "qport", std::to_string(static_cast<unsigned>(qport)));
		appendField(info, "invited", "1");
		appendField(info, "name", "Player2");
		appendField(info, "challenge", "0");
		info += '"';

		// 16-byte loopback netadr: type dword 0 at +8 (marks it local -> password skipped), qport at +4.
		alignas(16) unsigned char netadr[16] = {};
		*reinterpret_cast<std::uint16_t*>(netadr + 4) = qport;

		// Stage userinfo -> SV_DirectConnect(&netadr) reads it and seats a slot (state=3) -> unstage.
		reinterpret_cast<Functions::SV_StageConnectMessageT*>(this->m_SV_StageConnectMessage)(info.c_str());
		reinterpret_cast<Functions::SV_DirectConnectT*>(this->m_SV_DirectConnect)(netadr);
		reinterpret_cast<Functions::SV_UnstageConnectMessageT*>(this->m_SV_UnstageConnectMessage)();

		LOG("Pointers", INFO, "Seat-2nd-player: SV_DirectConnect issued (qport={}, netfieldchk={}, mode='{}'). "
			"Check the slot dumper for a new slot at state 3.", qport, netfieldchk, sessionmode);
		return true;
	}
}
