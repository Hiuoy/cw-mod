#pragma once
// Internal helpers shared by the game/*.cpp translation units. Not part of the public surface of
// Pointers - nothing outside client/game/ should include this.
//
// Everything here exists for one reason: this module reaches into an Arxan-obfuscated process over
// struct layouts recovered by reverse engineering. A wrong offset must degrade to a diagnostic line,
// never an access violation, so every deref on an unverified layout goes through SafeRead/SafeCopy.
#include "game/game.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace Client::Game {
	// Marks the thread as being inside a deliberate may-fault probe, so the crash logger can tell an
	// expected, already-handled probe fault apart from the process actually dying. The flag itself
	// lives in common.hpp - see the commentary there for why this is needed at all.
	struct SafeProbeScope {
		SafeProbeScope() { ++Client::g_SafeProbeDepth; }
		~SafeProbeScope() { --Client::g_SafeProbeDepth; }
	};

	// SEH-guarded typed read. Returns false (and leaves `out` untouched) if the address faults, so a
	// read-only pointer chase over an UNVERIFIED struct layout degrades to a diagnostic line instead
	// of crashing the game. Templated on POD T only (int / pointer) - the body has no C++ objects
	// requiring unwinding, which is what lets __try/__except coexist here (MSVC C2712 otherwise).
	// Never call this on addresses that might have side effects; it is for plain memory reads only.
	// Split in two because __try cannot share a function with anything needing object unwinding
	// (MSVC C2712), and the probe scope is an object: the inner half does the guarded read, the outer
	// half marks the thread as probing for the benefit of the crash logger.
	template <typename T>
	bool SafeReadGuarded(const void* addr, T& out) {
		__try {
			out = *reinterpret_cast<const T*>(addr);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	template <typename T>
	bool SafeRead(const void* addr, T& out) {
		if (!addr) return false;
		SafeProbeScope scope;
		return SafeReadGuarded(addr, out);
	}

	// SEH-guarded typed write, the mirror of SafeRead. Same rule: the target offsets come from RE, so a
	// wrong one must report false instead of killing the process. POD T only, for the same C2712 reason.
	template <typename T>
	bool SafeWrite(void* addr, const T& value) {
		if (!addr) return false;
		__try {
			*reinterpret_cast<T*>(addr) = value;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Copy a chunk of the game's address space into our own buffer. A region that VirtualQuery just
	// reported as committed and readable can still be freed or re-protected by another thread before
	// we touch it, so every bulk read goes through here: a fault costs one chunk, not the process.
	bool SafeCopy(void* dst, const void* src, std::size_t n);

	// Space-separated hex rendering, for the address/key blobs the join transcript prints.
	std::string HexBytes(const std::uint8_t* p, std::size_t n);

	// Dvar type/flag decoding. Ids from Dvar_SetInt's type switch; flags from Dvar_CanSetValue.
	const char* DvarTypeName(int type);
	std::string DvarFlagNames(int flags);
}
