#include "common_core.hpp"
#include "logger/console.hpp"
#include "logger/log_service.hpp"
#include "utility/utility.hpp"

#include <fstream>

namespace Common {
	// Persistent log sink: every console line is also appended (ANSI stripped) to cw-mod.log in the
	// game's working directory. Survives the console window closing on a crash, so the last line
	// written pinpoints exactly how far init got. Flushed per line.
	static void WriteToLogFile(const std::string& text) {
		static std::ofstream file("cw-mod.log", std::ios::out | std::ios::trunc);
		if (!file.is_open()) {
			return;
		}

		std::string clean;
		clean.reserve(text.size());
		for (std::size_t i = 0; i < text.size(); i++) {
			if (text[i] == '\x1b') { // skip an ANSI escape: ESC '[' ... final byte in @-~
				i++;
				if (i < text.size() && text[i] == '[') {
					i++;
					while (i < text.size() && !(text[i] >= '@' && text[i] <= '~')) {
						i++;
					}
				}
				continue;
			}
			clean.push_back(text[i]);
		}

		file << clean << '\n' << std::flush;
	}

	HANDLE Console::GetOutputHandle() {
		return GetStdHandle(STD_OUTPUT_HANDLE);
	}

	DWORD Console::GetConsoleMode() {
		DWORD mode = 0;
		::GetConsoleMode(this->GetOutputHandle(), &mode);
		return mode;
	}

	void Console::SetConsoleMode(DWORD consoleMode) {
		::SetConsoleMode(this->GetOutputHandle(), consoleMode);
	}

	void Console::SetCursorPos(Vector2<int> pos) {
		SetConsoleCursorPosition(this->GetOutputHandle(), { static_cast<short>(pos.GetX()), static_cast<short>(pos.GetY()) });
	}

	Vector2<int> Console::GetCursorPos() {
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (GetConsoleScreenBufferInfo(this->GetOutputHandle(), &csbi)) {
			return { csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y };
		}

		return { -1, -1 };
	}

	std::string Console::GetTextAtPos(Vector2<int> pos, int length) {
		if (length <= 0) {
			return "";
		}

		short lengthSh = static_cast<short>(length);
		Vector2<short> posSh = pos.As<short>();

		CHAR_INFO* buffer = new CHAR_INFO[length];
		SMALL_RECT readRegion = { posSh.GetX(), posSh.GetY(), posSh.GetX() + lengthSh - 1, posSh.GetY() };
		ReadConsoleOutputA(this->GetOutputHandle(), buffer, { lengthSh, 1 }, { 0, 0 }, &readRegion);

		std::string result;
		for (int i = 0; i < length; ++i) {
			result += buffer[i].Char.AsciiChar;
		}

		delete[] buffer;
		return result;
	}

	void Console::InputThread() {
		while (true) {
			std::string input;
			std::getline(std::cin, input);
			if (g_Console.m_OnInput) {
				g_Console.m_OnInput(input);
			}
		}
	}

	void Console::StartInputThread() {
		std::thread inputThread(Console::InputThread);
		inputThread.detach();
	}

	void Console::Write(std::string text) {
		std::lock_guard lock(m_Mutex);
		WriteToLogFile(text);
		this->SetConsoleMode(this->GetConsoleMode() | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

		Vector2<int> cursorPos = this->GetCursorPos();
		Vector2<int> targetCursorPos = { 0, cursorPos.GetY() };
		std::string currentInput = this->GetTextAtPos(targetCursorPos, cursorPos.GetX());
		this->SetCursorPos(targetCursorPos);
		std::cout << text << std::flush << std::endl;
		std::cout << currentInput << std::flush;
	}
}
