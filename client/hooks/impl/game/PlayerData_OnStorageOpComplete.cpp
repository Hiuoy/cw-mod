#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/boot_profile.hpp"
#include "game/game_internal.hpp"
#include "game/dump_anchors.hpp"
#include "game/zm_progression.hpp"

#include <atomic>
#include <string>

// The async storage transcript — where a data map actually becomes readable.
//
// This hook exists because the previous one was aimed at the wrong function. The reasoning was that
// PlayerData_ResetBufferToDefaults is the only writer of entry+120, so watching it would show every
// load. Measured on an online boot: it fired ONCE, for map 1, which was already loaded — while
// thirteen maps went from unloaded to loaded in the same few seconds. The claim came from an old IDB
// comment that was repeated without being checked, which is precisely the failure this project has a
// rule against.
//
// The real writer, on the load path, is here. a4 is the ENTRY itself (a4[30] is entry+120, *a4 is the
// dataMapId), and on a kind-0 (READ) completion this function decides the flag:
//
//     resultCode 0, DDL import succeeds       -> entry+120 = 1     the map is now readable
//     resultCode 0, entry.def is null         -> entry+120 = 1
//     resultCode 0, DDL import FAILS          -> entry+120 = 1, result forced to 2, logs badDDL_*
//     resultCode != 0 (the read failed)       -> entry+120 = 1
//     resultCode 3 and storageLocation == 4   -> entry+120 = 2     fastfile absent
//     ...then a per-map backend hook may veto -> entry+120 = 0     and a retry is armed
//
// Note what that list means: entry+120 becomes 1 on failures too. "Loaded" here is closer to
// "settled" than to "succeeded", which is why the resultCode is logged next to it rather than being
// collapsed into a boolean. A map can be readable and hold defaults because its read failed.
//
// So the load is ASYNCHRONOUS — SubmitLocationLoad queues the reads, a job thread runs them, and the
// completions arrive over several seconds. That is why the LOADED set grew across the six DDL-instance
// calls in the last boot, and it makes ordering the leading explanation for the director menu's nil.
// It does not make it the confirmed one: if map 4's read never completes, or completes with a veto,
// that is a different bug with a different fix. This transcript distinguishes them.
//
// THREADING. This runs on the storage job thread as well as the main thread. Everything here is a
// bounded SafeRead plus an atomic counter; nothing calls into Lua, allocates engine memory, or
// touches shared mod state. The log sink is already used from multiple threads elsewhere.

namespace {
	std::atomic<int> g_CompletionCount{ 0 };

	// The map the online frontend's director menu asks for. Marked in the transcript so the line can
	// be found without counting; every completion is logged regardless.
	constexpr unsigned int kDataMapIdOfInterest = 4;

	// entry[30] — the engine indexes the entry as unsigned int*, so this is entry+120.
	constexpr std::size_t kEntryLoadedIndex = 30;

	const char* OpKindName(int opKind) {
		switch (opKind) {
		case 0:  return "READ";
		case 1:  return "WRITE";
		default: return "?";
		}
	}

	// The engine's own result codes, as far as this function distinguishes them. 3 is singled out
	// because it is the one the fastfile branch tests for.
	const char* ResultName(unsigned int result) {
		switch (result) {
		case 0:  return "ok";
		case 1:  return "failed";
		case 2:  return "bad DDL (import rejected the bytes)";
		case 3:  return "not found";
		default: return "?";
		}
	}

	const char* LoadedFlagMeaning(int flag) {
		switch (flag) {
		case 0:  return "NOT readable — a backend hook vetoed this completion and armed a retry";
		case 1:  return "readable (IsBufferReady's entry+120 test now passes)";
		case 2:  return "fastfile absent";
		default: return "unexpected";
		}
	}

	// A bounded read that yields something printable rather than a bool to branch on — these are
	// diagnostics inside a log line, and an unreadable field is worth seeing as -1.
	int ReadEntryInt(const std::uint8_t* entry, std::size_t off) {
		int v = -1;
		if (entry) Client::Game::SafeRead(entry + off, v);
		return v;
	}

