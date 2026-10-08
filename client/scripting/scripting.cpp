#include "common.hpp"
#include "scripting/scripting.hpp"
#include "memory/memory.hpp"
#include "memory/minhook.hpp"
#include "game/arxan_call.hpp"
#include "game/mapkit_loader.hpp"
#include "game/mapkit_usage.hpp"
#include "game/settings.hpp"

#include <utility/nt.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <intrin.h>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The GSC loader. What it does and why is in scripting.hpp; this file is the how.
//
// Provenance: the DB_FindXAssetHeader, gVmOpJumpTable, gObjFileInfo and xassetpool patterns and
// the LazyLink handler are ate47's (atian-cod-tools, src/dll/bocw-dll, MIT). The string fixup,
// include injection and SL_GetString pattern are ours, from the IDB (2026-09-23): the engine's
// GetString handler (VM_OP_GetString) reads a 4-aligned u32 SL id, and nothing on the link path
// writes one, so a buffer that skipped fastfile load carries ACTS's 0x12345678 placeholder.

namespace Client::Scripting {
	namespace {
		// --- resolved addresses ---
		CW::DB_FindXAssetHeaderT g_DbFindTarget{};
		void** g_VmTable{};
		CW::ObjFileInfoStruct* g_ObjInfo{};          // [2][800]
		std::uint32_t* g_ObjInfoCount{};             // [2]
		CW::XAssetPool* g_Pools{};
		void* g_SlGetStringTarget{};
		const std::uint32_t* g_ScrConstInt{};        // diagnostics only
		void* g_CfShutdownTarget{};                  // diagnostics only
		const std::uint32_t* g_GameTlsIndex{};       // diagnostics only
		std::uintptr_t g_ServerVmCtx = 0;            // diagnostics only
		std::uintptr_t g_ModuleBase = 0;
		std::uintptr_t g_ModuleSize = 0;
		std::vector<SignatureStatus> g_Report;
		std::once_flag g_InitOnce;

		constexpr const char* SIG_DB_FindXAssetHeader = "48 89 74 24 ? 55 57 41 54 41 56 41 57 48 8D AC 24 80 D0";
		constexpr const char* SIG_gVmOpJumpTable      = "41 FF 94 FC ? ? ? ? 80 7C 24";          // disp32 @+4, from image base
		constexpr const char* SIG_gObjFileInfo        = "4C 8D 2D ? ? ? ? 48 8D 15 ? ? ? ? 43 39 4C B5 00"; // count @+3, table @+10
		constexpr const char* SIG_xassetpool          = "48 8D 05 ? ? ? ? 48 C1 E2 ? 48 03 D0";  // lea @+3
		// Inside SL_GetString (IDB SL_GetString_Guarded, dump 0x7FF71E783780): the user/type/decrypt
		// argument moves, the 0x80 header test and the FNV offset basis. Function start = match - 0x2F.
		constexpr const char* SIG_SL_GetString        = "45 0F B6 E9 44 89 44 24 20 44 0F B6 E2 48 8B F1 33 DB 45 84 C9 75 ? 0F B6 01 24 C0 3C 80 74 ? 48 BA 25 23 22 84 E4 9C F2 CB";
		constexpr std::uintptr_t kSlGetStringSigOffset = 0x2F;
		// SL_GetString loads the string pool's base at +0x23F with `sub rax, [rip+disp32]` (48 2B 05). The
		// pool is 16-byte slots; an entry's u64 name hash sits at +16.
		constexpr std::uintptr_t kSlPoolBaseLoad = 0x23F;
		constexpr std::size_t kSlEntryHash = 16;
		// SL_GetString's `lea rcx, [rip+disp32]` (48 8D 0D) of the bucket heads at +0x1B0: u32 entry ids indexed
		// by the low 16 bits of the hash, each chain linked through an entry's +8.
		constexpr std::uintptr_t kSlBucketsLoad = 0x1B0;
		// The clientfield type check (IDB ClientField_TypeFromString, dump 0x7FF727A8BE60) opens with `cmp ecx,
		// [rip+disp32]` against the engine's own "int" (scr_const, which Scr_InitConstStrings sets on every
		// SV_SpawnServer). Diagnostics only: it tells whether the "int" we intern is the engine's.
		constexpr const char* SIG_ClientFieldType = "48 83 EC 28 3B 0D ? ? ? ? 74 ? 3B 0D ? ? ? ? 75 ? B0 01 48 83 C4 28 C3";
		// An entry's flags byte (+2): bits 0-5 are users, 0x80 = text stored encrypted, and 0x40 = every lookup
		// skips it (SL_GetString's `test byte ptr [rbx+2], 40h`), so an id we wrote earlier that now carries it
		// no longer equals the engine's copy of the text.
		constexpr std::uint8_t kSlSkipped = 0x40;

		// Clientfield diagnostics (IDB 2026-09-26). ClientField_Shutdown (dump 0x7FF727A8D6C0) takes no arguments and
		// erases the CURRENT VM's clientfield table: the client's on disconnect and at CG_Init, the server's in
		// G_ShutdownGame. The current VM is the context at +0x38 of the game's TLS block (the server's is
		// g_serverVmCtx, 0x7FF72E6BBA40); a context's first qword is its table: 13 pools of 4120 bytes, each
		// {+8 i32 count, +16 entry*[512]}, with 64-byte entries (ClientField_Register fills them).
		constexpr const char* SIG_ClientFieldShutdown = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 30 8B 0D ? ? ? ? 65 48 8B 04 25 58 00 00 00 4C 8B 3C C8 41 BD 3C 0C 01 00";  // TlsIndex @+0x1E
		constexpr const char* SIG_ServerVmCtx = "BA 38 00 00 00 48 8D 0D ? ? ? ? 48 39 0C 1A 48 8B 5C 24 20 0F 94";  // ClientField_PoolFromString, lea @+8
		constexpr std::size_t kTlsReady = 0x10C3C;    // TLS block: byte, set once the thread's TLS is initialised
		constexpr std::size_t kTlsVmCtx = 0x38;       // TLS block: the current VM context
		constexpr int kCfPools = 13;
		constexpr std::size_t kCfPoolStride = 4120;
		constexpr int kCfMaxPerPool = 512;

		constexpr std::uint64_t kNameMask = 0x7FFFFFFFFFFFFFFFull;
		constexpr std::size_t kLazyLinkOpcode = 0x13;
		constexpr int kVmProbe = 32;
		constexpr std::uint32_t kMaxObjs = 800;
		constexpr std::uint32_t kStringPlaceholder = 0x12345678;  // what ACTS leaves in a GetString operand
		constexpr std::uint8_t kGscMagic[8] = { 0x80, 0x47, 0x53, 0x43, 0x0D, 0x0A, 0x00, 0x38 };

		// ZM scripts every match loads through an include, never as a top-level script. Our INJECT
		// scripts are appended to their include tables.
		constexpr const char* kHostPaths[] = { "scripts/zm_common/zm_utility.gsc" };

		bool InModule(std::uintptr_t p) {
			return g_ModuleBase && p >= g_ModuleBase && p < g_ModuleBase + g_ModuleSize;
		}

		int CountCodePtrs(std::uintptr_t table) {
			if (!InModule(table) || !InModule(table + static_cast<std::uintptr_t>(kVmProbe) * 8)) return -1;
			int hits = 0;
			const auto* e = reinterpret_cast<const std::uintptr_t*>(table);
			for (int i = 0; i < kVmProbe; ++i) hits += InModule(e[i]);
			return hits;
		}

		void Record(const std::string& name, bool isData, std::uintptr_t addr) {
			SignatureStatus st{ name, addr != 0, isData, addr, addr && g_ModuleBase ? addr - g_ModuleBase : 0 };
			if (st.resolved) LOG("Scripting", INFO, "Resolved '{}' at +0x{:X}", name, st.rva);
			else LOG("Scripting", WARN, "FAILED to resolve '{}' (pattern did not match this build)", name);
			g_Report.push_back(st);
		}

		// A 32-byte aligned, zero-initialised byte buffer. Fastfile script buffers are 0x20-aligned
		// and the compiler pads operands against the object base, so ours are too.
		struct AlignedBuffer {
			std::uint8_t* p = nullptr;
			std::size_t n = 0;
			explicit AlignedBuffer(std::size_t size) : n(size) {
				p = static_cast<std::uint8_t*>(_aligned_malloc(size ? size : 1, 32));
				if (p) std::memset(p, 0, size);
			}
			~AlignedBuffer() { _aligned_free(p); }
			AlignedBuffer(const AlignedBuffer&) = delete;
			AlignedBuffer& operator=(const AlignedBuffer&) = delete;
		};

		// --- VM object table ---
		// Engine link state lives in gObjFileInfo: one entry per linked object, keyed by BUFFER
		// pointer (Scr_GscObjUnlink matches on it). Only read here.
		template <typename F>
		void ForEachLinked(F&& f) {
			if (!g_ObjInfo || !g_ObjInfoCount) return;
			for (int inst = 0; inst < 2; ++inst) {
				const std::uint32_t count = std::min(g_ObjInfoCount[inst], kMaxObjs);
				for (std::uint32_t i = 0; i < count; ++i) {
					if (CW::T9GSCOBJ* obj = g_ObjInfo[inst][i].activeVersion) {
						if (f(obj)) return;
					}
				}
			}
		}

		bool IsLinked(const void* buffer) {
			bool found = false;
			ForEachLinked([&](CW::T9GSCOBJ* obj) { return found = (obj == buffer); });
			return found;
		}

		// IsLinked with a remembered slot (inst * kMaxObjs + index). The host is asked for once per
		// import per includer while a map links, so the common answer must not cost a full scan.
		bool IsLinkedHinted(const void* buffer, int& hint) {
			if (!g_ObjInfo || !g_ObjInfoCount) return false;
			if (hint >= 0) {
				const int inst = hint / static_cast<int>(kMaxObjs);
				const std::uint32_t idx = static_cast<std::uint32_t>(hint) % kMaxObjs;
				if (idx < std::min(g_ObjInfoCount[inst], kMaxObjs) && g_ObjInfo[inst][idx].activeVersion == buffer) {
					return true;
				}
			}
			for (int inst = 0; inst < 2; ++inst) {
				const std::uint32_t count = std::min(g_ObjInfoCount[inst], kMaxObjs);
				for (std::uint32_t i = 0; i < count; ++i) {
					if (g_ObjInfo[inst][i].activeVersion == buffer) {
						hint = inst * static_cast<int>(kMaxObjs) + static_cast<int>(i);
						return true;
					}
				}
			}
			hint = -1;
			return false;
		}

