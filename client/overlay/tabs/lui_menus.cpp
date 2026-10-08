#include "common.hpp"
#include "overlay/menu.hpp"
#include "overlay/tabs/tabs.hpp"
#include "game/game.hpp"
#include "scripting/scripting.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Client::Overlay::Menu {
	// Open any LUI menu by name or hash (Pointers::OpenLuiMenu), and list the ones that exist.
	//
	// The list is the live registry (_G.LUI.createMenu, read on the game thread) joined with
	// cw-mod/lui_menus.txt, which tools/lui_menu_names.py writes from the decompiled Lua dump. The
	// dump names only some of them and prints hashes masked to 60 bits, so the join is on the low 60
	// bits. A registry menu with no recovered name is still listed and opens by its full hash.
	namespace {
		constexpr std::uint64_t kMask60 = 0x0FFFFFFFFFFFFFFFULL;

		struct Row {
			std::uint64_t hash{};     // 63-bit for registry rows; 60-bit for dump-only unnamed rows
			std::string   name;       // "" = not recovered
			std::string   file;       // decompiled file the builder lives in, if known
			bool          registered{};
		};

		// Written on the game thread, drawn on the render thread.
		std::mutex g_Mutex;
		std::vector<Row> g_Rows;
		std::string g_Status = "Press 'Refresh list' (from the main menu) to read the menus this UI state can open.";
		std::string g_ListInfo;

		void SetStatus(std::string s) {
			std::lock_guard<std::mutex> lock(g_Mutex);
			g_Status.swap(s);
		}

		struct DumpEntry { std::string name; std::string file; };

		// cw-mod/lui_menus.txt: "0x<60-bit hash>  <name or ->  <file>" per line, '#' comments.
		std::unordered_map<std::uint64_t, DumpEntry> LoadDumpNames(std::string& note) {
			std::unordered_map<std::uint64_t, DumpEntry> out;
			std::error_code ec;
			const auto path = std::filesystem::current_path(ec) / "cw-mod" / "lui_menus.txt";
			std::ifstream f(path);
			if (!f) {
				note = "no cw-mod/lui_menus.txt (run tools/lui_menu_names.py) - names unavailable";
				return out;
			}
			std::string line;
			while (std::getline(f, line)) {
				if (line.empty() || line[0] == '#' || line.rfind("0x", 0) != 0) continue;
				char* end = nullptr;
				const std::uint64_t h = std::strtoull(line.c_str() + 2, &end, 16) & kMask60;
				std::string rest = end ? end : "";
				auto next = [&rest]() {
					const std::size_t a = rest.find_first_not_of(" \t");
					if (a == std::string::npos) { rest.clear(); return std::string{}; }
					const std::size_t b = rest.find_first_of(" \t", a);
					std::string tok = rest.substr(a, b == std::string::npos ? std::string::npos : b - a);
					rest = b == std::string::npos ? std::string{} : rest.substr(b);
					return tok;
				};
				DumpEntry e;
				e.name = next();
				if (e.name == "-") e.name.clear();
				e.file = next();
				out.emplace(h, std::move(e));
			}
			note = std::format("{} menus named from cw-mod/lui_menus.txt", out.size());
			return out;
		}

		// Game thread.
		void RefreshList() {
			if (!g_Pointers) return;
			std::vector<std::uint64_t> registry;
			const std::string err = g_Pointers->LuaCollectMenuHashes(registry);

			std::string note;
			auto dump = LoadDumpNames(note);

			std::vector<Row> rows;
			rows.reserve(registry.size() + dump.size());
			for (const std::uint64_t h : registry) {
				Row r{ h, {}, {}, true };
				if (const auto it = dump.find(h & kMask60); it != dump.end()) {
					r.name = it->second.name;
					r.file = it->second.file;
					dump.erase(it);
				}
				rows.push_back(std::move(r));
			}
			// What is left in the dump is not buildable from this UI state (an in-game menu seen
			// from the frontend, or the other way round). Listed so it can still be tried.
			for (auto& [h60, e] : dump) {
				Row r{ h60, e.name, e.file, false };
				if (!r.name.empty()) r.hash = Client::Game::Pointers::HashString(r.name.c_str());
				rows.push_back(std::move(r));
			}
			std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
				if (a.registered != b.registered) return a.registered;
				if (a.name.empty() != b.name.empty()) return !a.name.empty();
				return _stricmp(a.name.c_str(), b.name.c_str()) < 0 || (a.name == b.name && a.hash < b.hash);
			});

			const std::size_t live = registry.size();
			const std::size_t named = static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(),
				[](const Row& r) { return r.registered && !r.name.empty(); }));

			std::lock_guard<std::mutex> lock(g_Mutex);
			g_Rows.swap(rows);
			g_ListInfo = err.empty()
				? std::format("{} menus registered here ({} named), {} more only in the dump. {}.",
					live, named, g_Rows.size() - live, note)
				: std::format("Registry unreadable: {} Showing the dump list only. {}.", err, note);
		}

		// How a menu is opened. OpenOverlay is how the game's own buttons do it and can pass the params
		// table menus read (ZMUpgrades needs _sessionMode); addmenu passes the controller only.
		enum OpenVia : int { kViaOverlayZombies, kViaOverlayMultiplayer, kViaOverlayNoParams, kViaAddMenu };
		constexpr const char* kViaNames[] = {
			"OpenOverlay  { _sessionMode = Zombies }",
			"OpenOverlay  { _sessionMode = Multiplayer }",
			"OpenOverlay  { }",
			"addmenu  (controller only)",
		};

		void QueueOpen(std::string what, bool force, int via) {
			Enqueue([what = std::move(what), force, via] {
				if (!g_Pointers) return;
				switch (via) {
				case kViaOverlayZombies:     SetStatus(g_Pointers->OpenLuiOverlay(what.c_str(), 0)); break;
				case kViaOverlayMultiplayer: SetStatus(g_Pointers->OpenLuiOverlay(what.c_str(), 1)); break;
				case kViaOverlayNoParams:    SetStatus(g_Pointers->OpenLuiOverlay(what.c_str(), -1)); break;
				default:                     SetStatus(g_Pointers->OpenLuiMenu(what.c_str(), force)); break;
				}
			});
		}

		bool Matches(const Row& r, const std::string& filter) {
			if (filter.empty()) return true;
			auto lower = [](std::string s) {
				for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				return s;
			};
			const std::string f = lower(filter);
			return lower(r.name).find(f) != std::string::npos
				|| lower(std::format("{:016x}", r.hash)).find(f) != std::string::npos
				|| lower(r.file).find(f) != std::string::npos;
		}
	}

	void DrawLuiMenusTab() {
		ImGui::TextWrapped("Open any LUI menu the way the engine's 'openmenu' command does. Type a name "
			"(ZMUpgrades - case does not matter) or a hash (0x...).");

		static char s_Input[128] = "ZMUpgrades";
		static bool s_Force = false;
		static int s_Via = kViaOverlayZombies;
		ImGui::SetNextItemWidth(260.0f);
		const bool enter = ImGui::InputText("##luimenu", s_Input, sizeof(s_Input),
			ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if (ImGui::Button("Open") || enter) QueueOpen(s_Input, s_Force, s_Via);
		ImGui::SetNextItemWidth(300.0f);
		ImGui::Combo("open via", &s_Via, kViaNames, IM_ARRAYSIZE(kViaNames));
		ImGui::SameLine();
		HelpMarker("OpenOverlay = CoD.BaseUtility.OpenOverlay(menu on top, name, controller, params), exactly "
			"how the game's buttons open menus, from the menu currently on top. Menus that read their session "
			"mode (ZMUpgrades and the other loadout/upgrade screens) need _sessionMode; use Zombies for ZM "
			"screens. The menu must be registered in this UI state.\n\n"
			"addmenu = the console 'openmenu' path: controller only, no params. 'force' (addmenu only) "
			"sends a hash that is not registered here, e.g. 'lobby'.\n\n"
			"A Lua error would normally make the engine quit. Once this tab has opened anything, every LUI "
			"fatal is suppressed for the rest of the session (hand-opened menus often raise later, on an "
			"input change) and logged to client.log. Back out with Escape / the game's back button.");
		if (s_Via == kViaAddMenu) {
			ImGui::SameLine();
			ImGui::Checkbox("force", &s_Force);
		}

		std::string status, info;
		std::vector<Row> rows;
		{
			std::lock_guard<std::mutex> lock(g_Mutex);
			status = g_Status;
			info = g_ListInfo;
			rows = g_Rows;
		}
		ImGui::TextWrapped("%s", status.c_str());
		if (const int late = Client::Game::LuiMenus::g_LateSuppressed.load(std::memory_order_relaxed)) {
			ImGui::TextDisabled("%d later LUI error(s) suppressed this session - see client.log.", late);
		}

		if (ImGui::Button("Dump loaded Lua files")) Enqueue([] { SetStatus(Client::Scripting::DumpLuaFiles()); });
		ImGui::SameLine();
		HelpMarker("Writes every LUI chunk loaded right now to cw-mod/lua_dump/<hash>.luac (raw bytecode for "
			"CoDLuaDecompiler). Files already there are kept, so pressing it again in another screen only adds "
			"what that screen loaded.");

		ImGui::Separator();
		if (ImGui::Button("Refresh list")) Enqueue(RefreshList);
		ImGui::SameLine();
		static char s_Filter[64] = "";
		ImGui::SetNextItemWidth(200.0f);
		ImGui::InputTextWithHint("##luifilter", "filter (name, hash, file)", s_Filter, sizeof(s_Filter));
		ImGui::SameLine();
		static bool s_OnlyHere = true;
		ImGui::Checkbox("registered here only", &s_OnlyHere);
		if (!info.empty()) ImGui::TextDisabled("%s", info.c_str());

		const std::string filter = s_Filter;
		if (ImGui::BeginTable("##luimenus", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
			| ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, ImVec2(0.0f, 0.0f))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 44.0f);
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			ImGui::TableSetupColumn("Lua file", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			for (std::size_t i = 0; i < rows.size(); ++i) {
				const Row& r = rows[i];
				if (s_OnlyHere && !r.registered) continue;
				if (!Matches(r, filter)) continue;

				ImGui::PushID(static_cast<int>(i));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				// A dump-only row with no name has only 60 bits of its hash - the top bits are
				// unknown, so it cannot be opened.
				const bool openable = r.registered || !r.name.empty();
				ImGui::BeginDisabled(!openable);
				if (ImGui::SmallButton("Open")) {
					// OpenOverlay needs the registry's key object, so a row not registered here can
					// only go through addmenu + force.
					QueueOpen(std::format("0x{:016X}", r.hash), !r.registered, r.registered ? s_Via : kViaAddMenu);
				}
				ImGui::EndDisabled();
				ImGui::TableNextColumn();
				if (r.name.empty()) ImGui::TextDisabled("?");
				else if (r.registered) ImGui::TextUnformatted(r.name.c_str());
				else ImGui::TextDisabled("%s", r.name.c_str());
				ImGui::TableNextColumn();
				ImGui::TextDisabled("0x%016llX", static_cast<unsigned long long>(r.hash));
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", r.file.c_str());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}
}
