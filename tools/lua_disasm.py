#!/usr/bin/env python3
"""Disassemble T9 LUI bytecode (the .luac chunks the overlay's "Dump loaded Lua files" writes).

For the chunks CoDLuaDecompiler hangs on, and for reading a Lua error: the line in
"x64:<chunk>.lua:<line>" is a SOURCE line, which the bytecode's line table has and a decompiled
file does not.

The format is LuaJIT 2.1's bytecode dump with three changes (checked by parsing all 4753 chunks
of build 1.34.0.15931218 with every constant block ending where the debug block starts):
  - one opcode added at 40, KXHASH (load a hashed-name constant); every later opcode is +1;
  - GC constant type 4 is a hashed name (uleb hi32, uleb lo32), and strings start at type 6;
  - in a constant table, key/value type 5 is a hashed name and strings start at type 6.
There is no chunk name in the header. Hashes print in full (63 bits); the decompiler shows 60.

A chunk named x64:<h>.lua is the file <asset>.luac, asset = FNV-1a continued from h over ".lua".

Usage:
    python tools/lua_disasm.py <chunk> list              every function: lines, size, where it is stored
    python tools/lua_disasm.py <chunk> line <n>          the function that holds source line n
    python tools/lua_disasm.py <chunk> key <hash>...     functions stored under a hashed key (60-bit match)
    python tools/lua_disasm.py <chunk> proto <i>...      functions by index
<chunk> is a .luac path, or a chunk name (x64:34f7b9134b089568.lua, or just the hash) with
--dump <folder of .luac files>.
"""

import argparse
import os
import struct
import sys

M60 = 0x0FFFFFFFFFFFFFFF
M63 = 0x7FFFFFFFFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF
FNV_PRIME = 0x100000001B3

OPS = """ISLT ISGE ISLE ISGT ISEQV ISNEV ISEQS ISNES ISEQN ISNEN ISEQP ISNEP ISTC ISFC IST ISF ISTYPE ISNUM
MOV NOT UNM LEN ADDVN SUBVN MULVN DIVVN MODVN ADDNV SUBNV MULNV DIVNV MODNV ADDVV SUBVV MULVV DIVVV MODVV
POW CAT KSTR KXHASH KCDATA KSHORT KNUM KPRI KNIL UGET USETV USETS USETN USETP UCLO FNEW TNEW TDUP GGET GSET
TGETV TGETS TGETB TGETR TSETV TSETS TSETB TSETM TSETR CALLM CALL CALLMT CALLT ITERC ITERN VARG ISNEXT RETM
RET RET0 RET1 FORI JFORI FORL IFORL JFORL ITERL IITERL JITERL LOOP ILOOP JLOOP JMP FUNCF IFUNCF JFUNCF
FUNCV IFUNCV JFUNCV FUNCC FUNCCW""".split()

