#include "common.hpp"
#include "overlay/menu.hpp"
#include "overlay/tabs/tabs.hpp"
#include "game/game.hpp"

#include <imgui.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <cfloat>

namespace Client::Overlay::Menu {
	// The "no dev console" tab. BOCW ships without a console and stores no dvar name strings —
	// everything is keyed by a 64-bit hash — so the only way to reach a dvar used to be hardcoding a
	// constant and rebuilding. This tab closes that loop: type a name, it gets hashed with the
	// engine's own algorithm, and looked up in the live registry.
	// Result strings are written on the game thread and read on the render thread, so they need the
	// same treatment as g_CoopDump: swapping a std::string under a reader can free the buffer it is
	// mid-read of.
	static std::mutex g_DebugResultMutex;

	static void SetDebugResult(std::string& slot, std::string value) {
		std::lock_guard<std::mutex> lock(g_DebugResultMutex);
		slot.swap(value);
	}

	// Copy under the lock, then draw from the copy — the widget never points into a string another
	// thread can swap out from under it.
	static std::string GetDebugResult(const std::string& slot) {
		std::lock_guard<std::mutex> lock(g_DebugResultMutex);
		return slot;
	}

	void DrawDebugTab() {
		ImGui::TextWrapped("Reach dvars by NAME. Everything here hashes the name with the engine's own "
			"function, so nothing needs hardcoding.");
		ImGui::Separator();

		// --- hash calculator ---------------------------------------------------------------
		ImGui::TextDisabled("Name hash");
		static char s_HashInput[128] = "lobby";
		ImGui::InputText("String##hash", s_HashInput, sizeof(s_HashInput));
		ImGui::Text("0x%016llX", static_cast<unsigned long long>(
			Client::Game::Pointers::HashString(s_HashInput)));
		ImGui::SameLine();
		HelpMarker("FNV-1a-64, ASCII A-Z lowercased, masked to 63 bits. The same hash is used for "
			"dvars, LUI menu names and GSC #\"...\" literals, verified against the engine.\n\n"
			"Pure math on the render thread - no game calls, safe to leave open.");

		// --- dvars -------------------------------------------------------------------------
		ImGui::Separator();
		ImGui::TextDisabled("Dvars");
		static char s_DvarName[128] = "com_maxclients";
		static int s_DvarInt = 1;
		static bool s_DvarBool = true;
		static std::string s_DvarResult;

		ImGui::InputText("Dvar name", s_DvarName, sizeof(s_DvarName));

		// The name is snapshotted into every closure rather than read from the shared buffer on the
		// game thread — the render thread can be mid-edit of s_DvarName when the action runs.
		if (ImGui::Button("Inspect")) {
			const std::string name = s_DvarName;
			Enqueue([name] {
				if (g_Pointers) SetDebugResult(s_DvarResult, g_Pointers->DescribeDvar(name.c_str()));
			});
		}
		ImGui::SameLine();
		HelpMarker("Looks the name up in the live dvar registry and reports type, flags and the "
			"plaintext copy of its value. 'NOT REGISTERED' means this build has no such dvar.");

		ImGui::SetNextItemWidth(120.0f);
		ImGui::InputInt("##dvarint", &s_DvarInt);
		ImGui::SameLine();
		if (ImGui::Button("Set int")) {
			const int v = s_DvarInt;
			const std::string name = s_DvarName;
			Enqueue([name, v] {
				if (g_Pointers) SetDebugResult(s_DvarResult, g_Pointers->SetDvarInt(name.c_str(), v));
			});
		}
		ImGui::SameLine();
		ImGui::Checkbox("##dvarbool", &s_DvarBool);
		ImGui::SameLine();
		if (ImGui::Button("Set bool")) {
			const bool v = s_DvarBool;
			const std::string name = s_DvarName;
			Enqueue([name, v] {
				if (g_Pointers) SetDebugResult(s_DvarResult, g_Pointers->SetDvarBool(name.c_str(), v));
			});
		}
		ImGui::SameLine();
		HelpMarker("Both go through the engine's own type-dispatching setters, on the game thread. "
			"Use 'Set int' for int/enum/float dvars and 'Set bool' for bools - the result line says "
			"if the engine would refuse (READONLY / WRITEPROTECTED / cheat-protected).");

		if (ImGui::Button("Dump all registered dvars")) {
			Enqueue([] {
				if (g_Pointers) SetDebugResult(s_DvarResult, g_Pointers->DumpAllDvars());
			});
		}
		ImGui::SameLine();
		HelpMarker("Walks all 1024 buckets of the dvar hash table and writes every registered dvar "
			"(hash, type, flags, address) to cw-mod/dvars.txt in the game folder. That file is the "
			"only build-accurate hash list there is - match a name wordlist against it offline.\n\n"
			"Note: tools/wordlists/massive_dvar_dump.txt is from a different CoD (its hashes use the "
			"IW prime 0x10000000233, which appears nowhere in this exe). Good names, wrong hashes.");

		// Not Enqueue'd: the harvest is pure memory reading with no engine call and no engine lock,
		// so putting it on the game thread would only buy a multi-second freeze. A flag keeps a
		// second scan from starting while one is in flight.
		static std::atomic<bool> s_Harvesting{false};
		ImGui::BeginDisabled(s_Harvesting.load());
		if (ImGui::Button("Recover names from memory")) {
			s_Harvesting = true;
			SetDebugResult(s_DvarResult, "Scanning process memory...");
			std::thread([] {
				if (g_Pointers) SetDebugResult(s_DvarResult, g_Pointers->HarvestNamesFromMemory());
				s_Harvesting = false;
			}).detach();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		HelpMarker("Hashes every name-shaped string in the game's committed memory and joins it "
			"against the live dvar registry (plus any 0x... lines in cw-mod/hashes_wanted.txt, so "
			"you can drop tools/wordlists/lui_menu_hashes.txt in unedited). Writes cw-mod/names_recovered.txt.\n\n"
			"This reaches what the exe never could: decompressed fastfile zones, the Lua heap, asset "
			"name pools. Load into the map/menu you care about FIRST - a name only turns up if its "
			"zone is resident, so the further you get in, the more it finds.\n\n"
			"Runs on a worker thread (pure memory reads, no engine calls), so the game keeps running.");

		std::string dvarResult = GetDebugResult(s_DvarResult);
		if (!dvarResult.empty()) {
			ImGui::InputTextMultiline("##dvarresult", dvarResult.data(), dvarResult.size() + 1,
				ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 7), ImGuiInputTextFlags_ReadOnly);
		}
	}
}
