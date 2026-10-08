#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/settings.hpp"
#include "game/ui_scripts.hpp"

#include <format>
#include <mutex>
#include <string>

// Lua print transcript. After the trial fix (2026-09-24 06:47) the mode tiles are still padlocked
// and a click only plays a sound: the tile action (CoD.LobbyUtility[0x7CF8330D78EB9E5]) does nothing
// unless the current lobby menu is the online/LAN select menu, i.e. unless the lobby exists. Which step
// of PressStart -> BeginLivePlay -> LobbyVM.OnGoForward -> Lobby.ProcessQueue -> LobbyClientStart stops
// on an online boot can't be read statically, but the Lua narrates every one of them ("PressStart -
// BeginLivePlay.", "ShouldBeginLAN(false).", "Lobby.Timer.HostingLobby: Creating a lobby timer." ...)
// through Engine.PrintInfo/PrintWarning/PrintError, which retail compiles down to `return 0`.
//
// Each detour runs the original first (its arg checks and caller check are unchanged), then reads the
// channel and text straight off the Lua stack. Consecutive repeats collapse into one count line, and
// the whole transcript is capped, because timers print every frame. See dump_anchors.hpp.
//
// PrintInfo is also how our own menu Lua talks to the DLL: a text starting "cw-mod " is a command for
// UiScripts::OnCommand, never transcribed. So the PrintInfo detour is installed for "ui_scripts" too, and
// the transcript itself only with "lua_print".

namespace {
	using namespace Client;

	constexpr int kMaxLines = 4000;

	std::mutex g_Lock;
	int g_Lines = 0;
	std::string g_Last;
	int g_Repeats = 0;

	void Record(const char* level, void* L) {
		std::string channel, text;
		if (!Game::Pointers::LuaArgText(L, 2, text)) {
			return;
		}
		if (text.starts_with(Game::UiScripts::kCommandPrefix)) {
			Game::UiScripts::OnCommand(std::string_view(text).substr(Game::UiScripts::kCommandPrefix.size()));
			return;
		}
		if (!Game::Settings::Get().luaPrint) {
			return;
		}
		Game::Pointers::LuaArgText(L, 1, channel);
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
			text.pop_back();
		}

		std::lock_guard<std::mutex> lock(g_Lock);
		if (g_Lines > kMaxLines) {
			return;
		}
		std::string line = std::format("{} ch{} {}", level, channel, text);
		if (line == g_Last) {
			++g_Repeats;
			return;
		}
		if (g_Repeats) {
			LOG("LuaPrint", INFO, "  (previous line x{} more)", g_Repeats);
			g_Repeats = 0;
		}
		if (++g_Lines > kMaxLines) {
			LOG("LuaPrint", WARN, "cap of {} lines reached; the Lua print transcript stops here.", kMaxLines);
			return;
		}
		LOG("LuaPrint", INFO, "{}", line);
		g_Last = std::move(line);
	}
}

template <>
std::int32_t Client::Hook::Hooks::HK_LuaNative_PrintInfo::hkCallback(void* L) {
	const std::int32_t ret = m_Original(L);
	Record("info", L);
	return ret;
}

template <>
std::int32_t Client::Hook::Hooks::HK_LuaNative_PrintWarning::hkCallback(void* L) {
	const std::int32_t ret = m_Original(L);
	Record("WARN", L);
	return ret;
}

template <>
std::int32_t Client::Hook::Hooks::HK_LuaNative_PrintError::hkCallback(void* L) {
	const std::int32_t ret = m_Original(L);
	Record("ERROR", L);
	return ret;
}