# Operand kinds per opcode, as (A, B, C or D). B empty = the instruction is A + 16-bit D.
_VVV = ('dst', 'var', 'var')
_VVN = ('dst', 'var', 'num')
MODES = {
    'ISLT': ('var', '', 'var'), 'ISGE': ('var', '', 'var'), 'ISLE': ('var', '', 'var'), 'ISGT': ('var', '', 'var'),
    'ISEQV': ('var', '', 'var'), 'ISNEV': ('var', '', 'var'), 'ISEQS': ('var', '', 'kgc'), 'ISNES': ('var', '', 'kgc'),
    'ISEQN': ('var', '', 'num'), 'ISNEN': ('var', '', 'num'), 'ISEQP': ('var', '', 'pri'), 'ISNEP': ('var', '', 'pri'),
    'ISTC': ('dst', '', 'var'), 'ISFC': ('dst', '', 'var'), 'IST': ('', '', 'var'), 'ISF': ('', '', 'var'),
    'ISTYPE': ('var', '', 'lit'), 'ISNUM': ('var', '', 'lit'),
    'MOV': ('dst', '', 'var'), 'NOT': ('dst', '', 'var'), 'UNM': ('dst', '', 'var'), 'LEN': ('dst', '', 'var'),
    'ADDVN': _VVN, 'SUBVN': _VVN, 'MULVN': _VVN, 'DIVVN': _VVN, 'MODVN': _VVN,
    'ADDNV': _VVN, 'SUBNV': _VVN, 'MULNV': _VVN, 'DIVNV': _VVN, 'MODNV': _VVN,
    'ADDVV': _VVV, 'SUBVV': _VVV, 'MULVV': _VVV, 'DIVVV': _VVV, 'MODVV': _VVV, 'POW': _VVV,
    'CAT': ('dst', 'var', 'var'),
    'KSTR': ('dst', '', 'kgc'), 'KXHASH': ('dst', '', 'kgc'), 'KCDATA': ('dst', '', 'kgc'),
    'KSHORT': ('dst', '', 'lits'), 'KNUM': ('dst', '', 'num'), 'KPRI': ('dst', '', 'pri'), 'KNIL': ('var', '', 'var'),
    'UGET': ('dst', '', 'uv'), 'USETV': ('uv', '', 'var'), 'USETS': ('uv', '', 'kgc'), 'USETN': ('uv', '', 'num'),
    'USETP': ('uv', '', 'pri'), 'UCLO': ('var', '', 'jump'), 'FNEW': ('dst', '', 'kgc'),
    'TNEW': ('dst', '', 'lit'), 'TDUP': ('dst', '', 'kgc'), 'GGET': ('dst', '', 'kgc'), 'GSET': ('var', '', 'kgc'),
    'TGETV': _VVV, 'TGETS': ('dst', 'var', 'kgc'), 'TGETB': ('dst', 'var', 'lit'), 'TGETR': _VVV,
    'TSETV': ('var', 'var', 'var'), 'TSETS': ('var', 'var', 'kgc'), 'TSETB': ('var', 'var', 'lit'),
    'TSETM': ('var', '', 'num'), 'TSETR': ('var', 'var', 'var'),
    'CALLM': ('var', 'lit', 'lit'), 'CALL': ('var', 'lit', 'lit'), 'CALLMT': ('var', '', 'lit'),
    'CALLT': ('var', '', 'lit'), 'ITERC': ('var', 'lit', 'lit'), 'ITERN': ('var', 'lit', 'lit'),
    'VARG': ('var', 'lit', 'lit'), 'ISNEXT': ('var', '', 'jump'),
    'RETM': ('var', '', 'lit'), 'RET': ('var', '', 'lit'), 'RET0': ('var', '', 'lit'), 'RET1': ('var', '', 'lit'),
    'FORI': ('var', '', 'jump'), 'JFORI': ('var', '', 'jump'), 'FORL': ('var', '', 'jump'),
    'IFORL': ('var', '', 'jump'), 'JFORL': ('var', '', 'lit'), 'ITERL': ('var', '', 'jump'),
    'IITERL': ('var', '', 'jump'), 'JITERL': ('var', '', 'lit'), 'LOOP': ('var', '', 'jump'),
    'ILOOP': ('var', '', 'jump'), 'JLOOP': ('var', '', 'lit'), 'JMP': ('var', '', 'jump'),
}


class XHash:
    """A hashed-name constant (63-bit FNV-1a of the lowercased name)."""

    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return "#%016X" % self.value


class Proto:
    """One function prototype."""


class Reader:
    def __init__(self, data, pos):
        self.data = data
        self.pos = pos

    def u8(self):
        v = self.data[self.pos]
        self.pos += 1
        return v

    def uleb(self):
        v = shift = 0
        while True:
            b = self.u8()
            v |= (b & 0x7F) << shift
            shift += 7
            if b < 0x80:
                return v

    def uleb33(self):
        """The 33-bit form number constants use: the low bit of the first byte is a flag."""
        b = self.u8()
        v = b >> 1
        if v >= 0x40:
            v &= 0x3F
            shift = -1
            while True:
                b = self.u8()
                shift += 7
                v |= (b & 0x7F) << shift
                if b < 0x80:
                    break
        return v

    def take(self, n):
        v = self.data[self.pos:self.pos + n]
        self.pos += n
        return v

    def xhash(self):
        hi = self.uleb()
        return XHash((hi << 32) | self.uleb())

    def double(self):
        lo = self.uleb()
        return struct.unpack('<d', struct.pack('<II', lo, self.uleb()))[0]


def _int32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def _table_item(r):
    tp = r.uleb()
    if tp >= 6:
        return r.take(tp - 6).decode('latin-1')
    if tp == 5:
        return r.xhash()
    if tp == 4:
        return r.double()
    if tp == 3:
        return _int32(r.uleb())
    return (None, False, True)[tp]


def _table(r):
    narray = r.uleb()
    nhash = r.uleb()
    array = [_table_item(r) for _ in range(narray)]
    pairs = []
    for _ in range(nhash):
        key = _table_item(r)
        pairs.append((key, _table_item(r)))
    return ('table', array, pairs)


