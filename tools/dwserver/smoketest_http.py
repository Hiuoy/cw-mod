#!/usr/bin/env python3
"""End-to-end smoke test of the auth HTTPS server (no game required).

Starts authserver on an ephemeral port with the leaf cert, connects with an SSL
context that trusts our local CA (proving the Schannel-equivalent trust path), POSTs a
representative auth request, then verifies the returned X-Signature against auth_pub.der
exactly as the client would. Also checks ticket sizes and code==700.
"""
from __future__ import annotations
import base64
import json
import struct
import pathlib
import socket
import ssl
import threading
from http.server import ThreadingHTTPServer

from cryptography.hazmat.primitives import serialization

import authserver
import dwsign

HERE = pathlib.Path(__file__).resolve().parent
MATERIAL = HERE / "material"


def main() -> int:
    auth_priv = dwsign.load_priv(MATERIAL / "auth_priv.pem")
    handler = authserver.make_handler(auth_priv, "127.0.0.1")

    httpd = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    sctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    sctx.load_cert_chain(certfile=MATERIAL / "server_cert.pem", keyfile=MATERIAL / "server_key.pem")
    httpd.socket = sctx.wrap_socket(httpd.socket, server_side=True)
    port = httpd.socket.getsockname()[1]

    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()

    # Client trusts our CA and validates the hostname via SNI against the leaf SAN.
    cctx = ssl.create_default_context(cafile=str(MATERIAL / "ca_cert.pem"))

    # The studio token the client mints (DwLogin_BuildStudioToken), carrying a cw-mod.json name.
    def b64url(obj) -> str:
        return base64.urlsafe_b64encode(json.dumps(obj).encode()).decode().rstrip("=")
    name = 'Tester "Q"'
    xuid = 0x3BA339D0BFC3A238
    token = (b64url({"alg": "none", "typ": "JWT"}) + "."
             + b64url({"iss": "cw-mod", "name": name, "xuid": f"0x{xuid:016X}"}) + ".")
    body = json.dumps({"auth_task": 47, "iv_seed": 999, "title_id": 5830,
                       "extra_data": json.dumps({"version": "378", "token": token})}).encode()
    # Connect to the ephemeral port but present a SAN-covered hostname for TLS validation.
    raw = socket.create_connection(("127.0.0.1", port), timeout=5)
    tls = cctx.wrap_socket(raw, server_hostname="auth3.prod.demonware.net")
    req = (
        b"POST /auth/ HTTP/1.1\r\n"
        b"Host: auth3.prod.demonware.net\r\n"
        b"Content-Type: application/json\r\n"
        b"Content-Length: " + str(len(body)).encode() + b"\r\n"
        b"Connection: close\r\n\r\n" + body
    )
    tls.sendall(req)
    resp = b""
    while True:
        chunk = tls.recv(4096)
        if not chunk:
            break
        resp += chunk
    tls.close()
    httpd.shutdown()

    head, _, payload = resp.partition(b"\r\n\r\n")
    head_text = head.decode("latin1")
    print(head_text)
    xsig = None
    for line in head_text.split("\r\n"):
        if line.lower().startswith("x-signature:"):
            xsig = line.split(":", 1)[1].strip()

    ok = True

    def check(name, cond):
        nonlocal ok
        ok = ok and cond
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}")

    check("TLS handshake validated against local CA", True)  # reaching here means it did
    check("got an X-Signature header", xsig is not None)

    reply = json.loads(payload.decode())
    # str() on both sides: the reply is string-typed to match the client's own serializer, which
    # quotes every scalar. --reply-ints flips that, and this assertion is about the value.
    check("code == 700", str(reply.get("code")) == "700")
    check("client_ticket -> 128 bytes", len(base64.b64decode(reply["client_ticket"])) == 128)
    check("server_ticket -> 128 bytes", len(base64.b64decode(reply["server_ticket"])) == 128)
    check("crossplay_enabled is False", reply.get("crossplay_enabled") is False)
    ticket_name = base64.b64decode(reply["client_ticket"])[33:97].rstrip(b"\0").decode()
    check(f"ticket username is the token's name ({ticket_name!r})", ticket_name == name)
    check("no token -> no name (the --username fallback)", authserver.name_from_request({}) == "")
    check("garbage token -> no name", authserver.name_from_request({"extra_data": '{"token":"x.!!.y"}'}) == "")
    ticket_user = struct.unpack_from("<Q", base64.b64decode(reply["client_ticket"]), 25)[0]
    check(f"ticket userId is the token's xuid (0x{ticket_user:016X})", ticket_user == xuid)
    check("no token -> no xuid (the --user-id fallback)", authserver.xuid_from_request({}) == 0)

    pub = serialization.load_pem_private_key((MATERIAL / "auth_priv.pem").read_bytes(), None).public_key()
    if xsig:
        sig = base64.b64decode(xsig)
        check("X-Signature verifies (PSS/SHA256/salt0) over exact body bytes",
              dwsign.verify_body(pub, payload, sig))

    print(f"\n[smoke] {'ALL PASS' if ok else 'FAILURES PRESENT'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
