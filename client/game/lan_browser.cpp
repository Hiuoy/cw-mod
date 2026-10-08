// LAN server browser: every host broadcasts its join descriptor, every client lists what it hears.
//
// Why this exists. The retail build has no LAN discovery left. g_netMsgHandlers maps msgId 22
// (ServerlistInfo) to a thunk into `mov al,1; ret`, nothing writes msgId 22, and the image carries no
// bdLANDiscovery. The two-PC join itself works (JoinHostByDescriptor); it only ever lacked a way to get
// the descriptor across without copy-paste. This is that way.
//
// Wire format: one UDP datagram per host per second, to 255.255.255.255 and to every IPv4 adapter's
// directed broadcast (Windows sends the limited broadcast out of one adapter only). Text, one
// key=value per line after the magic, so new fields never break an older reader:
//   CWLAN2
//   inst=<hex>        random per process: drops our own echo, and two instances on one PC still work
//   blob=CWJOIN1.<hex>
//   netmode= gamemode= members= max= match= map= gametype= build= mod=
//   player=<gamertag> (repeated)
// Pings are unicast to the host's port: "CWPING2\nt=<us>", answered with "CWPONG2\nt=<same>".
#include "common.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/dump_anchors.hpp"
#include <utility/nt.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <string_view>
#include <thread>

namespace Client::Game::LanBrowser {
	namespace {
		constexpr std::string_view kMagic     = "CWLAN2";
		constexpr std::string_view kPingMagic = "CWPING2";
		constexpr std::string_view kPongMagic = "CWPONG2";
		constexpr std::uint64_t kAdvertPeriodMs = 1000;
		constexpr std::uint64_t kPingPeriodMs   = 2000;
		constexpr std::uint64_t kExpireMs       = 5000;    // five missed beacons
		constexpr std::uint64_t kIfaceRefreshMs = 10000;   // adapters come and go (VPNs, cable pulls)
		constexpr std::size_t   kMaxDatagram    = 1400;    // stay inside one Ethernet frame

		std::mutex                    g_Mutex;
		std::string                   g_Payload;  // what to broadcast; empty = nothing live to offer
		Status                        g_Status;
		std::map<std::uint64_t, Host> g_Hosts;    // by host XUID: a re-host replaces the stale entry

		std::uint32_t    g_Instance = 0;
		std::atomic_bool g_ThreadStarted{ false };

		// --- Peer address (the descriptor's 84-byte serializedadr, a bdCommonAddr) ------------------
		// bdCommonAddr_Deserialize (0x7FF729D377F0): 5 x local (ip4 + port, network order), public
		// (same), u8 NAT type, u32, u8 relay flag. The engine's "is this peer me?" test
		// (bdCommonAddr_IsSameAddr 0x7FF729D37AC0) compares main addresses: the public one when it is
		// set, else the first local one. When a host's matches ours, a join goes to this PC instead.
		struct PeerAddr {
			std::string   text;
			std::uint64_t main{};   // ip << 16 | port; 0 = none
		};

		PeerAddr DecodePeerAddr(const std::uint8_t (&a)[84]) {
			auto at = [&a](int off) {
				const std::uint32_t ip = (std::uint32_t{ a[off] } << 24) | (a[off + 1] << 16) | (a[off + 2] << 8) | a[off + 3];
				const std::uint16_t port = static_cast<std::uint16_t>((a[off + 4] << 8) | a[off + 5]);
				return (std::uint64_t{ ip } << 16) | port;
			};
			auto show = [](std::uint64_t key) {
				return std::format("{}.{}.{}.{}:{}", (key >> 40) & 0xFF, (key >> 32) & 0xFF, (key >> 24) & 0xFF,
					(key >> 16) & 0xFF, key & 0xFFFF);
			};
			PeerAddr p;
			std::string locals;
			std::uint64_t firstLocal = 0;
			for (int i = 0; i < 5; ++i) {
				const std::uint64_t key = at(i * 6);
				if (!key) continue;
				locals += (locals.empty() ? "" : ", ") + show(key);
				if (!firstLocal) firstLocal = key;
			}
			const std::uint64_t pub = at(30);
			p.main = pub ? pub : firstLocal;
			p.text = std::format("local [{}], public {}, NAT {}, relay {}", locals, pub ? show(pub) : "none",
				a[36], a[41]);
			return p;
		}

