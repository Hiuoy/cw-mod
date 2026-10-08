#pragma once
// One translation unit per menu tab. Render() (menu.cpp) owns the window and the tab bar and does
// nothing else; each Draw*Tab below owns its own widgets and its own static UI state.
//
// Every one of these runs on the RENDER thread. They must never call game code directly - button
// handlers Enqueue() a closure for the game thread instead. See menu.hpp for why.

namespace Client::Overlay::Menu {
	void DrawHomeTab();
	void DrawSessionTab();
	void DrawServerBrowserTab();
	void DrawScriptingTab();
	void DrawDemonwareTab();
	void DrawLuiMenusTab();
	void DrawMapsTab();
	void DrawDebugTab();

	// The "(?)" hover tooltip every tab uses to explain what a button actually does.
	void HelpMarker(const char* desc);
}
