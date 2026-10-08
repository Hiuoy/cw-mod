#pragma once
// Our own menu Lua: every <game>/cw-mod/ui_scripts/*.lua, run as SOURCE in the LUI state (the engine's
// chunks are bytecode; T9 kept LuaJIT's full parser, see kDump_lua_load in dump_anchors.hpp).
//
// When: right after ui/main.lua (LUI_RunFile detour), before LUI_Init sends main_loaded and opens the
// first menu. First the map prelude (CWMOD.maps and CWMOD.active from mapkit_loader.hpp), then the
// built-in scripts (custom_maps.lua, the Zombies Private CUSTOM MAPS tab; server_browser.lua, the Zombies
// main screen's SERVER BROWSER button and menu; a file of the same name in the folder replaces one), then
// the folder in file-name order. Then, once a second from the game-thread tick, any file that changed or
// appeared is run again in the same state: edit a script while the game is up and it re-runs. So a script
// must be safe to run twice (keep the original of anything it wraps in a global, set once). LUI's _G
// refuses new globals: create one with rawset(_G, name, value).
//
// A script is loaded with lua_load and called with LUI_ProtectedCall (one result). client.log gets one
// (UiScripts) line per run: the returned value when it is a string/number, else "ok", or the Lua error.
//
// Back to the DLL: Engine.PrintInfo(channel, "cw-mod <command>") is a command, not a print (OnCommand):
//   cw-mod map <id>             the CUSTOM MAPS pick (MapKit::SetActiveMap); "cw-mod map" = none
//   cw-mod browser open|close   the SERVER BROWSER menu came up / went away
//   cw-mod browser join <xuid>  join that listed LAN host (hex XUID), on the next tick
//   cw-mod log <text>           a (UiScripts) line
//
// To the Lua, while the SERVER BROWSER menu is open: the tick runs a small chunk setting CWMOD.browser's
// hosts, status lines and last join answer whenever they changed (the overlay tab's snapshot, from
// LanBrowser). It only sets data; the menu's own timer reads it, so no element changes outside LUI's update.
//
// Hash literals: T9's lexer turns @"text" into a hashed name (type "xhash"). While our own chunk is
// parsed the lexer's hash callback is ours, and it takes two forms:
//   @"DirectorPrivateZM"   the engine's name hash (FNV-1a over the lowercased text, 63 bits)
//   @"0x9E283FFCDB06CE1"   a hash as the Lua dump prints it (its low 60 bits): the hashed name already
//                          in the state with those bits, or the number itself when none is (logged)
//   @"0x39E283FFCDB06CE1"  a hash in full (63 bits, as tools/lua_disasm.py prints a KXHASH): as written
// An element's native methods are keyed by hashed name, element[@"GetModel"](element); the ones LUI's own
// Lua defines by plain string, element:setClass(...). The decompiled text prints both alike, by name: which
// it is shows only in the bytecode (lua_disasm.py: KXHASH + TGETV is hashed, TGETS is a string).
//
// Text handed to a stock widget: the game's Lua passes nearly all of it through Engine.LocalizeHash
// (LuaNative_Localize_Impl), often twice. That call returns a string untouched only when it starts with byte
// 21, the form of its own results (21, the text, 20). Any other string, and any hash, is the name of a
// localize entry, and a missing entry ends the process: UI Error 100004, then DB_FindXAssetHeader's fatal
// error for a missing localize asset, which no pcall sees (the first SERVER BROWSER build, 2026-10-07). So a
// name of ours goes in as "\021text\020", and a prompt label is a hash the game's own menus use. An
// element's own setText takes plain text.
// Off with "ui_scripts": false in cw-mod.json.

#include <string_view>

namespace Client::Game::UiScripts {
	// The prefix that makes an Engine.PrintInfo text a command.
	inline constexpr std::string_view kCommandPrefix = "cw-mod ";

	// LUI_RunFile detour, after the original ran ui/main.lua on L. Game thread.
	void OnMainLoaded(void* L);

	// Per-frame game-thread tick: re-run changed scripts (checks the folder once a second), and feed the
	// SERVER BROWSER menu while it is open.
	void Tick();

	// The SERVER BROWSER menu is open. A Lua error LUI catches while it is must not end the process
	// (LuiError_ReportFatal.cpp): the menu is ours, built on stock widgets that were never fed this data.
	bool MenuOpen();

	// Engine.PrintInfo detour: a command from our Lua, the text after kCommandPrefix. Game thread.
	void OnCommand(std::string_view command);
}
