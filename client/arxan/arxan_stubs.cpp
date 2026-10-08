// The two JIT-generated stub builders. Both walk a signature scan for Arxan's checksum write sites,
// allocate a trampoline within rel32 range, and assemble a stub that calls back into
// FixChecksum / ArxanHealingChecksum (arxan_internal.cpp).
#include "common.hpp"
#include "arxan/arxan_utility.hpp"
#include "arxan/arxan_internal.hpp"
#include "memory/memory.hpp"

#define ASMJIT_STATIC
#include <asmjit/core/jitruntime.h>
#include <asmjit/core/operand.h>
#include <asmjit/x86/x86assembler.h>
#include <asmjit/x86/x86operand.h>

namespace Client::Arxan {
	void Utility::CreateInlineAsmStub() {
		Common::Utility::NT::Library game{};

		auto locationsIntact = Memory::VectoredSigScan<void*>("89 04 8A 83 45 ? FF", game.GetName(), "Arxan Locations Intact");
		auto locationsIntactBig = Memory::VectoredSigScan<void*>("89 04 8A 83 85", game.GetName(), "Arxan Locations Intact Big");
		auto locationsSplit = Memory::VectoredSigScan<void*>("89 04 8A E9", game.GetName(), "Arxan Locations Split");

		uint64_t baseAddr = reinterpret_cast<uint64_t>(game.GetPtr());

		size_t intactCount = locationsIntact.size();
		size_t intactBigCount = locationsIntactBig.size();
		size_t splitCount = locationsSplit.size();
		size_t totalCount = intactCount + intactBigCount + splitCount;

		const size_t allocationSize = sizeof(uint8_t) * 128;
		s_InlineStubs.clear();

		for (int i = 0; i < intactCount; i++) {
			s_InlineStubs.push_back(InlineAsmStub{ locationsIntact.at(i).As<void*>(), nullptr, 7, IntactSmall });
		}

		for (int i = 0; i < intactBigCount; i++) {
			s_InlineStubs.push_back(InlineAsmStub{ locationsIntactBig.at(i).As<void*>(), nullptr, 10, IntactBig });
		}

		for (int i = 0; i < splitCount; i++) {
			s_InlineStubs.push_back(InlineAsmStub{ locationsSplit.at(i).As<void*>(), nullptr, 8, Split });
		}

		LPVOID asmBigStubLocation = AllocateSomewhereNear(game.GetPtr(), allocationSize * 0x80);
		memset(asmBigStubLocation, 0x90, allocationSize * 0x80);

		// avoid stub generation collision
		char* previousStubOffset = nullptr;
		// for jmp distance calculation
		char* currentStubOffset = nullptr;

		// TODO: once we are done with that merge all the checksum fix stub generators into one function
		// make that also use one big allocated memory page

		// TODO: fix the asm stub that requires a movzx, registers maybe are getting owned?

		for (int i = 0; i < s_InlineStubs.size(); i++) {
			// we don't know the previous offset yet
			if (currentStubOffset == nullptr) {
				currentStubOffset = (char*)asmBigStubLocation;
			}

			if (previousStubOffset != nullptr) {
				currentStubOffset = previousStubOffset;
			}

			void* functionAddress = s_InlineStubs[i].m_FunctionAddress;
			uint64_t jmpDistance = (uint64_t)currentStubOffset - (uint64_t)functionAddress - 5; // 5 bytes from relative call instruction

			// backup instructions that will get destroyed
			const int length = sizeof(uint8_t) * 8;
			uint8_t instructionBuffer[8] = {};
			memcpy(instructionBuffer, functionAddress, length);

			uint32_t instructionBufferJmpDistance = 0;
			if (instructionBuffer[3] == 0xE9) {
				memcpy(&instructionBufferJmpDistance, (char*)functionAddress + 0x4, 4); // 0x4 so we skip 0xE9
			}

			uint64_t rbpOffset = 0x0;
			bool jumpDistanceNegative = instructionBufferJmpDistance >> 31; // get sign bit from jump distance
			int32_t jumpDistance = instructionBufferJmpDistance;

			if (s_InlineStubs[i].m_Type == Split) {
				// TODO: receive the rbpOffset by going through the jmp instruction
				// on big rbp offsets we could do the same hack we did on big intact where we do rbpOffset+0x100 if its below 0x60
				char* rbpOffsetPtr = nullptr;

				// TODO: just use jumpDistance once we got a working test case
				if (jumpDistanceNegative) {
					rbpOffsetPtr = (char*)((uint64_t)functionAddress + jumpDistance + 0x8);
				}
				else {
					rbpOffsetPtr = (char*)((uint64_t)functionAddress + instructionBufferJmpDistance + 0x8);
				}

				rbpOffsetPtr++;

				// depending on the rbp offset from add dword ptr we need one more byte for the rbpOffset
				if (*(unsigned char*)rbpOffsetPtr == 0x45) {		// add dword ptr [rbp+68],-01
					rbpOffsetPtr++;
					rbpOffset = *(char*)rbpOffsetPtr;
				}
				else if (*(unsigned char*)rbpOffsetPtr == 0x85)	{	// add dword ptr [rbp+1CC],-01
					rbpOffsetPtr++;
					rbpOffset = *(short*)rbpOffsetPtr;
				}
			}

			// create assembly stub content
			// TODO: we could create three different asmjit build sections for each type
			// so we don't have if statements inbetween instructions for the cost of LOC but it would be more readible
			static asmjit::JitRuntime runtime;
			asmjit::CodeHolder code;
			code.init(runtime.environment());
			asmjit::x86::Assembler a(&code);

			if (s_InlineStubs[i].m_Type != Split) {
				rbpOffset = instructionBuffer[5];
			}

			a.sub(asmjit::x86::rsp, 0x32);
			pushad64();

			a.mov(asmjit::x86::qword_ptr(asmjit::x86::rsp, 0x20), asmjit::x86::rax);
			a.mov(asmjit::x86::rdx, asmjit::x86::rcx);	// offset within text section pointer (ecx*4)

			// we dont use rbpoffset since we only get 1 byte from the 2 byte offset (rbpOffset)
			// 0x130 is a good starting ptr to decrement downwards so we can find the original checksum
			if (s_InlineStubs[i].m_Type == IntactBig) {
				a.mov(asmjit::x86::rcx, 0x120);
			}
			else {
				a.mov(asmjit::x86::rcx, rbpOffset);
			}

			a.mov(asmjit::x86::r8, asmjit::x86::rbp);

			if (s_InlineStubs[i].m_Type == Split) {
				if (jumpDistanceNegative) {
					a.mov(asmjit::x86::r9, jumpDistance);
				}
				else {
					a.mov(asmjit::x86::r9, instructionBufferJmpDistance);
				}
			}
			else {
				a.mov(asmjit::x86::r9, instructionBufferJmpDistance); // incase we mess up a split checksum
			}

			a.mov(asmjit::x86::rax, (uint64_t)(void*)FixChecksum);
			a.call(asmjit::x86::rax);
			a.add(asmjit::x86::rsp, 0x8 * 4); // so that r12-r15 registers dont get corrupt

			popad64WithoutRAX();
			a.add(asmjit::x86::rsp, 0x32);

			a.mov(ptr(asmjit::x86::rdx, asmjit::x86::rcx, 2), asmjit::x86::eax); // mov [rdx+rcx*4], eax

			if (instructionBufferJmpDistance == 0) {
				if (s_InlineStubs[i].m_Type == IntactBig) {
					rbpOffset += 0x100;
				}

				a.add(dword_ptr(asmjit::x86::rbp, rbpOffset), -1); // add dword ptr [rbp+rbpOffset], 0FFFFFFFFh
			}
			else {
				// jmp loc_7FF641C707A5
				// push the desired address on to the stack and then perform a 64 bit RET
				a.add(asmjit::x86::rsp, 0x8); // pop return address off the stack cause we will jump
				uint64_t addressToJump = (uint64_t)functionAddress + instructionBufferJmpDistance;

				if (s_InlineStubs[i].m_Type == Split) {
					// TODO: just use jumpDistance once we got a working test case
					if (jumpDistanceNegative) {
						addressToJump = (uint64_t)functionAddress + jumpDistance + 0x8; // 0x8 call instruction + offset + 2 nops
					}
					else {
						addressToJump = (uint64_t)functionAddress + instructionBufferJmpDistance + 0x8; // 0x8 call instruction + offset + 2 nops
					}
				}

				a.mov(asmjit::x86::r11, addressToJump);	// r11 is being used but should be fine based on documentation

				if (s_InlineStubs[i].m_Type == Split) {
					a.add(asmjit::x86::rsp, 0x8); // since we dont pop off rax we need to sub 0x8 the rsp
				}

				a.push(asmjit::x86::r11);
			}

			if (s_InlineStubs[i].m_Type != Split) {
				a.add(asmjit::x86::rsp, 0x8); // since we dont pop off rax we need to sub 0x8 the rsp
			}

			a.ret();

			void* asmjitResult = nullptr;
			runtime.add(&asmjitResult, &code);

			// copy over the content to the stub
			uint8_t* tempBuffer = (uint8_t*)malloc(sizeof(uint8_t) * code.codeSize());
			memcpy(tempBuffer, asmjitResult, code.codeSize());
			memcpy(currentStubOffset, tempBuffer, sizeof(uint8_t) * code.codeSize());

			size_t callInstructionBytes = s_InlineStubs[i].m_BufferSize;
			size_t callInstructionLength = sizeof(uint8_t) * callInstructionBytes;

			DWORD old_protect{};
			VirtualProtect(functionAddress, callInstructionLength, PAGE_EXECUTE_READWRITE, &old_protect);
			memset(functionAddress, 0, callInstructionLength);
			VirtualProtect(functionAddress, callInstructionLength, old_protect, &old_protect);
			FlushInstructionCache(GetCurrentProcess(), functionAddress, callInstructionLength);

			// E8 cd CALL rel32  Call near, relative, displacement relative to next instruction
			uint8_t* jmpInstructionBuffer = (uint8_t*)malloc(sizeof(uint8_t) * callInstructionBytes);
			jmpInstructionBuffer[0] = 0xE8;
			jmpInstructionBuffer[1] = (jmpDistance >> (0 * 8));
			jmpInstructionBuffer[2] = (jmpDistance >> (1 * 8));
			jmpInstructionBuffer[3] = (jmpDistance >> (2 * 8));
			jmpInstructionBuffer[4] = (jmpDistance >> (3 * 8));
			jmpInstructionBuffer[5] = 0x90;
			jmpInstructionBuffer[6] = 0x90;

			if (s_InlineStubs[i].m_Type == IntactBig) {
				jmpInstructionBuffer[7] = 0x90;
				jmpInstructionBuffer[8] = 0x90;
				jmpInstructionBuffer[9] = 0x90;
			}

			if (s_InlineStubs[i].m_Type == Split) {
				jmpInstructionBuffer[7] = 0x90;
			}

			VirtualProtect(functionAddress, callInstructionLength, PAGE_EXECUTE_READWRITE, &old_protect);
			memcpy(functionAddress, jmpInstructionBuffer, callInstructionLength);
			VirtualProtect(functionAddress, callInstructionLength, old_protect, &old_protect);
			FlushInstructionCache(GetCurrentProcess(), functionAddress, callInstructionLength);

			// store location & bytes to check if arxan is removing our hooks
			if (s_InlineStubs[i].m_Type == IntactSmall) {
				IntactChecksumHook intactChecksum = {};
				intactChecksum.m_FunctionAddress = (uint64_t*)functionAddress;
				memcpy(intactChecksum.m_Buffer, jmpInstructionBuffer, sizeof(uint8_t) * s_InlineStubs[i].m_BufferSize);
				s_InlineStubs[i].m_Buffer = intactChecksum.m_Buffer;
				s_IntactChecksumHooks.push_back(intactChecksum);
			}

			if (s_InlineStubs[i].m_Type == IntactBig) {
				IntactBigChecksumHook intactBigChecksum = {};
				intactBigChecksum.m_FunctionAddress = (uint64_t*)functionAddress;
				memcpy(intactBigChecksum.m_Buffer, jmpInstructionBuffer, sizeof(uint8_t) * s_InlineStubs[i].m_BufferSize);
				s_InlineStubs[i].m_Buffer = intactBigChecksum.m_Buffer;
				s_IntactBigChecksumHooks.push_back(intactBigChecksum);
			}

			if (s_InlineStubs[i].m_Type == Split) {
				SplitChecksumHook splitChecksum = {};
				splitChecksum.m_FunctionAddress = (uint64_t*)functionAddress;
				memcpy(splitChecksum.m_Buffer, jmpInstructionBuffer, sizeof(uint8_t) * s_InlineStubs[i].m_BufferSize);
				s_InlineStubs[i].m_Buffer = splitChecksum.m_Buffer;
				s_SplitChecksumHooks.push_back(splitChecksum);
			}

			previousStubOffset = currentStubOffset + sizeof(uint8_t) * code.codeSize() + 0x8;
		}

		LOG("Arxan/CreateInlineAsmStub", DEBUG, "Intact checksum count: {}", intactCount);
		LOG("Arxan/CreateInlineAsmStub", DEBUG, "Intact big checksum count: {}", intactBigCount);
		LOG("Arxan/CreateInlineAsmStub", DEBUG, "Split checksum count: {}", splitCount);
	}

