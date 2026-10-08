#include "common.hpp"
#include "hooks/hook.hpp"
#include "overlay/d3d12_hook.hpp"

// Why this hook exists at all: our WndProc already swallows WM_KEY*/WM_MOUSE* while the overlay is
// open, and that was enough for the mouse but not for the keyboard. T9 imports GetRawInputBuffer
// and drains the raw-input queue in bulk, which bypasses the window procedure entirely - so every
// keystroke typed into an ImGui text field was also reaching the game (opening the scoreboard,
// firing weapons, triggering binds) while the menu had focus.
//
// The original is still called rather than short-circuited. Raw input that is never read stays
// queued, so returning early would build up a backlog that all arrives at once the moment the
// overlay closes. Draining it and then reporting zero events consumes the input and discards it,
// which is what "the game should not see this" actually means.
//
// A size query (pData == nullptr) consumes nothing and only asks how large the buffer must be, so
// it is passed straight through - answering 0 there would be a lie the caller acts on.
template <>
UINT Client::Hook::Hooks::HK_GetRawInputBuffer::hkCallback(const PRAWINPUT pData, const PUINT pcbSize,
	const UINT cbSizeHeader) {
	const UINT result = m_Original(pData, pcbSize, cbSizeHeader);

	if (pData && result != static_cast<UINT>(-1) && Client::Overlay::IsMenuOpen()) {
		return 0;   // drained, then dropped on the floor
	}
	return result;
}
