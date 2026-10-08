#!/usr/bin/env python3
"""Proves the routing and the recorder, with no game needed.

WHAT THIS PROTECTS. Before routing existed, authserver answered every path -- GET included --
with the signed auth blob. Under the winsock redirect that hands an auth reply to objectstore,
umbrella and uno as well, and the client discards it without complaint. The observable result is
a client that looks like it is silently doing nothing, with no way to tell which request caused
it. This test fails if that behaviour ever comes back.

It also pins the shape of material/requests.jsonl, because that file is the request contract the
local server gets built from -- the schema is meant to be READ off the client, not guessed at,
and a recorder that quietly drops a field would send us back to guessing.

Run:  python recordtest.py      # expect: ALL PASS
"""
from __future__ import annotations
import base64
import http.client
import json
import pathlib
import ssl
import struct
import sys
import threading
import time
from http.server import ThreadingHTTPServer

import authserver
import dwsign

HERE = pathlib.Path(__file__).resolve().parent
MAT = HERE / "material"

_fails: list[str] = []


def check(label: str, cond: bool) -> None:
    print(("  [PASS] " if cond else "  [FAIL] ") + label)
    if not cond:
        _fails.append(label)


def main() -> int:
    for needed in ("auth_priv.pem", "server_cert.pem", "server_key.pem", "ca_cert.pem"):
        if not (MAT / needed).exists():
            print(f"[record] missing material/{needed} -- run gen_keys.py first")
            return 2

    path = authserver.REQUESTS_PATH
    before = len(path.read_text(encoding="utf-8").splitlines()) if path.exists() else 0

    handler = authserver.make_handler(dwsign.load_priv(str(MAT / "auth_priv.pem")), "127.0.0.1")
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=str(MAT / "server_cert.pem"), keyfile=str(MAT / "server_key.pem"))
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    port = httpd.socket.getsockname()[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    cctx = ssl.create_default_context(cafile=str(MAT / "ca_cert.pem"))
    # The leaf is issued for the Demonware names, not for 127.0.0.1, and the client reaches it by
    # address because the redirect already rewrote the name. Checking the chain but not the
    # hostname is the same trust decision the game makes after the redirect.
    cctx.check_hostname = False

    def request(method: str, target: str, body: bytes | None = None,
                host: str = "objectstore.prod.demonware.net") -> tuple[int, bytes]:
        conn = http.client.HTTPSConnection("127.0.0.1", port, context=cctx)
        conn.request(method, target, body=body, headers={"Host": host})
        resp = conn.getresponse()
        data = resp.read()
        conn.close()
        return resp.status, data

    print("[record] routing")
    status, body = request("GET", "/v1.0/publisher/file/playlists")
    check("an unimplemented endpoint 404s instead of getting the auth blob", status == 404)
    check("the 404 body says who refused it", b"cw-mod" in body)

    status, _ = request("POST", "/auth/", b'{"auth_task":94,"iv_seed":7}',
                        host="auth3.prod.demonware.net")
    check("/auth/ still serves the signed reply", status == 200)

    status, body = request("HEAD", "/v1.0/anything")
    check("HEAD replies with headers and no body", status == 404 and body == b"")

    # THE REAL CLIENT SENDS EVERY SCALAR AS A STRING. Captured live 2026-07-31:
    # {"auth_task":"94","iv_seed":"1152714428","title_id":"5836","service_level":"paid",...}
    # The handler used to do `req["auth_task"] + 1` and die with TypeError on the first genuine
    # request the game ever delivered -- reported by the client as `HTTP code [0]`, the same
    # symptom the earlier TLS failure produced. This pins the fix.
    print("[record] the client's stringified scalars")
    live = (b'{"auth_task":"94","iv_seed":"1152714428","title_id":"5836",'
            b'"service_level":"paid","identity":"356c4bc3"}')
    status, body = request("POST", "/auth/", live, host="t9-bnet-auth3.prod.demonware.net")
    check("a stringified auth request does not throw", status == 200)
    reply = json.loads(body)
    check("auth_task is still incremented across the string boundary",
          str(reply["auth_task"]) == "95")
    check("iv_seed echoed verbatim (it feeds LSG key derivation)",
          str(reply["iv_seed"]) == "1152714428")

    # DwAuth_ParseReply -> sub_7FF729DF0AB0 is an unguarded && chain: no extra_data means
    # "Auth task reply contains invalid data", with nothing naming the missing field. Omitting it
    # cost a full test cycle, so the nesting is pinned here rather than left to a comment.
    check("reply carries extra_data", "extra_data" in reply)
    extra = json.loads(reply["extra_data"])          # a JSON-encoded STRING, not a nested object
    check("extra_data parses as its own JSON document", isinstance(extra, dict))
    check("extended_data present and a string", isinstance(extra.get("extended_data"), str))
    check("extended_data within the 4096-byte setter limit",
          len(extra["extended_data"].encode()) <= 4096)

    # THE TICKET IS PLAINTEXT ON PURPOSE. The decrypt slot in DwAuth_ParseReply is
    # DwAuth_StubReturnTrue (0x7FF729DF0AA0), which is `mov al,1; ret` -- it decrypts nothing, so an
    # encrypted ticket can never reach the magic check and always dies as "Auth ticket decryption
    # error" (login status 25). That is exactly what 128 random bytes did. Writing the magic at
    # offset 0 makes the client skip the slot entirely. Layout from bdAuthTicket_Deserialize
    # (0x7FF729D342E0), whose cursor runs 0 -> 128 with no slack -- hence the packed offsets.
    print("[record] the bdAuthTicket")
    ct = base64.b64decode(reply["client_ticket"])
    st = base64.b64decode(reply["server_ticket"])
    check("client_ticket is exactly 128 bytes", len(ct) == 128)
    check("client_ticket opens with the bdAuthTicket magic 0xEFBDADDE",
          ct[:4] == b"\xde\xad\xbd\xef")
    check("server_ticket opens with the magic too", st[:4] == b"\xde\xad\xbd\xef")
    magic, ttype, title_id, issued, expires, license_id, user_id = struct.unpack_from("<IBIIIQQ", ct)
    check("title_id echoed from the request, not hardcoded", title_id == 5836)
    check("timeExpires is after timeIssued", expires > issued)
    check("username field is NUL-padded to 64 bytes",
          len(ct[33:97]) == 64 and ct[33:97].rstrip(b"\0").isascii())
    # One key, both tickets: the client keeps its copy from client_ticket and relays server_ticket
    # to LSG untouched, so a mismatch would hand the two ends different keys and surface much later
    # as an unexplained handshake failure.
    check("both tickets carry the SAME 24-byte session key", ct[97:121] == st[97:121])
    check("session key is not all zeroes", ct[97:121] != bytes(24))

    # lsg_endpoint IS A BARE HOST. Lsg_ConnectTask_BeginResolve (0x7FF729DF1BE0) hands the string
    # straight to the resolver as a hostname and takes the port from Lsg_ConnectTask_Ctor's default
    # at +3504, which is 3074 and is assigned from nothing else. We shipped "127.0.0.1:3075" once
    # and the client dialled 127.0.0.1:3074; this assertion is what stops that costing another test
    # cycle. If a local LSG server ever appears, it binds authserver.LSG_PORT.
    check("lsg_endpoint carries no ':port' suffix", ":" not in str(reply["lsg_endpoint"]))
    check("the LSG port the client will dial is pinned at 3074", authserver.LSG_PORT == 3074)

    # Umbrella legacy login, the step directly after auth. 200 + any JSON object is the ENTIRE
    # contract: the reply handler (0x7FF729DF4B90) parses the body and then checks
    # sub_7FF729D46FB0 == `*(int *)json == 5`, a "root is an object" type tag, reading no field.
    # Pinned because a future edit that "helpfully" adds a schema here would be inventing one, and
    # because this must stay a named route -- if it ever falls through to the 404 default, login
    # stops dead at "Umbrella login task failed with HTTP code [404]".
    print("[record] umbrella legacy login")
    status, body = request("POST", "/v1.0/tokens/lsg/?client=",
                           b'{"ticket":"AAAA","initialVectorSeed":"MQ==","titleID":5836}',
                           host="prod.umbrella.demonware.net")
    check("the umbrella LSG-token endpoint answers 200", status == 200)
    check("its body is a JSON OBJECT (the only thing the client checks)",
          isinstance(json.loads(body), dict))

    print("[record] the artifact")
    time.sleep(0.4)  # the record lands after the reply is flushed; don't race the server thread
    new = [json.loads(line) for line in
           path.read_text(encoding="utf-8").splitlines()[before:]]
    check(f"all five requests recorded (got {len(new)})", len(new) == 5)
    if len(new) != 5:
        httpd.shutdown()
        return 1

    unknown, auth, head, live_auth, umbrella = new
    check("umbrella marked as served by its own route, not the unknown default",
          umbrella["served"] == "umbrella/lsg")
    check("no handler threw", all("error" not in r for r in new))
    # The Host header is the whole ballgame post-redirect: every endpoint shares the address
    # 127.0.0.1, so this is the only field that says which service was being asked for.
    check("Host header captured", unknown["host"] == "objectstore.prod.demonware.net")
    check("path captured", unknown["path"] == "/v1.0/publisher/file/playlists")
    check("method captured", head["method"] == "HEAD")
    check("unimplemented endpoints are marked", unknown["served"] == "unknown/404")
    check("request headers captured", "Host" in unknown["headers"])
    check("auth marked as served", auth["served"] == "auth")
    check("auth request body captured", '"auth_task":94' in auth["body"])
    # str() on both sides: the reply is string-typed by default to match the client's own
    # serializer, but --reply-ints flips it and this assertion is about the recorder, not the type.
    check("auth reply captured", str(auth["reply"]["code"]) == "700")
    check("bodies are flagged text-or-base64", auth["body_is_base64"] is False)

    httpd.shutdown()
    print()
    if _fails:
        print(f"[record] {len(_fails)} FAILED")
        return 1
    print("[record] ALL PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
