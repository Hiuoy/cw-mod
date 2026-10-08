#include "common.hpp"
#include "game/game_internal.hpp"

namespace Client::Game {
	// No C++ objects in the body, which is what lets __try/__except live here (MSVC C2712).
	bool SafeCopy(void* dst, const void* src, std::size_t n) {
		if (!dst || !src) return false;
		__try {
			std::memcpy(dst, src, n);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	std::string HexBytes(const std::uint8_t* p, std::size_t n) {
		std::string out;
		out.reserve(n * 3);
		for (std::size_t i = 0; i < n; ++i) {
			out += std::format("{:02X}", p[i]);
			if (i + 1 < n) out += ' ';
		}
		return out;
	}

	const char* DvarTypeName(int type) {
		switch (type) {
		case Pointers::kDvarType_Bool:   return "bool";
		case Pointers::kDvarType_Float:  return "float";
		case Pointers::kDvarType_Int:    return "int";
		case Pointers::kDvarType_Enum:   return "enum";
		case Pointers::kDvarType_String: return "string";
		case Pointers::kDvarType_Int64:  return "int64";
		case Pointers::kDvarType_UInt64: return "uint64";
		default:                         return "?";
		}
	}

	std::string DvarFlagNames(int flags) {
		std::string s;
		auto bit = [&](int mask, const char* name) {
			if (flags & mask) { if (!s.empty()) s += '|'; s += name; }
		};
		bit(Pointers::kDvarFlag_ReadOnly,       "READONLY");
		bit(Pointers::kDvarFlag_WriteProtected, "WRITEPROTECTED");
		bit(Pointers::kDvarFlag_CheatProtected, "CHEAT");
		bit(Pointers::kDvarFlag_PerGameMode,    "PERGAMEMODE");
		bit(Pointers::kDvarFlag_Undefined,      "UNDEFINED");
		if (s.empty()) s = "-";
		return s;
	}
}
