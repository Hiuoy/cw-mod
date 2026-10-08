#pragma once

// The cw-mod in-game menu.
//
// Threading model: the menu is *drawn* on the render thread (inside the Present hook), but game
// functions must be *called* on the game thread. Render() therefore never calls game code
// directly — button handlers Enqueue() a closure, and DrainActions() runs those closures from
// the per-frame game-thread tick (OnShowOverStack). This is the foundation every future feature
// (god mode, points, give-weapon, ...) will build on.
#include <functional>
#include <string>

namespace Client::Overlay::Menu {
	// Render thread: draw the menu UI. Call between ImGui::NewFrame() and ImGui::Render().
	void Render();

	// Any thread: schedule an action to run on the game thread (the safe place to touch game state).
	void Enqueue(std::function<void()> action);

	// Game thread: run all queued actions. Call once per frame from a game-thread tick.
	void DrainActions();

	// Any thread: show a script's print (iprintln / iprintlnbold) for a few seconds at the top of the screen,
	// whether or not the menu is open.
	void PushScriptMessage(std::string text);

	// Render thread: draw the script messages still on screen. Call every frame between NewFrame and Render.
	void RenderScriptMessages();
}
