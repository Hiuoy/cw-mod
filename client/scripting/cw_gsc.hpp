#pragma once
#include <cstddef>
#include <cstdint>

// -----------------------------------------------------------------------------
// Call of Duty: Black Ops Cold War (T9) GSC/CSC runtime structures.
//
// These layouts are salvaged from atian-cod-tools (ACTS) by ate47, MIT-licensed
// portion: src/dll/bocw-dll/data/cw.hpp
//   https://github.com/ate47/atian-cod-tools  (MIT / GPL-3 dual-license)
// Reused here under the MIT terms with attribution. Thanks to ate47 and the
// ACTS contributors for the T9 reverse-engineering work.
//
// This is the compiled-GSC object format (T9GSCOBJ), the script VM value/stack
// types, the XAsset pool layout, and the SCRIPTPARSETREE asset (type 68) that the
// loader in scripting.cpp serves and injects. Shapes only, no behavior.
// -----------------------------------------------------------------------------

namespace Client::Scripting::CW {
	// Which script VM a call targets (server vs client). ACTS: scriptinstance::ScriptInstance.
	using ScriptInstance = std::uint32_t;
	enum : std::uint32_t {
		SCRIPTINSTANCE_SERVER = 0,
		SCRIPTINSTANCE_CLIENT = 1,
	};

	// Compiled GSC object header ("GSC OBJ", VM 0x38). All table offsets are relative to `magic`.
	// Layout per ACTS tools/gsc/data/gsc_data_t9.hpp; the fields the engine reads were confirmed
	// in the IDB (Scr_GscObjUnlink reads includes @+0x24/+0x34, exports @+0x1A/+0x38; the import
	// fixup reads imports @+0x1C/+0x3C).
	struct T9GSCOBJ {
		std::uint8_t magic[8];
		std::int32_t crc;
		std::int32_t pad0c;
		std::uint64_t name;             // FNV-63 of "scripts/.../file.gsc" (extension included)
		std::uint16_t string_count;
		std::uint16_t exports_count;
		std::uint16_t imports_count;
		std::uint16_t unk1e;
		std::uint16_t globalvar_count;
		std::uint16_t unk22;
		std::uint16_t includes_count;
		std::uint16_t devblock_string_count;
		std::uint32_t devblock_string_offset;
		std::uint32_t cseg_offset;
		std::uint32_t string_offset;
		std::uint32_t includes_table;   // u64[includes_count], each hash & 0x7FFF...
		std::uint32_t exports_tables;
		std::uint32_t import_tables;
		std::uint32_t unk40;
		std::uint32_t globalvar_offset;
		std::uint32_t file_size;
		std::uint32_t unk4c;
		std::uint32_t cseg_size;
		std::uint32_t unk54;
	};
	static_assert(sizeof(T9GSCOBJ) == 0x58);

	// One string-table entry, followed in the object by `num_address` u32 offsets. Each offset
	// points at the 4-byte-aligned operand of a GetString opcode, which must hold the string's SL
	// id. ACTS writes 0x12345678 there; the engine only interns strings of scripts it loaded from
	// a fastfile, so a buffer we serve must be fixed up by us (see FixupStrings).
	struct T8GSCString {
		std::uint32_t string;      // offset of the text: 3-byte header (0x8B, len+1, 0) then chars
		std::uint8_t num_address;
		std::uint8_t type;
		std::uint16_t pad;
	};
	static_assert(sizeof(T8GSCString) == 8);

	struct T8GSCExport {
		std::uint32_t checksum;
		std::uint32_t address;
		std::uint32_t name;
		std::uint32_t name_space;
		std::uint32_t callback_event;
		std::uint8_t param_count;
		std::uint8_t flags;
		std::uint16_t padding;
	};

