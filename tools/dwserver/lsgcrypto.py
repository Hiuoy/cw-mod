#!/usr/bin/env python3
"""The LSG bdSecureSocket key schedule, ported from Lsg_BuildHandshake_ParseBDDATA (0x7FF729DE7050).

WHY THIS IS TRACTABLE AT ALL. Every input to this schedule is something cw-mod already controls:

  * `session_key` is the 24-byte sessionKey we mint in authserver.build_auth_ticket and ship in BOTH
    tickets. The client copies it out of client_ticket into loginConfig+6920, and the LSG task copies
    it again to bdSecureSocket+280. It is the root of everything below.
  * `g_lsgHandshakePubKey_DER` (294 bytes) is the blob the in-game DwBackend key patch already
    replaces -- the log line "lsg: key replaced and verified" is that patch landing.

So there is no server secret to recover here. That is the whole reason Milestone 2 is a porting job
rather than a cryptanalysis one.

THE SCHEDULE, transcribed from the decompilation:

    digest = SHA1(frame_bytes)                                    # 20 bytes, bdHashSHA1
    base   = KDF(session_key, pubkey_der, 24) if wrap else session_key      # 24 bytes
    prk    = HMAC-SHA1(key=digest, msg=base)                      # 20 bytes  <-- note the operands
    chal   = KDF(prk, b"CLIENTCHAL", 16)      # [0:8] goes on the wire, [8:16] is kept at +644
    bd     = KDF(prk, b"BDDATA",     72)      # -> +672/+688/+692/+708 and two 16-byte cipher keys

NOTE THE HMAC OPERAND ORDER. It is HMAC(key=the frame digest, msg=the 24-byte key), not the other way
round, which is what you would write by habit. sub_7FF729D3A850 calls bdHMacSHA1's ctor with (a1, 20)
as the key and then hashes a2 -- and at the call site a1 is the digest and a2 is the key material.
Getting this backwards produces a perfectly plausible 20 bytes that simply never match the client.

`wrap` is `*(bdSecureSocket+618)`, selecting whether the session key is used raw or first stretched
through the KDF with the DER pubkey as the LABEL (not as a key). Which branch T9 takes at runtime is
NOT yet confirmed by observation -- see derive_prk's `wrap` argument. Both are implemented so the
recorded handshake can be tested against each.
"""
from __future__ import annotations
import hashlib
import hmac
import struct

SHA1_LEN = 20
SESSION_KEY_LEN = 24

# Labels are passed to the KDF as raw bytes with NO NUL terminator and NO length suffix -- the
# counter byte is appended directly after them. See kdf_counter_mode.
LABEL_CLIENTCHAL = b"CLIENTCHAL"
LABEL_BDDATA = b"BDDATA"


def hmac_sha1(key: bytes, msg: bytes) -> bytes:
    return hmac.new(key, msg, hashlib.sha1).digest()


def kdf_counter_mode(key: bytes, label: bytes, out_len: int) -> bytes:
    """SP800-108 KDF in FEEDBACK mode, exactly as Crypto_KdfCounterMode (0x7FF729D3A560) does it.

    Block 1:  K1 = PRF(key, label || 0x01)
    Block i:  Ki = PRF(key, K(i-1) || label || i)

    This is feedback mode -- the previous block is prepended, which is what the second loop in the
    decompilation does and what distinguishes it from plain counter mode. There is no 0x00 separator
    between label and counter and no [L]_2 length suffix; both are common in SP800-108 profiles and
    neither is present here, so do not "fix" this to match a textbook.

    PRF is HMAC-SHA1 (alg selector 2 -> 20-byte blocks). The engine also supports a 32-byte variant
    for other call sites; the LSG handshake only ever passes 2, so only SHA-1 is implemented.
    """
    if len(key) <= 8:
        raise ValueError("KDF rejects keys of 8 bytes or fewer (guard in the original)")
    if out_len >= 255 * SHA1_LEN:
        raise ValueError("KDF output too long")
    if SHA1_LEN + len(label) + 1 >= 0x190:
        raise ValueError("KDF scratch buffer is 400 bytes in the original")

    out = b""
    prev = b""
    counter = 0
    while len(out) < out_len:
        counter += 1
        if counter > 255:
            raise ValueError("counter overflow")
        block = hmac_sha1(key, prev + label + bytes([counter]))
        prev = block
        out += block
    return out[:out_len]


def derive_base_key(session_key: bytes, pubkey_der: bytes | None, wrap: bool) -> bytes:
    """The 24-byte value that gets HMAC'd under the frame digest.

    wrap=False: the session key straight from the auth ticket (bdSecureSocket+280).
    wrap=True:  KDF(session_key, label=pubkey_der, 24). The DER blob is the KDF LABEL, not a key --
                it is 294 bytes of public data used purely as domain separation.
    """
    if len(session_key) != SESSION_KEY_LEN:
        raise ValueError(f"session key must be {SESSION_KEY_LEN} bytes, got {len(session_key)}")
    if not wrap:
        return session_key
    if not pubkey_der:
        raise ValueError("wrap=True needs the g_lsgHandshakePubKey_DER blob")
    return kdf_counter_mode(session_key, pubkey_der, SESSION_KEY_LEN)


