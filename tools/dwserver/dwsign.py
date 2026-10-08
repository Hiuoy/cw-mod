#!/usr/bin/env python3
"""Auth-reply signing, matching the client's verifier exactly.

Reversed from DwAuth_VerifyReplySignature (0x7FF729DF0F90) -> Crypto_RsaPss_Verify
(0x7FF729A24210): the client reads the base64 `X-Signature` response header, decodes it
to 256 bytes, and verifies it as RSASSA-PSS over the *raw response body bytes* with:

    hash   = SHA-256   (NID table index 0)
    MGF    = MGF1(SHA-256)
    salt   = length 0  (a7 propagated as 0 through the call chain)

Salt length 0 makes PSS deterministic, which is why the client can use a fixed
expected value. We sign with our auth private key (material/auth_priv.pem), whose
public half is patched over the client's embedded key.
"""
from __future__ import annotations
import base64
import pathlib

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa

_PSS = padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=0)


def load_priv(path: str | pathlib.Path) -> rsa.RSAPrivateKey:
    key = serialization.load_pem_private_key(pathlib.Path(path).read_bytes(), password=None)
    assert isinstance(key, rsa.RSAPrivateKey)
    return key


def sign_body(priv: rsa.RSAPrivateKey, body: bytes) -> bytes:
    """Return the raw 256-byte PSS signature over the response body."""
    return priv.sign(body, _PSS, hashes.SHA256())


def x_signature_header(priv: rsa.RSAPrivateKey, body: bytes) -> str:
    """Return the base64 value the client expects in the `X-Signature` header."""
    return base64.b64encode(sign_body(priv, body)).decode("ascii")


def verify_body(pub: rsa.RSAPublicKey, body: bytes, sig: bytes) -> bool:
    """Client-side check reproduced for the self-test."""
    from cryptography.exceptions import InvalidSignature
    try:
        pub.verify(sig, body, _PSS, hashes.SHA256())
        return True
    except InvalidSignature:
        return False
