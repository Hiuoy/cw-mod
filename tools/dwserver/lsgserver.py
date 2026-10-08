#!/usr/bin/env python3
"""LSG listener on port 3074 -- the transport the login flow dies without.

WHAT THIS IS AND IS NOT. This is deliberately a RECORDER first and a responder second, the same shape
that solved auth and umbrella. Two boots' worth of evidence says that is the fast path: each time,
the client's own bytes answered a question that staring at the disassembly did not.

  * It IS a live TCP peer on 3074, so `Lsg_ConnectTask` stops getting a refused connect and the login
    flow proceeds far enough to actually send its first frame.
  * It DOES capture that frame byte-for-byte to material/lsg_frames.jsonl.
  * It DOES try, against the recorded bytes, every plausible reading of "which byte range does the
    client SHA-1", and reports which one -- if any -- reproduces the 8 CLIENTCHAL bytes on the wire.
  * It does NOT yet claim to complete the handshake. The key schedule (lsgcrypto.py) is transcribed
    and testable, but the exact hashed range is the one thing the decompilation does not pin, because
    the client hashes a SCRATCH buffer (v62) that is assembled separately from the outbound frame.
    Guessing it would cost a boot per guess; solving it from one recorded frame costs none.

WHY THE PORT IS 3074 AND NOT CONFIGURABLE. Lsg_ConnectTask_Ctor (0x7FF729DF1140) hardcodes it at
+3504 and nothing assigns that field. The auth reply's lsg_endpoint supplies the HOST ONLY. See the
LSG_PORT note in authserver.py -- we already burned a test cycle on this.

Run (the user starts their own servers; this is the command, not an invitation for a tool to run it):
    python -u lsgserver.py
"""
from __future__ import annotations
import argparse
import base64
import binascii
import datetime
import hashlib
import json
import os
import pathlib
import socket
import socketserver
import struct
import sys
import threading
import time

import bdbuf
import lobby_router
import lsgcrypto

HERE = pathlib.Path(__file__).resolve().parent
MATERIAL = HERE / "material"
FRAMES_PATH = MATERIAL / "lsg_frames.jsonl"
TICKETS_DIR = MATERIAL / "tickets"

LSG_PORT = 3074

# THE REPLY KNOBS, and the one experiment they exist for.
#
# MEASURED 2026-08-01, and it moved the wall: replying with inner tag 1 changed the client's
# behaviour completely. It stopped waiting, closed the socket, and reported a NAMED failure --
#     [status 25] Error encountered while reporting extended auth info; error: HIDDEN (4)
# where previously it sat silent through keepalives forever. So tag 1 IS the right envelope; the
# message is now being dispatched and acted on. (`HIDDEN` is not a clue: Lsg_ErrorCodeToString
# 0x7FF729D55E90 is a stub that returns the literal "HIDDEN" for every code in retail. Only the
# NUMBER means anything, and it comes from *(task+88) via Lsg_Task_GetErrorCode 0x7FF729D6B720.)
#
# Two candidates remain for that 4, and they want opposite fixes:
#   (a) our [UInt64][UInt32] did not parse, so BdLobby_OnServiceReply_Tag1 took its else-branch,
#       which writes *(event+24) = 4 -- a hardcoded parse-failure code.
#   (b) it parsed fine, errorCode 0 sent us down the SUCCESS branch, and the ExtendedAuthInfo
#       task's own result decoder then failed on a body that carries none of its fields.
#
# `--reply-error 5` separates them in ONE boot, because the non-zero path reads nothing after the
# code: if the client then reports 5, parsing is perfect and only the success-path body is missing
# (b). If it still reports 4, the envelope itself is not parsing (a). A discriminating experiment is
# worth more than another guess at the body -- that is the lesson this file keeps re-learning.
REPLY = {"enabled": True, "handle": 0, "error": 0, "flag": 0}

# THESE KNOBS ARE NOW AN OVERRIDE, NOT THE SERVER.
#
# Milestone 2 is done, and with it the era of one hard-coded answer: lobby_router.py parses each
# request into (serviceId, taskId) and dispatches it, so the reply depends on what was asked. The
# knobs stay because the sweep is this project's cheapest instrument -- but when one is passed it
# now speaks for EVERY service on the connection, which is a blunt thing to leave switched on. So
# it is opt-in and announced at startup, and a plain run goes through the router.
#
# Nothing is lost by the change: the router's default reply is byte-for-byte the one that reached
# `[status 27] Login Complete` (0:0:0:u32), which is what the sweep had already found.
OVERRIDE = {"active": False}

# ONE BOOT SHOULD ANSWER MORE THAN ONE QUESTION.
#
# The knobs above are process-wide, so a boot tests exactly one hypothesis and the next one costs
# another launch. That is the real reason this effort moves a fact at a time -- not the difficulty of
# any single question.
#
# But the client hands us free trials and we have been throwing them away. MEASURED: on a tag-1 error
# the client CLOSES and retries the whole login, and it did so four times in a row on 2026-08-01
# (seven consecutive logins on the run before). Each retry is a fresh TCP connection carrying the same
# request, so answering the Nth one differently turns a boot into a SEQUENCE of experiments whose
# results the game log already separates for us: one `[status 25] ... error: HIDDEN (n)` line per
# attempt, in order.
#
# So --sweep takes an ordered list of variants and hands out the next one per connection. Read the
# client's error codes top to bottom and they line up with the list.
#
# ASSIGNMENT IS LAZY -- a variant is taken at the moment we are about to send the first reply on a
# connection, not when the connection is accepted. A connection that dies in the handshake, or a stray
# probe, would otherwise silently consume a variant and shift every later result by one, which is the
# kind of off-by-one that reads as a real finding.
#
# The last variant is HELD, not wrapped, once the list runs out: extra retries then re-test the final
# case instead of quietly starting a second pass that looks like new data.
SWEEP: list[dict] = []
_sweep_lock = threading.Lock()
_sweep_next = 0


