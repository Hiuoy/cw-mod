#!/usr/bin/env python3
"""Resolve T9 dvar name hashes against a candidate name wordlist.

The in-game dvar registry dump stores only FNV-1a-64 hashes of dvar names
(basis 0xCBF29CE484222325, prime 0x100000001B3, name lowercased, result masked
to 63 bits).  This hashes every name in a wordlist the same way and joins the
two, producing an annotated registry with real names.

Usage:
    python tools/dvar_hash_match.py <hashes.txt> <wordlist.txt> [-o out.txt]
    python tools/dvar_hash_match.py <hashes.txt> <wordlist.txt> --unmatched-names
"""

import argparse
import re
import sys

FNV64_BASIS = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF
MASK63 = 0x7FFFFFFFFFFFFFFF

# "0x1234ABCD...  type  flags  flagnames  address" - hash first, rest free-form.
HASH_LINE = re.compile(r"^\s*(0x[0-9A-Fa-f]{1,16})\s*(.*)$")


def dvar_hash(name):
    """T9 dvar name hash: FNV-1a-64 over the lowercased name, masked to 63 bits."""
    h = FNV64_BASIS
    for ch in name.lower().encode("utf-8"):
        h = ((h ^ ch) * FNV64_PRIME) & MASK64
    return h & MASK63


def load_wordlist(path):
    """Yield candidate names from a wordlist.

    Accepts bare names one per line as well as the quoted/comma style
    ("someDvar",) that dvar dumps are usually pasted in.
    """
    names = []
    seen = set()
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


def build_table(names):
    """hash -> list of names (collisions kept so they stay visible)."""
    table = {}
    for name in names:
        table.setdefault(dvar_hash(name), []).append(name)
    return table


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hashes", help="registry dump whose first column is the hash")
    ap.add_argument("wordlist", help="candidate dvar names, one per line")
    ap.add_argument("-o", "--output", help="write annotated dump here (default: stdout)")
    ap.add_argument("--only-matched", action="store_true",
                    help="drop registry lines whose hash stayed unresolved")
    ap.add_argument("--unmatched-names", action="store_true",
                    help="instead list wordlist names absent from the registry")
    args = ap.parse_args(argv)

    table = build_table(load_wordlist(args.wordlist))

    out_lines = []
    total = matched = 0
    hit_hashes = set()

    with open(args.hashes, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = HASH_LINE.match(line)
            if not m or line.lstrip().startswith("#"):
                if not args.unmatched_names:
                    out_lines.append(line)
                continue
            total += 1
            h = int(m.group(1), 16) & MASK63
            names = table.get(h)
            if names:
                matched += 1
                hit_hashes.add(h)
                name = names[0] if len(names) == 1 else "|".join(names)
            else:
                name = "?"
            if names or not args.only_matched:
                out_lines.append("%s  %-52s %s" % (m.group(1), name, m.group(2).rstrip()))

    if args.unmatched_names:
        out_lines = ["# wordlist names not present in %s" % args.hashes]
        out_lines += sorted(n for h, ns in table.items() if h not in hit_hashes for n in ns)

    text = "\n".join(out_lines) + "\n"
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)

    pct = (100.0 * matched / total) if total else 0.0
    sys.stderr.write("resolved %d/%d hashes (%.1f%%)\n" % (matched, total, pct))
    return 0


if __name__ == "__main__":
    sys.exit(main())
