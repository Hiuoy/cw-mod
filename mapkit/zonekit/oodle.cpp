#include "oodle.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace MapKit::Zone::Oodle {
	namespace {
		// Oodle 2.8 signatures. The decompress arguments match the game's own call in
		// DB_DecompressBlockJob (0x7FF72769F6E0): fuzzSafe=1, checkCRC=0, verbosity=0, threadPhase=3.
		using DecompressFn = std::intptr_t(__stdcall*)(const void* comp, std::intptr_t compLen, void* raw, std::intptr_t rawLen,
			int fuzzSafe, int checkCrc, int verbosity, void* decBufBase, std::intptr_t decBufSize, void* callback,
			void* callbackUserData, void* decoderMemory, std::intptr_t decoderMemorySize, int threadPhase);
		using CompressFn = std::intptr_t(__stdcall*)(int compressor, const void* raw, std::intptr_t rawLen, void* comp,
			int level, const void* options, const void* dictionaryBase, const void* lrm, void* scratch, std::intptr_t scratchSize);

		constexpr int kCompressorKraken = 8;
		constexpr int kLevelNormal = 4;
		constexpr int kThreadPhaseAll = 3;

		HMODULE g_module = nullptr;
		DecompressFn g_decompress = nullptr;
		CompressFn g_compress = nullptr;
	}

	bool Load(const std::filesystem::path& gameDir, std::string& error) {
		if (g_module) {
			return true;
		}

		const std::filesystem::path dll = gameDir / "oo2core_8_win64.dll";
		g_module = LoadLibraryW(dll.c_str());
		if (!g_module) {
			error = "could not load " + dll.string() + " (pass --game <Black Ops Cold War folder>)";
			return false;
		}

		g_decompress = reinterpret_cast<DecompressFn>(GetProcAddress(g_module, "OodleLZ_Decompress"));
		g_compress = reinterpret_cast<CompressFn>(GetProcAddress(g_module, "OodleLZ_Compress"));
		if (!g_decompress || !g_compress) {
			error = dll.string() + " does not export OodleLZ_Decompress/OodleLZ_Compress";
			FreeLibrary(g_module);
			g_module = nullptr;
			return false;
		}
		return true;
	}

	bool IsLoaded() {
		return g_module != nullptr;
	}

	bool Decompress(std::span<const std::uint8_t> src, std::span<std::uint8_t> dst) {
		if (!g_decompress) {
			return false;
		}
		const std::intptr_t written = g_decompress(src.data(), static_cast<std::intptr_t>(src.size()), dst.data(),
			static_cast<std::intptr_t>(dst.size()), 1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, kThreadPhaseAll);
		return written == static_cast<std::intptr_t>(dst.size());
	}

	bool Compress(std::span<const std::uint8_t> src, std::vector<std::uint8_t>& dst) {
		if (!g_compress) {
			return false;
		}
		// Kraken's worst case is the raw size plus a few hundred bytes per 256 KB chunk.
		dst.resize(src.size() + (src.size() >> 6) + 0x1000);
		const std::intptr_t written = g_compress(kCompressorKraken, src.data(), static_cast<std::intptr_t>(src.size()),
			dst.data(), kLevelNormal, nullptr, nullptr, nullptr, nullptr, 0);
		if (written <= 0) {
			dst.clear();
			return false;
		}
		dst.resize(static_cast<std::size_t>(written));
		return true;
	}
}