		std::atomic<std::uint64_t> g_OwnMainAddr{ 0 };
		std::atomic<std::uint64_t> g_OwnXuid{ 0 };
		std::string g_OwnAddrText;   // under g_Mutex

		std::int64_t NowUs() {
			return std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// --- Payload -------------------------------------------------------------------------------

		void AppendField(std::string& s, std::string_view key, std::string_view value) {
			s += key;
			s += '=';
			for (const char c : value) {
				if (c != '\r' && c != '\n') s += c;   // a newline would end the field early
			}
			s += '\n';
		}

		std::string BuildPayload(const Advert& a) {
			std::string s(kMagic);
			s += '\n';
			AppendField(s, "inst", std::format("{:08X}", g_Instance));
			AppendField(s, "blob", a.blob);
			AppendField(s, "netmode", std::to_string(a.networkMode));
			AppendField(s, "gamemode", std::to_string(a.gameMode));
			AppendField(s, "members", std::to_string(a.members));
			AppendField(s, "max", std::to_string(a.maxClients));
			AppendField(s, "match", a.inMatch ? "1" : "0");
			AppendField(s, "map", a.map);
			AppendField(s, "gametype", a.gametype);
			AppendField(s, "build", a.build);
			AppendField(s, "mod", a.mod);
			for (const std::string& p : a.players) {
				if (s.size() + p.size() + 8 > kMaxDatagram) break;   // the list is display-only; truncate
				AppendField(s, "player", p);
			}
			return s;
		}

		template <typename T>
		bool ParseNum(std::string_view s, T& out, int base = 10) {
			const auto r = std::from_chars(s.data(), s.data() + s.size(), out, base);
			return r.ec == std::errc{} && r.ptr == s.data() + s.size();
		}

		// Calls fn(key, value) for every "key=value" line after the first. False if the first line is
		// not `magic`.
		template <typename Fn>
		bool ForEachField(std::string_view s, std::string_view magic, Fn&& fn) {
			std::size_t nl = s.find('\n');
			if (s.substr(0, nl) != magic) return false;
			while (nl != std::string_view::npos) {
				s.remove_prefix(nl + 1);
				nl = s.find('\n');
				const std::string_view line = s.substr(0, nl);
				const std::size_t eq = line.find('=');
				if (eq != std::string_view::npos) fn(line.substr(0, eq), line.substr(eq + 1));
			}
			return true;
		}

		// --- Engine reads (game thread) -------------------------------------------------------------

		// sv_running's plaintext value and not the frontend UI level: Com_IsGameServerRunning's rule,
		// read from memory rather than through its Arxan-flattened Dvar_GetBool calls.
		bool ReadInMatch() {
			static std::uintptr_t s_Base = 0, s_Size = 0;
			if (!s_Base) {
				const Common::Utility::NT::Library game;
				s_Base = reinterpret_cast<std::uintptr_t>(game.GetPtr());
				s_Size = game.GetOptionalHeader()->SizeOfImage;
			}
			auto at = [](std::uintptr_t dumpAbs) -> const void* {
				const std::uintptr_t a = s_Base + (dumpAbs - kDumpImagebase);
				return (a >= s_Base && a < s_Base + s_Size) ? reinterpret_cast<const void*>(a) : nullptr;
			};
			const void* pDvar = at(kDump_p_dvar_svRunning);
			const void* pUiLevel = at(kDump_g_uiLevelRunning);
			const std::uint8_t* dvar = nullptr;
			const std::uint8_t* values = nullptr;
			std::uint8_t running = 0, uiLevel = 0;
			if (!pDvar || !pUiLevel || !SafeRead(pDvar, dvar) || !dvar
				|| !SafeRead(dvar + Pointers::kDvar_Values, values) || !values || !SafeRead(values, running)) {
				return false;
			}
			SafeRead(pUiLevel, uiLevel);
			return running != 0 && uiLevel == 0;
		}

		bool ReadOwnAdvert(Advert& a) {
			if (!g_Pointers) return false;
			Pointers::JoinDescriptor d;
			const std::uint8_t* obj = nullptr;
			if (!g_Pointers->ReadAdvertisedSession(d, a.members, &obj)) return false;

			a.blob = Pointers::BuildJoinDescriptorBlob(d.xuid, d.slot, d.name, d.secId, d.secKey, d.adr);
			const PeerAddr own = DecodePeerAddr(d.adr);
			g_OwnMainAddr.store(own.main, std::memory_order_relaxed);
			g_OwnXuid.store(d.xuid, std::memory_order_relaxed);
			bool addrChanged = false;
			{
				std::lock_guard lock(g_Mutex);
				addrChanged = own.text != g_OwnAddrText;
				if (addrChanged) g_OwnAddrText = own.text;
			}
			if (addrChanged) {
				LOG("LanBrowser", INFO, "own peer address: {}", own.text);
			}
			a.xuid = d.xuid;
			a.name = d.name;
			a.slot = d.slot;
			const std::uint32_t packed = g_Pointers->m_g_sessionModePacked ? *g_Pointers->m_g_sessionModePacked : 0xFFFFFFFF;
			a.networkMode = static_cast<int>((packed >> 4) & 0xF);
			a.gameMode = static_cast<int>(packed & 0xF);
			a.inMatch = ReadInMatch();
			Pointers::LobbyDetails details = g_Pointers->ReadLobbyDetails(obj);
			a.map = std::move(details.map);
			a.gametype = std::move(details.gametype);
			a.maxClients = details.maxClients;
			a.players = std::move(details.players);
			a.build = g_GameIdentifier.m_Version;
			a.mod = GIT_DESCRIBE;
			return true;
		}

		// --- Network thread -------------------------------------------------------------------------

		std::vector<sockaddr_in> BroadcastTargets(SOCKET s) {
			std::vector<sockaddr_in> out;
			auto add = [&out](std::uint32_t hostOrder) {
				const std::uint32_t net = htonl(hostOrder);
				for (const auto& t : out) {
					if (t.sin_addr.s_addr == net) return;
				}
				sockaddr_in a{};
				a.sin_family = AF_INET;
				a.sin_port = htons(kPort);
				a.sin_addr.s_addr = net;
				out.push_back(a);
			};
			add(INADDR_BROADCAST);

			INTERFACE_INFO ifs[32]{};
			DWORD bytes = 0;
			if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, nullptr, 0, ifs, sizeof(ifs), &bytes, nullptr, nullptr) == 0) {
				for (DWORD i = 0; i < bytes / sizeof(INTERFACE_INFO); ++i) {
					const INTERFACE_INFO& f = ifs[i];
					if (!(f.iiFlags & IFF_UP) || (f.iiFlags & IFF_LOOPBACK) || !(f.iiFlags & IFF_BROADCAST)) {
						continue;
					}
					const std::uint32_t addr = ntohl(f.iiAddress.AddressIn.sin_addr.s_addr);
					const std::uint32_t mask = ntohl(f.iiNetmask.AddressIn.sin_addr.s_addr);
					if (addr != 0 && mask != 0) {
						add(addr | ~mask);
					}
				}
			}
			return out;
		}