# THE REMAINDER AFTER THE FLAG -- what error 4 is actually about.
#
# READ OUT OF THE ENGINE 2026-08-02, not guessed. The task is bdAntiCheat::reportExtendedAuthInfo()
# (named by its own error strings in sub_7FF729DF2420), and the 4 comes from ONE place:
#
#   sub_7FF729D6B930 (the generic bdLobby task reply handler)
#       decoder = *(*(task) + 40)            // the task class's own result deserializer
#       ok      = decoder(task, &buffer)
#       *(task+36) = 2                       // state = success
#       if (!ok) { *(task+88) = 4;           // <- HARDCODED. the ONLY writer of this code
#                  *(task+36) = 3; }         // state = failed
#
# So error 4 means exactly "the task's result deserializer returned false", with no detail -- the
# same shape as the tag-1 else-branch's hardcoded 4. Two different hardcoded 4s, one meaning:
# DIDN'T PARSE. Which is why 0:0 and 0:1 were indistinguishable: the flag is read before the
# decoder ever runs, so its value cannot matter.
#
# The deserializer itself sits behind Arxan return-gadget thunks and could not be read directly.
# The shapes below are therefore CANDIDATES, and the standard bdLobby idiom -- a UInt32 row count
# first, then that many rows -- is the leading one: our reply currently ends after the flag, so a
# decoder reading a single typed UInt32 fails on an exhausted buffer. That is a hypothesis with a
# named mechanism, not a guess at a field, and the sweep tests all of them in one boot.
REMAINDERS = {
    "none": lambda: b"",
    "u32":  lambda: bdbuf.w_u32(0),      # zero result rows -- the leading candidate
    "u8":   lambda: bdbuf.w_u8(0),
    "u64":  lambda: bdbuf.w_u64(0),
    "i32":  lambda: bdbuf.w_i32(0),
    "u32x2": lambda: bdbuf.w_u32(0) + bdbuf.w_u32(0),
}


def parse_variant(spec: str) -> dict:
    """`err[:flag[:handle[:remainder]]]`, or `none` for the silent control.

    Deliberately terse because it is typed on a command line between game launches.
    """
    s = spec.strip().lower()
    if s in ("none", "silent", "-"):
        return {"enabled": False, "handle": 0, "error": 0, "flag": 0, "remainder": "none",
                "spec": spec.strip()}
    parts = s.split(":")
    if len(parts) > 4:
        raise ValueError(f"variant {spec!r}: expected err[:flag[:handle[:remainder]]]")
    rem = "none"
    if len(parts) == 4:
        rem = parts[3]
        if rem not in REMAINDERS:
            raise ValueError(f"variant {spec!r}: remainder must be one of "
                             f"{', '.join(REMAINDERS)}")
        parts = parts[:3]
    try:
        nums = [int(p, 0) for p in parts]
    except ValueError:
        raise ValueError(f"variant {spec!r}: expected err[:flag[:handle[:remainder]]] "
                         f"or 'none'") from None
    nums += [0] * (3 - len(nums))
    return {"enabled": True, "error": nums[0], "flag": nums[1], "handle": nums[2],
            "remainder": rem, "spec": spec.strip()}


def describe_variant(v: dict) -> str:
    if not v["enabled"]:
        return "SILENT (no reply -- the control)"
    if v["error"] != 0:
        return f"errorCode={v['error']}  (non-zero: the client reads nothing after the code)"
    return (f"errorCode=0 flag={v['flag']} handle={v['handle']} "
            f"remainder={v.get('remainder', 'none')}")


def take_variant() -> tuple[int, dict]:
    """Hand out the next sweep variant, or the process-wide REPLY when not sweeping."""
    global _sweep_next
    if not SWEEP:
        return -1, REPLY
    with _sweep_lock:
        idx = _sweep_next
        _sweep_next += 1
    return idx, SWEEP[min(idx, len(SWEEP) - 1)]

# THE HELLO, confirmed against three captured connections on 2026-08-01. Lsg_SendHello
# (0x7FF729DE8940) writes exactly 28 bytes and the capture matched byte-for-byte:
#     c8000000 c8000000 dc000000 dc000000 ffff0300 <8 bytes>
#     u32 200, u32 200, u32 220, u32 220, u32 maxPayload, then 8 bytes from bdSecureSocket+636.
# The tail varies per connection, so +636 is a per-connection CLIENT NONCE -- and it is the same
# field the handshake later feeds into the hash, which is why capturing it matters.
# 220 twice as separate DWORDS is where the folklore "magic 0xDCDC" comes from; on the wire it is
# DC 00 00 00 DC 00 00 00. The HELLO carries NO type-tag byte, unlike every server->client message.
HELLO_LEN = 28
HELLO_PREFIX = struct.pack("<IIII", 200, 200, 220, 220)

