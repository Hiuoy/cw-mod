#pragma once
#include "common_core.hpp"
#include "logger/console.hpp"
#include "logger/log_levels.hpp"

#include <format>

namespace Common {
	// Append one already-formatted, ANSI-free line to <cwd>/cw-mod/client.log.
	//
	// Declared here rather than in log_service.hpp because that header includes THIS one; defined in
	// log_service.cpp next to the stream it writes.
	//
	// WHY A FILE AT ALL. Until now every LOG line went to the AllocConsole window and nowhere else,
	// so the login status sequence — the primary result of a test boot — lived only in a scrollback
	// buffer that had to be selected and copied before the game closed, and was lost outright if the
	// boot ended in an ERR_DROP. Same reasoning that already put DwNet's journal on disk: the run
	// most worth measuring is the run most likely to take its own findings down with it.
	void LogFileWrite(const std::string& line);

	class Logger {
	public:
		Logger(std::string logName) {
			this->m_LogName = logName;
		}

		template <typename... Args>
		void Print(LogLevel level, const std::string_view& format, Args const &...args) {
			std::string caller = "<unknown module>";

			auto message = std::vformat(format, std::make_format_args(args...));

			struct tm localTime;
			const time_t timeSinceEpoch = std::time(nullptr);
			localtime_s(&localTime, &timeSinceEpoch);

			auto consoleTimestamp = std::format("[{:0>2}:{:0>2}:{:0>2}]", localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
			auto fileTimestamp = std::format("[{}-{}-{} {:0>2}:{:0>2}:{:0>2}]", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
				localTime.tm_hour, localTime.tm_min, localTime.tm_sec);

			g_Console.Write(std::format(ANSI_FG_CYAN "{} " ANSI_RESET "{}[{}{}] " ANSI_RESET ANSI_FG_RGB(0, 163, 163) "({}) " ANSI_RESET "{}" ANSI_RESET,
				consoleTimestamp, level.GetAnsiColor(), caller.compare("<unknown module>") ? (caller + "/").c_str() : "", level.GetLabel(), this->m_LogName, message));

			// The same line, with the full date and no colour escapes, so the file greps cleanly.
			// fileTimestamp was already being computed here and thrown away.
			LogFileWrite(std::format("{} [{}] ({}) {}",
				fileTimestamp, level.GetLabel(), this->m_LogName, message));
		}
	private:
		std::string m_LogName{};
	};
}
