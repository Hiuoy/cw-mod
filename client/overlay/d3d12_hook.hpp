#pragma once

// D3D12 / DXGI hook plumbing for the in-game overlay.
//
// BOCW renders with DirectX 12. To draw an overlay we need to run code inside the
// game's present loop and get hold of its D3D12 command queue. We do that by hooking
// three functions, resolved from a throwaway device's vtables (no hardcoded offsets):
//   * IDXGISwapChain::Present        (vtable index 8)  - our per-frame render tick
//   * IDXGISwapChain::ResizeBuffers  (vtable index 13) - drop our back-buffer refs on resize
//   * ID3D12CommandQueue::ExecuteCommandLists (index 10) - to capture the command queue
//
// ResizeBuffers is not optional: DXGI fails the call if any back-buffer reference is
// outstanding, and the game does not survive that failure - so every resolution, display
// or fullscreen change would take the process down with it.
#include <cstdint>

namespace Client::Overlay {
	// Installs the Present + ExecuteCommandLists hooks. Call once, after MH_Initialize().
	// Returns true if both hooks were installed.
	bool Initialize();

	// Number of frames the Present hook has seen (render-thread heartbeat). For UI/diagnostics.
	std::uint64_t FrameCount();

	// True while the menu is toggled open (Insert). For UI/diagnostics.
	bool IsMenuOpen();
}