def derive_prk(frame_bytes: bytes, session_key: bytes,
               pubkey_der: bytes | None = None, wrap: bool = False) -> bytes:
    """The 20-byte pseudorandom key both sides derive, from which CLIENTCHAL and BDDATA hang.

    `frame_bytes` is the exact byte range the client SHA-1s. A server does not have to reconstruct
    that range -- it receives it -- but it does have to know where the range ENDS, which is the one
    piece still to be pinned from a recorded handshake. See lsgserver.py.
    """
    digest = hashlib.sha1(frame_bytes).digest()
    base = derive_base_key(session_key, pubkey_der, wrap)
    return hmac_sha1(digest, base)


def build_hashed_scratch(max_payload: int, client_nonce: bytes,
                         server_msg_slice: bytes, client_body: bytes) -> bytes:
    """Reassemble the buffer the client SHA-1s (v62 in the decompilation).

    The client hashes a scratch buffer that is assembled SEPARATELY from the frame it puts on the
    wire, which is why the range could not be read off the disassembly alone. Layout:

        u32 220, u32 220, u32 maxPayload          # 12 bytes, same header as the HELLO's middle
        client_nonce                              # 8 bytes, bdSecureSocket+636
        u32 (n+2), 0xAB, 0x81, server_msg_slice   # n = len(server_msg_slice)
        client_body                               # the outbound frame body

    A server can reproduce every part: it saw the nonce in the HELLO, it chose the server message,
    and the client hands back the body. `client_body` is the piece still being pinned by sweep --
    pass b"" to get just the fixed prefix.
    """
    out = struct.pack("<III", 220, 220, max_payload)
    out += client_nonce
    out += struct.pack("<I", len(server_msg_slice) + 2) + b"\xab\x81" + server_msg_slice
    return out + client_body


def derive_session_material(prk: bytes) -> dict[str, bytes]:
    """CLIENTCHAL and BDDATA expansion.

    chal_wire   the 8 bytes appended to the outbound frame -- what a server compares against to
                decide the client proved possession of the session key.
    chal_kept   bytes 8..15, stashed at bdSecureSocket+644 and not transmitted.
    bd          72 bytes -> +672 (16), +688 (4), +692 (16), +708 (4), plus two 16-byte keys that
                seed the send and recv cipher contexts (+656 / +664).
    """
    chal = kdf_counter_mode(prk, LABEL_CLIENTCHAL, 16)
    bd = kdf_counter_mode(prk, LABEL_BDDATA, 72)
    return {
        "chal_wire": chal[:8],
        "chal_kept": chal[8:16],
        "bd": bd,
        "bd_0": bd[0:16],
        "bd_16": bd[16:20],
        "bd_20": bd[20:36],
        "bd_36": bd[36:40],
    }


# --------------------------------------------------------------------------------------------------
# THE RECORD LAYER (LSG tag 0x85)
#
# Ported from Lsg_SendEncryptedMessage (0x7FF729DE8D60) and its mirror Lsg_OnEncryptedMessage
# (0x7FF729DE7780). DECODED AND VERIFIED AGAINST A LIVE CAPTURE 2026-08-01: all seven recorded
# client frames decrypt with a matching HMAC, using only keys derived from our own session key.
#
#     ctlen = (inner_len + 20) & ~0xF
#     [0]  u32 N = ctlen + 30
#     [4]  0xAB
#     [5]  0x85
#     [6]  u32 counter                      # starts at 1 in BOTH directions, see below
#     [10] 16-byte random IV
#     [26] AES-128-CBC( u32 inner_len | u8 inner_tag | body | zero pad )   # ctlen bytes
#     [26+ctlen] 8-byte truncated HMAC-SHA1
#
# TWO THINGS THAT LOOK LIKE DETAILS AND ARE NOT:
#
#   * THE HMAC COVERS THE u32 LENGTH PREFIX AND THE 0xAB. It is taken over frame[0 : N-4] -- the
#     entire frame except the 8 tag bytes. Starting at the 0x85 instead yields a perfectly plausible
#     8 bytes that never verify, and the client's only complaint is "HMAC mismatch".
#
#   * THE COUNTER STARTS AT 1, NOT 0. Lsg_OnLoginOk_VerifyChalEcho sets both +628 and +632 to 1 as
#     it accepts the handshake, so the first encrypted message each way carries 1. The client's
#     captured first frame does exactly this. A 0 here is rejected as "Bad recv counter".
#
# The padding is plain zeroes to a 16-byte multiple, NOT PKCS#7 -- the true length rides in the
# encrypted u32, so the pad is never parsed and must not be interpreted.
# --------------------------------------------------------------------------------------------------
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes  # noqa: E402

FRAME_BYTE_AB = 0xAB     # same framing byte as the handshake; lsgserver re-exports it as FRAME_BYTE
RECORD_TAG = 0x85
IV_LEN = 16
MAC_LEN = 8
HEADER_LEN = 26          # u32 N, 0xAB, 0x85, u32 counter, 16-byte IV
FIRST_COUNTER = 1


