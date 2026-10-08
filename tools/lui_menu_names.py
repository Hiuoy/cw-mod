#!/usr/bin/env python3
"""Name the LUI menus in a decompiled T9 Lua dump, for the overlay's 'LUI menus' list.

Every `LUI.createMenu[0x...] = function` in the dump is a menu the game can open. The key is
the menu name's hash; the decompiler prints it masked to 60 bits (the live registry holds the
63-bit FNV-1a-64 of the lowercased name - see Pointers::HashString). The name itself is not in
the dump, so this hashes candidates and keeps the ones that land:

  1. every string literal, identifier and file name in the dump;
  2. our wordlists (tools/wordlists/*.txt);
  3. those split into CamelCase tokens and recombined two at a time, with and without a mode
     prefix (ZM/MP/CP/WZ/...) - this is how 'ZMUpgrades' is found, which appears nowhere as text.

Output (default <game>/cw-mod/lui_menus.txt) is one menu per line:
    0x<60-bit hash>  <name or ->  <dump file>
The overlay matches it against the live registry on the low 60 bits, so an unnamed entry is
still listed and still openable by its full hash.

Usage:
    python tools/lui_menu_names.py <dump dir> [-o lui_menus.txt] [--no-combos]
"""

import argparse
import os
import re
import sys

FNV_BASIS = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
M64 = 0xFFFFFFFFFFFFFFFF
M60 = 0x0FFFFFFFFFFFFFFF

CREATE = re.compile(r"LUI\.createMenu\[0x([0-9A-Fa-f]+)\]\s*=\s*function")
STRING = re.compile(r'"([A-Za-z_][A-Za-z0-9_]{1,63})"')
IDENT = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]{2,63})\b")
CAMEL = re.compile(r"[A-Z]+(?=[A-Z][a-z0-9])|[A-Z]?[a-z0-9]+|[A-Z]+")

PREFIXES = ["", "ZM", "MP", "CP", "WZ", "Start", "Director", "Frontend", "Lobby", "InGame",
            "Popup", "Overlay", "Common", "Zombies", "Zombie"]


def fnv(text, h=FNV_BASIS):
    for ch in text.lower().encode("utf-8", "replace"):
        h = ((h ^ ch) * FNV_PRIME) & M64
    return h


def scan_dump(root):
    menus = {}          # 60-bit hash -> dump file (relative)
    words = set()
    for dirpath, _, files in os.walk(root):
        for fn in files:
            if not fn.endswith(".lua"):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, root)
            stem = fn[:-len(".dec.lua")] if fn.endswith(".dec.lua") else fn[:-4]
            if not stem.startswith("Luafile_"):
                words.add(stem)
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
            for m in CREATE.finditer(text):
                menus.setdefault(int(m.group(1), 16) & M60, rel)
            words.update(STRING.findall(text))
            words.update(w for w in IDENT.findall(text) if not w.startswith(("f", "0x")) or len(w) > 12)
    return menus, words


def load_wordlists(folder):
    words = set()
    if not os.path.isdir(folder):
        return words
    for fn in os.listdir(folder):
        if not fn.endswith(".txt"):
            continue
        with open(os.path.join(folder, fn), encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                for tok in re.split(r"[\s|,]+", line):
                    tok = tok.strip("\"'")
                    if tok and not tok.startswith("0x") and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{1,63}", tok):
                        words.add(tok)
    return words


def combos(tokens, menus, names):
    """prefix + a + b for every token pair, vectorised over `a` (numpy uint64 wraps like FNV)."""
    import numpy as np
    prime = np.uint64(FNV_PRIME)
    mask = np.uint64(M60)
    want = np.array(sorted(menus), dtype=np.uint64)
    lowered = [t.lower().encode() for t in tokens]
    for p in PREFIXES:
        hp = fnv(p)
        base = np.array([fnv(a, hp) for a in tokens], dtype=np.uint64)
        with np.errstate(over="ignore"):
            for bi, b in enumerate(lowered):
                h = base.copy()
                for ch in b:
                    h = (h ^ np.uint64(ch)) * prime
                hits = np.nonzero(np.isin(h & mask, want))[0]
                for ai in hits:
                    key = int(h[ai] & mask)
                    if key not in names:
                        names[key] = p + tokens[ai] + tokens[bi]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("-o", "--out", default=None)
    ap.add_argument("--no-combos", action="store_true")
    args = ap.parse_args()

    menus, words = scan_dump(args.dump)
    here = os.path.dirname(os.path.abspath(__file__))
    words |= load_wordlists(os.path.join(here, "wordlists"))
    if not menus:
        sys.exit(f"no LUI.createMenu entries under {args.dump}")

    names = {}

    def try_name(name):
        h = fnv(name) & M60
        if h in menus and h not in names:
            names[h] = name

    for w in words:
        try_name(w)
        for p in PREFIXES[1:]:
            try_name(p + w)
    direct = len(names)

    if not args.no_combos:
        tokens = set()
        for w in words:
            for t in CAMEL.findall(w):
                if 2 <= len(t) <= 16:
                    # The hash ignores case, so this is cosmetic: 'BATTLENET' -> 'Battlenet', and
                    # one spelling per token also halves the combo space.
                    tokens.add(t.capitalize() if t.isupper() and len(t) > 2 else t[0].upper() + t[1:])
        tokens = sorted(tokens)
        print(f"{len(menus)} menus, {direct} named directly; trying {len(PREFIXES)}x{len(tokens)}^2 combos...",
              file=sys.stderr)
        combos(tokens, menus, names)

    out = args.out or "lui_menus.txt"
    with open(out, "w", encoding="utf-8") as f:
        f.write("# LUI menus found in the decompiled Lua dump (LUI.createMenu), build 1.34.0.15931218.\n")
        f.write("# hash = low 60 bits of the 63-bit FNV-1a name hash; the overlay matches the live\n")
        f.write("# registry on these bits. name '-' = not recovered (still openable by hash).\n")
        f.write(f"# {len(menus)} menus, {len(names)} named. Written by tools/lui_menu_names.py\n")
        for h in sorted(menus, key=lambda k: (names.get(k, "~").lower(), k)):
            f.write(f"0x{h:015X}  {names.get(h, '-')}  {menus[h]}\n")
    print(f"{len(menus)} menus, {len(names)} named ({direct} directly) -> {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
