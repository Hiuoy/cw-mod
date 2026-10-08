#pragma once
#include "scripting/cw_gsc.hpp"

#include <cstdint>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// GSC script loader.
//
// Every compiled script (*.gscc, any file name, any subfolder) under <game>/cw-mod/scripts is
// loaded at boot and used in the next match. A script is used in one of two ways, decided per
// match:
//
//   REPLACE  its name is a stock script's name, so the engine asks for it and gets ours instead.
//            Compile with `--name scripts/zm_common/foo.gsc` (the .gsc is part of the hash), or
//            name the file after the hash: 0x<16 hex digits>.gscc.
//   INJECT   anything else. The engine never asks for a name it does not know, so we add the
//            script to the include table of a ZM script every match loads (the "host",
//            scripts/zm_common/zm_utility.gsc). The engine then loads, links and runs it like
//            any other include. A script compiled without --name gets a name made from its path.
//
// Client scripts (*.cscc, ACTS --name-client) are REPLACE only: there is no client host to inject
// into. So are a mapkit map's own scripts, in <game>/cw-mod/maps/<map>/scripts (loaded when
// "custom_maps" is on): the level scripts cwlink ships with a map, which stand in for the stock map's
// (scripts/zm/zm_silver.gsc and .csc, ...) and must never be injected into another map.
//
// All of it rides one detour on DB_FindXAssetHeader: scriptparsetree requests for our names
// get our buffers; a request for the host gets a copy of the stock host with our includes
// appended. The engine's own linker does everything else. Nothing here calls the linker.
//
// Two things the engine does not do for a buffer that did not come from a fastfile, done here:
//   - String literals. The engine interns object strings at fastfile load, not at link, so we
//     intern each one (SL_GetString, through an Arxan thunk) and write its id into every
//     GetString operand before the buffer is first served.
//   - Lazy function references (&ns::fn). The stock VM handler for opcode 0x13 is an empty stub;
//     ours (a port of ate47's acts-bocw) is patched into gVmOpJumpTable when one of our scripts is first served.
//
// On by default. `"scripts": false` in cw-mod.json turns it off at boot; the Scripts tab can turn it on/off.
// -----------------------------------------------------------------------------

namespace Client::Scripting {
	// Resolve every signature. Reads memory only; safe to call more than once.
	void Initialize();

	// Boot-time install, called once from PostArxanDetectionHooks: loads the folder, hooks
	// DB_FindXAssetHeader (LazyLink is patched on first serve), unless "scripts" is false in cw-mod.json.
	void AutoInstall();

	// --- loader ---
	enum class NameSource : std::uint8_t {
		Embedded,     // the name ACTS compiled in (--name)
		FileHash,     // file named 0x<hash>.gscc: always REPLACE that hash
		FromPath,     // compiled without a name (or a duplicate): made from the file's path
	};
	enum class Mode : std::uint8_t {
		Pending,      // decided when the next match loads
		Inject,
		Replace,
	};
	struct ScriptInfo {
		std::string file;              // path relative to the scripts folder
		std::uint64_t name = 0;        // the hash we serve it under
		NameSource source = NameSource::Embedded;
		Mode mode = Mode::Pending;
		std::uint32_t bytes = 0;
		std::uint16_t strings = 0;     // string literals in the object
		bool stringsReady = false;     // interned and written into the bytecode
		std::uint64_t served = 0;      // times the engine asked for it and got ours
		bool valid = false;
		std::string error;             // why it was rejected, when !valid
	};
	struct HostInfo {
		std::string path;              // e.g. scripts/zm_common/zm_utility.gsc
		std::uint64_t name = 0;
		std::uint32_t injected = 0;    // includes appended to the copy currently served
		std::uint64_t served = 0;      // times the copy was handed out
		bool skippedDuplicate = false; // stock host was linked outside our hook: injection skipped
	};

	bool LoaderActive();
	std::string EnableLoader();       // game thread: load folder, hook, patch LazyLink
	std::string DisableLoader();      // game thread: unhook, restore LazyLink. Not mid-match.
	std::string ReloadScripts();      // re-read the folder; applied once no script of ours is linked
	bool ReloadPending();
	std::vector<ScriptInfo> Scripts();
	std::vector<HostInfo> Hosts();
	std::string ScriptsFolder();

	// --- diagnostics ---
	struct SignatureStatus {
		std::string name;
		bool resolved = false;
		bool isData = false;
		std::uintptr_t address = 0;
		std::uintptr_t rva = 0;
	};
	const std::vector<SignatureStatus>& Signatures();
	bool AllResolved();

	// Log whether the string ids in our served scripts are the ones the engine's own lookups return, next
	// to the engine's "int" constant. Any thread; called on every err_drop while the loader is on.
	void LogStringDiagnostics(const char* why);

	bool LazyLinkInstalled();
	std::uint64_t LazyLinkCallCount();
	std::uint64_t LazyLinkMisses();   // lazy references that resolved to nothing

	// Every scriptparsetree name the engine asked for since capture started, with a count.
	// The list of stock names a REPLACE script can target (hashes only; the engine has no names).
	struct CapturedName {
		std::uint64_t nameHash = 0;
		std::uint64_t count = 0;
		bool ours = false;
	};
	bool CaptureEnabled();
	void SetCapture(bool on);
	void ClearCapture();
	std::vector<CapturedName> CapturedNames();

	// Snapshot the decrypted module to <game>/bocw_dump.bin for offline IDA (tools/dump_fixup.py).
	std::string DumpDecryptedModule();

	// Write every loaded LUI chunk (the luafile pool) to <game>/cw-mod/lua_dump/<asset hash>.luac, as
	// the raw bytecode CoDLuaDecompiler takes. Files already there are kept. For the Lua the memory
	// export missed (DirectorUtility). Any thread; reads memory only.
	std::string DumpLuaFiles();

	// Whether a LUI chunk with this asset name (HashScriptName of "<path>.lua") is in the luafile pool:
	// 1 yes, 0 no, -1 the pool did not resolve. Any thread; reads memory only.
	int LuaFileLoaded(std::uint64_t nameHash);

	// FNV-1a 64 masked to 63 bits, lowercase: the engine's script-name hash.
	std::uint64_t HashScriptName(const std::string& path);
}
