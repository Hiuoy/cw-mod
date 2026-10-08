#include "common.hpp"
#include "game/dw_net.hpp"
#include "game/settings.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace Client::Game::DwNet {
	std::atomic<bool> g_Redirect{ false };
	std::atomic<bool> g_Block{ false };
	std::atomic<bool> g_Journal{ false };

	namespace {
		// Matched as case-insensitive SUBSTRINGS, not exact names, and that is deliberate.
		//
		// T9 does not hold finished hostnames — it holds printf templates it fills at runtime with
		// the environment and the title codename ("navyblue"): `auth3.%s.demonware.net`,
		// `loginqueue.%s.demonware.net`, `https://%s.umbrella.demonware.net`,
		// `https://%s.uno.demonware.net/v1.0`, plus the already-complete
		// `navyblue-auth3.prod.demonware.net`, `navyblue-lobby.prod.demonware.net`,
		// `objectstore.prod.demonware.net`, and the STUN set `stun.{us,eu,jp,au}.demonware.net` /
		// `ops4-stun.{us,eu,jp,au}.demonware.net`. Enumerating every expansion is a losing game and
		// an incomplete list here fails OPEN — a name we forgot resolves for real and the client
		// dials Activision, which is the one outcome this file exists to make impossible.
		//
		// So the rule is the domain, not the host: anything under demonware.net is ours. The
		// Activision entries alongside it are there for the same fail-closed reason.
		constexpr const char* kDemonwareDomains[] = {
			"demonware.net",
			"activision.com",
			"callofduty.com",
		};

		std::string Lower(const char* s) {
			std::string out;
			if (!s) return out;
			for (const char* p = s; *p && out.size() < 256; ++p) {
				out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
			}
			return out;
		}

		struct Sighting {
			std::string what;
			std::string verdict;
			std::uint64_t count{ 1 };
		};

		std::mutex g_JournalLock;
		std::vector<Sighting> g_Sightings;

		// WHY THERE IS A FILE AT ALL, and not just the in-memory table.
		//
		// The boot this instrument exists to measure is an ONLINE boot, and online boots on this build
		// end in an ERR_DROP or take the process down outright. An in-memory table read through the
		// overlay only survives if the game does, so the one run that finally reaches an interesting
		// endpoint is exactly the run most likely to lose its own findings. Lines are therefore
		// appended and flushed as they happen, not written at exit.
		std::filesystem::path JournalPath() {
			std::error_code ec;
			const std::filesystem::path dir = std::filesystem::current_path(ec) / "cw-mod";
			std::filesystem::create_directories(dir, ec);
			return dir / "dw_journal.txt";
		}

		// Wall clock, to the millisecond, in the same shape the game log uses — the whole point of a
		// timestamp here is lining a resolve up against "the moment I clicked the tile" in the other
		// log, and a relative counter cannot be cross-referenced that way.
		std::string Stamp() {
			using namespace std::chrono;
			const auto now = system_clock::now();
			const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
			const std::time_t t = system_clock::to_time_t(now);
			std::tm tm{};
			localtime_s(&tm, &t);
			return std::format("{:02}:{:02}:{:02}.{:03}",
				tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
		}

		// Must be called with g_JournalLock held: the append and the table update have to stay in the
		// same order in the file as they are in the table, or the artifact contradicts the overlay.
		void JournalAppend(const std::string& line) {
			std::ofstream f(JournalPath(), std::ios::app);
			if (!f) return;
			f << line << "\n";
		}

		// THE HOSTNAME CORRELATION, and why it is a heuristic.
		//
		// Once the redirect is on, getaddrinfo answers every Demonware name with 127.0.0.1, so by the
		// time connect() sees the address the name is gone and every endpoint looks identical:
		// "connect 127.0.0.1:443". That erases the one thing this journal is being run to learn —
		// WHICH endpoint speaks on which port, i.e. whether the playlist data rides plain HTTPS or the
		// LSG socket.
		//
		// Both TLS stacks here (libcurl/Schannel and WinHTTP) resolve and then connect on the same
		// thread, so the last Demonware name resolved on THIS thread is the name being dialled. That
		// is a heuristic, not a guarantee — a pooled connection reused across threads would attribute
		// wrongly — so the correlation is printed as "for <host>" and the raw address is kept next to
		// it. If the two ever disagree, believe the address.
		thread_local std::string t_LastDwHost;

		// Deduped: a login retry loop would otherwise produce thousands of identical lines and bury
		// the one endpoint that appeared once. The count is kept so "tried 900 times" is still
		// visible, which is itself diagnostic.
		void Note(std::string what, const char* verdict) {
			std::lock_guard<std::mutex> lock(g_JournalLock);
			for (auto& s : g_Sightings) {
				if (s.what == what && s.verdict == verdict) {
					++s.count;
					return;
				}
			}
			if (g_Sightings.size() >= 256) return;   // bounded; this runs on game threads
			g_Sightings.push_back({ std::move(what), verdict, 1 });
			const auto& added = g_Sightings.back();
			LOG("DwNet", INFO, "{} — {}", added.what, added.verdict);
			JournalAppend(std::format("{}  {:<52} {}", Stamp(), added.what, added.verdict));
		}
	}

	bool IsDemonwareHost(const char* host) {
		const std::string h = Lower(host);
		if (h.empty()) return false;
		for (const char* domain : kDemonwareDomains) {
			if (h.find(domain) != std::string::npos) return true;
		}
		return false;
	}

	void NoteResolve(const char* host, const char* verdict) {
		// Recorded BEFORE the journal check on purpose. The correlation is what makes the connect
		// lines legible, and it must not depend on whether journalling happened to be on at the
		// moment the name was resolved — a switch flipped mid-session would otherwise produce a run
		// of anonymous 127.0.0.1 connects with no way to recover what they were.
		if (host) t_LastDwHost = host;

		if (!g_Journal.load(std::memory_order_relaxed)) return;
		Note(std::format("resolve  {}", host ? host : "<null>"), verdict);
	}

	void NoteConnect(std::uint32_t addrHostOrder, std::uint16_t portHostOrder, const char* verdict) {
		if (!g_Journal.load(std::memory_order_relaxed)) return;

		// The port is the payload of this line. Post-redirect every Demonware endpoint shares the
		// same address, so 443 (HTTPS: auth3 / objectstore / umbrella / uno) versus 3074 (the LSG
		// bdSecureSocket) is what says whether an endpoint can be served over plain HTTPS or needs
		// the LSG handshake built first. 3074 is hardcoded client-side -- Lsg_ConnectTask_Ctor's
		// default, which the auth reply's lsg_endpoint cannot move (it supplies the host only).
		std::string what = std::format("connect  {}.{}.{}.{}:{}",
			(addrHostOrder >> 24) & 0xFF, (addrHostOrder >> 16) & 0xFF,
			(addrHostOrder >> 8) & 0xFF, addrHostOrder & 0xFF, portHostOrder);
		if (!t_LastDwHost.empty()) what += std::format("  for {}", t_LastDwHost);

		Note(std::move(what), verdict);
	}

	void Init() {
		// Same opt-in convention as the rest of the online work, so a normal install is untouched and
		// switching modes costs a relaunch rather than a build.
		//
		//   cw-mod/dwserver/ present + "backend" on  -> redirect Demonware to loopback (the local server)
		//   cw-mod.json "mode" not "offline"         -> journal everything (an online boot is being observed)
		//
		// Blocking is tied to neither: it is on whenever we are NOT redirecting but ARE instrumenting
		// an online boot, which is exactly the window in which the client would otherwise have a live
		// route to retail.
		std::error_code ec;
		const std::filesystem::path root = std::filesystem::current_path(ec) / "cw-mod";
		const bool haveServer = Settings::Get().backend && std::filesystem::exists(root / "dwserver", ec);
		const bool online = Settings::Get().mode != Settings::Mode::Offline;

		g_Redirect.store(haveServer);
		g_Journal.store(haveServer || online);
		g_Block.store(online && !haveServer);

		LOG("DwNet", INFO, "Winsock choke point: redirect={} block={} journal={} "
			"(backend {}, mode {}).",
			g_Redirect.load(), g_Block.load(), g_Journal.load(),
			haveServer ? "present" : "absent", Settings::ModeName(Settings::Get().mode));

		if (!g_Journal.load()) return;

		// Truncates: one file per boot. Appending across boots would merge an online run with the
		// offline run that followed it, and the whole value of this artifact is being able to say
		// "THIS boot, with these switches, reached these endpoints".
		std::ofstream f(JournalPath(), std::ios::trunc);
		if (!f) return;
		f << "# cw-mod Demonware endpoint journal - build " << g_GameIdentifier.m_Version << "\n"
			<< "# Every name resolved and every address dialled through the winsock choke point,\n"
			<< "# deduped, in order of first sighting, appended live so a boot that dies still\n"
			<< "# leaves its record. Repeat counts are NOT here - press 'Save journal' in the\n"
			<< "# overlay's Demonware tab to append a counted summary block.\n"
			<< "#\n"
			<< "# The 'for <host>' on a connect line is the last Demonware name resolved on the same\n"
			<< "# thread. Post-redirect every endpoint dials 127.0.0.1, so that attribution is how a\n"
			<< "# port gets tied back to an endpoint. It is a heuristic - trust the address over it.\n"
			<< "#\n"
			<< "# switches: redirect=" << (g_Redirect.load() ? "on" : "off")
			<< " block=" << (g_Block.load() ? "on" : "off")
			<< "   (backend " << (haveServer ? "present" : "absent")
			<< ", mode " << Settings::ModeName(Settings::Get().mode) << ")\n"
			<< "# time          what                                                 verdict\n";
	}

	void SaveJournal() {
		std::lock_guard<std::mutex> lock(g_JournalLock);
		std::ofstream f(JournalPath(), std::ios::app);
		if (!f) return;
		f << "#\n# --- summary at " << Stamp() << " --- " << g_Sightings.size()
			<< " distinct sighting(s), redirect=" << (g_Redirect.load() ? "on" : "off")
			<< " block=" << (g_Block.load() ? "on" : "off") << "\n";
		if (g_Sightings.empty()) {
			f << "# nothing was resolved or dialled through the choke point on this boot.\n";
			return;
		}
		for (const auto& s : g_Sightings) {
			f << std::format("#   {:<52} {}  (x{})\n", s.what, s.verdict, s.count);
		}
	}

	std::string Report() {
		std::lock_guard<std::mutex> lock(g_JournalLock);
		if (g_Sightings.empty()) {
			return g_Journal.load()
				? "No Demonware name resolution or outbound connect has happened yet."
				: "Journalling is off (no backend, and cw-mod.json mode is \"offline\").";
		}
		std::string out = std::format("redirect={} block={}  —  {} distinct endpoints\n\n",
			g_Redirect.load(), g_Block.load(), g_Sightings.size());
		for (const auto& s : g_Sightings) {
			out += std::format("  {:<48} {}", s.what, s.verdict);
			if (s.count > 1) out += std::format("  (x{})", s.count);
			out.push_back('\n');
		}
		return out;
	}
}
