#!/usr/bin/env python3
"""bdByteBuffer -- the serialization the LSG lobby layer speaks, above the 0x85 record layer.

THIS FILE REPLACES A WRONG MODEL, so read the correction before using it.

The 2026-08-01 commit "the bit grammar, confirmed against known plaintext" concluded that the lobby
payload was a BIT-PACKED stream with 5-bit type tags, and explicitly dismissed the byte-aligned
reading as "a coincidence" and "a trap avoided". That was backwards. The byte-aligned reading was
right and it was the one thrown away.

What settles it is not a plausibility argument, it is the accessors themselves:

    bdByteBuffer_ReadUChar8  (0x7FF729D38E00) = CheckTypeTag(buf,  3) + ReadRaw(buf, dst, 1)
    bdByteBuffer_ReadUInt32  (0x7FF729D38EC0) = CheckTypeTag(buf,  8) + ReadRaw(buf, dst, 4)
    bdByteBuffer_ReadUInt64  (0x7FF729D38F20) = CheckTypeTag(buf, 10) + ReadRaw(buf, dst, 8)

and bdByteBuffer_ReadRaw (0x7FF729D38470) is a plain byte cursor: it advances *(this+32) by exactly
the requested byte count, with no bit position anywhere in the object. bdByteBuffer_CheckTypeTag
(0x7FF729D388D0) reads ONE WHOLE BYTE and compares it to the expected type id.

The bit-buffer finding was real -- but it is a DIFFERENT CLASS. The handshake buffer that
Lsg_BuildHandshake_ParseBDDATA fills genuinely is bit-packed. The lobby payload buffer is
bdByteBuffer, constructed by Lsg_RecvFrameByteAndAlloc at socket+200. Generalising the grammar of the
first to the second is the whole mistake, and it is an easy one: both are "the buffer" at a glance.

THE TYPE TABLE IS NOT INFERRED. bdByteBuffer_TypeName (0x7FF729D3BEF0) holds all 25 names as a literal
array, used to format the mismatch diagnostic. That is the enum, verbatim.
"""
from __future__ import annotations
import struct

# ------------------------------------------------------------------------------------------------
# The type enum, transcribed from bdByteBuffer_TypeName's string array.
# ------------------------------------------------------------------------------------------------
TYPE_NAMES = {
    0: "NoType", 1: "Bool", 2: "Char8", 3: "UChar8", 4: "WChar16",
    5: "Int16", 6: "UInt16", 7: "Int32", 8: "UInt32", 9: "Int64",
    10: "UInt64", 11: "RangedInt32", 12: "RangeUInt32", 13: "Float32",
    14: "Float64", 15: "RangeFloat32", 16: "String", 17: "String",
    18: "MultiByteString", 19: "Blob", 20: "NaN", 21: "FullType",
    22: "Blob", 23: "StructData", 24: "Unknown Type",
}

T_NOTYPE = 0
T_BOOL, T_CHAR8, T_UCHAR8 = 1, 2, 3
T_INT16, T_UINT16, T_INT32, T_UINT32 = 5, 6, 7, 8
T_INT64, T_UINT64 = 9, 10
T_FLOAT32, T_FLOAT64 = 13, 14
T_STRING, T_BLOB = 16, 19

# TYPE 22 IS NOT A VALUE. In bdByteBuffer_CheckTypeTag a tag of 22 triggers an in-place splice of the
# buffer (sub_7FF729D38580 + three memcpys) and then RECURSES to re-read the real tag. Any reader that
# treats 22 as data desynchronises. We have never seen one on the wire; it is here so that a future
# capture containing one is recognised rather than silently mis-parsed.
T_SPLICE = 22

# STRUCTDATA (23), read out of the engine 2026-09-16. bdByteBuffer_WriteStructDataHeader (0x7FF729D394D0)
# writes the tag and then a TYPED UInt32 length (17 08 <u32>), exactly like Blob's nested length;
# bdByteBuffer_ReadStructDataHeader (0x7FF729D38DB0) is its mirror. The <len> bytes that follow are a
# protobuf message, serialized/parsed through the payload object's vtable (a len-bounded stream,
# bdStructBufferInputStream_Ctor 0x7FF729DC70F0). The first capture (msgType 95, service 3) decodes as
# valid protobuf to exactly its declared length, so the reading is checked, not assumed.
T_STRUCTDATA = 23

# Widths PROVEN from the accessors (3/8/10) or read off the fixed-size reader family alongside them.
# Anything absent here has no proven width and is deliberately NOT guessed -- an unknown type stops
# the walk instead of inventing a length.
FIXED_WIDTH = {
    T_BOOL: 1, T_CHAR8: 1, T_UCHAR8: 1,
    4: 2, T_INT16: 2, T_UINT16: 2,
    T_INT32: 4, T_UINT32: 4,
    T_INT64: 8, T_UINT64: 8,
    T_FLOAT32: 4, T_FLOAT64: 8,
}

