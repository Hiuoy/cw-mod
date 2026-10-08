#!/usr/bin/env python3
"""Recover unresolved T9 dvar names by generating candidates from a vocabulary.

Dvar names are not random - they are underscore-joined words drawn from a small
engine vocabulary (r_, ai_, scr_, cg_, sv_, zm_, ...).  This builds that
vocabulary out of the names already recovered, then walks every combination up
to --depth segments and checks each against the unresolved hashes.

FNV-1a is a streaming hash, so a candidate's hash is built incrementally: the
frontier of partial hashes at depth N is extended one segment at a time, and
every intermediate value is itself a candidate.  The byte loop is vectorised
over the whole frontier with numpy, so the cost is (frontier x vocab x bytes)
element ops rather than a Python loop per candidate.

Usage:
    # depth 2 over a vocabulary mined from the names dvar_hash_match resolved
    python tools/dvar_hash_brute.py dvars.txt --seed-names tools/wordlists/dvars_named.txt

    # deeper, but restrict the first segment to known dvar prefixes
    python tools/dvar_hash_brute.py dvars.txt --seed-names tools/wordlists/dvars_named.txt \
        --depth 3 --root-vocab 200 --vocab 1500
"""

import argparse
import re
import sys

import numpy as np

FNV64_BASIS = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
MASK63 = 0x7FFFFFFFFFFFFFFF

U64 = np.uint64
PRIME_U64 = U64(FNV64_PRIME)
MASK63_U64 = U64(MASK63)

HASH_LINE = re.compile(r"^\s*(0x[0-9A-Fa-f]{1,16})\s+(\S+)")
WORD = re.compile(r"[A-Za-z][A-Za-z0-9]*")


def load_targets(path):
    """Unresolved hashes from an annotated dump (name column == '?')."""
    targets = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.lstrip().startswith("#"):
                continue
            m = HASH_LINE.match(line)
            if m and m.group(2) == "?":
                targets.append(int(m.group(1), 16) & MASK63)
    return sorted(set(targets))


def load_resolved(path):
    """Names already recovered, used to mine the vocabulary."""
    names = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.lstrip().startswith("#"):
                continue
            m = HASH_LINE.match(line)
            if m and m.group(2) != "?":
                names.append(m.group(2))
    return names


def build_vocab(names, extra_paths):
    """Rank underscore/camel segments by how often they appear in real names."""
    freq = {}
    prefix_freq = {}

    def feed(name):
        parts = [p for p in name.split("_") if p]
        for i, p in enumerate(parts):
            freq[p] = freq.get(p, 0) + 1
            # split camelCase too: viewmodelMoveAnimScale -> viewmodel move anim scale
            for w in re.findall(r"[a-z0-9]+|[A-Z][a-z0-9]*", p):
                if len(w) > 1:
                    freq[w.lower()] = freq.get(w.lower(), 0) + 1
            if i == 0 and len(parts) > 1:
                prefix_freq[p] = prefix_freq.get(p, 0) + 1

    for n in names:
        feed(n)
    for p in extra_paths or []:
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip().rstrip(",").strip().strip("\"'")
                if line and not line.startswith("#"):
                    feed(line)

    vocab = sorted(freq, key=lambda w: (-freq[w], w))
    roots = sorted(prefix_freq, key=lambda w: (-prefix_freq[w], w))
    return vocab, roots


def hash_prefixes(segments):
    """Vector of FNV-1a states after hashing each segment from the basis."""
    out = np.empty(len(segments), dtype=U64)
    for i, s in enumerate(segments):
        h = FNV64_BASIS
        for ch in s.encode():
            h = ((h ^ ch) * FNV64_PRIME) & 0xFFFFFFFFFFFFFFFF
        out[i] = h
    return out


def extend(states, suffix_bytes):
    """Continue every FNV state in `states` with the same byte string."""
    h = states
    for b in suffix_bytes:
        h = (h ^ U64(b)) * PRIME_U64
    return h


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hashes", help="annotated dump from dvar_hash_match.py")
    ap.add_argument("--seed-names", help="file to mine vocabulary from (default: the dump itself)")
    ap.add_argument("--extra-vocab", action="append",
                    help="additional name list to mine (repeatable)")
    ap.add_argument("--depth", type=int, default=2,
                    help="max underscore-joined segments per candidate (default 2)")
    ap.add_argument("--vocab", type=int, default=3000,
                    help="how many vocabulary segments to use (default 3000)")
    ap.add_argument("--root-vocab", type=int, default=0,
                    help="restrict the FIRST segment to the N most common name prefixes; "
                         "0 uses the full vocabulary (needed to keep depth>=3 tractable)")
    ap.add_argument("-o", "--output", help="write hits here (default: stdout)")
    args = ap.parse_args(argv)

    targets = load_targets(args.hashes)
    if not targets:
        sys.stderr.write("no unresolved hashes in %s\n" % args.hashes)
        return 1
    target_arr = np.array(targets, dtype=U64)
    remaining = set(targets)

    resolved = load_resolved(args.seed_names or args.hashes)
    vocab, roots = build_vocab(resolved, args.extra_vocab)
    vocab = vocab[:args.vocab]
    roots = roots[:args.root_vocab] if args.root_vocab else vocab
    sys.stderr.write("%d unresolved, vocab=%d roots=%d depth=%d\n"
                     % (len(targets), len(vocab), len(roots), args.depth))

    suffixes = [("_" + w).encode() for w in vocab]
    hits = {}

    def record(states, names):
        found = np.isin(states & MASK63_U64, target_arr)
        for i in np.nonzero(found)[0]:
            h = int(states[i]) & MASK63
            if h in remaining:
                hits.setdefault(h, names[int(i)])

    names = list(roots)
    states = hash_prefixes(names)
    record(states, names)

    for level in range(2, args.depth + 1):
        new_states = np.empty(len(states) * len(vocab), dtype=U64)
        new_names = []
        for j, suf in enumerate(suffixes):
            new_states[j * len(states):(j + 1) * len(states)] = extend(states, suf)
        # names are only materialised lazily, on a hit, to keep memory sane
        base_names, base_n = names, len(states)

        found = np.isin(new_states & MASK63_U64, target_arr)
        for idx in np.nonzero(found)[0]:
            idx = int(idx)
            h = int(new_states[idx]) & MASK63
            if h in remaining:
                hits.setdefault(h, base_names[idx % base_n] + "_" + vocab[idx // base_n])

        sys.stderr.write("depth %d: %d candidates, %d hits so far\n"
                         % (level, new_states.size, len(hits)))
        if level < args.depth:
            states = new_states
            new_names = [b + "_" + w for w in vocab for b in base_names]
            names = new_names

    lines = ["0x%016X  %s" % (h, n) for h, n in sorted(hits.items(), key=lambda kv: kv[1])]
    text = "\n".join(lines) + ("\n" if lines else "")
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    sys.stderr.write("recovered %d/%d unresolved names\n" % (len(hits), len(targets)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
