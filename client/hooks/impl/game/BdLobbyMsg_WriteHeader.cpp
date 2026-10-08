#include "common.hpp"
#include "hooks/hook.hpp"

#include <array>
#include <atomic>
#include <format>
#include <string>

// Lobby request census. Every outgoing Demonware lobby request header is written by
// BdLobbyMsg_WriteHeader(buffer, msgType, serviceId). (The first version hooked BdLobbyMsg_Ctor, which
// only some requests use; service 1 and the StructData requests never went through it.) The call sites are Arxan-obfuscated jump chains, so reading them statically is
// slow; this names the sender at runtime instead.
//
// Why now: boot 2026-09-16 23:20, right after the content-slot bypass set 2,2, the game sent
// 'service 1 / msgType 8' (one UInt64 = 1) every frame, 2129 times in 40 s, retrying each default
// empty reply. The first sighting of each (msgType, service) pair logs its return chain as module
// RVAs (paste into IDA as 0x7FF71CBC0000 + RVA); repeats log a count at 1, 10, 100, 1000...

namespace {
	std::array<std::atomic<std::uint32_t>, 256 * 256> g_Seen{};

	bool IsReportCount(std::uint32_t n) {
		for (std::uint32_t p = 1; p <= n; p *= 10) {
			if (p == n) return true;
		}
		return false;
	}
}

template <>
std::uint64_t Client::Hook::Hooks::HK_BdLobbyMsg_WriteHeader::hkCallback(void** buffer, std::uint8_t msgType,
	std::uint8_t serviceId) {
	const std::uint32_t n = g_Seen[(static_cast<std::size_t>(msgType) << 8) | serviceId].fetch_add(1) + 1;
	if (n == 1) {
		void* frames[12]{};
		const USHORT captured = RtlCaptureStackBackTrace(1, 12, frames, nullptr);
		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
		std::string trace;
		for (USHORT i = 0; i < captured; ++i) {
			const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
			trace += (base && addr >= base && addr - base < 0x20000000ULL)
				? std::format(" +0x{:X}", addr - base)
				: std::format(" [0x{:X}]", addr);
		}
		LOG("LobbyCensus", INFO, "first request msgType={} service={} tid={} callers:{}", msgType, serviceId,
			::GetCurrentThreadId(), trace.empty() ? " <none captured>" : trace);
	}
	else if (IsReportCount(n)) {
		LOG("LobbyCensus", INFO, "msgType={} service={} built {} times", msgType, serviceId, n);
	}
	return m_Original(buffer, msgType, serviceId);
}