def split_bd(bd: bytes) -> dict[str, bytes]:
    """The 72-byte BDDATA block, split the way Lsg_BuildHandshake_ParseBDDATA splits it.

    The offsets are not inferred from behaviour -- v55..v60 are contiguous on that function's stack
    from rbp-40h, so the 72 bytes land in a known order:

        bd[ 0:20]  HMAC key, client -> server   (this+672 .. this+692)
        bd[20:40]  HMAC key, server -> client   (this+692 .. this+712)
        bd[40:56]  AES-128 key, client -> server   (cipher at this+656)
        bd[56:72]  AES-128 key, server -> client   (cipher at this+664)

    Note the two 4-byte fields at +688/+708 are the TAILS of the 20-byte HMAC keys, not counters.
    """
    if len(bd) != 72:
        raise ValueError(f"BDDATA must be 72 bytes, got {len(bd)}")
    return {
        "c2s_mac": bd[0:20],
        "s2c_mac": bd[20:40],
        "c2s_aes": bd[40:56],
        "s2c_aes": bd[56:72],
    }


def _aes_cbc(key: bytes, iv: bytes, data: bytes, encrypt: bool) -> bytes:
    ctx = Cipher(algorithms.AES(key), modes.CBC(iv))
    op = ctx.encryptor() if encrypt else ctx.decryptor()
    return op.update(data) + op.finalize()


def record_frame_len(data: bytes) -> int | None:
    """Total wire length of the record starting at data[0], or None if the prefix is incomplete.

    N == 0 gives 4, a bare length prefix with no body: THAT IS A KEEPALIVE, not a malformed frame.
    Lsg_RecvLengthPrefix (0x7FF729DE86A0) explicitly idles when N is zero rather than tearing the
    connection down, and the client sends one after roughly 40s of silence. Treating it as an error
    cost a session on 2026-08-01 -- see is_keepalive.
    """
    if len(data) < 4:
        return None
    return struct.unpack_from("<I", data, 0)[0] + 4


KEEPALIVE_FRAME = b"\x00\x00\x00\x00"


def is_keepalive(frame: bytes) -> bool:
    """A 4-byte frame whose length prefix is zero. Consume it and carry on; there is nothing to ack."""
    return len(frame) == 4 and struct.unpack_from("<I", frame, 0)[0] == 0


def build_record(bd: bytes, counter: int, inner_tag: int, body: bytes, iv: bytes) -> bytes:
    """Server -> client. Uses the s2c key pair; `iv` is caller-supplied so tests are deterministic."""
    keys = split_bd(bd)
    if len(iv) != IV_LEN:
        raise ValueError("IV must be 16 bytes")
    inner = struct.pack("<I", len(body)) + bytes([inner_tag]) + body
    ctlen = (len(body) + 20) & ~0xF
    if len(inner) > ctlen:
        raise ValueError("padding maths disagree with the original")
    ct = _aes_cbc(keys["s2c_aes"], iv, inner + b"\x00" * (ctlen - len(inner)), True)

    head = (struct.pack("<I", ctlen + 30) + bytes([FRAME_BYTE_AB, RECORD_TAG])
            + struct.pack("<I", counter) + iv)
    frame = head + ct
    return frame + hmac_sha1(keys["s2c_mac"], frame)[:MAC_LEN]


def parse_record(bd: bytes, frame: bytes, expect_counter: int | None = None) -> dict:
    """Client -> server. Returns the decrypted inner tag and body, or raises with the SAME reason
    string the engine would have logged -- those strings are the best oracle in the protocol."""
    keys = split_bd(bd)
    if len(frame) < HEADER_LEN + MAC_LEN:
        raise ValueError("Bad frame/payload size")
    n = struct.unpack_from("<I", frame, 0)[0]
    if len(frame) != n + 4 or frame[4] != FRAME_BYTE_AB or frame[5] != RECORD_TAG:
        raise ValueError("Bad frame/payload size")
    counter = struct.unpack_from("<I", frame, 6)[0]
    if expect_counter is not None and counter != expect_counter:
        raise ValueError("Bad recv counter")

    iv = frame[10:26]
    ct = frame[HEADER_LEN:len(frame) - MAC_LEN]
    if len(ct) % 16:
        raise ValueError("Bad frame/payload size")
    if hmac_sha1(keys["c2s_mac"], frame[:len(frame) - MAC_LEN])[:MAC_LEN] != frame[-MAC_LEN:]:
        raise ValueError("HMAC mismatch")

    pt = _aes_cbc(keys["c2s_aes"], iv, ct, False)
    inner_len = struct.unpack_from("<I", pt, 0)[0]
    if inner_len + 5 > len(pt):
        raise ValueError("Bad frame/payload size")
    return {
        "counter": counter,
        "iv": iv,
        "inner_tag": pt[4],
        "body": pt[5:5 + inner_len],
        "pad": pt[5 + inner_len:],
    }
