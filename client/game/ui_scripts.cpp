// cw-mod/ui_scripts: our own menu Lua, run as source in the LUI state. See ui_scripts.hpp.
#include "common.hpp"
#include "game/ui_scripts.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/mapkit_loader.hpp"
#include "game/settings.hpp"
#include "game/ui_scripts_custom_maps.hpp"
#include "game/ui_scripts_server_browser.hpp"
#include "scripting/scripting.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Client::Game::UiScripts {
	namespace {
		namespace fs = std::filesystem;
		using Clock = std::chrono::steady_clock;

		// lua_State and global_State fields (the NaN-boxed LuaJIT 2.1 layout T9 uses; see lua_state.cpp).
		constexpr std::size_t kL_Global    = 24;   // L->glref
		constexpr std::size_t kL_StackLast = 40;   // L->maxstack
		constexpr std::size_t kL_Base      = 80;   // L->base
		// The lexer's @"..." hash callback and the hashed-name intern table (kDump_lua_load).
		constexpr std::size_t kG_XhashCreateFn = 816;
		constexpr std::size_t kG_XhashBuckets  = 16;
		constexpr std::size_t kG_XhashMask     = 24;
		constexpr std::size_t kG_XhashCount    = 28;
		constexpr std::size_t kXhash_Type      = 8;
		constexpr std::size_t kXhash_Value     = 16;
		constexpr std::uint8_t kXhashObjType   = 5;
		constexpr std::uint64_t kMask60 = 0x0FFFFFFFFFFFFFFFULL;
		constexpr std::uint64_t kMask63 = 0x7FFFFFFFFFFFFFFFULL;
		constexpr std::size_t kMaxResultText = 64 * 1024;

		using XhashCreateFn = std::uint64_t(const char* text, std::uint32_t len);

		void* g_State = nullptr;                          // the state the scripts last ran in
		std::string g_ActivePlaylist;                     // the CUSTOM MAPS pick's playlist, as Lua sent it
		std::map<std::string, fs::file_time_type> g_Ran;  // file name -> stamp it ran with
		std::map<std::string, fs::file_time_type> g_Pending;  // changed, run once the stamp holds still
		Clock::time_point g_NextScan{};

		// The SERVER BROWSER menu (ui_scripts_server_browser.hpp). Game thread only.
		struct Browser {
			bool open = false;             // between Lua's "browser open" and "browser close"
			std::string pushed;            // the data chunk Lua last got; a different one is sent
			Clock::time_point next{};      // the next look at whether to send
			std::uint64_t join = 0;        // the host XUID Lua asked to join, until the next tick runs it
			int joinSerial = 0;            // counts join answers, so Lua can tell a new one from the last
			bool joinOk = false;
			std::string joinText;
		};
		Browser g_Browser;

		// Our lexer hash callback's state for one lua_load. Game thread only.
		struct LexState {
			const std::uint8_t* G = nullptr;
			bool tableRead = false;
			std::unordered_map<std::uint64_t, std::uint64_t> byLow60;   // low 60 bits -> value, 0 = ambiguous
			std::vector<std::string> notes;
		};
		LexState* g_Lex = nullptr;

		fs::path Folder() {
			std::error_code ec;
			return fs::current_path(ec) / "cw-mod" / "ui_scripts";
		}

		int TagOf(std::uint64_t v) {
			return static_cast<int>(static_cast<std::int64_t>(v) >> Pointers::kLuaTagShift);
		}

		// Every interned hashed name's value into out[0..cap). -1 when the table faults. POD only (C2712).
		long long CollectXhashesGuarded(const std::uint8_t* G, std::uint64_t* out, std::size_t cap) {
			__try {
				const auto* const* buckets = *reinterpret_cast<const std::uint8_t* const* const*>(G + kG_XhashBuckets);
				const std::uint32_t mask = *reinterpret_cast<const std::uint32_t*>(G + kG_XhashMask);
				if (!buckets || mask > 0x3FFFFFF) return -1;
				std::size_t n = 0, steps = 0;
				for (std::uint64_t i = 0; i <= mask; ++i) {
					for (const std::uint8_t* o = buckets[i]; o; o = *reinterpret_cast<const std::uint8_t* const*>(o)) {
						if (++steps > 0x4000000 || n >= cap) return -1;
						if (o[kXhash_Type] == kXhashObjType) out[n++] = *reinterpret_cast<const std::uint64_t*>(o + kXhash_Value);
					}
				}
				return static_cast<long long>(n);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
		}

		void ReadInternTable(LexState& lex) {
			lex.tableRead = true;
			std::uint32_t count = 0;
			if (!SafeRead(lex.G + kG_XhashCount, count)) return;
			std::vector<std::uint64_t> values(static_cast<std::size_t>(count) + 4096);
			long long n = 0;
			{
				SafeProbeScope scope;
				n = CollectXhashesGuarded(lex.G, values.data(), values.size());
			}
			if (n < 0) {
				lex.notes.push_back("the hashed-name intern table did not read; every @\"0x...\" is its number as-is");
				return;
			}
			lex.byLow60.reserve(static_cast<std::size_t>(n));
			for (long long i = 0; i < n; ++i) {
				const std::uint64_t v = values[static_cast<std::size_t>(i)];
				auto [it, fresh] = lex.byLow60.emplace(v & kMask60, v);
				if (!fresh && it->second != v) it->second = 0;
			}
		}

		// The lexer's hash callback while our chunk is parsed (see ui_scripts.hpp for the two forms).
		std::uint64_t LexHash(const char* text, std::uint32_t len) {
			if (!text) return 0;
			if (len > 2 && len <= 18 && text[0] == '0' && (text[1] | 0x20) == 'x') {
				std::uint64_t v = 0;
				bool hex = true;
				for (std::uint32_t i = 2; i < len && hex; ++i) {
					const char c = text[i];
					const int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
					if (d < 0) hex = false;
					else v = (v << 4) | static_cast<std::uint64_t>(d);
				}
				if (hex) {
					if (v > kMask60 || !g_Lex) return v & kMask63;   // a full hash: as written
					if (!g_Lex->tableRead) ReadInternTable(*g_Lex);
					const auto it = g_Lex->byLow60.find(v);
					if (it != g_Lex->byLow60.end() && it->second) return it->second;
					g_Lex->notes.push_back(std::format("@\"{}\": {}, used as-is", std::string(text, len),
						it == g_Lex->byLow60.end() ? "no hashed name in the state has those 60 bits"
						: "more than one hashed name has those 60 bits"));
					return v;
				}
			}
			std::uint64_t h = 0xCBF29CE484222325ULL;
			for (std::uint32_t i = 0; i < len; ++i) {
				unsigned char c = static_cast<unsigned char>(text[i]);
				if (c >= 'A' && c <= 'Z') c += 32;
				h = (h ^ c) * 0x100000001B3ULL;
			}
			return h & kMask63;
		}

		struct Chunk {
			const char* data;
			std::size_t size;
			bool given;
		};

		const char* ReadChunk(void*, void* data, std::size_t* size) {
			auto* chunk = static_cast<Chunk*>(data);
			if (chunk->given) {
				*size = 0;
				return nullptr;
			}
			chunk->given = true;
			*size = chunk->size;
			return chunk->data;
		}

		// POD-only (C2712), like the Safe* wrappers in lua_state.cpp.
		bool SafeLoad(void* L, Chunk* chunk, const char* name, int& status) {
			__try { status = g_Pointers->m_lua_load(L, &ReadChunk, chunk, name); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		bool SafeCall(void* L, int nresults, int& status) {
			__try { status = g_Pointers->m_LUI_ProtectedCall(L, 0, nresults, 0); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		// The value at L->top - 1 as text: a string in full (up to kMaxResultText), anything else the way
		// Pointers::LuaResultText renders it.
		std::string TopText(void* L) {
			std::uint64_t* top = nullptr;
			std::uint64_t v = 0;
			if (!SafeRead(static_cast<std::uint8_t*>(L) + Pointers::kLuaState_Top, top) || !top || !SafeRead(top - 1, v)) {
				return "<unreadable>";
			}
			if (TagOf(v) != Pointers::kLuaTag_String) {
				std::string out;
				return Pointers::LuaResultText(L, out) ? out : "<unreadable>";
			}
			const auto* ts = reinterpret_cast<const std::uint8_t*>(v & Pointers::kLuaPayloadMask);
			std::uint32_t len = 0;
			if (!SafeRead(ts + Pointers::kLuaTString_Len, len)) return "<unreadable string>";
			std::string out(std::min<std::size_t>(len, kMaxResultText), '\0');
			if (!out.empty() && !SafeCopy(out.data(), ts + Pointers::kLuaTString_Data, out.size())) return "<unreadable string>";
			return out;
		}

		// Load and run one chunk of source; chunkName is what its errors are prefixed with. Everything it pushes
		// is popped again, on every path. quiet: no log line for a run that went well.
		void RunSource(void* L, std::string source, const std::string& name, const std::string& chunkName, const char* why,
				bool quiet = false) {
			if (source.starts_with("\xEF\xBB\xBF")) source.erase(0, 3);   // the parser does not skip a BOM

			auto* const l = static_cast<std::uint8_t*>(L);
			auto** const topSlot = reinterpret_cast<std::uint64_t**>(l + Pointers::kLuaState_Top);
			auto** const baseSlot = reinterpret_cast<std::uint64_t**>(l + kL_Base);
			std::uint64_t* top = nullptr;
			std::uint64_t* base = nullptr;
			std::uint64_t* last = nullptr;
			std::uint8_t* G = nullptr;
			if (!SafeRead(topSlot, top) || !SafeRead(baseSlot, base) || !SafeRead(l + kL_StackLast, last)
				|| !SafeRead(l + kL_Global, G) || !top || !base || !last || !G) {
				LOG("UiScripts", ERROR, "{} ({}): the Lua state did not read; not run.", name, why);
				return;
			}
			if (top + 8 >= last) {
				LOG("UiScripts", WARN, "{} ({}): no Lua stack headroom right now; not run.", name, why);
				return;
			}
			// Base-relative: the parser and the call can reallocate the stack (see LuaBaseSlot in lua_state.cpp).
			const std::ptrdiff_t depth = top - base;
			auto frame = [&] { return *baseSlot + depth; };

			// Our hash callback for the length of the parse, then whatever was there before.
			void* previousHash = nullptr;
			SafeRead(G + kG_XhashCreateFn, previousHash);
			LexState lex;
			lex.G = G;
			g_Lex = &lex;
			XhashCreateFn* ours = &LexHash;
			SafeWrite(G + kG_XhashCreateFn, reinterpret_cast<void*>(ours));

			Chunk chunk{ source.data(), source.size(), false };
			int status = -1;
			const bool loaded = SafeLoad(L, &chunk, chunkName.c_str(), status);

			SafeWrite(G + kG_XhashCreateFn, previousHash);
			g_Lex = nullptr;
			for (const std::string& note : lex.notes) {
				LOG("UiScripts", WARN, "{}: {}", name, note);
			}

			if (!loaded) {
				*topSlot = frame();
				LOG("UiScripts", ERROR, "{} ({}): lua_load faulted (contained).", name, why);
				return;
			}
			if (status != 0) {
				const std::string error = TopText(L);
				*topSlot = frame();
				LOG("UiScripts", ERROR, "{} ({}): does not compile: {}", name, why, error);
				return;
			}
			std::uint64_t fn = 0;
			if (*topSlot != frame() + 1 || !SafeRead(frame(), fn) || TagOf(fn) != Pointers::kLuaTag_Function) {
				*topSlot = frame();
				LOG("UiScripts", ERROR, "{} ({}): lua_load returned 0 but did not push a function.", name, why);
				return;
			}

			int callStatus = -1;
			const bool called = SafeCall(L, 1, callStatus);
			const std::string result = called && *topSlot == frame() + 1 ? TopText(L) : std::string();
			*topSlot = frame();

			if (!called) {
				LOG("UiScripts", ERROR, "{} ({}): faulted while running (contained).", name, why);
			}
			else if (callStatus != 0) {
				LOG("UiScripts", ERROR, "{} ({}): raised: {}", name, why, result);
			}
			else if (!quiet) {
				LOG("UiScripts", INFO, "{} ({}): {}", name, why, result.empty() || result == "nil" ? "ok" : result);
			}
		}

		void RunScript(void* L, const fs::path& file, const char* why) {
			const std::string name = file.filename().string();
			std::ifstream in(file, std::ios::binary);
			if (!in) {
				LOG("UiScripts", WARN, "{} ({}): could not be read.", name, why);
				return;
			}
			std::string source;
			source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			RunSource(L, std::move(source), name, "@ui_scripts/" + name, why);
		}

		// A Lua string literal holding s exactly.
		std::string LuaQuote(std::string_view s) {
			std::string out = "\"";
			for (const char c : s) {
				const auto u = static_cast<unsigned char>(c);
				if (c == '"' || c == '\\') {
					out += '\\';
					out += c;
				}
				else if (u < 32 || u == 127) {
					out += std::format("\\{:03}", static_cast<int>(u));
				}
				else {
					out += c;
				}
			}
			return out + "\"";
		}

		// CWMOD.maps (the listed maps, for the CUSTOM MAPS tab), CWMOD.active and CWMOD.activePlaylist (the pick
		// the DLL holds, so a rebuilt LUI state neither reports it again nor drops it when the lobby re-sets it).
		std::string MapsPrelude() {
			std::string maps;
			for (const MapKit::MapFolder& map : MapKit::ListedMaps()) {
				maps += std::format("\t{{ id = {}, title = {}, base = {}, description = {}, playlist = {} }},\n",
					LuaQuote(map.name), LuaQuote(map.title), LuaQuote(map.base), LuaQuote(map.description),
					map.playlist ? std::to_string(map.playlist) : "nil");
			}
			const std::string active = MapKit::PickedMap();
			const bool playlist = !active.empty() && !g_ActivePlaylist.empty()
				&& std::ranges::all_of(g_ActivePlaylist, [](char c) { return c >= '0' && c <= '9'; });
			return std::format("rawset(_G, \"CWMOD\", rawget(_G, \"CWMOD\") or {{}})\n"
				"local S = rawget(_G, \"CWMOD\")\n"
				"S.active = {}\n"
				"S.activePlaylist = {}\n"
				"S.maps = {{\n{}}}\n"
				"return #S.maps .. \" listed map(s), active: \" .. (S.active ~= \"\" and S.active or \"none\")\n",
				LuaQuote(active), playlist ? g_ActivePlaylist : std::string("nil"), maps);
		}

		std::string PlayersLabel(int members, int maxClients) {
			return maxClients > 0 ? std::format("{}/{}", members, maxClients) : std::to_string(members);
		}

		// What the SERVER BROWSER menu shows, as the chunk that hands it to the menu's Lua: the snapshot and the
		// wording of the overlay's Server Browser tab (overlay/tabs/server_browser.cpp), minus its packet
		// counters, which would make a new chunk every second.
		std::string BrowserChunk(std::size_t& hostCount) {
			namespace Lan = LanBrowser;
			const Lan::Status st = Lan::GetStatus();
			const std::vector<Lan::Host> hosts = Lan::Hosts();
			const std::uint64_t now = GetTickCount64();
			hostCount = hosts.size();

			const std::string status = st.socketUp
				? std::format("Listening on UDP {} ({} broadcast target{}).", Lan::kPort, st.broadcastTargets,
					st.broadcastTargets == 1 ? "" : "s")
				: std::format("LAN socket down: {}", st.socketError.empty() ? "not started yet" : st.socketError);
			std::string advert;
			if (st.advertising) {
				const Lan::Advert& a = st.advert;
				const char* const friendly = Lan::FriendlyMapName(a.map);
				advert = std::format("Advertising your lobby: '{}', {}, {} players, {}.", a.name,
					*friendly ? friendly : a.map.empty() ? "no map yet" : a.map.c_str(), PlayersLabel(a.members, a.maxClients),
					a.inMatch ? "in match" : "in lobby");
			}
			else {
				advert = std::format("Not advertising: {}.", Lan::g_Advertise.load() ? "no live lobby on this PC"
					: "turned off in the overlay");
			}

			std::string rows;
			for (const Lan::Host& h : hosts) {
				const char* const friendly = Lan::FriendlyMapName(h.map);
				const std::string map = *friendly ? friendly : h.map.empty() ? "-" : h.map;
				const std::string mapLong = h.map.empty() ? "-" : *friendly ? std::format("{} ({})", friendly, h.map) : h.map;
				const std::string mode = std::format("{}{}{}", Lan::GameModeName(h.gameMode), h.gametype.empty() ? "" : " / ",
					h.gametype);
				const char* const network = h.networkMode == 1 ? "LAN" : h.networkMode == 2 ? "online"
					: h.networkMode == 0 ? "offline" : "?";

				// The beacon is once a second, and a row goes 5 s after its host went quiet: only a late one says so.
				const std::uint64_t quiet = now > h.lastSeenMs ? (now - h.lastSeenMs) / 1000 : 0;
				const std::string address = quiet >= 2 ? std::format("{}   (quiet for {} s)", h.address, quiet) : h.address;

				std::string playerList = PlayersLabel(h.members, h.maxClients);
				for (std::size_t i = 0; i < h.players.size(); ++i) {
					playerList += i ? ", " : ": ";
					playerList += h.players[i];
				}
				if (static_cast<int>(h.players.size()) < h.members) {
					playerList += std::format(" (+{} not listed)", h.members - static_cast<int>(h.players.size()));
				}

				std::string build = h.build.empty() ? "?" : h.build;
				if (!h.build.empty() && h.build != g_GameIdentifier.m_Version) {
					build += std::format("   (yours: {}, different build)", g_GameIdentifier.m_Version);
				}
				std::string mod = h.mod.empty() ? "?" : h.mod;
				if (!h.mod.empty() && h.mod != GIT_DESCRIBE) {
					mod += "   (yours: " GIT_DESCRIBE ")";
				}

				rows += std::format("\t{{ xuid = \"{:016X}\", host = {}, map = {}, mode = {}, players = {}, state = {}, ping = {},\n"
					"\t  address = {}, pingLong = {}, mapLong = {}, gametype = {}, modeLong = {},\n"
					"\t  playerList = {}, hostId = {}, build = {}, mod = {} }},\n",
					h.xuid, LuaQuote(h.name), LuaQuote(map), LuaQuote(mode), LuaQuote(PlayersLabel(h.members, h.maxClients)),
					h.inMatch ? "\"in match\"" : "\"lobby\"", LuaQuote(h.pingMs < 0 ? "-" : std::format("{:.0f} ms", h.pingMs)),
					LuaQuote(address), LuaQuote(h.pingMs < 0 ? "no answer yet" : std::format("{:.1f} ms", h.pingMs)),
					LuaQuote(mapLong), LuaQuote(h.gametype.empty() ? "-" : h.gametype),
					LuaQuote(std::format("{}, {}, {}", Lan::GameModeName(h.gameMode), network, h.inMatch ? "in match" : "in lobby")),
					LuaQuote(playerList), LuaQuote(std::format("0x{:016X}, slot {}", h.xuid, h.slot)), LuaQuote(build),
					LuaQuote(mod));
			}

			return std::format("local S = rawget(_G, \"CWMOD\")\n"
				"local B = type(S) == \"table\" and S.browser\n"
				"if type(B) ~= \"table\" then return \"this LUI state has no server browser\" end\n"
				"B.status = {}\n"
				"B.advert = {}\n"
				"B.joinSerial = {}\n"
				"B.joinOk = {}\n"
				"B.joinText = {}\n"
				"B.hosts = {{\n{}}}\n"
				"B.serial = (B.serial or 0) + 1\n"
				"return #B.hosts .. \" game(s)\"\n",
				LuaQuote(status), LuaQuote(advert), g_Browser.joinSerial, g_Browser.joinOk ? "true" : "false",
				LuaQuote(g_Browser.joinText), rows);
		}

		// The join the menu asked for, by host XUID. Here, in the tick, like the overlay's join (a queued action
		// run from this same tick), not inside the Lua call that asked.
		void RunBrowserJoin() {
			const std::uint64_t xuid = std::exchange(g_Browser.join, 0);
			std::string name = std::format("0x{:016X}", xuid);
			std::string report = "that game is no longer listed.";
			bool accepted = false;
			for (const LanBrowser::Host& h : LanBrowser::Hosts()) {
				if (h.xuid != xuid) continue;
				name = h.name;
				// ctx 0, pad 0, jointype 4: the overlay tab's defaults (a LAN host refuses any jointype but 1 and 4).
				report = g_Pointers->JoinHostByDescriptor(h.blob, 0, 0, 4);
				// join.cpp's wording for a ClientSession_JoinPendingTarget that returned true.
				accepted = report.find("-> accepted") != std::string::npos;
				break;
			}

			// The report is written for the overlay's log pane: several lines once it reached the engine call, one
			// "Join by descriptor: <why>" before that. The menu has one line.
			std::string line = report.substr(0, report.find('\n'));
			constexpr std::string_view kPrefix = "Join by descriptor: ";
			if (line.starts_with(kPrefix)) line.erase(0, kPrefix.size());
			++g_Browser.joinSerial;
			g_Browser.joinOk = accepted;
			g_Browser.joinText = accepted ? "Joining..." : report.find("REFUSED") != std::string::npos
				? "Join: the game refused to start it." : "Join: " + line;
			g_Browser.next = {};
			LOG("UiScripts", INFO, "Server browser: join '{}' {}. {}", name, accepted ? "started" : "did NOT start", report);
		}

		// While the menu is open: a new data chunk whenever what it shows changed, looked at four times a second.
		void TickBrowser(Clock::time_point now) {
			if (g_Browser.join) RunBrowserJoin();
			if (!g_Browser.open || now < g_Browser.next) return;
			g_Browser.next = now + std::chrono::milliseconds(250);

			void* L = nullptr;
			if (!g_Pointers->m_g_luiCtx || !SafeRead(g_Pointers->m_g_luiCtx, L) || L != g_State) return;
			std::size_t hostCount = 0;
			std::string chunk = BrowserChunk(hostCount);
			if (chunk == g_Browser.pushed) return;

			static std::size_t s_LoggedCount = static_cast<std::size_t>(-1);
			if (g_Browser.pushed.empty() || hostCount != s_LoggedCount) {
				s_LoggedCount = hostCount;
				LOG("UiScripts", INFO, "Server browser: {} LAN game(s) sent to the menu.", hostCount);
			}
			RunSource(L, chunk, "server browser data", "=cw-mod server browser data", "push", true);
			g_Browser.pushed = std::move(chunk);
		}

		void OnBrowserCommand(std::string_view arg) {
			const std::size_t space = arg.find(' ');
			const std::string_view what = arg.substr(0, space);
			if (what == "open") {
				if (!g_Browser.open) LOG("UiScripts", INFO, "Server browser menu opened.");
				g_Browser.open = true;
				g_Browser.pushed.clear();   // the new menu starts empty: send what there is, whatever was sent before
				g_Browser.next = {};
				// The last join's answer is not this menu's: an old "started" would close it as it opens.
				g_Browser.joinOk = false;
				g_Browser.joinText.clear();
			}
			else if (what == "close") {
				if (g_Browser.open) LOG("UiScripts", INFO, "Server browser menu closed.");
				g_Browser.open = false;
			}
			else if (what == "join" && space != std::string_view::npos) {
				const std::string_view hex = arg.substr(space + 1);
				std::uint64_t xuid = 0;
				const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), xuid, 16);
				if (ec != std::errc() || end != hex.data() + hex.size() || !xuid) {
					LOG("UiScripts", WARN, "Server browser: join '{}' is not a host XUID; ignored.", hex);
					return;
				}
				g_Browser.join = xuid;
			}
			else {
				LOG("UiScripts", WARN, "unknown server browser command from Lua: '{}'", arg);
			}
		}

		// Scripts built into the DLL, run after the prelude unless the folder has a file of the same name.
		struct Builtin {
			const char* name;
			const char* source;
		};
		constexpr Builtin kBuiltins[] = {
			{ "custom_maps.lua", kCustomMapsLua },
			{ "server_browser.lua", kServerBrowserLua },
		};

		// The folder's *.lua files with their stamps, in name order.
		std::map<std::string, fs::file_time_type> ListScripts() {
			std::map<std::string, fs::file_time_type> out;
			std::error_code ec;
			for (const auto& entry : fs::directory_iterator(Folder(), ec)) {
				if (!entry.is_regular_file(ec)) continue;
				const fs::path& p = entry.path();
				if (_stricmp(p.extension().string().c_str(), ".lua") != 0) continue;
				const auto stamp = fs::last_write_time(p, ec);
				if (!ec) out.emplace(p.filename().string(), stamp);
			}
			return out;
		}

		bool Ready() {
			return Settings::Get().uiScripts && g_Pointers && g_Pointers->m_lua_load && g_Pointers->m_LUI_ProtectedCall;
		}
	}

	void OnMainLoaded(void* L) {
		if (!L || !Settings::Get().uiScripts) return;
		if (!Ready()) {
			LOG("UiScripts", ERROR, "lua_load or LUI_ProtectedCall did not resolve on this build: cw-mod/ui_scripts not run.");
			return;
		}
		g_State = L;
		g_Ran.clear();
		g_Pending.clear();
		// A menu of the last UI state went with it, without its "browser close".
		g_Browser.open = false;
		g_Browser.join = 0;
		g_Browser.pushed.clear();

		const auto scripts = ListScripts();
		{
			void* hash = nullptr;
			std::uint8_t* G = nullptr;
			SafeRead(static_cast<std::uint8_t*>(L) + kL_Global, G);
			if (G) SafeRead(G + kG_XhashCreateFn, hash);
			LOG("UiScripts", INFO, "ui/main.lua ran: {} script(s) in {} (the engine's own @-literal hash callback is {}).",
				scripts.size(), Folder().string(), hash ? std::format("0x{:X}", reinterpret_cast<std::uintptr_t>(hash)) : "null");
		}
		RunSource(L, MapsPrelude(), "maps prelude", "=cw-mod maps prelude", "boot");
		for (const Builtin& builtin : kBuiltins) {
			if (scripts.contains(builtin.name)) {
				LOG("UiScripts", INFO, "{}: the file in ui_scripts replaces the built-in one.", builtin.name);
				continue;
			}
			RunSource(L, builtin.source, builtin.name, std::string("@cw-mod/") + builtin.name, "built-in");
		}
		for (const auto& [name, stamp] : scripts) {
			RunScript(L, Folder() / name, "boot");
			g_Ran[name] = stamp;
		}
	}

	void OnCommand(std::string_view command) {
		const std::size_t space = command.find(' ');
		const std::string_view verb = command.substr(0, space);
		const std::string_view arg = space == std::string_view::npos ? std::string_view() : command.substr(space + 1);
		if (verb == "map") {
			// "map <id> <playlist>", or "map" alone for none.
			const std::size_t split = arg.find(' ');
			const std::string id(arg.substr(0, split));
			const bool active = MapKit::SetActiveMap(id);
			g_ActivePlaylist = active && split != std::string_view::npos ? std::string(arg.substr(split + 1)) : std::string();
			if (!active || !Client::Scripting::LoaderActive()) return;
			// The picked map's level scripts are served, any other listed map's are not (scripting.cpp).
			LOG("UiScripts", INFO, "CUSTOM MAPS pick '{}': scripts reloaded. {}", id, Client::Scripting::ReloadScripts());
		}
		else if (verb == "browser") {
			OnBrowserCommand(arg);
		}
		else if (verb == "log") {
			LOG("UiScripts", INFO, "(Lua) {}", arg);
		}
		else {
			LOG("UiScripts", WARN, "unknown command from Lua: '{}'", command);
		}
	}

	bool MenuOpen() {
		return g_Browser.open;
	}

	void Tick() {
		if (!g_State || !Ready()) return;
		const auto now = Clock::now();
		TickBrowser(now);
		if (now < g_NextScan) return;
		g_NextScan = now + std::chrono::seconds(1);

		void* L = nullptr;
		if (!g_Pointers->m_g_luiCtx || !SafeRead(g_Pointers->m_g_luiCtx, L) || L != g_State) {
			static void* s_Reported = nullptr;
			if (s_Reported != L) {
				s_Reported = L;
				LOG("UiScripts", INFO, "The LUI state changed without ui/main.lua; live reload paused until it runs again.");
			}
			return;
		}

		const auto scripts = ListScripts();
		for (const auto& [name, stamp] : scripts) {
			const auto ran = g_Ran.find(name);
			if (ran != g_Ran.end() && ran->second == stamp) {
				g_Pending.erase(name);
				continue;
			}
			// Run a changed file once its stamp has held for a whole scan: an editor may still be writing it.
			const auto pending = g_Pending.find(name);
			if (pending == g_Pending.end() || pending->second != stamp) {
				g_Pending[name] = stamp;
				continue;
			}
			g_Pending.erase(pending);
			const char* const why = ran == g_Ran.end() ? "new" : "changed";
			g_Ran[name] = stamp;
			RunScript(L, Folder() / name, why);
		}
	}
}
