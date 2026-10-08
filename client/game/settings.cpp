#include "common.hpp"
#include "game/settings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

namespace Client::Game::Settings {
	namespace {
		namespace fs = std::filesystem;
		// Ordered, so a file we write (or top up with a missing key) keeps the order people wrote it in.
		using Json = nlohmann::ordered_json;

		struct State {
			Values values;
			std::string playerName;
			std::uint64_t playerXuid{};
			std::string note;
		};

		// 63 random bits, never 0 or 1 (1 is what the backend gave every PC before this key existed).
		std::uint64_t NewXuid() {
			std::random_device rd;
			std::uint64_t x = 0;
			while (x <= 1) {
				x = ((std::uint64_t{ rd() } << 32) | rd()) & 0x7FFFFFFFFFFFFFFFULL;
			}
			return x;
		}

		std::string FormatXuid(std::uint64_t x) { return std::format("0x{:016X}", x); }

		// "0x..." or plain hex in a string, or a JSON number. 0 means "not set".
		std::uint64_t ParseXuid(const Json& j) {
			if (j.is_number_unsigned()) return j.get<std::uint64_t>();
			if (!j.is_string()) return 0;
			std::string s = j.get<std::string>();
			if (s.starts_with("0x") || s.starts_with("0X")) s = s.substr(2);
			std::uint64_t x = 0;
			const auto r = std::from_chars(s.data(), s.data() + s.size(), x, 16);
			return r.ec == std::errc{} && r.ptr == s.data() + s.size() && !s.empty() ? x : 0;
		}

		// Every on/off key, with the marker file it replaces. A present marker set the key to
		// `markerValue`; every default is the opposite, so an absent marker means the default.
		struct BoolKey {
			const char* key;
			bool Values::* member;
			const char* marker;
			bool markerValue;
		};
		constexpr BoolKey kBoolKeys[] = {
			{ "backend",           &Values::backend,          nullptr,             false },
			{ "start_screen",      &Values::startScreen,      "no-start-screen",   false },
			{ "scripts",           &Values::scripts,          "no-scripts",        false },
			{ "progression",       &Values::progression,      "no-progression",    false },
			{ "live_menus",        &Values::liveMenus,        "no-live-menus",     false },
			{ "local_playlists",   &Values::localPlaylists,   "no-lpc",            false },
			{ "lobby_waiver",      &Values::lobbyWaiver,      "no-lobby-waiver",   false },
			{ "lua_print",         &Values::luaPrint,         "no-lua-print",      false },
			{ "fpsession_standin", &Values::fpsessionStandin, "fpsession_standin", true },
			{ "ui_text_log",       &Values::uiTextLog,        nullptr,             true },
			{ "custom_maps",       &Values::customMaps,       nullptr,             false },
			{ "ui_scripts",        &Values::uiScripts,        nullptr,             false },
			{ "mapkit_usage",      &Values::mapkitUsage,      nullptr,             true },
			{ "unlock_all",        &Values::unlockAll,        nullptr,             true },
		};
		constexpr const char* kModeMarkers[] = { "online", "online.txt" };
		// Next to the exe, not in cw-mod/: that is where it always lived.
		constexpr const char* kNameFile = "cw_mod_name.txt";

		fs::path Dir() {
			std::error_code ec;
			return fs::current_path(ec) / "cw-mod";
		}

		std::string Join(const std::vector<std::string>& items) {
			std::string out;
			for (const auto& item : items) {
				out += (out.empty() ? "" : ", ") + item;
			}
			return out;
		}

