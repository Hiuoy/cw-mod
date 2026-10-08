#include "common.hpp"
#include "overlay/menu.hpp"
#include "overlay/tabs/tabs.hpp"
#include "game/dw_net.hpp"
#include "game/dw_backend.hpp"
#include "game/game.hpp"

#include <imgui.h>

#include <cfloat>
#include <mutex>
#include <string>

namespace Client::Overlay::Menu {
	namespace {
		// Written on the game thread by the Enqueue'd probe, read here on the render thread.
		std::mutex  g_DwLoginDumpMutex;
		std::string g_DwLoginDump;
	}

	// The read-out for the winsock choke point.
	//
	// This tab exists because the instrument did not: DwNet::Report() was written, and then had no
	// caller anywhere in the codebase, so every endpoint the choke point observed went to the log and
	// nowhere a person could look at it while the game was running. The redirect has been shipped and
	// never once confirmed in-game, which is the same failure this project has already paid for --
	// "auth was never reached" and "auth was reached and failed" are indistinguishable without this.
	//
	// It is deliberately READ-ONLY. There is no button here that turns the redirect on, because the
	// switches are read once at boot from the marker files (see DwNet::Init): flipping an atomic
	// mid-session would leave connections already established under the old policy and produce a
	// journal that describes no single coherent boot. Changing modes costs a relaunch, on purpose.
	//
	// THREADING. Everything on this tab is render-thread-safe with no Enqueue: Report() and
	// SaveJournal() take DwNet's own mutex and touch no game state, and the atomics are plain reads.
	// That is the whole reason the report is built as a string on the DwNet side rather than drawn
	// from the live table.
	void DrawDemonwareTab() {
		ImGui::TextWrapped("What the game asked the network for. Every Demonware name resolved and "
			"every address dialled passes through the winsock choke point and lands here.");
		ImGui::Separator();

		// --- switches, and what set them ---------------------------------------------------
		ImGui::TextDisabled("Choke point");

		const bool redirect = Client::Game::DwNet::g_Redirect.load(std::memory_order_relaxed);
		const bool block = Client::Game::DwNet::g_Block.load(std::memory_order_relaxed);
		const bool journal = Client::Game::DwNet::g_Journal.load(std::memory_order_relaxed);

		ImGui::Text("redirect: %s", redirect ? "ON  (demonware.net -> 127.0.0.1)" : "off");
		ImGui::SameLine();
		HelpMarker("Set by the presence of <game>/cw-mod/dwserver/ (and \"backend\": true in cw-mod.json). When on, no Demonware name is "
			"ever handed to the real resolver, so the local server is the only thing the client "
			"can be talking to.");

		ImGui::Text("block:    %s", block ? "ON  (resolves fail closed)" : "off");
		ImGui::SameLine();
		HelpMarker("On for an online boot with NO local server. This is the guardrail: a Demonware "
			"name cannot resolve to a routable address, so the client structurally cannot reach "
			"retail Activision servers. Retail is not a target for this project.");

		ImGui::Text("journal:  %s", journal ? "ON" : "off  (mode offline, no cw-mod/dwserver)");
		ImGui::SameLine();
		HelpMarker("Recording is independent of the other two, so a completely unmodified online "
			"boot can be observed without redirecting anything.");

		// --- the observations --------------------------------------------------------------
		ImGui::Separator();
		ImGui::TextDisabled("Endpoints seen");

		if (ImGui::Button("Save journal")) {
			Client::Game::DwNet::SaveJournal();
		}
		ImGui::SameLine();
		HelpMarker("Appends a counted summary to cw-mod/dw_journal.txt. The live lines in that file "
			"are written as each endpoint is FIRST seen -- so the file survives a boot that dies, "
			"but the repeat counts only exist once this is pressed.\n\n"
			"The count is diagnostic on its own: an endpoint dialled 900 times is a retry loop, "
			"an endpoint dialled once got an answer it accepted.");

		// The port is the thing to read on a connect line. Post-redirect everything shares an
		// address, so 443 vs 3074 is what says whether an endpoint can be served over plain HTTPS or
		// needs the LSG handshake -- which is the question that decides what gets built next.
		ImGui::TextWrapped("Read the PORT on connect lines: 443 is HTTPS (auth3 / objectstore / "
			"umbrella / uno), 3074 is the LSG bdSecureSocket (hardcoded client-side).");

		const std::string report = Client::Game::DwNet::Report();
		ImGui::BeginChild("##dwreport", ImVec2(0.0f, 260.0f), ImGuiChildFlags_Borders,
			ImGuiWindowFlags_HorizontalScrollbar);
		ImGui::TextUnformatted(report.c_str());
		ImGui::EndChild();

		// --- the key patch -----------------------------------------------------------------
		ImGui::Separator();
		ImGui::TextDisabled("Embedded key patch");

		const auto& status = Client::Game::DwBackend::LastStatus();
		ImGui::Text("enabled: %s", Client::Game::DwBackend::g_Enabled ? "yes" : "no");
		ImGui::SameLine();
		HelpMarker("Enabled means our auth public key was actually written over the one baked into "
			"the image, which is the only condition under which replies signed by the local server "
			"verify. Until then the startup path leaves the client offline.");
		ImGui::Text("auth key: %s%s", status.authReplaced ? "replaced" : "not replaced",
			status.authKeyFileFound ? "" : "  (no auth_pub.der in cw-mod/dwserver/)");
		ImGui::Text("lsg key:  %s%s", status.lsgReplaced ? "replaced" : "not replaced",
			status.lsgKeyFileFound ? "" : "  (no lsg_pub.der in cw-mod/dwserver/)");
		if (!status.detail.empty()) {
			ImGui::TextWrapped("%s", status.detail.c_str());
		}

		// --- the login driver --------------------------------------------------------------
		ImGui::Separator();
		ImGui::TextDisabled("Login state");
		if (ImGui::Button("Probe DW login state")) {
			Enqueue([] {
				if (!g_Pointers) return;
				std::string d = g_Pointers->DumpDwLoginState();
				std::lock_guard<std::mutex> lock(g_DwLoginDumpMutex);
				g_DwLoginDump.swap(d);
			});
		}
		ImGui::SameLine();
		HelpMarker("Read-only snapshot of the login driver's preconditions: g_liveUserSystemActive (the "
			"LiveUser loop ticks at all), the nodw dvar (true = the driver refuses to dial), and per "
			"controller the sign-in state (+5764), login state (+5768) and whether the DW login "
			"controller (+24) exists. The (Login) status lines in cw-mod/client.log are the live "
			"transcript; this is for when they stay silent.");
		std::string dump;
		{
			std::lock_guard<std::mutex> lock(g_DwLoginDumpMutex);
			dump = g_DwLoginDump;
		}
		if (!dump.empty()) {
			ImGui::InputTextMultiline("##dwlogindump", dump.data(), dump.size() + 1,
				ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 12), ImGuiInputTextFlags_ReadOnly);
		}
	}
}
