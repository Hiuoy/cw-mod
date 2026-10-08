#include "common.hpp"
#include "overlay/menu.hpp"
#include "overlay/tabs/tabs.hpp"
#include "game/mapkit_loader.hpp"
#include "game/mapkit_live.hpp"
#include "game/mapkit_usage.hpp"

#include <imgui.h>

#include <array>
#include <cstdio>
#include <string>
#include <vector>

// Custom maps (mapkit): what cw-mod/maps holds, whether each map's zone set is complete, and the
// map-start redirect, and the live-state dump (plan P0). Everything but the dump is cw-mod's own state; the
// dump reads the game's memory, so it runs on the game thread through Enqueue.
namespace Client::Overlay::Menu {
	namespace {
		namespace Mk = Game::MapKit;

		// The four ZM maps a lobby can start, internal names.
		constexpr std::array<const char*, 4> kLobbyMaps = { "zm_silver", "zm_gold", "zm_platinum", "zm_tungsten" };
		constexpr std::array<const char*, 4> kLobbyMapNames = { "Die Maschine", "Firebase Z", "Mauer der Toten", "Forsaken" };

		int s_From = 0;
		int s_To = 0; // 0 = off, then the folders in order

		void DrawRedirect(const std::vector<Mk::MapFolder>& folders) {
			ImGui::Separator();
			ImGui::TextDisabled("Start a custom map");

			std::vector<std::string> targets = { "off (start the map itself)" };
			for (const Mk::MapFolder& folder : folders) {
				if (!(folder.listed && folder.overlay)) targets.push_back(folder.name); // an overlay has no zone to start
			}
			if (s_To >= static_cast<int>(targets.size())) s_To = 0;

			ImGui::SetNextItemWidth(170.0f);
			if (ImGui::BeginCombo("When the lobby starts", kLobbyMapNames[s_From])) {
				for (int i = 0; i < static_cast<int>(kLobbyMaps.size()); ++i) {
					if (ImGui::Selectable(kLobbyMapNames[i], i == s_From)) s_From = i;
				}
				ImGui::EndCombo();
			}
			ImGui::SetNextItemWidth(170.0f);
			if (ImGui::BeginCombo("start instead", targets[s_To].c_str())) {
				for (int i = 0; i < static_cast<int>(targets.size()); ++i) {
					if (ImGui::Selectable(targets[i].c_str(), i == s_To)) s_To = i;
				}
				ImGui::EndCombo();
			}
			if (ImGui::Button("Apply")) {
				Mk::SetRedirect(kLobbyMaps[s_From], s_To == 0 ? std::string() : targets[s_To]);
			}
			ImGui::SameLine();
			HelpMarker("Apply this BEFORE opening the Zombies lobby: the lobby loads the map as soon as it "
				"opens, and the custom map has to be loaded in its place. Then pick the map as usual and press "
				"Start. Applied too late, the retail map starts instead (the log says why). The redirect lasts "
				"until you change it or restart the game. A map whose zone set is incomplete is refused, "
				"because a missing zone ends the match load with an error.");

			const std::string from = Mk::RedirectFrom();
			const std::string to = Mk::RedirectTo();
			if (to.empty()) ImGui::TextDisabled("Redirect: off");
			else ImGui::Text("Redirect: %s -> %s", from.c_str(), to.c_str());
		}

		std::array<char, 256> s_Ours{};
		std::array<char, 256> s_Retail{};

		void DrawLiveDump() {
			ImGui::Separator();
			ImGui::TextDisabled("Live-state dump (compare our model with a retail one, in memory)");
			if (!s_Ours[0] && !s_Retail[0]) {
				const auto [ours, retail] = Game::MapKit::Live::Models();
				std::snprintf(s_Ours.data(), s_Ours.size(), "%s", ours.c_str());
				std::snprintf(s_Retail.data(), s_Retail.size(), "%s", retail.c_str());
			}
			ImGui::SetNextItemWidth(420.0f);
			ImGui::InputText("our xmodels", s_Ours.data(), s_Ours.size());
			ImGui::SetNextItemWidth(420.0f);
			ImGui::InputText("retail xmodels", s_Retail.data(), s_Retail.size());
			if (ImGui::Button("Dump live state")) {
				Game::MapKit::Live::SetModels(s_Ours.data(), s_Retail.data());
				Enqueue([] { Game::MapKit::Live::Dump("manual"); });
			}
			ImGui::SameLine();
			HelpMarker("Writes the live bytes of the models (xmodel, LODs, mesh info, surfaces, materials) and of the "
				"world roots to cw-mod/mapkit/live/<time>_manual/, and logs every field where ours differs from the "
				"retail one ('live diff' lines in client.log). Names or name hashes (0x...), comma-separated: our n-th is "
				"compared with the retail n-th, or with the last retail one. It also runs by itself "
				"at the start of a mapkit world and 20 s later. Press it while looking at the spot where our model "
				"should be.");
			const std::string last = Game::MapKit::Live::LastDump();
			if (!last.empty()) ImGui::TextWrapped("Last dump: %s", last.c_str());
		}

