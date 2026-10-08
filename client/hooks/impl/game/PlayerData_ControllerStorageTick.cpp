#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/boot_profile.hpp"
#include "game/game_internal.hpp"
#include "game/dump_anchors.hpp"
#include "game/zm_progression.hpp"

#include <atomic>
#include <format>
#include <string>

// The dwuser -> hdd rewrite, applied just before the load driver submits its first location.
//
// WHAT THIS FIXES. The online frontend's director menu raises at x64:4be06f4309b61a6f.lua:6052 because
// the DDL-instance native returned nil for dataMapId 4. That was chased to IsBufferReady cause 6 —
// present but entry+120 == 0 — and then to the reason the flag never gets set. Measured on an online
// boot: fifty storage completions, every one hdd(0) with result 0, and map 4 (storageLocation dwuser)
// absent from the transcript entirely. Its read is not queued, not failed, not retried.
//
// The function we are hooking is the reason:
//
//     for (loc = 0; loc < 5; ++loc) {
//         if (!submitted[loc]) {
//             if (PlayerDataStorage_IsAvailable(controller, loc)) {
//                 if (PlayerData_SubmitLocationLoad(controller, loc)) submitted[loc] = 1;
//             }
//             ... else a split-screen-only borrow path for loc == 1 ...
//         }
//         for (j = 0; j < 2; ++j) PlayerDataStorage_RunQueuedOps(controller, loc, j);
//     }
//
// With nodw=true, dwuser is never available, so location 1 is simply skipped. There is no failure to
// observe because nothing is ever attempted. Ordering was never the answer, and neither was a veto.
//
// SubmitLocationLoad selects entries by *def* storage location (`*(_DWORD *)(entry.def + 64) != loc`),
// so moving a def to hdd is enough to get it carried out with the location-0 batch.
//
// WHY THIS SEAM. The engine ships this rewrite in PlayerData_Init:
//
//     if (def.storageLocation == 1 && Dvar_GetBool(dvar_playerData_forceLocalStorage))
//         def.storageLocation = 0;                                  // 0x7FF7275C187E
//
// so the obvious place was Init's own window, hooking PlayerData_AllocateStoreAndBuildEntries on the
// next line. That was built and MEASURED NOT TO FIRE: Init runs before PostArxanDetectionHooks, so the
// detour went onto a function that had already been called. The log showed the install line and no
// body line, which is exactly what that hook was shaped to reveal.
//
// This function has the property the init seam lacked: it is self-timing. It is the code that submits
// locations, so its first call is by construction before location 0 has gone out — submitted[0] is
// still clear — and no race with init exists. The rewrite lands, then the very same call submits the
// rewritten maps along with the genuine hdd ones.
//
// Three alternatives were rejected, each for a concrete reason:
//   - Setting the dvar. The dump's slot for dvar_playerData_forceLocalStorage resolves to a dvar_t
//     whose hash is r_norefresh, which is plainly wrong; its runtime identity has never been pinned.
//     Writing a bool into an unidentified dvar to fix a storage bug is not a trade worth making, and
//     this path needs no dvar at all.
//   - The existing overlay button (Pointers::ForcePlayerDataLocalStorage). Same stores, wrong moment:
//     it can only be clicked from a frontend that, in online mode, does not finish building. It is
//     also not merely late but permanently late — submitted[loc] latches, so once location 0 has been
//     submitted, a def rewritten afterwards is never revisited.
//   - Forcing entry+120 = 1 by hand. That marks a buffer readable without ever filling it from the
//     map's DDL defaults, which hands the accessors a structurally invalid instance. Rewriting the
//     def instead lets the engine's own read path run and produce a real one.
//
// SCOPE. Online boots, and offline/LAN boots while ZM progression is on (game/zm_progression.hpp,
// opt out with "progression": false). Offline, the progression maps 5/18/19 are dwuser too, so
// without this they never load and nothing the match records has anywhere to go. The pass-through is
// logged with the same detail as the action, so a boot still proves the hook fired and chose not to act.
//
// THREADING. The tick is a main-thread frame call. The rewrite happens once, guarded by an atomic
// flag, and touches only def+64 — a field the engine reads but never writes after init.

namespace {
	std::atomic_bool g_Rewritten{ false };
}

