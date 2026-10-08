#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/arxan_call.hpp"

#include <atomic>
#include <string>

// Reading a Lua error's call stack while it still exists.
//
// The online boot's first frontend error was logged as:
//
//     x64:4be06f4309b61a6f.lua:6052: attempt to index a nil value
//     stack traceback:
//             x64:4be06f4309b61a6f.lua:6052: in function '@&['
//             x64:631c783a3f8851f2.lua:1684: in function 'createMenu'
//             ...
//
// and that is very nearly all the engine is capable of telling us. T9 compiles its LUI chunks with
// local and upvalue names stripped, so Lua's luaG_typeerror cannot append its usual "(field 'foo')"
// / "(global 'bar')" - the parenthetical is missing because the debug info to build it is missing,
// NOT because the failure is uninteresting. The chunk names are hashes for the same reason, and the
// function name renders as three bytes of a hash. So the message is generic by construction, and no
// amount of staring at it distinguishes "the publisher dataset is empty" from any other nil.
//
// The half that IS still there, for one instant, is the call stack. LUI runs its menu builders under
// lua_pcall with luaL_traceback as the error function, and Lua calls the error function BEFORE
// luaD_throw unwinds. So while this detour is on the stack, every frame in that traceback is live,
// and each frame's arguments are sitting in the value stack where lua_getstack can point us at them.
// The frame below the raise is createMenu, and ITS first argument is the menu name.
//
// LuiError_ReportFatal cannot do this: it runs after the pcall returned, when the frames are gone.
// That is why it only ever had a string to log.
//
// Everything here is read-only. lua_getstack and lua_getinfo("nSl") fill a buffer we own and push
// nothing onto the Lua stack ('f' and 'L', the two selectors that DO push, are deliberately never
// asked for), and the argument slots are read with SafeRead. Nothing can run a metamethod or raise -
// which matters more than usual, because a raise from inside an error handler has nowhere to go.

namespace {
	// Bounded on purpose. If the online frontend raises on every widget of every menu it tries to
	// build, an unbounded per-error dump is a frame-time problem and a log we cannot read. The first
	// handful of errors are the ones that matter - after that, count them and stay quiet.
	constexpr int kMaxDetailedDumps = 24;

	std::atomic<int> g_TracebackCount{ 0 };
	std::atomic<bool> g_InDump{ false };

	// The engine calls luaL_traceback with a NULL `msg` — measured, the first run logged
	// "<no message>" for an error that plainly had one. In Lua 5.1 the error object is the value on
	// top of the stack when the error function runs, and the traceback is appended to it afterwards,
	// so the text is there; it is just not in the argument.
	//
	// Read the same way LuiError_ReportFatal reads it, and for the same reason: going through
	// lua_tolstring would mean an Arxan-guarded engine call (and a possible string allocation) on a
	// path that is already handling an error, when the TString layout is known and a bounded byte
	// copy cannot fail.
	std::string ReadErrorMessage(void* L) {
		if (!L) return {};
		auto* const topSlot = reinterpret_cast<std::uint64_t**>(
			reinterpret_cast<std::uint8_t*>(L) + Client::Game::Pointers::kLuaState_Top);

		std::uint64_t* top = nullptr;
		if (!Client::Game::SafeRead(topSlot, top) || !top) return {};

		std::uint64_t value = 0;
		if (!Client::Game::SafeRead(top - 1, value)) return {};
		const int tag = static_cast<int>(
			static_cast<std::int64_t>(value) >> Client::Game::Pointers::kLuaTagShift);
		if (tag != Client::Game::Pointers::kLuaTag_String) return {};

		const auto ts = reinterpret_cast<const std::uint8_t*>(
			value & Client::Game::Pointers::kLuaPayloadMask);
		std::uint32_t len = 0;
		if (!Client::Game::SafeRead(ts + Client::Game::Pointers::kLuaTString_Len, len)) return {};
		if (len > 512) len = 512;

		std::string out;
		out.reserve(len);
		for (std::uint32_t i = 0; i < len; ++i) {
			char c = 0;
			if (!Client::Game::SafeRead(ts + Client::Game::Pointers::kLuaTString_Data + i, c)) break;
			if (c == '\0') break;
			out.push_back(c);
		}
		return out;
	}

	// What ar.name is set to on the broken path. Static storage with a lifetime longer than any
	// traceback, because the engine keeps the pointer and formats it later.
	constexpr const char* kHashedNamePlaceholder = "<hashed>";

	std::atomic<int> g_ObjNameRepairs{ 0 };

	// Calling the engine's traceback is only safe once luaG_getobjname is repaired: unrepaired, it
	// runs "%s" over an uninitialised stack slot and takes the process down. If the repair did not
	// install, skip it and say so.
	//
	// Skipping is a behaviour the engine already produces on its own - luaL_traceback's Arxan caller
	// guard makes it return having done nothing whenever the check fails, so every caller already
	// copes with an empty traceback. What is lost is the "stack traceback:" text appended to the
	// error message, and we log a strictly better stack ourselves a few lines above.
	std::uint64_t CallEngineTraceback(Client::Game::Functions::luaL_tracebackT* original,
		void* buffer, void* luaState, const char* message, int level) {
		if (Client::g_Pointers && !Client::g_Pointers->m_luaG_getobjname) {
			static std::atomic<bool> warned{ false };
			if (!warned.exchange(true)) {
				LOG("LUI", WARN, "Skipping the engine's own traceback: luaG_getobjname is not "
					"hooked on this build, and calling luaL_traceback without that repair formats "
					"an uninitialised pointer and crashes. Our own frame dump above is unaffected.");
			}
			return 0;
		}
		return original(buffer, luaState, message, level);
	}
}

