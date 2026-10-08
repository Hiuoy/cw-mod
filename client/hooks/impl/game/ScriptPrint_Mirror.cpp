#include "common.hpp"
#include "hooks/hook.hpp"
#include "overlay/menu.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>

// Script print mirror. A script's iprintln / iprintlnbold of plain text reaches the screen as an empty box
// (2026-09-26), so the payload the server builds is copied here into client.log and onto the overlay.
//
// Scr_ConstructMessageString (kDump_Scr_ConstructMessageString) writes out = {u32 length; char text[1024]},
// one segment per script arg, each led by a code byte:
//   0x12 plain text (an entity arg gives its name + "^7"), 0x13 int, 0x15 float,
//   0x10 a localized key that exists (its name follows), 0x11 a hash with no localized string.
// The log line keeps the codes visible (<12>) so a bad payload can be told from a bad renderer; the overlay
// gets the readable text, and only for prints that carry script-built text and no localized key. A print
// with a key is the game's own and the game draws it itself, even when an int or a name follows the key
// (2026-09-26: "<10>key<13>int" prints reached the overlay as "[loc] ????", the key being binary).

namespace {
	constexpr int kMaxLogLines = 2000;
	std::atomic<int> g_Logged{ 0 };

	bool IsCode(unsigned char c) {
		return c >= 0x10 && c <= 0x16;
	}
}

// The server's script thread. Everything after the original is a read of the buffer it filled.
template <>
void Client::Hook::Hooks::HK_Scr_ConstructMessageString::hkCallback(int inst, std::uint32_t* out,
	std::uint32_t firstParam, int count) {
	m_Original(inst, out, firstParam, count);
	if (!out) {
		return;
	}

	const std::uint32_t length = std::min<std::uint32_t>(out[0], 1023);
	const auto* text = reinterpret_cast<const unsigned char*>(out + 1);
	std::string logged, shown;
	bool scriptText = false, localized = false;
	for (std::uint32_t i = 0; i < length && text[i]; ++i) {
		const unsigned char c = text[i];
		if (IsCode(c)) {
			logged += std::format("<{:02X}>", c);
			scriptText |= c == 0x12 || c == 0x13 || c == 0x15;
			localized |= c == 0x10 || c == 0x11;
			continue;
		}
		if (c == '^' && i + 1 < length && text[i + 1] >= '0' && text[i + 1] <= '9') {
			logged += "^";
			++i;
			logged += static_cast<char>(text[i]);
			continue; // colour codes stay in the log, not on the overlay
		}
		if (c < 0x20) {
			logged += std::format("<{:02X}>", c);
			continue;
		}
		logged += static_cast<char>(c);
		shown += static_cast<char>(c);
	}

	if (g_Logged.fetch_add(1, std::memory_order_relaxed) < kMaxLogLines) {
		LOG("Script", INFO, "print: \"{}\" ({} bytes)", logged, length);
	}
	if (scriptText && !localized) {
		Client::Overlay::Menu::PushScriptMessage(std::move(shown));
	}
}
