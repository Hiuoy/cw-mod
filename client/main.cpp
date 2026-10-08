#include "common.hpp"
#include "arxan/sys_calls.hpp"
#include "arxan/sys_hooks.hpp"
#include "arxan/ntdll_restore.hpp"
#include "game/game.hpp"
#include "game/boot_profile.hpp"
#include "game/mapkit_trace.hpp"
#include "hooks/hook.hpp"
#include "scripting/scripting.hpp"
#include <utility/nt.hpp>

namespace {
	// A crash logger, because right now a fatal fault kills the process with nothing written at all.
	// The mod already hooks AddVectoredExceptionHandler to keep track of the game's own handlers, but
	// it installs none of its own, so "the game just crashes" is literally all the information there
	// is. This turns that into a faulting address.
	//
	// Installed with first = 0, i.e. APPENDED to the VEH chain: Arxan raises and handles exceptions
	// deliberately as part of its protection, and a handler that runs first would drown the log in
	// them. We only ever see what nothing else claimed. The handler always returns
	// EXCEPTION_CONTINUE_SEARCH, so it observes and never alters behaviour.
	std::atomic<int> s_CrashesLogged{0};

	// Kept in its own function on purpose: __try cannot live in a function that needs C++ object
	// unwinding, and the logger below is full of std::string.
	bool ReadStackWord(const std::uintptr_t* at, std::uintptr_t& out) {
		__try {
			out = *at;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	std::string DescribeCodeAddress(const void* addr) {
		HMODULE mod = nullptr;
		if (GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
			wchar_t wide[MAX_PATH]{};
			GetModuleFileNameW(mod, wide, MAX_PATH);
			std::filesystem::path p{wide};
			return std::format("{}+0x{:X}", p.filename().string(),
				reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(mod));
		}
		return std::format("0x{:X} <no module>", reinterpret_cast<std::uintptr_t>(addr));
	}

	bool IsFatalLookingException(DWORD code) {
		switch (code) {
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_ILLEGAL_INSTRUCTION:
		case EXCEPTION_PRIV_INSTRUCTION:
		case EXCEPTION_STACK_OVERFLOW:
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
		case EXCEPTION_IN_PAGE_ERROR:
		case STATUS_HEAP_CORRUPTION:
			return true;
		default:
			return false;
		}
	}

	// Faults that reach this logger on every healthy boot, measured across four boots on 2026-09-16.
	// Both are survived, so something later in the chain handles them. Logging them in full used up
	// the 24-event cap (33 of 37 events were the first one) and pushed real crashes out of the log.
	// Each gets one short line the first time and a running count every 100th time, so a change in
	// frequency is still visible without the dump.
	enum class KnownFault { None, NoModuleFFE, KuserSharedWrite };
	std::atomic<int> s_KnownFaultCounts[3]{};

	KnownFault ClassifyKnownFault(const EXCEPTION_RECORD* rec) {
		if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return KnownFault::None;
		const ULONG_PTR op = rec->ExceptionInformation[0];
		const ULONG_PTR target = rec->ExceptionInformation[1];
		const auto at = reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress);

		// Every ~3s after login: a READ of -1 from generated code outside any module, always at a
		// page offset of 0xFFE (a new page each time). Not in our DLL's range.
		HMODULE mod = nullptr;
		const bool inModule = GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(rec->ExceptionAddress), &mod) && mod;
		if (op == 0 && target == ~ULONG_PTR{0} && !inModule && (at & 0xFFF) == 0xFFE) return KnownFault::NoModuleFFE;

		// Once per boot, during Arxan setup: a WRITE to the kernel-mode KUSER_SHARED_DATA alias
		// (0xFFFFF78000000900) from BlackOpsColdWar.exe+0xCD42D06 (build 1.34.0.15931218).
		const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		if (op == 1 && target == 0xFFFFF78000000900ULL && at == exe + 0xCD42D06) return KnownFault::KuserSharedWrite;

		return KnownFault::None;
	}

	LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* info) {
		if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
		const auto* const rec = info->ExceptionRecord;
		if (!IsFatalLookingException(rec->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;

		// A first-chance AV reaches every VEH before the __except that catches it. Our own SafeRead
		// probes unverified struct layouts and faults BY DESIGN, dozens of times per error dump, and
		// logging those filled this log and pushed the real fault out of it entirely. If the thread is
		// inside a deliberate probe, this fault is already handled - it is not the crash.
		if (Client::g_SafeProbeDepth > 0) return EXCEPTION_CONTINUE_SEARCH;

		if (const KnownFault known = ClassifyKnownFault(rec); known != KnownFault::None) {
			const int n = s_KnownFaultCounts[static_cast<int>(known)].fetch_add(1) + 1;
			if (n == 1 || n % 100 == 0) {
				LOG("Crash", DEBUG, "known benign fault #{}: {} (not dumped; see ClassifyKnownFault)", n,
					known == KnownFault::NoModuleFFE ? "no-module +0xFFE read of -1" : "KUSER_SHARED_DATA write at exe+0xCD42D06");
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}

		if (s_CrashesLogged.fetch_add(1) >= 24) return EXCEPTION_CONTINUE_SEARCH;

		std::string detail;
		if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
			const ULONG_PTR op = rec->ExceptionInformation[0];
			detail = std::format(" ({} 0x{:X})",
				op == 0 ? "reading" : op == 1 ? "writing" : "executing",
				static_cast<std::uintptr_t>(rec->ExceptionInformation[1]));
		}

		const auto* const ctx = info->ContextRecord;
		std::string out = std::format(
			"\n  code    0x{:08X}{}\n  at      {}\n  rsp     0x{:X}\n  rcx 0x{:X}  rdx 0x{:X}  r8 0x{:X}  r9 0x{:X}\n",
			rec->ExceptionCode, detail, DescribeCodeAddress(rec->ExceptionAddress),
			ctx->Rsp, ctx->Rcx, ctx->Rdx, ctx->R8, ctx->R9);

		// A poor man's stack trace: scan the stack for values that point into a loaded module. It is
		// not a real unwind and will include stale frames, but it needs no dbghelp (which Arxan is
		// hostile to) and it reliably shows WHO called the faulting code, which is the whole question.
		out += "  stack (return-address candidates):\n";
		const auto* sp = reinterpret_cast<const std::uintptr_t*>(ctx->Rsp);
		int shown = 0;
		for (int i = 0; i < 256 && shown < 16; ++i) {
			std::uintptr_t v = 0;
			if (!ReadStackWord(sp + i, v)) break;
			if (v < 0x10000) continue;
			HMODULE mod = nullptr;
			if (!GetModuleHandleExW(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(v), &mod) || !mod) {
				continue;
			}
			out += std::format("    [rsp+0x{:03X}] {}\n", i * sizeof(std::uintptr_t),
				DescribeCodeAddress(reinterpret_cast<const void*>(v)));
			++shown;
		}

		LOG("Crash", ERROR, "Unhandled-looking exception:{}", out);
		Client::Game::MapKitTrace::OnCrash();
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, PVOID) {
	using namespace Client;
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(hMod);
		static Common::Utility::NT::Library game{};

		g_Module = hMod;
		std::uint32_t gameChecksum = game.GetChecksum();
		if (g_GameVersions.contains(gameChecksum)) {
			g_GameIdentifier = g_GameVersions[gameChecksum];
		}

		g_MainThread = CreateThread(nullptr, 0, [](PVOID) -> DWORD {
			Common::g_LogService = std::make_unique<Common::LogService>();
			Common::WinAPI::_SetConsoleTitle(std::format("t9-mod: " GIT_DESCRIBE " - Call of Duty: Black Ops Cold War v{}", g_GameIdentifier.m_Version));
			g_GameModuleName = game.GetName();
			game.Unprotect();
			LOG("MainThread", INFO, "T9-Mod injected.");

			AddVectoredExceptionHandler(0, CrashLogger);
			LOG("MainThread", INFO, "Crash logger installed (appended to the VEH chain).");

			Arxan::NtDllRestore::RestoreDebugFunctions();
			MH_Initialize();

			g_ArxanSysHooks = std::make_unique<Arxan::SysHooks>();
			LOG("MainThread", INFO, "System hooks initialized.");

			while (g_Running) {
				// no, we are not planning on unloading the mod, that will cause
				// absolute disaster. instead, we just unload it when the dll is
				// called for detach, therefore safely exiting.
				std::this_thread::sleep_for(1s);
			}

			if (g_Pointers) {
				g_Pointers.reset();
				LOG("MainThread", INFO, "Pointers uninitialized.");
			}

			if (g_Hooks) {
				g_Hooks.reset();
				LOG("MainThread", INFO, "Hooks uninitialized.");
			}

			g_ArxanSysHooks.reset();
			LOG("MainThread", INFO, "System hooks uninitialized.");

			Common::g_LogService.reset();
			return 0;
		}, nullptr, 0, &g_MainThreadId);
	}
	else if (reason == DLL_PROCESS_DETACH) {
		g_Running = false;
	}

	return TRUE;
}

bool s_CalledMainEntryPoint = false;
void MainEntryPoint() {
	if (s_CalledMainEntryPoint) {
		return;
	}
	s_CalledMainEntryPoint = true;
	using namespace Client;

	LOG("MainThread", INFO, "MainEntryPoint reached (DiscordCreate called); constructing Pointers...");
	g_Pointers = std::make_unique<Game::Pointers>();
	LOG("MainThread", INFO, "Pointers initialized.");

	g_Hooks = std::make_unique<Hook::Hooks>();
	LOG("MainThread", INFO, "Hooks initialized.");

	// Resolve the GSC loader's signatures (read-only). The loader itself installs from
	// PostArxanDetectionHooks, with the other game-code detours.
	Scripting::Initialize();
	LOG("MainThread", INFO, "Scripting subsystem initialized.");

	CreateThread(nullptr, 0, [](PVOID) -> DWORD {
		while (!(*g_Pointers->m_Scr_Initialized)) {
			std::this_thread::sleep_for(100ms);
		}

		Game::Boot::ApplyAfterScriptInit(*g_Pointers);
		return 0;
	}, nullptr, 0, nullptr);
}

extern "C" __declspec(dllexport) int /* EDiscordResult */ /* DISCORD_API */ DiscordCreate(int /* DiscordVersion */ version, struct DiscordCreateParams* params, struct IDiscordCore** result) {
	_Unreferenced_parameter_(version);
	_Unreferenced_parameter_(params);
	_Unreferenced_parameter_(result);

	MainEntryPoint();

	LOG("Proxy/DiscordCreate", INFO, "DiscordCreate called, returning 1 (ServiceUnavailable).");
	return 1 /* DiscordResult_ServiceUnavailable */;
}