		// A linked object called `name` whose buffer is not `except`: the stock host linked by some
		// path that did not go through our hook. Injecting too would link the host twice.
		bool LinkedElsewhere(std::uint64_t name, const void* except) {
			bool found = false;
			ForEachLinked([&](CW::T9GSCOBJ* obj) {
				return found = (obj != except && (obj->name & kNameMask) == name);
			});
			return found;
		}

		// Is there a stock scriptparsetree called `name` in a loaded fastfile? Free slots chain
		// through their first qword, so a first qword pointing into the pool is a link, not a name.
		bool StockScriptExists(std::uint64_t name) {
			if (!g_Pools) return false;
			const CW::XAssetPool& pool = g_Pools[CW::ASSET_TYPE_SCRIPTPARSETREE];
			const auto base = reinterpret_cast<std::uintptr_t>(pool.pool.ptr);
			if (!base || pool.itemSize < sizeof(CW::ScriptParseTree) || pool.itemCount <= 0) return false;
			const std::uintptr_t end = base + static_cast<std::uintptr_t>(pool.itemSize) * pool.itemCount;
			for (std::uintptr_t e = base; e < end; e += pool.itemSize) {
				const auto* spt = reinterpret_cast<const CW::ScriptParseTree*>(e);
				if (!spt->name || (spt->name >= base && spt->name < end)) continue;
				if ((spt->name & kNameMask) == name && spt->buffer) return true;
			}
			return false;
		}

		// --- LazyLink (opcode 0x13) ---
		std::atomic_bool g_LazyLinkInstalled{ false };
		void* g_OrigLazyLink = nullptr;
		std::atomic<std::uint64_t> g_LazyLinkCalls{ 0 };
		std::atomic<std::uint64_t> g_LazyLinkMisses{ 0 };

		std::uint8_t* FindExportIn(CW::T9GSCOBJ* obj, std::uint32_t nameSpace, std::uint32_t name) {
			auto* exports = reinterpret_cast<CW::T8GSCExport*>(obj->magic + obj->exports_tables);
			for (std::uint16_t i = 0; i < obj->exports_count; ++i) {
				if (exports[i].name == name && exports[i].name_space == nameSpace) {
					return obj->magic + exports[i].address;
				}
			}
			return nullptr;
		}

		// ACTS FindExport: the export in the linked object named `script`. If the compiler's idea of
		// that script's name differs from the engine's (extension or path), fall back to any linked
		// object in this VM exporting ns::name, rather than handing back undefined.
		std::uint8_t* FindExport(CW::ScriptInstance inst, std::uint64_t script, std::uint32_t nameSpace,
				std::uint32_t name) {
			if (!g_ObjInfo || !g_ObjInfoCount || inst >= 2) return nullptr;
			const std::uint32_t count = std::min(g_ObjInfoCount[inst], kMaxObjs);
			CW::ObjFileInfo* infos = g_ObjInfo[inst];
			for (std::uint32_t i = 0; i < count; ++i) {
				CW::T9GSCOBJ* obj = infos[i].activeVersion;
				if (obj && (obj->name & kNameMask) == (script & kNameMask)) {
					if (auto* at = FindExportIn(obj, nameSpace, name)) return at;
					break;
				}
			}
			for (std::uint32_t i = 0; i < count; ++i) {
				if (CW::T9GSCOBJ* obj = infos[i].activeVersion) {
					if (auto* at = FindExportIn(obj, nameSpace, name)) return at;
				}
			}
			return nullptr;
		}

		// Direct port of ACTS VM_OP_LazyLink_Handler. Operand: 4-aligned {u32 ns, u32 name, u64 script}.
		void VM_OP_LazyLink_Handler(CW::ScriptInstance inst, void* /*varInfo*/,
				CW::FunctionStack* fs_0, void* /*ctx*/, bool* /*terminate*/) {
			g_LazyLinkCalls.fetch_add(1, std::memory_order_relaxed);

			struct LazyLinkData {
				std::uint32_t nameSpace;
				std::uint32_t name;
				std::uint64_t script;
			};
			auto* data = reinterpret_cast<LazyLinkData*>(
				(reinterpret_cast<std::uintptr_t>(fs_0->pos) + 3u) & ~std::uintptr_t(3u));
			fs_0->pos = reinterpret_cast<std::uint8_t*>(data + 1);

			std::uint8_t* exp = FindExport(inst, data->script, data->nameSpace, data->name);

			fs_0->top++;
			if (exp) {
				fs_0->top->type = CW::TYPE_SCRIPT_FUNCTION;
				fs_0->top->u.codePosValue = exp;
			} else {
				g_LazyLinkMisses.fetch_add(1, std::memory_order_relaxed);
				fs_0->top->type = CW::TYPE_UNDEFINED;
				fs_0->top->u.intValue = 0;
			}
		}

		std::mutex g_LazyLinkMutex;   // installs come from engine threads (first serve); a second patch would save our own handler as "stock"

		std::string SetLazyLink(bool on) {
			std::lock_guard<std::mutex> lock(g_LazyLinkMutex);
			if (on == g_LazyLinkInstalled.load()) return {};
			if (!g_VmTable) return "gVmOpJumpTable not resolved.";
			if (CountCodePtrs(reinterpret_cast<std::uintptr_t>(g_VmTable)) <= kVmProbe / 2) {
				return "gVmOpJumpTable failed validation (not a run of code pointers); not patching.";
			}
			void** slot = &g_VmTable[kLazyLinkOpcode];
			DWORD oldProt = 0;
			if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
				return "VirtualProtect failed on the VM jump-table slot.";
			}
			if (on) {
				g_OrigLazyLink = *slot;
				*slot = reinterpret_cast<void*>(&VM_OP_LazyLink_Handler);
			} else {
				*slot = g_OrigLazyLink;
			}
			VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
			g_LazyLinkInstalled.store(on);
			LOG("Scripting", INFO, "LazyLink handler {} at gVmOpJumpTable[0x{:X}].", on ? "installed" : "removed",
				kLazyLinkOpcode);
			return {};
		}

		// --- scripts ---
		struct Script {
			std::string file;                       // relative to the scripts folder, '/' separators
			std::unique_ptr<AlignedBuffer> buffer;  // what the engine links; it writes into it
			CW::ScriptParseTree spt{};
			std::uint64_t name = 0;
			NameSource source = NameSource::Embedded;
			std::atomic<Mode> mode{ Mode::Pending };
			std::uint16_t strings = 0;
			std::mutex fixMutex;
			std::atomic_bool stringsReady{ false };
			bool fixFailed = false;
			std::atomic<std::uint64_t> served{ 0 };
			// Never injected: a client script (there is no client host to inject into) or a map's own script
			// (cw-mod/maps/<map>/scripts), which only ever stands in for the stock script of its name.
			bool replaceOnly = false;
			bool valid = false;
			std::string error;

			CW::T9GSCOBJ* Obj() const { return reinterpret_cast<CW::T9GSCOBJ*>(buffer ? buffer->p : nullptr); }
		};
		using ScriptPtr = std::shared_ptr<Script>;

		struct HostCopy {
			std::unique_ptr<AlignedBuffer> buffer;
			std::vector<std::uint8_t> stockSnapshot;   // stock bytes the copy was built from
			const void* stockPtr = nullptr;
			std::uint64_t generation = 0;              // g_Generation it was built for
			std::vector<std::uint64_t> injected;
			CW::ScriptParseTree spt{};
			int linkHint = -1;
		};

		struct Host {
			std::string path;
			std::uint64_t name = 0;
			std::unique_ptr<HostCopy> current;
			std::vector<std::unique_ptr<HostCopy>> retired;   // never freed: the engine may still hold one
			// Last decision was "nothing to inject": serve this stock buffer untouched without re-deciding.
			const void* passStockPtr = nullptr;
			std::uint64_t passGeneration = 0;
			std::atomic<std::uint64_t> served{ 0 };
			std::atomic_bool skippedDuplicate{ false };
		};

		std::mutex g_Mutex;                          // g_Scripts, g_Pending, g_Retired, every Host
		std::uint64_t g_Generation = 1;              // bumped whenever g_Scripts is replaced
		std::vector<ScriptPtr> g_Scripts;
		std::unique_ptr<std::vector<ScriptPtr>> g_Pending;
		std::vector<ScriptPtr> g_Retired;            // never freed: a linked buffer must outlive the link
		std::vector<std::unique_ptr<Host>> g_Hosts;

		std::atomic_bool g_Enabled{ false };
		std::atomic_bool g_Hooked{ false };
		CW::DB_FindXAssetHeaderT g_OrigDbFind = nullptr;     // MinHook trampoline, set before the detour is enabled
		CW::SL_GetStringT g_SlGetString = nullptr;           // Arxan thunk; null = strings unsupported

		std::atomic_bool g_CaptureEnabled{ false };
		std::mutex g_CaptureMutex;
		std::unordered_map<std::uint64_t, std::uint64_t> g_Captured;

		// UTF-8, '/' separators. path::string() throws on names the ANSI code page cannot hold.
		std::string Utf8(const std::filesystem::path& p) {
			const std::u8string u = p.generic_u8string();
			return std::string(u.begin(), u.end());
		}

		std::filesystem::path ScriptsFolderPath() {
			std::error_code ec;
			std::filesystem::path dir = std::filesystem::current_path(ec) / "cw-mod" / "scripts";
			std::filesystem::create_directories(dir, ec);
			return dir;
		}

