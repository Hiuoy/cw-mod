#!/usr/bin/env python3
"""Read back the last boot: what the client did, and what the local backend saw.

    python tools/bootlog.py              last session of <game>/cw-mod/client.log, filtered + summary
    python tools/bootlog.py --back 1     the session before that
    python tools/bootlog.py --all        no filter (still only that one session)
    python tools/bootlog.py --full       keep whole crash dumps and multi-line entries

Kept: the boot profile, login status transcript, Demonware redirect/connect, err_drops, crashes,
Battle.net suppressions, sign-out gate, and any WARN/ERROR. Hook installs, scanner hits and per-map
playerdata reads are dropped. The summary ends with the matching tools/dwserver/material/server.log
session, if run.py was used, so "the client says HTTP 0" and "the server saw nothing" sit side by side.
"""
from __future__ import annotations

import argparse
import datetime
import json
import pathlib
import re
import sys
from collections import Counter

REPO = pathlib.Path(__file__).resolve().parent.parent
DIRECTORIES_JSON = REPO / "tools" / "game" / "directories.json"
SERVER_LOG = REPO / "tools" / "dwserver" / "material" / "server.log"

SESSION_START = "===== session start"
ENTRY = re.compile(r"^\[(?P<ts>[^\]]+)\] \[(?P<level>[A-Z]+)\] \((?P<cat>[^)]*)\) (?P<msg>.*)$")
LOGIN_STATUS = re.compile(r"\[status code=(\d+)\] (.*)")

KEEP_CATEGORIES = {
    "Boot", "Settings", "Login", "DwNet", "DwBackend", "BB_Alert", "Crash", "Bnet", "LiveUser", "FirstParty",
    "AuthPatcher", "OnlineMode", "LuiError",
}
KEEP_MESSAGES = re.compile(r"boot marker|MainEntryPoint reached|session start", re.I)


def find_client_log(game: str | None) -> pathlib.Path:
    if game:
        return pathlib.Path(game) / "cw-mod" / "client.log"
    try:
        for path in json.loads(DIRECTORIES_JSON.read_text()).values():
            if path and (pathlib.Path(path) / "cw-mod" / "client.log").exists():
                return pathlib.Path(path) / "cw-mod" / "client.log"
    except (OSError, ValueError):
        pass
    sys.exit(f"no client.log found via {DIRECTORIES_JSON}; pass --game")


def split_sessions(lines: list[str]) -> list[list[str]]:
    sessions: list[list[str]] = [[]]
    for line in lines:
        if line.startswith(SESSION_START):
            sessions.append([])
        sessions[-1].append(line)
    return [s for s in sessions if s]


def group_entries(lines: list[str]) -> list[tuple[re.Match | None, list[str]]]:
    """One log call can span several lines (crash dumps, callers); continuation lines have no [ts]."""
    entries: list[tuple[re.Match | None, list[str]]] = []
    for line in lines:
        m = ENTRY.match(line)
        if m or not entries:
            entries.append((m, [line]))
        else:
            entries[-1][1].append(line)
    return entries


def keep(m: re.Match | None, first_line: str) -> bool:
    if m is None:
        return first_line.startswith(SESSION_START)
    return (m["cat"] in KEEP_CATEGORIES or m["level"] in ("WARN", "ERROR")
            or bool(KEEP_MESSAGES.search(m["msg"])))


def parse_client_ts(ts: str) -> datetime.datetime | None:
    try:
        return datetime.datetime.strptime(ts, "%Y-%m-%d %H:%M:%S")  # client writes 2026-9-16 7:05:01
    except ValueError:
        return None


