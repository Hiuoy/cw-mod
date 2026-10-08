// The engine name hash, the dvar registry, and the runtime name-recovery scan.
#include "common.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace Client::Game {
	// FNV-1a-64 over the name with ASCII A-Z lowercased, masked to 63 bits. See the commentary on
	// the declaration in game.hpp for how this was verified against build 1.34.0.15931218.
	std::uint64_t Pointers::HashString(const char* name) {
		if (!name) return 0;
		std::uint64_t v = 0xCBF29CE484222325ULL;
		for (; *name; ++name) {
			unsigned char c = static_cast<unsigned char>(*name);
			if (c >= 'A' && c <= 'Z') c += 32;
			v = (v ^ c) * 0x100000001B3ULL;
		}
		return v & 0x7FFFFFFFFFFFFFFFULL;
	}

	std::uintptr_t* Pointers::FindDvar(const char* name) const {
		if (!this->m_Dvar_FindVar || !name || !*name) return nullptr;
		return this->m_Dvar_FindVar(Pointers::HashString(name));
	}

	std::string Pointers::DescribeDvar(const char* name) const {
		if (!name || !*name) return "Enter a dvar name.";
		const std::uint64_t hash = Pointers::HashString(name);

		if (!this->m_Dvar_FindVar) {
			return std::format("{}\nhash = 0x{:016X}\nDvar_FindVar signature did not resolve on this build.",
				name, hash);
		}

		std::uintptr_t* dvar = this->m_Dvar_FindVar(hash);
		if (!dvar) {
			return std::format("{}\nhash = 0x{:016X}\nNOT REGISTERED on this build.\n"
				"(If you got this name from tools/wordlists/massive_dvar_dump.txt, remember that file is "
				"from a different CoD — the names are a wordlist, not a guarantee.)", name, hash);
		}

		const std::uint8_t* d = reinterpret_cast<const std::uint8_t*>(dvar);
		int type = 0, flags = 0;
		const std::uint8_t* values = nullptr;
		SafeRead(d + kDvar_Type, type);
		SafeRead(d + kDvar_Flags, flags);
		SafeRead(d + kDvar_Values, values);

		std::string out = std::format("{}\nhash = 0x{:016X}\nDvar @ 0x{:X}\ntype = {} ({})\nflags = 0x{:X} ({})",
			name, hash, reinterpret_cast<std::uintptr_t>(dvar), type, DvarTypeName(type), flags,
			DvarFlagNames(flags));

		// The value block holds a plaintext copy in its first 16 bytes and an obfuscated/tagged
		// copy after it (Dvar_SetInt writes both: v26[0] = plaintext, v26[1] = obfuscated). We read
		// only the plaintext copy, and label it as such — the engine treats the obfuscated one as
		// authoritative, so a mismatch here would mean the value was written behind the setter.
		if (values) {
			std::uint32_t raw = 0;
			if (SafeRead(values, raw)) {
				float asFloat = 0.0f;
				std::memcpy(&asFloat, &raw, sizeof(asFloat));
				out += std::format("\nvalue[gamemode 0] raw = {} / 0x{:X} / {:g}f  (plaintext copy)",
					static_cast<std::int32_t>(raw), raw, asFloat);
			}
		}
		return out;
	}

	// Shared guts of SetDvarInt/SetDvarBool: resolve, refuse the obviously-doomed writes with an
	// explanation, then hand off to the caller's setter. Returning a sentence rather than a bool is
	// deliberate — with no console, the menu is the only place a failure can be explained.
	std::string Pointers::SetDvarInt(const char* name, int value) {
		if (!this->m_Dvar_SetIntFromSource) return "Dvar_SetIntFromSource did not resolve on this build.";
		std::uintptr_t* dvar = this->FindDvar(name);
		if (!dvar) return std::format("'{}' is not registered (hash 0x{:016X}).", name ? name : "",
			Pointers::HashString(name));

		int flags = 0;
		SafeRead(reinterpret_cast<const std::uint8_t*>(dvar) + kDvar_Flags, flags);
		if (flags & kDvarFlag_ReadOnly)       return std::format("'{}' is READONLY; the engine will reject the write.", name);
		if (flags & kDvarFlag_WriteProtected) return std::format("'{}' is WRITEPROTECTED; the engine will reject the write.", name);

		this->WriteDvarInt(dvar, value);
		LOG("Pointers", INFO, "SetDvarInt: {} = {} (dvar @ 0x{:X}).", name, value,
			reinterpret_cast<std::uintptr_t>(dvar));
		return std::format("Set '{}' = {}{}", name, value,
			(flags & kDvarFlag_CheatProtected) ? "  (cheat-protected: may need sv_cheats)" : "");
	}

	// The flag-0x400 bracket. Save/restore rather than clear: the engine's own packet-apply path
	// (Dvar_ApplyServerDvarPacket) holds this latch across a batch, and zeroing it unconditionally
	// would break that batch instead of ours.
	namespace {
		class FlaggedDvarWriteScope {
		public:
			explicit FlaggedDvarWriteScope(std::uint8_t* latch) : m_latch(latch), m_prev(0) {
				if (m_latch) {
					m_prev = *m_latch;
					*m_latch = 1;
				}
			}
			~FlaggedDvarWriteScope() {
				if (m_latch) *m_latch = m_prev;
			}
			FlaggedDvarWriteScope(const FlaggedDvarWriteScope&) = delete;
			FlaggedDvarWriteScope& operator=(const FlaggedDvarWriteScope&) = delete;

		private:
			std::uint8_t* m_latch;
			std::uint8_t  m_prev;
		};
	}

	void Pointers::WriteDvarBool(std::uintptr_t* dvar, bool value) {
		if (!this->m_Dvar_SetBoolFromSource || !dvar) return;
		FlaggedDvarWriteScope scope(this->m_g_dvarAllowServerFlaggedWrites);
		this->m_Dvar_SetBoolFromSource(dvar, value, 0);
	}

	void Pointers::WriteDvarInt(std::uintptr_t* dvar, int value) {
		if (!this->m_Dvar_SetIntFromSource || !dvar) return;
		FlaggedDvarWriteScope scope(this->m_g_dvarAllowServerFlaggedWrites);
		this->m_Dvar_SetIntFromSource(dvar, value, 0);
	}

	bool Pointers::WriteDvarString(std::uintptr_t* dvar, const char* value) {
		if (!this->m_Dvar_StringToValue || !this->m_Dvar_ApplyValueInternal || !dvar || !value) return false;
		const auto* d = reinterpret_cast<const std::uint8_t*>(dvar);
		int type = 0;
		if (!SafeRead(d + kDvar_Type, type) || type != kDvarType_String) return false;
		alignas(16) std::uint8_t domain[16]{};
		if (!SafeCopy(domain, d + kDvar_Domain, sizeof(domain))) return false;

		// The value holds the text pointer plus the obfuscated check word; ApplyValueInternal copies
		// the text into engine memory, so `value` only has to outlive the call.
		alignas(16) std::uint8_t built[32]{};
		const void* v = this->m_Dvar_StringToValue(built, type, domain, value);
		FlaggedDvarWriteScope scope(this->m_g_dvarAllowServerFlaggedWrites);
		this->m_Dvar_ApplyValueInternal(dvar, v ? v : built, 0);
		return true;
	}

	std::string Pointers::ReadDvarString(const std::uintptr_t* dvar) const {
		const auto* d = reinterpret_cast<const std::uint8_t*>(dvar);
		int type = 0, flags = 0;
		const std::uint8_t* values = nullptr;
		if (!d || !SafeRead(d + kDvar_Type, type) || type != kDvarType_String) return {};
		SafeRead(d + kDvar_Flags, flags);
		if (!SafeRead(d + kDvar_Values, values) || !values) return {};

		// As Dvar_GetStringValue: the session's gameMode slot for per-gamemode dvars (4 -> 0), else 0.
		std::size_t slot = 0;
		std::uint32_t w = 0;
		if ((flags & kDvarFlag_PerGameMode) && this->m_g_sessionModePacked
			&& SafeRead(this->m_g_sessionModePacked, w) && (w & 0xF) != 4)
			slot = w & 0xF;
		const char* s = nullptr;
		if (!SafeRead(values + 96 * slot, s) || !s) return {};
		std::string out;
		for (char c = 0; out.size() < 255 && SafeRead(s + out.size(), c) && c; ) out.push_back(c);
		return out;
	}

	std::string Pointers::SetDvarBool(const char* name, bool value) {
		if (!this->m_Dvar_SetBoolFromSource) return "Dvar_SetBoolFromSource did not resolve on this build.";
		std::uintptr_t* dvar = this->FindDvar(name);
		if (!dvar) return std::format("'{}' is not registered (hash 0x{:016X}).", name ? name : "",
			Pointers::HashString(name));

		int flags = 0;
		SafeRead(reinterpret_cast<const std::uint8_t*>(dvar) + kDvar_Flags, flags);
		if (flags & kDvarFlag_ReadOnly)       return std::format("'{}' is READONLY; the engine will reject the write.", name);
		if (flags & kDvarFlag_WriteProtected) return std::format("'{}' is WRITEPROTECTED; the engine will reject the write.", name);

		this->WriteDvarBool(dvar, value);
		LOG("Pointers", INFO, "SetDvarBool: {} = {} (dvar @ 0x{:X}).", name, value,
			reinterpret_cast<std::uintptr_t>(dvar));
		return std::format("Set '{}' = {}{}", name, value ? "true" : "false",
			(flags & kDvarFlag_CheatProtected) ? "  (cheat-protected: may need sv_cheats)" : "");
	}

	std::string Pointers::DumpAllDvars() const {
		if (!this->m_g_dvarHashTable) return "g_dvarHashTable signature did not resolve on this build.";

		// Read-only walk. We do NOT take the engine's spinlock: acquiring it means a CAS loop that
		// can yield to the scheduler, and holding it across several thousand SafeReads would stall
		// every other dvar consumer. A dvar being registered mid-walk can only add a node, so the
		// worst case is one missing row, not a bad read — and every deref is SEH-guarded anyway.
		std::string body;
		std::size_t count = 0, faults = 0;

		for (std::size_t bucket = 0; bucket < kDvarBucketCount; ++bucket) {
			const std::uint8_t* node = nullptr;
			if (!SafeRead(this->m_g_dvarHashTable + bucket, node)) { ++faults; continue; }

			// Cap the chain walk: a torn/garbage `next` must not spin forever inside a game-thread
			// callback. No bucket in a 1024-way table over a few thousand dvars comes near this.
			for (std::size_t depth = 0; node && depth < 4096; ++depth) {
				std::uint64_t hash = 0;
				int type = 0, flags = 0;
				const std::uint8_t* next = nullptr;

				if (!SafeRead(node + kDvar_Hash, hash)) { ++faults; break; }
				SafeRead(node + kDvar_Type, type);
				SafeRead(node + kDvar_Flags, flags);
				if (!SafeRead(node + kDvar_Next, next)) next = nullptr;

				body += std::format("0x{:016X}  {:<7} 0x{:<6X} {:<16} 0x{:X}\n",
					hash & 0x7FFFFFFFFFFFFFFFULL, DvarTypeName(type), flags, DvarFlagNames(flags),
					reinterpret_cast<std::uintptr_t>(node));
				++count;
				node = next;
			}
		}

		// Non-throwing throughout: this runs inside a game-thread callback, where an escaping
		// std::filesystem_error would take the game down rather than show a message.
		std::error_code ec;
		const std::filesystem::path dir = std::filesystem::current_path(ec) / "cw-mod";
		std::filesystem::create_directories(dir, ec);
		const std::filesystem::path path = dir / "dvars.txt";

		std::ofstream out(path, std::ios::trunc);
		if (!out) {
			return std::format("Walked {} dvars ({} faulted reads) but could not open {} for writing.",
				count, faults, path.string());
		}
		out << "# cw-mod dvar registry dump - build " << g_GameIdentifier.m_Version << "\n"
			<< "# Hashes are FNV-1a-64 (basis 0xCBF29CE484222325, prime 0x100000001B3), name\n"
			<< "# lowercased A-Z, masked to 63 bits. To recover names, hash a wordlist the same\n"
			<< "# way and match against column 1.\n"
			<< "# hash                type    flags    flagnames        address\n"
			<< body;
		out.close();

		LOG("Pointers", INFO, "DumpAllDvars: {} dvars -> {} ({} faulted reads).", count, path.string(), faults);
		return std::format("Dumped {} registered dvars -> {}{}", count, path.string(),
			faults ? std::format("  ({} faulted reads skipped)", faults) : "");
	}

	namespace {
		// The alphabet a T9 name can be built from. Deliberately NOT including '/' or '\' so that
		// an asset path breaks into its components on its own - "ui/menus/lobby" yields "lobby",
		// and it is the component, not the path, that gets hashed into a name.
		inline bool IsNameByte(unsigned char c) {
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
				|| c == '_' || c == '.' || c == '-';
		}

		constexpr std::size_t kHarvestMinLen   = 3;
		constexpr std::size_t kHarvestMaxLen   = 128;
		constexpr std::size_t kHarvestChunk    = 1u << 20;   // 1 MiB per SEH-guarded copy

		// Incremental FNV-1a-63 so a candidate is hashed exactly once, as it is scanned, instead of
		// being materialised into a std::string first. At a few hundred million bytes of heap the
		// allocation is the whole cost, so this is what makes the scan finish in seconds.
		inline std::uint64_t HashRun(const unsigned char* p, std::size_t n) {
			std::uint64_t v = 0xCBF29CE484222325ULL;
			for (std::size_t i = 0; i < n; ++i) {
				unsigned char c = p[i];
				if (c >= 'A' && c <= 'Z') c += 32;
				v = (v ^ c) * 0x100000001B3ULL;
			}
			return v & 0x7FFFFFFFFFFFFFFFULL;
		}

		// Extra hashes to hunt for, one "0x..." per line, rest of the line ignored - so a dumped
		// hash list (cw-mod/dvars.txt, tools/wordlists/lui_menu_hashes.txt) can be dropped in unedited.
		std::size_t LoadWantedHashes(const std::filesystem::path& path,
			std::unordered_map<std::uint64_t, std::string>& out) {
			std::ifstream in(path);
			if (!in) return 0;
			std::size_t added = 0;
			std::string line;
			while (std::getline(in, line)) {
				const std::size_t at = line.find_first_not_of(" \t");
				if (at == std::string::npos || line[at] == '#') continue;
				if (line.compare(at, 2, "0x") != 0 && line.compare(at, 2, "0X") != 0) continue;
				const std::uint64_t h = std::strtoull(line.c_str() + at + 2, nullptr, 16);
				if (h && out.emplace(h & 0x7FFFFFFFFFFFFFFFULL, std::string{}).second) ++added;
			}
			return added;
		}
	}

	std::string Pointers::HarvestNamesFromMemory() const {
		// Every hash we would like a name for. The live dvar registry is the free half of this: it
		// is already in memory, already build-accurate, and needs no file.
		std::unordered_map<std::uint64_t, std::string> wanted;
		std::size_t fromRegistry = 0;

		if (this->m_g_dvarHashTable) {
			for (std::size_t bucket = 0; bucket < kDvarBucketCount; ++bucket) {
				const std::uint8_t* node = nullptr;
				if (!SafeRead(this->m_g_dvarHashTable + bucket, node)) continue;
				for (std::size_t depth = 0; node && depth < 4096; ++depth) {
					std::uint64_t hash = 0;
					const std::uint8_t* next = nullptr;
					if (!SafeRead(node + kDvar_Hash, hash)) break;
					if (wanted.emplace(hash & 0x7FFFFFFFFFFFFFFFULL, std::string{}).second) ++fromRegistry;
					if (!SafeRead(node + kDvar_Next, next)) next = nullptr;
					node = next;
				}
			}
		}

		std::error_code ec;
		const std::filesystem::path dir = std::filesystem::current_path(ec) / "cw-mod";
		std::filesystem::create_directories(dir, ec);
		const std::size_t fromFile = LoadWantedHashes(dir / "hashes_wanted.txt", wanted);

		if (wanted.empty()) {
			return "Nothing to look for: the dvar hash table did not resolve and cw-mod/"
				"hashes_wanted.txt is missing or has no 0x... lines.";
		}

		// Walk our own address space. Everything committed and readable is fair game: the point of
		// scanning the live process rather than the exe dump is precisely the memory the exe does
		// not contain - decompressed fastfile zones, the Lua heap, asset name pools.
		std::vector<unsigned char> buf(kHarvestChunk + kHarvestMaxLen);
		std::size_t carry = 0;              // bytes of a run straddling the previous chunk
		std::uint64_t scanned = 0, regions = 0, faults = 0;
		std::size_t found = 0;

		MEMORY_BASIC_INFORMATION mbi{};
		for (std::uintptr_t addr = 0; addr < 0x00007FFFFFFF0000ULL; ) {
			if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
			const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
			const std::uintptr_t end  = base + mbi.RegionSize;
			if (end <= addr) break;                      // no forward progress: stop rather than spin
			addr = end;

			if (mbi.State != MEM_COMMIT) continue;
			if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
			const DWORD prot = mbi.Protect & 0xFF;
			const bool readable = prot == PAGE_READONLY || prot == PAGE_READWRITE
				|| prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ
				|| prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
			if (!readable) continue;
			++regions;

			carry = 0;
			for (std::uintptr_t p = base; p < end; p += kHarvestChunk) {
				const std::size_t n = static_cast<std::size_t>(std::min<std::uintptr_t>(kHarvestChunk, end - p));
				if (!SafeCopy(buf.data() + carry, reinterpret_cast<const void*>(p), n)) {
					++faults;
					carry = 0;
					continue;
				}
				scanned += n;

				const std::size_t total = carry + n;
				std::size_t i = 0, runStart = 0;
				bool inRun = carry != 0;
				for (; i < total; ++i) {
					if (IsNameByte(buf[i])) {
						if (!inRun) { inRun = true; runStart = i; }
						continue;
					}
					if (inRun) {
						const std::size_t len = i - runStart;
						if (len >= kHarvestMinLen && len <= kHarvestMaxLen) {
							auto it = wanted.find(HashRun(buf.data() + runStart, len));
							if (it != wanted.end() && it->second.empty()) {
								it->second.assign(reinterpret_cast<const char*>(buf.data() + runStart), len);
								++found;
							}
						}
						inRun = false;
					}
				}

				// A run touching the end of the chunk may continue into the next one. Keep its tail
				// (bounded by the longest name we accept) and prepend it to the following copy, so a
				// name is never lost or truncated at a 1 MiB boundary.
				carry = 0;
				if (inRun) {
					const std::size_t len = total - runStart;
					if (len <= kHarvestMaxLen) {
						std::memmove(buf.data(), buf.data() + runStart, len);
						carry = len;
					}
				}
			}
		}

		const std::filesystem::path path = dir / "names_recovered.txt";
		std::ofstream out(path, std::ios::trunc);
		if (!out) {
			return std::format("Recovered {} of {} names but could not open {} for writing.",
				found, wanted.size(), path.string());
		}
		out << "# cw-mod runtime name recovery - build " << g_GameIdentifier.m_Version << "\n"
			<< "# Every printable name-shaped run in this process's committed memory, hashed with the\n"
			<< "# engine's FNV-1a-63 and joined against " << wanted.size() << " wanted hashes\n"
			<< "# (" << fromRegistry << " from the live dvar registry, " << fromFile
			<< " from cw-mod/hashes_wanted.txt).\n"
			<< "# Scanned " << (scanned >> 20) << " MiB across " << regions << " regions.\n"
			<< "# hash                name\n";
		std::size_t written = 0;
		for (const auto& [hash, name] : wanted) {
			if (name.empty()) continue;
			out << std::format("0x{:016X}  {}\n", hash, name);
			++written;
		}
		out.close();

		LOG("Pointers", INFO, "HarvestNamesFromMemory: {}/{} names, {} MiB, {} regions, {} faults -> {}",
			found, wanted.size(), scanned >> 20, regions, faults, path.string());
		return std::format("Recovered {} of {} names ({} from the dvar registry, {} from file). "
			"Scanned {} MiB across {} regions{}. -> {}",
			written, wanted.size(), fromRegistry, fromFile, scanned >> 20, regions,
			faults ? std::format(", {} chunks faulted", faults) : "", path.string());
	}
}
