#!/usr/bin/env python3
"""
IDAPython recon for the LUI menu-registry work.

Run inside IDA (File > Script file...) against the fixed-up BOCW dump.
Writes a report next to the idb and echoes it to the Output window.

Gathers, in one pass, what two separate jobs need:

  Phase C  - the "UI Error <n>" popup path, so we can hook it and read the
             real Lua error string instead of a bare number.
  Phase A  - the Lua VM natives (lua_next / lua_gettable / lua_tolstring and
             friends), so the client can walk the Lua state at runtime and
             dump the menu registry the way DumpAllDvars walks the dvar table.

Nothing here modifies the database.
"""

import re
import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_ida
import ida_kernwin
import ida_nalt
import ida_segment
import idautils

# --------------------------------------------------------------------------
# report plumbing
# --------------------------------------------------------------------------

_lines = []


def out(s=""):
    _lines.append(s)
    ida_kernwin.msg(s + "\n")


def header(s):
    out()
    out("=" * 78)
    out(s)
    out("=" * 78)


# --------------------------------------------------------------------------
# string search
#
# Arxan-mangled control flow makes IDA's own string list patchy in places, so
# every literal we actually care about is also confirmed by a raw scan of the
# read-only segments. The two results are merged.
# --------------------------------------------------------------------------

def segment_blobs(writable_too=False):
    """Yield (segname, start_ea, bytes) for segments worth scanning."""
    for n in range(ida_segment.get_segm_qty()):
        seg = ida_segment.getnseg(n)
        if not seg:
            continue
        name = ida_segment.get_segm_name(seg)
        if not writable_too and name not in (".rdata", ".text", ".data"):
            continue
        size = seg.end_ea - seg.start_ea
        if size <= 0 or size > 0x8000000:
            continue
        data = ida_bytes.get_bytes(seg.start_ea, size)
        if data:
            yield name, seg.start_ea, data


_ASCII = re.compile(rb"[\x20-\x7e]{4,}")


def raw_strings():
    """[(ea, text)] from a direct byte scan - independent of IDA's string list."""
    found = []
    for _name, base, data in segment_blobs():
        for m in _ASCII.finditer(data):
            found.append((base + m.start(), m.group().decode("ascii", "replace")))
    return found


def ida_strings():
    found = []
    try:
        for s in idautils.Strings():
            try:
                found.append((s.ea, str(s)))
            except Exception:
                pass
    except Exception as e:
        out(f"[warn] idautils.Strings() unavailable: {e}")
    return found


def xrefs_to(ea, limit=12):
    """Functions referencing ea, as (xref_ea, containing_func_ea, func_name)."""
    hits = []
    for xr in idautils.XrefsTo(ea):
        f = ida_funcs.get_func(xr.frm)
        hits.append((xr.frm,
                     f.start_ea if f else None,
                     ida_funcs.get_func_name(xr.frm) if f else "<no func>"))
        if len(hits) >= limit:
            break
    return hits


def report_matches(title, matches, show_xrefs=True, limit=40):
    out()
    out(f"--- {title}  ({len(matches)} hit(s))")
    if not matches:
        out("    none")
        return
    for ea, text in matches[:limit]:
        shown = text if len(text) <= 90 else text[:87] + "..."
        out(f"  {ea:#018x}  {shown!r}")
        if show_xrefs:
            xs = xrefs_to(ea)
            if not xs:
                out("        (no xrefs - likely reached via a computed/obfuscated load)")
            for xref_ea, func_ea, fname in xs:
                loc = f"{func_ea:#x}" if func_ea else "-"
                out(f"        xref {xref_ea:#018x}  in {loc}  {fname}")
    if len(matches) > limit:
        out(f"    ... {len(matches) - limit} more suppressed")


# --------------------------------------------------------------------------
# byte-pattern search (for the signatures the client already relies on)
# --------------------------------------------------------------------------

def parse_pattern(sig):
    """'48 89 ? 24' -> (bytes, bytearray mask)"""
    pat, mask = bytearray(), bytearray()
    for tok in sig.split():
        if tok == "?" or tok == "??":
            pat.append(0)
            mask.append(0)
        else:
            pat.append(int(tok, 16))
            mask.append(1)
    return bytes(pat), mask


def find_pattern(sig, limit=4):
    """All addresses matching a masked byte signature, across .text."""
    pat, mask = parse_pattern(sig)
    n = len(pat)
    hits = []
    for name, base, data in segment_blobs():
        if name != ".text":
            continue
        # anchor the scan on the first concrete byte to keep it quick
        first = next((i for i, m in enumerate(mask) if m), 0)
        anchor = pat[first]
        start = 0
        while True:
            i = data.find(bytes([anchor]), start)
            if i < 0:
                break
            s = i - first
            start = i + 1
            if s < 0 or s + n > len(data):
                continue
            if all(not mask[k] or data[s + k] == pat[k] for k in range(n)):
                hits.append(base + s)
                if len(hits) >= limit:
                    return hits
    return hits


