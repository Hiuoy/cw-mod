#!/usr/bin/env python3
"""Names for the models mkasset reads: the level builder's model picker list
(mapkit/godot/addons/mapkit/game_model_names.txt).

The zones store a model's hash, not its name. This looks each hash of an
`mkasset catalog` up in ACTS's hash index (`acts -t lookup`, acts on PATH) and
keeps a name only when it hashes back to the model (FNV-1a 64 over the
lowercased name, top bit cleared). The output is names and hashes only.

Usage:
    mkasset catalog catalog.json
    python mapkit/mkasset/model_names.py catalog.json mapkit/godot/addons/mapkit/game_model_names.txt
"""

import argparse
import json
import subprocess
import sys

FNV64_BASIS = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF
# Hashes per acts call: the command line has a length limit.
BATCH = 400


def hash_name(name):
    value = FNV64_BASIS
    for byte in name.lower().encode():
        value = ((value ^ byte) * FNV64_PRIME) & MASK64
    return value & 0x7FFFFFFFFFFFFFFF


def lookup(keys):
    """{key: name} for the keys ACTS knows. acts prints <hex>=<name>, or <hex>=can't be find."""
    names = {}
    for start in range(0, len(keys), BATCH):
        batch = keys[start:start + BATCH]
        out = subprocess.run(["acts", "-t", "lookup", *batch], capture_output=True, text=True).stdout
        for line in out.splitlines():
            key, sep, name = line.strip().partition("=")
            if not sep or name.startswith("can't") or " " in name:
                continue
            try:
                value = int(key, 16)
            except ValueError:
                continue
            if hash_name(name) == value:
                names[value] = name
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("catalog", help="mkasset catalog output (JSON)")
    parser.add_argument("out", help="the names list to write")
    args = parser.parse_args()

    catalog = json.load(open(args.catalog, encoding="utf-8"))
    keys = sorted({model["model"] for model in catalog["models"]})
    names = lookup(keys)
    lines = [
        "# The names of the game's models, for the model picker: <hash> <name>, one per line.",
        "# Names and hashes only (no game data). Written by mapkit/mkasset/model_names.py from an mkasset",
        "# catalog of %s and ACTS's hash index; a model without a line here shows as its hash." % ", ".join(catalog["zones"]),
    ]
    lines += ["%016X %s" % (key, name) for key, name in sorted(names.items(), key=lambda item: item[1])]
    with open(args.out, "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(lines) + "\n")
    print("%d of %d models named -> %s" % (len(names), len(keys), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
