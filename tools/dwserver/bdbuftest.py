#!/usr/bin/env python3
"""Pins the bdByteBuffer grammar against the real captured lobby payload.

WHY THIS TEST EXISTS AND WHAT IT IS GUARDING. The previous model of this layer -- 5-bit type tags in a
bit-packed stream -- was adopted BECAUSE a known-plaintext check appeared to confirm it, and the
byte-aligned reading was written off in the same breath as "a coincidence". The coincidence claim was
the error. So the useful test is not "does our decoder parse something", it is:

    the ONE captured frame must decode to values we can independently corroborate,
    and it must consume the buffer EXACTLY.

Both halves matter. A wrong grammar usually parses the first field or two fine; what it cannot do is
land on the end of the buffer having produced 378 (the version string the /auth/ request carried) and
a 6-byte MAC-shaped blob and "{}" along the way.

Run:  python bdbuftest.py      # expect: ALL PASS
"""
from __future__ import annotations

import bdbuf

_fails: list[str] = []


def check(label: str, cond: bool) -> None:
    print(("  [PASS] " if cond else "  [FAIL] ") + label)
    if not cond:
        _fails.append(label)


# VERBATIM from material/lsg_frames.jsonl, 2026-08-01: the decrypted inner body of the client's first
# encrypted record (inner tag 0x86), the message the login flow sends at status 18 "Connected to LSG,
# now reporting Extended Auth Info". Annotated as byte-for-byte identical across seven logins.
CAPTURED = bytes.fromhex(
    "2603080804000000087a0100000a00000000000000000a00000000000000000a"
    "0000000000000000130806000000bb442e6d5002107b7d00070000000000")


