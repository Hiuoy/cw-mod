#!/usr/bin/env python3
"""Generate the crypto material for the local Demonware backend.

The BOCW client verifies Demonware auth replies with an RSA-2048 public key baked
into its image (RSASSA-PSS / MGF1 / SHA-256 / salt-len 0 — reversed in the .i64,
see memory cw-mod-demonware-backend). A second RSA-2048 key guards the LSG
handshake. Because both keys live *in the client*, we never need Demonware's
private keys: we mint our own keypairs here, patch our public keys into the client
at runtime (client/game/dw_backend.cpp), and sign with the private keys held only
by this local server.

This script emits, into ./material/ :
  auth_priv.pem / lsg_priv.pem      - our RSA-2048 private keys (server-side secret)
  auth_pub.der  / lsg_pub.der       - 294-byte DER SubjectPublicKeyInfo blobs
  dw_embedded_keys.hpp              - the same two blobs as C++ byte arrays, for the
                                      runtime patch that overwrites the client's keys
  ca_key.pem / ca_cert.pem          - a local root CA (install into Windows Trusted Root)
  server_key.pem / server_cert.pem  - one leaf cert (SAN-covers every DW hostname) that
                                      the auth/objectstore HTTPS listeners present

The client's embedded key is a fixed-length 294-byte RSA-2048 SPKI (e=65537); ours
is the same length, so the runtime patch is an in-place byte replace at the two RVAs
(auth 0xD9781B0, lsg 0xD977330).

Run once; re-running regenerates everything (you must re-patch + re-install the CA).
"""
from __future__ import annotations
import datetime
import pathlib
import sys

from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE / "material"

# Every hostname the client resolves for Demonware services (title codename navyblue).
# The leaf cert's SAN must cover all of them so Schannel validates whichever the client
# picks by environment. Redirect these to the server via the Windows hosts file
# (see hosts_entries.txt, emitted by print_hosts()).
#
# The `t9-bnet-` prefixed names are the ones this Battle.net build actually resolves — VERIFIED
# from live ClientHello SNI (t9-bnet-auth3.prod.demonware.net -> 185.34.106.72). They come from
# DwAuth_BuildRequest's "%s-%s-auth3.%s.demonware.net" template: title "t9", provider "bnet", env.
DW_HOSTNAMES = [
    "t9-bnet-auth3.prod.demonware.net",
    "t9-bnet-auth3.cert.demonware.net",
    "t9-bnet-auth3.dev.demonware.net",
    "t9-bnet-lobby.prod.demonware.net",
    "t9-bnet-lobby.cert.demonware.net",
    "t9-bnet-lobby.dev.demonware.net",
    "t9-bnet-loginqueue.prod.demonware.net",
    "t9-bnet-loginqueue.cert.demonware.net",
    "navyblue-auth3.prod.demonware.net",
    "navyblue-vanilla-auth3.cert.demonware.net",
    "navyblue-vanilla-auth3.dev.demonware.net",
    "auth3.prod.demonware.net",
    "auth3.cert.demonware.net",
    "auth3.dev.demonware.net",
    "navyblue-lobby.prod.demonware.net",
    "navyblue-vanilla-lobby.cert.demonware.net",
    "navyblue-vanilla-lobby.dev.demonware.net",
    "loginqueue.prod.demonware.net",
    "loginqueue.cert.demonware.net",
    "objectstore.prod.demonware.net",
    "objectstore.cert.demonware.net",
    "objectstore.dev.demonware.net",
    "prod.uno.demonware.net",
    "prod.umbrella.demonware.net",
]

