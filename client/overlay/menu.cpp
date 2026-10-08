#include "common.hpp"
#include "overlay/menu.hpp"
#include "overlay/tabs/tabs.hpp"

#include <imgui.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <vector>

namespace Client::Overlay::Menu {
	// --- game-thread action queue ---
	static std::mutex g_ActionMutex;
	static std::vector<std::function<void()>> g_Actions;

	// --- script messages (ScriptPrint_Mirror.cpp) ---
	using Clock = std::chrono::steady_clock;
	constexpr auto kScriptMessageTime = std::chrono::seconds(8);
	constexpr std::size_t kMaxScriptMessages = 6;
	struct ScriptMessage {
		std::string text;
		Clock::time_point shown;
	};
	static std::mutex g_MessageMutex;
	static std::deque<ScriptMessage> g_Messages;

	void PushScriptMessage(std::string text) {
		std::lock_guard<std::mutex> lock(g_MessageMutex);
		g_Messages.push_back({ std::move(text), Clock::now() });
		while (g_Messages.size() > kMaxScriptMessages) {
			g_Messages.pop_front();
		}
	}

	void RenderScriptMessages() {
		std::vector<std::string> lines;
		{
			std::lock_guard<std::mutex> lock(g_MessageMutex);
			const auto now = Clock::now();
			while (!g_Messages.empty() && now - g_Messages.front().shown > kScriptMessageTime) {
				g_Messages.pop_front();
			}
			for (const ScriptMessage& message : g_Messages) {
				lines.push_back(message.text);
			}
		}
		if (lines.empty()) {
			return;
		}
		// Top centre, under the game's own bold-print area; never takes input or focus.
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + 80.0f),
			ImGuiCond_Always, ImVec2(0.5f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.55f);
		constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
			| ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
			| ImGuiWindowFlags_NoSavedSettings;
		if (ImGui::Begin("##cwmod_script_messages", nullptr, kFlags)) {
			for (const std::string& line : lines) {
				ImGui::TextUnformatted(line.c_str());
			}
		}
		ImGui::End();
	}

	void Enqueue(std::function<void()> action) {
		std::lock_guard<std::mutex> lock(g_ActionMutex);
		g_Actions.emplace_back(std::move(action));
	}

	void DrainActions() {
		std::vector<std::function<void()>> pending;
		{
			std::lock_guard<std::mutex> lock(g_ActionMutex);
			pending.swap(g_Actions);
		}
		for (auto& action : pending) {
			action();
		}
	}

	void Render() {
		ImGui::SetNextWindowSize(ImVec2(440, 340), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("cw-mod", nullptr, ImGuiWindowFlags_NoCollapse)) {
			ImGui::TextColored(ImVec4(0.36f, 0.72f, 1.0f, 1.0f), "cw-mod");
			ImGui::SameLine();
			ImGui::TextDisabled("Black Ops Cold War Zombies - community revival");
			ImGui::Separator();

			if (ImGui::BeginTabBar("##cwmod_tabs")) {
				if (ImGui::BeginTabItem("Home")) { DrawHomeTab(); ImGui::EndTabItem(); }
				if (ImGui::BeginTabItem("Session")) { DrawSessionTab(); ImGui::EndTabItem(); }
				if (ImGui::BeginTabItem("Server Browser")) { DrawServerBrowserTab(); ImGui::EndTabItem(); }
				if (ImGui::BeginTabItem("Scripts")) { DrawScriptingTab(); ImGui::EndTabItem(); }
				if (ImGui::BeginTabItem("Demonware")) { DrawDemonwareTab(); ImGui::EndTabItem(); }
				if (ImGui::BeginTabItem("LUI Menus")) { DrawLuiMenusTab(); ImGui::EndTabItem(); }
				if (ImGui::BeginTabItem("Maps")) { DrawMapsTab(); ImGui::EndTabItem(); }
				if (ImGui::BeginTabItem("Debug")) { DrawDebugTab(); ImGui::EndTabItem(); }
				ImGui::EndTabBar();
			}

			ImGui::Separator();
			ImGui::TextDisabled("INSERT to toggle");
		}
		ImGui::End();
	}
}
