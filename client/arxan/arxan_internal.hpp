#pragma once
// Shared internals of the Arxan defeat: the checksum-hook bookkeeping and the helpers that the
// JIT-generated stubs call back into.
//
// These are NOT in an anonymous namespace on purpose. CreateInlineAsmStub / CreateChecksumHealingStub
// (arxan_stubs.cpp) emit machine code that CALLs FixChecksum and ArxanHealingChecksum by address,
// and the bookkeeping vectors below are written by those builders and read by the callbacks, so both
// halves need external linkage to live in separate translation units.
//
// Nothing outside client/arxan/ should include this.
#include "memory/memory.hpp"

#include <cstdint>
#include <vector>

namespace Client::Arxan {
	enum ChecksumType {
		IntactSmall,
		IntactBig,
		Split
	};

	struct InlineAsmStub {
		void* m_FunctionAddress;
		uint8_t* m_Buffer;
		size_t m_BufferSize;
		ChecksumType m_Type;
	};

	struct NtdllDbgLocation {
		const char* m_FunctionName;
		void* m_AddrLocation;
		uint8_t m_PatchedByArxanBuffer[14];
	};

	struct IntactChecksumHook {
		uint64_t* m_FunctionAddress;
		uint8_t m_Buffer[7];
	};

	struct IntactBigChecksumHook {
		uint64_t* m_FunctionAddress;
		uint8_t m_Buffer[7 + 3];
	};

	struct SplitChecksumHook {
		uint64_t* m_FunctionAddress;
		uint8_t m_Buffer[8];
	};

	struct ChecksumHealingLocation {
		std::vector<Memory::ScannedResult<void>> m_ChecksumPattern;
		size_t m_Length;
	};

	// The 14 ntdll debug entrypoints Arxan patches, with the 14-byte thunk it expects to find.
	// TODO: Sig these so they work on 1.34.1 too
	extern NtdllDbgLocation s_NtdllDbgLocations[14];

	extern std::vector<InlineAsmStub>          s_InlineStubs;
	extern std::vector<IntactChecksumHook>     s_IntactChecksumHooks;
	extern std::vector<IntactBigChecksumHook>  s_IntactBigChecksumHooks;
	extern std::vector<SplitChecksumHook>      s_SplitChecksumHooks;

	// A no-op we redirect the game's TLS callbacks to.
	void GeneralTlsCallbackStub();

	// Point the two Arxan-owned pointers at our own patched-thunk buffer, so nopping out an ntdll
	// debug function does not crash the game.
	void ReplaceNtdllStackWithOurOwn(uint64_t addr, uint64_t locationCount);

	// True when `data` is further from `pointer` than a rel32 jump can reach.
	bool IsRelativelyFar(const void* pointer, const void* data);

	// Reserve executable memory within rel32 range of `base_address`, so a 5-byte jmp can reach it.
	uint8_t* AllocateSomewhereNear(const void* base_address, size_t size);

	uint32_t ReverseBytes(uint32_t bytes);

	// Called FROM the generated stubs. Restores our own checksum hooks if Arxan healed over them,
	// then rewrites the freshly calculated checksum on the stack with the original one.
	int FixChecksum(uint64_t rbpOffset, uint64_t ptrOffset, uint64_t* ptrStack,
		uint32_t jmpInstructionDistance, uint32_t calculatedChecksumFromArg);

	// Called FROM the generated healing stubs: true when the write Arxan is about to perform lands
	// on one of our own hooks (and must therefore be suppressed).
	bool ArxanHealingChecksum(uint64_t rbp);
}
