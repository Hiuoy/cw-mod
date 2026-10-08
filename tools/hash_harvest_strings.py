#!/usr/bin/env python3
"""Harvest every printable string out of a binary, hash it the T9 way, and match
against a list of unknown name hashes."""
import re, sys, collections

FNV64_BASIS = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
M64 = 0xFFFFFFFFFFFFFFFF
M63 = 0x7FFFFFFFFFFFFFFF

def h63(name: bytes) -> int:
    h = FNV64_BASIS
    for ch in name.lower():
        h = ((h ^ ch) * FNV64_PRIME) & M64
    return h & M63

def load_hashes(path):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^(0x[0-9A-Fa-f]{1,16})\s*(.*)$", line)
        if m:
            out[int(m.group(1), 16) & M63] = m.group(2).strip()
    return out

binpath, hashpaths = sys.argv[1], sys.argv[2:]
targets = {}
for p in hashpaths:
    for k, v in load_hashes(p).items():
        targets.setdefault(k, v)
print(f"targets: {len(targets)} hashes")

data = open(binpath, "rb").read()
print(f"binary: {len(data)} bytes")

# ASCII runs of >=3 chars from the identifier/path alphabet
strings = set()
for m in re.finditer(rb"[A-Za-z0-9_./\\|+\- ]{3,128}", data):
    s = m.group(0)
    strings.add(s)
    # names are often embedded in longer runs; also split on separators
    for part in re.split(rb"[ /\\|]+", s):
        if len(part) >= 3:
            strings.add(part)
print(f"unique candidate strings: {len(strings)}")

hits = {}
for s in strings:
    hh = h63(s)
    if hh in targets:
        hits.setdefault(hh, set()).add(s.decode("ascii", "replace"))

known = sum(1 for v in targets.values() if v and v != "?")
print(f"already-named in list: {known}")
print(f"MATCHED: {len(hits)} / {len(targets)}")
for hh, names in sorted(hits.items()):
    prev = targets[hh] or "-"
    print(f"0x{hh:016X}  {sorted(names)}   (was: {prev})")
