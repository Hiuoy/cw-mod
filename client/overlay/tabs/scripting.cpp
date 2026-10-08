#include "common.hpp"
#include "overlay/menu.hpp"
#include "overlay/tabs/tabs.hpp"

#include <imgui.h>

#include "scripting/scripting.hpp"

#include <mutex>
#include <string>

namespace Client::Overlay::Menu {
	// The GSC loader: what is on disk, how each script will be used, and whether the engine took it.
	// Everything that changes state is Enqueue()d to the game thread; the tables are read-only views.
	namespace {
		namespace S = Client::Scripting;

		const ImVec4 kGreen(0.30f, 0.85f, 0.40f, 1.0f);
		const ImVec4 kRed(0.95f, 0.35f, 0.35f, 1.0f);
		const ImVec4 kAmber(1.0f, 0.6f, 0.2f, 1.0f);
		const ImVec4 kBlue(0.45f, 0.70f, 1.0f, 1.0f);

		std::mutex g_StatusMutex;
		std::string g_Status;

		void SetStatus(std::string s) {
			std::lock_guard<std::mutex> lock(g_StatusMutex);
			g_Status.swap(s);
		}

		void DrawScriptTable() {
			const auto scripts = S::Scripts();
			if (scripts.empty()) {
				ImGui::TextDisabled("No .gscc files in the folder.");
				return;
			}
			if (!ImGui::BeginTable("##scripts", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
					ImGuiTableFlags_SizingStretchProp)) {
				return;
			}
			ImGui::TableSetupColumn("File");
			ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed, 60.0f);
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 140.0f);
			ImGui::TableSetupColumn("Strings", ImGuiTableColumnFlags_WidthFixed, 55.0f);
			ImGui::TableSetupColumn("Served", ImGuiTableColumnFlags_WidthFixed, 50.0f);
			ImGui::TableHeadersRow();
			for (const auto& s : scripts) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextColored(s.valid ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : kRed, "%s", s.file.c_str());
				if (!s.valid && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.error.c_str());

				ImGui::TableSetColumnIndex(1);
				if (!s.valid) ImGui::TextColored(kRed, "error");
				else if (s.mode == S::Mode::Inject) ImGui::TextColored(kGreen, "inject");
				else if (s.mode == S::Mode::Replace) ImGui::TextColored(kBlue, "replace");
				else ImGui::TextDisabled("next match");

