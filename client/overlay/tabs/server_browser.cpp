// The LAN server browser: games heard on this network, with one-click join. The discovery itself
// lives in game/lan_browser.cpp; this tab only draws its snapshot and enqueues the join.
#include "common.hpp"
#include "overlay/menu.hpp"
#include "overlay/tabs/tabs.hpp"
#include "game/game.hpp"

#include <imgui.h>

#include <mutex>
#include <string>

namespace Client::Overlay::Menu {
	namespace {
		namespace Lan = Client::Game::LanBrowser;

		// Written by the game thread (the Enqueue'd join), read here on the render thread.
		std::mutex  g_JoinStatusMutex;
		std::string g_JoinStatus;

		// Same defaults and meaning as the Session tab's paste join. jointype must be 1 or 4: a LAN
		// host refuses anything else with verdict 36.
		int g_JoinCtxId = 0;
		int g_ControllerIdx = 0;
		int g_JoinType = 4;

		const ImVec4 kAmber(1.0f, 0.6f, 0.2f, 1.0f);
		const ImVec4 kGreen(0.30f, 0.85f, 0.40f, 1.0f);

		std::string MapLabel(const std::string& map) {
			if (map.empty()) return "-";
			const char* friendly = Lan::FriendlyMapName(map);
			return *friendly ? std::format("{} ({})", friendly, map) : map;
		}

		std::string PlayersLabel(int members, int maxClients) {
			return maxClients > 0 ? std::format("{}/{}", members, maxClients) : std::to_string(members);
		}

		const char* NetworkModeName(int m) {
			return m == 1 ? "LAN" : m == 2 ? "online" : m == 0 ? "offline" : "?";
		}

		void Join(const Lan::Host& h) {
			Enqueue([blob = h.blob, ctx = g_JoinCtxId, pad = g_ControllerIdx, type = g_JoinType] {
				if (!g_Pointers) return;
				std::string r = g_Pointers->JoinHostByDescriptor(blob, ctx, pad, type);
				std::lock_guard lock(g_JoinStatusMutex);
				g_JoinStatus.swap(r);
			});
		}

		void DrawDetails(const Lan::Host& h) {
			const std::uint64_t now = GetTickCount64();
			ImGui::SeparatorText(h.name.c_str());
			ImGui::Text("Address   %s   (heard %llus ago)", h.address.c_str(),
				static_cast<unsigned long long>((now - h.lastSeenMs) / 1000));
			ImGui::Text("Ping      %s", h.pingMs < 0 ? "no answer yet" : std::format("{:.1f} ms", h.pingMs).c_str());
			ImGui::Text("Map       %s", MapLabel(h.map).c_str());
			ImGui::Text("Gametype  %s", h.gametype.empty() ? "-" : h.gametype.c_str());
			ImGui::Text("Mode      %s, %s, %s", Lan::GameModeName(h.gameMode), NetworkModeName(h.networkMode),
				h.inMatch ? "in match" : "in lobby");
			ImGui::Text("Host XUID 0x%016llX   slot %d", static_cast<unsigned long long>(h.xuid), h.slot);

			ImGui::Text("Players   %s", PlayersLabel(h.members, h.maxClients).c_str());
			for (const std::string& p : h.players) {
				ImGui::BulletText("%s", p.c_str());
			}
			if (static_cast<int>(h.players.size()) < h.members) {
				ImGui::TextDisabled("  (%d more not listed)", h.members - static_cast<int>(h.players.size()));
			}

			const bool buildDiffers = !h.build.empty() && h.build != g_GameIdentifier.m_Version;
			const bool modDiffers = !h.mod.empty() && h.mod != GIT_DESCRIBE;
			ImGui::Text("Game      %s", h.build.empty() ? "?" : h.build.c_str());
			if (buildDiffers) {
				ImGui::SameLine();
				ImGui::TextColored(kAmber, "(yours: %s, different build)", g_GameIdentifier.m_Version.c_str());
			}
			ImGui::Text("cw-mod    %s", h.mod.empty() ? "?" : h.mod.c_str());
			if (modDiffers) {
				ImGui::SameLine();
				ImGui::TextColored(kAmber, "(yours: %s)", GIT_DESCRIBE);
			}

			if (ImGui::Button("Join this game")) {
				Join(h);
			}
		}
	}

