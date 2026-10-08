#include "common.hpp"
#include "overlay/menu.hpp"
#include "overlay/tabs/tabs.hpp"
#include "overlay/d3d12_hook.hpp"   // FrameCount
#include "game/game.hpp"
#include "game/settings.hpp"
#include "game/unlock_all.hpp"
#include "game/zm_progression.hpp"

#include <imgui.h>
#include <shellapi.h>

#include <atomic>
#include <mutex>

namespace Client::Overlay::Menu {
	namespace {
		namespace Zm = Game::ZmProgression;

		// Written by the Enqueue'd read on the game thread, drawn here.
		std::mutex s_XpLock;
		Zm::XpStatus s_Xp;
		std::atomic_bool s_XpReadQueued{ false };
		double s_XpLastRead = -10.0;

		void DrawXpSection() {
			ImGui::Separator();
			ImGui::TextDisabled("ZM progression");

			// Refresh once a second while the tab is open. Reading playerdata is game code, so it
			// runs on the game thread; one read in flight at a time.
			const double now = ImGui::GetTime();
			if (now - s_XpLastRead >= 1.0 && !s_XpReadQueued.exchange(true)) {
				s_XpLastRead = now;
				Enqueue([] {
					const Zm::XpStatus st = Zm::ReadXpStatus();
					{ std::lock_guard lock(s_XpLock); s_Xp = st; }
					s_XpReadQueued = false;
				});
			}
			Zm::XpStatus st;
			{ std::lock_guard lock(s_XpLock); st = s_Xp; }

			if (!st.available) {
				ImGui::TextDisabled("Off (\"progression\": false, or an engine call did not resolve).");
				return;
			}
			if (st.saved >= 0) ImGui::Text("Saved XP     : %d (level %d)", st.saved, st.savedLevel + 1);
			else ImGui::Text("Saved XP     : not loaded yet");
			if (st.match >= 0) ImGui::Text("This match   : %d%s", st.match, st.tainted ? "  (NOT saved: blank start)" : "");

			const int pending = Zm::PendingBonusXp();
			ImGui::Text("Pending bonus: %d", pending);

			static int s_Custom = 5000;
			for (const int amount : { 1000, 10000, 100000 }) {
				if (ImGui::Button(std::format("+{}##xp", amount).c_str())) Zm::QueueBonusXp(amount);
				ImGui::SameLine();
			}
			ImGui::SetNextItemWidth(110.0f);
			ImGui::InputInt("##xpcustom", &s_Custom, 1000, 10000);
			ImGui::SameLine();
			if (ImGui::Button("Add XP")) Zm::QueueBonusXp(s_Custom);
			ImGui::SameLine();
			HelpMarker("Queues XP for you (client 0). It is added to your next XP event in a Zombies match "
				"(kill anything), through the engine's own XP path: multiplier, rank-table cap and the "
				"level-up popup. It is saved with the rest of the match's XP when the match ends. In the "
				"menus it just waits for the next match.");
		}

		void DrawSettingsSection() {
			namespace Settings = Game::Settings;
			ImGui::Separator();
			ImGui::TextDisabled("Settings (cw-mod/cw-mod.json)");
			ImGui::Text("Name         : %s", Settings::PlayerName().c_str());
			ImGui::Text("Mode         : %s", Settings::ModeName(Settings::Get().mode));
			if (const Game::UnlockAll::Status unlock = Game::UnlockAll::ReadStatus(); unlock.enabled) {
				ImGui::Text("Unlock all   : on (%llu of %llu lock checks answered unlocked)",
					static_cast<unsigned long long>(unlock.changed), static_cast<unsigned long long>(unlock.asked));
			}
			else {
				ImGui::TextDisabled("Unlock all   : off (\"unlock_all\" in cw-mod.json)");
			}
			const std::string& note = Settings::LoadNote();
			if (note.starts_with("loaded") || note.starts_with("created")) {
				ImGui::TextDisabled("File         : %s", note.c_str());
			}
			else {
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "File         : %s", note.c_str());
				ImGui::PopTextWrapPos();
			}
			if (ImGui::Button("Open cw-mod.json")) {
				const std::wstring path = Settings::Path().wstring();
				if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
					ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
				}
			}
			ImGui::SameLine();
			HelpMarker("Every cw-mod setting: \"name\", \"mode\" (offline / lan / lanlobby / online) and one "
				"true/false per feature. It is read once at boot, so save it and relaunch the game.");
		}
	}

	void DrawHomeTab() {
		ImGui::TextDisabled("Status");
		ImGui::Text("Game version : %s", g_GameIdentifier.m_Version.c_str());
		ImGui::Text("Module       : %s", g_GameModuleName.c_str());
		ImGui::Text("Frames drawn : %llu", static_cast<unsigned long long>(FrameCount()));

		const bool scrInit = g_Pointers && g_Pointers->m_Scr_Initialized && *g_Pointers->m_Scr_Initialized;
		ImGui::Text("Script system: %s", scrInit ? "initialized" : "not ready");

		DrawSettingsSection();

		ImGui::Separator();
		ImGui::TextDisabled("Sanity check");
		if (ImGui::Button("Send test notification")) {
			Enqueue([] {
				if (g_Pointers && g_Pointers->m_BB_Alert) {
					g_Pointers->m_BB_Alert("cw-mod", "Hello from the cw-mod menu!");
				}
			});
		}
		ImGui::SameLine();
		HelpMarker("Calls the game's BB_Alert on the game thread. Proves menu -> game calls work end to end.");

		DrawXpSection();
	}
}