				ImGui::TableSetColumnIndex(2);
				if (s.valid) {
					ImGui::Text("0x%016llX", static_cast<unsigned long long>(s.name));
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("%s", s.source == S::NameSource::FileHash ? "Taken from the file name."
							: s.source == S::NameSource::FromPath ? "Made from the file path (compiled without --name)."
							: "Compiled into the script (--name).");
					}
				} else {
					ImGui::TextDisabled("-");
				}

				ImGui::TableSetColumnIndex(3);
				if (!s.strings) ImGui::TextDisabled("0");
				else ImGui::TextColored(s.stringsReady ? kGreen : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
					"%u", s.strings);

				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%llu", static_cast<unsigned long long>(s.served));
			}
			ImGui::EndTable();
		}

		void DrawDiagnostics() {
			const auto& sigs = S::Signatures();
			if (ImGui::BeginTable("##sigs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("Signature");
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 40.0f);
				ImGui::TableSetupColumn("RVA", ImGuiTableColumnFlags_WidthFixed, 100.0f);
				for (const auto& s : sigs) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(s.name.c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::TextColored(s.resolved ? kGreen : kRed, s.resolved ? "OK" : "MISS");
					ImGui::TableSetColumnIndex(2);
					if (s.resolved) ImGui::Text("+0x%llX", static_cast<unsigned long long>(s.rva));
					else ImGui::TextDisabled("-");
				}
				ImGui::EndTable();
			}

			ImGui::Text("LazyLink handler: %s   calls %llu   unresolved %llu",
				S::LazyLinkInstalled() ? "installed" : "stock (empty stub)",
				static_cast<unsigned long long>(S::LazyLinkCallCount()),
				static_cast<unsigned long long>(S::LazyLinkMisses()));
			ImGui::SameLine();
			HelpMarker("Opcode 0x13 resolves &namespace::function references at run time. The game's own "
				"handler is an empty stub, so ours is patched in when one of our scripts is first served. 'Unresolved' "
				"counts references that found no linked function and pushed undefined.");

			ImGui::Separator();
			bool capture = S::CaptureEnabled();
			if (ImGui::Checkbox("Record script requests", &capture)) S::SetCapture(capture);
			ImGui::SameLine();
			if (ImGui::SmallButton("Clear")) S::ClearCapture();
			ImGui::SameLine();
			HelpMarker("Records every script name the engine asks for. Those hashes are what a REPLACE "
				"script can target (the engine has hashes, not names). `acts lookup <hash>` names most "
				"of them.");
			const auto names = S::CapturedNames();
			if (!names.empty() && ImGui::BeginTable("##cap", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
					ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 150.0f))) {
				ImGui::TableSetupColumn("Name hash");
				ImGui::TableSetupColumn("Requests", ImGuiTableColumnFlags_WidthFixed, 65.0f);
				ImGui::TableSetupColumn("Ours", ImGuiTableColumnFlags_WidthFixed, 40.0f);
				ImGui::TableHeadersRow();
				for (const auto& n : names) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::Text("0x%016llX", static_cast<unsigned long long>(n.nameHash));
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%llu", static_cast<unsigned long long>(n.count));
					ImGui::TableSetColumnIndex(2);
					if (n.ours) ImGui::TextColored(kGreen, "yes");
				}
				ImGui::EndTable();
			}

			ImGui::Separator();
			if (ImGui::Button("Dump decrypted module")) SetStatus(S::DumpDecryptedModule());
			ImGui::SameLine();
			HelpMarker("Writes the decrypted game image to bocw_dump.bin in the game folder, for "
				"tools/dump_fixup.py and IDA. Do it from the main menu.");
		}
	}

	void DrawScriptingTab() {
		const bool active = S::LoaderActive();
		ImGui::TextColored(active ? kGreen : kAmber, active ? "Loader on" : "Loader off");
		ImGui::SameLine();
		ImGui::TextDisabled("- every .gscc in the folder is used in the next match, whatever its file name.");

		static std::string s_Folder;
		if (s_Folder.empty()) s_Folder = S::ScriptsFolder();
		ImGui::TextDisabled("%s", s_Folder.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Copy path")) ImGui::SetClipboardText(s_Folder.c_str());

		if (ImGui::Button("Reload folder")) {
			Menu::Enqueue([] { SetStatus(S::ReloadScripts()); });
		}
		ImGui::SameLine();
		if (ImGui::Button(active ? "Turn loader off" : "Turn loader on")) {
			Menu::Enqueue([active] { SetStatus(active ? S::DisableLoader() : S::EnableLoader()); });
		}
		ImGui::SameLine();
		HelpMarker("Reload re-reads the folder. In a match it is staged and applies to the next one, so a "
			"match never mixes old and new copies. Turning the loader off also waits for you to leave "
			"the match. \"scripts\": false in cw-mod.json keeps it off at boot.");
		if (S::ReloadPending()) ImGui::TextColored(kAmber, "Reload staged: applies when this match ends.");
		{
			std::lock_guard<std::mutex> lock(g_StatusMutex);
			if (!g_Status.empty()) ImGui::TextWrapped("%s", g_Status.c_str());
		}

		ImGui::Separator();
		DrawScriptTable();

		for (const auto& h : S::Hosts()) {
			if (h.skippedDuplicate) {
				ImGui::TextColored(kRed, "Host %s was linked outside the loader; injection was skipped.", h.path.c_str());
			} else {
				ImGui::TextDisabled("Injected through %s: %u script(s), handed out %llu time(s).", h.path.c_str(),
					h.injected, static_cast<unsigned long long>(h.served));
			}
		}

		if (ImGui::CollapsingHeader("Adding a script")) {
			ImGui::TextWrapped("Compile with ACTS for Cold War, then copy the .gscc into the folder above:");
			ImGui::TextUnformatted("  acts gscc -g cw -o mytrainer <folder with your .gsc>");
			ImGui::TextWrapped("inject: a new script. It is added to every ZM match; its autoexec functions run "
				"like the game's own. Use system::register / callback::on_spawned the way stock scripts do.");
			ImGui::TextWrapped("replace: compile with --name scripts/zm_common/foo.gsc (the .gsc is part of the "
				"name), or name the file 0x<16 hex digits>.gscc after a hash from 'Record script requests'. "
				"The engine gets yours instead of its own, so it must provide everything the original did.");
			ImGui::TextWrapped("String literals work: they are interned and patched in before the engine first "
				"sees the script. Server scripts (.gsc) only.");
		}

		if (ImGui::CollapsingHeader("Diagnostics")) {
			DrawDiagnostics();
		}
	}
}