# SERVER->CLIENT MESSAGES ARE u32-LENGTH-PREFIXED. The receive path is a state machine on
# *(this+180): sub_7FF729DE86A0 reads exactly 4 bytes into *(this+192) = N, tears the connection down
# if N > maxPayload and idles if N == 0; sub_7FF729DE8740 then reads 1 tag byte and N-2 more.
# Lsg_DispatchRecvMessage hands handlers (buf, offset=1, len=N-1) with the tag at buf[0], so
#     N = len(tag + payload) + 1
# MEASURED THE HARD WAY 2026-08-01: a first attempt sent the 21 bytes with no prefix. The client read
# `81 dc 00 00` as the length (56449), waited for 56KB that never came, and sat for 9s without
# tearing down -- which is why the log showed a LONGER stall rather than an error. No prefix is not a
# framing detail you can skip; it is the whole message boundary.
#
# The client's own HELLO is the exception: it goes out raw with no prefix and no tag. Preamble
# asymmetry like this is normal, but it is exactly what made the send side look symmetric when it is
# not -- do not infer the server format from the captured HELLO.
#
# In connection state 1 the only tag that is not an instant teardown is 0x81 ->
# Lsg_OnServerChallenge, which wants, counting from the tag:
#     [1]  u32 == 220     mismatch = teardown
#     [5]  8 bytes        -> bdSecureSocket+712, the server nonce
#     [13] 8 bytes        skipped by the parser but INSIDE the hashed range, so still load-bearing
# The client then hashes recv[1:21] into its scratch buffer and replies with CLIENTCHAL.
TAG_SERVER_CHALLENGE = 0x81
TAG_LOGIN_OK = 0x83
TAG_REJECT = 0x84
# AND THERE IS A 0xAB FRAME BYTE BETWEEN THE LENGTH AND THE TAG. The evidence is the client's own
# reconstruction of our message inside Lsg_BuildHandshake_ParseBDDATA, which hashes:
#     u32 (n+2), 0xAB, 0x81, <n received bytes>
# It is modelling the whole server message, so the real wire format is
#     [u32 N][0xAB][tag][payload]        with N = len(payload) + 2
# That reconciles every count: sub_7FF729DE8740 reads ONE byte -- the 0xAB, not the tag -- then the
# body is N-1 = tag + payload, and Lsg_DispatchRecvMessage finds the tag at buf[0] with len = N-1.
#
# MEASURED 2026-08-01: sending [u32 22][0x81][20 bytes] made the client consume 0x81 as the 0xAB
# slot and then block waiting for 21 body bytes when only 20 remained -- one byte short, hence
# another ~10s hold with no teardown. Same signature as the missing length prefix, same lesson:
# a stall means "waiting for bytes", an instant failure means "rejected".
FRAME_BYTE = 0xAB
SERVER_CHALLENGE_PAYLOAD_LEN = 20       # u32 220 + 8 server nonce + 8 skipped-but-hashed
SERVER_CHALLENGE_LEN = 4 + 1 + 1 + SERVER_CHALLENGE_PAYLOAD_LEN


def frame_message(tag: int, payload: bytes) -> bytes:
    """[u32 N][0xAB][tag][payload], N = len(payload) + 2."""
    return (struct.pack("<I", len(payload) + 2)
            + bytes([FRAME_BYTE, tag])
            + payload)

_record_lock = threading.Lock()


def _now() -> str:
    return datetime.datetime.now().isoformat(timespec="milliseconds")


def record(entry: dict) -> None:
    MATERIAL.mkdir(parents=True, exist_ok=True)
    line = json.dumps(entry, separators=(",", ":"))
    with _record_lock:
        with FRAMES_PATH.open("a", encoding="utf-8") as f:
            f.write(line + "\n")


def load_session_key() -> bytes | None:
    """The 24-byte key our own auth reply minted for the most recent transaction.

    authserver persists it per transaction AND as tickets/latest.json. Latest is the right one here:
    the client reconnects to LSG immediately after authenticating, and every observed boot re-runs
    auth first, so the newest ticket is always the one in play.
    """
    latest = TICKETS_DIR / "latest.json"
    if not latest.exists():
        return None
    rec = json.loads(latest.read_text(encoding="utf-8"))
    key = rec.get("session_key")
    return base64.b64decode(key) if key else None


def parse_hello(data: bytes) -> dict | None:
    """Recognise the 28-byte HELLO and pull out the two fields a server needs."""
    if len(data) < HELLO_LEN or not data.startswith(HELLO_PREFIX):
        return None
    max_payload = struct.unpack_from("<I", data, 16)[0]
    return {"max_payload": max_payload, "client_nonce": data[20:28]}


def build_server_challenge(server_nonce: bytes, filler: bytes) -> bytes:
    """The tag-0x81 reply the client is waiting for in state 1.

    Both 8-byte fields are ours to choose -- the second one is not even parsed -- but they must be
    remembered, because the client hashes bytes [1:21] of this message and the digest feeds the whole
    key schedule. That is the entire reason this function returns the bytes instead of writing them
    straight to the socket.
    """
    if len(server_nonce) != 8 or len(filler) != 8:
        raise ValueError("both server-challenge fields are exactly 8 bytes")
    payload = struct.pack("<I", 220) + server_nonce + filler
    return frame_message(TAG_SERVER_CHALLENGE, payload)