# TYPE 0 (NoType) IS A ZERO-WIDTH END MARKER, and it is the one part of this grammar that is observed
# rather than proven. The captured 62-byte payload is: 1 header byte + 9 fields (60 bytes) + a single
# trailing 0x00. It is not padding to a boundary (62 is not a round number) and it is not part of the
# preceding Int32, whose four value bytes are already accounted for.
#
# What has NOT been found is the code that writes it. bdByteBuffer's own size getter (0x7FF729D38380)
# is a bare `*(this+40) - *(this+24)` and appends nothing, so the marker comes from the lobby layer
# above, which reaches the send path through a vtable slot and has not been traced yet.
#
# It costs nothing either way on the read side -- BdLobby_OnServiceReply_Tag1 reads a fixed field
# sequence and then hands the remainder to a task decoder; nothing looks for a terminator. We emit it
# anyway, to mirror the client exactly, and we accept it on input instead of failing the walk.
#
# String is NUL-TERMINATED, not length-prefixed. bdByteBuffer_ReadStringInto (0x7FF729D38D00) checks
# tag 0x10 then loops ReadRaw(1) until it reads a zero byte.
#
# Blob carries its length as a NESTED TYPED FIELD, not a bare u32: bdByteBuffer_ReadBlob
# (0x7FF729D384D0) checks tag 0x13 and then calls bdByteBuffer_ReadUInt32, which type-checks 0x08 on
# its own. So a blob on the wire is  13 08 <u32 len> <len bytes>  -- five header bytes, not four.


class Truncated(ValueError):
    """Ran off the end of the buffer. Distinct from an unknown type: this one is a framing fault."""


class UnknownType(ValueError):
    """A type byte with no proven width. We stop rather than guess -- see FIXED_WIDTH."""


class Reader:
    """Byte-cursor reader mirroring bdByteBuffer's read side.

    `check_types` mirrors *(this+48). Lsg_RecvFrameByteAndAlloc builds the RECEIVE buffer with
    bdByteBuffer_Ctor(buf, N-2, /*typeTags=*/1, 11), so on everything the game receives it is 1 and
    type tags are mandatory. It is a constructor argument rather than a constant only because the
    same class is used elsewhere with tags off.
    """

    def __init__(self, data: bytes, offset: int = 0, check_types: bool = True):
        self.data = data
        self.pos = offset
        self.check_types = check_types

    @property
    def remaining(self) -> int:
        return len(self.data) - self.pos

    def _raw(self, n: int) -> bytes:
        if self.remaining < n:
            raise Truncated(f"want {n} bytes at {self.pos}, {self.remaining} left")
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def _tag(self, expect: int) -> None:
        if not self.check_types:
            return
        got = self._raw(1)[0]
        if got == T_SPLICE:
            raise UnknownType("type 22 (spliced blob) -- CheckTypeTag would re-splice and recurse")
        if got != expect:
            raise UnknownType(
                f"type mismatch at {self.pos - 1}: expected "
                f"{TYPE_NAMES.get(expect, expect)}, got {TYPE_NAMES.get(got, got)}")

    def u8(self) -> int:
        self._tag(T_UCHAR8)
        return self._raw(1)[0]

    def u32(self) -> int:
        self._tag(T_UINT32)
        return struct.unpack("<I", self._raw(4))[0]

    def u64(self) -> int:
        self._tag(T_UINT64)
        return struct.unpack("<Q", self._raw(8))[0]

    def string(self) -> bytes:
        self._tag(T_STRING)
        end = self.data.find(b"\x00", self.pos)
        if end < 0:
            raise Truncated(f"unterminated string at {self.pos}")
        out = self.data[self.pos:end]
        self.pos = end + 1
        return out

    def structdata(self) -> bytes:
        self._tag(T_STRUCTDATA)
        return self._raw(self.u32())

    def blob(self) -> bytes:
        self._tag(T_BLOB)
        return self._raw(self.u32())


