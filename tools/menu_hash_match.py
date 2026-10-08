#!/usr/bin/env python3
"""Resolve LUI menu-name hashes out of a cw-mod lua_tree.txt dump.

LUI.createMenu is the menu registry: its values are the builder functions and
its KEYS are the menu names.  Those keys are not Lua strings - they are the
engine's tag -6 interned-identifier objects, so the walker prints the head of
each object as raw qwords ("+0: <w0> <w1> <w2> <w3>") instead of text.

If that type stores a T9 name hash, one of those qwords is the FNV-1a-64 of the
menu name, lowercased and masked to 63 bits - the same hash the dvar registry
uses.  This hashes a candidate wordlist that way and reports, per qword slot,
how many keys it resolves.  The slot that lights up IS the hash field; the rest
are the vtable/next/refcount around it.

Usage:
    python tools/menu_hash_match.py <lua_tree.txt> <wordlist.txt>
    python tools/menu_hash_match.py <lua_tree.txt> <wordlist.txt> --slot 1 -o menus.txt
"""

import argparse
import re
import sys
from collections import defaultdict

FNV64_BASIS = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF
MASK63 = 0x7FFFFFFFFFFFFFFF

# "tag -6  0x1234  [lt 5] +0: AAAA BBBB CCCC DDDD"
OBJ = re.compile(r"tag\s+(-?\d+)\s+0x([0-9A-Fa-f]+)\s+\[lt\s+(-?\d+)\]\s+\+0:((?:\s+[0-9A-Fa-f]{16})+)")


def name_hash(name):
    """T9 name hash: FNV-1a-64 over the lowercased name, masked to 63 bits."""
    h = FNV64_BASIS
    for ch in name.lower().encode("utf-8"):
        h = ((h ^ ch) * FNV64_PRIME) & MASK64
    return h & MASK63


def load_wordlist(path):
    names, seen = [], set()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip().rstrip(",").strip()
            if not line or line.startswith("#") or line.startswith("//"):
                continue
            if len(line) >= 2 and line[0] == line[-1] and line[0] in "\"'":
                line = line[1:-1]
            if line and line not in seen:
                seen.add(line)
                names.append(line)
    return names


def collect(path, tag_filter):
    """[(tag, addr, [qwords])] for every printed object in the dump."""
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            for m in OBJ.finditer(line):
                tag = int(m.group(1))
                if tag_filter is not None and tag != tag_filter:
                    continue
                words = [int(w, 16) for w in m.group(4).split()]
                out.append((tag, int(m.group(2), 16), words))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="lua_tree.txt produced by the in-game walker")
    ap.add_argument("wordlist", help="candidate menu names, one per line")
    ap.add_argument("--tag", type=int, default=-6, help="object tag to decode (default -6)")
    ap.add_argument("--slot", type=int, help="report resolved names using this qword slot")
    ap.add_argument("-o", "--output", help="write the resolved name list here")
    args = ap.parse_args(argv)

    table = defaultdict(list)
    for n in load_wordlist(args.wordlist):
        table[name_hash(n)].append(n)

    objs = collect(args.dump, args.tag)
    if not objs:
        sys.stderr.write("no tag %d objects with a +0: preview found in %s\n"
                         "(rebuild the mod and re-dump - the preview is new)\n"
                         % (args.tag, args.dump))
        return 1

    width = max(len(w) for _, _, w in objs)
    sys.stderr.write("%d tag %d objects, %d wordlist names\n"
                     % (len(objs), args.tag, sum(len(v) for v in table.values())))

    # Which qword slot behaves like the name hash?
    for slot in range(width):
        hits = distinct = 0
        seen = set()
        for _, _, words in objs:
            if slot >= len(words):
                continue
            h = words[slot] & MASK63
            if h in table:
                hits += 1
                if h not in seen:
                    seen.add(h)
                    distinct += 1
        sys.stderr.write("  slot %d (+%d): %d/%d resolve (%d distinct)\n"
                         % (slot, slot * 8, hits, len(objs), distinct))

    if args.slot is None:
        sys.stderr.write("\npick the winning slot with --slot N to emit names\n")
        return 0

    lines, unresolved = [], 0
    for _, addr, words in objs:
        if args.slot >= len(words):
            continue
        h = words[args.slot] & MASK63
        names = table.get(h)
        if names:
            lines.append("%016X  %s" % (h, "|".join(names)))
        else:
            unresolved += 1
            lines.append("%016X  ?" % h)

    text = "\n".join(sorted(set(lines))) + "\n"
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    sys.stderr.write("%d unresolved\n" % unresolved)
    return 0


if __name__ == "__main__":
    sys.exit(main())
