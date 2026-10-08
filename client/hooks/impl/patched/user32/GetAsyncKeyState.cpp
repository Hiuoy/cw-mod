#include "common.hpp"
#include "hooks/hook.hpp"
#include "overlay/d3d12_hook.hpp"

#include <intrin.h>

// The second half of the overlay's keyboard leak. Blocking WM_KEY* in the WndProc and draining the
// raw-input queue in GetRawInputBuffer between them cover every path that DELIVERS input - but T9
// also POLLS with GetAsyncKeyState, which asks the OS for the live key state and has no queue to
// starve and no message to swallow. That is why typing in a text box still moved the player.
//
// Answering 0 means "not pressed, and not pressed since the last call", which is exactly the state
// we want the game to observe while the overlay owns the keyboard.
//
// Scoped by CALLER, not applied globally: ImGui's Win32 backend polls key state too, and zeroing it
// for everyone would break shift-select and the modifier keys in the very text boxes this is meant
// to make usable. MinHook replaces the target's prologue with a jmp rather than a call, so
// _ReturnAddress() here is the caller's own return site - if it lands inside the game module the
// caller is the game, and anything else (our DLL, ImGui, the CRT) gets the truth.
namespace {
	// Resolved once. The game is the main module, so this is the image the loader mapped at start.
	struct GameModuleRange {
		std::uintptr_t m_Begin{};
		std::uintptr_t m_End{};

		GameModuleRange() {
			const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
			if (!base) return;
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return;
			m_Begin = base;
			m_End = base + nt->OptionalHeader.SizeOfImage;
		}

		bool Contains(std::uintptr_t addr) const {
			return m_Begin && addr >= m_Begin && addr < m_End;
		}
	};

	const GameModuleRange& GameModule() {
		static const GameModuleRange range;
		return range;
	}
}

template <>
SHORT Client::Hook::Hooks::HK_GetAsyncKeyState::hkCallback(const int key) {
	if (Client::Overlay::IsMenuOpen()
		&& GameModule().Contains(reinterpret_cast<std::uintptr_t>(_ReturnAddress()))) {
		return 0;
	}
	return m_Original(key);
}