def walk(data: bytes, offset: int = 0) -> tuple[list[dict], str | None]:
    """Decode a whole buffer of unknown shape into a field list.

    Returns (fields, error). `error` is None only when the walk consumed the buffer exactly -- which
    is the signal that matters. A grammar that is wrong does not usually fail on field one; it fails
    by running past the end or landing on an impossible type several fields in, so "consumed exactly"
    is the check to look at, not "parsed something".
    """
    r = Reader(data, offset)
    fields: list[dict] = []
    while r.remaining:
        at = r.pos
        t = data[r.pos]
        name = TYPE_NAMES.get(t, f"?{t}")
        try:
            if t == T_NOTYPE:
                r.pos += 1
                fields.append({"at": at, "type": t, "name": name, "value": None})
            elif t in FIXED_WIDTH:
                r.pos += 1
                raw = r._raw(FIXED_WIDTH[t])
                signed = t in (T_CHAR8, T_INT16, T_INT32, T_INT64)
                val: object
                if t == T_FLOAT32:
                    val = struct.unpack("<f", raw)[0]
                elif t == T_FLOAT64:
                    val = struct.unpack("<d", raw)[0]
                else:
                    val = int.from_bytes(raw, "little", signed=signed)
                fields.append({"at": at, "type": t, "name": name, "value": val})
            elif t in (16, 17, 18):
                fields.append({"at": at, "type": t, "name": name,
                               "value": r.string().decode("utf-8", "replace")})
            elif t == T_BLOB:
                fields.append({"at": at, "type": t, "name": name, "value": r.blob().hex()})
            elif t == T_STRUCTDATA:
                raw = r.structdata()
                pb, pb_err = pb_decode(raw)
                fields.append({"at": at, "type": t, "name": name, "value": raw.hex(),
                               "protobuf": pb, "protobuf_error": pb_err})
            else:
                return fields, f"no rule for type {t} ({name}) at {at}"
        except (Truncated, UnknownType) as exc:
            return fields, f"{exc}"
    return fields, None


# ------------------------------------------------------------------------------------------------
# Protobuf, just enough to READ a StructData payload in the journal (no schema, so no names)
# ------------------------------------------------------------------------------------------------
def _varint(b: bytes, i: int) -> tuple[int, int]:
    v = shift = 0
    while True:
        if i >= len(b):
            raise Truncated("varint runs past the end")
        c = b[i]; i += 1
        v |= (c & 0x7F) << shift; shift += 7
        if not c & 0x80:
            return v, i
        if shift > 63:
            raise UnknownType("varint longer than 10 bytes")


def pb_decode(b: bytes) -> tuple[list[dict], str | None]:
    """Schema-less decode: [{field, wire, value}]. Returns (fields, error); error None = consumed exactly.

    A length-delimited value is shown as text when it is clean UTF-8 and as hex otherwise -- without
    the .proto, a nested message and a byte string cannot be told apart, so neither is claimed.
    """
    out: list[dict] = []
    i = 0
    try:
        while i < len(b):
            key, i = _varint(b, i)
            fno, wire = key >> 3, key & 7
            if wire == 0:
                val, i = _varint(b, i)
            elif wire == 1:
                if i + 8 > len(b):
                    raise Truncated("fixed64 runs past the end")
                val = int.from_bytes(b[i:i + 8], "little"); i += 8
            elif wire == 5:
                if i + 4 > len(b):
                    raise Truncated("fixed32 runs past the end")
                val = int.from_bytes(b[i:i + 4], "little"); i += 4
            elif wire == 2:
                n, i = _varint(b, i)
                if i + n > len(b):
                    raise Truncated("length-delimited runs past the end")
                chunk = b[i:i + n]; i += n
                try:
                    txt = chunk.decode("utf-8")
                    val = txt if txt.isprintable() else chunk.hex()
                except UnicodeDecodeError:
                    val = chunk.hex()
            else:
                raise UnknownType(f"wire type {wire}")
            out.append({"field": fno, "wire": wire, "value": val})
    except (Truncated, UnknownType) as exc:
        return out, str(exc)
    return out, None


# ------------------------------------------------------------------------------------------------
# Write side
# ------------------------------------------------------------------------------------------------
def _f(tag: int, payload: bytes) -> bytes:
    return bytes([tag]) + payload


def w_bool(v: bool) -> bytes:
    return _f(T_BOOL, b"\x01" if v else b"\x00")


def w_u8(v: int) -> bytes:
    return _f(T_UCHAR8, bytes([v & 0xFF]))


def w_u32(v: int) -> bytes:
    return _f(T_UINT32, struct.pack("<I", v & 0xFFFFFFFF))


def w_i32(v: int) -> bytes:
    return _f(T_INT32, struct.pack("<i", v))


def w_u64(v: int) -> bytes:
    return _f(T_UINT64, struct.pack("<Q", v & 0xFFFFFFFFFFFFFFFF))


def w_string(s: str | bytes) -> bytes:
    b = s.encode("utf-8") if isinstance(s, str) else s
    return _f(T_STRING, b + b"\x00")


def w_blob(b: bytes) -> bytes:
    # The length is a full typed UInt32 field, so this is 13 08 <len> -- see bdByteBuffer_ReadBlob.
    return _f(T_BLOB, w_u32(len(b)) + b)


