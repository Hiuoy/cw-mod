#include "common.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/signature_store.hpp"
#include "game/dw_backend.hpp"
#include "game/boot_profile.hpp"
#include "game/settings.hpp"
#include "memory/memory.hpp"
#include <utility/nt.hpp>

#define SETUP_POINTER(name) #name, (void**)&this->m_##name
#define SETUP_MOD(mod) [](Memory::ScannedResult<void> r) { return r.##mod##; }

namespace Client::Game {
	void Pointers::PatchAuth() {
		if (this->m_unk_ContentManager && *this->m_unk_ContentManager) {
			T9::ContentManager* contentMgr = *this->m_unk_ContentManager;
			for (int i = 0; i < contentMgr->m_PackCount; i++) {
				if (contentMgr->m_ContentPacks == nullptr) {
					continue;
				}

				contentMgr->m_ContentPacks[i].m_Var1 = 0;
				contentMgr->m_ContentPacks[i].m_Var2 = 2;
				contentMgr->m_ContentPacks[i].m_Var3 = 3;
			}
		}
		else {
			LOG("Pointers", ERROR, "Failed to patch Call of Duty: Black Ops Cold War content because the pointer is null.");
		}
	}

	// `mode` is the session-mode network nibble: 0 = offline, 1 = lan, 2 = online/live. The lobby's own
	// network mode is a SEPARATE enum that must agree with it, so derive it here instead of pinning LAN:
	// offline and lan both map to LOBBY_NETWORKMODE_LAN (== LOCAL == 1), online maps to LIVE (2). Passing
	// mode=2 while the lobby still said LAN is what made "online mode" half-apply.
	//
	// `menu` only tells the lobby UI which menu it is on (10 = director_lan). It skips the title screen,
	// and with it the only step that creates the party (Lobby.ProcessNavigate.CreatePrivateLobby). The
	// director's "is party leader" test then reads false, and that locks the mode tiles. Pass menu < 0
	// to keep the title screen.
	void Pointers::SetMode(int mode, int menu, int lobbyLive) {
		const bool live = lobbyLive < 0 ? (mode == 2) : (lobbyLive != 0);
		if (menu >= 0) {
			this->m_LobbyUI_SetTargetMenuAndNotify(menu, 0);
		}
		this->m_LobbyBase_SetNetworkMode(live ? T9::LobbyNetworkMode::LOBBY_NETWORKMODE_LIVE
		                                      : T9::LobbyNetworkMode::LOBBY_NETWORKMODE_LAN);
		this->m_Com_SessionMode_SetNetworkMode(mode);

		*this->m_unk_Config_1 = 1;
		*this->m_unk_Config_2 = 1;
	}

	// We deliberately do NOT read the dvar's value back through the engine. Dvar_GetInt is an Arxan
	// control-flow-flattened split-thunk whose callee dispatches on the return address; calling it
	// from our module faults (that was the boot crash). The dvar struct is also Arxan-obfuscated, so
	// it can't be read at a fixed offset either. Instead we track what we last *set* (below) and use
	// g_svMaxClients — a plain global int — as the ground truth for the effective cap.

	// g_svMaxClients is a plain global int (the effective server cap, set at map spawn) — safe to
	// read directly from any thread. This is the number that sizes the client array and gates
	// SV_DirectConnect's server-full check, so it's the definitive proof the cap took effect.
	int Pointers::ServerMaxClients() {
		if (!this->m_g_svMaxClients) {
			return -1;
		}
		return *this->m_g_svMaxClients;
	}

	// Last value we successfully pushed to com_maxclients this session (-1 if never set). Cached on
	// the game thread in SetMaxClients; read from the menu thread. Not an engine read.
	int Pointers::LastSetMaxClients() const {
		return this->m_MaxClientsCache.load(std::memory_order_relaxed);
	}

