#!/usr/bin/env python3
"""Re-issue ONLY the server leaf cert (new SAN) + rewrite hosts_entries.txt.

Use this when the hostname list changes (e.g. the build resolves a host we didn't have —
t9-bnet-auth3.prod.demonware.net was found from live SNI). Running gen_keys.py would mint
NEW keypairs and a NEW CA, which would invalidate the public key already patched into the
game and the CA already trusted in the Windows store. This script touches neither:

  - CA (ca_key.pem / ca_cert.pem)      : loaded, unchanged  -> stays trusted.
  - auth/lsg keypairs (*_priv/*.der)   : untouched          -> stays patched in the client.
  - server leaf key (server_key.pem)   : reused             -> authserver --key still matches.
  - server_cert.pem                    : RE-SIGNED by the existing CA with the new SAN list.
  - hosts_entries.txt                  : rewritten from gen_keys.DW_HOSTNAMES.

After running: re-append the new lines from hosts_entries.txt to the Windows hosts file and
restart authserver.py (to load the new cert). No CA re-trust, no .der re-copy needed.
"""
from __future__ import annotations
import datetime
import pathlib

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.x509.oid import NameOID

import gen_keys

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE / "material"


def load_priv(path: pathlib.Path):
    return serialization.load_pem_private_key(path.read_bytes(), password=None)


def load_cert(path: pathlib.Path) -> x509.Certificate:
    return x509.load_pem_x509_certificate(path.read_bytes())


def main() -> int:
    ca_key = load_priv(OUT / "ca_key.pem")
    ca_cert = load_cert(OUT / "ca_cert.pem")
    leaf_key = load_priv(OUT / "server_key.pem")  # reuse existing leaf key

    hosts = gen_keys.DW_HOSTNAMES
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, hosts[0])]))
        .issuer_name(ca_cert.subject)
        .public_key(leaf_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        # Without this the handshake dies with CRYPT_E_NO_REVOCATION_CHECK before a byte of HTTP
        # is sent. See the long note in gen_keys.py -- measured in-game, reproduced with curl.
        .add_extension(gen_keys.crl_distribution_points(), critical=False)
        .add_extension(
            x509.SubjectAlternativeName([x509.DNSName(h) for h in hosts]), critical=False
        )
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(leaf_key.public_key()), critical=False
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
    (OUT / "server_cert.pem").write_bytes(cert.public_bytes(serialization.Encoding.PEM))

    # Re-signed from the unchanged CA, so it stays valid under the already-trusted root.
    (OUT / gen_keys.CRL_FILENAME).write_bytes(
        gen_keys.build_crl(ca_key, ca_cert).public_bytes(serialization.Encoding.DER)
    )

    gen_keys.print_hosts()

    print(f"[reissue] server_cert.pem re-signed by existing CA; SAN now covers {len(hosts)} hosts:")
    for h in hosts:
        print(f"          {h}")
    print("[reissue] hosts_entries.txt rewritten.")
    print(f"[reissue] {gen_keys.CRL_FILENAME} written; leaf now carries a CRL DP at {gen_keys.CRL_URL}")
    print("[reissue] CA and all keypairs unchanged — no re-trust, no .der re-copy needed.")
    print("[reissue] NEXT: restart authserver.py (it serves the CRL on port 80 and needs admin).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