	void DrawServerBrowserTab() {
		const Lan::Status st = Lan::GetStatus();

		bool advertise = Lan::g_Advertise.load();
		if (ImGui::Checkbox("Advertise my lobby", &advertise)) {
			Lan::g_Advertise.store(advertise);
		}
		ImGui::SameLine();
		bool listen = Lan::g_Listen.load();
		if (ImGui::Checkbox("Listen", &listen)) {
			Lan::g_Listen.store(listen);
		}
		ImGui::SameLine();
		HelpMarker("The game's own LAN discovery is compiled out of the retail build (the ServerlistInfo "
			"message handler is a stub), so cw-mod carries it. Every PC with a live lobby broadcasts its "
			"join descriptor plus map, mode and player list on UDP port 28970 once a second; every PC lists "
			"what it hears. Join is the same join as pasting a CWJOIN1 blob in the Session tab.\n\n"
			"Empty list while the host's 'sent' count climbs? The other PC's firewall is dropping UDP 28970 "
			"inbound. Rows disappear 5 s after their host goes quiet.");

		if (!st.socketUp) {
			ImGui::TextColored(kAmber, "LAN socket down: %s",
				st.socketError.empty() ? "not started yet (starts on the first frame)" : st.socketError.c_str());
		} else {
			ImGui::TextDisabled("udp/%u   %d broadcast targets   sent %llu   heard %llu   rejected %llu",
				static_cast<unsigned>(Lan::kPort), st.broadcastTargets,
				static_cast<unsigned long long>(st.sent), static_cast<unsigned long long>(st.received),
				static_cast<unsigned long long>(st.rejected));
		}

		if (st.advertising) {
			const Lan::Advert& a = st.advert;
			ImGui::TextColored(kGreen, "Advertising");
			ImGui::SameLine();
			ImGui::Text("'%s'   %s   %s   %s players   %s", a.name.c_str(), MapLabel(a.map).c_str(),
				a.gametype.empty() ? "-" : a.gametype.c_str(), PlayersLabel(a.members, a.maxClients).c_str(),
				a.inMatch ? "in match" : "in lobby");
		} else {
			ImGui::TextDisabled("Not advertising: %s", advertise ? "no live lobby on this PC" : "turned off");
		}

		ImGui::Separator();

		static std::uint64_t s_Selected = 0;
		const std::vector<Lan::Host> hosts = Lan::Hosts();
		const Lan::Host* selected = nullptr;

		if (hosts.empty()) {
			ImGui::TextDisabled("No LAN games heard yet.");
		} else if (ImGui::BeginTable("##servers", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
				| ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
				ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 9))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Host");
			ImGui::TableSetupColumn("Map");
			ImGui::TableSetupColumn("Mode");
			ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();

			for (const Lan::Host& h : hosts) {
				ImGui::PushID(static_cast<int>(h.xuid ^ (h.xuid >> 32)));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (ImGui::Selectable(h.name.c_str(), s_Selected == h.xuid, ImGuiSelectableFlags_SpanAllColumns
						| ImGuiSelectableFlags_AllowOverlap)) {
					s_Selected = h.xuid;
				}
				ImGui::TableNextColumn();
				const char* friendly = Lan::FriendlyMapName(h.map);
				ImGui::TextUnformatted(*friendly ? friendly : (h.map.empty() ? "-" : h.map.c_str()));
				ImGui::TableNextColumn();
				ImGui::Text("%s%s%s", Lan::GameModeName(h.gameMode), h.gametype.empty() ? "" : " / ",
					h.gametype.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(PlayersLabel(h.members, h.maxClients).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(h.inMatch ? "in match" : "lobby");
				ImGui::TableNextColumn();
				if (h.pingMs < 0) ImGui::TextDisabled("-");
				else ImGui::Text("%.0f ms", h.pingMs);
				ImGui::TableNextColumn();
				if (ImGui::SmallButton("Join")) {
					Join(h);
				}
				ImGui::PopID();

				if (h.xuid == s_Selected) {
					selected = &h;
				}
			}
			ImGui::EndTable();
		}

		if (selected) {
			DrawDetails(*selected);
		} else if (!hosts.empty()) {
			ImGui::TextDisabled("Click a row for details.");
		}

		{
			std::lock_guard lock(g_JoinStatusMutex);
			if (!g_JoinStatus.empty()) {
				ImGui::Separator();
				ImGui::TextWrapped("%s", g_JoinStatus.c_str());
			}
		}

		ImGui::Spacing();
		if (ImGui::TreeNode("Join options")) {
			ImGui::SetNextItemWidth(90.0f);
			ImGui::InputInt("ctx", &g_JoinCtxId);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(90.0f);
			ImGui::InputInt("pad", &g_ControllerIdx);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(90.0f);
			ImGui::InputInt("jointype", &g_JoinType);
			ImGui::SameLine();
			HelpMarker("join-context id, controller index, and jointype, as in the Session tab. jointype must "
				"be 1 or 4; a LAN host refuses anything else with verdict 36. Leave these at 0 / 0 / 4.");
			ImGui::TreePop();
		}
	}
}
