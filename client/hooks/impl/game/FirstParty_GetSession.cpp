#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/dw_backend.hpp"
#include "game/dump_anchors.hpp"
#include "game/settings.hpp"

#include <utility/nt.hpp>

#include <intrin.h>
#include <mutex>

// THE NULL FIRST-PARTY SESSION PROBE.
//
// What it is for. Completing the Demonware login for the first time (2026-08-02, "[status code=27]
// Login Complete") immediately produced a null dereference:
//
//     FirstParty_GetSessionFieldD0_NULLDEREF (0x7FF7297D9EE0)
//       *(uint*)( *(qword*)(g_firstPartyManager + 24) + 0xD0 )     // 0xC0000005 reading 0xD0
//
// g_firstPartyManager+24 is the Battle.net first-party SESSION object. Its manager's constructor
// (0x7FF7297DD5A0) sets that field to 0 explicitly, and nothing on the studio-auth path ever
// assigns it: login flow 9 goes straight to the Demonware auth task and skips Battle.net
// first-party entirely — a bypass we installed on purpose. All 15 call sites of the getter
// dereference the result without a null check.
//
// This is a NEWLY EXPOSED gap, not a regression. It is post-login code; until the LSG lobby reply
// was accepted the flow never passed status 18, so nothing downstream ever asked for a session.
//
// WHAT WE ACTUALLY WANT TO LEARN, and why a plain crash will not tell us. The fix has two very
// different shapes — synthesise a session object, or find the post-login step that should have
// created one and let it run — and choosing needs the WHOLE set of call sites that ask for a
// session, not just whichever one faults first. A crash gives us exactly one. So the detour
// substitutes a benign stand-in, the boot carries on, and every asking call site is censused.
//
// ============================================================================================
// THE CENSUS RAN, 2026-08-02. RESULT: ONE SITE, ONE CALL, ONE FIELD.
//
// The boot reached the main menu and logged exactly one line:
//     null session asked for from BlackOpsColdWar.exe+0xCC19EF1 (site #1)
// which is 0x7FF7297D9EF1 — the return of the call inside FirstParty_GetLocalUserIndex_ViaSession,
// i.e. the very function that used to crash. The other 98 xrefs to the getter never ran, and no
// virtual call on the stand-in ever happened. The static "15 call sites dereference it unchecked"
// was a count of what COULD run; what DOES run is one.
//
// And session+0xD0 turned out to be nothing exotic: the LOCAL USER / CONTROLLER INDEX. Its only
// consumer, FirstParty_RefreshUserRequest, hands it straight to BdUser_SubmitRequestForLocalUser as
// the user index. So 0 is the CORRECT value on a single-local-user PC, not a placeholder that
// happens to survive.
//
// CONSEQUENTLY the stand-in is retired as the default. Keeping it would mean a fake object with a
// fake vtable circulating through engine code forever to serve one integer — the census earned the
// right to stop doing that. HK_FirstParty_GetLocalUserIndex below returns the 0 directly, and this
// detour stays behind as a pure WATCHMAN: it returns null exactly as the original does (changing
// nothing) but names any future call site in the log BEFORE that site can crash. Set
// "fpsession_standin": true (cw-mod.json) to re-arm the stand-in and census a new site the way the first run did.
// ============================================================================================
//
// DELIBERATELY NOT DONE: runtime logging of which FIELD offsets get read (PAGE_GUARD + VEH on the
// stand-in page). It is not needed and it is the risky part. The call-site return address already
// identifies the reading code, and decompiling those ~15 sites in the IDB gives exact offsets with
// full context and zero runtime risk. Static analysis is the better tool for that half.

namespace {
	using namespace Client;

	// One page, zeroed, so any field read up to 0x1000 yields 0 instead of faulting. The observed
	// read is at +0xD0; the page gives generous headroom without pretending we know the real size.
	constexpr std::size_t kStandInSize = 0x1000;
	// Slot count is arbitrary but ample; real bdLobby/first-party vtables are far shorter.
	constexpr std::size_t kFakeVtableSlots = 64;
	constexpr std::size_t kMaxSightings = 64;

	std::mutex g_Lock;

	std::uint8_t* g_StandIn = nullptr;
	void* g_FakeVtable[kFakeVtableSlots]{};

	struct Sighting {
		std::uintptr_t m_ReturnAddress;
		std::uint64_t  m_Count;
		bool           m_Virtual;
	};
	Sighting g_Sightings[kMaxSightings]{};
	std::size_t g_SightingCount = 0;

	bool g_LogOnly = false;
	bool g_Initialised = false;

	std::uintptr_t GameBase() {
		static const std::uintptr_t base = [] {
			const Common::Utility::NT::Library game;
			return reinterpret_cast<std::uintptr_t>(game.GetPtr());
		}();
		return base;
	}

	// RVA against the game module, so a logged address can be pasted straight into the IDB
	// (dump VA = kDumpImagebase + rva).
	std::uintptr_t ToRva(std::uintptr_t addr) {
		const std::uintptr_t base = GameBase();
		return (addr >= base) ? (addr - base) : addr;
	}

