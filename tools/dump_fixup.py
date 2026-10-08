#!/usr/bin/env python3
"""Convert a runtime memory-image dump of BlackOpsColdWar.exe into a loadable PE.

BOCW.exe is Arxan-encrypted on disk, so static analysis of the shipped exe is
impossible (every function reads as zeros/ciphertext in the file). The in-game
"Dump decrypted module" button (Client::Scripting::DumpDecryptedModule) snapshots
the *decrypted* module straight from the running process to bocw_dump.bin.

That snapshot is a flat memory image: each section's bytes sit at its RVA offset
(page-aligned via SectionAlignment), not at a file-alignment offset. IDA can't
load that as a normal PE. This script rewrites the section headers so that, for
every section, PointerToRawData == VirtualAddress and SizeOfRawData covers
VirtualSize, and sets FileAlignment == SectionAlignment. Because the file offset
then equals the RVA, the bytes are already in the right place — no data moves.

Result: a PE that loads at the module's preferred base (0x140000000) with RVAs
preserved, so an RVA logged by the DLL (e.g. +0x1BF6EB0) is the same address in
IDA (0x141BF6EB0).

usage:  python tools/dump_fixup.py <bocw_dump.bin> [out.exe]
"""
import os
import struct
import sys


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    inp = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(inp)[0] + "_fixed.exe"

    with open(inp, "rb") as f:
        data = bytearray(f.read())

    if data[0:2] != b"MZ":
        print("error: not a PE image (missing MZ header)")
        return 1

    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        print("error: no PE signature at e_lfanew=0x%X" % e_lfanew)
        return 1

    coff = e_lfanew + 4
    machine, num_sections = struct.unpack_from("<HH", data, coff)
    size_opt = struct.unpack_from("<H", data, coff + 16)[0]  # SizeOfOptionalHeader
    opt = coff + 20

    magic = struct.unpack_from("<H", data, opt)[0]
    is_pe32plus = magic == 0x20B
    # These fields live at the same offset for PE32 and PE32+.
    section_alignment = struct.unpack_from("<I", data, opt + 32)[0]
    file_alignment = struct.unpack_from("<I", data, opt + 36)[0]
    size_of_image = struct.unpack_from("<I", data, opt + 56)[0]

    print("machine=0x%X  sections=%d  pe32+=%s" % (machine, num_sections, is_pe32plus))
    print("secAlign=0x%X  fileAlign=0x%X  sizeOfImage=0x%X  dumpSize=0x%X"
          % (section_alignment, file_alignment, size_of_image, len(data)))
    if len(data) < size_of_image:
        print("warning: dump (0x%X) is smaller than SizeOfImage (0x%X); tail sections may be truncated"
              % (len(data), size_of_image))

    # Make page-aligned RVAs valid file offsets: FileAlignment := SectionAlignment.
    struct.pack_into("<I", data, opt + 36, section_alignment)

    sec_table = opt + size_opt
    for i in range(num_sections):
        sh = sec_table + i * 40
        name = bytes(data[sh:sh + 8]).rstrip(b"\0").decode("latin1", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, sh + 8)

        new_rawsize = align(max(vsize, rawsize), section_alignment)
        if vaddr + new_rawsize > len(data):
            new_rawsize = align(len(data) - vaddr, section_alignment) if len(data) > vaddr else 0

        # SizeOfRawData @ sh+16, PointerToRawData @ sh+20
        struct.pack_into("<II", data, sh + 16, new_rawsize, vaddr)
        print("  %-10s vaddr=0x%08X vsize=0x%08X  ->  rawptr=0x%08X rawsize=0x%08X"
              % (name, vaddr, vsize, vaddr, new_rawsize))

    with open(out, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (out, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