	enum T9ScrVarType : std::uint32_t {
		TYPE_UNDEFINED = 0x0,
		TYPE_POINTER = 0x1,
		TYPE_STRING = 0x2,
		TYPE_VECTOR = 0x3,
		TYPE_HASH = 0x4,
		TYPE_FLOAT = 0x5,
		TYPE_INTEGER = 0x6,
		TYPE_FINALIZATION = 0x7,
		TYPE_UINTPTR = 0x8,
		TYPE_ENTITY_OFFSET = 0x9,
		TYPE_CODEPOS = 0xA,
		TYPE_PRECODEPOS = 0xB,
		TYPE_API_FUNCTION = 0xC,
		TYPE_SCRIPT_FUNCTION = 0xD,
		TYPE_STACK = 0xE,
		TYPE_THREAD = 0xF,
		TYPE_NOTIFY_THREAD = 0x10,
		TYPE_TIME_THREAD = 0x11,
		TYPE_FRAME_THREAD = 0x12,
		TYPE_CHILD_THREAD = 0x13,
		TYPE_REMOVED_THREAD = 0x14,
		TYPE_ARRAY = 0x15,
		TYPE_CLASS = 0x16,
		TYPE_SHARED_STRUCT = 0x17,
		TYPE_STRUCT = 0x18,
		TYPE_REMOVED_ENTITY = 0x19,
		TYPE_ENTITY = 0x1A,
		TYPE_FREE = 0x1B,
		TYPE_THREAD_LIST = 0x1C,
		TYPE_ENT_LIST = 0x1D,
	};

	// Only the members we actually reference. SCRIPTPARSETREE = 68 is the compiled-GSC asset.
	// LUAFILE = 124: LuaFile_LoadAsset (0x7FF729642580) looks LUI chunks up under it.
	enum XAssetType : std::uint8_t {
		ASSET_TYPE_MATERIAL = 10,
		ASSET_TYPE_SCRIPTPARSETREE = 68,
		ASSET_TYPE_SCRIPTPARSETREEDBG = 69,
		ASSET_TYPE_LUAFILE = 124,
		ASSET_TYPE_COUNT = 221,
	};

	struct ScriptParseTree {
		std::uint64_t name;
		T9GSCOBJ* buffer;
		std::int32_t len;
	};

	// A compiled LUI chunk. name = hash of "<path>.lua" (a "x64:<hash>.lua" chunk name continues
	// FNV from <hash> over ".lua"). The lua_load reader hands out buffer[len] in one piece.
	struct LuaFile {
		std::uint64_t name;
		std::int32_t len;
		const std::uint8_t* buffer;
	};
	static_assert(offsetof(LuaFile, buffer) == 0x10);

	union XAssetHeader {
		void* ptr;
		ScriptParseTree* spt;
	};

	using ScrVarIndex = std::uint32_t;
	using ScrString = std::uint32_t;

	union ScrVarValueUnion {
		std::int64_t intValue;
		std::uintptr_t uintptrValue;
		float floatValue;
		ScrString stringValue;
		const float* vectorValue;
		std::uint8_t* codePosValue;
		ScrVarIndex pointerValue;
	};

	struct __declspec(align(8)) ScrVarValue {
		ScrVarValueUnion u;
		T9ScrVarType type;
	};

	// The GSC VM operand stack.
	struct FunctionStack {
		std::uint8_t* pos;
		ScrVarValue* top;
		ScrVarValue* startTop;
		ScrVarIndex threadId;
		std::uint16_t localVarCount;
		std::uint16_t profileInfoCount;
	};

	// One entry per loaded compiled-GSC object; the loader walks this to resolve links.
	struct __declspec(align(8)) ObjFileInfo {
		T9GSCOBJ* activeVersion;
		std::int32_t slot;
		std::int32_t refCount;
		std::uint32_t groupId;
	};

	using ObjFileInfoStruct = ObjFileInfo[800];

	// One pool per asset type, 32 bytes each (DB_AllocXAssetPoolEntry indexes the array by type*32).
	// Free slots are chained through their first qword via freeHead.
	struct XAssetPool {
		XAssetHeader pool;
		unsigned int itemSize;
		int itemCount;
		bool isSingleton;
		int itemAllocCount;
		void* freeHead;
	};
	static_assert(sizeof(XAssetPool) == 32);

	// --- resolved function signatures ---
	using DB_FindXAssetHeaderT = XAssetHeader (*)(XAssetType type, std::uint64_t name, bool includeOverride, int waitTime);
	using Scr_GscObjLinkT = void (*)(ScriptInstance inst, std::uint64_t scriptname);
	// SL_GetString(text, user, type, decrypt) -> SL id. Arxan caller-guarded: called from outside
	// the image it inverts the hash and files the string in the wrong bucket, so it is only ever
	// called through an ArxanCall thunk. user 0 = a plain refcounted entry no user shutdown frees.
	using SL_GetStringT = std::uint32_t (*)(const char* text, std::uint8_t user, int type, bool decrypt);
}
