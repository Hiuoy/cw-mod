#include "common.hpp"
#include "arxan/arxan_internal.hpp"

namespace Client::Arxan {
	NtdllDbgLocation s_NtdllDbgLocations[] = {
		{ "DbgBreakPoint",						(void*)0x1BFA100E, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgUserBreakPoint",					(void*)0x1E529177, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgUiConnectToDbg",					(void*)0x07694928, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgUiContinue",						(void*)0x1EA8F9BA, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgUiConvertStateChangeStructure",	(void*)0x1DBD8612, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgUiDebugActiveProcess",			(void*)0x1DFB2F44, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgUiGetThreadDebugObject",			(void*)0x1BD3FBF2, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgUiIssueRemoteBreakin",			(void*)0x1DB74F0B, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgUiRemoteBreakin",					(void*)0x1CB7AFD3, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgUiSetThreadDebugObject",			(void*)0x1BD63117, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgUiStopDebugging",					(void*)0x1BDB3908, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgUiWaitStateChange",				(void*)0x1BF02D1B, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgPrintReturnControlC",				(void*)0x05E9DFE8, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
		{ "DbgPrompt",							(void*)0x1D24ABA6, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xA0, 0xE0, 0x15, 0xB1, 0xFA, 0x7F, 0x00, 0x00 } },
	};

	std::vector<InlineAsmStub> s_InlineStubs{};

	std::vector<IntactChecksumHook> s_IntactChecksumHooks;
	std::vector<IntactBigChecksumHook> s_IntactBigChecksumHooks;
	std::vector<SplitChecksumHook> s_SplitChecksumHooks;

	void GeneralTlsCallbackStub() {
		return;
	}

	void ReplaceNtdllStackWithOurOwn(uint64_t addr, uint64_t locationCount) {
		uint64_t addressToReplace = *(uint64_t*)(char*)(addr - 0x8);
		uint64_t* addressReplace = (uint64_t*)addr;

		// we have to get the address location to exitprocess correctly or we will crash
		uint64_t kernel32ExitProcessAddress = (uint64_t)GetProcAddress(GetModuleHandle(TEXT("KERNEL32.dll")), "ExitProcess");
		memcpy((char*)s_NtdllDbgLocations[locationCount].m_PatchedByArxanBuffer + 6, &kernel32ExitProcessAddress, sizeof(uint64_t));

		int counter = 0;
		for (int i = 0; i < INT_MAX; i++) {
			addressReplace++;
			uint64_t addressResult = *(uint64_t*)addressReplace;

			if (addressResult == addressToReplace) {
				*addressReplace = (uint64_t)s_NtdllDbgLocations[locationCount].m_PatchedByArxanBuffer;
				counter++;

				// there are 2 pointers that point to the ntdll dbg locations we nopped out
				// we replace those 2 pointers with our own location to satisfy arxan so that the game doesnt crash
				if (counter == 2) {
					return;
				}
			}
		}
	}

	bool IsRelativelyFar(const void* pointer, const void* data) {
		const int64_t diff = size_t(data) - (size_t(pointer) + 5);
		const auto small_diff = int32_t(diff);
		return diff != int64_t(small_diff);
	}