template <>
char Client::Hook::Hooks::HK_PlayerData_ControllerStorageTick::hkCallback(unsigned int controller) {

	using P = Client::Game::Pointers;

	// exchange, not a plain read: the rewrite must happen exactly once even though the tick is called
	// per controller per frame.
	if (!g_Rewritten.exchange(true)) {
		if (!Client::Game::Boot::Current().IsOnline() && !Client::Game::ZmProgression::Enabled()) {
			LOG("PlayerData", INFO, "load driver: offline/LAN boot with ZM progression off — dwuser data "
				"maps left as they are. (This hook fired and deliberately did nothing; it is not a no-op.)");
		}
		else if (auto* const defs = Client::g_Pointers->m_g_playerDataDefsById) {
			// Report whether we actually got here in time. If location 0 has already been submitted,
			// the rewrite below is cosmetic and the boot will fail the same way — say so rather than
			// letting a green log line imply success.
			int alreadySubmitted = -1;
			if (auto* const store = Client::g_Pointers->m_g_playerDataStore) {
				std::uint8_t flag = 0;
				if (Client::Game::SafeRead(store + static_cast<std::size_t>(controller) * P::kPdStoreStride
						+ Client::Game::kPdStoreSubmittedFlagsOff, flag)) {
					alreadySubmitted = flag;
				}
			}

			// Each store is the same aligned 4-byte write the engine performs at 0x7FF7275C187E;
			// storageLocation is only ever read after init, so no locking is involved.
			int rewritten = 0, scanned = 0;
			std::string ids;
			for (int id = 1; id <= Client::Game::kPdMaxDataMapId; ++id) {
				std::uint8_t* def = nullptr;
				if (!Client::Game::SafeRead(defs + id, def) || !def) continue;
				++scanned;

				int loc = 0;
				if (!Client::Game::SafeRead(def + P::kPdDef_StorageLoc, loc)) continue;
				// dwclan/memory/fastfile are not ours to redirect — only the Demonware *user* location
				// has a local equivalent, and only that one carries the progression maps.
				if (loc != P::kPdLoc_DwUser) continue;

				*reinterpret_cast<volatile int*>(def + P::kPdDef_StorageLoc) = P::kPdLoc_Hdd;
				++rewritten;
				Client::Game::PlayerDataRedirect::MarkRedirected(id);
				if (!ids.empty()) ids += ",";
				ids += std::to_string(id);
			}

			// Seed the buffers with the maps' own DDL defaults, now that we can.
			//
			// Redirecting the def gets these maps read, and the reads FAIL — measured, 14 of them —
			// because the data has only ever lived on Demonware and no local .cgp exists. A failed
			// read leaves the buffer exactly as it was: zeroed, with no DDL structure in it. That is
			// not a readable instance, so settling the flag over it would hand the accessors garbage.
			//
			// ResetBufferToDefaults is the engine's own answer to "there is nothing to read": it
			// builds a real instance via Ddl_CreateInstance and then sets entry+120 = 1 — but only
			// if PlayerDataStorage_IsAvailable(controller, def.storageLocation) agrees. That check is
			// exactly why this call was useless before: it tested dwuser. The loop above has just
			// made it test hdd, which every successful read this boot proves is available. Same call,
			// same engine code, newly viable.
			//
			// Main thread, and before the reads are submitted, so the completion that arrives later
			// finds a populated buffer rather than racing us for it.
			//
			// EVERY version, not just the default (-1). Maps 32 (20 versions) and 33 (10) are never read
			// at all, so seeding only v0 left 28 entries at entry+120 = 0. Retail skips them, because
			// PlayerData_AreLocalFilesReady ignores dwuser maps, but the rewrite puts them in its loop
			// over every version. That kept Lua's AreLocalFilesReady false, which is the BeginLivePlay
			// gate behind the flashing "Connecting" overlay (measured 21:13). ResetBufferToDefaults finds
			// the (id, version) entry itself, so an explicit version works the same way -1 does.
			int seeded = 0, seededMaps = 0;
			std::string multi;
			if (auto* const reset = Client::g_Pointers->m_PlayerData_ResetBufferToDefaults) {
				for (int id = 1; id <= Client::Game::kPdMaxDataMapId; ++id) {
					if (!Client::Game::PlayerDataRedirect::IsRedirected(static_cast<unsigned>(id)))
						continue;
					std::uint8_t* def = nullptr;
					int versions = 0;
					if (Client::Game::SafeRead(defs + id, def) && def) {
						Client::Game::SafeRead(def + Client::Game::kPdDef_VersionCount, versions);
					}
					if (versions <= 1) {
						reset(controller, static_cast<unsigned int>(id), -1);   // -1 = the def's default version
						++seeded;
					}
					else {
						for (int v = 0; v < versions; ++v) {
							reset(controller, static_cast<unsigned int>(id), v);
							++seeded;
						}
						multi += std::format("{}{} x{}", multi.empty() ? "" : ", ", id, versions);
					}
					++seededMaps;
				}
			}
			else {
				LOG("PlayerData", WARN, "load driver: ResetBufferToDefaults did not resolve through the "
					"Arxan thunk arena — the redirected maps will be read, will fail, and will be left "
					"holding an empty buffer. Expect the director menu to raise as before.");
			}

			LOG("PlayerData", INFO,
				"load driver: rewrote {} of {} data map defs dwuser -> hdd on the first tick "
				"(controller {}, ids {}).\n"
				"    location-0 submitted flag was {} — {}\n"
				"    seeded {} buffers across {} maps with DDL defaults via ResetBufferToDefaults (every version; multi-version maps: {})\n"
				"    Expect these maps in the storage-completion transcript as storage=hdd(0); map 4 "
				"reaching entry+120=1 is what the director menu was waiting for.",
				rewritten, scanned, controller, ids.empty() ? "-" : ids,
				alreadySubmitted,
				alreadySubmitted == 0
					? "clear, so this call's own location-0 pass will carry them"
					: "*** ALREADY SET OR UNREADABLE — we arrived too late and this rewrite will NOT be "
					  "picked up; the boot will fail the same way ***",
				seeded, seededMaps, multi.empty() ? "none" : multi);

			if (rewritten == 0) {
				LOG("PlayerData", WARN, "load driver: no def was dwuser-backed. Either the engine's own "
					"dvar already applied this rewrite, or the def anchor is pointing at the wrong "
					"build — check the probe before reading anything else in this boot as evidence.");
			}
		}
		else {
			LOG("PlayerData", WARN, "load driver: g_playerDataDefsById did not resolve — dwuser data maps "
				"will stay unloadable, and the online director menu will raise on map 4 as before.");
		}
	}

	return m_Original(controller);
}
