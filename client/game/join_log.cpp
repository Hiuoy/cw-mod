// The shared join/netmsg transcript: one append-only log written by the capture detours,
// the join watcher and the descriptor join, read back by the menu.
#include "common.hpp"
#include "game/game.hpp"
#include "game/join_log.hpp"

#include <fstream>
#include <mutex>

namespace Client::Game {
	namespace {
		std::mutex  g_jcLogMutex;
		std::string g_jcLog;
	}

	// Wall clock, not elapsed: a PC1 and a PC2 transcript are only interleavable on absolute time.
	std::string WallClockNow() {
		SYSTEMTIME st{};
		GetLocalTime(&st);
		return std::format("{:02}:{:02}:{:02}.{:03}", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	}

	const char* JoinStateLabel(int s) {
		switch (s) {
		case 0: return "idle";
		case 1: return "pick-candidate";
		case 2: return "send-JoinLobby";
		case 3: return "await-JoinResponse";
		case 4: return "await-agreement";
		case 5: return "next-candidate";
		case 6: return "JOIN COMPLETE";
		case 7: return "teardown";
		default: return "?";
		}
	}

	void JcAppend(const std::string& line) {
		{
			std::lock_guard<std::mutex> lock(g_jcLogMutex);
			g_jcLog += line;
			g_jcLog += '\n';
			if (g_jcLog.size() > 16384) {
				g_jcLog.erase(0, g_jcLog.size() - 16384);
			}
		}
		std::ofstream f("client_join_args.txt", std::ios::app);
		if (f) { f << line << '\n'; }
		LOG("Pointers", INFO, "join-capture: {}", line);
	}

	std::string Pointers::ClientJoinCaptureLog() const {
		std::lock_guard<std::mutex> lock(g_jcLogMutex);
		return g_jcLog.empty()
			? std::string("(nothing captured yet — install, then join a game normally)")
			: g_jcLog;
	}
}