		void Reject() {
			std::lock_guard lock(g_Mutex);
			++g_Status.rejected;
		}

		void OnBeacon(std::string_view data, const std::string& ip) {
			Host h;
			std::uint32_t instance = 0;
			bool haveInstance = false;
			const bool ok = ForEachField(data, kMagic, [&](std::string_view k, std::string_view v) {
				if      (k == "inst")     haveInstance = ParseNum(v, instance, 16);
				else if (k == "blob")     h.blob = v;
				else if (k == "netmode")  ParseNum(v, h.networkMode);
				else if (k == "gamemode") ParseNum(v, h.gameMode);
				else if (k == "members")  ParseNum(v, h.members);
				else if (k == "max")      ParseNum(v, h.maxClients);
				else if (k == "match")    h.inMatch = v == "1";
				else if (k == "map")      h.map = v;
				else if (k == "gametype") h.gametype = v;
				else if (k == "build")    h.build = v;
				else if (k == "mod")      h.mod = v;
				else if (k == "player")   h.players.emplace_back(v);
			});
			if (!ok || !haveInstance) {
				Reject();
				return;
			}
			if (instance == g_Instance) {
				return;   // our own broadcast, looped back
			}
			// The descriptor is the only field the join trusts, so it alone decides admission.
			Pointers::JoinDescriptor d;
			if (!Pointers::DecodeJoinDescriptorBlob(h.blob, d).empty()) {
				Reject();
				return;
			}
			h.xuid = d.xuid;
			h.name = d.name;
			h.slot = d.slot;
			h.address = ip;
			h.lastSeenMs = GetTickCount64();

			bool isNew = false, changed = false;
			{
				std::lock_guard lock(g_Mutex);
				++g_Status.received;
				const auto it = g_Hosts.find(d.xuid);
				isNew = it == g_Hosts.end();
				if (!isNew) {
					changed = it->second.blob != h.blob;
					if (it->second.address == h.address) h.pingMs = it->second.pingMs;
				}
				g_Hosts[d.xuid] = h;
			}
			if (isNew || changed) {
				const PeerAddr peer = DecodePeerAddr(d.adr);
				if (isNew) {
					LOG("LanBrowser", INFO, "heard host '{}' xuid=0x{:016X} at {}: map '{}' gametype '{}' {}/{} players, "
						"networkMode {}, {}, build {} mod {}", h.name, h.xuid, h.address, h.map, h.gametype, h.members,
						h.maxClients, h.networkMode, h.inMatch ? "in match" : "in lobby", h.build, h.mod);
				} else {
					LOG("LanBrowser", INFO, "host '{}' at {} re-advertised a new descriptor (re-host or slot change)",
						h.name, h.address);
				}
				LOG("LanBrowser", INFO, "  its peer address: {}", peer.text);
				if (h.xuid == g_OwnXuid.load(std::memory_order_relaxed)) {
					LOG("LanBrowser", WARN, "  its XUID 0x{:X} is OURS too: the two PCs share one identity, so the host "
						"cannot tell our join from itself. Each PC needs its own \"xuid\" in cw-mod.json.", h.xuid);
				}
				if (peer.main && peer.main == g_OwnMainAddr.load(std::memory_order_relaxed)) {
					LOG("LanBrowser", WARN, "  its main address equals OURS: the engine will take this host for this PC "
						"and a join will never reach it");
				}
			}
		}