def hashed_slice(server_msg: bytes) -> bytes:
    """The bytes the client feeds into its digest: recv[1:21] counting from the TAG.

    The handler's buffer is [tag][payload], so this is just the payload -- everything after the
    4-byte length and the 0xAB frame byte. Slicing from the wrong origin is silent: it still yields
    20 plausible bytes and a key schedule that simply never matches.
    """
    return server_msg[6:6 + SERVER_CHALLENGE_PAYLOAD_LEN]


# CONFIRMED IN-GAME 2026-08-01, three connections, identical result: wrap=False and the hashed body
# is the client's frame minus its trailing 8 bytes. The 153-byte CLIENTCHAL frame hashed 145 and
# carried the challenge at offset 145 -- i.e. the client appends the 8 challenge bytes AFTER hashing,
# so "everything except the last 8" is the rule, not the constant 145.
CHAL_LEN = 8

# How long to hold a completed session open before giving up. VERY generous on purpose.
#
# MEASURED 2026-08-01: the client does not time out on an unanswered lobby payload at all. It sends a
# zero-length keepalive every 40s and waits. A 90s window ran to completion with two keepalives and no
# complaint, and the "Lobby disconnected for player" that followed was our own window expiring -- the
# third time in a row that our own socket close got read as the client rejecting something.
# 15 minutes so that anything we observe from here is the client's doing.
RECORD_SESSION_SECS = 900.0
# Answer every client keepalive with one of our own; see the keepalive branch in the record loop.
ECHO_KEEPALIVE = True
WRAP_CONFIRMED = False


def derive_from_exchange(hello: dict, server_msg: bytes, client_frame: bytes,
                         session_key: bytes, pubkey_der: bytes | None = None,
                         wrap: bool = WRAP_CONFIRMED) -> dict | None:
    """Reproduce the client's key schedule from a completed HELLO/challenge/CLIENTCHAL exchange.

    Returns None if our derived challenge does not match the 8 bytes the client actually sent --
    which is the only honest way to answer, because sending a 0x83 built on a wrong key would be
    rejected by the client with no explanation of which half was wrong.
    """
    if len(client_frame) <= CHAL_LEN:
        return None
    body, chal_seen = client_frame[:-CHAL_LEN], client_frame[-CHAL_LEN:]
    scratch = lsgcrypto.build_hashed_scratch(
        hello["max_payload"], hello["client_nonce"], hashed_slice(server_msg), body)
    prk = lsgcrypto.derive_prk(scratch, session_key, pubkey_der, wrap)
    mat = lsgcrypto.derive_session_material(prk)
    if mat["chal_wire"] != chal_seen:
        return None
    return {"prk": prk, **mat}


def build_login_ok(chal_kept: bytes) -> bytes:
    """Tag 0x83 -- the message that completes the handshake.

    sub_7FF729DE7B50 reads 8 bytes and compares them to bdSecureSocket+644, which holds CLIENTCHAL
    bytes [8:16] -- the half the client derived but never transmitted. Echoing it is how the server
    proves it holds the same session key. A mismatch is an immediate teardown, not an error message.
    On success the client sets state 3 and marks the connection usable (+628 and +632 = 1).
    """
    if len(chal_kept) != CHAL_LEN:
        raise ValueError(f"chal_kept must be {CHAL_LEN} bytes")
    return frame_message(TAG_LOGIN_OK, chal_kept)


def probe_prk(client_frame: bytes, session_key: bytes, pubkey_der: bytes | None,
              max_payload: int | None = None, client_nonce: bytes | None = None,
              server_msg: bytes | None = None) -> list[dict]:
    """Find the reading of the hashed range that reproduces the client's 8 CLIENTCHAL bytes.

    Two strategies, because we may or may not have the HELLO context:

      * RECONSTRUCTED (when we answered the HELLO): rebuild the fixed part of the client's scratch
        buffer with lsgcrypto.build_hashed_scratch and sweep only how much of the client's reply
        frame is appended. This is a narrow, well-founded search.
      * RAW (fallback): treat the frame itself as the hashed buffer and sweep prefix lengths. Kept
        because it costs nothing and would catch a wrong assumption in the reconstruction.

    A hit pins the hashed range AND the `wrap` flag together, which is why this is worth more than
    guessing one combination per boot.
    """
    results = []
    have_ctx = max_payload is not None and client_nonce is not None and server_msg is not None

    for wrap in (False, True):
        if wrap and not pubkey_der:
            continue
        if have_ctx:
            slice_ = hashed_slice(server_msg)
            for end in range(0, len(client_frame) + 1):
                scratch = lsgcrypto.build_hashed_scratch(
                    max_payload, client_nonce, slice_, client_frame[:end])
                try:
                    prk = lsgcrypto.derive_prk(scratch, session_key, pubkey_der, wrap)
                    mat = lsgcrypto.derive_session_material(prk)
                except ValueError:
                    continue
                idx = client_frame.find(mat["chal_wire"])
                if idx >= 0:
                    results.append({"mode": "reconstructed", "wrap": wrap, "body_len": end,
                                    "chal_at": idx, "chal_wire": mat["chal_wire"].hex()})
        for end in range(8, len(client_frame) + 1):
            try:
                prk = lsgcrypto.derive_prk(client_frame[:end], session_key, pubkey_der, wrap)
                mat = lsgcrypto.derive_session_material(prk)
            except ValueError:
                continue
            idx = client_frame.find(mat["chal_wire"])
            if idx >= 0:
                results.append({"mode": "raw", "wrap": wrap, "hashed_len": end,
                                "chal_at": idx, "chal_wire": mat["chal_wire"].hex()})
    return results


class LsgHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        print(f"[lsgd] connect from {peer}")
        self.request.settimeout(20.0)

        session_key = load_session_key()
        pubkey_der = None
        pub = MATERIAL / "lsg_pub.der"
        if pub.exists():
            pubkey_der = pub.read_bytes()

        entry: dict = {
            "ts": _now(),
            "peer": peer,
            "session_key_available": session_key is not None,
            "messages": [],
        }

        hello = None
        server_msg = None
        client_frame = b""

        try:
            first = self._recv_burst(20.0)
            entry["messages"].append({"dir": "c2s", "bytes": len(first), "hex": first.hex()})
            if not first:
                print("[lsgd] peer connected and said nothing")
                entry["note"] = "no bytes"
                return

            print(f"[lsgd] <- {len(first)} bytes: {binascii.hexlify(first[:32]).decode()}")
            hello = parse_hello(first)
            if not hello:
                print("[lsgd] not a recognised HELLO -- recorded verbatim, not answering blind")
                entry["note"] = "unrecognised first message"
                return

            print(f"[lsgd] HELLO ok: maxPayload=0x{hello['max_payload']:X} "
                  f"nonce={hello['client_nonce'].hex()}")
            entry["hello"] = {"max_payload": hello["max_payload"],
                              "client_nonce": hello["client_nonce"].hex()}

            # Both 8-byte fields are ours to pick. Fixed values, not random: if this reply turns out
            # to be malformed, a deterministic one makes two captures comparable instead of leaving
            # us wondering whether the difference was the bug.
            server_msg = build_server_challenge(b"\xa0" * 8, b"\xb0" * 8)
            self.request.sendall(server_msg)
            print(f"[lsgd] -> server challenge (tag 0x81, {len(server_msg)} bytes)")
            entry["messages"].append({"dir": "s2c", "bytes": len(server_msg),
                                      "hex": server_msg.hex()})

            client_frame = self._recv_burst(8.0)
            entry["messages"].append({"dir": "c2s", "bytes": len(client_frame),
                                      "hex": client_frame.hex()})
            if not client_frame:
                print("[lsgd] client said nothing after the challenge -- the reply was rejected, "
                      "or the framing is wrong. Recorded.")
                return

            print(f"[lsgd] <- {len(client_frame)} bytes after challenge (CLIENTCHAL)")
            if not session_key:
                print("[lsgd] no session key -- cannot complete the handshake")
                return

            keys = derive_from_exchange(hello, server_msg, client_frame, session_key, pubkey_der)
            if not keys:
                # Deliberately do NOT send 0x83 on a mismatch. A wrong echo is torn down with no
                # diagnostic, which would look identical to a framing bug and cost another boot.
                print("[lsgd] derived challenge does NOT match the client's -- not answering. "
                      "The probe below will say which reading, if any, does.")
                entry["derived"] = None
            else:
                print(f"[lsgd] *** KEY SCHEDULE MATCHES *** chal={keys['chal_wire'].hex()}")
                entry["derived"] = {"chal_wire": keys["chal_wire"].hex(),
                                    "chal_kept": keys["chal_kept"].hex(),
                                    "bd": keys["bd"].hex()}
                ok = build_login_ok(keys["chal_kept"])
                self.request.sendall(ok)
                print(f"[lsgd] -> login OK (tag 0x83, {len(ok)} bytes) -- handshake complete")
                entry["messages"].append({"dir": "s2c", "bytes": len(ok), "hex": ok.hex()})

                # Past this point the socket is a live bdLobbyService session carrying encrypted
                # 0x85 records. We read them properly framed, decrypt each one, and now ANSWER.
                #
                # WE DO NOT CLOSE. The previous capture ended with "Lobby disconnected for player"
                # two seconds after the handshake completed, and that disconnect was OURS: the old
                # code took one burst and returned, which drops the socket. The client's own
                # behaviour was never observed. Holding the connection open for RECORD_SESSION_SECS
                # makes any subsequent disconnect a fact about the client instead of about us.
                self._pump_records(keys["bd"], entry)
        except socket.timeout:
            pass
        except OSError as exc:
            print(f"[lsgd] socket error: {exc}")
            entry["error"] = str(exc)
        finally:
            entry["client_frame_sha1"] = (
                hashlib.sha1(client_frame).hexdigest() if client_frame else None)

            # Only sweep when the direct derivation failed -- on success it would just re-find the
            # same answer at a known offset.
            if client_frame and session_key and entry.get("derived") is None:
                hits = probe_prk(
                    client_frame, session_key, pubkey_der,
                    max_payload=hello["max_payload"] if hello else None,
                    client_nonce=hello["client_nonce"] if hello else None,
                    server_msg=server_msg)
                entry["prk_probe"] = hits
                if hits:
                    print(f"[lsgd] *** KEY SCHEDULE CONFIRMED *** "
                          f"{len(hits)} reading(s) reproduce CLIENTCHAL:")
                    for h in hits:
                        print(f"[lsgd]     {h}")
                else:
                    print("[lsgd] no reading reproduced CLIENTCHAL. Recorded for offline analysis.")
            elif client_frame and not session_key:
                print("[lsgd] no tickets/latest.json -- start authserver first so a session key "
                      "exists, or the probe cannot run")

            record(entry)
            print(f"[lsgd] recorded to {FRAMES_PATH.name}")

    def _pump_records(self, bd: bytes, entry: dict) -> None:
        """Read length-prefixed 0x85 records until the CLIENT closes or the session window expires.

        Unlike _recv_burst this does not guess at message boundaries -- past the handshake every
        message carries its own u32 length, so the framing is exact and a partial read is just a
        partial read rather than a lost boundary.
        """
        records: list[dict] = []
        entry["records"] = records
        buf = b""
        expect = lsgcrypto.FIRST_COUNTER
        # The client's RECEIVE counter is *(bdSecureSocket+628), which Lsg_OnLoginOk_VerifyChalEcho
        # sets to 1 as it accepts the handshake -- the same initialiser as the send side. So our
        # first outbound record carries 1 too, and Lsg_OnEncryptedMessage rejects anything else with
        # "Bad recv counter" and tears the connection down.
        send_counter = lsgcrypto.FIRST_COUNTER
        keepalives = 0
        decrypted = 0
        sent = 0
        parsing = True
        # Taken lazily on the first reply -- see the SWEEP note. Constant for the rest of THIS
        # connection: varying it mid-session would leave the client's single error code ambiguous
        # between the replies that produced it.
        variant: dict | None = None
        vidx = -1
        # Per-connection router state: one LSG connection is one Session, and handlers hang their
        # own state off it. Created here rather than in handle() because everything before the
        # record layer is handshake, and nothing there is service-aware.
        session = lobby_router.Session(
            peer=f"{self.client_address[0]}:{self.client_address[1]}")
        started = time.monotonic()
        deadline = started + RECORD_SESSION_SECS
        closed_by = "window expired"

        while time.monotonic() < deadline:
            self.request.settimeout(max(0.5, deadline - time.monotonic()))
            try:
                chunk = self.request.recv(8192)
            except socket.timeout:
                continue
            if not chunk:
                closed_by = "CLIENT closed the connection"
                break
            buf += chunk

            while parsing:
                need = lsgcrypto.record_frame_len(buf)
                if need is None or len(buf) < need:
                    break
                frame, buf = buf[:need], buf[need:]
                entry["messages"].append({"dir": "c2s", "bytes": len(frame), "hex": frame.hex()})

                if lsgcrypto.is_keepalive(frame):
                    # A bare zero length prefix. Nothing to decrypt, and NOT an error -- the engine
                    # idles on N == 0. Counted so the interval is visible.
                    #
                    # ECHO IT. MEASURED 2026-09-16: the client drops the LSG exactly 120s after the
                    # last bytes WE sent ([status 25], full re-login), and its own keepalives do not
                    # reset that. Lsg_PumpRecv (0x7FF729DE8470) restarts its last-recv stopwatch
                    # (conn+320) on any read of >0 bytes, a bare length prefix included, so an
                    # unencrypted zero-length frame back is enough. No record counter is consumed.
                    keepalives += 1
                    print(f"[lsgd] <- keepalive (zero-length frame) #{keepalives}")
                    records.append({"keepalive": True, "at": time.monotonic() - started})
                    if ECHO_KEEPALIVE:
                        self.request.sendall(lsgcrypto.KEEPALIVE_FRAME)
                        print("[lsgd] -> keepalive echo (zero-length frame)")
                        entry["messages"].append({"dir": "s2c", "bytes": 4, "hex": "00000000"})
                    continue

                try:
                    rec = lsgcrypto.parse_record(bd, frame, expect_counter=expect)
                except ValueError as exc:
                    # These strings are the engine's own; if one shows up here it means our reading
                    # of the record layer is wrong, not that the client misbehaved.
                    #
                    # DO NOT RETURN. Returning closes the socket, and then the client's status 26 is
                    # our doing rather than its own -- which is exactly how the zero-length keepalive
                    # was misread as a protocol failure on 2026-08-01. Record it, stop parsing, and
                    # keep holding the connection so the rest of the window is still a measurement.
                    print(f"[lsgd] record REJECTED: {exc} -- holding the socket open anyway")
                    records.append({"error": str(exc), "hex": frame.hex()})
                    parsing = False
                    break
                expect += 1
                decrypted += 1
                print(f"[lsgd] <- record #{rec['counter']} innerTag=0x{rec['inner_tag']:02x} "
                      f"innerLen={len(rec['body'])}")
                print(f"[lsgd]    body: {rec['body'].hex()}")

                # Decode the lobby payload as bdByteBuffer TLV. The one leading byte is
                # client->server only -- BdLobbyService_SetPayloadBuffer points the client's read
                # cursor at payload[0] and tag 1 starts with a type-checked UInt64, so there is no
                # such byte coming back the other way. See bdbuf.REQUEST_HEADER_LEN.
                fields, err = bdbuf.walk(rec["body"], bdbuf.REQUEST_HEADER_LEN)
                for f in fields:
                    print(f"[lsgd]    +{f['at']:<3} {f['name']:<8} = {f['value']!r}")
                if err:
                    # Worth reading closely rather than skimming: "consumed the buffer exactly" is
                    # the signal that the grammar is right. A partial walk that stops mid-buffer is
                    # how a wrong reading looks, and it is what the bit-packed model produced.
                    print(f"[lsgd]    !! walk stopped: {err}")
                records.append({"counter": rec["counter"],
                                "inner_tag": rec["inner_tag"],
                                "body": rec["body"].hex(),
                                "header_byte": rec["body"][0] if rec["body"] else None,
                                "fields": fields,
                                "walk_error": err})

                # ANSWER IT. Inner tag 1, not 0x86 -- BdLobbyConnection_PumpRecv only dispatches
                # 1/2/3/5 and discards everything else in silence, which is why every previous
                # session looked like the client "just waiting".
                #
                # AND ANSWER EVERY ONE, EXACTLY ONCE, IN ORDER. BdLobby_OnServiceReply_Tag1 pops its
                # pending-request queue before reading a byte of the reply, so a request we choose
                # not to answer does not merely go unanswered -- it slides every later reply onto
                # the wrong request. That is why there is no "unknown service" silence here to match
                # authserver's deliberate 404. See lobby_router's THE RULE.
                req = lobby_router.parse_request(rec["body"])
                session.seq += 1
                print(f"[lsgd]    request #{session.seq}: {req.describe()}")

                if OVERRIDE["active"]:
                    if variant is None:
                        vidx, variant = take_variant()
                        if SWEEP:
                            held = " (list exhausted -- HELD)" if vidx >= len(SWEEP) else ""
                            # Loud on purpose. This line is the only thing that maps a console
                            # session to an error code in the game log, and it has to survive being
                            # skimmed.
                            print(f"[lsgd] ===== VARIANT {vidx + 1}{held}: "
                                  f"{describe_variant(variant)} =====")
                        entry["variant"] = {"index": vidx, **variant}
                    reply = lobby_router.Reply(
                        handle=variant["handle"], error_code=variant["error"],
                        flag=variant["flag"],
                        results=REMAINDERS[variant.get("remainder", "none")](),
                        send=variant["enabled"])
                    how = "override"
                else:
                    reply, how = lobby_router.dispatch(req, session)
                    if how == "default":
                        # The one line that turns a boot into a to-do list. Not an error: answering
                        # is mandatory, so this is the router saying which service it answered
                        # blind.
                        print(f"[lsgd]    !! UNHANDLED {req.describe()} -- answered with the "
                              f"default ({reply.describe()})")

                lobby_router.journal(req, reply, how, session)
                if not reply.send:
                    print("[lsgd] variant is SILENT: not answering (the pre-2026-08-01 behaviour)")
                    continue
                reply_body = reply.to_bytes()
                record_bytes = lsgcrypto.build_record(bd, send_counter, bdbuf.TAG_SERVICE_REPLY,
                                                      reply_body, os.urandom(lsgcrypto.IV_LEN))
                self.request.sendall(record_bytes)
                print(f"[lsgd] -> reply record #{send_counter} innerTag=0x01 "
                      f"(service reply, {reply.describe()}, via {how}) "
                      f"{len(record_bytes)} bytes")
                print(f"[lsgd]    inner: {reply_body.hex()}")
                entry["messages"].append({"dir": "s2c", "bytes": len(record_bytes),
                                          "hex": record_bytes.hex()})
                sent += 1
                records.append({"sent": True, "counter": send_counter,
                                "inner_tag": bdbuf.TAG_SERVICE_REPLY,
                                "error_code": reply.error_code,
                                "handled_by": how,
                                "service_id": req.service_id,
                                "task_id": req.task_id,
                                "variant_index": vidx,
                                "body": reply_body.hex()})
                send_counter += 1

        entry["post_handshake_end"] = closed_by
        entry["post_handshake_counts"] = {"decrypted": decrypted, "sent": sent,
                                          "keepalives": keepalives}
        # Count received and sent SEPARATELY. The first version summed the records list, which also
        # holds our own outbound entries, and reported "2 record(s) decrypted" for one request and
        # one reply -- a made-up number in the one line you read first.
        vtag = f" [variant {vidx + 1}: {describe_variant(variant)}]" if variant is not None else ""
        print(f"[lsgd] post-handshake session ended: {closed_by} "
              f"({decrypted} received, {sent} sent, {keepalives} keepalive(s)){vtag}")

    def _recv_burst(self, first_timeout: float) -> bytes:
        """Read until the peer goes quiet. The client sends a message then waits for us, so a short
        idle gap after the first bytes is the message boundary -- there is no length prefix at this
        layer (Lsg_QueueSend adds none, and the captured HELLO had none)."""
        chunks: list[bytes] = []
        self.request.settimeout(first_timeout)
        try:
            while True:
                data = self.request.recv(8192)
                if not data:
                    break
                chunks.append(data)
                self.request.settimeout(1.5)
        except socket.timeout:
            pass
        return b"".join(chunks)


