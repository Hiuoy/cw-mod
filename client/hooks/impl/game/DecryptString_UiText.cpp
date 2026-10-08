#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/settings.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

// UI text replacement: "ui_text" in cw-mod.json, { "text the game shows": "text to show instead" }.
//
// Every localized string (asset type 0x1D) is stored encrypted, and every reader, Lua's
// Engine.Localize and the ~100 native sites alike, hands it to DecryptString before using it (see
// kDump_DecryptString). So the text that comes OUT of DecryptString is the one place where every UI
// string can be recognised and swapped, whoever asked for it.
//
// Matching is on the whole string with ASCII case ignored: menus often upper-case a string after
// localizing it ("CONNECTING" on screen can be "Connecting" in the asset), so what the player reads
// off the screen still matches. "ui_text_log": true lists each distinct string once as a (UiText)
// line, which is how to find the exact text when a key does not match.
//
// A replacement must keep the original's "&&1"-style placeholders if it has any; the caller fills
// them in after this returns.

namespace {
	struct Entry {
		std::string from;
		std::string to;
		std::atomic<int> hits{ 0 };
	};

	constexpr int kMaxLogLines = 4000;

	// Built by BuildUiTextTable before the detour is enabled and never modified afterwards, so the
	// game's threads read it without a lock.
	std::unordered_map<std::uint64_t, Entry> g_Table;
	bool g_Log = false;

	std::mutex g_LogLock;
	std::unordered_set<std::uint64_t> g_Logged;

	char Fold(char c) {
		return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
	}

	// FNV-1a over the case-folded bytes, and the length on the way, so a lookup allocates nothing.
	std::uint64_t FoldHash(const char* s, std::size_t& len) {
		std::uint64_t h = 0xCBF29CE484222325ULL;
		len = 0;
		for (; s[len]; ++len) {
			h = (h ^ static_cast<unsigned char>(Fold(s[len]))) * 0x100000001B3ULL;
		}
		return h;
	}

	bool EqualFold(const char* a, const std::string& b) {
		for (std::size_t i = 0; i < b.size(); ++i) {
			if (Fold(a[i]) != Fold(b[i])) {
				return false;
			}
		}
		return true;
	}

	// One line per string, so the log stays greppable.
	std::string Escape(const char* s) {
		std::string out;
		for (; *s; ++s) {
			if (*s == '\n') out += "\\n";
			else if (*s == '\r') out += "\\r";
			else if (*s == '\t') out += "\\t";
			else if (*s == '"') out += "\\\"";
			else out += *s;
		}
		return out;
	}
}

void Client::Hook::BuildUiTextTable() {
	const auto& settings = Client::Game::Settings::Get();
	g_Log = settings.uiTextLog;

	for (const auto& [from, to] : settings.uiText) {
		// DecryptString decrypts anything starting with 0x80-0xBF. A replacement starting there would
		// be scrambled if the engine ever ran it through again. It is not valid UTF-8 anyway.
		if (!to.empty() && (static_cast<unsigned char>(to[0]) & 0xC0) == 0x80) {
			LOG("UiText", ERROR, "\"{}\": the replacement starts with a UTF-8 continuation byte; skipped.", from);
			continue;
		}
		std::size_t len = 0;
		const std::uint64_t h = FoldHash(from.c_str(), len);
		auto [it, added] = g_Table.try_emplace(h);
		if (!added) {
			LOG("UiText", WARN, "\"{}\" is listed twice (case is ignored); keeping \"{}\".", from, it->second.to);
			continue;
		}
		it->second.from = from;
		it->second.to = to;
	}
	LOG("UiText", INFO, "{} UI text replacement(s) loaded{}.", g_Table.size(),
		g_Log ? "; listing every UI string once (ui_text_log)" : "");
}

// Any thread (asset loads, the main thread, LUI). Everything after the original is a read of the
// string it returned; nothing here writes to engine memory.
template <>
char* Client::Hook::Hooks::HK_DecryptString::hkCallback(char* s) {
	char* const out = m_Original(s);
	if (!out) {
		return out;
	}

	std::size_t len = 0;
	const std::uint64_t h = FoldHash(out, len);

	Entry* hit = nullptr;
	if (const auto it = g_Table.find(h); it != g_Table.end()
		&& it->second.from.size() == len && EqualFold(out, it->second.from)) {
		hit = &it->second;
	}

	if (g_Log) {
		std::lock_guard<std::mutex> lock(g_LogLock);
		if (g_Logged.size() < kMaxLogLines && g_Logged.insert(h).second) {
			LOG("UiText", INFO, "\"{}\"{}", Escape(out), hit ? " (replaced)" : "");
			if (g_Logged.size() == kMaxLogLines) {
				LOG("UiText", WARN, "ui_text_log: {} strings listed, stopping here.", kMaxLogLines);
			}
		}
	}

	if (!hit) {
		return out;
	}
	if (hit->hits.fetch_add(1, std::memory_order_relaxed) == 0) {
		LOG("UiText", INFO, "\"{}\" -> \"{}\"", Escape(out), Escape(hit->to.c_str()));
	}
	return hit->to.data();
}