		void OnPong(std::string_view data, const std::string& ip) {
			std::int64_t sent = 0;
			bool have = false;
			if (!ForEachField(data, kPongMagic, [&](std::string_view k, std::string_view v) {
					if (k == "t") have = ParseNum(v, sent);
				}) || !have) {
				return;
			}
			const float ms = static_cast<float>(NowUs() - sent) / 1000.f;
			std::lock_guard lock(g_Mutex);
			for (auto& [xuid, h] : g_Hosts) {
				if (h.address == ip) h.pingMs = ms;
			}
		}

		void OnDatagram(SOCKET s, const char* data, int len, const sockaddr_in& from) {
			const std::string_view msg(data, static_cast<std::size_t>(len));
			char ipBuf[INET_ADDRSTRLEN] = "?";
			inet_ntop(AF_INET, &from.sin_addr, ipBuf, sizeof(ipBuf));
			const std::string ip = ipBuf;

			if (msg.starts_with(kPingMagic)) {
				// Answer straight back to the sender, even when not advertising: it costs nothing and
				// lets a client tell "host went away" from "firewall eats our unicast".
				std::string pong(kPongMagic);
				pong += '\n';
				ForEachField(msg, kPingMagic, [&](std::string_view k, std::string_view v) {
					if (k == "t") AppendField(pong, "t", v);
				});
				sendto(s, pong.data(), static_cast<int>(pong.size()), 0,
					reinterpret_cast<const sockaddr*>(&from), sizeof(from));
			} else if (msg.starts_with(kPongMagic)) {
				OnPong(msg, ip);
			} else if (g_Listen.load(std::memory_order_relaxed)) {
				OnBeacon(msg, ip);
			}
		}