		std::string Lower(std::string s) {
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		// Control characters dropped, spaces trimmed, cut to 32 bytes on a UTF-8 boundary: the engine's
		// name field is char[36].
		std::string SanitizeName(std::string s) {
			std::erase_if(s, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
			const std::size_t first = s.find_first_not_of(' ');
			if (first == std::string::npos) {
				return {};
			}
			s = s.substr(first, s.find_last_not_of(' ') - first + 1);
			if (s.size() > 32) {
				std::size_t cut = 32;
				while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
				s.resize(cut);
			}
			return s;
		}

		bool ParseMode(const std::string& text, Mode& out) {
			const std::string s = Lower(text);
			if (s == "offline") out = Mode::Offline;
			else if (s == "lan") out = Mode::Lan;
			else if (s == "lanlobby") out = Mode::LanLobby;
			else if (s == "online") out = Mode::Online;
			else return false;
			return true;
		}

		Json ToJson(const Values& v) {
			Json j;
			j["name"] = v.name;
			j["xuid"] = FormatXuid(v.xuid);
			j["mode"] = ModeName(v.mode);
			for (const auto& k : kBoolKeys) {
				j[k.key] = v.*k.member;
			}
			j["ui_text"] = Json::object();
			for (const auto& [from, to] : v.uiText) {
				j["ui_text"][from] = to;
			}
			j["mapkit_trace"] = v.mapkitTrace;
			return j;
		}

		bool Write(const fs::path& path, const Json& j) {
			std::error_code ec;
			fs::create_directories(path.parent_path(), ec);
			std::ofstream f(path, std::ios::trunc | std::ios::binary);
			if (!f) {
				return false;
			}
			// replace: a name read from an ANSI cw_mod_name.txt may not be valid UTF-8.
			f << j.dump(2, ' ', false, Json::error_handler_t::replace) << "\n";
			return static_cast<bool>(f);
		}

		// The old marker files that are still on disk, by the name people would look for.
		std::vector<std::string> MarkersOnDisk() {
			std::vector<std::string> found;
			std::error_code ec;
			const fs::path dir = Dir();
			for (const char* name : kModeMarkers) {
				if (fs::exists(dir / name, ec)) found.emplace_back(name);
			}
			for (const auto& k : kBoolKeys) {
				if (k.marker && fs::exists(dir / k.marker, ec)) found.emplace_back(k.marker);
			}
			if (fs::exists(kNameFile, ec)) found.emplace_back(std::string("../") + kNameFile);
			return found;
		}

		// The settings the marker files described, for the one boot that writes cw-mod.json.
		Values FromMarkers() {
			Values v;
			std::error_code ec;
			const fs::path dir = Dir();
			for (const char* name : kModeMarkers) {
				if (!fs::exists(dir / name, ec)) continue;
				std::string token;
				std::ifstream(dir / name) >> token;
				token = Lower(token);
				v.mode = token == "lan" || token == "1" ? Mode::Lan : token == "lanlobby" ? Mode::LanLobby : Mode::Online;
				break;
			}
			for (const auto& k : kBoolKeys) {
				if (k.marker && fs::exists(dir / k.marker, ec)) v.*k.member = k.markerValue;
			}
			if (std::ifstream f(kNameFile); f) {
				std::string line;
				std::getline(f, line);
				v.name = SanitizeName(line);
			}
			return v;
		}

		// Reads every known key into `v`, and adds the ones that are missing to `j`. Returns whether
		// any were added, so the file can be topped up for the next person who opens it.
		bool ReadKeys(Json& j, Values& v) {
			bool added = false;
			auto has = [&](const char* key) {
				if (j.contains(key)) return true;
				added = true;
				return false;
			};

			if (has("name")) {
				if (const auto& n = j["name"]; n.is_string()) {
					v.name = SanitizeName(n.get<std::string>());
					if (v.name != n.get<std::string>()) {
						LOG("Settings", WARN, "cw-mod.json: \"name\" trimmed to \"{}\" (at most 32 bytes, no control "
							"characters).", v.name);
					}
				}
				else {
					LOG("Settings", ERROR, "cw-mod.json: \"name\" must be a string; using the default.");
				}
			}
			else {
				j["name"] = v.name;
			}

			if (has("xuid")) {
				v.xuid = ParseXuid(j["xuid"]);
				if (v.xuid == 0) {
					v.xuid = NewXuid();
					LOG("Settings", ERROR, "cw-mod.json: \"xuid\" is {}; it must be a nonzero hex id like \"{}\". Using {} "
						"for this boot and writing it back.", j["xuid"].dump(), FormatXuid(v.xuid), FormatXuid(v.xuid));
					j["xuid"] = FormatXuid(v.xuid);
					added = true;
				}
			}
			else {
				v.xuid = NewXuid();
				j["xuid"] = FormatXuid(v.xuid);
			}

			if (has("mode")) {
				const auto& m = j["mode"];
				if (!m.is_string() || !ParseMode(m.get<std::string>(), v.mode)) {
					LOG("Settings", ERROR, "cw-mod.json: \"mode\" is {}; it must be \"offline\", \"lan\", \"lanlobby\" or "
						"\"online\". Booting offline.", m.dump());
				}
			}
			else {
				j["mode"] = ModeName(v.mode);
			}

			for (const auto& k : kBoolKeys) {
				if (!has(k.key)) {
					j[k.key] = v.*k.member;
					continue;
				}
				if (const auto& b = j[k.key]; b.is_boolean()) {
					v.*k.member = b.get<bool>();
				}
				else {
					LOG("Settings", ERROR, "cw-mod.json: \"{}\" is {}; it must be true or false. Using {}.",
						k.key, b.dump(), v.*k.member);
				}
			}

			if (has("ui_text")) {
				if (const auto& t = j["ui_text"]; t.is_object()) {
					for (const auto& [from, to] : t.items()) {
						if (from.empty() || !to.is_string()) {
							LOG("Settings", ERROR, "cw-mod.json: \"ui_text\" entry \"{}\" skipped: the key must be the text "
								"the game shows and the value a string.", from);
							continue;
						}
						v.uiText.emplace_back(from, to.get<std::string>());
					}
				}
				else {
					LOG("Settings", ERROR, "cw-mod.json: \"ui_text\" must be an object like {{ \"old text\": \"new text\" }}; "
						"no UI text is replaced.");
				}
			}
			else {
				j["ui_text"] = Json::object();
			}

			if (has("mapkit_trace")) {
				if (const auto& t = j["mapkit_trace"]; t.is_array()) {
					for (const auto& zone : t) {
						if (zone.is_string() && !zone.get<std::string>().empty()) {
							v.mapkitTrace.push_back(Lower(zone.get<std::string>()));
						}
						else {
							LOG("Settings", ERROR, "cw-mod.json: \"mapkit_trace\" entry {} skipped: it must be a zone name.",
								zone.dump());
						}
					}
				}
				else {
					LOG("Settings", ERROR, "cw-mod.json: \"mapkit_trace\" must be a list of zone names like [\"zm_silver\"]; "
						"nothing is traced.");
				}
			}
			else {
				j["mapkit_trace"] = Json::array();
			}

			for (const auto& [key, value] : j.items()) {
				const bool known = key == "name" || key == "xuid" || key == "mode" || key == "ui_text"
					|| key == "mapkit_trace"
					|| std::ranges::any_of(kBoolKeys, [&](const BoolKey& k) { return key == k.key; });
				if (!known) {
					LOG("Settings", WARN, "cw-mod.json: unknown key \"{}\" ignored (a typo?).", key);
				}
			}
			return added;
		}

		std::string ResolvePlayerName(const std::string& configured, const char*& source) {
			char env[64];
			if (const DWORD n = GetEnvironmentVariableA("CW_MOD_NAME", env, sizeof(env)); n > 0 && n < sizeof(env)) {
				if (std::string s = SanitizeName(env); !s.empty()) {
					source = "CW_MOD_NAME";
					return s;
				}
			}
			if (!configured.empty()) {
				source = "cw-mod.json";
				return configured;
			}
			char user[256];
			DWORD size = sizeof(user);
			if (GetUserNameA(user, &size)) {
				if (std::string s = SanitizeName(user); !s.empty()) {
					source = "Windows account";
					return s;
				}
			}
			source = "fallback";
			return "Unknown Soldier";
		}

		State Load() {
			State s;
			const fs::path path = Path();
			std::error_code ec;

			if (!fs::exists(path, ec)) {
				const std::vector<std::string> markers = MarkersOnDisk();
				s.values = FromMarkers();
				s.values.xuid = NewXuid();
				if (!Write(path, ToJson(s.values))) {
					s.note = "could not write cw-mod.json; this boot uses the old marker files";
					LOG("Settings", ERROR, "{} ({}).", s.note, path.string());
				}
				else if (markers.empty()) {
					s.note = "created with defaults";
					LOG("Settings", INFO, "Created {} with the defaults.", path.string());
				}
				else {
					s.note = std::format("created from {} old marker file(s)", markers.size());
					LOG("Settings", WARN, "Created {} from the old marker files ({}). They are not read any more; "
						"delete them when you like.", path.string(), Join(markers));
				}
			}
			else {
				std::stringstream text;
				text << std::ifstream(path, std::ios::binary).rdbuf();
				try {
					// Plain JSON, no comments: tools/dwserver/run.py reads the same file with Python's parser.
					Json j = Json::parse(text.str());
					if (!j.is_object()) {
						throw std::runtime_error("the file is not a JSON object ({ ... })");
					}
					if (ReadKeys(j, s.values) && !Write(path, j)) {
						LOG("Settings", WARN, "cw-mod.json: could not add the missing keys to the file.");
					}
					s.note = "loaded";
				}
				catch (const std::exception& e) {
					s.values = Values{};
					s.values.xuid = NewXuid();   // this boot only; the broken file is left alone
					s.note = std::format("cw-mod.json is broken, so every setting is at its default (offline): {}", e.what());
					LOG("Settings", ERROR, "{}", s.note);
				}

				if (const auto markers = MarkersOnDisk(); !markers.empty()) {
					LOG("Settings", WARN, "cw-mod.json is in charge; these old marker files are NOT read: {}. "
						"Delete them to avoid confusion.", Join(markers));
				}
			}

			const char* source = "";
			s.playerName = ResolvePlayerName(s.values.name, source);

			const char* xuidSource = "cw-mod.json";
			s.playerXuid = s.values.xuid;
			char env[32];
			if (const DWORD n = GetEnvironmentVariableA("CW_MOD_XUID", env, sizeof(env)); n > 0 && n < sizeof(env)) {
				if (const std::uint64_t x = ParseXuid(Json(std::string(env))); x != 0) {
					s.playerXuid = x;
					xuidSource = "CW_MOD_XUID";
				}
			}

			std::string keys;
			for (const auto& k : kBoolKeys) {
				keys += std::format(" {}={}", k.key, s.values.*k.member ? "on" : "off");
			}
			LOG("Settings", INFO, "name \"{}\" (from {}), xuid {} (from {}), mode {},{} ui_text={} entr{}", s.playerName, source,
				FormatXuid(s.playerXuid), xuidSource, ModeName(s.values.mode), keys, s.values.uiText.size(),
				s.values.uiText.size() == 1 ? "y" : "ies");
			return s;
		}

		State& Current() {
			static State s = Load();
			return s;
		}
	}

	const Values& Get() { return Current().values; }
	const std::string& PlayerName() { return Current().playerName; }
	std::uint64_t PlayerXuid() { return Current().playerXuid; }
	const std::string& LoadNote() { return Current().note; }

	fs::path Path() { return Dir() / "cw-mod.json"; }

	const char* ModeName(Mode mode) {
		switch (mode) {
		case Mode::Lan: return "lan";
		case Mode::LanLobby: return "lanlobby";
		case Mode::Online: return "online";
		default: return "offline";
		}
	}
}