	bool Pointers::SetMaxClients(int value) {
		if (!this->m_Dvar_SetIntFromSource || !this->m_dvar_com_maxclients || !*this->m_dvar_com_maxclients) {
			LOG("Pointers", ERROR, "Cannot set com_maxclients: dvar or setter unresolved.");
			return false;
		}
		// source 0 = the same source the engine's own clamp-set uses in SV_StartMap. The setter is a
		// normal function entry (sigged at its prologue) — safe to call, unlike the getter thunk.
		this->WriteDvarInt(*this->m_dvar_com_maxclients, value);
		this->m_MaxClientsCache.store(value, std::memory_order_relaxed);
		LOG("Pointers", INFO, "com_maxclients set to {} (source 0). Verify via 'live server cap' after loading a map.", value);
		return true;
	}

	Pointers::Pointers() {
		SignatureStore batch;

		batch.Add(SETUP_POINTER(BB_Alert), "40 55 53 56 48 8D AC 24 ? ? ? ? B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 80 3D");

		batch.Add(SETUP_POINTER(CL_Disconnect), "40 53 56 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 ? ? ? ? 4D 8B F8");

		batch.Add(SETUP_POINTER(CL_DrawTextPhysical), "48 83 EC 78 F3 0F 10 84 24 ? ? ? ? F3 0F 10 8C 24 ? ? ? ? 8B 84 24 ? ? ? ? F3 0F 11 44 24 ? F3 0F 10"
			" 84 24 ? ? ? ? C6 44 24 ? ? C7 44 24 ? ? ? ? ? 89 44 24 48 48 8B 84 24 ? ? ? ? 48 89 44 24 ? F3 0F 11 44 24 ? F3 0F 10 84 24 ? ? ? ? F3 0F 11"
			" 4C 24 ? F3 0F 10 8C 24 ? ? ? ? F3 0F 11 44 24 ? F3 0F 11 4C 24 ? E8 ? ? ? ? 48 83 C4 78 C3");

		batch.Add(SETUP_POINTER(Com_SessionMode_SetNetworkMode), "8B 05 ? ? ? ? 8B D0 33 D1 83 E2 0F 33 C2 89 05 ? ? ? ? C3");

		/*
			48 85 C9 0F 84 ? ? ? ? 48 89 5C 24 ? 57 48 83 EC 60 0F 57 C0 44 0F B6 CA 48 B8 ? ? ? ? ? ? ? ? 41 8B F8 48 8B D9 0F 11 44 24 ? 0F 11 44 24 ? 48 85 01 0F 84 ? ? ? ?
			48 85 C9 0F 84 ? ? ? ? 4C 8B DC 56 57 48 83 EC ? 0F 57 C0 44 0F B6 CA 48 B8 ? ? ? ? ? ? ? ? 41 8B F0 48 8B F9 0F 11 44 24 ? 0F 11 44 24 ? 48 85 01 0F 84 ? ? ? ?
		*/
		batch.Add(SETUP_POINTER(Dvar_SetBoolFromSource), "48 85 C9 0F 84 ? ? ? ? 4C 8B DC 56 57 48 83 EC ? 0F 57 C0 44 0F B6 CA 48 B8 ? ? ? ? ? ? ? ? 41 8B"
			" F0 48 8B F9 0F 11 44 24 ? 0F 11 44 24 ? 48 85 01 0F 84 ? ? ? ?");

		batch.Add(SETUP_POINTER(Dvar_ShowOverStack), "48 8B 0D ? ? ? ? 41 0F 28 F2 F3 0F 5E F0 C7 44 24 ? ? ? ? ? 0F 28 05 ? ? ? ? 0F 28 FE 44 0F 28 C6 F3"
			" 0F 59 3D ? ? ? ? 44 0F 28 CE F3 0F 59 35 ? ? ? ?", SETUP_MOD(Add(3).Rip()));

		// Dvar_SetInt(dvar, value, source): generic int-typed dvar setter (switches on dvar type at
		// +0x18). SV_StartMap uses this exact fn to clamp-set com_maxclients. Prologue is unique.
		batch.Add(SETUP_POINTER(Dvar_SetIntFromSource), "48 85 C9 0F 84 ? ? ? ? 4C 8B DC 56 57 48 81 EC B8 00 00 00 48 8B 05 ? ? ?"
			" ? 48 33 C4 48 89 84 24 90 00 00 00 0F 57 C0 48 B8 FF FF FF FF FF FF FF 7F 41 8B F0 48 8B F9");

		// com_maxclients dvar pointer slot: resolved from the RIP-relative load in SV_StartMap
		// (mov rcx, cs:p_dvar_com_maxclients ; call Dvar_GetInt ; mov edi,40h ; cmp eax,edi ; cmovl).
		// *m_dvar_com_maxclients is the dvar object. The dvar struct is Arxan-obfuscated, so read
		// its value via the engine getter (below), not a fixed offset. See phase3 doc.
		batch.Add(SETUP_POINTER(dvar_com_maxclients), "48 8B 0D ? ? ? ? E8 ? ? ? ? BF 40 00 00 00 3B C7 8B DF 0F 4C D8",
			SETUP_MOD(Add(3).Rip()));

		// g_svMaxClients: live server player cap, plain global int set by SV_Init_SpawnServer from
		// the clamped com_maxclients. Resolved from `cmp eax, cs:g_svMaxClients` in a slot-iterator
		// leaf (movzx eax,di ; cmp eax,g_svMaxClients ; jge ; ...). disp32 at +5. See phase3 doc.
		batch.Add(SETUP_POINTER(g_svMaxClients), "0F B7 C7 3B 05 ? ? ? ? 7D ? 48 8B 0D ? ? ? ? 48 8D 1C 80 48 C1 E3 04",
			SETUP_MOD(Add(5).Rip()));

		batch.Add(SETUP_POINTER(LiveUser_GetUserDataForController), "E8 ? ? ? ? 48 8B E8 33 C0 48 89 85 ? ? ? ? 48 8D 4D 10 48 89 85 ? ? ? ? 48 89 85 ? ? ?"
			" ? 89 85 ? ? ? ?", SETUP_MOD(Add(1).Rip()));

		batch.Add(SETUP_POINTER(LobbyBase_SetNetworkMode), "40 53 48 83 EC 20 8B D9 89 0D ? ? ? ? E8 ? ? ? ? 8B CB E8 ? ? ? ?");

		batch.Add(SETUP_POINTER(LobbyUI_SetTargetMenuAndNotify),"48 89 5C 24 ? 48 89 74 24 ? 55 57 41 54 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33"
			" C4 48 89 45 27 0F B6 FA 48 63 D9 8B 05 ? ? ? ?");

		batch.Add(SETUP_POINTER(Unk_SetUsername), "48 89 5C 24 ? 57 48 83 EC 20 48 8B D9 4C 8B C2 BA ? ? ? ? 48 81 C1 ? ? ? ?");

		batch.Add(SETUP_POINTER(Dvar_NoDW), "48 8B 0D ? ? ? ? E8 ? ? ? ? 83 F8 01 75 ? 48 8B 0F 8D 68 ? 44 8B F8 48 85 C9", SETUP_MOD(Add(3).Rip()));

		// --- Debug tab: dvars by name --------------------------------------------
		// Dvar_FindVar(hash). We deliberately bind the IMPLEMENTATION, not the public thunk at
		// +0xC0B2230 — that thunk is an Arxan `E9` split stub whose target moves. The impl is
		// recognisable by its spinlock preamble: xor ebx,ebx / mov edi,ebx / inc edi / test dil,0Fh.
		batch.Add(SETUP_POINTER(Dvar_FindVar), "48 89 5C 24 ? 48 89 4C 24 ? 57 48 83 EC 20 33 DB 8B FB FF C7 40 F6 C7 0F");

		// The 1024-bucket dvar table, from the bucket index inside Dvar_FindVar:
		//   mov rax,[rsp+arg_0] ; lea rdi,<table> ; mov rcx,rax ; and ecx,3FFh ; mov rdi,[rdi+rcx*8]
		// The `and ecx, 3FFh` + scaled load make this unique in the image. disp32 sits at +8.
		batch.Add(SETUP_POINTER(g_dvarHashTable), "48 8B 44 24 ? 48 8D 3D ? ? ? ? 48 8B C8 81 E1 FF 03 00 00 48 8B 3C CF",
			SETUP_MOD(Add(8).Rip()));

		batch.Add(SETUP_POINTER(Scr_Initialized), "38 ? ? ? ? ? 0F 85 ? ? ? ? 48 ? 4F B6 E2 AE 25 2E C1 17", SETUP_MOD(Add(2).Rip()));

		batch.Add(SETUP_POINTER(unk_ContentManager), "4C 8B 35 ? ? ? ? 4C 89 BC 24 ? ? ? ? 49 BF", SETUP_MOD(Add(3).Rip()));

		batch.Add(SETUP_POINTER(unk_Config_1), "80 3D ? ? ? ? ? 48 8D 15 ? ? ? ? 48 8B 3F 48 8B C8 48 0F 45 3D ? ? ? ? E8 ? ? ? ?",
			SETUP_MOD(Add(2).Rip().Add(1)));

		batch.Add(SETUP_POINTER(unk_Config_2), "80 3D ? ? ? ? ? 75 58 33 C9 48 89 5C 24 ?", SETUP_MOD(Add(2).Rip().Add(1)));

		batch.Add(SETUP_POINTER(unk_WatermarkFont), "48 89 05 ? ? ? ? E8 ? ? ? ? 48 8D 0D ? ? ? ? 48 89 05 ? ? ? ? E8 ? ? ? ? 48 89 05 ? ? ? ? 48 89 1D",
			SETUP_MOD(Add(3).Rip()));

		LOG("Pointers", INFO, "ctor: scanning game-module signatures...");
		batch.ScanAll();
		LOG("Pointers", INFO, "ctor: game-module scan done (g_svMaxClients={}).",
			static_cast<void*>(this->m_g_svMaxClients));

		// Phase 3 lever-1: resolve the loopback co-op anchors (base + RVA, provisional). No calls;
		// pure address math + logging. Everything they feed stays dormant until the menu triggers it.
		this->ResolveCoopAnchors(reinterpret_cast<std::uintptr_t>(Common::Utility::NT::Library().GetPtr()));

		// The Lua C API, wrapped in Arxan return-address thunks. Address math and one 5-byte
		// verification read; nothing is called until the Debug tab asks for a walk.
		this->ResolveLuaApi(reinterpret_cast<std::uintptr_t>(Common::Utility::NT::Library().GetPtr()));

		// Local Demonware backend: replace the two embedded RSA public keys so replies from our
		// local server verify. No-ops unless DwBackend::g_Enabled and cw-mod/dwserver/*_pub.der exist.
		DwBackend::PatchEmbeddedKeys(reinterpret_cast<std::uintptr_t>(Common::Utility::NT::Library().GetPtr()));

		batch = SignatureStore("ntdll.dll");

		// Windows 10
		batch.Add(SETUP_POINTER(RtlDispatchException), "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 48 8D 6C 24 ? 48 89 9D ? ? ? ? 48 8B 05 ? ? ? ? 48"
			" 33 C5 48 89 85 ? ? ? ? 65 48 8B 04 25 ? ? ? ? 33 DB");
		// Windows 11
		batch.Add(SETUP_POINTER(RtlDispatchException), "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 48 8D 6C 24 ? 48 89 9D ? ? ? ? 48 8B 05 ? ? ? ? 48"
			" 33 C5 48 89 85 ? ? ? ? 33 F6");

		batch.ScanAll();
		LOG("Pointers", INFO, "ctor: ntdll scan done; calling SetUsername/SetMode...");

		// Offline/LAN name. On a backend login the auth ticket's name wins, and that one comes from the
		// same setting via the studio token (DwLogin_BuildStudioToken).
		this->m_Unk_SetUsername(this->m_LiveUser_GetUserDataForController(0), Settings::PlayerName().c_str());
		// After PatchEmbeddedKeys above: the profile snapshots whether the backend is enabled.
		Boot::Resolve();
		Boot::ApplyEarly(*this);
		LOG("Pointers", INFO, "ctor: SetUsername/SetMode done.");
	}
}