class ExclusiveTCPServer(socketserver.ThreadingTCPServer):
    # Same reason as authserver's ExclusiveHTTPServer: on Windows SO_REUSEADDR lets a SECOND process
    # bind an already-listening port, and the traffic then splits unpredictably between them. That
    # already cost one debugging cycle on the HTTP side. Fail the bind instead.
    allow_reuse_address = False
    daemon_threads = True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=LSG_PORT,
                    help=f"default {LSG_PORT}; the client hardcodes this and lsg_endpoint cannot "
                         f"move it, so changing this only makes the client miss us")
    ap.add_argument("--unknown-error", type=int, default=0, metavar="N",
                    help="answer services with NO registered handler with this errorCode instead of "
                         "the default success. The experiment for 'did this request matter?' -- the "
                         "client surfaces the code, so a request whose failure changes nothing is "
                         "distinguishable from one that stops the frontend. Handled services are "
                         "unaffected, so login still completes. Not a mode to leave on.")
    ap.add_argument("--reply-error", type=int, default=0, metavar="N",
                    help="errorCode in the tag-1 reply (default 0 = success). THE DISCRIMINATING "
                         "EXPERIMENT: pass 5. The client reads NOTHING after a non-zero code, so if "
                         "it then reports error 5 our envelope parses perfectly and only the "
                         "success-path body is missing; if it still reports 4, the envelope itself "
                         "is not parsing. One boot, two hypotheses separated.")
    ap.add_argument("--reply-handle", type=int, default=0, metavar="N",
                    help="the UInt64 handle. Ours to choose -- the client matches replies to "
                         "requests by QUEUE ORDER, not by this value")
    ap.add_argument("--reply-flag", type=int, default=0, metavar="N",
                    help="the UChar8 that follows a zero errorCode")
    ap.add_argument("--no-reply", action="store_true",
                    help="do not answer at all -- the pre-2026-08-01 behaviour, kept as the control")
    ap.add_argument("--sweep", metavar="V1,V2,...",
                    help="answer each successive login attempt with a DIFFERENT variant, so one boot "
                         "tests several hypotheses instead of one. The client retries the whole login "
                         "after a tag-1 error, and each retry is a fresh connection. Each variant is "
                         "`err[:flag[:handle[:remainder]]]` or `none` for silence, where remainder is "
                         f"one of {'/'.join(REMAINDERS)} and is what error 4 is about. Example: "
                         "--sweep 0:0:0:u32,0:0:0:u8,0:0:0:u64,none . The client's error codes "
                         "appear in the game log in the same order. Overrides --reply-*.")
    args = ap.parse_args()

    REPLY["enabled"] = not args.no_reply
    REPLY["handle"] = args.reply_handle
    REPLY["error"] = args.reply_error
    REPLY["flag"] = args.reply_flag
    lobby_router.unknown_error = args.unknown_error

    # The knobs override the router only when one was actually PASSED. Comparing against the
    # argparse defaults would be wrong here -- `--reply-error 0` is a real instruction to answer
    # every service with a fixed success, and it must not be indistinguishable from not asking.
    OVERRIDE["active"] = any(f in sys.argv for f in
                             ("--reply-error", "--reply-handle", "--reply-flag", "--no-reply",
                              "--sweep"))

    if args.sweep:
        try:
            SWEEP.extend(parse_variant(s) for s in args.sweep.split(",") if s.strip())
        except ValueError as exc:
            ap.error(str(exc))
        if not SWEEP:
            ap.error("--sweep needs at least one variant")

    srv = ExclusiveTCPServer((args.host, args.port), LsgHandler)
    print(f"[lsgd] listening on {args.host}:{args.port}")
    print(f"[lsgd] frames -> {FRAMES_PATH}")
    if not OVERRIDE["active"]:
        # What the server claims to implement, printed before the game launches so the census is
        # read against a known starting point.
        print(f"[lsgd] router: {len(lobby_router.registered())} handler(s) registered")
        for line in lobby_router.registered():
            print(f"[lsgd]     {line}")
        print(f"[lsgd] router: every other service gets "
              + (f"errorCode {args.unknown_error} (--unknown-error)" if args.unknown_error
                 else "the default success (errorCode 0, zero result rows)"))
        print(f"[lsgd] census -> {lobby_router.CENSUS_PATH}")
    else:
        print("[lsgd] OVERRIDE ACTIVE: the reply knobs answer EVERY service, router bypassed")
    if SWEEP:
        # Printed BEFORE the game is launched so the mapping is on screen above the whole run.
        print(f"[lsgd] SWEEP: {len(SWEEP)} variant(s), one per login attempt, in this order:")
        for i, v in enumerate(SWEEP):
            print(f"[lsgd]     {i + 1}. {describe_variant(v)}")
        print("[lsgd]     (extra attempts re-test the last one; they do not wrap)")
        print("[lsgd] read the client's `error: HIDDEN (n)` lines in the same order")
    elif OVERRIDE["active"] and REPLY["enabled"]:
        print(f"[lsgd] reply: inner tag 1, handle={REPLY['handle']} errorCode={REPLY['error']}"
              + (f" flag={REPLY['flag']}" if REPLY["error"] == 0 else
                 "  (non-zero: the client reads nothing after the code)"))
    elif OVERRIDE["active"]:
        print("[lsgd] reply: DISABLED (--no-reply) -- the control run")
    if not (TICKETS_DIR / "latest.json").exists():
        print("[lsgd] WARNING: no tickets/latest.json yet -- start authserver and let the game "
              "authenticate first, or the key-schedule probe cannot run")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[lsgd] stopping")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