	// Every slot of the stand-in's vtable points here, so a virtual call on the fake session logs
	// and returns 0 rather than jumping through a null vtable pointer.
	//
	// ABI note: on x64 Windows the caller cleans up and integer/pointer returns come back in rax,
	// so one fixed-signature stub is safe for any slot whose real signature returns an integer or
	// pointer. A slot returning a float or a large struct by value would be mis-handled — acceptable
	// in an instrument whose whole job is to survive long enough to enumerate call sites, and called
	// out here so it is not mistaken for a general-purpose shim.
	std::uint64_t __fastcall FakeSessionVirtual(void*);

	void RecordSighting(std::uintptr_t ra, bool isVirtual) {
		std::lock_guard<std::mutex> lock(g_Lock);
		for (std::size_t i = 0; i < g_SightingCount; ++i) {
			if (g_Sightings[i].m_ReturnAddress == ra && g_Sightings[i].m_Virtual == isVirtual) {
				++g_Sightings[i].m_Count;
				return;
			}
		}
		if (g_SightingCount >= kMaxSightings) {
			return;
		}
		g_Sightings[g_SightingCount++] = Sighting{ ra, 1, isVirtual };
		// Logged on FIRST sighting and never repeated: the set of call sites is the result, and this
		// getter can run every frame. Logging as it happens (rather than at exit) means a boot that
		// still dies keeps its census — the same reason DwNet's journal is append-and-flush.
		LOG("FirstParty", INFO,
			"null session {} from {}+0x{:X} (site #{})",
			isVirtual ? "VIRTUAL CALL" : "asked for",
			Client::g_GameModuleName, ToRva(ra), g_SightingCount);
	}

	std::uint64_t __fastcall FakeSessionVirtual(void*) {
		RecordSighting(reinterpret_cast<std::uintptr_t>(_ReturnAddress()), true);
		return 0;
	}

	// Built once, lazily, on the first null sighting — so a boot that never hits the null path pays
	// nothing and allocates nothing.
	bool EnsureStandIn() {
		if (g_Initialised) {
			return g_StandIn != nullptr;
		}
		g_Initialised = true;

		// Default is now LOG-ONLY: the census is finished and the stand-in is no longer how the one
		// known site gets served. The setting is therefore inverted from the first run — it OPTS IN to
		// the stand-in, for the day a new site shows up and we want to survive it and census the rest.
		g_LogOnly = !Client::Game::Settings::Get().fpsessionStandin;

		if (g_LogOnly) {
			LOG("FirstParty", INFO,
				"first-party session is NULL on this boot (studio auth never creates one), as expected. "
				"WATCHMAN mode: returning null exactly as the original does. The one known consumer is "
				"served by the FirstParty_GetLocalUserIndex detour instead, so no stand-in is needed. "
				"Set \"fpsession_standin\": true in cw-mod.json to re-arm the stand-in and census a NEW site.");
			return false;
		}

		auto* const page = static_cast<std::uint8_t*>(
			::VirtualAlloc(nullptr, kStandInSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (!page) {
			LOG("FirstParty", ERROR, "stand-in allocation failed — falling back to log-only.");
			g_LogOnly = true;
			return false;
		}
		std::memset(page, 0, kStandInSize);

		for (auto& slot : g_FakeVtable) {
			slot = reinterpret_cast<void*>(&FakeSessionVirtual);
		}
		// vtable pointer at +0; every other field stays zero.
		*reinterpret_cast<void**>(page) = g_FakeVtable;

		g_StandIn = page;
		LOG("FirstParty", INFO,
			"first-party session is NULL on this boot (studio auth never creates one). STAND-IN mode "
			"(\"fpsession_standin\" on in cw-mod.json): substituting a zeroed object so the boot continues "
			"and every asking call site gets censused. Stand-in at 0x{:X}, {} bytes, fake vtable with "
			"{} stub slots.",
			reinterpret_cast<std::uintptr_t>(page), kStandInSize, kFakeVtableSlots);
		return true;
	}
}

template <>
void* Client::Hook::Hooks::HK_FirstParty_GetSession::hkCallback(void* mgr) {
	// Reproduced rather than trampolined: the target is two instructions
	// (mov rax,[rcx+18h] / ret) and calling m_Original through a 5-byte-function trampoline buys
	// nothing. The null check on `mgr` is the one difference from the original, which would simply
	// fault — and a faulting manager pointer is a different bug worth naming rather than hiding.
	if (!mgr) {
		RecordSighting(reinterpret_cast<std::uintptr_t>(_ReturnAddress()), false);
		LOG("FirstParty", ERROR, "getter called with a NULL manager — not the +24 case.");
		return nullptr;
	}

	void* const real = *reinterpret_cast<void**>(
		static_cast<std::uint8_t*>(mgr) + Client::Game::kFirstPartyMgr_SessionOffset);

	// FAST PATH, and the common one on any boot where first-party actually signed in: byte-identical
	// to the original, no lock, no logging, no allocation.
	if (real) {
		return real;
	}

	const std::uintptr_t ra = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
	RecordSighting(ra, false);

	{
		std::lock_guard<std::mutex> lock(g_Lock);
		if (!EnsureStandIn() || g_LogOnly) {
			return nullptr;
		}
		return g_StandIn;
	}
}
