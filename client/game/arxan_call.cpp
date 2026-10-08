#include "common.hpp"
#include "game/arxan_call.hpp"

#include <cstring>
#include <mutex>

namespace Client::Game::ArxanCall {
	namespace {
		// RVA of `add rsp,28h; retn` at the tail of sub_7FF71D0B3D10, preceded by a call rel32.
		// Dump-absolute 0x7FF71D0B3D2C, dump imagebase 0x7FF71CBC0000 (build 1.34.0.15931218 - the
		// same pinned build every kDump_* constant in game.cpp is keyed to).
		//
		// PROVISIONAL, exactly like the other RVA-resolved anchors: this is not an AOB signature yet.
		// Init verifies the bytes before anything is allowed to jump there, so a build bump turns
		// into a clean "not ready" instead of a jump into moved code. The verification pattern is
		// what an AOB scan would look for, so converting this later is mechanical.
		constexpr std::uintptr_t kImageRetRva = 0x4F3D2CULL;
		constexpr std::uint8_t   kGadgetBytes[]  = { 0x48, 0x83, 0xC4, 0x28, 0xC3 }; // add rsp,28h; retn
		constexpr std::uint8_t   kGadgetPrefix   = 0xE8;                             // call rel32, at -5

		// sub rsp,30h / mov r11,imm64 / mov [rsp],r11 / mov rax,imm64 / jmp rax. See the header for
		// why the frame moves by exactly 0x30 and why the callee still sees a conforming stack.
		constexpr std::uint8_t kThunkTemplate[] = {
			0x48, 0x83, 0xEC, 0x30,                                                  // sub  rsp, 30h
			0x49, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,              // mov  r11, imm64
			0x4C, 0x89, 0x1C, 0x24,                                                  // mov  [rsp], r11
			0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,              // mov  rax, imm64
			0xFF, 0xE0                                                               // jmp  rax
		};
		constexpr std::size_t kOffGadgetImm = 6;
		constexpr std::size_t kOffTargetImm = 20;
		constexpr std::size_t kThunkSize    = sizeof(kThunkTemplate);
		constexpr std::size_t kThunkStride  = 32;    // padded so each thunk starts 16-byte aligned
		constexpr std::size_t kArenaSize    = 4096;  // 128 thunks; we need well under a dozen

		std::mutex     g_Mutex;
		void*          g_ImageRet = nullptr;
		std::uint8_t*  g_Arena    = nullptr;
		std::size_t    g_Used     = 0;

		bool BytesMatch(const std::uint8_t* at) {
			if (*(at - 5) != kGadgetPrefix) return false;
			for (std::size_t i = 0; i < sizeof(kGadgetBytes); ++i) {
				if (at[i] != kGadgetBytes[i]) return false;
			}
			return true;
		}

		// Kept free of anything needing unwinding so __try is legal here (MSVC C2712). The read is
		// inside the image and cannot normally fault, but a wrong build could put the RVA in a
		// non-committed page and this must degrade rather than crash the game on startup.
		bool SafeBytesMatch(const std::uint8_t* at) {
			__try {
				return BytesMatch(at);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	bool Init(std::uintptr_t moduleBase, std::size_t imageSize) {
		std::lock_guard<std::mutex> lock(g_Mutex);
		if (g_ImageRet) return true;
		if (!moduleBase || imageSize <= kImageRetRva + sizeof(kGadgetBytes)) {
			LOG("ArxanCall", ERROR, "no module base / image too small - guarded calls unavailable");
			return false;
		}

		const auto at = reinterpret_cast<std::uint8_t*>(moduleBase + kImageRetRva);
		if (!SafeBytesMatch(at)) {
			LOG("ArxanCall", ERROR, "return gadget at {} does not read `E8 .. | add rsp,28h; retn` - "
				"build mismatch, guarded calls disabled", static_cast<void*>(at));
			return false;
		}

		// RWX for life. Flipping the page to RW to add a thunk faults any thread running another thunk in
		// it at that moment, and MakeThunk now also runs after the game is live (GSC loader enable).
		g_Arena = static_cast<std::uint8_t*>(
			VirtualAlloc(nullptr, kArenaSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!g_Arena) {
			LOG("ArxanCall", ERROR, "thunk arena allocation failed");
			return false;
		}

		g_ImageRet = at;
		g_Used = 0;
		LOG("ArxanCall", INFO, "guarded-call thunks ready: return gadget {} (rva 0x{:X}), arena {}",
			g_ImageRet, kImageRetRva, static_cast<void*>(g_Arena));
		return true;
	}

	bool Ready() {
		std::lock_guard<std::mutex> lock(g_Mutex);
		return g_ImageRet != nullptr && g_Arena != nullptr;
	}

	void* ImageRetGadget() {
		std::lock_guard<std::mutex> lock(g_Mutex);
		return g_ImageRet;
	}

	void* MakeThunk(void* target) {
		if (!target) return nullptr;

		std::lock_guard<std::mutex> lock(g_Mutex);
		if (!g_ImageRet || !g_Arena) return nullptr;
		if (g_Used + kThunkStride > kArenaSize) {
			LOG("ArxanCall", ERROR, "thunk arena exhausted");
			return nullptr;
		}

		std::uint8_t* slot = g_Arena + g_Used;

		std::memcpy(slot, kThunkTemplate, kThunkSize);
		std::memcpy(slot + kOffGadgetImm, &g_ImageRet, sizeof(void*));
		std::memcpy(slot + kOffTargetImm, &target, sizeof(void*));
		FlushInstructionCache(GetCurrentProcess(), slot, kThunkSize);

		g_Used += kThunkStride;
		return slot;
	}
}
