#include "common.hpp"
#include "arxan/arxan_utility.hpp"
#include "arxan/arxan_internal.hpp"
#include "arxan/ntdll_restore.hpp"
#include "arxan/sys_calls.hpp"
#include "memory/memory.hpp"

#include <MinHook.h>

// RemoveNtdllChecksumChecks assembles a small stub of its own (the checksum-check bypass), so this
// TU needs asmjit too - the two bigger builders live in arxan_stubs.cpp.
#define ASMJIT_STATIC
#include <asmjit/core/jitruntime.h>
#include <asmjit/core/operand.h>
#include <asmjit/x86/x86assembler.h>
#include <asmjit/x86/x86operand.h>

namespace Client::Arxan {
	void Utility::DisableTlsCallbacks() {
		Common::Utility::NT::Library game{};
		std::vector<Common::Utility::NT::Library::TlsCallback*> callbacks = game.GetTlsCallbacks();

		for (Common::Utility::NT::Library::TlsCallback* callback : callbacks) {
			if (MH_CreateHook(callback, &GeneralTlsCallbackStub, nullptr) != MH_OK) {
				LOG("Arxan/DisableTlsCalbacks", ERROR, "Failed to create hook @ 0x{:016X} (0x{:016X})", PTR_AS(std::uintptr_t, callback),
					PTR_AS(std::uintptr_t, callback) - PTR_AS(std::uintptr_t, game.GetPtr()) + 0x140000000);
				continue;
			}

			if (MH_EnableHook(callback) != MH_OK) {
				LOG("Arxan/DisableTlsCalbacks", ERROR, "Failed to enable hook @ 0x{:016X} (0x{:016X})", PTR_AS(std::uintptr_t, callback),
					PTR_AS(std::uintptr_t, callback) - PTR_AS(std::uintptr_t, game.GetPtr()) + 0x140000000);
			}
		}

		LOG("Arxan/DisableTlsCallbacks", INFO, "Disabled TLS callbacks.");
	}

	// Reads `size` pristine bytes at `rva` straight out of the on-disk copy of a module. Used to
	// undo Arxan's ntdll patches without hardcoding one Windows build's instruction bytes: the file
	// on disk is by definition the unpatched image for whatever OS we happen to be running on.
	static bool ReadPristineImageBytes(const Common::Utility::NT::Library& module, std::uintptr_t rva, void* out, size_t size) {
		const std::filesystem::path path = module.GetPath();
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return false;
		}

		std::vector<char> image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (image.size() < sizeof(IMAGE_DOS_HEADER)) {
			return false;
		}

		auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(image.data());
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || image.size() < static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS)) {
			return false;
		}

		auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(image.data() + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) {
			return false;
		}

		auto* section = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
			const std::uintptr_t start = section->VirtualAddress;
			const std::uintptr_t end = start + (section->Misc.VirtualSize > section->SizeOfRawData
				? section->Misc.VirtualSize : section->SizeOfRawData);
			if (rva < start || rva >= end) {
				continue;
			}

			const std::uintptr_t offset = section->PointerToRawData + (rva - start);
			if (offset + size > image.size()) {
				return false;
			}

			memcpy(out, image.data() + offset, size);
			return true;
		}

		return false;
	}

	void Utility::DisableKiUserApcDispatcherHook() {
		Common::Utility::NT::Library ntdll("ntdll.dll");
		void* procAddr = ntdll.GetProc<void*>("KiUserApcDispatcher");
		if (procAddr == nullptr) {
			LOG("Arxan/DisableKiUserApcDispatcherHook", ERROR, "Couldn't find KiUserApcDispatcher.");
			return;
		}

		// The prologue Arxan overwrites differs between ntdll builds, so recover it from the file on
		// disk rather than from a hardcoded array (a wrong array here = an instant boot crash on any
		// Windows build other than the one it was captured on).
		uint8_t bytes[14]{};
		const std::uintptr_t rva = PTR_AS(std::uintptr_t, procAddr) - PTR_AS(std::uintptr_t, ntdll.GetPtr());
		if (!ReadPristineImageBytes(ntdll, rva, bytes, sizeof(bytes))) {
			LOG("Arxan/DisableKiUserApcDispatcherHook", ERROR,
				"Couldn't read the pristine KiUserApcDispatcher bytes from disk - leaving it patched.");
			return;
		}

		if (memcmp(procAddr, bytes, sizeof(bytes)) == 0) {
			LOG("Arxan/DisableKiUserApcDispatcherHook", DEBUG, "KiUserApcDispatcher is already intact.");
			return;
		}

		DWORD oldProtect{};
		VirtualProtect(procAddr, sizeof(bytes), PAGE_EXECUTE_READWRITE, &oldProtect);
		memcpy(procAddr, bytes, sizeof(bytes));
		VirtualProtect(procAddr, sizeof(bytes), oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), procAddr, sizeof(bytes));

		LOG("Arxan/DisableKiUserApcDispatcherHook", DEBUG, "Restored KiUserApcDispatcher from disk (ntdll+0x{:X}).", rva);
	}

	void Utility::RestoreKernel32ThreadInitThunkFunction() {
		// cold war removes the function ptr from ntdll Kernel32ThreadInitThunkFunction to its own, redirecting createremotethread
		// does rdtsc checks which in turn makes it so that if the process is completely suspended, will crash on created threads

		void* rtlUserThreadStartAddr = Common::Utility::NT::Library("ntdll.dll").GetProc<void*>("RtlUserThreadStart");
		// BaseThreadInitThunk is exported by kernel32.dll, not ntdll.dll. Looking it up in ntdll
		// returns null, so this restore used to early-return and never rewrote the thread-init
		// thunk pointer — leaving Arxan's RDTSC-checked redirect in place and faulting threads
		// created during the game's own boot. See docs/crash_boot_arxan.md.
		void* baseThreadInitThunkAddr = Common::Utility::NT::Library("kernel32.dll").GetProc<void*>("BaseThreadInitThunk");

		if (rtlUserThreadStartAddr == nullptr || baseThreadInitThunkAddr == nullptr) {
			if (rtlUserThreadStartAddr == nullptr) {
				LOG("Arxan/RestoreKernel32ThreadInitThunkFunction", ERROR, "Couldn't find RtlUserThreadStart.");
			}
			if (baseThreadInitThunkAddr == nullptr) {
				LOG("Arxan/RestoreKernel32ThreadInitThunkFunction", ERROR, "Couldn't find BaseThreadInitThunk.");
			}
			return;
		}

		// The pointer we want lives in an ntdll global that RtlUserThreadStart loads with a
		// `mov rax, [rip+disp32]` (48 8B 05). Its offset inside the prologue is NOT stable across
		// Windows builds - it used to be hardcoded at +0x7, which on a differently-compiled ntdll
		// decodes garbage and makes the memcpy below scribble 8 bytes over a random address (black
		// screen + crash right after "Disabled TLS callbacks"). Scan for the instruction instead and
		// validate that the resolved slot actually lands inside ntdll before writing to it.
		Common::Utility::NT::Library ntdll("ntdll.dll");
		auto* const prologue = PTR_AS(PUCHAR, rtlUserThreadStartAddr);
		uint64_t* baseThreadInitPtr = nullptr;

		for (size_t i = 0; i + 7 <= 0x40; ++i) {
			if (prologue[i] != 0x48 || prologue[i + 1] != 0x8B || prologue[i + 2] != 0x05) {
				continue;
			}

			auto* const next = prologue + i + 7;
			auto* const candidate = PTR_AS(uint64_t*, next + DEREF_PTR_AS(LONG, prologue + i + 3));
			if (!ntdll.IsAddressInRange(PTR_AS(std::size_t, candidate))) {
				continue;
			}

			baseThreadInitPtr = candidate;
			break;
		}

		if (baseThreadInitPtr == nullptr) {
			LOG("Arxan/RestoreKernel32ThreadInitThunkFunction", ERROR,
				"Couldn't locate the thread-init thunk slot in RtlUserThreadStart - leaving it alone.");
			return;
		}

		DWORD oldProtect{};
		VirtualProtect(baseThreadInitPtr, sizeof(uint64_t), PAGE_READWRITE, &oldProtect);
		memcpy(baseThreadInitPtr, &baseThreadInitThunkAddr, sizeof(uint64_t));
		VirtualProtect(baseThreadInitPtr, sizeof(uint64_t), oldProtect, &oldProtect);

		LOG("Arxan/RestoreKernel32ThreadInitThunkFunction", DEBUG, "Restored thread-init thunk (ntdll+0x{:X}).",
			PTR_AS(std::uintptr_t, baseThreadInitPtr) - PTR_AS(std::uintptr_t, ntdll.GetPtr()));
	}

	void Utility::RemoveNtdllChecksumChecks() {
		void* baseModule = GetModuleHandle(nullptr);

		const size_t allocationSize = sizeof(uint8_t) * 0x100 * 1000;
		LPVOID healingStubLocation = AllocateSomewhereNear(GetModuleHandle(nullptr), allocationSize);
		memset(healingStubLocation, 0x90, allocationSize);

		// avoid stub generation collision
		char* previousStubOffset = nullptr;
		// for jmp distance calculation
		char* currentStubOffset = nullptr;

		size_t amountOfLocations = sizeof(s_NtdllDbgLocations) / sizeof(NtdllDbgLocation);
		for (uint64_t i = 0; i < amountOfLocations; i++) {
			void* functionAddress = (char*)s_NtdllDbgLocations[i].m_AddrLocation + (uint64_t)baseModule;

			// movzx   eax, byte ptr [rax]		0F B6 00
			// jmp     loc_7FF71B61F9FA			E9 80 EC D3 E3
			// 0F B6 00 E9 42 F5 BC E4 = 8 bytes
			const uint64_t instructionsInBytes = 0x8;

			int32_t jumpDistance = 0;
			uint64_t locationToJump;

			// we don't know the previous offset yet
			if (currentStubOffset == nullptr) {
				currentStubOffset = (char*)healingStubLocation;
			}

			if (previousStubOffset != nullptr) {
				currentStubOffset = previousStubOffset;
			}

			memcpy(&jumpDistance, (char*)functionAddress + 4, 4); // ptr after 0xE9
			locationToJump = jumpDistance + instructionsInBytes + (uint64_t)functionAddress;

			static asmjit::JitRuntime runtime;
			asmjit::CodeHolder code;
			code.init(runtime.environment());

			using namespace asmjit::x86;
			Assembler a(&code);

			asmjit::Label DEBUG = a.newLabel();

			a.movq(xmm15, rsp);
			pushad64();
			a.sub(rsp, 0x20);
			a.movq(rcx, xmm15);
			a.mov(rdx, i);
			a.call(ReplaceNtdllStackWithOurOwn);
			a.add(rsp, 0x20);
			popad64();
			a.push(rax);
			a.mov(rax, 0);
			a.movq(xmm15, rax);
			a.pop(rax);

			uint64_t patchedBuffer = (uint64_t)s_NtdllDbgLocations[i].m_PatchedByArxanBuffer;
			a.mov(rax, patchedBuffer);
			a.mov(rcx, patchedBuffer);
			a.movzx(eax, byte_ptr(rax));
			a.jmp(locationToJump);

			void* asmjitResult = nullptr;
			runtime.add(&asmjitResult, &code);

			// copy over the content to the stub
			uint8_t* tempBuffer = (uint8_t*)malloc(sizeof(uint8_t) * code.codeSize());
			memcpy(tempBuffer, asmjitResult, code.codeSize());
			memcpy(currentStubOffset, tempBuffer, sizeof(uint8_t) * code.codeSize());

			DWORD oldProtect{};
			size_t instructionLength = 0x8;

			VirtualProtect(functionAddress, instructionLength, PAGE_EXECUTE_READWRITE, &oldProtect);
			memset(functionAddress, 0x90, instructionLength);
			VirtualProtect(functionAddress, instructionLength, oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), functionAddress, instructionLength);

			uint64_t jmpDistance = (uint64_t)currentStubOffset - (uint64_t)functionAddress - 5;
			uint8_t* jmpInstructionBuffer = (uint8_t*)malloc(sizeof(uint8_t) * instructionLength);
			memset(jmpInstructionBuffer, 0x90, instructionLength);

			// E8 cd CALL rel32  Call near, relative, displacement relative to next instruction
			jmpInstructionBuffer[0] = 0xE9;
			jmpInstructionBuffer[1] = (jmpDistance >> (0 * 8));
			jmpInstructionBuffer[2] = (jmpDistance >> (1 * 8));
			jmpInstructionBuffer[3] = (jmpDistance >> (2 * 8));
			jmpInstructionBuffer[4] = (jmpDistance >> (3 * 8));

			VirtualProtect(functionAddress, instructionLength, PAGE_EXECUTE_READWRITE, &oldProtect);
			memcpy(functionAddress, jmpInstructionBuffer, instructionLength);
			VirtualProtect(functionAddress, instructionLength, oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), functionAddress, instructionLength);

			previousStubOffset = currentStubOffset + sizeof(uint8_t) * code.codeSize() + 0x8;
		}

		LOG("Arxan/RemoveNtdllChecksumChecks", INFO, "Done.");
	}

	// NOTE: currently not spawned — see the DbgRemove call site in
	// client/arxan/sys_hooks/ntdll/NtAllocateVirtualMemory.cpp (it crashes boot on this build).
	void Utility::DbgRemove() {
		bool forceDebuggedNum = false;
		bool setOneTime = false;
		while (true) {
			auto* const peb = reinterpret_cast<PPEB>(__readgsqword(0x60));
			peb->BeingDebugged = 0x8F; // this could end up getting the game to crash cause of arxan

			NtDllRestore::RestoreDebugFunctions();
			Sleep(100);
		}
	}
}