	void Utility::CreateChecksumHealingStub() {
		void* baseModule = GetModuleHandle(nullptr);
		Common::Utility::NT::Library game{};

		ChecksumHealingLocation healingLocations[]{
			{ Memory::VectoredSigScan<void*>("89 02 8B 45 20", game.GetName(), "Arxan Healing Locations [1]"), 5 },
			{ Memory::VectoredSigScan<void*>("88 02 83 45 20 FF", game.GetName(), "Arxan Healing Locations [2]"), 6 },
			{ Memory::VectoredSigScan<void*>("89 02 E9", game.GetName(), "Arxan Healing Locations [3]"), 7 },
			{ Memory::VectoredSigScan<void*>("88 02 E9", game.GetName(), "Arxan Healing Locations [4]"), 7 },
		};

		const size_t allocationSize = sizeof(uint8_t) * 0x100 * 1000;
		LPVOID healingStubLocation = AllocateSomewhereNear(GetModuleHandle(nullptr), allocationSize);
		memset(healingStubLocation, 0x90, allocationSize);

		// avoid stub generation collision
		char* previousStubOffset = nullptr;
		// for jmp distance calculation
		char* currentStubOffset = nullptr;

		size_t amountOfPatterns = sizeof(healingLocations) / sizeof(ChecksumHealingLocation);
		for (int type = 0; type < amountOfPatterns; type++) {
			size_t locations = healingLocations[type].m_ChecksumPattern.size();
			for (int i = 0; i < locations; i++) {
				uint8_t instructionBuffer[4] = {}; // 88 02 E9: 4          
				int32_t jumpDistance = 0;
				size_t callInstructionOffset = 5; // 0xE8 ? ? ? ?
				uint64_t jumpInstruction;
				uint64_t locationToJump;

				// we don't know the previous offset yet
				if (currentStubOffset == nullptr) {
					currentStubOffset = (char*)healingStubLocation;
				}

				if (previousStubOffset != nullptr) {
					currentStubOffset = previousStubOffset;
				}

				void* functionAddress = healingLocations[type].m_ChecksumPattern.at(i).As<void*>();

				if (*(uint8_t*)((uint8_t*)functionAddress + 2) == 0xE9) {
					memcpy(&jumpDistance, (char*)functionAddress + 3, 4); // ptr after 0xE9
					jumpInstruction = (uint64_t)functionAddress + 2; 		// at the jmp instruction
					locationToJump = jumpInstruction + jumpDistance + callInstructionOffset;

					// get size of image from codcw
					uint64_t baseAddressStart = (uint64_t)GetModuleHandle(nullptr);
					IMAGE_DOS_HEADER* pDOSHeader = (IMAGE_DOS_HEADER*)GetModuleHandle(nullptr);
					IMAGE_NT_HEADERS* pNTHeaders = (IMAGE_NT_HEADERS*)((BYTE*)pDOSHeader + pDOSHeader->e_lfanew);
					auto sizeOfImage = pNTHeaders->OptionalHeader.SizeOfImage;
					uint64_t baseAddressEnd = baseAddressStart + sizeOfImage;

					if ((locationToJump > baseAddressStart && locationToJump < baseAddressEnd) != true) {
						continue;
					}

					memcpy(instructionBuffer, (char*)locationToJump, sizeof(uint8_t) * 4);

					if (type == 2) {
						uint8_t instruction[3] = { 0x8B, 0x45, 0x20 };
						if (memcmp(instructionBuffer, instruction, sizeof(uint8_t) * 3) != 0) {
							continue;
						}
					}

					if (type == 3) {
						uint8_t instruction[4] = { 0x83, 0x45, 0x20, 0xFF };
						if (memcmp(instructionBuffer, instruction, sizeof(uint8_t) * 4) != 0) {
							continue;
						}
					}
				}

				static asmjit::JitRuntime runtime;
				asmjit::CodeHolder code;
				code.init(runtime.environment());

				using namespace asmjit::x86;
				Assembler a(&code);
				asmjit::Label L1 = a.newLabel();
				asmjit::Label DEBUG = a.newLabel();

				a.sub(rsp, 0x32);
				pushad64_Min();

				a.mov(rcx, rbp);
				a.mov(r15, (uint64_t)(void*)ArxanHealingChecksum);
				a.call(r15);
				a.movzx(r15, al);	// if arxan tries to replace our checksum set r15 to 1

				popad64_Min();
				a.add(rsp, 0x32);

				switch (type) {
				case 0:
					/*
						mov     [rdx], eax
						mov     eax, [rbp+20h]
					*/
					// dont replace our checksum if r15 is 1
					a.cmp(r15, 1);
					a.je(L1);
					a.mov(qword_ptr(rdx), eax);

					a.bind(L1);
					a.mov(eax, qword_ptr(rbp, 0x20));
					a.ret();
					break;
				case 1:
					/*
						mov     [rdx], al
						add     dword ptr [rbp+20h], -1
					*/
					// dont replace our checksum if r15 is 1
					a.cmp(r15, 1);
					a.je(L1);
					a.mov(qword_ptr(rdx), al);

					a.bind(L1);
					a.add(dword_ptr(rbp, 0x20), -1);
					a.ret();
					break;
				case 2:
					/*
						mov     [rdx], eax
						jmp     loc_7FF7366C7B94
					*/
					// dont replace our checksum if r15 is 1
					a.cmp(r15, 1);
					a.je(L1);
					a.mov(qword_ptr(rdx), eax);

					a.bind(L1);
					a.add(rsp, 0x8);
					a.mov(r15, locationToJump);
					a.push(r15);
					a.ret();
					break;
				case 3:
					/*
						mov     [rdx], al
						jmp     loc_7FF738FB7A45
					*/
					// dont replace our checksum if r15 is 1
					a.cmp(r15, 1);
					a.je(L1);
					a.mov(qword_ptr(rdx), al);

					a.bind(L1);
					a.add(rsp, 0x8);
					a.mov(r15, locationToJump);
					a.push(r15);
					a.ret();
					break;
				default:
					LOG("Arxan/CreateChecksumHealingStub", ERROR, "We shouldn't be here.");
					getchar();
					abort();
				}

				void* asmjitResult = nullptr;
				runtime.add(&asmjitResult, &code);

				// copy over the content to the stub
				uint8_t* tempBuffer = (uint8_t*)malloc(sizeof(uint8_t) * code.codeSize());
				memcpy(tempBuffer, asmjitResult, code.codeSize());
				memcpy(currentStubOffset, tempBuffer, sizeof(uint8_t) * code.codeSize());

				size_t callInstructionBytes = healingLocations[type].m_Length;
				size_t callInstructionLength = sizeof(uint8_t) * callInstructionBytes;

				DWORD old_protect{};
				VirtualProtect(functionAddress, callInstructionLength, PAGE_EXECUTE_READWRITE, &old_protect);
				memset(functionAddress, 0, callInstructionLength);
				VirtualProtect(functionAddress, callInstructionLength, old_protect, &old_protect);
				FlushInstructionCache(GetCurrentProcess(), functionAddress, callInstructionLength);

				uint64_t jmpDistance = (uint64_t)currentStubOffset - (uint64_t)functionAddress - 5;
				uint8_t* jmpInstructionBuffer = (uint8_t*)malloc(sizeof(uint8_t) * callInstructionBytes);

				// E8 cd CALL rel32  Call near, relative, displacement relative to next instruction
				jmpInstructionBuffer[0] = 0xE8;
				jmpInstructionBuffer[1] = (jmpDistance >> (0 * 8));
				jmpInstructionBuffer[2] = (jmpDistance >> (1 * 8));
				jmpInstructionBuffer[3] = (jmpDistance >> (2 * 8));
				jmpInstructionBuffer[4] = (jmpDistance >> (3 * 8));

				for (int v = 0; v < callInstructionBytes - 5; v++) {
					jmpInstructionBuffer[5 + v] = 0x90;
				}

				VirtualProtect(functionAddress, callInstructionLength, PAGE_EXECUTE_READWRITE, &old_protect);
				memcpy(functionAddress, jmpInstructionBuffer, callInstructionLength);
				VirtualProtect(functionAddress, callInstructionLength, old_protect, &old_protect);
				FlushInstructionCache(GetCurrentProcess(), functionAddress, callInstructionLength);

				previousStubOffset = currentStubOffset + sizeof(uint8_t) * code.codeSize() + 0x8;

				// debugging printf
				if (i == 0) {
					LOG("Arxan/CreateChecksumHealingStub", DEBUG, "Type {} @ 0x{:016X} (0x{:016X})", type, PTR_AS(std::uintptr_t, functionAddress),
						PTR_AS(std::uintptr_t, functionAddress) - PTR_AS(std::uintptr_t, game.GetPtr()) + 0x140000000);
				}
			}
		}
	}
}
