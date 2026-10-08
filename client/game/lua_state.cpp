// Describes an erroring Lua call stack for the luaL_traceback detour: frames, decoded instructions
// and argument slots, read while the stack still exists. Read-only - raw reads plus the two
// non-pushing debug calls - so nothing here can run game script or raise. See the ResolveLuaApi
// commentary in anchors.cpp for why every engine accessor is wrapped in an Arxan thunk.
#include "common.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <unordered_map>
#include <vector>

namespace Client::Game {
	// --- Lua state walking ----------------------------------------------------------------------
	//
	// Everything below is read-only with respect to the game: it pushes values onto the Lua stack,
	// reads them, and puts L->top back exactly where it found it. It never calls lua_call and never
	// uses lua_gettable on the walk, so no Lua metamethod and no game script can run as a side
	// effect, and nothing can raise a Lua error out from under us.
	namespace {
		std::uint64_t** LuaTopSlot(void* L) {
			return reinterpret_cast<std::uint64_t**>(
				reinterpret_cast<std::uint8_t*>(L) + Pointers::kLuaState_Top);
		}

		// L->stack_last, the allocated end of the value stack. lua_getfield compares L->top (+56)
		// against this (+40) after every push, so it is the same bound the engine respects.
		constexpr std::size_t kLuaState_StackLast = 40;

		// L->base. lua_index2adr resolves a positive index as `*(L+80) + 8*idx - 8`, bounded by
		// L->top, which is Lua 5.1's `o = L->base + (idx - 1)`. We need it because luaD_growstack
		// REALLOCATES the value stack and fixes up L->top/L->base to point into the new buffer: a
		// raw L->top saved before a push is a dangling pointer afterwards, so the stack top has to
		// be saved and restored as a base-relative depth, never as an absolute pointer.
		constexpr std::size_t kLuaState_Base = 80;

		// L->stack, the ALLOCATED base of the value stack - not L->base, which moves with the
		// current frame. lua_getstack packs its frame locator as an index relative to this one
		// (`(frame - *(L+32)) >> 3`), so reading another frame's slots has to start here.
		constexpr std::size_t kLuaState_Stack = 32;

		std::uint64_t** LuaBaseSlot(void* L) {
			return reinterpret_cast<std::uint64_t**>(
				reinterpret_cast<std::uint8_t*>(L) + kLuaState_Base);
		}

		int LuaTagOf(std::uint64_t v) {
			return static_cast<int>(static_cast<std::int64_t>(v) >> Pointers::kLuaTagShift);
		}

		// Tagged values live in the top of the negative tag space; anything else is a double. A
		// negative double sits far below this range (-1.0 tags as -8224), so the window is safe.
		// The engine's own bound is lua_type's `(unsigned)tag <= 0xFFFFFFF1 -> it is a number`,
		// i.e. tags -14..-1; ours is one wider on the low side, which only ever makes us print a
		// "tag NN" line instead of a number.
		bool LuaIsTagged(int tag) { return tag <= -1 && tag >= -16; }

		// The engine's OWN nil TValue, rather than one we synthesize.
		//
		// lua_index2adr returns `*(L+24) + 304` for any out-of-range index, and lua_type reports
		// that exact address as LUA_TNONE (-1) - which identifies it as luaO_nilobject, a real
		// canonical nil the engine built. `*(L+24)` is the global_State; +288 next to it is the
		// scratch TValue index2adr uses for the pseudo-indices.
		//
		// Why this matters rather than just writing tag -1 with a zero payload: nothing guarantees
		// the engine's nil test is a tag compare instead of an equality compare against this
		// constant, and being wrong is NOT a quiet failure. lua_next -> luaH_next -> findindex
		// starts a fresh iteration only if the key tests as nil; otherwise it treats it as a real
		// key, fails to find it, and raises "invalid key to 'next'" through luaG_runerror. That is
		// a Lua throw with no protected call frame anywhere above us, i.e. fatal.
		constexpr std::size_t kLuaState_G      = 24;   // -> global_State
		constexpr std::size_t kLuaG_NilObject  = 304;  // luaO_nilobject within it

		bool LuaCanonicalNil(void* L, std::uint64_t& out) {
			std::uint8_t* G = nullptr;
			if (!SafeRead(reinterpret_cast<std::uint8_t*>(L) + kLuaState_G, G) || !G) return false;
			std::uint64_t v = 0;
			if (!SafeRead(G + kLuaG_NilObject, v)) return false;
			if (LuaTagOf(v) != Pointers::kLuaTag_Nil) return false;   // not the object we think
			out = v;
			return true;
		}