	uint8_t* AllocateSomewhereNear(const void* base_address, const size_t size) {
		size_t offset = 0;
		while (true) {
			offset += size;
			auto* target_address = static_cast<const uint8_t*>(base_address) - offset;
			if (IsRelativelyFar(base_address, target_address)) {
				return nullptr;
			}

			const auto res = VirtualAlloc(const_cast<uint8_t*>(target_address), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
			if (res) {
				if (IsRelativelyFar(base_address, target_address)) {
					VirtualFree(res, 0, MEM_RELEASE);
					return nullptr;
				}

				return static_cast<uint8_t*>(res);
			}
		}
	}

	uint32_t ReverseBytes(uint32_t bytes) {
		uint32_t aux = 0;
		uint8_t byte;
		int i;

		for (i = 0; i < 32; i += 8) {
			byte = (bytes >> i) & 0xff;
			aux |= byte << (32 - 8 - i);
		}

		return aux;
	}

	int FixChecksum(uint64_t rbpOffset, uint64_t ptrOffset, uint64_t* ptrStack, uint32_t jmpInstructionDistance, uint32_t calculatedChecksumFromArg) {
		_Unreferenced_parameter_(jmpInstructionDistance);

		// get size of image from codcw
		uint64_t baseAddressStart = (uint64_t)GetModuleHandle(nullptr);
		IMAGE_DOS_HEADER* pDOSHeader = (IMAGE_DOS_HEADER*)GetModuleHandle(nullptr);
		IMAGE_NT_HEADERS* pNTHeaders = (IMAGE_NT_HEADERS*)((BYTE*)pDOSHeader + pDOSHeader->e_lfanew);
		auto sizeOfImage = pNTHeaders->OptionalHeader.SizeOfImage;
		uint64_t baseAddressEnd = baseAddressStart + sizeOfImage;

		// check if our checksum hooks got overwritten
		// we could probably ifdef this now but it's a good indicator to know if our checksum hooks still exist
		{
			for (int i = 0; i < s_IntactChecksumHooks.size(); i++) {
				DWORD oldProtect{};

				if (memcmp(s_IntactChecksumHooks[i].m_FunctionAddress, s_IntactChecksumHooks[i].m_Buffer, sizeof(uint8_t) * 7)) {
					uint64_t idaAddress = (uint64_t)s_IntactChecksumHooks[i].m_FunctionAddress - baseAddressStart + 0x140000000;

					MessageBoxA(nullptr, "Oh no! Our checksum... it's broken.", "t9-mod/CWHook", MB_OK);
					//printf("%llx %llx got changed\n", idaAddress, (uint64_t)s_IntactChecksumHooks[i].m_FunctionAddress);
					//fprintf(logFile, "%llx got changed\n", idaAddress);
					//fflush(logFile);
				}

				VirtualProtect(s_IntactChecksumHooks[i].m_FunctionAddress, sizeof(uint8_t) * 7, PAGE_EXECUTE_READWRITE, &oldProtect);
				memcpy(s_IntactChecksumHooks[i].m_FunctionAddress, s_IntactChecksumHooks[i].m_Buffer, sizeof(uint8_t) * 7);
				VirtualProtect(s_IntactChecksumHooks[i].m_FunctionAddress, sizeof(uint8_t) * 7, oldProtect, &oldProtect);
				FlushInstructionCache(GetCurrentProcess(), s_IntactChecksumHooks[i].m_FunctionAddress, sizeof(uint8_t) * 7);
			}

			for (int i = 0; i < s_IntactBigChecksumHooks.size(); i++) {
				DWORD oldProtect{};

				if (memcmp(s_IntactBigChecksumHooks[i].m_FunctionAddress, s_IntactBigChecksumHooks[i].m_Buffer, sizeof(uint8_t) * 7)) {
					uint64_t idaAddress = (uint64_t)s_IntactBigChecksumHooks[i].m_FunctionAddress - baseAddressStart + 0x140000000;

					MessageBoxA(nullptr, "Oh no! Our big checksum... it's broken.", "t9-mod/CWHook", MB_OK);
					//printf("%llx %llx got changed\n", idaAddress, (uint64_t)s_IntactBigChecksumHooks[i].m_FunctionAddress);
					//fprintf(logFile, "%llx got changed\n", idaAddress);
					//fflush(logFile);
				}

				VirtualProtect(s_IntactBigChecksumHooks[i].m_FunctionAddress, sizeof(uint8_t) * 10, PAGE_EXECUTE_READWRITE, &oldProtect);
				memcpy(s_IntactBigChecksumHooks[i].m_FunctionAddress, s_IntactBigChecksumHooks[i].m_Buffer, sizeof(uint8_t) * 10);
				VirtualProtect(s_IntactBigChecksumHooks[i].m_FunctionAddress, sizeof(uint8_t) * 10, oldProtect, &oldProtect);
				FlushInstructionCache(GetCurrentProcess(), s_IntactBigChecksumHooks[i].m_FunctionAddress, sizeof(uint8_t) * 10);
			}

			for (int i = 0; i < s_SplitChecksumHooks.size(); i++) {
				DWORD old_protect{};

				if (memcmp(s_SplitChecksumHooks[i].m_FunctionAddress, s_SplitChecksumHooks[i].m_Buffer, sizeof(uint8_t) * 7)) {
					uint64_t idaAddress = (uint64_t)s_SplitChecksumHooks[i].m_FunctionAddress - baseAddressStart + 0x140000000;

					MessageBoxA(nullptr, "Oh no! Our split... it's broken.", "t9-mod/CWHook", MB_OK);
					//printf("%llx %llx got changed\n", idaAddress, (uint64_t)s_SplitChecksumHooks[i].m_FunctionAddress);
					//fprintf(logFile, "%llx got changed\n", idaAddress);
					//fflush(logFile);
				}

				VirtualProtect(s_SplitChecksumHooks[i].m_FunctionAddress, sizeof(uint8_t) * 8, PAGE_EXECUTE_READWRITE, &old_protect);
				memcpy(s_SplitChecksumHooks[i].m_FunctionAddress, s_SplitChecksumHooks[i].m_Buffer, sizeof(uint8_t) * 8);
				VirtualProtect(s_SplitChecksumHooks[i].m_FunctionAddress, sizeof(uint8_t) * 8, old_protect, &old_protect);
				FlushInstructionCache(GetCurrentProcess(), s_SplitChecksumHooks[i].m_FunctionAddress, sizeof(uint8_t) * 8);
			}
		}

		static int fixChecksumCalls = 0;
		fixChecksumCalls++;

		uint32_t calculatedChecksum = calculatedChecksumFromArg;
		uint32_t reversedChecksum = ReverseBytes(calculatedChecksumFromArg);
		uint32_t* calculatedChecksumPtr = (uint32_t*)((char*)ptrStack + 0x120); // 0x120 is a good starting point to decrement downwards to find the calculated checksum on the stack
		uint32_t* calculatedReversedChecksumPtr = (uint32_t*)((char*)ptrStack + 0x120); // 0x120 is a good starting point to decrement downwards to find the calculated checksum on the stack

		bool doubleTextChecksum = false;
		uint64_t* previousResultPtr = nullptr;
		if (ptrOffset == 0 && rbpOffset < 0x90) {
			uint64_t* textPtr = (uint64_t*)((char*)ptrStack + rbpOffset + (rbpOffset % 0x8)); // make sure rbpOffset is aligned by 8 bytes
			int pointerCounter = 0;

			for (int i = 0; i < 20; i++) {
				uint64_t derefPtr = *(uint64_t*)textPtr;

				if (derefPtr >= baseAddressStart && derefPtr <= baseAddressEnd) {
					uint64_t derefResult = **(uint64_t**)textPtr;
					pointerCounter++;

					// store the ptr above 0xffffffffffffffff and then use it in our originalchecksum check
					if (derefResult == 0xffffffffffffffff) {
						if (pointerCounter > 2) {
							doubleTextChecksum = true;

							// because textptr will be pointing at 0xffffffffffffffff, increment it once 
							// so we are pointing to the correct checksum location

							// TODO: remove this, doesnt do anything, confirm with checksum 0x79d397c8
							// since we use previousResultPtr which doesnt rely on this
							textPtr++;
						}

						break;
					}

					previousResultPtr = textPtr;
				}

				textPtr--;
			}
		}
		else {
			// for debugging stack traces on bigger rbp offset checksums
			uint64_t* textPtr = (uint64_t*)((char*)ptrStack + rbpOffset + (rbpOffset % 0x8)); // make sure rbpOffset is aligned by 8 bytes

			for (int i = 0; i < 30; i++) {
				uint64_t derefPtr = *(uint64_t*)textPtr;

				if (derefPtr >= baseAddressStart && derefPtr <= baseAddressEnd) {
					uint64_t derefResult = **(uint64_t**)textPtr;
				}

				textPtr--;
			}
		}

		// find calculatedChecksumPtr, we will overwrite this later with the original checksum
		for (int i = 0; i < 80; i++) {
			uint32_t derefPtr = *(uint32_t*)calculatedChecksumPtr;

			if (derefPtr == calculatedChecksum) {
				break;
			}

			calculatedChecksumPtr--;
		}

		// find calculatedReversedChecksumPtr, we will overwrite this later with the original checksum
		for (int i = 0; i < 80; i++) {
			uint32_t derefPtr = *(uint32_t*)calculatedReversedChecksumPtr;

			if (derefPtr == reversedChecksum) {
				break;
			}

			calculatedReversedChecksumPtr--;
		}

		uint64_t* textPtr = (uint64_t*)((char*)ptrStack + rbpOffset + (rbpOffset % 0x8)); // add remainder to align ptr
		uint32_t originalChecksum = NULL;
		uint32_t* originalChecksumPtr = nullptr;

		// searching for a .text pointer that points to the original checksum, upwards from the rbp	
		for (int i = 0; i < 10; i++) {
			uint64_t derefPtr = *(uint64_t*)textPtr;

			if (derefPtr >= baseAddressStart && derefPtr <= baseAddressEnd) {
				if (ptrOffset == 0 && rbpOffset < 0x90) {
					if (doubleTextChecksum) {
						originalChecksum = **(uint32_t**)previousResultPtr;
					}
					else {
						originalChecksum = *(uint32_t*)derefPtr;
					}
				}
				else {
					originalChecksum = *(uint32_t*)((char*)derefPtr + ptrOffset * 4); // if ptrOffset is used the original checksum is in a different spot
					originalChecksumPtr = (uint32_t*)((char*)derefPtr + ptrOffset * 4);
				}

				break;
			}

			textPtr--;
		}

		*calculatedChecksumPtr = (uint32_t)originalChecksum;
		*calculatedReversedChecksumPtr = ReverseBytes((uint32_t)originalChecksum);

		// for big intact we need to keep overwriting 4 more times
		// seems to still run even if we comment this out wtf?
		uint32_t* tmpOriginalChecksumPtr = originalChecksumPtr;
		uint32_t* tmpCalculatedChecksumPtr = calculatedChecksumPtr;
		uint32_t* tmpReversedChecksumPtr = calculatedReversedChecksumPtr;
		if (originalChecksumPtr != nullptr)
		{
			for (int i = 0; i <= ptrOffset; i++)
			{
				*tmpCalculatedChecksumPtr = *(uint32_t*)tmpOriginalChecksumPtr;
				*tmpReversedChecksumPtr = ReverseBytes(*(uint32_t*)tmpOriginalChecksumPtr);

				tmpOriginalChecksumPtr--;
				tmpCalculatedChecksumPtr--;
				tmpReversedChecksumPtr--;
			}
		}

		return originalChecksum;
	}

	bool ArxanHealingChecksum(uint64_t rbp) {
		// check if rbpAddressLocationPtr is within the range of 8 bytes up & down from every checksum that we placed.
		uint64_t rbpAddressLocationPtr = *(uint64_t*)(rbp + 0x10);

		for (int i = 0; i < s_InlineStubs.size(); i++) {
			// 0x8
			// TODO: if 0x7 is too big then "mov [rdx], al" will make the game crash probably because its trying to overwrite areas next to our hooks that have to get modified.
			// we could do two seperate functions since "mov [rdx], eax" would be a 32 byte offset (?) and "mov [rdx], al" would be 4 byte offset (?)

			if (rbpAddressLocationPtr + 0x7 >= (uint64_t)s_InlineStubs[i].m_FunctionAddress &&
				rbpAddressLocationPtr - 0x7 <= (uint64_t)s_InlineStubs[i].m_FunctionAddress)
			{
				return true;
			}
		}

		return false;
	}
}