		// cw-mod/maps/<map>/scripts of every mapkit map folder, when custom maps are on. A map built by cwlink
		// carries its own level scripts there (a map of its own: scripts/zm/<map>.gsc/.csc plus empty stand-ins for
		// its asset library's level scripts; an overlay: replacements for the stock map's, e.g. scripts/zm/zm_silver.gsc);
		// they are live exactly when the map's zones are: a listed map's (it has a map.json) only while it is the map
		// this PC loads (a map of its own, mapkit_loader.hpp 7.: host and client alike) or the one picked in CUSTOM
		// MAPS (an overlay, 6.), any other folder's always.
		std::vector<std::filesystem::path> MapScriptFolders() {
			namespace fs = std::filesystem;
			std::vector<fs::path> out;
			if (!Game::Settings::Get().customMaps) return out;
			std::error_code ec;
			const fs::path maps = fs::current_path(ec) / "cw-mod" / "maps";
			const std::string active = Game::MapKit::ActiveMap();
			for (auto it = fs::directory_iterator(maps, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
				std::error_code dirEc;
				if (!it->is_directory(dirEc) || !fs::is_directory(it->path() / "scripts", dirEc)) continue;
				if (fs::exists(it->path() / "map.json", dirEc) && _stricmp(Utf8(it->path().filename()).c_str(), active.c_str()) != 0) {
					continue;
				}
				out.push_back(it->path() / "scripts");
			}
			std::sort(out.begin(), out.end());
			return out;
		}

		// "0x<hex>" or a bare 16-hex-digit stem: serve under that hash, always as a replacement.
		bool ParseHashFilename(const std::string& stem, std::uint64_t& out) {
			std::string h = stem;
			if (h.size() > 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
			else if (h.size() != 16) return false;
			if (h.empty() || h.size() > 16) return false;
			for (char c : h) {
				if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
			}
			out = std::strtoull(h.c_str(), nullptr, 16) & kNameMask;
			return out != 0;
		}

		bool InBounds(std::size_t len, std::size_t off, std::size_t size) {
			return off <= len && size <= len - off;
		}

		// Structural checks on every table the engine or we will walk. Returns an error or "".
		std::string ValidateObject(const std::uint8_t* d, std::size_t len) {
			if (len < sizeof(CW::T9GSCOBJ)) return "too small to be a compiled script";
			if (std::memcmp(d, kGscMagic, sizeof(kGscMagic)) != 0) return "not a Cold War (VM 38) compiled script";
			const auto* o = reinterpret_cast<const CW::T9GSCOBJ*>(d);
			if (!InBounds(len, o->includes_table, 8ull * o->includes_count)) return "include table out of bounds";
			if (!InBounds(len, o->exports_tables, sizeof(CW::T8GSCExport) * o->exports_count)) return "export table out of bounds";
			std::size_t off = o->string_offset;
			for (std::uint16_t i = 0; i < o->string_count; ++i) {
				if (!InBounds(len, off, sizeof(CW::T8GSCString))) return "string table out of bounds";
				CW::T8GSCString e;
				std::memcpy(&e, d + off, sizeof(e));
				off += sizeof(e);
				if (!InBounds(len, off, 4ull * e.num_address)) return "string refs out of bounds";
				for (std::uint8_t a = 0; a < e.num_address; ++a) {
					std::uint32_t ref;
					std::memcpy(&ref, d + off + 4ull * a, 4);
					if (!InBounds(len, ref, 4)) return "string ref out of bounds";
				}
				off += 4ull * e.num_address;
				if (e.string >= len || !std::memchr(d + e.string, 0, len - e.string)) return "string text out of bounds";
			}
			return {};
		}

		// The string pool's base, read the way SL_GetString reads it; 0 when the instruction is not there.
		std::uintptr_t SlPoolBase() {
			if (!g_SlGetStringTarget) return 0;
			const auto* p = static_cast<const std::uint8_t*>(g_SlGetStringTarget) + kSlPoolBaseLoad;
			if (p[0] != 0x48 || p[1] != 0x2B || p[2] != 0x05) return 0;
			std::int32_t disp = 0;
			std::memcpy(&disp, p + 3, 4);
			return *reinterpret_cast<const std::uintptr_t*>(p + 7 + disp);
		}

		// The bucket heads, read the way SL_GetString reads them; null when the instruction is not there.
		const std::uint32_t* SlBuckets() {
			if (!g_SlGetStringTarget) return nullptr;
			const auto* p = static_cast<const std::uint8_t*>(g_SlGetStringTarget) + kSlBucketsLoad;
			if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x0D) return nullptr;
			std::int32_t disp = 0;
			std::memcpy(&disp, p + 3, 4);
			return reinterpret_cast<const std::uint32_t*>(p + 7 + disp);
		}

		std::uint64_t SlHash(const char* text) {
			std::uint64_t h = 0xCBF29CE484222325ull;
			for (const char* c = text; *c; ++c) {
				h = (h ^ static_cast<std::uint8_t>(*c)) * 0x100000001B3ull;
			}
			return h;
		}

		// An entry's header: +0 refcount, +2 flags (see kSlSkipped), +4 length, +8 next id in the bucket,
		// +16 hash. The text follows at +24.
		struct SlEntry {
			std::uint16_t refs;
			std::uint8_t flags;
			std::uint8_t kind;
			std::uint32_t len;
			std::uint32_t next;
			std::uint32_t pad;
			std::uint64_t hash;
		};
		static_assert(sizeof(SlEntry) == 24);

		bool CopyGuarded(void* dst, const void* src, std::size_t n) {
			__try { std::memcpy(dst, src, n); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		// A stale or garbage id reads as false instead of faulting; the crash logger is told it is a probe.
		bool ReadGuarded(void* dst, const void* src, std::size_t n) {
			++Client::g_SafeProbeDepth;
			const bool ok = CopyGuarded(dst, src, n);
			--Client::g_SafeProbeDepth;
			return ok;
		}

		bool ReadSlEntry(std::uintptr_t pool, std::uint32_t id, SlEntry& e) {
			return pool && id && ReadGuarded(&e, reinterpret_cast<const void*>(pool + 16ull * id), sizeof(e));
		}

		// Is the entry `id` filed under the text's own hash? SL_GetString files it under the complement when
		// its Arxan caller guard trips. Such an id holds the right text but is a private copy: the engine's
		// own string never compares equal to it, so getentarray("script_model", "classname") and === on it
		// find nothing. -1 = could not tell.
		int IsCanonicalStringId(std::uintptr_t pool, std::uint32_t id, const char* text) {
			SlEntry e{};
			if (!ReadSlEntry(pool, id, e)) return -1;
			const std::uint64_t h = SlHash(text);
			if (e.hash == h) return 1;
			return e.hash == ~h ? 0 : -1;
		}

		// Would a lookup of `text` still land on `id`? Not once the entry is skipped, or freed and reused.
		bool IsLiveStringId(std::uintptr_t pool, std::uint32_t id, const char* text) {
			SlEntry e{};
			if (!ReadSlEntry(pool, id, e)) return false;
			return !(e.flags & kSlSkipped) && e.hash == SlHash(text) && e.len == std::strlen(text);
		}

		// The text behind an id, for a log line: plain when it reads as printable, else the entry's hash.
		std::string SlText(std::uintptr_t pool, std::uint32_t id) {
			SlEntry e{};
			if (!ReadSlEntry(pool, id, e)) return std::format("<id 0x{:X} unreadable>", id);
			char buf[97]{};
			const std::size_t n = std::min<std::size_t>(e.len, 96);
			const bool read = n && ReadGuarded(buf, reinterpret_cast<const void*>(pool + 16ull * id + sizeof(SlEntry)), n);
			if (read && std::all_of(buf, buf + n, [](char c) { return c >= 0x20 && c < 0x7F; })) return std::string(buf, n);
			return std::format("<hash {:016X} len {} flags 0x{:02X}>", e.hash, e.len, e.flags);
		}

		// f(text, refs, count) for each string literal. `text` is plain (ACTS's header skipped); `refs` points
		// at `count` u32 offsets of GetString operands.
		template <typename F>
		void ForEachLiteral(const Script& s, F&& f) {
			const std::uint8_t* d = s.buffer->p;
			const auto* o = s.Obj();
			std::size_t off = o->string_offset;
			for (std::uint16_t i = 0; i < o->string_count; ++i) {
				CW::T8GSCString e;
				std::memcpy(&e, d + off, sizeof(e));
				off += sizeof(e);
				const char* text = reinterpret_cast<const char*>(d + e.string);
				// ACTS writes a 3-byte header (0x8B, len+1, 0) the way the fastfile's encrypted strings
				// have one; the text after it is plain. Pass plain text so the hash is computed on it.
				if ((static_cast<std::uint8_t>(text[0]) & 0xC0) == 0x80) text += 3;
				f(text, d + off, e.num_address);
				off += 4ull * e.num_address;
			}
		}

		std::uint32_t OperandId(const Script& s, const std::uint8_t* refs) {
			std::uint32_t ref = 0, id = 0;
			std::memcpy(&ref, refs, 4);
			std::memcpy(&id, s.buffer->p + ref, 4);
			return id;
		}

		// Ids we wrote that a lookup of their text would no longer return. Scr_InitConstStrings re-interns
		// the engine's constant strings on every SV_SpawnServer, and an id taken before that (an earlier map,
		// or earlier in this spawn) is suspected of being the copy the engine's own string stops equalling:
		// 2026-09-26, the first mapkit level script's "int" was refused by the clientfield type check and the
		// match dropped with "November 406 Cut Rain". LogStringDiagnostics shows whether this is the cause.
		int CountStaleStrings(const Script& s, std::uintptr_t pool) {
			int stale = 0;
			ForEachLiteral(s, [&](const char* text, const std::uint8_t* refs, std::uint8_t n) {
				if (n) stale += !IsLiveStringId(pool, OperandId(s, refs), text);
			});
			return stale;
		}

		// Intern every string literal and write its SL id into each GetString operand: on the first
		// serve, and again on any later serve that finds an id gone stale (CountStaleStrings).
		bool EnsureStrings(Script& s) {
			const std::uintptr_t pool = SlPoolBase();
			if (s.stringsReady.load(std::memory_order_acquire) && (!pool || !CountStaleStrings(s, pool))) return true;
			std::lock_guard<std::mutex> lock(s.fixMutex);
			if (s.fixFailed) return false;
			const bool refresh = s.stringsReady.load();
			int stale = 0;
			if (refresh && (!pool || !(stale = CountStaleStrings(s, pool)))) return true;

			auto* d = s.buffer->p;
			const auto* o = s.Obj();
			if (o->string_count && !g_SlGetString) {
				s.fixFailed = true;
				LOG("Scripting", ERROR, "'{}' has {} string literal(s) but SL_GetString is unavailable on this "
					"build; not serving it (its strings would be garbage ids).", s.file, o->string_count);
				return false;
			}
			int refs = 0, notPlaceholder = 0, changed = 0, canonical = 0, unknown = 0, examples = 0;
			std::string inverted, overwritten;
			ForEachLiteral(s, [&](const char* text, const std::uint8_t* refList, std::uint8_t n) {
				const std::uint32_t id = g_SlGetString(text, 0, 0, false);
				switch (IsCanonicalStringId(pool, id, text)) {
				case 1: ++canonical; break;
				case 0: inverted += std::format(" \"{}\"", text); break;
				default: ++unknown; break;
				}
				for (std::uint8_t a = 0; a < n; ++a) {
					std::uint32_t ref, before;
					std::memcpy(&ref, refList + 4ull * a, 4);
					std::memcpy(&before, d + ref, 4);
					notPlaceholder += before != kStringPlaceholder;
					changed += before != id;
					// What replaced our id since the last serve: the engine's own processing of the buffer, or
					// a string that was re-interned elsewhere. The text behind it tells which.
					if (refresh && before != id && examples < 4) {
						++examples;
						overwritten += std::format(" \"{}\": 0x{:X} ({}) -> 0x{:X};", text, before, SlText(pool, before), id);
					}
					std::memcpy(d + ref, &id, 4);
					++refs;
				}
			});
			s.stringsReady.store(true, std::memory_order_release);
			if (refresh) {
				LOG("Scripting", WARN, "'{}': {} string id(s) went stale since the last serve; re-interned, {} of {} "
					"operand(s) changed. Found in the operands:{}", s.file, stale, changed, refs, overwritten);
			} else if (o->string_count) {
				LOG("Scripting", INFO, "'{}': {} string literal(s) interned, {} operand(s) written; {} filed under "
					"their own hash{}.", s.file, o->string_count, refs, canonical,
					unknown ? std::format(", {} could not be checked", unknown) : std::string());
			}
			if (!inverted.empty()) {
				LOG("Scripting", ERROR, "'{}': SL_GetString filed these under the complement of their hash (its Arxan "
					"caller guard tripped), so they never equal the engine's own copies:{}", s.file, inverted);
			}
			if (notPlaceholder && !refresh) {
				// ACTS leaves 0x12345678 in every GetString operand. Anything else means the ref list
				// does not point where we think, and we just overwrote bytecode.
				LOG("Scripting", ERROR, "'{}': {} of {} string ref(s) did not hold the ACTS placeholder. Was it "
					"compiled by something other than ACTS for Cold War?", s.file, notPlaceholder, refs);
			}
			return true;
		}

		// --- string diagnostics ---
		std::string DescribeSlEntry(std::uintptr_t pool, std::uint32_t id, const char* text) {
			SlEntry e{};
			if (!ReadSlEntry(pool, id, e)) return "unreadable";
			const std::uint64_t h = SlHash(text);
			return std::format("refs {} flags 0x{:02X} len {} hash {}", e.refs, e.flags, e.len,
				e.hash == h ? std::string("own") : e.hash == ~h ? std::string("INVERTED") : std::format("other {:016X}", e.hash));
		}

		// Every entry in the text's bucket that carries its hash (or the complement). `live` gets the one
		// SL_GetString would return: the first with the hash and length that is not skipped.
		std::string BucketMatches(std::uintptr_t pool, const char* text, std::uint32_t& live) {
			live = 0;
			const std::uint32_t* buckets = SlBuckets();
			if (!pool || !buckets) return " (no bucket table)";
			const std::uint64_t h = SlHash(text);
			const std::size_t len = std::strlen(text);
			std::uint32_t id = 0;
			if (!ReadGuarded(&id, buckets + (h & 0xFFFF), sizeof(id))) return " (bucket unreadable)";
			std::string out;
			for (int steps = 0; id && steps < 256; ++steps) {
				SlEntry e{};
				if (!ReadSlEntry(pool, id, e)) {
					out += std::format(" 0x{:X}(unreadable)", id);
					break;
				}
				if (e.hash == h || e.hash == ~h) {
					out += std::format(" 0x{:X}(flags 0x{:02X} refs {} len {}{})", id, e.flags, e.refs, e.len,
						e.hash == h ? "" : " inverted");
					if (!live && e.hash == h && e.len == len && !(e.flags & kSlSkipped)) live = id;
				}
				id = e.next;
			}
			return out.empty() ? " none" : out;
		}

		void ReportEngineInt(const char* why) {
			const std::uintptr_t pool = SlPoolBase();
			std::uint32_t id = 0;
			if (!g_ScrConstInt || !ReadGuarded(&id, g_ScrConstInt, sizeof(id))) {
				LOG("Scripting", WARN, "String check ({}): the engine's \"int\" constant did not resolve.", why);
				return;
			}
			std::uint32_t live = 0;
			const std::string bucket = BucketMatches(pool, "int", live);
			LOG("Scripting", INFO, "String check ({}), thread {}: the engine's \"int\" is 0x{:X} [{}]; a lookup of \"int\" "
				"returns {}; entries with its hash:{}", why, ::GetCurrentThreadId(), id, DescribeSlEntry(pool, id, "int"),
				live ? std::format("0x{:X}", live) : std::string("nothing"), bucket);
		}

		// One line per script, plus one for each literal whose id is not what a lookup of its text returns now.
		void ReportStrings(const Script& s, const char* why) {
			if (!s.valid || !s.buffer || !s.Obj()->string_count || !s.stringsReady.load()) return;
			const std::uintptr_t pool = SlPoolBase();
			int same = 0, differ = 0;
			ForEachLiteral(s, [&](const char* text, const std::uint8_t* refs, std::uint8_t n) {
				if (!n) return;
				const std::uint32_t id = OperandId(s, refs);
				std::uint32_t live = 0;
				const std::string bucket = BucketMatches(pool, text, live);
				if (id == live) {
					++same;
					return;
				}
				++differ;
				LOG("Scripting", WARN, "  '{}' \"{}\": ours 0x{:X} [{}], a lookup returns {}; entries with its hash:{}",
					s.file, text, id, DescribeSlEntry(pool, id, text),
					live ? std::format("0x{:X}", live) : std::string("nothing"), bucket);
			});
			if (differ) {
				LOG("Scripting", WARN, "String check ({}): '{}': {} literal(s) are what a lookup returns, {} are NOT.",
					why, s.file, same, differ);
			} else {
				LOG("Scripting", INFO, "String check ({}): '{}': all {} literal(s) are what a lookup returns.",
					why, s.file, same);
			}
		}

		// --- clientfield diagnostics ---
		// "Server Disconnected - Clientfield Mismatch" (2026-09-26, the first mapkit level script with its own .csc):
		// the server's and the client's registered clientfields differ, and the engine prints which only to a TTY
		// we do not have. Each VM's table is dumped just before ClientField_Shutdown erases it, and once both
		// have been dumped the difference goes to the log.
		struct CfEntry {
			std::uint64_t callback;        // +0 the client's change callback
			std::uint8_t unk8[16];
			std::uint32_t name;            // +24 SL id
			std::uint32_t stateOffset;     // +28
			std::uint32_t mask;            // +32
			std::uint8_t shift;            // +36
			std::uint8_t type;             // +37 0 int, 1 float, 2 counter, 3 bgcache
			std::uint8_t versionNegative;  // +38
			std::uint8_t pool;             // +39
			std::uint32_t bits;            // +40
			std::uint32_t version;         // +44
			std::uint8_t opt[8];           // +48 the register call's option bytes
			std::uint32_t opt56;
			std::uint8_t opt60;
			std::uint8_t pad[3];
		};
		static_assert(sizeof(CfEntry) == 64);

		struct CfRecord {
			int pool = 0;
			int index = 0;
			std::uint32_t name = 0;
			std::string text;
			std::uint32_t bits = 0;
			std::uint32_t version = 0;
			std::uint8_t type = 0;
			bool callback = false;
		};

		using ClientFieldShutdownT = std::uintptr_t();
		ClientFieldShutdownT* g_CfShutdownOrig{};
		std::atomic_bool g_CfHooked{ false };
		std::mutex g_CfMutex;
		std::vector<CfRecord> g_CfLast[2];   // [0] server, [1] client; empty = not dumped since the last diff

		const char* CfTypeName(std::uint8_t t) {
			static constexpr const char* kNames[] = { "int", "float", "counter", "bgcache" };
			return t < 4 ? kNames[t] : "?";
		}

		// The "scriptmover" pool string maps to 3 on the client and 4 on the server (ClientField_PoolFromString),
		// so the two are one pool when comparing VMs.
		int CfPoolKey(int pool) { return pool == 4 ? 3 : pool; }

		std::string CfDescribe(const CfRecord& r) {
			return std::format("pool {} #{} \"{}\" bits {} v{} {}", r.pool, r.index, r.text, r.bits, r.version, CfTypeName(r.type));
		}

		std::vector<CfRecord> ReadClientFields(int& vm) {
			std::vector<CfRecord> out;
			vm = -1;
			if (!g_GameTlsIndex) return out;
			const auto* slots = reinterpret_cast<const std::uintptr_t*>(__readgsqword(0x58));
			std::uintptr_t block = 0, ctx = 0, table = 0;
			std::uint8_t ready = 0;
			if (!slots || !ReadGuarded(&block, slots + *g_GameTlsIndex, sizeof(block)) || !block) return out;
			if (!ReadGuarded(&ready, reinterpret_cast<const void*>(block + kTlsReady), 1) || !ready) return out;
			if (!ReadGuarded(&ctx, reinterpret_cast<const void*>(block + kTlsVmCtx), sizeof(ctx)) || !ctx) return out;
			if (!ReadGuarded(&table, reinterpret_cast<const void*>(ctx), sizeof(table)) || !table) return out;
			vm = (g_ServerVmCtx && ctx == g_ServerVmCtx) ? 0 : 1;
			const std::uintptr_t pool = SlPoolBase();
			for (int p = 0; p < kCfPools; ++p) {
				const std::uintptr_t base = table + kCfPoolStride * p;
				std::int32_t count = 0;
				if (!ReadGuarded(&count, reinterpret_cast<const void*>(base + 8), sizeof(count)) || count <= 0) continue;
				count = std::min(count, kCfMaxPerPool);
				for (int i = 0; i < count; ++i) {
					std::uintptr_t ep = 0;
					CfEntry e{};
					if (!ReadGuarded(&ep, reinterpret_cast<const void*>(base + 16 + 8ull * i), sizeof(ep)) || !ep) continue;
					if (!ReadGuarded(&e, reinterpret_cast<const void*>(ep), sizeof(e))) continue;
					out.push_back({ p, i, e.name, SlText(pool, e.name), e.bits, e.version, e.type, e.callback != 0 });
				}
			}
			return out;
		}

		void WriteClientFieldList(const std::vector<CfRecord>& fields, const char* vmName) {
			std::error_code ec;
			const auto dir = std::filesystem::current_path(ec) / "cw-mod";
			std::ofstream f(dir / std::format("clientfields_{}.txt", vmName), std::ios::trunc);
			if (!f.is_open()) return;
			for (const auto& r : fields) {
				f << std::format("{}{}\n", CfDescribe(r), r.callback ? " callback" : "");
			}
		}

		// Logs the difference; true when the two lists differ in any way.
		bool DiffClientFields(const std::vector<CfRecord>& server, const std::vector<CfRecord>& client) {
			auto keyOf = [](const CfRecord& r) { return (static_cast<std::uint64_t>(CfPoolKey(r.pool)) << 32) | r.name; };
			std::unordered_map<std::uint64_t, const CfRecord*> sv, cl;
			for (const auto& r : server) sv[keyOf(r)] = &r;
			for (const auto& r : client) cl[keyOf(r)] = &r;
			int onlyServer = 0, onlyClient = 0, differ = 0;
			for (const auto& r : server) {
				auto it = cl.find(keyOf(r));
				if (it == cl.end()) {
					if (++onlyServer <= 60) LOG("Scripting", WARN, "  clientfield only on the SERVER: {}", CfDescribe(r));
				} else if (it->second->bits != r.bits || it->second->version != r.version || it->second->type != r.type) {
					if (++differ <= 60) LOG("Scripting", WARN, "  clientfield differs: server {} / client {}", CfDescribe(r), CfDescribe(*it->second));
				}
			}
			for (const auto& r : client) {
				if (!sv.count(keyOf(r)) && ++onlyClient <= 60) LOG("Scripting", WARN, "  clientfield only on the CLIENT: {}", CfDescribe(r));
			}
			// Same sets can still mismatch on order: a field's index in its pool is what goes over the wire.
			int orderDiffs = 0;
			if (!onlyServer && !onlyClient) {
				std::unordered_map<std::uint64_t, int> clIndex;
				for (const auto& r : client) clIndex[keyOf(r)] = r.index;
				for (const auto& r : server) {
					if (clIndex[keyOf(r)] != r.index && ++orderDiffs <= 20) {
						LOG("Scripting", WARN, "  clientfield order differs: server {} is #{} on the client", CfDescribe(r), clIndex[keyOf(r)]);
					}
				}
			}
			const std::string summary = std::format("Clientfield diff: server {} field(s), client {}; only on the server {}, "
				"only on the client {}, different bits/version/type {}, different order {}. Lists: "
				"cw-mod/clientfields_server.txt, clientfields_client.txt.",
				server.size(), client.size(), onlyServer, onlyClient, differ, orderDiffs);
			if (onlyServer || onlyClient || differ || orderDiffs) {
				LOG("Scripting", WARN, "{}", summary);
				return true;
			}
			LOG("Scripting", INFO, "{}", summary);
			return false;
		}

		void DumpClientFields() {
			try {
				int vm = -1;
				std::vector<CfRecord> fields = ReadClientFields(vm);
				if (vm < 0 || fields.empty()) return;
				const char* vmName = vm == 0 ? "server" : "client";
				std::array<int, kCfPools> perPool{};
				for (const auto& r : fields) ++perPool[r.pool];
				std::string counts;
				for (int p = 0; p < kCfPools; ++p) {
					if (perPool[p]) counts += std::format(" {}:{}", p, perPool[p]);
				}
				LOG("Scripting", INFO, "Clientfields of the {} VM before ClientField_Shutdown, thread {}: {} field(s), per pool{}.",
					vmName, ::GetCurrentThreadId(), fields.size(), counts);
				std::lock_guard<std::mutex> lock(g_CfMutex);
				g_CfLast[vm] = std::move(fields);
				if (!g_CfLast[0].empty() && !g_CfLast[1].empty()) {
					// Only a mismatch writes the lists: a clean dump right after (the frontend) would bury them.
					if (DiffClientFields(g_CfLast[0], g_CfLast[1])) {
						WriteClientFieldList(g_CfLast[0], "server");
						WriteClientFieldList(g_CfLast[1], "client");
					}
					g_CfLast[0].clear();
					g_CfLast[1].clear();
				}
			} catch (...) {
				// Diagnostics must never take the game down with them.
			}
		}

		std::uintptr_t hkClientFieldShutdown() {
			if (g_Enabled.load(std::memory_order_relaxed)) DumpClientFields();
			return g_CfShutdownOrig();
		}

		// Diagnostics only: a failure is logged and the loader carries on.
		void InstallClientFieldDiag() {
			if (g_CfHooked.load() || !g_CfShutdownTarget || !g_GameTlsIndex) {
				if (!g_CfHooked.load()) LOG("Scripting", WARN, "Clientfield dump unavailable: ClientField_Shutdown or TlsIndex did not resolve.");
				return;
			}
			void* trampoline = nullptr;
			if (MH_CreateHook(g_CfShutdownTarget, reinterpret_cast<void*>(&hkClientFieldShutdown), &trampoline) != MH_OK || !trampoline) {
				LOG("Scripting", WARN, "Clientfield dump unavailable: could not hook ClientField_Shutdown.");
				return;
			}
			// Through the Arxan thunk: a caller-guarded original called from our module silently skips its body,
			// and this one's body is what empties the table for the next map.
			auto* thunk = Client::Game::ArxanCall::MakeThunk<ClientFieldShutdownT>(trampoline);
			g_CfShutdownOrig = thunk ? thunk : reinterpret_cast<ClientFieldShutdownT*>(trampoline);
			if (MH_EnableHook(g_CfShutdownTarget) != MH_OK) {
				MH_RemoveHook(g_CfShutdownTarget);
				LOG("Scripting", WARN, "Clientfield dump unavailable: could not enable the ClientField_Shutdown hook.");
				return;
			}
			g_CfHooked.store(true);
			LOG("Scripting", INFO, "Clientfield dump on (ClientField_Shutdown hooked{}, server VM context {}).",
				thunk ? ", original through the Arxan thunk" : ", original called DIRECTLY (no thunk)",
				g_ServerVmCtx ? std::format("0x{:X}", g_ServerVmCtx) : std::string("unresolved"));
		}

		ScriptPtr LoadOne(const std::filesystem::path& path, const std::filesystem::path& root) {
			auto s = std::make_shared<Script>();
			std::error_code ec;
			s->file = Utf8(std::filesystem::relative(path, root, ec));
			if (ec) s->file = Utf8(path.filename());

			std::ifstream in(path, std::ios::binary);
			if (!in.is_open()) {
				s->error = "could not open the file";
				return s;
			}
			std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			if (std::string err = ValidateObject(bytes.data(), bytes.size()); !err.empty()) {
				s->error = err;
				return s;
			}
			s->buffer = std::make_unique<AlignedBuffer>(bytes.size());
			if (!s->buffer->p) {
				s->error = "out of memory";
				return s;
			}
			std::memcpy(s->buffer->p, bytes.data(), bytes.size());

			std::string ext = Utf8(path.extension());
			for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			s->replaceOnly = ext == ".cscc";

			std::uint64_t fileHash = 0;
			const auto* o = s->Obj();
			if (ParseHashFilename(Utf8(path.stem()), fileHash)) {
				s->name = fileHash;
				s->source = NameSource::FileHash;
			} else if (o->name & kNameMask) {
				s->name = o->name & kNameMask;
				s->source = NameSource::Embedded;
			} else {
				s->source = NameSource::FromPath;
			}
			s->strings = o->string_count;
			s->spt.buffer = s->Obj();
			s->spt.len = static_cast<std::int32_t>(bytes.size());
			for (char& c : s->file) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			s->valid = true;
			return s;
		}

		// A name made from the file's path, in the engine's own form: scripts/cwmod/<path>.gsc.
		std::uint64_t PathName(const std::string& rel) {
			std::string p = rel;
			if (auto dot = p.rfind('.'); dot != std::string::npos) p.resize(dot);
			return HashScriptName("scripts/cwmod/" + p + ".gsc");
		}

		std::vector<ScriptPtr> LoadFolder(std::string& summary) {
			namespace fs = std::filesystem;
			const fs::path root = ScriptsFolderPath();
			const auto compiledIn = [](const fs::path& dir) {
				std::vector<fs::path> found;
				std::error_code ec;
				for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
						!ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
					if (!it->is_regular_file(ec)) continue;
					std::string ext = Utf8(it->path().extension());
					for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
					if (ext == ".gscc" || ext == ".cscc") found.push_back(it->path());
				}
				std::sort(found.begin(), found.end());
				return found;
			};
			// The scripts folder first, then each map's; a map's own file is named relative to cw-mod
			// (maps/<map>/scripts/...) so the log and the Scripts tab tell the two apart.
			std::vector<std::pair<fs::path, fs::path>> files;   // {file, root it is named from}
			for (const fs::path& f : compiledIn(root)) files.emplace_back(f, root);
			const std::vector<fs::path> mapFolders = MapScriptFolders();
			for (const fs::path& dir : mapFolders) {
				for (const fs::path& f : compiledIn(dir)) files.emplace_back(f, root.parent_path());
			}

			std::vector<ScriptPtr> out;
			std::vector<std::uint64_t> used;
			std::vector<std::uint64_t> hostNames;
			for (const char* h : kHostPaths) hostNames.push_back(HashScriptName(h));
			auto taken = [&](std::uint64_t n) { return std::find(used.begin(), used.end(), n) != used.end(); };

			int valid = 0;
			for (const auto& [f, namedFrom] : files) {
				ScriptPtr s = LoadOne(f, namedFrom);
				if (s->valid && namedFrom != root) s->replaceOnly = true;
				if (s->valid && s->replaceOnly && s->source != NameSource::Embedded && s->source != NameSource::FileHash) {
					// A path name is only good for injecting, and this one is never injected.
					s->valid = false;
					s->error = "it has no name to replace: compile it with --name (--name-client for a .csc) or name the "
						"file 0x<hash>";
				}
				if (s->valid) {
					if (s->replaceOnly && taken(s->name)) {
						s->valid = false;
						s->error = std::format("name 0x{:X} is already used by another script", s->name);
					}
					else if (s->source != NameSource::FileHash && (s->source == NameSource::FromPath || taken(s->name))) {
						if (s->source == NameSource::Embedded) {
							LOG("Scripting", WARN, "'{}' is compiled under a name another script already uses; "
								"naming it from its path instead.", s->file);
						}
						s->name = PathName(s->file);
						s->source = NameSource::FromPath;
					}
					if (taken(s->name)) {
						s->valid = false;
						s->error = std::format("name 0x{:X} is already used by another script", s->name);
					} else if (std::find(hostNames.begin(), hostNames.end(), s->name) != hostNames.end()) {
						s->valid = false;
						s->error = "replaces the injection host (zm_utility), which is not supported";
					}
				}
				if (s->valid) {
					s->Obj()->name = s->name;   // our copy: the linked object names itself as served
					s->spt.name = s->name;
					used.push_back(s->name);
					++valid;
					LOG("Scripting", INFO, "Loaded '{}' as 0x{:X} ({}, {} bytes, {} string(s){}).", s->file, s->name,
						s->source == NameSource::FileHash ? "name from file" :
						s->source == NameSource::FromPath ? "name from path" : "compiled name",
						s->spt.len, s->strings, s->replaceOnly ? ", replace only" : "");
				} else {
					LOG("Scripting", WARN, "Skipped '{}': {}.", s->file, s->error);
				}
				out.push_back(std::move(s));
			}
			summary = files.empty()
				? std::format("No .gscc/.cscc files in {}.", Utf8(root))
				: std::format("{}/{} script(s) loaded from {}{}.", valid, files.size(), Utf8(root),
					mapFolders.empty() ? std::string() : std::format(" and {} map folder(s)", mapFolders.size()));
			return out;
		}

		bool AnyOfOursLinked_Locked() {
			for (const auto& s : g_Scripts) {
				if (s->valid && IsLinked(s->buffer->p)) return true;
			}
			for (const auto& h : g_Hosts) {
				if (h->current && IsLinked(h->current->buffer->p)) return true;
			}
			return false;
		}

		// Swap in a staged reload once nothing of ours is linked, so a match never mixes the old and
		// new copy of a script (unlink finds objects by the buffer Scr_GetGscObj returns).
		bool TryApplyPending_Locked() {
			if (!g_Pending || AnyOfOursLinked_Locked()) return false;
			for (auto& s : g_Scripts) g_Retired.push_back(std::move(s));
			g_Scripts = std::move(*g_Pending);
			g_Pending.reset();
			++g_Generation;
			LOG("Scripting", INFO, "Reloaded script set is now live ({} file(s)).", g_Scripts.size());
			return true;
		}

		Host* FindHost_Locked(std::uint64_t name) {
			for (auto& h : g_Hosts) {
				if (h->name == name) return h.get();
			}
			return nullptr;
		}

		// Build a copy of the stock host with `inject` appended to its include table. The copy is
		// the stock bytes verbatim plus a new table at the end; only the table offset, the count and
		// file_size change.
		std::unique_ptr<HostCopy> BuildHostCopy(const CW::ScriptParseTree& stock, const std::vector<std::uint64_t>& inject) {
			const auto* src = reinterpret_cast<const std::uint8_t*>(stock.buffer);
			const std::size_t len = static_cast<std::size_t>(stock.len);
			const auto* so = reinterpret_cast<const CW::T9GSCOBJ*>(src);
			if (len < sizeof(CW::T9GSCOBJ) || std::memcmp(src, kGscMagic, sizeof(kGscMagic)) != 0
					|| !InBounds(len, so->includes_table, 8ull * so->includes_count)) {
				return nullptr;
			}
			std::vector<std::uint64_t> includes(so->includes_count);
			std::memcpy(includes.data(), src + so->includes_table, 8ull * so->includes_count);
			for (std::uint64_t n : inject) {
				const bool present = std::any_of(includes.begin(), includes.end(),
					[n](std::uint64_t i) { return (i & kNameMask) == n; });
				if (!present) includes.push_back(n);
			}
			if (includes.size() > 0xFFFF) return nullptr;

			const std::size_t tableOff = (len + 15) & ~std::size_t(15);
			const std::size_t newLen = tableOff + 8 * includes.size();
			auto copy = std::make_unique<HostCopy>();
			copy->buffer = std::make_unique<AlignedBuffer>(newLen);
			if (!copy->buffer->p) return nullptr;
			std::memcpy(copy->buffer->p, src, len);
			std::memcpy(copy->buffer->p + tableOff, includes.data(), 8 * includes.size());
			auto* o = reinterpret_cast<CW::T9GSCOBJ*>(copy->buffer->p);
			o->includes_table = static_cast<std::uint32_t>(tableOff);
			o->includes_count = static_cast<std::uint16_t>(includes.size());
			if (o->file_size == len) o->file_size = static_cast<std::uint32_t>(newLen);

			copy->stockSnapshot.assign(src, src + len);
			copy->stockPtr = src;
			copy->injected = inject;
			copy->spt.name = stock.name;
			copy->spt.buffer = o;
			copy->spt.len = static_cast<std::int32_t>(newLen);
			return copy;
		}

		CW::XAssetHeader ServeCopy(Host& host, HostCopy& copy) {
			host.served.fetch_add(1, std::memory_order_relaxed);
			CW::XAssetHeader h{};
			h.spt = &copy.spt;
			return h;
		}

		// Can the current copy be handed out again? Yes while it is linked (unlink and every import
		// lookup must see the same buffer), and yes when the stock bytes and script set are exactly
		// what it was built from (the requests before the linker registers it, or a map that reloaded
		// the same stock host). Stock bytes include the string ids the fastfile load wrote, so a
		// byte-identical stock means the copy's ids are current too.
		bool CopyStillValid_Locked(HostCopy& cur, const CW::ScriptParseTree& stock) {
			if (IsLinkedHinted(cur.buffer->p, cur.linkHint)) return true;
			return cur.generation == g_Generation && cur.stockPtr == stock.buffer
				&& cur.stockSnapshot.size() == static_cast<std::size_t>(stock.len)
				&& std::memcmp(cur.stockSnapshot.data(), stock.buffer, cur.stockSnapshot.size()) == 0;
		}

		// The engine asked for a host and the stock DB gave `stock`. Hand back a copy carrying our
		// INJECT scripts as includes, or the stock header when there is nothing to inject. While a
		// map links this runs once per import per includer, so the repeat answers come first.
		CW::XAssetHeader ServeHost(Host& host, CW::XAssetHeader stock) {
			if (!stock.spt || !stock.spt->buffer || stock.spt->len < static_cast<std::int32_t>(sizeof(CW::T9GSCOBJ))) {
				return stock;
			}

			std::vector<ScriptPtr> candidates;
			{
				std::lock_guard<std::mutex> lock(g_Mutex);
				TryApplyPending_Locked();
				if (host.current && CopyStillValid_Locked(*host.current, *stock.spt)) {
					return ServeCopy(host, *host.current);
				}
				if (!host.current && host.passStockPtr == stock.spt->buffer && host.passGeneration == g_Generation) {
					return stock;
				}
				// Decide INJECT vs REPLACE now: this map's fastfiles are loaded, so the pool is the truth.
				for (auto& s : g_Scripts) {
					if (!s->valid) continue;
					const bool replace = s->source == NameSource::FileHash || s->replaceOnly || StockScriptExists(s->name);
					s->mode.store(replace ? Mode::Replace : Mode::Inject);
					if (!replace) candidates.push_back(s);
				}
			}
			// Strings are fixed outside g_Mutex: SL_GetString takes the engine's own string lock.
			std::vector<std::uint64_t> inject;
			for (auto& s : candidates) {
				if (EnsureStrings(*s)) inject.push_back(s->name);
			}

			std::lock_guard<std::mutex> lock(g_Mutex);
			HostCopy* cur = host.current.get();
			if (cur && IsLinkedHinted(cur->buffer->p, cur->linkHint)) {
				// Linked while we were deciding, or a reload mid-match: keep what is linked.
				return ServeCopy(host, *cur);
			}
			// Serve the stock host untouched for the rest of this stock buffer / script set.
			auto passThrough = [&] {
				if (host.current) host.retired.push_back(std::move(host.current));
				host.passStockPtr = stock.spt->buffer;
				host.passGeneration = g_Generation;
				return stock;
			};
			if (inject.empty()) return passThrough();
			if (LinkedElsewhere(host.name, cur ? cur->buffer->p : nullptr)) {
				if (!host.skippedDuplicate.exchange(true)) {
					LOG("Scripting", ERROR, "Host {} is already linked from its stock buffer (a path that bypasses "
						"DB_FindXAssetHeader). Not injecting: that would link it twice. Scripts to inject: {}.",
						host.path, inject.size());
				}
				return passThrough();
			}

			auto copy = BuildHostCopy(*stock.spt, inject);
			if (!copy) {
				LOG("Scripting", ERROR, "Host {}: stock object failed validation; serving it unmodified.", host.path);
				return passThrough();
			}
			copy->generation = g_Generation;
			LOG("Scripting", INFO, "Host {}: injecting {} script(s) ({} -> {} includes).", host.path, inject.size(),
				reinterpret_cast<const CW::T9GSCOBJ*>(stock.spt->buffer)->includes_count,
				reinterpret_cast<const CW::T9GSCOBJ*>(copy->buffer->p)->includes_count);
			if (host.current) host.retired.push_back(std::move(host.current));
			host.current = std::move(copy);
			host.passStockPtr = nullptr;
			return ServeCopy(host, *host.current);
		}

		// The one detour. Every asset lookup in the game comes through here, so anything that is not
		// a scriptparsetree leaves after one compare (and one atomic load: a world lookup by a map of its own's name,
		// mapkit_loader.hpp 7., goes to its asset library's name, which its world has).
		CW::XAssetHeader hkDB_FindXAssetHeader(CW::XAssetType type, std::uint64_t name, bool includeOverride,
				int waitTime) {
			Client::Game::MapKitUsage::OnFind(static_cast<std::uint32_t>(type), name, _ReturnAddress());
			if (type != CW::ASSET_TYPE_SCRIPTPARSETREE) {
				return g_OrigDbFind(type, Client::Game::MapKit::WorldAssetName(static_cast<std::uint32_t>(type), name),
					includeOverride, waitTime);
			}

			const std::uint64_t key = name & kNameMask;
			if (g_CaptureEnabled.load(std::memory_order_relaxed)) {
				std::lock_guard<std::mutex> lock(g_CaptureMutex);
				++g_Captured[key];
			}
			if (!g_Enabled.load(std::memory_order_relaxed)) return g_OrigDbFind(type, name, includeOverride, waitTime);

			ScriptPtr serve;
			Host* host = nullptr;
			std::array<Script*, 16> recheck{};   // alive for good: a replaced script moves to g_Retired
			std::size_t recheckCount = 0;
			{
				std::lock_guard<std::mutex> lock(g_Mutex);
				for (auto& s : g_Scripts) {
					if (!serve && s->valid && s->name == key) serve = s;
					if (s->strings && s->served.load(std::memory_order_relaxed) && recheckCount < recheck.size()) {
						recheck[recheckCount++] = s.get();
					}
				}
				if (!serve) host = FindHost_Locked(key);
			}
			// Every script lookup re-checks the string ids of what we already served. A first serve can come
			// before this spawn's Scr_InitConstStrings, while the link that follows it asks for every include
			// afterwards, so a stale id is caught before the level script runs. Lock-free unless one is stale.
			for (std::size_t i = 0; i < recheckCount; ++i) {
				if (recheck[i] != serve.get()) EnsureStrings(*recheck[i]);
			}
			if (serve && EnsureStrings(*serve)) {
				if (serve->served.fetch_add(1, std::memory_order_relaxed) == 0) {
					// Patched here, not at boot: nothing runs our bytecode before it is served, and keeping
					// this write apart from the detour lets the log say which one a crash followed.
					std::string lazy = SetLazyLink(true);
					if (!lazy.empty()) {
						LOG("Scripting", ERROR, "LazyLink handler not installed ({}): a script taking &ns::fn of "
							"another script would crash the VM.", lazy);
					}
					LOG("Scripting", INFO, "Serving '{}' (0x{:X}) to the engine.", serve->file, key);
					if (serve->strings) {
						ReportEngineInt("first serve");
						ReportStrings(*serve, "first serve");
					}
				}
				CW::XAssetHeader h{};
				h.spt = &serve->spt;
				return h;
			}
			if (host) return ServeHost(*host, g_OrigDbFind(type, name, includeOverride, waitTime));
			return g_OrigDbFind(type, name, includeOverride, waitTime);
		}

		bool InstallDbHook() {
			if (g_Hooked.load()) return true;
			if (!g_DbFindTarget) return false;
			// Create, publish the original, THEN enable. The asset threads call this constantly from boot on,
			// and one that enters the detour before g_OrigDbFind is set calls null. The original is called
			// directly: this prologue has no Arxan caller check (IDB), and the July loader ran it this way.
			void* target = reinterpret_cast<void*>(g_DbFindTarget);
			void* trampoline = nullptr;
			if (MH_CreateHook(target, reinterpret_cast<void*>(&hkDB_FindXAssetHeader), &trampoline) != MH_OK || !trampoline) {
				return false;
			}
			g_OrigDbFind = reinterpret_cast<CW::DB_FindXAssetHeaderT>(trampoline);
			if (MH_EnableHook(target) != MH_OK) {
				MH_RemoveHook(target);
				return false;
			}
			g_Hooked.store(true);
			const std::uint8_t prologue = *reinterpret_cast<const std::uint8_t*>(g_DbFindTarget);
			if (prologue == 0xE9) {
				LOG("Scripting", INFO, "DB_FindXAssetHeader detour live.");
			} else {
				LOG("Scripting", ERROR, "DB_FindXAssetHeader detour NOT live: prologue 0x{:02X}, expected 0xE9. "
					"No script will be served this boot.", prologue);
			}
			return true;
		}
	}

	std::uint64_t HashScriptName(const std::string& path) {
		std::uint64_t h = 0xCBF29CE484222325ull;
		for (char c : path) {
			const char l = (c == '\\') ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			h ^= static_cast<std::uint8_t>(l);
			h *= 0x100000001B3ull;
		}
		return h & kNameMask;
	}

	void Initialize() {
		std::call_once(g_InitOnce, [] {
			const std::string module = Common::Utility::NT::Library().GetName();
			const MODULEINFO modInfo = Common::Utility::GetModuleInfo(module);
			g_ModuleBase = reinterpret_cast<std::uintptr_t>(modInfo.lpBaseOfDll);
			g_ModuleSize = static_cast<std::uintptr_t>(modInfo.SizeOfImage);

			auto dbFind = Client::Memory::SigScan(SIG_DB_FindXAssetHeader, module, "DB_FindXAssetHeader");
			g_DbFindTarget = dbFind.As<CW::DB_FindXAssetHeaderT>();
			Record("DB_FindXAssetHeader", false, dbFind.As<std::uintptr_t>());

			auto sl = Client::Memory::SigScan(SIG_SL_GetString, module, "SL_GetString");
			g_SlGetStringTarget = sl ? sl.Sub(kSlGetStringSigOffset).As<void*>() : nullptr;
			Record("SL_GetString", false, reinterpret_cast<std::uintptr_t>(g_SlGetStringTarget));

			// Diagnostics only, so it stays out of the report AllResolved() reads.
			auto cft = Client::Memory::SigScan(SIG_ClientFieldType, module, "ClientField_TypeFromString");
			g_ScrConstInt = cft ? cft.Add(6).Rip().As<const std::uint32_t*>() : nullptr;
			auto cfs = Client::Memory::SigScan(SIG_ClientFieldShutdown, module, "ClientField_Shutdown");
			g_CfShutdownTarget = cfs ? cfs.As<void*>() : nullptr;
			g_GameTlsIndex = cfs ? cfs.Add(0x1E).Rip().As<const std::uint32_t*>() : nullptr;
			auto svc = Client::Memory::SigScan(SIG_ServerVmCtx, module, "g_serverVmCtx");
			g_ServerVmCtx = svc ? svc.Add(8).Rip().As<std::uintptr_t>() : 0;

			// The dispatch is `call [r12+rdi*8+disp32]` with r12 = image base, so the table is
			// base + disp32, not RIP-relative.
			auto vm = Client::Memory::SigScan(SIG_gVmOpJumpTable, module, "gVmOpJumpTable");
			std::uintptr_t table = 0;
			if (vm) {
				const auto disp = *reinterpret_cast<const std::int32_t*>(vm.As<std::uintptr_t>() + 4);
				table = g_ModuleBase + static_cast<std::uint32_t>(disp);
				if (CountCodePtrs(table) <= kVmProbe / 2) table = 0;
			}
			g_VmTable = reinterpret_cast<void**>(table);
			Record("gVmOpJumpTable", true, table);

			auto objInfo = Client::Memory::SigScan(SIG_gObjFileInfo, module, "gObjFileInfo");
			if (objInfo) {
				g_ObjInfoCount = objInfo.Add(3).Rip().As<std::uint32_t*>();
				g_ObjInfo = objInfo.Add(10).Rip().As<CW::ObjFileInfoStruct*>();
			}
			Record("gObjFileInfoCount", true, reinterpret_cast<std::uintptr_t>(g_ObjInfoCount));
			Record("gObjFileInfo", true, reinterpret_cast<std::uintptr_t>(g_ObjInfo));

			auto pools = Client::Memory::SigScan(SIG_xassetpool, module, "xassetpool");
			if (pools) g_Pools = pools.Add(3).Rip().As<CW::XAssetPool*>();
			Record("xassetpool", true, reinterpret_cast<std::uintptr_t>(g_Pools));

			for (const char* p : kHostPaths) {
				auto h = std::make_unique<Host>();
				h->path = p;
				h->name = HashScriptName(p);
				g_Hosts.push_back(std::move(h));
			}
			LOG("Scripting", INFO, "Signature resolution: {}/{} matched.",
				std::count_if(g_Report.begin(), g_Report.end(), [](const SignatureStatus& s) { return s.resolved; }),
				g_Report.size());
		});
	}

	const std::vector<SignatureStatus>& Signatures() { return g_Report; }

	bool AllResolved() {
		return !g_Report.empty() && std::all_of(g_Report.begin(), g_Report.end(),
			[](const SignatureStatus& s) { return s.resolved; });
	}

	bool LoaderActive() { return g_Enabled.load(std::memory_order_relaxed); }
	std::string ScriptsFolder() { return Utf8(ScriptsFolderPath()); }

	void LogStringDiagnostics(const char* why) {
		if (!g_Enabled.load(std::memory_order_relaxed)) return;
		std::vector<ScriptPtr> scripts;
		{
			// try_lock: an error raised while this thread holds g_Mutex must not deadlock the report.
			std::unique_lock<std::mutex> lock(g_Mutex, std::try_to_lock);
			if (!lock.owns_lock()) {
				LOG("Scripting", WARN, "String check ({}): script list busy, skipped.", why);
				return;
			}
			scripts = g_Scripts;
		}
		ReportEngineInt(why);
		for (const auto& s : scripts) {
			if (s->served.load(std::memory_order_relaxed)) ReportStrings(*s, why);
		}
	}

	std::string EnableLoader() {
		Initialize();
		if (g_Enabled.load()) return "Loader already on.";
		if (!g_DbFindTarget) return "DB_FindXAssetHeader did not resolve on this build; loader unavailable.";

		if (!g_SlGetString && g_SlGetStringTarget) {
			g_SlGetString = Client::Game::ArxanCall::MakeThunk<std::remove_pointer_t<CW::SL_GetStringT>>(g_SlGetStringTarget);
		}
		if (!g_SlGetString) {
			LOG("Scripting", ERROR, "No SL_GetString thunk: scripts with string literals will be refused.");
		}

		std::string summary;
		auto fresh = LoadFolder(summary);
		{
			std::lock_guard<std::mutex> lock(g_Mutex);
			for (auto& s : g_Scripts) g_Retired.push_back(std::move(s));
			g_Scripts = std::move(fresh);
			g_Pending.reset();
			++g_Generation;
		}

		if (!InstallDbHook()) return "Could not hook DB_FindXAssetHeader; loader unavailable.";
		InstallClientFieldDiag();
		// The LazyLink slot is patched on the first serve (hkDB_FindXAssetHeader), not here.
		g_Enabled.store(true);
		LOG("Scripting", INFO, "Loader on. {}", summary);
		return summary;
	}

	std::string DisableLoader() {
		if (!g_Enabled.load()) return "Loader already off.";
		{
			std::lock_guard<std::mutex> lock(g_Mutex);
			if (AnyOfOursLinked_Locked()) return "A match is using our scripts. Leave it first.";
		}
		g_Enabled.store(false);
		SetLazyLink(false);
		LOG("Scripting", INFO, "Loader off: the engine gets stock scripts from the next match.");
		return "Loader off. Stock scripts from the next match.";
	}

	std::string ReloadScripts() {
		std::string summary;
		auto fresh = LoadFolder(summary);
		std::lock_guard<std::mutex> lock(g_Mutex);
		g_Pending = std::make_unique<std::vector<ScriptPtr>>(std::move(fresh));
		if (TryApplyPending_Locked()) return summary;
		return summary + " A match is using the old set; the new one applies next match.";
	}

	bool ReloadPending() {
		std::lock_guard<std::mutex> lock(g_Mutex);
		return g_Pending != nullptr;
	}

	std::vector<ScriptInfo> Scripts() {
		std::vector<ScriptInfo> out;
		std::lock_guard<std::mutex> lock(g_Mutex);
		for (const auto& s : g_Scripts) {
			ScriptInfo i;
			i.file = s->file;
			i.name = s->name;
			i.source = s->source;
			i.mode = s->mode.load();
			i.bytes = s->spt.len > 0 ? static_cast<std::uint32_t>(s->spt.len) : 0;
			i.strings = s->strings;
			i.stringsReady = s->stringsReady.load();
			i.served = s->served.load();
			i.valid = s->valid;
			i.error = s->error;
			out.push_back(std::move(i));
		}
		return out;
	}

	std::vector<HostInfo> Hosts() {
		std::vector<HostInfo> out;
		std::lock_guard<std::mutex> lock(g_Mutex);
		for (const auto& h : g_Hosts) {
			HostInfo i;
			i.path = h->path;
			i.name = h->name;
			i.injected = h->current ? static_cast<std::uint32_t>(h->current->injected.size()) : 0;
			i.served = h->served.load();
			i.skippedDuplicate = h->skippedDuplicate.load();
			out.push_back(std::move(i));
		}
		return out;
	}

	void AutoInstall() {
		if (!Game::Settings::Get().scripts) {
			LOG("Scripting", INFO, "\"scripts\" is off in cw-mod.json: GSC loader stays off (Scripts tab can turn it on).");
			return;
		}
		EnableLoader();
	}

	bool LazyLinkInstalled() { return g_LazyLinkInstalled.load(std::memory_order_relaxed); }
	std::uint64_t LazyLinkCallCount() { return g_LazyLinkCalls.load(std::memory_order_relaxed); }
	std::uint64_t LazyLinkMisses() { return g_LazyLinkMisses.load(std::memory_order_relaxed); }

	bool CaptureEnabled() { return g_CaptureEnabled.load(std::memory_order_relaxed); }
	void SetCapture(bool on) { g_CaptureEnabled.store(on); }
	void ClearCapture() {
		std::lock_guard<std::mutex> lock(g_CaptureMutex);
		g_Captured.clear();
	}

	std::vector<CapturedName> CapturedNames() {
		std::vector<CapturedName> out;
		{
			std::lock_guard<std::mutex> lock(g_CaptureMutex);
			for (const auto& [hash, count] : g_Captured) out.push_back(CapturedName{ hash, count, false });
		}
		{
			std::lock_guard<std::mutex> lock(g_Mutex);
			for (auto& c : out) {
				c.ours = std::any_of(g_Scripts.begin(), g_Scripts.end(),
					[&](const ScriptPtr& s) { return s->valid && s->name == c.nameHash; });
			}
		}
		std::sort(out.begin(), out.end(), [](const CapturedName& a, const CapturedName& b) { return a.count > b.count; });
		return out;
	}

	// Walk our own image page by page and copy every committed, readable page into a flat buffer the
	// size of the image, then write it out. Page protections are left alone (VirtualProtect on an
	// Arxan-guarded page can trip its checks); unreadable pages stay zero. RVA X == file offset X.
	std::string DumpDecryptedModule() {
		Initialize();
		if (!g_ModuleBase || !g_ModuleSize) return "Dump failed: module base/size unknown.";

		const std::uintptr_t base = g_ModuleBase;
		const std::size_t size = static_cast<std::size_t>(g_ModuleSize);
		std::vector<std::uint8_t> buf(size, 0);
		std::size_t pos = 0, copied = 0, skipped = 0;
		while (pos < size) {
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery(reinterpret_cast<void*>(base + pos), &mbi, sizeof(mbi)) == 0) {
				pos += 0x1000;
				skipped += 0x1000;
				continue;
			}
			const std::uintptr_t regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
			const std::size_t regionRemain = static_cast<std::size_t>(mbi.RegionSize) - ((base + pos) - regionBase);
			const std::size_t chunk = std::min(regionRemain, size - pos);
			const bool readable = mbi.State == MEM_COMMIT && mbi.Protect != 0 && mbi.Protect != PAGE_NOACCESS
				&& !(mbi.Protect & PAGE_GUARD);
			if (readable) {
				std::memcpy(buf.data() + pos, reinterpret_cast<const void*>(base + pos), chunk);
				copied += chunk;
			} else {
				skipped += chunk;
			}
			pos += chunk;
		}

		std::error_code ec;
		const auto path = std::filesystem::current_path(ec) / "bocw_dump.bin";
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) return "Dump failed: cannot open " + path.string() + " for writing.";
		out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(size));
		LOG("Scripting", INFO, "Module dump written: {} ({} bytes, {} copied, {} skipped, base 0x{:X}).",
			path.string(), size, copied, skipped, base);
		return std::format("Dumped {} bytes -> {} ({} copied / {} skipped)", size, path.string(), copied, skipped);
	}

	namespace {
		// A zone unload can free a chunk under us; a fault just skips that file.
		bool CopyChunk(void* dst, const void* src, std::size_t n) {
			__try { std::memcpy(dst, src, n); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
	}

	std::string DumpLuaFiles() {
		Initialize();
		if (!g_Pools) return "Lua dump failed: xassetpool did not resolve.";
		const CW::XAssetPool& pool = g_Pools[CW::ASSET_TYPE_LUAFILE];
		const auto base = reinterpret_cast<std::uintptr_t>(pool.pool.ptr);
		if (!base || pool.itemSize < sizeof(CW::LuaFile) || pool.itemCount <= 0) {
			return std::format("Lua dump failed: luafile pool looks wrong (base 0x{:X}, item size {}, count {}).",
				base, pool.itemSize, pool.itemCount);
		}

		std::error_code ec;
		const auto dir = std::filesystem::current_path(ec) / "cw-mod" / "lua_dump";
		std::filesystem::create_directories(dir, ec);

		// ui/utility/directorutility.lua: the file this dump is for.
		const std::uint64_t directorUtility = HashScriptName("ui/utility/directorutility.lua");
		bool sawDirectorUtility = false;
		std::size_t live = 0, written = 0, kept = 0, notBytecode = 0, faulted = 0;
		std::vector<std::uint8_t> buf;
		const std::uintptr_t end = base + static_cast<std::uintptr_t>(pool.itemSize) * pool.itemCount;
		for (std::uintptr_t e = base; e < end; e += pool.itemSize) {
			CW::LuaFile file{};
			if (!CopyChunk(&file, reinterpret_cast<const void*>(e), sizeof(file))) { ++faulted; continue; }
			if (!file.name || (file.name >= base && file.name < end)) continue;   // free slot link
			if (!file.buffer || file.len <= 4 || file.len > (64 << 20)) continue;
			++live;
			const std::uint64_t name = file.name & 0x7FFFFFFFFFFFFFFFULL;
			sawDirectorUtility |= name == directorUtility;
			const auto path = dir / std::format("{:016x}.luac", name);
			if (std::filesystem::exists(path, ec)) { ++kept; continue; }
			buf.resize(static_cast<std::size_t>(file.len));
			if (!CopyChunk(buf.data(), file.buffer, buf.size())) { ++faulted; continue; }
			if (std::memcmp(buf.data(), "\x1BLJ", 3) != 0) ++notBytecode;   // T9 chunks are LuaJIT-style
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out) continue;
			out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
			++written;
		}

		const std::string result = std::format("Lua dump: {} chunks loaded, {} written, {} already on disk, "
			"{} not Lua bytecode, {} unreadable -> {}. DirectorUtility ({:016x}) {}.", live, written, kept,
			notBytecode, faulted, dir.string(), directorUtility, sawDirectorUtility ? "included" : "NOT loaded");
		LOG("Scripting", INFO, "{}", result);
		return result;
	}

	int LuaFileLoaded(std::uint64_t nameHash) {
		Initialize();
		if (!g_Pools) return -1;
		const CW::XAssetPool& pool = g_Pools[CW::ASSET_TYPE_LUAFILE];
		const auto base = reinterpret_cast<std::uintptr_t>(pool.pool.ptr);
		if (!base || pool.itemSize < sizeof(CW::LuaFile) || pool.itemCount <= 0) return -1;
		nameHash &= 0x7FFFFFFFFFFFFFFFULL;
		const std::uintptr_t end = base + static_cast<std::uintptr_t>(pool.itemSize) * pool.itemCount;
		for (std::uintptr_t e = base; e < end; e += pool.itemSize) {
			CW::LuaFile file{};
			if (!CopyChunk(&file, reinterpret_cast<const void*>(e), sizeof(file))) continue;
			if (!file.name || (file.name >= base && file.name < end)) continue;   // free slot link
			if ((file.name & 0x7FFFFFFFFFFFFFFFULL) == nameHash) return 1;
		}
		return 0;
	}
}
