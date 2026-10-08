#include "common.hpp"
#include "overlay/menu.hpp"
#include "overlay/tabs/tabs.hpp"
#include "game/game.hpp"
#include "game/boot_profile.hpp"

#include <imgui.h>

#include <mutex>
#include <string>
#include <cfloat>

namespace Client::Overlay::Menu {
	// The co-op slot dump: written on the game thread by the Enqueue'd action, read here on the
	// render thread. Swapping a std::string under a reader can free the buffer it is mid-read of,
	// so both sides take the mutex.
	namespace {
		std::mutex  g_CoopDumpMutex;
		std::string g_CoopDump;
	}

	void DrawSessionTab() {
		ImGui::TextWrapped("Session / network mode. This is the foundation for LAN co-op hosting.");
		ImGui::Separator();

		if (ImGui::Button("Set LAN / Local mode")) {
			Enqueue([] { if (g_Pointers) g_Pointers->SetMode(0, 10); });
		}
		ImGui::SameLine();
		HelpMarker("Puts the lobby into LAN network mode (the base already does this on startup; this re-applies it).");

		// --- Online (live) session mode -------------------------------------------------------
		ImGui::Separator();
		ImGui::TextDisabled("ONLINE (LIVE) SESSION MODE");
		ImGui::TextWrapped("The frontends are BUILT from the session network mode at boot, so online is chosen "
			"by the boot marker below, never by flipping the mode at runtime.");
		ImGui::Text("this boot: %s", Game::Boot::Current().Describe().c_str());

		{
			bool pin = Game::OnlineMode::g_Pin.load(std::memory_order_relaxed);
			if (ImGui::Checkbox("Pin networkMode to 2 (re-apply every frame)", &pin)) {
				Game::OnlineMode::g_Pin.store(pin, std::memory_order_relaxed);
			}
			ImGui::SameLine();
			HelpMarker("Lobby transitions reset the network mode. This re-asserts 2 whenever it drifts.\n\n"
				"Leave it OFF while doing LAN work: LAN discovery selects on networkMode == 1, so pinning 2 "
				"disables it. Online and LAN are mutually exclusive states, not a stack.");

			bool suppress = Game::OnlineMode::g_SuppressBnetErrors.load(std::memory_order_relaxed);
			if (ImGui::Checkbox("Suppress Battle.net errors (BLZBNTBGS)", &suppress)) {
				Game::OnlineMode::g_SuppressBnetErrors.store(suppress, std::memory_order_relaxed);
			}
			ImGui::SameLine();
			HelpMarker("Turns itself ON with online mode; here so you can turn it off and watch the stock failure.\n\n"
				"Going live with no Battle.net latches a first-party STATE_ERROR. That is not just a popup: the "
				"state field (+56) is the SAME one the signed-in check reads, so a latched error also un-signs-in "
				"you.\n\n"
				"The popup is a LUI menu that QUERIES the stored error, which is why it returns on every menu "
				"change until the error is cleared \xe2\x80\x94 the four detours stop it being stored at all.");

			ImGui::TextDisabled("boot mode: \"mode\" in <game>/cw-mod/cw-mod.json  (edit, relaunch)");
			ImGui::TextDisabled("   online=live | lanlobby=nibble 2 + LAN lobby | lan=control | offline");
		}
		ImGui::Separator();

		if (ImGui::Button("Disconnect")) {
			Enqueue([] {
				if (g_Pointers && g_Pointers->m_CL_Disconnect) {
					g_Pointers->m_CL_Disconnect(0, true, "cw-mod");
				}
			});
		}
		ImGui::SameLine();
		HelpMarker("Disconnects the local client from the current session.");

		ImGui::Separator();
		ImGui::TextDisabled("Player cap (com_maxclients)");
		{
			const ImVec4 amber(1.0f, 0.6f, 0.2f, 1.0f);
			const ImVec4 green(0.30f, 0.85f, 0.40f, 1.0f);

			// No engine reads here (render thread). We show the value we last pushed (cached) and the
			// live server cap (a plain global int, safe to read from any thread).
			const int cur = g_Pointers ? g_Pointers->LastSetMaxClients() : -1;
			ImGui::Text("last set: ");
			ImGui::SameLine();
			if (cur < 0) {
				ImGui::TextDisabled("(not set this session)");
			} else {
				ImGui::TextColored(cur > 1 ? green : amber, "%d", cur);
			}

			// g_svMaxClients: the effective cap the running server actually uses (set at map spawn).
			const int live = g_Pointers ? g_Pointers->ServerMaxClients() : -1;
			ImGui::Text("live server cap: ");
			ImGui::SameLine();
			if (live < 0) {
				ImGui::TextColored(amber, "unresolved");
			} else {
				ImGui::TextColored(live > 1 ? green : amber, "%d", live);
			}

			static int desired = 2;
			ImGui::SetNextItemWidth(120.0f);
			ImGui::InputInt("##maxclients", &desired);
			if (desired < 1) desired = 1;
			if (desired > 64) desired = 64;
			ImGui::SameLine();
			if (ImGui::Button("Apply cap")) {
				const int v = desired;
				Enqueue([v] { if (g_Pointers) g_Pointers->SetMaxClients(v); });
			}
			ImGui::SameLine();
			HelpMarker("Sets com_maxclients \xe2\x80\x94 the player cap SV_SpawnServer reads to size the "
				"client-slot array. Set to 2+ BEFORE starting a Zombies map so the server allocates "
				"co-op slots. Domain [1,64]. This is the direct equivalent of typing 'com_maxclients 2' "
				"in a dev console (the console UI itself is stripped from retail). Applied on the game thread.");
		}

		ImGui::Separator();
		ImGui::TextDisabled("Co-op \xe2\x80\x94 loopback 2nd player (Phase 3, lever 1)");
		{
			const ImVec4 amber(1.0f, 0.6f, 0.2f, 1.0f);
			const ImVec4 green(0.30f, 0.85f, 0.40f, 1.0f);

			const bool resolved = g_Pointers && g_Pointers->CoopAnchorsResolved();
			ImGui::Text("anchors: ");
			ImGui::SameLine();
			ImGui::TextColored(resolved ? green : amber, resolved ? "resolved (base+RVA)" : "unresolved");
			ImGui::SameLine();
			HelpMarker("SV_DirectConnect, the in-process loopback reseat, g_svClients and the migration "
				"flag \xe2\x80\x94 provisionally bound by module-base + RVA for build 1.34.0.15931218 "
				"(no AOB yet; the IDB was down when this was wired). 'unresolved' means the RVAs don't "
				"land in-module \xe2\x80\x94 likely a different game build.");

			// Read-only slot inspector: dumps the g_svClients array so we can find the connected-state
			// field empirically. Runs on the game thread (the array can be freed during teardown).
			if (ImGui::Button("Dump client slots")) {
				Enqueue([] {
					if (g_Pointers) {
						std::string dump = g_Pointers->DumpClientSlots();
						std::lock_guard<std::mutex> lock(g_CoopDumpMutex);
						g_CoopDump.swap(dump);
					}
				});
			}
			ImGui::SameLine();
			HelpMarker("Reads g_svClients (safe, read-only) and prints candidate state fields per slot. "
				"Start a Zombies map first, then dump: diff an occupied slot vs a free one to locate the "
				"'connected' field (recon says SV_DirectConnect sets state = 3). This is how we finish the "
				"reversing while the IDB is offline.");

			{
				std::lock_guard<std::mutex> lock(g_CoopDumpMutex);
				if (!g_CoopDump.empty()) {
					ImGui::InputTextMultiline("##coopdump", g_CoopDump.data(), g_CoopDump.size() + 1,
						ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8),
						ImGuiInputTextFlags_ReadOnly);
				}
			}

			// Live seat (Option B, surgical). Enabled once anchors resolve. Fires on the game thread;
			// only meaningful with a Zombies map loaded (needs a running server). Experimental.
			ImGui::BeginDisabled(!resolved);
			if (ImGui::Button("Seat 2nd local player (loopback)")) {
				Enqueue([] { if (g_Pointers) g_Pointers->SeatSecondPlayerLoopback(); });
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextColored(amber, "experimental");
			ImGui::SameLine();
			HelpMarker("Seats a 2nd local client in-process via SV_DirectConnect (Option B: our own "
				"fresh-connect userinfo \xe2\x80\x94 protocol 2426, live netfieldchk, current sessionmode, "
				"unique qport). LOAD A ZOMBIES MAP FIRST (needs a running server). After clicking, use "
				"'Dump client slots' to confirm a new slot reached state 3. Runs on the game thread.");
		}

		ImGui::Separator();
		ImGui::TextDisabled("HOST SIDE (PC1)");
		{
			// The host path is the game's own "Create your match" — we no longer replay a captured
			// launch, so the capture/replay scaffolding is gone (see docs/phase3_test_plan.md).
			// What remains is the one read that says whether the resulting session is on the LAN.
			static std::string s_SessStateDump;
			if (ImGui::Button("Dump session state")) {
				Enqueue([] {
					if (g_Pointers) {
						std::string d = g_Pointers->DumpSessionState();
						s_SessStateDump.swap(d);
					}
				});
			}
			ImGui::SameLine();
			HelpMarker("Read-only. After 'Launch systemlink host', click this to chase g_netSessionManager -> "
				"session -> announce/search state and see whether the host lobby is advertising over LAN "
				"(session state 1=negotiating/advertising, 2=published/discoverable, -2=error). Confirms "
				"discoverability on THIS PC before you set up the 2nd machine. Runs on the game thread.");
			if (!s_SessStateDump.empty()) {
				ImGui::InputTextMultiline("##sessstatedump", s_SessStateDump.data(), s_SessStateDump.size() + 1,
					ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 7), ImGuiInputTextFlags_ReadOnly);
			}

			ImGui::Spacing();
			static std::string s_HostIdentity;
			if (ImGui::Button("Show host descriptor")) {
				Enqueue([] {
					if (g_Pointers) {
						std::string d = g_Pointers->DumpMyHostIdentity();
						s_HostIdentity.swap(d);
					}
				});
			}
			ImGui::SameLine();
			HelpMarker("Read-only. Walks the 6 session objects (2 types x 3 slots) and prints the live ones: "
				"host XUID, name, secid, seckey and the 84-byte serialized address \xe2\x80\x94 exactly the "
				"fields the engine puts into an InfoResponse. Host through the game's own 'Create your match' "
				"FIRST, then hit this and copy the CWJOIN1 line to PC2.\n\n"
				"The blob is valid ONLY for the host's CURRENT session instance: secid/seckey rotate every "
				"time the session is re-created. Re-dump after any re-host.");
			if (!s_HostIdentity.empty()) {
				ImGui::InputTextMultiline("##hostidentity", s_HostIdentity.data(), s_HostIdentity.size() + 1,
					ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 12), ImGuiInputTextFlags_ReadOnly);
			}
		}

		ImGui::Separator();
		ImGui::TextDisabled("CLIENT SIDE (PC2) \xe2\x80\x94 join by descriptor");
		{
			// The XUID probe path was removed: addressing a netmsg by xuid needs a Demonware
			// xuid->address binding with no LAN fallback, so it can never work across two PCs.
			// The descriptor carries a real LAN address and needs no resolution at all.
			const ImVec4 amber(1.0f, 0.6f, 0.2f, 1.0f);
			static int  s_JoinCtxId = 0;
			static int  s_ControllerIdx = 0;
			// NOT "lobby type" \xe2\x80\x94 this is the wire field `jointype`, and it was the whole 2-PC
			// failure. It rides JoinPendingTarget arg4 -> AddHostCandidate arg6 -> dword_7FF730437700 ->
			// NetMsg_BuildSendJoinLobbyRequest arg5 -> request+8. NetMsg_Handle_JoinLobby refuses with
			// verdict 36 unless it is 1 or 4. The old default of 3 (copied from a Lua native) could
			// never be accepted by any host. 4 keeps playlistid at 255; 1 switches the request to the
			// real playlist id, which then has to match the host's too.
			static int  s_JoinType = 4;
			ImGui::SetNextItemWidth(90.0f);
			ImGui::InputInt("ctx", &s_JoinCtxId);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(90.0f);
			ImGui::InputInt("pad", &s_ControllerIdx);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(90.0f);
			ImGui::InputInt("jointype", &s_JoinType);
			ImGui::SameLine();
			HelpMarker("join-context id, controller index, and jointype. jointype MUST be 1 or 4 \xe2\x80\x94 "
				"the host refuses anything else with verdict 36, which is exactly what killed the two-PC "
				"join while this defaulted to 3. Prefer 4: it leaves playlistid at 255, so the request "
				"matches the host on every other compared field. Leave ctx/pad at 0/0.");

			ImGui::Spacing();
			static char s_BlobBuf[512] = "";
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputText("##cwjoinblob", s_BlobBuf, sizeof(s_BlobBuf));
			ImGui::TextDisabled("paste the CWJOIN1 line printed by PC1 above");
			ImGui::SameLine();
			HelpMarker("Skips the InfoRequest/InfoResponse round trip entirely. That round trip's ONLY "
				"product is g_pendingJoinTarget_*, and ClientSession_JoinPendingTarget reads just eight of "
				"those fields \xe2\x80\x94 host xuid, session id, name, slot, secid, seckey, the 84-byte "
				"serialized address, and the valid flag. PC1 can read all eight straight out of its own live "
				"session object (Session_BuildSendInfoResponse copies those exact bytes onto the wire), so we "
				"carry them by hand instead. The join itself is addressed by the serialized address \xe2\x80\x94 "
				"a real LAN address \xe2\x80\x94 so it never needs XUID resolution. Uses the same "
				"ctx/pad/jointype ints above. A malformed paste is rejected before anything is written.");

			static std::string s_BlobJoinStatus;
			if (ImGui::Button("Join host by descriptor")) {
				Enqueue([] {
					if (g_Pointers) {
						s_BlobJoinStatus = g_Pointers->JoinHostByDescriptor(s_BlobBuf, s_JoinCtxId,
							s_ControllerIdx, s_JoinType);
					}
				});
			}
			ImGui::SameLine();
			ImGui::TextColored(amber, "experimental");
			if (!s_BlobJoinStatus.empty()) ImGui::TextWrapped("%s", s_BlobJoinStatus.c_str());

			static std::string s_PendingDump;
			if (ImGui::Button("Dump pending join target")) {
				Enqueue([] {
					if (g_Pointers) {
						std::string d = g_Pointers->DumpPendingJoinTarget();
						s_PendingDump.swap(d);
					}
				});
			}
			ImGui::SameLine();
			HelpMarker("Read-only. Shows whether a probe is outstanding, the nonce, and \xe2\x80\x94 once the "
				"host answers \xe2\x80\x94 the host name and serialized address it sent back. 'valid = YES' "
				"means the descriptor landed and the join can proceed.");
			if (!s_PendingDump.empty()) {
				ImGui::InputTextMultiline("##pendingdump", s_PendingDump.data(), s_PendingDump.size() + 1,
					ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8), ImGuiInputTextFlags_ReadOnly);
			}
		}

		ImGui::Separator();
		ImGui::TextDisabled("DIAGNOSTICS");
		{
			// The netmsg transcript. This is the one instrument that reads the far machine directly:
			// it sits on the lobby message dispatcher, INSIDE the process, so it sees the payload after
			// transport decryption \xe2\x80\x94 which a packet capture provably cannot.
			static std::string s_NetMsgStatus;
			const bool netMsgOn = g_Pointers && g_Pointers->NetMsgTranscriptInstalled();
			if (ImGui::Button(netMsgOn ? "Remove netmsg transcript" : "Install netmsg transcript")) {
				Enqueue([netMsgOn] {
					if (g_Pointers) {
						std::string r = netMsgOn
							? g_Pointers->RemoveNetMsgTranscript()
							: g_Pointers->InstallNetMsgTranscript();
						s_NetMsgStatus.swap(r);
					}
				});
			}
			ImGui::SameLine();
			HelpMarker("Install on BOTH PCs before joining. Logs every inbound lobby message "
				"(msgId, name, source address, whether a handler existed) into the join log below and "
				"into client_join_args.txt, plus the host's JoinResponse verdict with its refusal "
				"reason. Read-only: it logs and calls through. Default off, fully removable. "
				"Verdict 1 = accepted; anything else is a refusal, and the reason is printed.");
			if (!s_NetMsgStatus.empty()) ImGui::TextWrapped("%s", s_NetMsgStatus.c_str());

			ImGui::Spacing();

			// The join transcript is the only admissible evidence on this side: every other client
			// signal (popups, "message sent" returns) reports a local belief, not a network fact.
			if (g_Pointers) {
				std::string log = g_Pointers->ClientJoinCaptureLog();
				ImGui::InputTextMultiline("##joincaplog", log.data(), log.size() + 1,
					ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8),
					ImGuiInputTextFlags_ReadOnly);
			}

			ImGui::Spacing();
			static std::string s_JoinStateDump;
			if (ImGui::Button("Dump client-join state")) {
				Enqueue([] {
					if (g_Pointers) {
						std::string d = g_Pointers->DumpClientJoinState();
						s_JoinStateDump.swap(d);
					}
				});
			}
			ImGui::SameLine();
			if (ImGui::Button("Arm join watcher (15s)")) {
				Enqueue([] { if (g_Pointers) { g_Pointers->StartJoinWatch(); } });
			}
			ImGui::SameLine();
			HelpMarker("Read-only. Decodes g_clientJoinCtx: FSM state (1=pick host, 2=sent JoinLobby, "
				"3=awaiting JoinResponse, 6=JOINED), the discovered host candidates with their names and "
				"netadrs, and cl_lobbyLaunchState. The watcher polls the same state every 2ms for 15s and "
				"logs every transition into the join log above \xe2\x80\x94 the join marches faster than you "
				"can click. Stuck at 1 with 0 candidates = nothing was discovered; stuck at 3 = the host "
				"never answered.");
			if (!s_JoinStateDump.empty()) {
				ImGui::InputTextMultiline("##joinstatedump", s_JoinStateDump.data(), s_JoinStateDump.size() + 1,
					ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 10), ImGuiInputTextFlags_ReadOnly);
			}
		}
	}
}