		bool SafeLuaGetStack(const Pointers& p, void* L, int level, void* ar, int& out) {
			__try { out = p.m_lua_getstack(L, level, ar); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		bool SafeLuaGetInfo(const Pointers& p, void* L, const char* what, void* ar) {
			__try { p.m_lua_getinfo(L, what, ar, 0); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		// TString: length at +16, characters at +24. Copied a byte at a time through SafeRead so a
		// bogus payload or a string straddling the end of a page degrades instead of faulting.
		//
		// The length is read as 32 bits, NOT as a size_t. Reading all 8 bytes made roughly one key
		// in ten render as its correct text followed by a few hundred bytes of heap garbage
		// ("addmenu", "m_eventHandlers", "CoDRoot" ...): the prefix and the data pointer were right
		// and only the count was wrong, so the high dword at +20 is not part of the field on this
		// build. Also stopped at the first NUL as a second line of defence - luaS_newlstr allocates
		// len+1 and terminates, so a correct length never reaches one, and a string with a genuine
		// embedded NUL is not a thing any LUI key is.
		bool LuaReadString(std::uint64_t payload, std::string& out) {
			const auto ts = reinterpret_cast<const std::uint8_t*>(payload);
			std::uint32_t len = 0;
			if (!SafeRead(ts + Pointers::kLuaTString_Len, len)) return false;
			if (len > 256) len = 256;   // display cap; menu and field names are short

			out.clear();
			out.reserve(len);
			for (std::uint32_t i = 0; i < len; ++i) {
				char c = 0;
				if (!SafeRead(ts + Pointers::kLuaTString_Data + i, c)) break;
				if (c == '\0') break;
				out.push_back(c);
			}
			return true;
		}

		// Speculatively read `candidate` as a TString and accept it only if the result is clean
		// printable text. Used to probe the slots of objects whose layout we have not confirmed:
		// a wrong guess reads a length and bytes from unrelated memory, which essentially never
		// produces a run of printable ASCII of a plausible length, so false positives are rare and
		// a false negative just means we fall back to the hash.
		bool LuaTryString(std::uint64_t candidate, std::string& out) {
			const auto ts = reinterpret_cast<const std::uint8_t*>(candidate);
			std::uint32_t len = 0;
			if (!SafeRead(ts + Pointers::kLuaTString_Len, len)) return false;
			if (len == 0 || len > 64) return false;

			std::string s;
			s.reserve(len);
			for (std::uint32_t i = 0; i < len; ++i) {
				char c = 0;
				if (!SafeRead(ts + Pointers::kLuaTString_Data + i, c)) return false;
				if (c < 0x20 || c > 0x7E) return false;
				s.push_back(c);
			}
			char term = 1;
			if (!SafeRead(ts + Pointers::kLuaTString_Data + len, term) || term != '\0') return false;
			out = s;
			return true;
		}

		// One-line rendering of the TValue at `slot`. `stackIdx` is the same value's Lua stack
		// index, used only to ask lua_type for its opinion - we print that number alongside our own
		// tag decode so the tag->type mapping can be confirmed from a single in-game run rather
		// than inferred from the nibble LUT.
		// Describe a closure, and for a C closure recover the RVA of the native behind it.
		//
		// This is the escape hatch from T9's hashed identifiers. The LUI natives are registered under
		// pre-hashed names, so `Engine.<hash>` cannot be named from any string in the image (measured:
		// zero hits across ~1M image strings). But the value IS a C closure, and its native pointer
		// is an address in the game module - so the function can be identified by ADDRESS in IDA even
		// though its name is unrecoverable. An address is a better answer than a name here anyway.
		//
		// The C function shares the same slot as a Lua closure's code pointer (+32). That is a
		// hypothesis, and a self-checking one: for a C closure the value must land inside the module's
		// executable range, and it is reported as "not in module" when it does not.
		std::string LuaDescribeClosure(std::uint64_t closure) {
			const auto cl = reinterpret_cast<const std::uint8_t*>(closure);
			std::uint8_t isC = 0;
			if (!SafeRead(cl + Pointers::kLuaClosure_IsC, isC)) {
				return std::format("function 0x{:X} <unreadable>", closure);
			}
			void* fn = nullptr;
			if (!SafeRead(cl + Pointers::kLuaClosure_Code, fn) || !fn) {
				return std::format("function 0x{:X} ({})", closure, isC ? "C" : "Lua");
			}
			if (!isC) return std::format("function 0x{:X} (Lua)", closure);

			const auto base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
			const auto addr = reinterpret_cast<std::uint64_t>(fn);
			if (base && addr > base && addr - base < 0x10000000ull) {
				return std::format("function 0x{:X} (C, native RVA 0x{:X})", closure, addr - base);
			}
			return std::format("function 0x{:X} (C, native 0x{:X} - not in module)", closure, addr);
		}

		// --- Resolving a name hash back to text, in-process ------------------------------------
		//
		// A tag -6 hashed-name object keeps only the 63-bit hash; the text was replaced at build
		// time. Offline recovery has plateaued (see [[cw-mod-dvar-hash-recovery]]) — the three
		// hashes the first error dump produced matched nothing across all 47k wordlist entries.
		//
		// But the Lua runtime interns every string the CURRENT UI state has actually touched, and
		// HashString of an interned string is the same hash. So walking the global string table
		// once and building hash -> text resolves any name whose text is live, which is strictly
		// more than a wordlist can do and costs no relaunch.
		//
		// Built once and cached: the walk is ~25k strings, each read a byte at a time through
		// SafeRead, and an error dump is capped at 24 records. The cost of that being stale is a
		// name interned after the first error going unresolved, which is a missing annotation
		// rather than a wrong one.
		std::unordered_map<std::uint64_t, std::string> g_NameHashes;
		bool g_NameHashesBuilt = false;

		void BuildNameHashMap(void* L) {
			if (g_NameHashesBuilt) return;
			g_NameHashesBuilt = true;   // set first: a fault below must not retry every error

			const std::uint8_t* G = nullptr;
			if (!SafeRead(reinterpret_cast<const std::uint8_t*>(L) + Pointers::kLuaState_GlobalState, G) || !G) return;

			const std::uint8_t* buckets = nullptr;
			std::uint32_t mask = 0;
			if (!SafeRead(G + Pointers::kLuaStrt_Buckets, buckets) || !buckets) return;
			if (!SafeRead(G + Pointers::kLuaStrt_SizeMask, mask)) return;
			if (mask == 0 || mask >= 0x3FFFFFF) return;   // same sanity bound as LuaDumpStringTable

			for (std::size_t i = 0; i <= static_cast<std::size_t>(mask); ++i) {
				const std::uint8_t* node = nullptr;
				if (!SafeRead(buckets + i * sizeof(void*), node)) continue;

				for (std::size_t depth = 0; node && depth < 8192; ++depth) {
					const std::uint8_t* next = nullptr;
					if (!SafeRead(node + Pointers::kLuaTString_Next, next)) break;

					std::uint32_t len = 0;
					if (SafeRead(node + Pointers::kLuaTString_Len, len) && len && len <= 128) {
						std::string s;
						s.reserve(len);
						bool clean = true;
						for (std::uint32_t k = 0; k < len; ++k) {
							char c = 0;
							if (!SafeRead(node + Pointers::kLuaTString_Data + k, c)) { clean = false; break; }
							if (c < 0x20 || c > 0x7E) { clean = false; break; }
							s.push_back(c);
						}
						if (clean) g_NameHashes.try_emplace(Pointers::HashString(s.c_str()), std::move(s));
					}
					node = next;
				}
			}
		}

		// "" when the hash is not interned in this UI state, which is the common case for menu
		// names — T9 pre-hashes identifiers at build time, so most never exist as strings at all.
		std::string ResolveNameHash(std::uint64_t hash) {
			const auto it = g_NameHashes.find(hash & 0x7FFFFFFFFFFFFFFFULL);
			return it == g_NameHashes.end() ? std::string{} : it->second;
		}

		// LuaBrief's sibling for slots that are NOT addressable as Lua stack indices.
		//
		// LuaBrief asks lua_type for a second opinion, which needs the value to sit at a known index
		// relative to the CURRENT frame. The error-frame walk reads slots belonging to frames further
		// down the stack, where no such index exists - handing lua_type an index computed for the
		// wrong frame would either be answered about some unrelated value or run off the end. So this
		// decodes the NaN box and nothing else: no engine call, no Lua stack use, safe on any qword.
		std::string LuaBriefRaw(const std::uint64_t* slot) {
			std::uint64_t v = 0;
			if (!SafeRead(slot, v)) return "<unreadable>";

			const int tag = LuaTagOf(v);
			const std::uint64_t payload = v & Pointers::kLuaPayloadMask;

			if (!LuaIsTagged(tag)) {
				double d = 0.0;
				std::memcpy(&d, &v, sizeof(d));
				return std::format("number  {}", d);
			}
			switch (tag) {
			case Pointers::kLuaTag_Nil:
				return "nil";
			case Pointers::kLuaTag_String: {
				std::string s;
				if (LuaReadString(payload, s)) return std::format("string  \"{}\"", s);
				return std::format("string  <unreadable 0x{:X}>", payload);
			}
			case Pointers::kLuaTag_Table:
				return std::format("table   0x{:X}", payload);
			case Pointers::kLuaTag_Function:
				return LuaDescribeClosure(payload);
			case Pointers::kLuaTag_HashedName: {
				// THE payload of this whole exercise. A LUI menu name compiles to one of these, so
				// when the frame below is createMenu this is the name of the menu that died. The
				// text is gone - only the 63-bit hash survives - so print the hash in the exact form
				// tools/menu_hash_match.py and cw-mod/lua_strings.txt are keyed by, and try the
				// object's slots as strings in case this particular one did retain its name.
				std::uint64_t hash = 0;
				SafeRead(reinterpret_cast<const std::uint8_t*>(payload) + Pointers::kLuaHashedName_Hash, hash);
				std::string text = ResolveNameHash(hash);
				if (!text.empty()) text = std::format("  = \"{}\"", text);
				for (int i = 0; i < 4 && text.empty(); ++i) {
					std::uint64_t w = 0;
					if (!SafeRead(reinterpret_cast<const std::uint64_t*>(payload) + i, w) || !w) continue;
					std::string s;
					if (LuaTryString(w, s)) text = std::format("  ~\"{}\"", s);
				}
				return std::format("name    hash 0x{:016X}{}", hash & 0x7FFFFFFFFFFFFFFFULL, text);
			}
			default:
				return std::format("tag {:<3} 0x{:X}", tag, payload);
			}
		}

		// Walk a Lua table straight out of memory: array part, then hash part.
		//
		// Deliberately NOT lua_next. The existing LuaEnumerate uses lua_next and is fine for the
		// overlay, but it needs the table on the Lua stack, it runs engine code behind the Arxan
		// guard, and luaH_next raises "invalid key to 'next'" if it dislikes the key it is handed.
		// None of that is acceptable from inside an error handler, where a raise has no protected
		// frame above it and the stack is mid-unwind. Every read here is a SafeRead against the
		// layout recorded in game.hpp, so the worst case is a truncated dump.
		//
		// `depth` expands nested tables. Kept shallow by default: an event table one level down is
		// the question being asked, and a LUI widget graph two levels down is a log nobody reads.
		std::string LuaDumpTableRaw(std::uint64_t table, int maxEntries, int depth,
			const std::string& indent) {
			const auto base = reinterpret_cast<const std::uint8_t*>(table);

			std::uint64_t* array = nullptr;
			const std::uint8_t* node = nullptr;
			std::uint32_t sizeArray = 0;
			std::uint32_t lastNode = 0;
			if (!SafeRead(base + Pointers::kLuaTable_Array, array)
				|| !SafeRead(base + Pointers::kLuaTable_Node, node)
				|| !SafeRead(base + Pointers::kLuaTable_SizeArray, sizeArray)
				|| !SafeRead(base + Pointers::kLuaTable_LastNode, lastNode)) {
				return indent + "<table header unreadable>\n";
			}
			// A torn or wrong read would otherwise send us walking millions of entries on the game
			// thread, which is indistinguishable from a hang.
			if (sizeArray > 65536 || lastNode > 65536) {
				return std::format("{}<table dimensions look wrong (sizearray {}, nodes {})>\n",
					indent, sizeArray, lastNode);
			}

			std::string out;
			int shown = 0;

			for (std::uint32_t i = 0; i < sizeArray && shown < maxEntries; ++i) {
				std::uint64_t v = 0;
				if (!array || !SafeRead(array + i, v) || v == Pointers::kLuaEmptySlot) continue;
				out += std::format("{}[{}] = {}\n", indent, i + 1, LuaBriefRaw(&v));
				++shown;
				if (depth > 0 && LuaTagOf(v) == Pointers::kLuaTag_Table) {
					out += LuaDumpTableRaw(v & Pointers::kLuaPayloadMask, maxEntries, depth - 1,
						indent + "    ");
				}
			}

			for (std::uint32_t i = 0; node && i <= lastNode && shown < maxEntries; ++i) {
				const std::uint8_t* const n = node + static_cast<std::size_t>(i) * Pointers::kLuaNodeStride;
				std::uint64_t val = 0;
				std::uint64_t key = 0;
				if (!SafeRead(n + Pointers::kLuaNode_Value, val) || val == Pointers::kLuaEmptySlot) continue;
				if (!SafeRead(n + Pointers::kLuaNode_Key, key)) continue;
				out += std::format("{}{} = {}\n", indent, LuaBriefRaw(&key), LuaBriefRaw(&val));
				++shown;
				if (depth > 0 && LuaTagOf(val) == Pointers::kLuaTag_Table) {
					out += LuaDumpTableRaw(val & Pointers::kLuaPayloadMask, maxEntries, depth - 1,
						indent + "    ");
				}
			}

			if (!shown) out += indent + "<empty>\n";
			else if (shown >= maxEntries) out += indent + "... truncated\n";
			return out;
		}

		// Visit every live (key, value) in a table's hash part - where string and hashed-name keys
		// live - with plain memory reads, same layout and same sanity bound as LuaDumpTableRaw.
		// Returns false when the header is unreadable or implausible.
		template <typename Fn>
		bool LuaForEachNode(std::uint64_t table, Fn&& fn) {
			const auto base = reinterpret_cast<const std::uint8_t*>(table);
			const std::uint8_t* node = nullptr;
			std::uint32_t lastNode = 0;
			if (!SafeRead(base + Pointers::kLuaTable_Node, node) || !node
				|| !SafeRead(base + Pointers::kLuaTable_LastNode, lastNode) || lastNode > 65536) {
				return false;
			}
			for (std::uint32_t i = 0; i <= lastNode; ++i) {
				const std::uint8_t* const n = node + static_cast<std::size_t>(i) * Pointers::kLuaNodeStride;
				std::uint64_t val = 0;
				std::uint64_t key = 0;
				if (!SafeRead(n + Pointers::kLuaNode_Value, val) || val == Pointers::kLuaEmptySlot) continue;
				if (!SafeRead(n + Pointers::kLuaNode_Key, key)) continue;
				fn(key, val);
			}
			return true;
		}

		// table[name] for a plain string key, as the raw TValue. False when absent.
		bool LuaStringField(std::uint64_t table, const char* name, std::uint64_t& out) {
			bool found = false;
			LuaForEachNode(table, [&](std::uint64_t key, std::uint64_t val) {
				if (found || LuaTagOf(key) != Pointers::kLuaTag_String) return;
				std::string s;
				if (LuaReadString(key & Pointers::kLuaPayloadMask, s) && s == name) {
					out = val;
					found = true;
				}
			});
			return found;
		}

		// table[name] for a plain string key, as a table. 0 when absent or not a table.
		std::uint64_t LuaSubTable(std::uint64_t table, const char* name) {
			std::uint64_t val = 0;
			if (!LuaStringField(table, name, val) || LuaTagOf(val) != Pointers::kLuaTag_Table) return 0;
			return val & Pointers::kLuaPayloadMask;
		}

		// table[#"name"] for a hashed-name key - what every compiled T9 identifier (CoD.BaseUtility,
		// Enum.eModes.MODE_ZOMBIES, the LUI.createMenu keys) turns into. Matched on the low 60 bits,
		// which is both what the Lua decompiler prints and a subset of the 63-bit engine hash, so
		// either form finds it. `keyOut` gets the key TValue itself: a hashed-name value cannot be
		// built from outside, but an existing key can be pushed as-is.
		bool LuaHashedField(std::uint64_t table, std::uint64_t hash, std::uint64_t& valOut,
			std::uint64_t* keyOut = nullptr) {
			constexpr std::uint64_t kMask60 = 0x0FFFFFFFFFFFFFFFULL;
			bool found = false;
			LuaForEachNode(table, [&](std::uint64_t key, std::uint64_t val) {
				if (found || LuaTagOf(key) != Pointers::kLuaTag_HashedName) return;
				std::uint64_t h = 0;
				if (!SafeRead(reinterpret_cast<const std::uint8_t*>(key & Pointers::kLuaPayloadMask)
					+ Pointers::kLuaHashedName_Hash, h)) return;
				if ((h & kMask60) != (hash & kMask60)) return;
				valOut = val;
				if (keyOut) *keyOut = key;
				found = true;
			});
			return found;
		}

		// The globals table: a raw Table* at L+48 (lua_index2adr's LUA_GLOBALSINDEX case).
		std::uint64_t LuaGlobals(void* L) {
			constexpr std::size_t kLuaState_Globals = 48;
			std::uint64_t g = 0;
			if (!SafeRead(reinterpret_cast<const std::uint8_t*>(L) + kLuaState_Globals, g)) return 0;
			return g & Pointers::kLuaPayloadMask;
		}

		// A LUI element: the UIElement userdata (lua_type 8). Tables and elements both keep their
		// metatable as a raw Table* at payload+0x20 - read off lua_getmetatable (0x7FF729E3D150).
		constexpr int         kLuaTag_Element   = -14;
		constexpr std::size_t kLuaObj_Metatable = 0x20;

		// v.key for a string key, following __index TABLES the way Lua would (an __index function
		// would have to run, so the chain stops there). An element has no fields of its own: its
		// metatable's __index is its per-instance field table, whose metatable leads on to the class.
		bool LuaIndexChain(std::uint64_t v, const char* key, std::uint64_t& out) {
			for (int depth = 0; depth < 12; ++depth) {
				const int tag = LuaTagOf(v);
				if (tag != Pointers::kLuaTag_Table && tag != kLuaTag_Element) return false;
				const std::uint64_t obj = v & Pointers::kLuaPayloadMask;
				if (tag == Pointers::kLuaTag_Table && LuaStringField(obj, key, out)
					&& LuaTagOf(out) != Pointers::kLuaTag_Nil) {
					return true;
				}
				std::uint64_t mt = 0;
				if (!SafeRead(reinterpret_cast<const std::uint8_t*>(obj) + kLuaObj_Metatable, mt) || !mt) return false;
				if (!LuaStringField(mt & Pointers::kLuaPayloadMask, "__index", v)) return false;
			}
			return false;
		}

		// table[#hash] the way Lua would index it: the raw hashed-name key, then on through __index
		// TABLES. Every Enum.* needs the second half: each is an EMPTY read-only proxy whose values
		// live in getmetatable(proxy).__index (LuiEnum_BeginReadOnly / LuiEnum_EndReadOnly,
		// 0x7FF727B8CC40 / 0x7FF727B8CE80), so a raw read of Enum.eModes finds nothing (measured
		// 2026-09-23: every "_sessionMode" open was refused on it).
		bool LuaHashedIndex(std::uint64_t table, std::uint64_t hash, std::uint64_t& out) {
			for (int depth = 0; depth < 12 && table; ++depth) {
				if (LuaHashedField(table, hash, out) && LuaTagOf(out) != Pointers::kLuaTag_Nil) return true;
				std::uint64_t mt = 0, index = 0;
				if (!SafeRead(reinterpret_cast<const std::uint8_t*>(table) + kLuaObj_Metatable, mt) || !mt
					|| !LuaStringField(mt & Pointers::kLuaPayloadMask, "__index", index)
					|| LuaTagOf(index) != Pointers::kLuaTag_Table) {
					return false;
				}
				table = index & Pointers::kLuaPayloadMask;
			}
			return false;
		}

		// The array part, 1-based like Lua. Same sanity bound as LuaForEachNode.
		template <typename Fn>
		bool LuaForEachArray(std::uint64_t table, Fn&& fn) {
			const auto base = reinterpret_cast<const std::uint8_t*>(table);
			const std::uint64_t* arr = nullptr;
			std::uint32_t size = 0;
			if (!SafeRead(base + Pointers::kLuaTable_Array, arr) || !SafeRead(base + Pointers::kLuaTable_SizeArray, size)
				|| size > 65536) {
				return false;
			}
			for (std::uint32_t i = 0; arr && i < size; ++i) {
				std::uint64_t v = 0;
				if (!SafeRead(arr + i, v) || LuaTagOf(v) == Pointers::kLuaTag_Nil) continue;
				fn(i + 1, v);
			}
			return true;
		}

		// t[n] for an integer key: the array part when n is in range, else a hash node whose key is
		// the number itself (a NaN-boxed number is the raw double).
		bool LuaIntField(std::uint64_t table, int n, std::uint64_t& out) {
			bool found = false;
			LuaForEachArray(table, [&](std::uint32_t i, std::uint64_t v) {
				if (!found && static_cast<int>(i) == n) { out = v; found = true; }
			});
			if (found) return true;
			const double d = n;
			std::uint64_t bits = 0;
			std::memcpy(&bits, &d, sizeof(bits));
			LuaForEachNode(table, [&](std::uint64_t key, std::uint64_t val) {
				if (!found && key == bits) { out = val; found = true; }
			});
			return found;
		}

		// The last non-nil array entry - what table.insert appended most recently.
		bool LuaArrayLast(std::uint64_t table, std::uint64_t& out) {
			bool found = false;
			LuaForEachArray(table, [&](std::uint32_t, std::uint64_t v) { out = v; found = true; });
			return found;
		}

		std::uint64_t LuaHashedNameValue(std::uint64_t v) {
			std::uint64_t h = 0;
			if (LuaTagOf(v) != Pointers::kLuaTag_HashedName
				|| !SafeRead(reinterpret_cast<const std::uint8_t*>(v & Pointers::kLuaPayloadMask)
					+ Pointers::kLuaHashedName_Hash, h)) {
				return 0;
			}
			return h & 0x7FFFFFFFFFFFFFFFULL;
		}

		// The menu on top of this controller's navigation stack: CoD[0x7C3CEE9552C8730][controller]
		// is a list of groups, each a list of menuNameHash values. OpenOverlay appends a new {hash}
		// group, GoBack pops (baseutility.lua). 0 when unreadable.
		std::uint64_t LuaNavTopHash(std::uint64_t cod, int controller) {
			constexpr std::uint64_t kHash_NavStack = 0x07C3CEE9552C8730ULL;
			std::uint64_t nav = 0, groups = 0, group = 0, top = 0;
			if (!LuaHashedField(cod, kHash_NavStack, nav) || LuaTagOf(nav) != Pointers::kLuaTag_Table
				|| !LuaIntField(nav & Pointers::kLuaPayloadMask, controller, groups)
				|| LuaTagOf(groups) != Pointers::kLuaTag_Table
				|| !LuaArrayLast(groups & Pointers::kLuaPayloadMask, group) || LuaTagOf(group) != Pointers::kLuaTag_Table
				|| !LuaArrayLast(group & Pointers::kLuaPayloadMask, top)) {
				return 0;
			}
			return LuaHashedNameValue(top);
		}

		struct LuaOpenMenu {
			std::uint64_t element{};   // the element TValue, ready to push
			std::uint64_t hash{};      // its menuNameHash, 0 if unreadable
		};

		// The menus open under a root. root.m_references holds the elements the root keeps alive
		// (codaccountutility walks it to find a menu by id). A menu is an element whose __index chain
		// has `openMenu` - exactly the test CoD.BaseUtility.OpenOverlay's parent walk makes, so any
		// element returned here passes it. Read-only: nothing is pushed, no Lua runs.
		std::vector<LuaOpenMenu> LuaOpenMenusUnder(std::uint64_t root, std::string& note) {
			std::vector<LuaOpenMenu> out;
			std::uint64_t refs = 0;
			if (!LuaIndexChain(root, "m_references", refs) || LuaTagOf(refs) != Pointers::kLuaTag_Table) {
				note = "root.m_references is not a table";
				return out;
			}
			int elements = 0;
			auto consider = [&](std::uint64_t v) {
				if (LuaTagOf(v) != kLuaTag_Element) return;
				++elements;
				for (const LuaOpenMenu& m : out) if (m.element == v) return;
				std::uint64_t openMenu = 0;
				if (!LuaIndexChain(v, "openMenu", openMenu)) return;
				LuaOpenMenu m{ v, 0 };
				std::uint64_t h = 0;
				if (LuaIndexChain(v, "menuNameHash", h)) m.hash = LuaHashedNameValue(h);
				out.push_back(m);
			};
			const std::uint64_t t = refs & Pointers::kLuaPayloadMask;
			LuaForEachArray(t, [&](std::uint32_t, std::uint64_t v) { consider(v); });
			LuaForEachNode(t, [&](std::uint64_t key, std::uint64_t val) { consider(key); consider(val); });
			note = std::format("{} elements in root.m_references, {} of them menus", elements, out.size());
			return out;
		}

		// POD-only (MSVC C2712), see the other Safe* wrappers.
		bool SafeCreateTable(const Pointers& p, void* L, int narr, int nrec) {
			__try { p.m_lua_createtable(L, narr, nrec); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		bool SafeSetField(const Pointers& p, void* L, int idx, const char* k) {
			__try { p.m_lua_setfield(L, idx, k); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		bool SafeProtectedCall(const Pointers& p, void* L, int nargs, int& status) {
			__try { status = p.m_LUI_ProtectedCall(L, nargs, 0, 0); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		// Name a bytecode constant. Constants are raw object pointers (NOT NaN-boxed TValues), and
		// the type byte at +8 says which kind: 5 is an interned hashed name, anything else is
		// treated as a TString whose characters start at +24 - which is exactly what
		// luaG_getobjname's global/field paths do when they return `constant + 24`.
		std::string LuaNameConstant(std::uint64_t kEnd, std::uint32_t index) {
			if (!kEnd) return "<no constant table>";
			std::uint64_t obj = 0;
			const auto slot = reinterpret_cast<const std::uint8_t*>(
				kEnd - 8ull * (static_cast<std::uint64_t>(index) + 1));
			if (!SafeRead(slot, obj) || !obj) return "<unreadable constant>";

			std::uint8_t type = 0;
			if (SafeRead(reinterpret_cast<const std::uint8_t*>(obj) + Pointers::kLuaObj_TypeByte, type)
				&& type == Pointers::kLuaObjType_HashedName) {
				std::uint64_t hash = 0;
				if (!SafeRead(reinterpret_cast<const std::uint8_t*>(obj) + Pointers::kLuaHashedName_Hash, hash)) {
					return "<unreadable hashed name>";
				}
				hash &= 0x7FFFFFFFFFFFFFFFULL;
				const std::string text = ResolveNameHash(hash);
				return text.empty()
					? std::format("#hash 0x{:016X} (not interned - unresolved)", hash)
					: std::format("\"{}\"", text);
			}

			std::string s;
			if (LuaReadString(obj, s) && !s.empty()) return std::format("\"{}\"", s);
			return std::format("<constant 0x{:X}, type {}>", obj, type);
		}

		// Decode the instruction the frame is stopped on, and name its constant operands.
		//
		// This is the last step of the diagnosis: Lua's own "attempt to index a nil value" omits the
		// usual "(field 'x')" because luaG_getobjname bails to "xhashfunc" without writing a name
		// whenever the key is a hashed constant - which on T9 is almost always. The name is still
		// sitting in the constant table; the engine just declines to format it.
		//
		// No engine call. luaG_currentpc's common path is simply
		//     pc = ((*frame - closure->code) >> 2) - 1
		// because frame slot +0 holds the SAVED PC for an ordinary Lua frame (the low 2 bits select
		// the frame type; 0 means a plain saved pc). Doing the arithmetic ourselves keeps this a
		// pure read on a path where calling into the engine is a liability.
		// Source line for a pc, an exact port of luaG_currentline (0x7FF729E486A0). Having this is
		// what turns a decoded pc from a guess into a checked fact: lua_getinfo already told us the
		// line the frame is stopped on, so a pc whose line does not match that is simply wrong.
		bool LuaLineForPc(const std::uint8_t* code, std::uint32_t pc, std::uint32_t& lineOut) {
			std::uint32_t size = 0, lineDefined = 0, span = 0;
			const std::uint8_t* info = nullptr;
			if (!SafeRead(code + Pointers::kLuaCode_ToSizeCode, size)) return false;
			if (!SafeRead(code + Pointers::kLuaCode_ToLineInfo, info) || !info) return false;
			if (!SafeRead(code + Pointers::kLuaCode_ToLineDefined, lineDefined)) return false;
			if (!SafeRead(code + Pointers::kLuaCode_ToLineSpan, span)) return false;
			if (pc > size) return false;

			if (pc == size) { lineOut = span + lineDefined; return true; }
			if (pc == 0)    { lineOut = lineDefined;        return true; }

			const std::uint32_t i = pc - 1;
			if (span < 256) {
				std::uint8_t d = 0;
				if (!SafeRead(info + i, d)) return false;
				lineOut = d + lineDefined;
			} else if (span >= 0x10000) {
				std::uint32_t d = 0;
				if (!SafeRead(info + 4ull * i, d)) return false;
				lineOut = d + lineDefined;
			} else {
				std::uint16_t d = 0;
				if (!SafeRead(info + 2ull * i, d)) return false;
				lineOut = d + lineDefined;
			}
			return true;
		}

		// The frame's code array plus the first pc the compiler emitted for `line`. The pc is what the
		// local-name lookup needs, and since the stack-slot saved pc is unusable during a live error
		// (rejected on every frame of every run so far), the line's first pc is the best available
		// stand-in - locals live at the failing line are live there too.
		bool LuaFrameCodeAndPc(const std::uint64_t* frame, std::uint32_t line,
			const std::uint8_t*& codeOut, std::uint32_t& pcOut) {
			std::uint64_t fnVal = 0;
			if (!SafeRead(frame - 1, fnVal)) return false;
			if (LuaTagOf(fnVal) != Pointers::kLuaTag_Function) return false;
			const auto cl = reinterpret_cast<const std::uint8_t*>(fnVal & Pointers::kLuaPayloadMask);

			std::uint8_t isC = 0;
			if (!SafeRead(cl + Pointers::kLuaClosure_IsC, isC) || isC) return false;

			std::uint8_t* code = nullptr;
			if (!SafeRead(cl + Pointers::kLuaClosure_Code, code) || !code) return false;

			std::uint32_t sizeCode = 0;
			if (!SafeRead(code + Pointers::kLuaCode_ToSizeCode, sizeCode)
				|| !sizeCode || sizeCode > 0x400000) {
				return false;
			}
			if (!line) return false;
			for (std::uint32_t pc = 0; pc < sizeCode; ++pc) {
				std::uint32_t l = 0;
				if (LuaLineForPc(code, pc, l) && l == line) {
					codeOut = code;
					pcOut = pc;
					return true;
				}
			}
			return false;
		}

		// ULEB128, a port of the reader at 0x7FF729E57240. Advances `p` past the value.
		bool LuaReadUleb(const std::uint8_t*& p, std::uint32_t& out) {
			std::uint8_t b = 0;
			if (!SafeRead(p, b)) return false;
			++p;
			out = b & 0x7Fu;
			int shift = 0;
			while (b >= 0x80u) {
				if (shift > 28) return false;
				if (!SafeRead(p, b)) return false;
				++p;
				shift += 7;
				out |= static_cast<std::uint32_t>(b & 0x7Fu) << shift;
			}
			return true;
		}

		// Name the local variable held in register `reg` at `pc`. A faithful port of
		// luaF_getlocalname (0x7FF729E48970).
		//
		// THE ASSUMPTION THAT T9 STRIPS LOCAL NAMES WAS WRONG. The blob is there and the engine reads
		// it; what T9 actually does is decline to FORMAT the name in error messages (luaG_getobjname
		// bails to "xhashfunc" before writing one). The data was never the problem.
		std::string LuaLocalName(const std::uint8_t* code, std::uint32_t pc, std::uint32_t reg) {
			static const char* const kInternal[6] = {
				"(for index)", "(for limit)", "(for step)",
				"(for generator)", "(for state)", "(for control)",
			};

			const std::uint8_t* p = nullptr;
			if (!SafeRead(code + Pointers::kLuaCode_ToLocVars, p) || !p) return {};

			std::uint32_t startAcc = 0;
			int remaining = static_cast<int>(reg);
			for (int guard = 0; guard < 4096; ++guard) {
				std::uint8_t tag = 0;
				if (!SafeRead(p, tag)) return {};
				if (!tag) return {};                       // end of the blob

				const std::uint8_t* const nameAt = p;
				if (tag >= 7) {                            // inline NUL-terminated name
					while (true) {
						std::uint8_t ch = 0;
						if (!SafeRead(p, ch)) return {};
						if (!ch) break;
						++p;
					}
				}
				++p;                                       // past the NUL, or past the 1..6 code byte

				std::uint32_t startDelta = 0, span = 0;
				if (!LuaReadUleb(p, startDelta)) return {};
				startAcc += startDelta;
				if (startAcc > pc) return {};              // locals from here on start after pc
				if (!LuaReadUleb(p, span)) return {};

				if (pc < startAcc + span && remaining-- == 0) {
					if (tag < 7) return kInternal[tag - 1];
					std::string name;
					for (int i = 0; i < 128; ++i) {
						std::uint8_t ch = 0;
						if (!SafeRead(nameAt + i, ch) || !ch) break;
						name.push_back(static_cast<char>(ch));
					}
					return name;
				}
			}
			return {};
		}

		// One decoded instruction plus the names of its constant operands.
		std::string LuaFormatInsn(const std::uint8_t* code, std::uint64_t kEnd, std::uint32_t pc,
			const char* lead) {
			std::uint32_t insn = 0;
			if (!SafeRead(code + 4ull * pc, insn)) return {};

			const std::uint32_t op = insn & 0xFF;
			const std::uint32_t a  = (insn >> 8)  & 0xFF;
			const std::uint32_t b  = (insn >> 16) & 0xFF;
			const std::uint32_t c  = (insn >> 24) & 0xFF;

			std::string out = std::format(
				"         {} pc {} insn 0x{:08X}  op {} A {} B {} C {}\n", lead, pc, insn, op, a, b, c);

			// Which operand is the key depends on the opcode, and the opcode table is only partly
			// mapped, so both are reported rather than guessed at. For an index that faulted, one of
			// these IS the field name that was looked up on nil.
			if (kEnd) {
				out += std::format("            B as constant: {}\n", LuaNameConstant(kEnd, b));
				out += std::format("            C as constant: {}\n", LuaNameConstant(kEnd, c));
				if (op == Pointers::kLuaOp_GetField || op == Pointers::kLuaOp_GetGlobal
					|| op == Pointers::kLuaOp_HashCall) {
					out += std::format("            ^ op {} is {}\n", op,
						op == Pointers::kLuaOp_GetField ? "GETFIELD/GETTABLE — the key is the field being indexed"
						: op == Pointers::kLuaOp_GetGlobal ? "GETGLOBAL"
						: "a hashed-name call");
				}
			}
			return out;
		}

		// Compact one-line form, for dumping a window of instructions rather than a single one.
		std::string LuaFormatInsnCompact(const std::uint8_t* code, std::uint64_t kEnd, std::uint32_t pc) {
			std::uint32_t insn = 0;
			if (!SafeRead(code + 4ull * pc, insn)) return {};
			const std::uint32_t op = insn & 0xFF;
			const std::uint32_t a  = (insn >> 8)  & 0xFF;
			const std::uint32_t b  = (insn >> 16) & 0xFF;
			const std::uint32_t c  = (insn >> 24) & 0xFF;

			std::uint32_t line = 0;
			LuaLineForPc(code, pc, line);

			// Only the opcodes whose B is known to be a constant index get their B named; naming
			// operands of opcodes we have not mapped is how the previous pass produced convincing
			// nonsense. Everything else is left as a bare number.
			std::string k;
			if (kEnd && (op == Pointers::kLuaOp_GetGlobal || op == Pointers::kLuaOp_GetField
				|| op == Pointers::kLuaOp_LoadK || op == Pointers::kLuaOp_SetGlobal)) {
				k = std::format("   k[{}] = {}", b, LuaNameConstant(kEnd, b));
			}
			return std::format("            L{:<6} pc {:<5} op {:<3} A {:<3} B {:<3} C {:<3}{}\n",
				line, pc, op, a, b, c, k);
		}

		std::string LuaDescribeInstruction(const std::uint64_t* frame, std::uint32_t currentLine,
			std::uint32_t windowBack) {
			std::uint64_t fnVal = 0;
			if (!SafeRead(frame - 1, fnVal)) return "         (insn: frame[-1] unreadable)\n";
			if (LuaTagOf(fnVal) != Pointers::kLuaTag_Function) {
				return std::format("         (insn: frame[-1] is tag {}, not a closure)\n", LuaTagOf(fnVal));
			}
			const auto cl = reinterpret_cast<const std::uint8_t*>(fnVal & Pointers::kLuaPayloadMask);

			std::uint8_t isC = 0;
			if (!SafeRead(cl + Pointers::kLuaClosure_IsC, isC)) return "         (insn: closure unreadable)\n";
			if (isC) return "         (insn: C function, no bytecode)\n";

			std::uint8_t* code = nullptr;
			if (!SafeRead(cl + Pointers::kLuaClosure_Code, code) || !code) {
				return "         (insn: no code array)\n";
			}

			std::uint32_t sizeCode = 0;
			if (!SafeRead(code + Pointers::kLuaCode_ToSizeCode, sizeCode) || !sizeCode || sizeCode > 0x400000) {
				return std::format("         (insn: implausible sizecode {})\n", sizeCode);
			}
			std::uint64_t kEnd = 0;
			SafeRead(code + Pointers::kLuaCode_ToKEnd, kEnd);

			std::string out;

			// Preferred source: frame slot +0, which luaG_currentpc treats as the saved pc when its
			// low 2 bits are clear. Accepted only if it lands inside the code array AND its line
			// agrees with the line lua_getinfo reported - otherwise it is a stale/foreign pointer that
			// merely looked pointer-shaped, which is exactly how the first version of this produced a
			// confident pc of 70223 for a ten-line function.
			std::uint64_t savedPc = 0;
			if (!SafeRead(frame, savedPc)) {
				out += "         (insn: frame[+0] unreadable)\n";
			} else if (savedPc & 3) {
				out += "         (insn: frame[+0] is a tagged frame link, not a saved pc)\n";
			} else if (savedPc <= reinterpret_cast<std::uint64_t>(code)) {
				out += "         (insn: frame[+0] is below the code array)\n";
			} else {
				const std::uint64_t delta = savedPc - reinterpret_cast<std::uint64_t>(code);
				const std::ptrdiff_t pc = static_cast<std::ptrdiff_t>(delta / 4) - 1;
				if (pc < 0 || static_cast<std::uint32_t>(pc) > sizeCode) {
					out += std::format(
						"         (insn: frame[+0] pc {} is outside this function's {} instructions"
						" - not this closure's saved pc)\n", pc, sizeCode);
				} else {
					std::uint32_t line = 0;
					const bool ok = LuaLineForPc(code, static_cast<std::uint32_t>(pc), line);
					if (ok && currentLine && line != currentLine) {
						out += std::format(
							"         (insn: frame[+0] pc {} maps to line {}, but the frame is at line {}"
							" - rejected)\n", pc, line, currentLine);
					} else {
						out += LuaFormatInsn(code, kEnd, static_cast<std::uint32_t>(pc), ">>");
						if (ok) out += std::format("            (line {} - matches the frame)\n", line);
						return out;
					}
				}
			}

			// Fallback that needs no pc at all: we know the line, and lineinfo maps every pc to a
			// line, so decode every instruction generated for that line. For a single source line
			// that is a handful of instructions, and the one that indexes a field is among them.
			std::uint32_t shown = 0;
			std::uint32_t firstPc = 0;
			if (currentLine) {
				for (std::uint32_t pc = 0; pc < sizeCode && shown < 12; ++pc) {
					std::uint32_t line = 0;
					if (!LuaLineForPc(code, pc, line) || line != currentLine) continue;
					if (!shown) {
						firstPc = pc;
						out += std::format(
							"         -- all instructions the compiler emitted for line {} --\n", currentLine);
					}
					out += LuaFormatInsn(code, kEnd, pc, "  ");
					++shown;
				}
			}

			// The instructions BEFORE the failing line, which is where a register that is nil at the
			// fault was last written. The fault itself names the key; only the history names the
			// expression that produced nil.
			if (shown && windowBack) {
				const std::uint32_t from = firstPc > windowBack ? firstPc - windowBack : 0;
				out += std::format("         -- preceding {} instructions (pc {}..{}) --\n",
					firstPc - from, from, firstPc ? firstPc - 1 : 0);
				for (std::uint32_t pc = from; pc < firstPc; ++pc) {
					out += LuaFormatInsnCompact(code, kEnd, pc);
				}
			}
			if (!shown) {
				out += std::format("         (insn: no instruction maps to line {} in {} instructions)\n",
					currentLine, sizeCode);
			}
			return out;
		}

		// Read a NUL-terminated C string out of a lua_Debug field. The pointers lua_getinfo writes
		// are into interned strings and static literals, but this runs on an error path where being
		// wrong about the struct layout is a real possibility, so it is bounded and fails soft.
		std::string LuaReadCString(const char* p, std::size_t cap = 192) {
			if (!p) return {};
			std::string out;
			for (std::size_t i = 0; i < cap; ++i) {
				char c = 0;
				if (!SafeRead(p + i, c) || c == '\0') break;
				out.push_back(c);
			}
			return out;
		}

		// Push _G, then walk `path` from it, leaving the addressed value on top of the stack.
		// Numeric components index the array part via lua_rawgeti. Returns an error string, or ""
		// on success with a table on top.
		// Every Lua accessor we call is behind the Arxan caller check: when the check rejects the
		// call the function returns having done NOTHING, pushing nothing and reporting nothing. That
		// silent bail is the dangerous case - the walk would carry on believing a value was pushed,
		// and the stale slot it then reads gets handed to lua_next as a Table*. So after every push
		// we assert that L->top actually moved by one, and treat "it did not" as fatal to the walk.
		constexpr const char* kArxanBailed =
			"the engine's caller check rejected the call (nothing was pushed). The ArxanCall return "
			"gadget is not satisfying it on this build - see arxan_call.cpp.";

	}

	bool Pointers::LuaApiResolved() const {
		return this->m_lua_getfield && this->m_lua_rawgeti && this->m_lua_next
			&& this->m_lua_pushvalue && this->m_lua_type;
	}

	namespace {
		// lua_pushboolean stores ~((b ? 2 : 1) << 47): false tags as -2, true as -3.
		constexpr int kLuaTag_False = -2;
		constexpr int kLuaTag_True  = -3;

		// One-line text for a non-string TValue: numbers (integral when they are), booleans, nil,
		// and a hashed name as #<63-bit hash>, the form the Lua dump and the native table use.
		std::string LuaScalarText(std::uint64_t v) {
			const int tag = LuaTagOf(v);
			if (!LuaIsTagged(tag)) {
				double d = 0;
				std::memcpy(&d, &v, sizeof(d));
				return d == static_cast<double>(static_cast<std::int64_t>(d))
					? std::format("{}", static_cast<std::int64_t>(d)) : std::format("{}", d);
			}
			switch (tag) {
			case Pointers::kLuaTag_Nil: return "nil";
			case kLuaTag_False:         return "false";
			case kLuaTag_True:          return "true";
			case Pointers::kLuaTag_HashedName: {
				std::uint64_t hash = 0;
				if (SafeRead(reinterpret_cast<const std::uint8_t*>(v & Pointers::kLuaPayloadMask)
					+ Pointers::kLuaHashedName_Hash, hash)) {
					return std::format("#{:X}", hash & 0x7FFFFFFFFFFFFFFFULL);
				}
				return "#<unreadable>";
			}
			default: return std::format("<tag {}>", tag);
			}
		}

		// A table result on one line, `{key=value, ...}` sorted by key, one level deep. Some natives
		// return a table (connection info, queue info), and the field is the answer.
		std::string LuaTableInline(std::uint64_t table) {
			std::vector<std::string> fields;
			const bool ok = LuaForEachNode(table, [&](std::uint64_t key, std::uint64_t val) {
				if (fields.size() >= 32) return;
				std::string k, v;
				if (LuaTagOf(key) == Pointers::kLuaTag_String) {
					if (!LuaReadString(key & Pointers::kLuaPayloadMask, k)) k = "<key?>";
				}
				else {
					k = std::format("[{}]", LuaScalarText(key));
				}
				if (LuaTagOf(val) == Pointers::kLuaTag_String) {
					if (!LuaReadString(val & Pointers::kLuaPayloadMask, v)) v = "<string?>";
					v = std::format("\"{}\"", v);
				}
				else if (LuaTagOf(val) == Pointers::kLuaTag_Table) {
					v = "{...}";
				}
				else {
					v = LuaScalarText(val);
				}
				fields.push_back(std::format("{}={}", k, v));
			});
			if (!ok) return "{<unreadable table>}";
			std::sort(fields.begin(), fields.end());
			std::string out = "{";
			for (std::size_t i = 0; i < fields.size(); ++i) {
				out += (i ? ", " : "") + fields[i];
			}
			return out + "}";
		}
	}

	// A native's arguments start at L->base (lua_index2adr: `*(L+80) + 8*idx - 8`, bounded by L->top).
	bool Pointers::LuaArgText(void* L, int idx, std::string& out) {
		out.clear();
		std::uint64_t* base = nullptr;
		std::uint64_t* top = nullptr;
		if (!L || idx < 1 || !SafeRead(LuaBaseSlot(L), base) || !SafeRead(LuaTopSlot(L), top) || !base
			|| base + (idx - 1) >= top) {
			return false;
		}
		std::uint64_t v = 0;
		if (!SafeRead(base + (idx - 1), v)) return false;

		const int tag = LuaTagOf(v);
		if (tag != kLuaTag_String) {
			out = LuaScalarText(v);
			return true;
		}

		// Same TString layout as LuaReadString, with a log-sized cap instead of a key-sized one.
		const auto ts = reinterpret_cast<const std::uint8_t*>(v & kLuaPayloadMask);
		std::uint32_t len = 0;
		if (!SafeRead(ts + kLuaTString_Len, len)) return false;
		if (len > 1024) len = 1024;
		out.reserve(len);
		for (std::uint32_t i = 0; i < len; ++i) {
			char c = 0;
			if (!SafeRead(ts + kLuaTString_Data + i, c) || c == '\0') break;
			out.push_back(c);
		}
		return true;
	}

	// Right after a native returns (inside a detour, before luaD_poscall moves anything), what it
	// pushed is still at L->top - 1, above its own base.
	bool Pointers::LuaResultText(void* L, std::string& out) {
		out.clear();
		std::uint64_t* base = nullptr;
		std::uint64_t* top = nullptr;
		if (!L || !SafeRead(LuaBaseSlot(L), base) || !SafeRead(LuaTopSlot(L), top) || !base || top <= base) {
			return false;
		}
		std::uint64_t v = 0;
		if (!SafeRead(top - 1, v)) return false;
		out = LuaTagOf(v) == kLuaTag_Table ? LuaTableInline(v & kLuaPayloadMask) : LuaScalarText(v);
		return true;
	}

	// _G.LUI.createMenu, walked straight out of memory. No lua_getfield/lua_next: nothing is pushed,
	// no Lua runs, so there is no stack to restore and no Arxan guard to satisfy.
	std::string Pointers::LuaCollectMenuHashes(std::vector<std::uint64_t>& out) const {
		out.clear();
		if (!this->m_g_luiCtx) return "g_luiCtx did not resolve on this build.";

		void* L = nullptr;
		if (!SafeRead(this->m_g_luiCtx, L) || !L) return "LUI is not up yet (the Lua state is null).";

		const std::uint64_t globals = LuaGlobals(L);
		if (!globals) return "Could not read _G off the Lua state.";

		const std::uint64_t lui = LuaSubTable(globals, "LUI");
		if (!lui) return "_G.LUI is not a table in this UI state.";
		const std::uint64_t registry = LuaSubTable(lui, "createMenu");
		if (!registry) return "_G.LUI.createMenu is not a table in this UI state.";

		std::size_t skipped = 0;
		LuaForEachNode(registry, [&](std::uint64_t key, std::uint64_t) {
			std::uint64_t hash = 0;
			if (LuaTagOf(key) == kLuaTag_HashedName
				&& SafeRead(reinterpret_cast<const std::uint8_t*>(key & kLuaPayloadMask) + kLuaHashedName_Hash, hash)
				&& hash) {
				out.push_back(hash & 0x7FFFFFFFFFFFFFFFULL);
			}
			else {
				++skipped;
			}
		});

		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		LOG("LuiMenus", INFO, "LUI.createMenu: {} menu hashes ({} non-hash keys skipped).", out.size(), skipped);
		return out.empty() ? "LUI.createMenu has no hashed-name keys." : "";
	}

	// CoD.BaseUtility.OpenOverlay(parentMenu, menu, controller, params) - how the game's own buttons
	// open a menu, params table included. See kDump_LUI_ProtectedCall for why it has to be this and
	// not addmenu. Every value pushed is an existing engine object read out of the Lua state (the
	// function, an open menu element, the registry's own key for the menu name, the Enum number);
	// only the params table is new, and the engine builds it. Every outcome is logged, early refusals
	// included - a refusal that only reached the tab's status line left no trace to debug from.
	std::string Pointers::OpenLuiOverlay(const char* nameOrHash, int mode, int localClient) {
		// Hashes as the decompiled Lua prints them (60-bit); LuaHashedField matches on those bits.
		constexpr std::uint64_t kHash_BaseUtility     = 0x0B1585DE9E3F390ULL;
		constexpr std::uint64_t kHash_OpenOverlay     = 0x0ECF58BF6668AB9FULL;
		constexpr std::uint64_t kHash_eModes          = 0x09C0C2196D8313A0ULL;
		constexpr std::uint64_t kHash_ModeZombies     = 0x03723205FAE52C4AULL;   // MODE_ZOMBIES
		constexpr std::uint64_t kHash_ModeMultiplayer = 0x083EBA96F36BC4E5ULL;   // MODE_MULTIPLAYER

		if (!nameOrHash || !*nameOrHash) return "Enter a menu name or a hash.";
		auto fail = [&](std::string why) {
			LOG("LuiMenus", WARN, "OpenOverlay '{}' not sent: {}", nameOrHash, why);
			return why;
		};
		if (!this->m_g_luiCtx || !this->m_lua_createtable || !this->m_lua_setfield || !this->m_LUI_ProtectedCall
			|| !this->m_LUI_GetRootName || !this->m_CL_LocalClientToController || !this->m_UI_SetUiActive) {
			return fail("The OpenOverlay anchors did not resolve on this build.");
		}
		void* L = nullptr;
		if (!SafeRead(this->m_g_luiCtx, L) || !L) return fail("LUI is not up yet (the Lua state is null).");
		const std::uint64_t globals = LuaGlobals(L);
		if (!globals) return fail("Could not read _G off the Lua state.");

		std::uint64_t menuHash = 0;
		if (!Pointers::ParseMenuHash(nameOrHash, menuHash)) menuHash = Pointers::HashString(nameOrHash);

		// The function.
		const std::uint64_t cod = LuaSubTable(globals, "CoD");
		std::uint64_t baseUtility = 0, fn = 0;
		if (!cod || !LuaHashedField(cod, kHash_BaseUtility, baseUtility) || LuaTagOf(baseUtility) != kLuaTag_Table
			|| !LuaHashedField(baseUtility & kLuaPayloadMask, kHash_OpenOverlay, fn)
			|| LuaTagOf(fn) != kLuaTag_Function) {
			return fail("CoD.BaseUtility.OpenOverlay is not in this Lua state.");
		}

		// The menu name, as the registry's own key object - a hashed-name value cannot be made here.
		const std::uint64_t lui = LuaSubTable(globals, "LUI");
		const std::uint64_t registry = lui ? LuaSubTable(lui, "createMenu") : 0;
		std::uint64_t builder = 0, menuKey = 0, fullHash = 0;
		if (!registry || !LuaHashedField(registry, menuHash, builder, &menuKey)) {
			return fail(std::format("'{}' (0x{:016X}) is not a LUI.createMenu key in this UI state, so there is no "
				"menu-name value to pass.", nameOrHash, menuHash & 0x7FFFFFFFFFFFFFFFULL));
		}
		SafeRead(reinterpret_cast<const std::uint8_t*>(menuKey & kLuaPayloadMask) + kLuaHashedName_Hash, fullHash);

		// This controller's UI root element.
		const int controller = this->m_CL_LocalClientToController(localClient);
		if (controller < 0) return fail(std::format("Local client {} has no controller bound.", localClient));
		const char* const rootName = this->m_LUI_GetRootName(controller);
		const std::uint64_t roots = LuaSubTable(lui, "roots");
		std::uint64_t root = 0;
		if (!rootName || !roots || !LuaStringField(roots, rootName, root)) {
			return fail(std::format("LUI.roots['{}'] is not in this Lua state.", rootName ? rootName : "?"));
		}

		// self: an open MENU under that root, not the root itself. OpenOverlay walks up from self to
		// the first element with .openMenu and calls :openOverlay on it. The root has none, so passing
		// it walked off the top and raised "attempt to index a nil value" (measured 2026-09-23). The
		// game's buttons pass the menu they sit on; the nearest equivalent is the menu on top of the
		// navigation stack, else any open menu.
		std::string refsNote;
		const std::vector<LuaOpenMenu> menus = LuaOpenMenusUnder(root, refsNote);
		const std::uint64_t navTop = LuaNavTopHash(cod, controller);
		const LuaOpenMenu* parent = nullptr;
		for (const LuaOpenMenu& m : menus) {
			if (navTop && m.hash == navTop) { parent = &m; break; }
		}
		const bool parentIsNavTop = parent != nullptr;
		if (!parent && !menus.empty()) parent = &menus.front();
		std::string menuList;
		for (const LuaOpenMenu& m : menus) menuList += std::format(" 0x{:016X}", m.hash);
		if (!parent) {
			return fail(std::format("there is no open menu under '{}' to open it from ({}).", rootName, refsNote));
		}

		// _sessionMode, read from Enum.eModes so the number is the game's own.
		std::uint64_t modeValue = 0;
		const char* const modeName = mode == 1 ? "MODE_MULTIPLAYER" : "MODE_ZOMBIES";
		if (mode >= 0) {
			const std::uint64_t enumTable = LuaSubTable(globals, "Enum");
			if (!enumTable) return fail("_G.Enum is not a table in this Lua state.");
			std::uint64_t eModes = 0;
			if (!(LuaStringField(enumTable, "eModes", eModes) || LuaHashedIndex(enumTable, kHash_eModes, eModes))
				|| LuaTagOf(eModes) != kLuaTag_Table) {
				return fail("Enum.eModes is not a table in this Lua state.");
			}
			if (!LuaHashedIndex(eModes & kLuaPayloadMask, mode == 1 ? kHash_ModeMultiplayer : kHash_ModeZombies,
					modeValue) || LuaIsTagged(LuaTagOf(modeValue))) {
				return fail(std::format("Enum.eModes.{} is not a number in this Lua state (not in the proxy or its "
					"__index).", modeName));
			}
		}

		// [OpenOverlay, parent, menuKey, controller, params] on the stack, written directly (the values
		// already exist), then the engine builds the params table. The top is saved base-relative and
		// put back on every path - see LuaBaseSlot for why an absolute pointer would dangle.
		std::uint64_t** const topSlot = LuaTopSlot(L);
		std::uint64_t* const top = *topSlot;
		std::uint64_t* last = nullptr;
		SafeRead(reinterpret_cast<const std::uint8_t*>(L) + kLuaState_StackLast, last);
		if (!top || !last || top + 8 >= last) return fail("Not enough Lua stack headroom right now; try again.");
		const std::ptrdiff_t depth = top - *LuaBaseSlot(L);
		auto frame = [&] { return *LuaBaseSlot(L) + depth; };
		auto restore = [&] { *topSlot = frame(); };

		const double controllerNumber = controller;
		std::uint64_t controllerBits = 0;
		std::memcpy(&controllerBits, &controllerNumber, sizeof(controllerBits));
		top[0] = fn;
		top[1] = parent->element;
		top[2] = menuKey;
		top[3] = controllerBits;
		*topSlot = top + 4;

		if (!SafeCreateTable(*this, L, 0, 1) || *topSlot != frame() + 5) {
			restore();
			return fail(std::format("lua_createtable did not push ({}).", kArxanBailed));
		}
		if (mode >= 0) {
			frame()[5] = modeValue;
			*topSlot = frame() + 6;
			if (!SafeSetField(*this, L, -2, "_sessionMode") || *topSlot != frame() + 5) {
				restore();
				return fail(std::format("lua_setfield did not pop ({}).", kArxanBailed));
			}
		}

		this->m_UI_SetUiActive(localClient, true);

		LuiMenus::g_LastError.clear();
		LuiMenus::g_InDispatch = true;
		int status = 0;
		const bool ok = SafeProtectedCall(*this, L, 4, status);
		LuiMenus::g_InDispatch = false;
		LuiMenus::g_HandOpened = true;

		std::string error;
		if (ok && status != 0) {
			std::uint64_t v = 0;
			if (SafeRead(*topSlot - 1, v) && LuaTagOf(v) == kLuaTag_String) LuaReadString(v & kLuaPayloadMask, error);
			if (error.empty()) error = std::format("status {}, no message", status);
		}
		restore();

		const std::string label = std::format("'{}' (0x{:016X})", nameOrHash, fullHash & 0x7FFFFFFFFFFFFFFFULL);
		const std::string params = mode >= 0 ? std::format("_sessionMode = {}", modeName) : "no params";
		const std::string from = std::format("from menu 0x{:016X} ({})", parent->hash,
			parentIsNavTop ? std::string("navigation top")
			: navTop ? std::format("nav top 0x{:016X} is not among them", navTop) : std::string("no navigation stack"));
		LOG("LuiMenus", INFO, "OpenOverlay {} {} on '{}' controller {} {{{}}} -> {}{}{}\n    open menus:{} ({})",
			label, from, rootName, controller, params, !ok ? "FAULTED" : status ? "raised: " + error : "ok",
			LuiMenus::g_LastError.empty() ? "" : " | suppressed fatal: ", LuiMenus::g_LastError, menuList, refsNote);

		if (!ok) return std::format("{} faulted inside the call (contained; the game is still alive).", label);
		if (status != 0) return std::format("OpenOverlay {} {{{}}} raised: {}", label, params, error);
		if (!LuiMenus::g_LastError.empty()) {
			return std::format("OpenOverlay {} {{{}}} ran, but a builder raised (suppressed): {}", label, params,
				LuiMenus::g_LastError);
		}
		return std::format("Opened {} via OpenOverlay {{{}}} {}.", label, params, from);
	}

	namespace {
		struct FrameHeader {
			std::int32_t currentLine = 0;
			std::string text;   // "src:line  name  [what, defined at N]"
		};

		// The printable header of a lua_Debug that lua_getinfo("nSl") has filled.
		FrameHeader ReadFrameHeader(const std::uint8_t* ar) {
			const char* namePtr = nullptr;
			const char* namewhatPtr = nullptr;
			const char* whatPtr = nullptr;
			std::int32_t currentLine = 0;
			std::int32_t lineDefined = 0;
			std::memcpy(&namePtr, ar + Functions::kLuaDebug_Name, sizeof(namePtr));
			std::memcpy(&namewhatPtr, ar + Functions::kLuaDebug_NameWhat, sizeof(namewhatPtr));
			std::memcpy(&whatPtr, ar + Functions::kLuaDebug_What, sizeof(whatPtr));
			std::memcpy(&currentLine, ar + Functions::kLuaDebug_CurrentLine, sizeof(currentLine));
			std::memcpy(&lineDefined, ar + Functions::kLuaDebug_LineDefined, sizeof(lineDefined));

			// short_src is a char array inside our own buffer, already run through luaO_chunkid, so
			// it is the "x64:<hash>.lua" form the engine prints in its own tracebacks - which is what
			// makes these lines line up with the message LuiError_ReportFatal logs.
			const char* const shortSrc = reinterpret_cast<const char*>(ar + Functions::kLuaDebug_ShortSrc);
			std::string src;
			for (std::size_t i = Functions::kLuaDebug_ShortSrc; i < Functions::kLuaDebug_ICI; ++i) {
				const char c = static_cast<char>(ar[i]);
				if (c == '\0') break;
				src.push_back(c);
			}
			if (src.empty()) src = LuaReadCString(shortSrc);

			std::string name = LuaReadCString(namePtr, 128);
			const std::string namewhat = LuaReadCString(namewhatPtr, 32);
			const std::string what = LuaReadCString(whatPtr, 32);

			// `ar.name` is NOT always a C string on this build.
			//
			// T9 adds a namewhat of its own, "xhashfunc", meaning the call was resolved through a
			// hashed-name lookup rather than a textual key. For those frames luaG_getobjname stores
			// a pointer to the interned hashed-name OBJECT, not to characters — so reading it as a
			// string yields whatever the object's first bytes happen to be. That is exactly the
			// engine's own `in function '@&['` (bytes of a pointer field) and, on a run where that
			// field was null, our empty "?".
			//
			// The object's real content is the 63-bit hash at +16, which is the same identifier
			// space as everything else here, so it can be resolved and compared.
			if (namePtr && (namewhat == "xhashfunc" || name.empty())) {
				std::uint64_t hash = 0;
				if (SafeRead(reinterpret_cast<const std::uint8_t*>(namePtr) + Pointers::kLuaHashedName_Hash, hash)
					&& hash) {
					hash &= 0x7FFFFFFFFFFFFFFFULL;
					const std::string resolved = ResolveNameHash(hash);
					name = resolved.empty()
						? std::format("#hash 0x{:016X}", hash)
						: std::format("\"{}\" (#hash 0x{:016X})", resolved, hash);
				}
			}

			FrameHeader h;
			h.currentLine = currentLine;
			h.text = std::format("{}:{}  {}{}{}  [{}, defined at {}]",
				src.empty() ? "?" : src, currentLine,
				namewhat.empty() ? "" : namewhat + " ",
				name.empty() ? "?" : name,
				namewhat.empty() && name.empty() ? "<anonymous>" : "",
				what.empty() ? "?" : what, lineDefined);
			return h;
		}
	}

	// --- The erroring call stack ---------------------------------------------------------------
	//
	// See the header for why the engine's own error text is not enough. Everything here is either a
	// SafeRead or one of the two non-pushing debug calls, so it is safe to run from inside an error
	// handler - which is the only moment it is useful.
	std::string Pointers::LuaDescribeErrorFrames(void* L, int maxLevels, int maxSlotsPerFrame) const {
		if (!L) return "    <no lua_State>\n";
		if (!this->m_lua_getstack || !this->m_lua_getinfo) {
			return "    <lua_getstack/lua_getinfo did not resolve on this build - no frame detail>\n";
		}

		maxLevels = std::clamp(maxLevels, 1, 32);
		maxSlotsPerFrame = std::clamp(maxSlotsPerFrame, 0, 48);

		// L->stack, the base the packed i_ci index is relative to. Distinct from L->base (+80),
		// which is only the CURRENT frame - the whole point here is to reach the others.
		std::uint64_t* stack = nullptr;
		SafeRead(reinterpret_cast<std::uint8_t*>(L) + kLuaState_Stack, stack);

		// Once, before the walk: every hashed name printed below is looked up in this.
		BuildNameHashMap(L);

		std::string out;
		alignas(16) std::uint8_t ar[Functions::kLuaDebugSize];

		for (int level = 0; level < maxLevels; ++level) {
			std::memset(ar, 0, sizeof(ar));

			int ok = 0;
			if (!SafeLuaGetStack(*this, L, level, ar, ok)) {
				out += std::format("    #{}: <lua_getstack faulted inside the engine>\n", level);
				break;
			}
			if (!ok) {
				// 0 means "no such level" - EXCEPT at level 0, where a live state always has a
				// frame. A 0 there is the Arxan caller check bailing, which returns exactly the
				// same value as an honest end-of-stack and would otherwise read as "no frames".
				if (level == 0) return std::format("    <lua_getstack(0) returned 0: {}>\n", kArxanBailed);
				break;
			}
			if (!SafeLuaGetInfo(*this, L, "nSl", ar)) {
				out += std::format("    #{}: <lua_getinfo faulted inside the engine>\n", level);
				break;
			}

			const FrameHeader h = ReadFrameHeader(ar);
			const std::int32_t currentLine = h.currentLine;
			std::uint32_t ici = 0;
			std::memcpy(&ici, ar + Functions::kLuaDebug_ICI, sizeof(ici));
			out += std::format("    #{} {}\n", level, h.text);

			// The frame's own slots. lua_getstack packs the locator into i_ci: the low half is the
			// frame position as an index into L->stack, the high half is the distance to the frame
			// above it.
			//
			// Both halves needed correcting after the first in-game run, and the corrections are
			// recorded here because both produced plausible-looking output while being wrong:
			//
			//  * hi16 is 0 for LEVEL 0, because the topmost frame has no frame above it to measure
			//    against - it runs to L->top. Treating 0 as "no slots" printed nothing at all for
			//    the one frame that actually raised, which is the frame we care about most.
			//
			//  * slot +0 is NOT the first argument. It is an internal frame link: this Lua fork
			//    tags frame links in the low bits of a stack slot (lua_getstack tests `*p & 3` and
			//    `*p & 7 == 3` while walking), so slot +0 comes back as an untagged raw pointer and
			//    renders as an absurd denormal double. The measured layout is
			//        [-1] running closure     [+0] frame link     [+1 ...] arguments, then locals
			//    read off a live dump: for `root:processEvent(event)`, +1 was the tag -14 LUI root
			//    (self) and +2 the event table, which pins it.
			//
			// Slots are still labelled by raw offset rather than as "arg N". The +1 = first argument
			// reading is measured on four frames, not derived from the engine, and printing a
			// derived label would hide it the next time it is wrong.
			const std::uint32_t baseIdx = ici & 0xFFFFu;
			std::uint32_t slotCount = ici >> 16;
			if (!stack || !maxSlotsPerFrame) continue;

			std::uint64_t* const frame = stack + baseIdx;

			if (!slotCount) {
				std::uint64_t* top = nullptr;
				if (SafeRead(reinterpret_cast<std::uint8_t*>(L) + Pointers::kLuaState_Top, top)
					&& top > frame) {
					const std::ptrdiff_t span = top - frame;
					slotCount = static_cast<std::uint32_t>(std::min<std::ptrdiff_t>(span, 64));
				}
			}

			out += std::format("         [-1] fn   = {}\n", LuaBriefRaw(frame - 1));
			out += std::format("         [+0] pc   = 0x{:X}  (saved program counter / frame tag)\n",
				[&] { std::uint64_t v = 0; SafeRead(frame, v); return v; }());
			// Only the frame that actually raised gets the backward window - it is the only one whose
			// register history matters, and it keeps the dump readable.
			out += LuaDescribeInstruction(frame,
				currentLine > 0 ? static_cast<std::uint32_t>(currentLine) : 0u,
				level == 0 ? 64u : 0u);

			// Local names for this frame's registers. R[n] == frame[+1+n], so slot i names register
			// i-1. This is the payoff from luaF_getlocalname: the register that is nil at the fault
			// gets its SOURCE NAME, which no amount of hash cracking was ever going to produce.
			const std::uint8_t* frameCode = nullptr;
			std::uint32_t frameFirstPc = 0;
			const bool haveLocals = LuaFrameCodeAndPc(frame,
				currentLine > 0 ? static_cast<std::uint32_t>(currentLine) : 0u, frameCode, frameFirstPc);

			const std::uint32_t show = std::min<std::uint32_t>(slotCount,
				static_cast<std::uint32_t>(maxSlotsPerFrame));
			for (std::uint32_t i = 1; i < show; ++i) {
				std::string localName;
				if (haveLocals) localName = LuaLocalName(frameCode, frameFirstPc, i - 1);
				out += std::format("         [+{}] {}{}\n", i, LuaBriefRaw(frame + i),
					localName.empty() ? "" : std::format("   ; R{} = local '{}'", i - 1, localName));

				// Expand tables. This is the point of the exercise for the processEvent frame: an
				// "addmenu" event is a table carrying the menu hash, and that hash is the menu
				// identity we have been unable to get any other way — the name is not interned, so
				// neither the wordlists nor the live string table can produce it.
				std::uint64_t v = 0;
				if (SafeRead(frame + i, v) && LuaTagOf(v) == Pointers::kLuaTag_Table) {
					out += LuaDumpTableRaw(v & Pointers::kLuaPayloadMask, 24, 1,
						std::format("              [+{}] ", i));
				}
			}
			if (slotCount > show) {
				out += std::format("         ... {} more slots\n", slotCount - show);
			}
		}

		if (out.empty()) out = "    <no frames>\n";
		return out;
	}

	std::uint64_t Pointers::LuaFrameFunction(void* L, int level) const {
		if (!L || !this->m_lua_getstack) return 0;
		std::uint64_t* stack = nullptr;
		if (!SafeRead(reinterpret_cast<std::uint8_t*>(L) + kLuaState_Stack, stack) || !stack) return 0;

		alignas(16) std::uint8_t ar[Functions::kLuaDebugSize]{};
		int ok = 0;
		if (!SafeLuaGetStack(*this, L, level, ar, ok) || !ok) return 0;
		std::uint32_t ici = 0;
		std::memcpy(&ici, ar + Functions::kLuaDebug_ICI, sizeof(ici));
		const std::uint32_t baseIdx = ici & 0xFFFFu;
		if (!baseIdx) return 0;

		std::uint64_t fn = 0;
		if (!SafeRead(stack + baseIdx - 1, fn) || LuaTagOf(fn) != kLuaTag_Function) return 0;
		return fn & kLuaPayloadMask;
	}

	std::string Pointers::LuaFrameBrief(void* L, int level) const {
		if (!L || !this->m_lua_getstack || !this->m_lua_getinfo) return "-";
		BuildNameHashMap(L);
		alignas(16) std::uint8_t ar[Functions::kLuaDebugSize]{};
		int ok = 0;
		if (!SafeLuaGetStack(*this, L, level, ar, ok) || !ok || !SafeLuaGetInfo(*this, L, "nSl", ar)) return "-";
		return ReadFrameHeader(ar).text;
	}

	std::uint64_t Pointers::LuaCoDFunction(void* L, std::uint64_t tableHash, std::uint64_t fnHash) const {
		if (!L) return 0;
		const std::uint64_t globals = LuaGlobals(L);
		const std::uint64_t cod = globals ? LuaSubTable(globals, "CoD") : 0;
		std::uint64_t table = 0, fn = 0;
		if (!cod || !LuaHashedField(cod, tableHash, table) || LuaTagOf(table) != kLuaTag_Table
			|| !LuaHashedField(table & kLuaPayloadMask, fnHash, fn) || LuaTagOf(fn) != kLuaTag_Function) {
			return 0;
		}
		return fn & kLuaPayloadMask;
	}
}
