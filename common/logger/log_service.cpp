#include "common_core.hpp"
#include "logger/log_service.hpp"
#include "utility/utility.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace Common {
	namespace {
		std::mutex g_LogFileLock;
		std::ofstream g_LogFile;

		// Alongside DwNet's dw_journal.txt, in the working directory the game already writes to.
		std::filesystem::path LogFilePath() {
			std::error_code ec;
			const std::filesystem::path dir = std::filesystem::current_path(ec) / "cw-mod";
			std::filesystem::create_directories(dir, ec);
			return dir / "client.log";
		}
	}

	// APPEND, never truncate, with a banner per process. Truncating would be tidier but it throws
	// away the previous boot, and comparing a run against the one before it is most of how anything
	// here gets established. The last banner marks the current boot.
	void LogFileWrite(const std::string& line) {
		std::lock_guard<std::mutex> lock(g_LogFileLock);
		if (!g_LogFile.is_open()) {
			return;
		}
		// Flushed per line on purpose: a boot that dies still keeps everything up to the fault, which
		// is exactly the line you want when it does.
		g_LogFile << line << '\n';
		g_LogFile.flush();
	}

	LogService::LogService() {
		AllocConsole();
		freopen_s((FILE**)(stdout), "CONOUT$", "w", stdout);
		SetConsoleTitleA("t6-mod");
		SetConsoleOutputCP(CP_UTF8);

		Utility::EnsureVTL();

		std::lock_guard<std::mutex> lock(g_LogFileLock);
		g_LogFile.open(LogFilePath(), std::ios::out | std::ios::app);
		if (g_LogFile.is_open()) {
			struct tm localTime;
			const time_t timeSinceEpoch = std::time(nullptr);
			localtime_s(&localTime, &timeSinceEpoch);
			g_LogFile << std::format("\n===== session start {}-{:0>2}-{:0>2} {:0>2}:{:0>2}:{:0>2} =====\n",
				localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
				localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
			g_LogFile.flush();
		}
	}

	LogService::~LogService() {
		{
			std::lock_guard<std::mutex> lock(g_LogFileLock);
			if (g_LogFile.is_open()) {
				g_LogFile.flush();
				g_LogFile.close();
			}
		}
		fclose(stdout);
		FreeConsole();
	}

	Logger LogService::GetLogger(std::string name) {
		return Logger(name);
	}
}