	// Name the two callbacks that can veto a completion, as module-relative RVAs.
	//
	// Worth the four lines: the veto is the whole reason this fix exists, and the registrars
	// (sub_7FF7275C29B0 / sub_7FF7275C29E0) take the dataMapId as a RUNTIME argument, so which
	// function owns map 4's slots cannot be answered statically without a hunt. Printing the live
	// pointers answers it from the log the fix already produces, instead of costing another boot if
	// the override turns out to misbehave.
	std::string DescribeVetoCallbacks(unsigned int dataMapId) {
		auto* const table = Client::g_Pointers->m_g_playerDataMapCallbacks;
		if (!table || dataMapId > static_cast<unsigned int>(Client::Game::kPdMaxDataMapId))
			return {};

		void* slot0 = nullptr;
		void* slot1 = nullptr;
		Client::Game::SafeRead(table + 4ull * dataMapId + 0, slot0);
		Client::Game::SafeRead(table + 4ull * dataMapId + 1, slot1);

		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
		const auto rva = [base](void* p) -> std::uintptr_t {
			return p ? reinterpret_cast<std::uintptr_t>(p) - base : 0;
		};
		return std::format("\n    veto candidates for map {}: slot[0] +0x{:X}, slot[1] +0x{:X} "
			"(0 = not registered)", dataMapId, rva(slot0), rva(slot1));
	}

	const char* StorageLocName(int loc) {
		switch (loc) {
		case 0:  return "hdd";
		case 1:  return "dwuser";
		case 2:  return "dwclan";
		case 3:  return "memory";
		case 4:  return "fastfile";
		default: return "?";
		}
	}
}

