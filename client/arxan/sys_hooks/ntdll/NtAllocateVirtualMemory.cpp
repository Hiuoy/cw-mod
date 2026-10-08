#include "common.hpp"
#include "arxan/arxan_utility.hpp"
#include "arxan/ntdll_restore.hpp"
#include "arxan/sys_hooks.hpp"

template <>
NTSTATUS Client::Arxan::SysHooks::HK_NtAllocateVirtualMemory::hkCallback(HANDLE processHandle, PVOID* baseAddress, ULONG_PTR zeroBits, PSIZE_T regionSize,
	ULONG allocationType, ULONG protect)
{
	NTSTATUS result = m_Original(processHandle, baseAddress, zeroBits, regionSize, allocationType, protect);
	static bool bInit = false;

	if (protect & PAGE_EXECUTE_READWRITE && *regionSize == Arxan::Utility::GetNtdllSize() && !bInit) {
		static int counter = 0;
		counter++;

		/* checksum counts for latest build supported by donetsk defcon
			p 57
			p 41
			p 30
		*/
		static bool firstTime = true;
		if (firstTime) {
			clock_t startTime = clock();
			Arxan::Utility::CreateInlineAsmStub();
			Arxan::Utility::CreateChecksumHealingStub();

			double elapsedTime = (double)(clock() - startTime) / CLOCKS_PER_SEC;
			LOG("Arxan/NtAllocateVirtualMemory", INFO, "Creating inline hooks for checksums took {} second{} - finished hooking.",
				elapsedTime, elapsedTime == 1.0 ? "" : "s");

			firstTime = false;
		}

		// Arxan does a startup checksum check routine that I didn't bother bypassing, 
		// doesn't matter anyways since iirc none of the game's functions gets called anyways.
		// 6 is just an arbitary number so that we create gameplay related hooks a little bit later.
		if (counter == 6) {
			Arxan::Utility::DisableTlsCallbacks();
			Arxan::Utility::DisableKiUserApcDispatcherHook();
			Arxan::Utility::RestoreKernel32ThreadInitThunkFunction();
			Arxan::Utility::RemoveNtdllChecksumChecks();
			Arxan::NtDllRestore::RestoreDebugFunctions();

			// TODO: we aint need this here right now, maybe add later?
			//InitializePluginLoader();

			// DbgRemove (the anti-debug loop) is intentionally NOT spawned. Bisection proved this
			// thread is what crashed this build during startup: boot only completes to the main menu
			// with it disabled. Prime suspect within it is the periodic `peb->BeingDebugged = 0x8F`
			// poke (its own comment flags it), but the poke wasn't isolated further — the whole
			// thread is off. Offline play doesn't need anti-debug. To revive for a debugger-attach
			// workflow, isolate the poke (try BeingDebugged = 0) and re-test. See
			// docs/crash_boot_arxan.md.
			//CreateThread(NULL, NULL, (LPTHREAD_START_ROUTINE)Arxan::Utility::DbgRemove, NULL, NULL, NULL);

			bInit = true;
		}
	}

	return result;
}
