#pragma once
// The Demonware network choke point.
//
// Everything the client says to Demonware leaves through WS2_32, and T9 imports the whole relevant
// surface statically (verified against the image's import table: getaddrinfo, gethostbyname, connect,
// send/recv/sendto/recvfrom, select, closesocket, ioctlsocket, getpeername, getsockname — and no
// wsock32 at all). So its IAT is a single place where every outbound DW connection can be answered
// locally, which is the same architecture boiii and shield/project-bo4 settled on. See memory
// [[cw-mod-prior-art-cod-projects]].
//
// TWO REASONS this replaces the hosts-file redirect, and one thing it deliberately does NOT replace:
//
//  1. Setup. A hosts entry needs an elevated edit of a system-wide file that affects every process on
//     the machine and outlives the game. This is process-scoped and disappears when the game exits.
//
//  2. Enforcement. The project's rule is that retail Activision servers are not a target
//     ([[cw-mod-legal-guardrails]]). A hosts entry is a promise the user can forget to keep; with
//     this installed, a DW hostname CANNOT resolve to a routable address, because the resolver never
//     asks the network. That is a structural guarantee rather than a configuration one, and it is why
//     this should land before `nodw` is ever cleared on a boot that can reach a network.
//
//  3. What it does NOT replace: the local CA. The handoff proposed following shield and disabling
//     curl's certificate verification instead of trusting a local root. Measured against this image,
//     that does not work here — T9 carries TWO independent TLS clients:
//       * libcurl with the **Schannel** backend (144 "schannel" strings; CRYPT32's
//         CertGetCertificateChain / CertVerifyCertificateChainPolicy / CertCreateCertificateChainEngine
//         are imported, which is curl's schannel.c doing the verifying), and
//       * **WinHTTP**, imported and called from four separate functions, with its own TLS and its own
//         validation that no curl option touches.
//     Both delegate to the OS certificate chain engine, so one CA in the Windows Trusted Root store
//     satisfies both, while shield's curl patch would satisfy neither cleanly. Keep the CA; drop the
//     hosts file.
//
// Install-always-inert, like every other hook here: the detours are registered unconditionally and
// consult these atomics, so an ordinary offline launch pays a predictable-branch and nothing else.

#include <atomic>
#include <cstdint>
#include <string>

namespace Client::Game::DwNet {
	// Answer Demonware hostnames from the loopback interface instead of the network. Off unless the
	// local backend is opted into, so a default launch resolves exactly as it always did.
	extern std::atomic<bool> g_Redirect;

	// Refuse Demonware hostnames outright when we are NOT redirecting them. This is the guardrail
	// half and it is independent of the redirect: with it on, a build that somehow reaches the login
	// path without a local server fails to resolve rather than dialling Activision.
	extern std::atomic<bool> g_Block;

	// Log every resolve and connect the game makes, redirected or not. The point is not the redirect
	// — it is discovering WHICH endpoints the online frontend actually wants, which is the shopping
	// list for the local server. Independent of the two switches above so a completely unmodified
	// online boot can be observed.
	extern std::atomic<bool> g_Journal;

	// Turn the switches on from the opt-in markers. Called once from the Pointers ctor.
	void Init();

	// Is `host` a Demonware/Activision endpoint we are responsible for? Case-insensitive suffix and
	// substring matching over the hostnames the T9 image builds; see dw_net.cpp for the list and for
	// why it is matched loosely.
	bool IsDemonwareHost(const char* host);

	// What the hooks observed, for the overlay's Demonware tab. Ordered by first sighting.
	std::string Report();

	// Append the current table, WITH the repeat counts, to cw-mod/dw_journal.txt.
	//
	// The live journal cannot carry counts — a line is written the first time a sighting is seen and
	// the 900 repeats that follow are folded into it afterwards — so this is what turns the file from
	// "these endpoints were touched" into "this one was retried 900 times and that one once". Called
	// from the overlay button, and safe to call repeatedly; each call appends a fresh block.
	void SaveJournal();

	// Called by the detours. Split out so the hook files stay thin and the policy stays in one place.
	void NoteResolve(const char* host, const char* verdict);
	void NoteConnect(std::uint32_t addrHostOrder, std::uint16_t portHostOrder, const char* verdict);
}