def main() -> int:
    print("[bdbuf] the type table")
    check("type 8 is UInt32 (bdByteBuffer_ReadUInt32 checks 8)",
          bdbuf.TYPE_NAMES[8] == "UInt32" and bdbuf.FIXED_WIDTH[8] == 4)
    check("type 10 is UInt64 (bdByteBuffer_ReadUInt64 checks 10)",
          bdbuf.TYPE_NAMES[10] == "UInt64" and bdbuf.FIXED_WIDTH[10] == 8)
    check("type 3 is UChar8 (bdByteBuffer_ReadUChar8 checks 3)",
          bdbuf.TYPE_NAMES[3] == "UChar8" and bdbuf.FIXED_WIDTH[3] == 1)
    check("type 22 has no width -- it is the splice marker, not a value",
          22 not in bdbuf.FIXED_WIDTH)

    print("[bdbuf] the captured 0x86 body")
    check("the leading byte is present and is 0x26", CAPTURED[0] == 0x26)
    fields, err = bdbuf.walk(CAPTURED, bdbuf.REQUEST_HEADER_LEN)
    # THE load-bearing assertion. Not "it parsed" -- it ran to the end with nothing left over.
    check(f"the walk consumed the buffer EXACTLY (err={err})", err is None)
    check("nine fields plus the trailing NoType marker", len(fields) == 10)
    check("the last field is the zero-width NoType end marker",
          fields[-1]["type"] == bdbuf.T_NOTYPE and fields[-1]["at"] == len(CAPTURED) - 1)

    got = [(f["name"], f["value"]) for f in fields[:-1]]
    want = [
        ("UChar8", 8),
        ("UInt32", 4),
        ("UInt32", 378),          # the client version -- /auth/ sent "version":"378"
        ("UInt64", 0),
        ("UInt64", 0),
        ("UInt64", 0),
        ("Blob", "bb442e6d5002"),  # 6 bytes, MAC-shaped
        ("String", "{}"),          # the extra_data JSON the auth reply round-tripped
        ("Int32", 0),
    ]
    check("every field matches the expected decode", got == want)
    check("the version field corroborates the /auth/ request body ('version':'378')",
          any(f["value"] == 378 for f in fields))
    check("the blob is exactly 6 bytes, i.e. MAC-shaped",
          len(bytes.fromhex(fields[6]["value"])) == 6)

    print("[bdbuf] why offset 1 and not some other origin")
    # BE PRECISE ABOUT WHAT THIS PROVES. "The walk consumed the buffer" on its own does NOT single out
    # offset 1: most of this message is self-synchronising, so starting at 2 or 3 also runs to the end
    # after garbling the first field or two. Overclaiming here is exactly the mistake that produced
    # the bit-packed model -- one check that fit, promoted to a proof.
    #
    # The honest discriminator is the END MARKER. A NoType is an end-of-message marker, so it may
    # appear at the last byte and nowhere else. That, plus "explains every byte from the payload
    # start", leaves exactly one origin.
    def clean(off: int) -> bool:
        f, e = bdbuf.walk(CAPTURED, off)
        if e is not None or not f:
            return False
        markers = [x for x in f if x["type"] == bdbuf.T_NOTYPE]
        return len(markers) == 1 and markers[0]["at"] == len(CAPTURED) - 1

    check("offset 0 fails outright (0x26 is not a type)", not clean(0))
    check("offset 2 is rejected: it puts a NoType end marker mid-message", not clean(2))
    check("offset 1 is clean", clean(1))
    # STATE THE RESIDUAL UNCERTAINTY INSTEAD OF BURYING IT. Offset 3 ALSO walks clean, so the capture
    # alone does not reduce the header to one byte -- it reduces it to one or three. Offset 1 is
    # preferred because a 3-byte header would have to contain `03 08`, which is a perfectly
    # well-formed UChar8 = 8 field; reading real data as header is the less likely of the two.
    # If a future capture ever fails to parse at offset 1, offset 3 is the first thing to try.
    check("offsets 1 and 3 both walk clean -- the capture narrows the header to 1 or 3 bytes",
          [o for o in range(4) if clean(o)] == [1, 3])
    check("we take the 1-byte reading, which spends no real field on the header",
          bdbuf.REQUEST_HEADER_LEN == 1)

    print("[bdbuf] round-tripping the writers")
    rt = (bdbuf.w_u8(8) + bdbuf.w_u32(4) + bdbuf.w_u32(378) + bdbuf.w_u64(0) * 3
          + bdbuf.w_blob(bytes.fromhex("bb442e6d5002")) + bdbuf.w_string("{}")
          + bdbuf.w_i32(0) + bdbuf.w_end())
    # The strongest single assertion in this file: our writers, driven only by the grammar read out
    # of the accessors, reproduce a real captured game frame exactly.
    check("the writers reproduce the captured body byte-for-byte", rt == CAPTURED[1:])
    check("a blob writes its length as a TYPED UInt32 (13 08 ...), five header bytes not four",
          bdbuf.w_blob(b"\xaa")[:2] == bytes([bdbuf.T_BLOB, bdbuf.T_UINT32]))
    check("a string is NUL-terminated, not length-prefixed",
          bdbuf.w_string("hi") == bytes([bdbuf.T_STRING]) + b"hi\x00")

    print("[bdbuf] the service reply")
    body = bdbuf.build_service_reply(handle=0, error_code=0, flag=0)
    check("it opens with the UInt64 type byte and NOT a leading header byte",
          body[0] == bdbuf.T_UINT64)
    check("then a UInt32 error code at offset 9", body[9] == bdbuf.T_UINT32)
    check("then a UChar8 when the error code is zero", body[14] == bdbuf.T_UCHAR8)
    r = bdbuf.Reader(body)
    check("it reads back through the same accessors the client uses",
          (r.u64(), r.u32(), r.u8()) == (0, 0, 0) and r.remaining == 1)
    check("and ends with the same NoType marker the client writes",
          body[-1] == bdbuf.T_NOTYPE)
    err_body = bdbuf.build_service_reply(handle=1, error_code=5)
    check("a non-zero error code stops after the code -- the client reads no further",
          len(err_body) == 15)
    check("the reply tag is 1, not 0x86 -- the tag spaces are asymmetric",
          bdbuf.TAG_SERVICE_REPLY == 1)

    print()
    if _fails:
        print(f"[bdbuf] {len(_fails)} FAILED")
        return 1
    print("[bdbuf] ALL PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