def parse(data):
    """Every prototype of a chunk, children before parents; the last one is the chunk's top level."""
    if data[:3] != b'\x1bLJ':
        raise ValueError("not LuaJIT bytecode (no ESC 'LJ' header)")
    r = Reader(data, 3)
    r.u8()      # version, 0x82 on this build
    r.uleb()    # flags, 0x18
    protos = []
    pending = []    # prototypes no parent has claimed yet
    while r.pos < len(data):
        length = r.uleb()
        if not length:
            break
        end = r.pos + length
        pt = Proto()
        pt.index = len(protos)
        pt.flags, pt.numparams, pt.framesize, numuv = r.u8(), r.u8(), r.u8(), r.u8()
        numkgc, numkn, numbc = r.uleb(), r.uleb(), r.uleb()
        sizedbg = r.uleb()
        pt.firstline = r.uleb() if sizedbg else 0
        pt.numline = r.uleb() if sizedbg else 0
        pt.code = [struct.unpack_from('<BBBB', data, r.pos + 4 * i) for i in range(numbc)]    # op, A, C, B
        r.pos += 4 * numbc
        pt.upvalues = list(struct.unpack_from('<%dH' % numuv, data, r.pos))
        r.pos += 2 * numuv

        # Written from the highest index down. A child constant is the newest unclaimed prototype.
        pt.kgc = [None] * numkgc
        for index in range(numkgc - 1, -1, -1):
            tp = r.uleb()
            if tp >= 6:
                pt.kgc[index] = r.take(tp - 6).decode('latin-1')
            elif tp == 4:
                pt.kgc[index] = r.xhash()
            elif tp == 1:
                pt.kgc[index] = _table(r)
            elif tp == 0:
                pt.kgc[index] = pending.pop()
            elif tp in (2, 3):
                lo = r.uleb()
                pt.kgc[index] = ('int64' if tp == 2 else 'uint64', (r.uleb() << 32) | lo)
            else:
                raise ValueError("function %d: unknown constant type %d at 0x%X" % (pt.index, tp, r.pos))
        pt.knum = []
        for _ in range(numkn):
            is_double = data[r.pos] & 1
            lo = r.uleb33()
            pt.knum.append(struct.unpack('<d', struct.pack('<II', lo, r.uleb()))[0] if is_double else _int32(lo))

        if r.pos != end - sizedbg:
            raise ValueError("function %d: constants end at 0x%X, the debug block starts at 0x%X"
                             % (pt.index, r.pos, end - sizedbg))
        pt.lines = []
        if sizedbg:
            width = 1 if pt.numline < 256 else 2 if pt.numline < 65536 else 4
            fmt = '<%d%s' % (numbc, {1: 'B', 2: 'H', 4: 'I'}[width])
            pt.lines = [pt.firstline + n for n in struct.unpack_from(fmt, data, r.pos)]
        r.pos = end
        pt.stored = ""
        pt.key = None
        protos.append(pt)
        pending.append(pt)
    if len(pending) != 1:
        raise ValueError("%d functions left without a parent (expected the top level alone)" % len(pending))
    return protos


def show(value):
    if isinstance(value, Proto):
        return "function#%d" % value.index
    if isinstance(value, str):
        return '"%s"' % value
    if isinstance(value, tuple) and value[0] == 'table':
        text = "{" + ", ".join([show(v) for v in value[1]] + ["[%s]=%s" % (show(k), show(v)) for k, v in value[2]]) + "}"
        return text if len(text) <= 240 else text[:240] + " ...}"
    if isinstance(value, tuple):
        return "%s 0x%X" % value
    return repr(value)


def label_stores(protos):
    """Label each function with where its closure goes: the table and key of the store, the global, or the
    call it is an argument of. Registers are followed within a straight run of code only."""
    for pt in protos:
        reg = {}

        def text(slot):
            v = reg.get(slot)
            return "r%d" % slot if v is None else show(v) if isinstance(v, (Proto, XHash)) else v

        for pc, (op, a, c, b) in enumerate(pt.code):
            name = OPS[op] if op < len(OPS) else ""
            d = c | (b << 8)
            if name == 'GGET':
                reg[a] = pt.kgc[d]
            elif name == 'KXHASH':
                reg[a] = pt.kgc[d]
            elif name == 'KSTR':
                reg[a] = '"%s"' % pt.kgc[d]
            elif name == 'FNEW':
                reg[a] = pt.kgc[d]
            elif name == 'TGETV':
                reg[a] = "%s[%s]" % (text(b), text(c))
            elif name == 'TGETS':
                reg[a] = "%s.%s" % (text(b), pt.kgc[c])
            elif name == 'MOV':
                reg[a] = reg.get(d)
            elif name in ('TSETV', 'TSETS', 'GSET'):
                child = reg.get(a)
                if isinstance(child, Proto) and not child.stored:
                    if name == 'TSETV':
                        child.stored, child.key = "%s[%s]" % (text(b), text(c)), reg.get(c)
                    elif name == 'TSETS':
                        child.stored, child.key = "%s.%s" % (text(b), pt.kgc[c]), pt.kgc[c]
                    else:
                        child.stored, child.key = "_G.%s" % pt.kgc[d], pt.kgc[d]
            elif name in ('CALL', 'CALLM', 'CALLT', 'CALLMT'):
                for slot in [s for s in reg if s > a]:
                    child = reg[slot]
                    if isinstance(child, Proto) and not child.stored:
                        child.stored = "argument of %s(), function#%d pc %d" % (text(a), pt.index, pc)
                for slot in [s for s in reg if s >= a]:
                    del reg[slot]
            elif name not in ('JMP', 'RET', 'RET0', 'RET1', 'RETM'):
                reg.pop(a, None)


