#!/usr/bin/env python3
"""Standalone proof that our signed auth reply is one the client would accept.

This reproduces the client's verifier path WITHOUT the game:
  1. generate/load our auth keypair,
  2. build the same JSON body the auth server sends,
  3. sign it with the private key (dwsign.sign_body),
  4. verify it with the *public* key using the exact PSS parameters reversed from
     Crypto_RsaPss_Verify (PSS/MGF1-SHA256/salt=0),
  5. sanity-check the SPKI blob is the fixed 294 bytes the runtime patch assumes and
     that decoded tickets are exactly 128 bytes.

If this passes, Milestone 1's signature gate is satisfied by construction; only the
hostname/CA plumbing and the actual in-game run remain to confirm end to end.

Run:  python selftest.py        (uses material/ if present, else ephemeral keys)
"""
from __future__ import annotations
import base64
import json
import pathlib
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa

import dwsign

HERE = pathlib.Path(__file__).resolve().parent
MATERIAL = HERE / "material"
EXPECTED_SPKI_LEN = 294
TICKET_LEN = 128


def load_or_make() -> rsa.RSAPrivateKey:
    p = MATERIAL / "auth_priv.pem"
    if p.exists():
        print(f"[selftest] using {p}")
        return dwsign.load_priv(p)
    print("[selftest] material/auth_priv.pem not found — using an ephemeral key")
    return rsa.generate_private_key(public_exponent=65537, key_size=2048)


def check(name: str, cond: bool) -> bool:
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
    return cond


def main() -> int:
    priv = load_or_make()
    pub = priv.public_key()

    spki = pub.public_bytes(
        serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo
    )

    client_ticket = base64.b64encode(bytes(range(128))).decode()
    server_ticket = base64.b64encode(bytes(range(128, 256))).decode()
    body = json.dumps(
        {
            "auth_task": 47,
            "code": 700,
            "iv_seed": 12345,
            "client_ticket": client_ticket,
            "server_ticket": server_ticket,
            "lsg_endpoint": "127.0.0.1",
            "crossplay_enabled": False,
        },
        separators=(",", ":"),
    ).encode("utf-8")

    sig = dwsign.sign_body(priv, body)

    ok = True
    ok &= check(f"SPKI is exactly {EXPECTED_SPKI_LEN} bytes (in-place patchable)", len(spki) == EXPECTED_SPKI_LEN)
    ok &= check("public exponent is 65537", pub.public_numbers().e == 65537)
    ok &= check("signature is 256 bytes (RSA-2048)", len(sig) == 256)
    ok &= check("PSS/MGF1-SHA256/salt=0 verify accepts our signature", dwsign.verify_body(pub, body, sig))
    ok &= check("verify rejects a tampered body", not dwsign.verify_body(pub, body + b" ", sig))
    ok &= check("client_ticket decodes to 128 bytes", len(base64.b64decode(client_ticket)) == TICKET_LEN)
    ok &= check("server_ticket decodes to 128 bytes", len(base64.b64decode(server_ticket)) == TICKET_LEN)

    print(f"\n[selftest] {'ALL PASS' if ok else 'FAILURES PRESENT'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