		void PingHosts(SOCKET s) {
			std::vector<std::string> addrs;
			{
				std::lock_guard lock(g_Mutex);
				for (const auto& [xuid, h] : g_Hosts) addrs.push_back(h.address);
			}
			std::string ping(kPingMagic);
			ping += '\n';
			AppendField(ping, "t", std::to_string(NowUs()));
			for (const std::string& a : addrs) {
				sockaddr_in to{};
				to.sin_family = AF_INET;
				to.sin_port = htons(kPort);
				if (inet_pton(AF_INET, a.c_str(), &to.sin_addr) == 1) {
					sendto(s, ping.data(), static_cast<int>(ping.size()), 0,
						reinterpret_cast<const sockaddr*>(&to), sizeof(to));
				}
			}
		}

		void Expire(std::uint64_t now) {
			std::vector<std::string> gone;
			{
				std::lock_guard lock(g_Mutex);
				for (auto it = g_Hosts.begin(); it != g_Hosts.end();) {
					if (now - it->second.lastSeenMs > kExpireMs) {
						gone.push_back(std::format("'{}' at {}", it->second.name, it->second.address));
						it = g_Hosts.erase(it);
					} else {
						++it;
					}
				}
			}
			for (const auto& g : gone) {
				LOG("LanBrowser", INFO, "host {} went quiet for {} ms, dropped from the list", g, kExpireMs);
			}
		}

		void SocketFailed(const char* what, int err) {
			LOG("LanBrowser", ERROR, "{} failed on udp/{} (WSA {}). No LAN browser this session.", what, kPort, err);
			std::lock_guard lock(g_Mutex);
			g_Status.socketUp = false;
			g_Status.socketError = std::format("{} failed, WSA error {}", what, err);
		}

