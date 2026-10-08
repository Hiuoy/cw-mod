#include "common.hpp"
#include "hooks/hook.hpp"
#include "scripting/scripting.hpp"

#include <cstring>
#include <format>
#include <string>

template <>
void Client::Hook::Hooks::HK_BB_Alert::hkCallback(const char* type, const char* msg) {
	// Mirror every alert before we touch it. The "Sail 630 Nuclear Bug" substitution below has been
	// here since long before online mode, so when that joke text turned up in a fatal EXIT TO DESKTOP
	// dialog on an online boot it was tempting to read it as the cause. It is not evidence of anything
	// yet — BB_Alert is a REPORTING call, and nothing here proves it is what raises the dialog.
	// Logging type + the ORIGINAL message is what distinguishes "this alert is the fatal" from "this
	// alert is a bystander that happens to be loud".
	//
	// UPDATE after the 14:08 log. The mirror answered the first question and replaced it with a
	// sharper one: type='err_drop', msg='An error occurred: Sail 630 Nuclear Bug'. So the joke text
	// is a red herring twice over — the substitution fires because the ORIGINAL already contained
	// that string, i.e. it is the engine's own message, and BB_Alert is only reporting a drop that
	// something else raised. ErrorQueue_Push stayed silent through the whole boot, which (now that
	// it logs every level) is positive evidence the queue is not involved: this is a direct
	// Com_Error at ERR_DROP.
	//
	// Which leaves exactly one thing worth knowing: WHO called it. So capture the return chain.
	// Frames are printed as module RVAs because that is the form dump_anchors.hpp and the IDB both
	// speak — the top in-module frame can be pasted straight into IDA. The thread id is here for the
	// same reason: the mod's force-offline block runs on its own worker, so "same thread as us" and
	// "the game's main thread" are distinguishable without any further guessing.
	{
		void* frames[16]{};
		const USHORT captured = RtlCaptureStackBackTrace(1, 16, frames, nullptr);
		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));

		std::string trace;
		for (USHORT i = 0; i < captured; ++i) {
			const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
			trace += (base && addr >= base && addr - base < 0x20000000ULL)
				? std::format(" +0x{:X}", addr - base)
				: std::format(" [0x{:X}]", addr);
		}

		LOG("BB_Alert", WARN, "type='{}' msg='{}' tid={} callers:{}",
			type ? type : "<null>", msg ? msg : "<null>", ::GetCurrentThreadId(),
			trace.empty() ? " <none captured>" : trace);
	}

	// A script error drops the match with an obfuscated code ("November 406 Cut Rain" was our level
	// script's clientfield type string not being the engine's "int"), so record the state of our
	// scripts' string ids at the moment of the drop.
	if (type && std::strcmp(type, "err_drop") == 0) Client::Scripting::LogStringDiagnostics("err_drop");

	if (msg && strstr(msg, "Sail 630 Nuclear Bug") != nullptr) {
		std::string msgPatched = "";
		msgPatched += "Hello HelloHelloHelloHello ^3cw-mod^7!\n";
		msgPatched += "\n";
		msgPatched += "This is ^1not^7 an error message.\n";
		msgPatched += "\n";
		msgPatched += "g3s\n";
		msgPatched += "\n";
		msgPatched += "- ^1Kinghunt\n";

		auto msgPtr = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(msg));

		memset(msgPtr, 0, 4096);
		memcpy(msgPtr, msgPatched.data(), msgPatched.size());
	}

	return m_Original(type, msg);
}