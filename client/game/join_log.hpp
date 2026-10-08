#pragma once
// The join/netmsg transcript, shared by the netmsg detours (netmsg.cpp), the join FSM watcher and
// the descriptor join (join.cpp). One append-only buffer, mirrored to client_join_args.txt, read
// back by the menu through Pointers::ClientJoinCaptureLog.
//
// Everything the client believes about a join lands here, because no other client-side signal is
// trustworthy: the popups and the "message sent" returns report local state, not what crossed the
// wire. See docs/phase3_test_plan.md.
#include <string>

namespace Client::Game {
	// Wall clock, not elapsed: a PC1 and a PC2 transcript are only interleavable on absolute time.
	std::string WallClockNow();

	// Join FSM state names (g_clientJoinCtx+0), from the StartJoin decompile.
	const char* JoinStateLabel(int s);

	// Append one line to the transcript, the log file and the mod console. Thread-safe.
	void JcAppend(const std::string& line);
}