# --- REVOCATION, and why the leaf cannot go without it ---------------------------------------
#
# MEASURED 2026-07-31, in-game and then reproduced exactly with curl. A leaf with no revocation
# information does NOT work against this client. Schannel checks the whole chain, cannot find a
# CRL or OCSP responder, and fails the handshake with CRYPT_E_NO_REVOCATION_CHECK (0x80092012)
# -- "the revocation function was unable to check revocation". The game reports that as
# `Auth task failed with HTTP code [0]`, and because TLS dies before a single HTTP byte is sent,
# the request never reaches our handler and never appears in requests.jsonl. Trusting the CA is
# necessary and NOT sufficient.
#
# Proof it is the only remaining blocker: the same request with `curl --ssl-no-revoke` returns
# 200 and a valid signed auth reply. So the fix is to give the chain a CRL it can actually fetch.
#
# THE URL IS A LITERAL IP ON PURPOSE -- do not "tidy" it into a demonware.net hostname.
# The revocation fetch is made by CRYPT32 inside the game process, and CRYPT32 links WS2_32
# itself rather than going through the game module's import table. Our winsock choke point only
# patches the game's IAT, so it would NOT see that lookup: a `crl.demonware.net` CDP would escape
# to real DNS, fail (the name does not exist publicly), and leak a query outward -- breaking both
# the fix and the guardrail that says this client cannot reach the network. 127.0.0.1 needs no
# resolver at all.
CRL_URL = "http://127.0.0.1/cwmod.crl"
CRL_PORT = 80
CRL_FILENAME = "cwmod.crl"


def crl_distribution_points() -> x509.CRLDistributionPoints:
    return x509.CRLDistributionPoints([
        x509.DistributionPoint(
            full_name=[x509.UniformResourceIdentifier(CRL_URL)],
            relative_name=None, reasons=None, crl_issuer=None,
        )
    ])


def build_crl(ca_key: rsa.RSAPrivateKey, ca_cert: x509.Certificate) -> x509.CertificateRevocationList:
    """An empty CRL -- we revoke nothing; it exists so the check can SUCCEED rather than error.

    next_update is far out because Windows caches CRLs and refetches on expiry; a short window
    would turn a working boot into a broken one later, for no benefit in a local-only PKI.
    """
    now = datetime.datetime.now(datetime.timezone.utc)
    return (
        x509.CertificateRevocationListBuilder()
        .issuer_name(ca_cert.subject)
        .last_update(now - datetime.timedelta(days=1))
        .next_update(now + datetime.timedelta(days=3650))
        .sign(ca_key, hashes.SHA256())
    )


# The client's embedded RSA-2048 SPKI is exactly this many bytes; ours must match for the
# in-place patch. RSA-2048 SPKI with e=65537 is always 294 bytes.
EXPECTED_SPKI_LEN = 294


def gen_rsa() -> rsa.RSAPrivateKey:
    return rsa.generate_private_key(public_exponent=65537, key_size=2048)


def spki_der(priv: rsa.RSAPrivateKey) -> bytes:
    der = priv.public_key().public_bytes(
        encoding=serialization.Encoding.DER,
        format=serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    if len(der) != EXPECTED_SPKI_LEN:
        raise SystemExit(
            f"SPKI is {len(der)} bytes, expected {EXPECTED_SPKI_LEN} — "
            "the runtime patch assumes a fixed-length in-place replace."
        )
    return der


def write_priv(priv: rsa.RSAPrivateKey, path: pathlib.Path) -> None:
    path.write_bytes(
        priv.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption(),
        )
    )


def emit_cpp_header(auth_der: bytes, lsg_der: bytes, path: pathlib.Path) -> None:
    def arr(name: str, data: bytes) -> str:
        body = ",".join(f"0x{b:02X}" for b in data)
        return (
            f"    // {len(data)}-byte DER SubjectPublicKeyInfo (RSA-2048, e=65537)\n"
            f"    inline constexpr std::uint8_t {name}[] = {{{body}}};\n"
        )

    header = (
        "// AUTO-GENERATED by tools/dwserver/gen_keys.py — do not edit by hand.\n"
        "// Our RSA-2048 public keys, patched over the client's embedded Demonware keys\n"
        "// at runtime (auth RVA 0xD9781B0, lsg RVA 0xD977330). The matching private keys\n"
        "// live only in tools/dwserver/material/*_priv.pem on the server.\n"
        "#pragma once\n"
        "#include <cstdint>\n\n"
        "namespace Client::Game::DwKeys {\n"
        + arr("kAuthSigPubKey", auth_der)
        + "\n"
        + arr("kLsgHandshakePubKey", lsg_der)
        + "}\n"
    )
    path.write_text(header)