def w_structdata(payload: bytes) -> bytes:
    """17 08 <len> <protobuf> -- what BdStructTask_ReadStructDataReply (0x7FF729DCCE20) reads."""
    return _f(T_STRUCTDATA, w_u32(len(payload)) + payload)


def w_end() -> bytes:
    """The trailing NoType marker the client puts at the end of its own lobby payload."""
    return bytes([T_NOTYPE])


# Protobuf write side -- the minimum the tunnelled-HTTP response header needs. The client's reader
# (sub_7FF729DC7BF0) is forward-only: fields must be written in ascending field-number order.
def pb_varint(v: int) -> bytes:
    out = bytearray()
    while True:
        c = v & 0x7F
        v >>= 7
        if v:
            out.append(c | 0x80)
        else:
            out.append(c)
            return bytes(out)


def pb_uint(field_no: int, v: int) -> bytes:
    return pb_varint(field_no << 3) + pb_varint(v)


def pb_double(field_no: int, v: float) -> bytes:
    return pb_varint((field_no << 3) | 1) + struct.pack("<d", v)


# ------------------------------------------------------------------------------------------------
# THE REPLY THE CLIENT IS ACTUALLY WAITING FOR
# ------------------------------------------------------------------------------------------------
# INNER TAGS ARE ASYMMETRIC, and this is the finding that unblocks the whole milestone.
#
# Client -> server:  0x86 lobby payload (Lsg_SendLobbyPayload_Tag86)
#                    0x88 migrate ack   (Lsg_SendMigrateAck_Tag88)
# Server -> client:  1, 2, 5   ... and NOTHING ELSE IS EVEN LOOKED AT.
#
# BdLobbyConnection_PumpRecv (0x7FF729DD7850) is the only consumer of a decrypted record. At
# 0x7FF729DD7901 it does:
#       movzx eax, [rbp+arg_10]      ; zero-extended: 0x86 is 134
#       sub eax,1 / jz -> tag 1      ; service reply
#       sub eax,1 / jz -> tag 2      ; notify (u32 kind; 20 is special-cased)
#       sub eax,1 / jz -> tag 3      ; connection rejected (synthesised by the 0x84 path)
#       cmp eax,2 / jnz -> fall through and DO NOTHING
#
# So a reply sent with inner tag 0x86 -- the obvious symmetric guess, and the one the previous plan
# recorded -- is decrypted, MAC-checked, installed into the payload buffer by
# BdLobbyService_SetPayloadBuffer, reported up as "message received"... and then dropped on the floor
# with no error, no teardown, and no log line. The client simply keeps waiting and keeps sending its
# 40-second zero-length keepalive. That is EXACTLY the capture we already have, which means the
# observed silence was never evidence about the body encoding at all.
#
# Tag 1 layout, from BdLobby_OnServiceReply_Tag1 (0x7FF729DD8090), in read order:
#       UInt64  handle      (0x0A + 8)
#       UInt32  errorCode   (0x08 + 4)
#     errorCode == 0  -> UChar8 (0x03 + 1), then the REMAINDER goes to the task-specific decoder
#     errorCode == 200 -> looked up in the handler map at this+168
#     otherwise        -> surfaced as an error event carrying errorCode
#
# NOTE WHAT IS *NOT* IN THAT LIST: there is no leading header byte on this direction.
# BdLobbyService_SetPayloadBuffer points the read cursor straight at payload[0], and the first thing
# tag 1 does is a type-checked UInt64 read -- so byte 0 of the reply must be 0x0A itself. The client's
# own OUTBOUND 0x86 body does carry one leading byte (0x26 in every capture) before its first type
# tag; that byte exists only in the client->server direction and must be skipped when parsing a
# request, never echoed when building a reply.
#
# Pending requests are matched BY QUEUE ORDER, not by the handle: this+112 is popped unconditionally
# before any of the reply is read. So the handle is ours to choose for a first reply.
TAG_SERVICE_REPLY = 1
TAG_NOTIFY = 2
TAG_REJECTED = 3
TAG_5 = 5

# The one leading byte on the client->server lobby payload. Constant across every capture so far.
REQUEST_HEADER_LEN = 1


def build_service_reply(handle: int = 0, error_code: int = 0,
                        flag: int = 0, results: bytes = b"") -> bytes:
    """The inner body for an inner-tag-1 record.

    `flag` and `results` are only read when error_code == 0. They are separated so that a reply can be
    made deliberately minimal: on a first run the honest position is that we know the ENVELOPE exactly
    and the task-specific result fields not at all, and a short well-formed reply fails visibly
    (the client reacts) where a wrong tag failed invisibly (the client waited).
    """
    out = w_u64(handle) + w_u32(error_code)
    if error_code == 0:
        out += w_u8(flag) + results
    return out + w_end()