def server_session_summary(boot_start: datetime.datetime | None,
                           boot_end: datetime.datetime | None) -> list[str]:
    """The run.py session that was up during this boot: the last one started before the boot's last
    line. None started means nothing was listening, which the client reports as HTTP code [0]."""
    if not SERVER_LOG.exists():
        return ["server.log: none (servers were never started with tools/dwserver/run.py)"]
    lines = SERVER_LOG.read_text(encoding="utf-8", errors="replace").splitlines()
    starts = []
    for i, line in enumerate(lines):
        if m := re.search(r"===== server session start (\S+ \S+)", line):
            starts.append((i, datetime.datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S")))
    candidates = [(i, t) for i, t in starts if boot_end is None or t <= boot_end]
    if not candidates:
        return ["server.log: NO run.py session was started before this boot ended, so nothing was "
                "listening (expect `HTTP code [0]`)"]
    begin, started = candidates[-1]
    later = [i for i, _ in starts if i > begin]
    session = lines[begin:later[0] if later else len(lines)]
    header = f"server session start {started:%Y-%m-%d %H:%M:%S}"
    ended = any("server session end" in l for l in session)
    if ended and boot_start and started.date() == boot_start.date():
        # The end line carries only a time of day; same-day sessions are all that happen in practice.
        end_line = next(l for l in session if "server session end" in l)
        end_t = datetime.datetime.combine(started.date(), datetime.time.fromisoformat(end_line.split()[0]))
        if end_t < boot_start:
            return [f"server.log: the last session ({header}) ENDED at {end_t:%H:%M:%S}, before this boot "
                    "began, so nothing was listening (expect `HTTP code [0]`)"]
    auth_req = sum(1 for l in session if "[authd] POST /auth" in l and "Host=" in l)
    auth_ok = sum(1 for l in session if "[authd] -> code 700" in l)
    lobby = sum(1 for l in session if re.search(r"request #\d+", l))
    unhandled = [l for l in session if "UNHANDLED" in l or "UNRECORDED ENDPOINT" in l]
    errors = [l for l in session if any(k in l for k in ("Traceback", "HANDLER RAISED", "EXITED", "NOT READY"))]
    out = [f"server.log: {header}{'' if ended else ' (still running or killed)'}",
           f"  auth: {auth_req} request(s), {auth_ok} answered 700   lobby requests: {lobby}   "
           f"unhandled: {len(unhandled)}"]
    out += [f"  {l}" for l in unhandled[:10]]
    out += [f"  !! {l}" for l in errors[:10]]
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", help="game folder (default: tools/game/directories.json)")
    ap.add_argument("--back", type=int, default=0, help="0 = last session, 1 = the one before, ...")
    ap.add_argument("--all", action="store_true", help="do not filter lines")
    ap.add_argument("--full", action="store_true", help="print multi-line entries in full")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    path = find_client_log(args.game)
    sessions = split_sessions(path.read_text(encoding="utf-8", errors="replace").splitlines())
    if args.back >= len(sessions):
        sys.exit(f"only {len(sessions)} session(s) in {path}")
    session = sessions[-1 - args.back]

    statuses: list[tuple[int, str]] = []
    counts: Counter[str] = Counter()
    hosts: Counter[str] = Counter()
    profile = None
    first_ts = last_ts = None

    print(f"# {path}  (session {len(sessions) - args.back} of {len(sessions)})")
    for m, block in group_entries(session):
        if m:
            msg, cat = m["msg"], m["cat"]
            if t := parse_client_ts(m["ts"]):
                first_ts = first_ts or t
                last_ts = t
            if (s := LOGIN_STATUS.search(msg)) and cat == "Login":
                statuses.append((int(s[1]), s[2]))
            if cat == "BB_Alert" and "err_drop" in msg:
                counts["err_drop"] += 1
            if cat == "Crash":
                counts["known benign fault" if "known benign" in msg else "crash dump"] += 1
            if cat == "Bnet" and "Suppressed" in msg:
                counts["Battle.net error suppressed"] += 1
            if cat == "LiveUser" and "sign-out" in msg:
                counts["sign-out drop gated"] += 1
            if cat == "DwNet" and (h := re.search(r"resolve\s+(\S+)", msg)):
                hosts[h[1]] += 1
            if cat == "Boot" and msg.startswith("profile:"):
                profile = msg.split(":", 1)[1].strip()
        if not (args.all or keep(m, block[0])):
            continue
        print("\n".join(block if args.full or len(block) <= 3 else block[:3] + [f"    ... ({len(block) - 3} more lines, --full)"]))

    print("\n# summary")
    print(f"boot profile: {profile or 'not logged (DLL older than boot_profile.cpp?)'}")
    if statuses:
        best = max(statuses)
        print(f"login: {len(statuses)} status lines, highest [{best[0]}] {best[1]}; last [{statuses[-1][0]}] {statuses[-1][1]}")
        failures = Counter(text for code, text in statuses if "fail" in text.lower())
        for text, n in failures.most_common(3):
            print(f"  {n}x {text}")
        print("  LOGIN COMPLETE" if any(code == 27 for code, _ in statuses) else "  login never completed")
    else:
        print("login: no status lines (login driver never started: nodw still true, or backend off)")
    for key, n in sorted(counts.items()):
        print(f"{key}: {n}")
    if hosts:
        print("resolved: " + ", ".join(f"{h} x{n}" for h, n in hosts.most_common()))
    for line in server_session_summary(first_ts, last_ts):
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