template <>
void* Client::Hook::Hooks::HK_PlayerData_OnStorageOpComplete::hkCallback(
	unsigned int controller, int opKind, unsigned int resultCode, unsigned int* entry) {

	using P = Client::Game::Pointers;

	// Read the entry's identity BEFORE the call. The completion can rewrite the flag and the engine
	// may recycle the entry afterwards, so anything we want to name has to be captured here.
	unsigned int dataMapId = 0xFFFFFFFFu;
	int version = -1;
	int loadedBefore = -1;
	int storageLoc = -1;

	auto* const e = reinterpret_cast<std::uint8_t*>(entry);
	if (e) {
		Client::Game::SafeRead(e + P::kPdEntry_DataMapId, dataMapId);
		Client::Game::SafeRead(e + P::kPdEntry_Version, version);
		Client::Game::SafeRead(e + P::kPdEntry_Loaded, loadedBefore);

		void* def = nullptr;
		if (Client::Game::SafeRead(e + P::kPdEntry_Def, def) && def) {
			Client::Game::SafeRead(
				reinterpret_cast<std::uint8_t*>(def) + P::kPdDef_StorageLoc, storageLoc);
		}
	}

	void* const result = m_Original(controller, opKind, resultCode, entry);

	int loadedAfter = -1;
	if (e) {
		Client::Game::SafeRead(e + P::kPdEntry_Loaded, loadedAfter);
	}

	const int index = g_CompletionCount.fetch_add(1) + 1;

	// The transition, stated rather than left to be read off two integers.
	//
	// CORRECTED TWICE. It first printed "entry+120 unchanged at 0" whenever before == after, which
	// was actively misleading: the decompile shows a failed READ sets the flag to 1 and a per-map
	// callback then puts it back to 0, so a veto arrived here looking like nothing had happened.
	//
	// The second correction is the condition below. A veto was defined as 0 -> 0, which was true only
	// while the buffers started out unloaded. Now that the tick seeds the redirected maps with DDL
	// defaults BEFORE their reads go out, the same veto presents as a plain, directly observed
	// 1 -> 0 — and the 0 -> 0 spelling stopped matching the case it was written for. What actually
	// identifies a veto is the END state: a READ completion has no path that leaves entry+120 at 0
	// except a callback putting it there. Before-value only decides whether the middle 1 was seen or
	// has to be deduced.
	std::string transition;
	const bool vetoed = opKind == P::kPdKind_Read && loadedAfter == 0;
	if (vetoed) {
		transition = std::format("entry+120 {} -> 0 — VETOED (the completion settled it at 1, then a "
			"per-map callback rejected it; retry attempt {}, next fire {})",
			loadedBefore == 0 ? std::string{ "0 -> 1" } : std::to_string(loadedBefore),
			ReadEntryInt(e, P::kPdEntry_ReadRetryAttempt),
			ReadEntryInt(e, P::kPdEntry_ReadRetryNextTime));
	}
	else if (loadedBefore == loadedAfter) {
		transition = std::format("entry+120 unchanged at {} ({})", loadedAfter,
			LoadedFlagMeaning(loadedAfter));
	}
	else {
		transition = std::format("entry+120 {} -> {} ({})", loadedBefore, loadedAfter,
			LoadedFlagMeaning(loadedAfter));
	}

	// Undo the veto — for the maps WE redirected, and only for those.
	//
	// The veto is the engine refusing a buffer whose read failed, which is the correct instinct for
	// a map that genuinely should have had data. These maps should not: they lived on Demonware, we
	// pointed them at a local disk that has never held them, and the tick has already filled their
	// buffers from the map's own DDL defaults. That is precisely the state a real first run produces,
	// so settling the flag over it is honest rather than a fudge.
	//
	// The retry counters at entry+132/136 are deliberately NOT touched. Zeroing them was the first
	// design, on the theory that an armed retry would re-read the absent file and veto the defaults
	// away again. Two things argue against it. Neither PlayerData_ControllerStorageTick nor
	// PlayerData_SubmitStorageOp reads those fields, so the consumer has not been identified — and
	// writing engine state whose reader is unknown to prevent a problem that has not been observed is
	// the wrong order of operations. Measured: map 4's read fired exactly once across 66 completions,
	// so nothing is re-firing on the boot timescale anyway.
	//
	// Instead this override is IDEMPOTENT. It runs on every vetoed completion, so if a retry ever does
	// land and veto again, the next completion restores the flag. That is strictly more robust than
	// disarming a mechanism we cannot yet read, and it costs nothing. The retry fields are still
	// printed, so a retry storm would be visible as a rising attempt count rather than a silent one.
	//
	// Stock maps keep stock behaviour, veto included: if a genuine hdd map's read fails, that is a
	// real problem and should stay visible.
	std::string overrideNote;
	if (vetoed && (Client::Game::Boot::Current().IsOnline() || Client::Game::ZmProgression::Enabled())
		&& Client::Game::PlayerDataRedirect::IsRedirected(dataMapId)) {

		*reinterpret_cast<volatile int*>(e + P::kPdEntry_Loaded) = 1;

		overrideNote = std::format("\n    cw-mod: redirected map, no local file has ever existed and the "
			"buffer already holds DDL defaults — forced entry+120 back to 1.{}",
			DescribeVetoCallbacks(dataMapId));
	}
	else if (vetoed) {
		overrideNote = std::format("\n    cw-mod: left alone (not a redirected map, or the redirect is off this boot).{}",
			DescribeVetoCallbacks(dataMapId));
	}

	// Not capped. This fires a bounded number of times per boot, once per map per controller, and the
	// single line that matters may be the last one.
	LOG("PlayerData", INFO,
		"storage {} #{}{}: controller={} map={} v{} storage={}({}) result={} ({})\n"
		"    {}{}",
		OpKindName(opKind), index,
		dataMapId == kDataMapIdOfInterest ? "  <-- the map the director menu asks for" : "",
		controller, dataMapId, version,
		StorageLocName(storageLoc), storageLoc,
		resultCode, ResultName(resultCode),
		transition, overrideNote);

	return result;
}