		void NetworkThread() {
			WSADATA wsa{};
			WSAStartup(MAKEWORD(2, 2), &wsa);   // refcounted; the game started winsock long ago

			const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (s == INVALID_SOCKET) {
				SocketFailed("socket", WSAGetLastError());
				return;
			}
			const BOOL on = TRUE;
			setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
			// Lets a second game instance on the same PC bind the port too; broadcasts reach both.
			setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));

			sockaddr_in local{};
			local.sin_family = AF_INET;
			local.sin_port = htons(kPort);
			local.sin_addr.s_addr = htonl(INADDR_ANY);
			if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
				SocketFailed("bind", WSAGetLastError());
				closesocket(s);
				return;
			}

			std::vector<sockaddr_in> targets;
			std::uint64_t lastIface = 0, lastSend = 0, lastPing = 0;
			bool loggedFirstSend = false;
			{
				std::lock_guard lock(g_Mutex);
				g_Status.socketUp = true;
				g_Status.socketError.clear();
			}
			LOG("LanBrowser", INFO, "listening on udp/{} (instance {:08X})", kPort, g_Instance);

			while (g_Running) {
				fd_set rd;
				FD_ZERO(&rd);
				FD_SET(s, &rd);
				timeval tv{ 0, 100 * 1000 };   // short, so a ping's receive stamp is not held back
				if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
					char buf[2048];
					sockaddr_in from{};
					int fromLen = sizeof(from);
					const int n = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
					// n < 0 is normally WSAECONNRESET from a stray ICMP; UDP has nothing to reset, so skip it.
					if (n > 0) {
						OnDatagram(s, buf, n, from);
					}
				}

				const std::uint64_t now = GetTickCount64();
				if (now - lastIface >= kIfaceRefreshMs) {
					targets = BroadcastTargets(s);
					lastIface = now;
					std::lock_guard lock(g_Mutex);
					g_Status.broadcastTargets = static_cast<int>(targets.size());
				}
				if (now - lastSend >= kAdvertPeriodMs) {
					lastSend = now;
					std::string payload;
					{
						std::lock_guard lock(g_Mutex);
						payload = g_Payload;
					}
					if (!payload.empty() && g_Advertise.load(std::memory_order_relaxed)) {
						std::uint64_t ok = 0;
						for (const auto& t : targets) {
							if (sendto(s, payload.data(), static_cast<int>(payload.size()), 0,
									reinterpret_cast<const sockaddr*>(&t), sizeof(t)) != SOCKET_ERROR) {
								++ok;
							}
						}
						{
							std::lock_guard lock(g_Mutex);
							g_Status.sent += ok;
						}
						if (!loggedFirstSend) {
							loggedFirstSend = true;
							LOG("LanBrowser", INFO, "first beacon ({} bytes): {} of {} broadcast targets accepted it",
								payload.size(), ok, targets.size());
						}
					}
				}
				if (!g_Listen.load(std::memory_order_relaxed)) {
					std::lock_guard lock(g_Mutex);
					g_Hosts.clear();
				} else {
					if (now - lastPing >= kPingPeriodMs) {
						lastPing = now;
						PingHosts(s);
					}
					Expire(now);
				}
			}
			closesocket(s);
		}
	}

	const char* FriendlyMapName(const std::string& map) {
		// Only zm_silver is a string in the exe; the other three are the community's names for the
		// internal ones (the metals series). The raw name is always shown beside this, so a wrong
		// label stays visible.
		static constexpr std::pair<std::string_view, const char*> kMaps[] = {
			{ "zm_silver",   "Die Maschine" },
			{ "zm_gold",     "Firebase Z" },
			{ "zm_platinum", "Mauer der Toten" },
			{ "zm_tungsten", "Forsaken" },
		};
		for (const auto& [raw, friendly] : kMaps) {
			if (map == raw) return friendly;
		}
		return "";
	}

	const char* GameModeName(int gameMode) {
		// Com_SessionMode_GetString's table: 0 zm, 1 mp, 2 cp, 3 wz; anything else is "no mode".
		switch (gameMode) {
		case 0: return "Zombies";
		case 1: return "Multiplayer";
		case 2: return "Campaign";
		case 3: return "Warzone";
		default: return "-";
		}
	}

	void Tick() {
		if (!g_ThreadStarted.exchange(true)) {
			g_Instance = std::random_device{}();
			std::thread(NetworkThread).detach();
		}

		static std::uint64_t s_Last = 0;
		const std::uint64_t now = GetTickCount64();
		if (now - s_Last < kAdvertPeriodMs) {
			return;
		}
		s_Last = now;

		Advert a;
		const bool live = g_Advertise.load(std::memory_order_relaxed) && ReadOwnAdvert(a);
		std::string payload = live ? BuildPayload(a) : std::string();

		bool started = false, stopped = false;
		{
			std::lock_guard lock(g_Mutex);
			started = g_Payload.empty() && live;
			stopped = !g_Payload.empty() && !live;
			g_Payload = std::move(payload);
			g_Status.advertising = live;
			g_Status.advert = live ? a : Advert{};
		}
		if (started) {
			LOG("LanBrowser", INFO, "advertising '{}' (xuid=0x{:016X}, slot {}): map '{}' gametype '{}' {}/{} players, {}",
				a.name, a.xuid, a.slot, a.map, a.gametype, a.members, a.maxClients, a.inMatch ? "in match" : "in lobby");
		} else if (stopped) {
			LOG("LanBrowser", INFO, "stopped advertising: {}", g_Advertise.load() ? "no live session slot" : "turned off");
		}
	}

	std::vector<Host> Hosts() {
		std::vector<Host> out;
		{
			std::lock_guard lock(g_Mutex);
			out.reserve(g_Hosts.size());
			for (const auto& [xuid, h] : g_Hosts) {
				out.push_back(h);
			}
		}
		// By name, not by last-heard: every host is re-heard once a second, so recency would reshuffle
		// the rows under the cursor.
		std::sort(out.begin(), out.end(), [](const Host& a, const Host& b) {
			return a.name != b.name ? a.name < b.name : a.address < b.address;
		});
		return out;
	}

	Status GetStatus() {
		std::lock_guard lock(g_Mutex);
		return g_Status;
	}
}