		std::string s_UsageNote;

		void DrawUsage() {
			if (!Game::MapKitUsage::Enabled()) return;
			ImGui::Separator();
			ImGui::TextDisabled("Asset usage census (plan P6: what a map still takes from its asset library)");
			if (ImGui::Button("Write asset usage now")) s_UsageNote = Game::MapKitUsage::WriteNow();
			ImGui::SameLine();
			HelpMarker("Every asset lookup and bgcache lookup since boot, plus every loaded asset and its zone, go to "
				"cw-mod/mapkit/usage/<map>_<time>.mkuse. It is written by itself 45 s after a level's world starts and "
				"every 2 minutes after that while new lookups come in; press this to write it now. Read it with "
				"ffinfo zm_silver --trace <zm_silver.mktrace> --needs --usage <file>. Turn \"mapkit_usage\" off "
				"in cw-mod.json when the count is done.");
			if (!s_UsageNote.empty()) ImGui::TextWrapped("%s", s_UsageNote.c_str());
			const std::string status = Game::MapKitUsage::LastStatus();
			if (!status.empty()) ImGui::TextWrapped("Last write: %s", status.c_str());
		}
	}

	void DrawMapsTab() {
		if (!Mk::Enabled()) {
			ImGui::TextWrapped("Custom maps are off: there is no map in cw-mod/maps, or \"custom_maps\" is false "
				"in cw-mod.json. Put each map in its own folder, cw-mod/maps/<map>/, and restart the game.");
			return;
		}

		const std::vector<Mk::MapFolder> folders = Mk::Folders();
		ImGui::Text("cw-mod/maps: %zu map%s, %s", folders.size(), folders.size() == 1 ? "" : "s",
			Mk::Mounted() ? "mounted" : "not mounted yet (mounts at the first menu frame)");

		if (ImGui::BeginTable("##maps", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Map");
			ImGui::TableSetupColumn("Zones");
			ImGui::TableSetupColumn("Status");
			ImGui::TableHeadersRow();
			for (const Mk::MapFolder& folder : folders) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(folder.name.c_str());
				if (folder.replacesRetail) ImGui::TextDisabled("replaces retail, every match");
				if (folder.listed) {
					ImGui::TextDisabled("'%s', %s of %s", folder.title.c_str(), folder.overlay ? "overlay" : "clone",
						folder.base.c_str());
				}
				ImGui::TableNextColumn();
				for (const std::string& zone : folder.zones) ImGui::TextUnformatted(zone.c_str());
				ImGui::TableNextColumn();
				if (!Mk::Mounted()) {
					ImGui::TextDisabled("-");
				}
				else if (folder.missing.empty()) {
					ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "complete");
				}
				else {
					ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "missing:");
					for (const std::string& zone : folder.missing) ImGui::TextUnformatted(zone.c_str());
				}
			}
			ImGui::EndTable();
		}

		const std::string picked = Mk::PickedMap();
		const std::string serving = Mk::ActiveMap();
		ImGui::Text("CUSTOM MAPS pick (Zombies > Private): %s", picked.empty() ? "none, every map starts stock" : picked.c_str());
		ImGui::SameLine();
		HelpMarker("A map with a map.json (what cwlink build writes) is listed in the CUSTOM MAPS tab of Zombies "
			"Private. A map of its own becomes the lobby's map when picked there, and every PC in the lobby loads it by "
			"name; an overlay is live only on the PC that picked it. Picking a stock map, or leaving the pick, starts the "
			"stock map. A folder without a map.json is live for every match.");
		ImGui::Text("Level scripts served from: %s", serving.empty() ? "no custom map" : ("cw-mod/maps/" + serving).c_str());

		DrawRedirect(folders);
		DrawLiveDump();
		DrawUsage();

		ImGui::Separator();
		ImGui::TextDisabled("Events");
		const std::vector<std::string> events = Mk::RecentEvents();
		if (ImGui::BeginChild("##mapkit_events", ImVec2(0, 140), ImGuiChildFlags_Borders)) {
			for (const std::string& line : events) ImGui::TextWrapped("%s", line.c_str());
		}
		ImGui::EndChild();
	}
}