// Repairs ar.name on the one luaG_getobjname return that sets a namewhat without setting a name.
//
// This is a bug fix, not an instrument: with it absent the engine's own luaL_traceback formats
// "%s" over whatever the caller's stack slot happened to hold, which is why earlier dumps showed
// "in function '@&['" and why the online frontend's first Lua error took the process down with an
// access violation reading 0x39. See dump_anchors.hpp for the decompiled chain.
//
// Identification is by POINTER, not by text. The engine has two distinct "xhashfunc" literals and
// only the first returns without writing; matching on the string's characters would also catch the
// sibling return, which resolves the name correctly and must not be overwritten.
//
// luaG_getobjname has no Arxan caller guard (checked in IDA - it opens straight into
// luaF_getlocalname with no return-address test), so the trampoline is called directly.
template <>
const char* Client::Hook::Hooks::HK_luaG_getobjname::hkCallback(
	void* luaState, void* proto, void* pc, unsigned int reg, const char** nameOut) {

	const char* const result = m_Original(luaState, proto, pc, reg, nameOut);

	if (result && nameOut && g_Pointers
		&& result == g_Pointers->m_luaG_getobjname_NoNameResult) {
		*nameOut = kHashedNamePlaceholder;

		// Logged once. This fires on every frame of every hashed-name traceback, so a line per
		// repair would bury the dump it is protecting - but a repair that never announces itself
		// at all is a hook we cannot prove ran, which this project has been caught by before.
		if (g_ObjNameRepairs.fetch_add(1) == 0) {
			LOG("LUI", INFO, "luaG_getobjname: filled in the name the engine leaves uninitialised "
				"on its hashed-name path. Without this the engine's traceback formats an "
				"uninitialised stack slot as a string; further repairs are silent.");
		}
	}

	return result;
}

template <>
std::uint64_t Client::Hook::Hooks::HK_luaL_traceback::hkCallback(
	void* buffer, void* luaState, const char* message, int level) {

	// Re-entering the original through an in-image return address, not directly.
	//
	// luaL_traceback opens with the same Arxan caller check as the Lua accessors: it reads its own
	// return address, and unless that address is inside the game image AND preceded by call-shaped
	// bytes it skips its whole body and returns as if it had run. Calling m_Original the ordinary way
	// from our DLL hands it a return address that fails both halves, which would silently delete the
	// game's own traceback - the very diagnostic this hook exists to extend, removed by the act of
	// extending it. So the trampoline is wrapped once in an ArxanCall thunk and called through that.
	static Client::Game::Functions::luaL_tracebackT* const original = [] {
		auto* const thunk =
			Client::Game::ArxanCall::MakeThunk<Client::Game::Functions::luaL_tracebackT>(
				reinterpret_cast<void*>(m_Original));
		if (!thunk) {
			LOG("LUI", WARN, "luaL_traceback: could not build an Arxan return thunk for the "
				"trampoline. Falling back to a direct call, which the function's own caller check "
				"will reject — expect Lua errors to arrive without the engine's traceback.");
		}
		return thunk ? thunk : m_Original;
	}();

	const int index = g_TracebackCount.fetch_add(1) + 1;

	// Logged on EVERY path, including the ones we do not detail. A hook that only speaks when it has
	// something to say cannot be told apart from a hook that never fired, and this project has been
	// caught by that before.
	std::string text = message ? message : std::string{};
	if (text.empty()) text = ReadErrorMessage(luaState);
	const char* const msg = text.empty() ? "<no message on the stack either>" : text.c_str();

	if (index > kMaxDetailedDumps) {
		LOG("LUI", WARN, "Lua error #{} (detail capped at {}): {}", index, kMaxDetailedDumps, msg);
		return CallEngineTraceback(original, buffer, luaState, message, level);
	}

	// The dump reads the same lua_State the error is being raised on. It cannot raise, but if a
	// future change makes it able to, re-entering here would recurse until the stack is gone.
	bool expected = false;
	if (!g_InDump.compare_exchange_strong(expected, true)) {
		return CallEngineTraceback(original, buffer, luaState, message, level);
	}

	std::string frames;
	if (g_Pointers) {
		frames = g_Pointers->LuaDescribeErrorFrames(luaState);
	}
	else {
		frames = "    <Pointers not up>\n";
	}
	g_InDump.store(false);

	// One record, not one line per frame interleaved with whatever else is logging this frame.
	LOG("LUI", WARN, "Lua error #{}: {}\n  live call stack (arg[0] of a createMenu frame is the "
		"menu being built):\n{}", index, msg, frames);

	return CallEngineTraceback(original, buffer, luaState, message, level);
}