def report_pattern(label, sig):
    hits = find_pattern(sig)
    out()
    out(f"--- {label}")
    out(f"    sig: {sig}")
    if not hits:
        out("    NO MATCH - signature has drifted or the segment is packed")
        return
    for ea in hits:
        f = ida_funcs.get_func(ea)
        base = ida_ida.inf_get_min_ea()
        rva = ea - ida_nalt.get_imagebase()
        out(f"    {ea:#018x}  rva {rva:#x}  func {ida_funcs.get_func_name(ea) or '<none>'}"
            f"{'' if f and f.start_ea == ea else '   [!] not a function start'}")


# --------------------------------------------------------------------------
# the actual recon
# --------------------------------------------------------------------------

def main():
    ida_kernwin.msg("\n\n")
    out("LUI menu-registry recon")
    out(f"imagebase {ida_nalt.get_imagebase():#x}   "
        f"min_ea {ida_ida.inf_get_min_ea():#x}   max_ea {ida_ida.inf_get_max_ea():#x}")

    out("waiting on autoanalysis...")
    ida_auto.auto_wait()

    out("collecting strings (IDA list + raw scan)...")
    pool = {}
    for ea, text in ida_strings():
        pool[ea] = text
    for ea, text in raw_strings():
        pool.setdefault(ea, text)
    allstr = sorted(pool.items())
    out(f"{len(allstr)} unique strings")

    def grep(pattern, flags=re.I):
        rx = re.compile(pattern, flags)
        return [(ea, t) for ea, t in allstr if rx.search(t)]

    # ---- Phase C: the error popup -------------------------------------
    header("PHASE C - the 'UI Error' popup path")
    out("Goal: find the formatter so we can hook it and log the Lua error text.")
    report_matches("literal 'UI Error'", grep(r"\bUI\s*Error"))
    report_matches("error format strings with a number slot",
                   grep(r"(error|Error).{0,20}%(d|i|u|llu|lu)"), limit=25)
    report_matches("LUI error reporting", grep(r"lui.{0,12}error|error.{0,12}lui"), limit=25)

    # ---- Phase A: Lua VM anchors --------------------------------------
    header("PHASE A - Lua VM natives")
    out("Anchors below are stock Lua/LuaJIT literals. The functions that")
    out("reference them ARE the VM; from there we name lua_next / lua_gettable /")
    out("lua_tolstring / lua_type by their use sites.")

    # luaT_typenames - a contiguous array of these is the strongest single anchor
    report_matches("type-name table ('nil'/'boolean'/'userdata'/...)",
                   [(ea, t) for ea, t in allstr
                    if t in ("nil", "boolean", "userdata", "number",
                             "string", "table", "function", "thread",
                             "proto", "upval", "cdata")],
                   show_xrefs=False, limit=60)

    report_matches("metamethod names ('__index' etc.)",
                   [(ea, t) for ea, t in allstr if t.startswith("__")],
                   show_xrefs=False, limit=60)

    report_matches("VM panic/error literals",
                   grep(r"PANIC|unprotected error|not enough memory|stack overflow|"
                        r"attempt to index|attempt to call|table index is"), limit=30)

    report_matches("Lua API / aux-lib literals",
                   grep(r"^(_G|_LOADED|_PRELOAD|luaL_|lua_|too many results)"), limit=30)

    # ---- LUI surface ---------------------------------------------------
    header("LUI surface strings")
    out("If a menu registry has ANY native-side presence it shows up here.")
    report_matches("event + root names", grep(r"^(addmenu|removemenu|UIRoot|UIRootFull)"), limit=25)
    report_matches("menu/MenuBuilder/registerType",
                   grep(r"MenuBuilder|registerType|menuData|OpenMenu|CloseMenu"), limit=30)
    report_matches("LUI.* dotted identifiers", grep(r"^LUI\.[A-Za-z]"), limit=40)

    # ---- known-good signatures from the client -------------------------
    header("Client signatures - confirm they still resolve in this db")
    report_pattern("LUI_BeginEvent",
                   "48 89 5C 24 ? 48 89 74 24 ? 48 89 4C 24 ? 57 48 83 EC 20 49 8B F9 49 8B F0")
    report_pattern("LUI_EndEvent",
                   "40 53 48 83 EC 20 48 8B D9 80 79 ? ? 75 ? FF 51 ? FF 05")
    report_pattern("LUI_SetEventHash",
                   "48 89 5C 24 ? 57 48 83 EC 20 48 B8 FF FF FF FF FF FF FF 7F 48 8B D9 48 23 D0")
    report_pattern("LUI_GetRootName",
                   "80 3D ? ? ? ? ? 75 ? 48 63 C1 48 69 C8 B0 00 00 00")
    report_pattern("g_luiCtx load site",
                   "4C 8B C0 48 8B 15 ? ? ? ? 48 8D 4C 24 ? E8")

    # ---- write it out --------------------------------------------------
    idb = ida_nalt.get_input_file_path() or "lui_recon"
    dest = os.path.join(os.path.dirname(idb) or ".", "lui_recon.txt")
    try:
        with open(dest, "w", encoding="utf-8") as fh:
            fh.write("\n".join(_lines))
        ida_kernwin.msg(f"\n[+] report written to {dest}\n")
    except Exception as e:
        ida_kernwin.msg(f"\n[!] could not write report: {e}\n")


main()
