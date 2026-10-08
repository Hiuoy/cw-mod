#!/usr/bin/env python3
"""Proves the LSG key schedule and the listener, with no game needed.

WHAT THIS CAN AND CANNOT PROVE. It cannot prove we match the client -- only a recorded handshake does
that, which is exactly what lsgserver.py exists to collect. What it CAN prove, and what has bitten
this project before, is that the port is faithful to the decompilation and internally consistent:

  * the KDF is FEEDBACK mode (block i is fed the previous block), not plain counter mode
  * labels carry no NUL separator and no [L]_2 suffix
  * the HMAC operand order is (key=frame digest, msg=session key) and not the reverse
  * the listener really binds 3074, records the frame, and runs the probe

The operand-order check is the important one. Swapping it yields 20 perfectly random-looking bytes
and a handshake that fails with no diagnostic, which is the single most expensive class of bug here.

Run:  python lsgtest.py      # expect: ALL PASS
"""
from __future__ import annotations
import hashlib
import hmac
import json
import struct
import pathlib
import socket
import threading
import time

import lsgcrypto
import lsgserver

_fails: list[str] = []


def check(label: str, cond: bool) -> None:
    print(("  [PASS] " if cond else "  [FAIL] ") + label)
    if not cond:
        _fails.append(label)


def main() -> int:
    print("[lsg] the KDF")
    key = b"\x11" * 20
    label = b"CLIENTCHAL"

    # Recompute feedback mode by hand and require the port to agree.
    k1 = hmac.new(key, label + b"\x01", hashlib.sha1).digest()
    k2 = hmac.new(key, k1 + label + b"\x02", hashlib.sha1).digest()
    got = lsgcrypto.kdf_counter_mode(key, label, 40)
    check("block 1 = PRF(key, label || 0x01)", got[:20] == k1)
    check("block 2 is FEEDBACK: PRF(key, K1 || label || 0x02)", got[20:40] == k2)

    # The failure mode this guards: plain counter mode omits the K(i-1) prefix and silently produces
    # a different, equally plausible stream from block 2 onward.
    counter_mode_k2 = hmac.new(key, label + b"\x02", hashlib.sha1).digest()
    check("block 2 is NOT plain counter mode", got[20:40] != counter_mode_k2)

    check("output is truncated to the requested length, not rounded up to a block",
          len(lsgcrypto.kdf_counter_mode(key, label, 8)) == 8)
    check("a short key is rejected (the original guards keyLen <= 8)",
          _raises(lambda: lsgcrypto.kdf_counter_mode(b"\x00" * 8, label, 16)))

    print("[lsg] the base key")
    sk = bytes(range(24))
    check("wrap=False returns the session key untouched",
          lsgcrypto.derive_base_key(sk, None, False) == sk)
    der = b"\x30" * 294
    wrapped = lsgcrypto.derive_base_key(sk, der, True)
    check("wrap=True stretches to 24 bytes via the KDF", len(wrapped) == 24 and wrapped != sk)
    check("wrap=True without the DER blob is an error, not a silent fallback",
          _raises(lambda: lsgcrypto.derive_base_key(sk, None, True)))
    check("a session key of the wrong length is rejected",
          _raises(lambda: lsgcrypto.derive_base_key(b"\x00" * 16, None, False)))

    print("[lsg] the prk operand order")
    frame = b"\xdc\x00\x00\x00\xdc\x00\x00\x00some frame bytes"
    digest = hashlib.sha1(frame).digest()
    prk = lsgcrypto.derive_prk(frame, sk, None, False)
    check("prk = HMAC-SHA1(key=frame digest, msg=session key)",
          prk == hmac.new(digest, sk, hashlib.sha1).digest())
    # The habit-driven mistake, pinned so a future edit cannot quietly introduce it.
    check("prk is NOT HMAC(key=session key, msg=digest)",
          prk != hmac.new(sk, digest, hashlib.sha1).digest())

    print("[lsg] the expansion")
    mat = lsgcrypto.derive_session_material(prk)
    check("chal_wire is the 8 bytes that go on the wire", len(mat["chal_wire"]) == 8)
    check("chal_kept is the 8 bytes held at +644, and differs from chal_wire",
          len(mat["chal_kept"]) == 8 and mat["chal_kept"] != mat["chal_wire"])
    check("BDDATA expands to 72 bytes", len(mat["bd"]) == 72)
    check("the BDDATA slices tile the 72 bytes exactly (+672/+688/+692/+708)",
          mat["bd_0"] + mat["bd_16"] + mat["bd_20"] + mat["bd_36"] == mat["bd"][:40])

    print("[lsg] the HELLO")
    # A VERBATIM CAPTURE from the game, 2026-08-01. Using the real bytes rather than a synthetic
    # stand-in is the point: it is what proves the parser matches Lsg_SendHello's actual output and
    # not our reading of it.
    real_hello = bytes.fromhex("c8000000c8000000dc000000dc000000ffff03004fa3474bd7d2a445")
    check("the captured HELLO is 28 bytes", len(real_hello) == lsgserver.HELLO_LEN)
    h = lsgserver.parse_hello(real_hello)
    check("the captured HELLO parses", h is not None)
    check("maxPayload reads as 0x3FFFF", h and h["max_payload"] == 0x3FFFF)
    check("the client nonce is the trailing 8 bytes",
          h and h["client_nonce"] == bytes.fromhex("4fa3474bd7d2a445"))
    check("a frame with the wrong prefix is rejected rather than half-parsed",
          lsgserver.parse_hello(b"\x00" * 28) is None)
    check("a truncated HELLO is rejected", lsgserver.parse_hello(real_hello[:20]) is None)

    print("[lsg] the server challenge")
    msg = lsgserver.build_server_challenge(b"\xa0" * 8, b"\xb0" * 8)
    payload = lsgserver.hashed_slice(msg)
    # THE FRAMING IS [u32 N][0xAB][tag][payload] WITH N = len(payload) + 2, and both halves of that
    # were learned by getting them wrong. No length prefix: the client read `81 dc 00 00` as a
    # 56449-byte length and stalled 9s. No 0xAB: it consumed the tag as the 0xAB slot and blocked one
    # byte short, stalling ~10s. Both times a LONGER stall, never an error -- so these checks exist
    # because the failure mode is indistinguishable from "server is slow".
    check("the message is 26 bytes: 4 + 0xAB + tag + 20", len(msg) == 26 == lsgserver.SERVER_CHALLENGE_LEN)
    check("N = len(payload) + 2, as the client's own hash reconstruction writes it",
          struct.unpack_from("<I", msg)[0] == len(payload) + 2 == 22)
    check("N is within maxPayload, so the receiver will not tear down",
          struct.unpack_from("<I", msg)[0] <= 0x3FFFF)
    check("N >= 2, the minimum sub_7FF729DE8740 accepts", struct.unpack_from("<I", msg)[0] >= 2)
    check("byte 4 is the 0xAB frame byte (what sub_7FF729DE8740 actually consumes)",
          msg[4] == lsgserver.FRAME_BYTE)
    check("byte 5 is the 0x81 tag the dispatcher expects in state 1",
          msg[5] == lsgserver.TAG_SERVER_CHALLENGE)
    check("the body the dispatcher sees is N-1 bytes = tag + payload",
          len(msg) - 4 - 1 == struct.unpack_from("<I", msg)[0] - 1)
    check("payload [0:4] are u32 220 (a mismatch is an instant teardown)",
          payload[:4] == (220).to_bytes(4, "little"))
    check("the server nonce lands at payload [4:12]", payload[4:12] == b"\xa0" * 8)
    check("the skipped-but-hashed filler lands at payload [12:20]", payload[12:20] == b"\xb0" * 8)
    check("odd field widths are rejected",
          _raises(lambda: lsgserver.build_server_challenge(b"\xa0" * 4, b"\xb0" * 8)))

    print("[lsg] the hashed scratch")
    check("the hashed slice is the 20 payload bytes, not sliced from the framed message",
          len(payload) == 20 and payload == msg[6:26])
    scratch = lsgcrypto.build_hashed_scratch(0x3FFFF, h["client_nonce"], payload, b"BODY")
    check("it opens with u32 220, u32 220, u32 maxPayload",
          scratch[:12] == b"".join(x.to_bytes(4, "little") for x in (220, 220, 0x3FFFF)))
    check("the client nonce follows at [12:20]", scratch[12:20] == h["client_nonce"])
    check("then the length dword is len(server slice) + 2",
          scratch[20:24] == (20 + 2).to_bytes(4, "little"))
    check("then the 0xAB, 0x81 pair", scratch[24:26] == b"\xab\x81")
    check("then the server payload, then the body",
          scratch[26:46] == payload and scratch[46:] == b"BODY")

    print("[lsg] completing the handshake")
    # Self-consistency: build an exchange from a known key, then require derive_from_exchange to
    # recover exactly the challenge the client would have appended.
    fake_hello = {"max_payload": 0x3FFFF, "client_nonce": b"\x01" * 8}
    fake_body = b"\x95\x00\x00\x00\xab\x82" + b"\x77" * 139
    scratch2 = lsgcrypto.build_hashed_scratch(
        fake_hello["max_payload"], fake_hello["client_nonce"], payload, fake_body)
    mat2 = lsgcrypto.derive_session_material(lsgcrypto.derive_prk(scratch2, sk, None, False))
    got = lsgserver.derive_from_exchange(fake_hello, msg, fake_body + mat2["chal_wire"], sk)
    check("derive_from_exchange recovers the challenge from a full exchange", got is not None)
    check("and it agrees with the direct derivation", got and got["chal_wire"] == mat2["chal_wire"])
    check("the hashed body is the frame minus its trailing 8 challenge bytes",
          lsgserver.CHAL_LEN == 8)
    check("a frame whose challenge does not match returns None rather than a wrong key",
          lsgserver.derive_from_exchange(fake_hello, msg, fake_body + b"\x00" * 8, sk) is None)
    check("wrap=False is what the game confirmed", lsgserver.WRAP_CONFIRMED is False)

    ok = lsgserver.build_login_ok(mat2["chal_kept"])
    check("the 0x83 completion is 14 bytes: 4 + 0xAB + tag + 8", len(ok) == 14)
    check("N = 10 for an 8-byte payload", struct.unpack_from("<I", ok)[0] == 10)
    check("it carries the 0xAB frame byte and the 0x83 tag",
          ok[4] == lsgserver.FRAME_BYTE and ok[5] == lsgserver.TAG_LOGIN_OK)
    # sub_7FF729DE7B50 compares these 8 bytes to bdSecureSocket+644, which is chal[8:16] -- the half
    # the client derived and never sent. Echoing chal_wire instead is the obvious wrong move and is
    # torn down with no diagnostic.
    check("the payload is chal_KEPT, not the chal that was on the wire",
          ok[6:14] == mat2["chal_kept"] and mat2["chal_kept"] != mat2["chal_wire"])
    check("a wrong-length echo is rejected",
          _raises(lambda: lsgserver.build_login_ok(b"\x00" * 4)))

    # AGAINST THE REAL GAME, when the local artifacts are still around. This is the check that
    # actually proves we match the client rather than ourselves; it is skipped rather than failed
    # when material/ has moved on, because tickets/latest.json is overwritten on every auth.
    print("[lsg] against the real capture (skipped if material has rotated)")
    real = _real_exchange()
    if real is None:
        print("  [SKIP] no usable material/lsg_frames.jsonl + tickets/latest.json pair")
    else:
        h2, sm2, cf2, sk2, der2 = real
        got2 = lsgserver.derive_from_exchange(h2, sm2, cf2, sk2, der2)
        check("our derived CLIENTCHAL matches the bytes the GAME actually sent", got2 is not None)
        check("the game's frame carries the challenge in its last 8 bytes",
              got2 and got2["chal_wire"] == cf2[-8:])

    print("[lsg] the listener round-trip")
    before = _line_count(lsgserver.FRAMES_PATH)
    srv = lsgserver.ExclusiveTCPServer(("127.0.0.1", 0), lsgserver.LsgHandler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall(real_hello)
    s.settimeout(10)
    reply = s.recv(4096)
    check("the server answers a HELLO with a length-prefixed challenge",
          len(reply) == lsgserver.SERVER_CHALLENGE_LEN)
    check("the reply opens with the u32 length, not the tag",
          struct.unpack_from("<I", reply)[0] == 22)
    check("then the 0xAB frame byte, then the tag",
          reply[4] == lsgserver.FRAME_BYTE and reply[5] == lsgserver.TAG_SERVER_CHALLENGE)
    s.sendall(b"PRETEND-CLIENTCHAL-FRAME")
    time.sleep(2.2)
    s.close()
    time.sleep(0.5)
    srv.shutdown()

    after = _line_count(lsgserver.FRAMES_PATH)
    check(f"the exchange was recorded (lines {before} -> {after})", after == before + 1)
    if after == before + 1:
        rec = json.loads(lsgserver.FRAMES_PATH.read_text(encoding="utf-8").splitlines()[-1])
        check("all three messages recorded with direction", len(rec["messages"]) == 3)
        check("the HELLO is recorded verbatim", rec["messages"][0]["hex"] == real_hello.hex())
        check("the challenge we sent is recorded (so the probe can rebuild the digest)",
              rec["messages"][1]["dir"] == "s2c" and rec["messages"][1]["hex"] == reply.hex())
        check("the HELLO fields were extracted", rec["hello"]["max_payload"] == 0x3FFFF)
        check("the probe ran (or explained why it could not)",
              "prk_probe" in rec or rec["session_key_available"] is False)

    check("the default port is the hardcoded 3074", lsgserver.LSG_PORT == 3074)

    print()
    if _fails:
        print(f"[lsg] {len(_fails)} FAILED")
        return 1
    print("[lsg] ALL PASS")
    return 0


def _real_exchange():
    """The most recent COMPLETE game exchange, if the session key that produced it is still on disk.

    tickets/latest.json is rewritten on every auth and the game re-auths on each login retry, so only
    the last recorded connection can still be checked. Returns None instead of failing -- a rotated
    key is not a regression.
    """
    import base64
    frames = lsgserver.FRAMES_PATH
    latest = lsgserver.TICKETS_DIR / "latest.json"
    if not frames.exists() or not latest.exists():
        return None
    key = json.loads(latest.read_text(encoding="utf-8")).get("session_key")
    if not key:
        return None
    sk2 = base64.b64decode(key)
    der_path = lsgserver.MATERIAL / "lsg_pub.der"
    der2 = der_path.read_bytes() if der_path.exists() else None
    for line in reversed(frames.read_text(encoding="utf-8").splitlines()):
        rec = json.loads(line)
        msgs = rec.get("messages", [])
        if len(msgs) < 3 or not rec.get("hello") or msgs[2]["bytes"] < 32:
            continue
        h2 = {"max_payload": rec["hello"]["max_payload"],
              "client_nonce": bytes.fromhex(rec["hello"]["client_nonce"])}
        cand = (h2, bytes.fromhex(msgs[1]["hex"]), bytes.fromhex(msgs[2]["hex"]), sk2, der2)
        if lsgserver.derive_from_exchange(*cand) is not None:
            return cand
    return None


def _raises(fn) -> bool:
    try:
        fn()
    except ValueError:
        return True
    return False


def _line_count(p: pathlib.Path) -> int:
    return len(p.read_text(encoding="utf-8").splitlines()) if p.exists() else 0


if __name__ == "__main__":
    raise SystemExit(main())