def operand(pt, kind, value, pc):
    if kind in ('var', 'dst'):
        return "r%d" % value
    if kind == 'kgc':
        return show(pt.kgc[value]) if value < len(pt.kgc) else "<constant %d?>" % value
    if kind == 'num':
        return repr(pt.knum[value]) if value < len(pt.knum) else "<number %d?>" % value
    if kind == 'pri':
        return ('nil', 'false', 'true')[value] if value < 3 else "pri%d" % value
    if kind == 'uv':
        return "uv%d" % value
    if kind == 'lits':
        return str(value - 0x10000 if value & 0x8000 else value)
    if kind == 'jump':
        return "-> %d" % (pc + 1 + value - 0x8000)
    return str(value)


def disassemble(pt, mark_line=None):
    out = ["function#%d  lines %d-%d  %d parameter(s)  frame %d  %d upvalue(s)%s" % (
        pt.index, pt.firstline, pt.firstline + pt.numline, pt.numparams, pt.framesize, len(pt.upvalues),
        "  stored: " + pt.stored if pt.stored else "")]
    for pc, (op, a, c, b) in enumerate(pt.code):
        name = OPS[op] if op < len(OPS) else "OP%d" % op
        kind_a, kind_b, kind_c = MODES.get(name, ('lit', 'lit', 'lit'))
        parts = [operand(pt, kind_a, a, pc)] if kind_a else []
        if kind_b:
            parts += [operand(pt, kind_b, b, pc), operand(pt, kind_c, c, pc)]
        elif kind_c:
            parts.append(operand(pt, kind_c, c | (b << 8), pc))
        line = pt.lines[pc] if pc < len(pt.lines) else 0
        out.append("%s %4d  L%-6d %-7s %s" % (">" if line == mark_line else " ", pc, line, name, ", ".join(parts)))
    return "\n".join(out)


def chunk_path(chunk, dump):
    if os.path.isfile(chunk):
        return chunk
    name = chunk.lower()
    for prefix in ("x64:", "0x"):
        if name.startswith(prefix):
            name = name[len(prefix):]
    if name.endswith(".lua"):
        name = name[:-4]
    try:
        h = int(name, 16)
    except ValueError:
        raise SystemExit("%s: not a file and not a chunk hash" % chunk)
    if not dump:
        raise SystemExit("%s is a chunk name: give --dump <folder of .luac files>" % chunk)
    for c in b".lua":
        h = ((h ^ c) * FNV_PRIME) & M64
    path = os.path.join(dump, "%016x.luac" % (h & M63))
    if not os.path.isfile(path):
        raise SystemExit("%s is asset %016x, and %s does not exist" % (chunk, h & M63, path))
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("chunk", help=".luac path, or a chunk name / hash with --dump")
    ap.add_argument("command", choices=["list", "line", "key", "proto"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--dump", help="folder of <asset hash>.luac files (<game>/cw-mod/lua_dump)")
    opts = ap.parse_args()

    path = chunk_path(opts.chunk, opts.dump)
    with open(path, "rb") as f:
        protos = parse(f.read())
    label_stores(protos)

    if opts.command == "list":
        for pt in protos:
            print("function#%-4d lines %6d-%-6d %5d instr  %s" % (
                pt.index, pt.firstline, pt.firstline + pt.numline, len(pt.code),
                pt.stored or ("(top level)" if pt is protos[-1] else "")))
    elif opts.command == "proto":
        for arg in opts.args:
            print(disassemble(protos[int(arg)]))
    elif opts.command == "key":
        for arg in opts.args:
            want = int(arg, 16) & M60
            hits = [pt for pt in protos if isinstance(pt.key, XHash) and pt.key.value & M60 == want]
            if not hits:
                print("no function is stored under a key ending in %015X" % want)
            for pt in hits:
                print(disassemble(pt))
    else:
        line = int(opts.args[0])
        # The innermost function whose code has an instruction on that line.
        hits = [pt for pt in protos[:-1] if line in pt.lines] or [pt for pt in protos if line in pt.lines]
        if not hits:
            print("no instruction is on line %d" % line)
        else:
            print(disassemble(min(hits, key=lambda pt: pt.numline), mark_line=line))
    return 0


if __name__ == "__main__":
    sys.exit(main())