def make_ca() -> tuple[rsa.RSAPrivateKey, x509.Certificate]:
    key = gen_rsa()
    now = datetime.datetime.now(datetime.timezone.utc)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "cw-mod Local Demonware Root CA")])
    ski = x509.SubjectKeyIdentifier.from_public_key(key.public_key())
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=True, path_length=1), critical=True)
        .add_extension(ski, critical=False)
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_subject_key_identifier(ski), critical=False
        )
        .add_extension(
            x509.KeyUsage(
                digital_signature=True, key_cert_sign=True, crl_sign=True,
                key_encipherment=False, content_commitment=False, data_encipherment=False,
                key_agreement=False, encipher_only=False, decipher_only=False,
            ),
            critical=True,
        )
        .sign(key, hashes.SHA256())
    )
    return key, cert


def make_leaf(ca_key: rsa.RSAPrivateKey, ca_cert: x509.Certificate) -> tuple[rsa.RSAPrivateKey, x509.Certificate]:
    key = gen_rsa()
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, DW_HOSTNAMES[0])]))
        .issuer_name(ca_cert.subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(crl_distribution_points(), critical=False)
        .add_extension(
            x509.SubjectAlternativeName([x509.DNSName(h) for h in DW_HOSTNAMES]),
            critical=False,
        )
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False
        )
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_subject_key_identifier(
                ca_cert.extensions.get_extension_for_class(x509.SubjectKeyIdentifier).value
            ),
            critical=False,
        )
        .add_extension(
            x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]), critical=False
        )
        .sign(ca_key, hashes.SHA256())
    )
    return key, cert


def write_cert(cert: x509.Certificate, path: pathlib.Path) -> None:
    path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))


def print_hosts() -> None:
    lines = ["# cw-mod local Demonware backend — append to C:\\Windows\\System32\\drivers\\etc\\hosts",
             "# (redirects the game's Demonware hostnames to the local server)"]
    lines += [f"127.0.0.1\t{h}" for h in DW_HOSTNAMES]
    (OUT / "hosts_entries.txt").write_text("\n".join(lines) + "\n")


def main() -> int:
    OUT.mkdir(exist_ok=True)

    auth_priv = gen_rsa()
    lsg_priv = gen_rsa()
    write_priv(auth_priv, OUT / "auth_priv.pem")
    write_priv(lsg_priv, OUT / "lsg_priv.pem")

    auth_der = spki_der(auth_priv)
    lsg_der = spki_der(lsg_priv)
    (OUT / "auth_pub.der").write_bytes(auth_der)
    (OUT / "lsg_pub.der").write_bytes(lsg_der)
    emit_cpp_header(auth_der, lsg_der, OUT / "dw_embedded_keys.hpp")

    ca_key, ca_cert = make_ca()
    write_priv(ca_key, OUT / "ca_key.pem")
    write_cert(ca_cert, OUT / "ca_cert.pem")
    leaf_key, leaf_cert = make_leaf(ca_key, ca_cert)
    write_priv(leaf_key, OUT / "server_key.pem")
    write_cert(leaf_cert, OUT / "server_cert.pem")
    (OUT / CRL_FILENAME).write_bytes(
        build_crl(ca_key, ca_cert).public_bytes(serialization.Encoding.DER)
    )

    print_hosts()

    print(f"Wrote crypto material to {OUT}")
    print("  auth_priv.pem / lsg_priv.pem      server-side RSA private keys")
    print("  auth_pub.der  / lsg_pub.der        294-byte SPKI blobs")
    print("  dw_embedded_keys.hpp               C++ arrays for the runtime patch")
    print("  ca_cert.pem                        install into Windows 'Trusted Root Certification Authorities'")
    print("  server_cert.pem / server_key.pem   leaf cert presented by the HTTPS listeners")
    print(f"  {CRL_FILENAME:<34} empty CRL; authserver serves it at {CRL_URL}")
    print("  hosts_entries.txt                  append to the Windows hosts file")
    print()
    print("Install the CA (elevated PowerShell):")
    print(f'  Import-Certificate -FilePath "{OUT / "ca_cert.pem"}" -CertStoreLocation Cert:\\LocalMachine\\Root')
    return 0


if __name__ == "__main__":
    sys.exit(main())
