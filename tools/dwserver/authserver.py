#!/usr/bin/env python3
"""Local Demonware service: the /auth/ endpoint, and a recorder for everything else.

TWO JOBS, and the second one is currently the more valuable.

1. AUTH (Milestone 1). Serves POST https://<auth-host>/auth/ , built by DwAuth_BuildRequest
   (0x7FF729DEF740) and consumed by DwAuth_ParseReply (0x7FF729DF01E0). We return a JSON body
   the parser accepts and an `X-Signature` header that DwAuth_VerifyReplySignature validates
   against our patched-in public key. Fields the parser requires:

       auth_task        must equal the env-derived task id the client expects (see below).
       code             must be 700 (the "success, tickets follow" path).
       iv_seed          integer seed; also fed back through the LSG key derivation.
       client_ticket    base64 -> exactly 128 bytes. LSG send-side key material (we mint it).
       server_ticket    base64 -> exactly 128 bytes. LSG recv-side key material (we mint it).
       lsg_endpoint     BARE HOST of our LSG listener (Milestone 2) -- NOT host:port. Read the
                        note on LSG_PORT below before "fixing" that.
       crossplay_enabled false -> steer the legacy path, skipping Umbrella/Uno for now.

   The tickets are ours to choose because we also run the LSG server that consumes them; we
   persist the minted pair per transaction so the LSG side can look them up. This server does
   NOT implement LSG -- it only hands the client the material and endpoint for it.

2. RECORDING (the schema-extraction engine). Once the winsock choke point redirects
   demonware.net to loopback, EVERY Demonware endpoint the client wants -- objectstore,
   umbrella, uno, loginqueue -- arrives here, at this one socket. Each one is written to
   material/requests.jsonl with its Host, path, headers and body.

   That file is the request contract, observed rather than guessed, which is the thing the
   pivot brief asks for and the thing disassembly is worst at producing. Note the `Host` header
   in particular: post-redirect every endpoint shares the address 127.0.0.1, so the Host header
   is the ONLY thing that says which service the client thought it was talking to.

   THE PREVIOUS VERSION OF THIS FILE COULD NOT DO ANY OF THAT. It ignored self.path entirely and
   answered every request -- GET included -- with the signed auth blob. Under redirect that hands
   an auth reply to objectstore, which the client discards without complaint, and the result
   reads exactly like "the client silently no-ops". That is the ambiguity the brief's step 6 says
   not to guess at, so unknown paths now fail LOUDLY and visibly instead.

Usage:
    python authserver.py --host 127.0.0.1 --port 443 \
        --priv material/auth_priv.pem \
        --cert material/server_cert.pem --key material/server_key.pem \
        --lsg-endpoint 127.0.0.1
"""
from __future__ import annotations
import argparse
import base64
import datetime
import json
import os
import pathlib
import ssl
import struct
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import dwsign

HERE = pathlib.Path(__file__).resolve().parent

TICKET_LEN = 128  # client_ticket / server_ticket decode to exactly this many bytes

# Minted tickets keyed by transaction id, so the LSG server (separate process) can read them.
# A file drop is the simplest cross-process handoff; LSG picks the newest on connect.
TICKETS_DIR = HERE / "material" / "tickets"

# One JSON object per request, appended and flushed as it arrives. Append-per-request rather than
# a summary at exit for the same reason the client-side journal works that way: the interesting run
# is the one where something crashes, and a buffered recorder loses exactly that run.
REQUESTS_PATH = HERE / "material" / "requests.jsonl"
_record_lock = threading.Lock()

# Paths we actually implement. Matched as a prefix because the client appends environment and
# version segments that we have not enumerated -- and enumerating them by guessing is the habit
# this file now exists to break.
AUTH_PREFIXES = ("/auth",)

# Umbrella's LEGACY login, the step right after auth succeeds. Observed live 2026-07-31:
#   POST prod.umbrella.demonware.net /v1.0/tokens/lsg/?client=
#   {"ticket": <our own client_ticket, base64>, "initialVectorSeed": <b64 of the iv_seed
#    DECIMAL STRING>, "titleID": 5836}
# Request builder is 0x7FF729DF48A0; reply handler is 0x7FF729DF4B90.
UMBRELLA_LSG_PREFIXES = ("/v1.0/tokens/lsg",)


# The classic Demonware bdAuthTicket, still byte-for-byte intact in T9. Read off
# bdAuthTicket_Deserialize (0x7FF729D342E0), which walks a cursor 0 -> 128 with no slack, so this
# layout is the whole ticket rather than a guess at part of it.
TICKET_MAGIC = 0xEFBDADDE          # 'DE AD BD EF' on the wire
SESSION_KEY_LEN = 24               # the 3DES key LSG's bdSecureSocket will use

# WHY PLAINTEXT. ParseReply routes the ticket through a decrypt slot only when the first dword is
# NOT the magic -- and that slot, DwAuth_StubReturnTrue (0x7FF729DF0AA0), is literally
# `mov al,1; ret`. It decrypts nothing. So an encrypted ticket can never reach the magic check in
# DwAuth_OnTicketDecoded and always fails with "Auth ticket decryption error" (login status 25),
# which is exactly what 128 random bytes produced here. Writing the magic at offset 0 makes the
# client skip the slot and read the struct straight through. Not a workaround: it is the only
# ticket form this build is capable of consuming.


def build_auth_ticket(user_id: int, username: str, session_key: bytes,
                      title_id: int, license_id: int = 0,
                      lifetime_sec: int = 8 * 60 * 60) -> bytes:
    """Pack a plaintext bdAuthTicket. Returns exactly TICKET_LEN bytes.

    Packed, not aligned -- the deserializer reads u32 at offset 5 and u64 at 17, so struct
    padding here would shift every later field and silently corrupt the username and key.
    """
    if len(session_key) != SESSION_KEY_LEN:
        raise ValueError(f"session key must be {SESSION_KEY_LEN} bytes, got {len(session_key)}")

    now = int(time.time())
    name = username.encode("utf-8")[:63]

    t = struct.pack(
        "<IBIIIQQ",
        TICKET_MAGIC,    # [0]   magic
        0,               # [4]   type
        title_id,        # [5]   titleId    -> also becomes loginConfig[0]
        now,             # [9]   timeIssued
        now + lifetime_sec,   # [13] timeExpires
        license_id,      # [17]  licenseId
        user_id,         # [25]  userId     -> loginConfig+7056
    )
    assert len(t) == 33, len(t)
    t += name.ljust(64, b"\0")          # [33]  username[64]
    t += session_key                    # [97]  sessionKey[24] -> loginConfig+6920
    t += b"\0" * 3                      # [121] usingHashMagic[3]
    t += b"\0" * 4                      # [124] hashedKey[4]
    assert len(t) == TICKET_LEN, len(t)
    return t


def as_int(value, default: int) -> int:
    """Demonware stringifies its scalars, and the first real request proved it.

    The live body is `{"auth_task":"94","iv_seed":"1152714428","title_id":"5836",...}` -- every
    numeric field quoted. The previous code did `req.get("auth_task", 94) + 1` and threw
    TypeError on the very first genuine request the game ever managed to deliver, which the
    client then reported as the same `HTTP code [0]` that the TLS failure had produced. Two
    unrelated faults with one symptom is exactly why the recorder writes its entry from a
    `finally` block -- the traceback is what named this in one pass.

    Lenient on purpose: a field we have not seen before should degrade to a default and get
    recorded, not take the handler down.
    """
    try:
        return int(str(value).strip())
    except (TypeError, ValueError):
        return default


def claims_from_request(req: dict) -> dict:
    """The claims of the studio token the client sent, or {} when there is none.

    extra_data is a JSON document inside a string, and its "token" is the unsigned JWT that
    DwLogin_BuildStudioToken mints: header.payload. with the claims in the payload.
    """
    try:
        extra = json.loads(req.get("extra_data") or "{}")
        payload = str(extra.get("token", "")).split(".")[1]
        claims = json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))
        return claims if isinstance(claims, dict) else {}
    except (ValueError, IndexError, TypeError, AttributeError):
        return {}


def name_from_request(req: dict) -> str:
    """The in-game name the client put in its studio token (cw-mod.json "name"), or "" for none.

    A client built before the name was added sends no "name", and gets the --username fallback.
    """
    name = claims_from_request(req).get("name", "")
    return name.strip() if isinstance(name, str) else ""


def xuid_from_request(req: dict) -> int:
    """The player id the client put in its studio token (cw-mod.json "xuid"), or 0 for none.

    It becomes the ticket's userId, which is the XUID every other PC sees. Each PC runs its own
    backend, so a fixed --user-id gave every PC the same XUID, and a join between two of them was a
    join to itself. A client built before the claim existed sends none and gets --user-id.
    """
    value = claims_from_request(req).get("xuid", "")
    try:
        xuid = int(value, 16) if isinstance(value, str) else int(value)
    except (TypeError, ValueError):
        return 0
    return xuid if 0 < xuid < 1 << 64 else 0


def _body_field(raw: bytes) -> dict:
    """Bodies are recorded as text when they are text and base64 when they are not.

    Demonware mixes JSON with bdBuffer-style binary on the same server, and a recorder that
    assumed either one would silently mangle the other -- which would corrupt the very artifact
    we are keeping. `body_is_base64` says which happened, so nothing has to be inferred later.
    """
    if not raw:
        return {"body": "", "body_is_base64": False, "body_len": 0}
    try:
        return {"body": raw.decode("utf-8"), "body_is_base64": False, "body_len": len(raw)}
    except UnicodeDecodeError:
        return {
            "body": base64.b64encode(raw).decode("ascii"),
            "body_is_base64": True,
            "body_len": len(raw),
        }


def record(entry: dict) -> None:
    REQUESTS_PATH.parent.mkdir(parents=True, exist_ok=True)
    entry["ts"] = datetime.datetime.now().isoformat(timespec="milliseconds")
    line = json.dumps(entry, separators=(",", ":"))
    with _record_lock:
        with REQUESTS_PATH.open("a", encoding="utf-8") as f:
            f.write(line + "\n")


# THE LSG PORT IS NOT NEGOTIABLE AND NOT OURS TO PICK. Lsg_ConnectTask_BeginResolve
# (0x7FF729DF1BE0) does:
#     host = loginConfig+42768        <- our lsg_endpoint string, VERBATIM, as a bare hostname
#     port = *(lsgTask+3504)          <- the constructor default, never assigned from anything
# Lsg_ConnectTask_Ctor (0x7FF729DF1140) sets that default to 3074, and the fallback path
# (DwLoginCfg_Ctor, cfg+2090) defaults to 3074 too, so both branches converge there. Nothing in
# the auth reply can move it. Confirmed in-game 2026-07-31: we advertised "127.0.0.1:3075" and the
# client dialled 127.0.0.1:3074 anyway. Hence: lsg_endpoint carries the HOST ONLY, and an LSG
# listener has to bind 3074. Putting a ":port" back into lsg_endpoint just corrupts the hostname.
LSG_PORT = 3074


class AuthHandler(BaseHTTPRequestHandler):
    server_version = "dw-authd/0.2"
    # injected by make_handler:
    auth_priv = None
    lsg_endpoint = "127.0.0.1"
    answer_unknown = False
    reply_ints = False
    extended_data = "{}"

    def log_message(self, fmt: str, *args) -> None:
        print(f"[authd] {self.address_string()} {fmt % args}")

    def _send(self, code: int, body: bytes, headers: dict[str, str]) -> None:
        self.send_response(code)
        for k, v in headers.items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        # A HEAD reply carries the headers of the GET it stands in for but none of its body.
        # Writing one anyway desynchronises the connection: the client reads our body as the
        # status line of the NEXT response on a keep-alive socket, and the failure surfaces as a
        # corrupt unrelated request rather than as anything to do with HEAD.
        if self.command != "HEAD":
            self.wfile.write(body)

    # --- routing ---------------------------------------------------------------------------

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0") or "0")
        return self.rfile.read(length) if length else b""

    def _handle(self) -> None:
        raw = self._read_body()
        host = self.headers.get("Host", "?")
        path = self.path.split("?", 1)[0]

        entry = {
            "method": self.command,
            "host": host,
            "path": self.path,
            "headers": dict(self.headers.items()),
            **_body_field(raw),
        }

        # The record is written in `finally`, so a request survives into the artifact even when
        # answering it throws. That is not hypothetical tidiness: the endpoints worth learning about
        # are the ones we have not built yet, and a handler that dies mid-reply would otherwise
        # erase the only evidence that the client ever asked.
        try:
            if any(path.startswith(p) for p in AUTH_PREFIXES):
                self._do_auth(raw, entry)
            elif any(path.startswith(p) for p in UMBRELLA_LSG_PREFIXES):
                self._do_umbrella_lsg(raw, entry)
            else:
                self._do_unknown(entry)
        except Exception as exc:
            entry.setdefault("served", "exception")
            entry["error"] = repr(exc)
            # The repr alone cost a diagnosis cycle: two different lines can raise the same
            # TypeError, and "which line" is the entire question. Traceback goes in the artifact.
            entry["traceback"] = traceback.format_exc()
            print(f"[authd] HANDLER RAISED on {self.command} {self.path}:\n{entry['traceback']}")
            raise
        finally:
            record(entry)

    do_GET = _handle
    do_POST = _handle
    do_PUT = _handle
    do_HEAD = _handle
    do_DELETE = _handle

    # --- the one endpoint we implement -----------------------------------------------------

    def _do_auth(self, raw: bytes, entry: dict) -> None:
        print(f"[authd] {self.command} {self.path} Host={entry['host']} ({len(raw)} bytes)")
        try:
            req = json.loads(raw.decode("utf-8")) if raw else {}
        except Exception:
            req = {}

        # The reply's auth_task must equal the id the client's parser expects, which is NOT the
        # value it sent. Reversed from DwAuth_BuildRequest vs DwAuth_ParseReply: across every flow
        # the offline-branch expected reply id is exactly (request id + 1) -- 46->47, 94->95 (studio
        # auth / flow 9), 98->99, etc. The client takes the offline branch when crossplay is off
        # (which we set below). Echoing the request id instead yields "Invalid or No Task ID".
        req_task = as_int(req.get("auth_task"), 94)
        auth_task = req_task + 1
        # Echoed VERBATIM rather than parsed and re-emitted. iv_seed is also fed through the LSG
        # key derivation, so any round-trip that changes its spelling risks deriving a different
        # key than the client does -- and that failure would surface much later, at the LSG
        # handshake, looking like a crypto bug rather than a formatting one.
        iv_seed = req.get("iv_seed", "1")

        # The title id comes from the client so we do not have to hardcode 5836 and be wrong on a
        # patch; it lands in loginConfig[0] and is read back by later Demonware calls.
        title_id = as_int(req.get("title_id"), 5836)

        # ONE session key shared by both tickets. The client keeps the copy inside client_ticket
        # (loginConfig+6920) and forwards server_ticket to LSG untouched, so LSG learns the same key
        # by reading its own ticket. Two different keys here would hand each side a different key
        # and break the handshake in a way that looks like a crypto bug at Milestone 2.
        session_key = os.urandom(SESSION_KEY_LEN)

        # The ticket's username is the name the game shows after login, so the client's own
        # cw-mod.json name wins; --username only covers a client that sends none.
        client_name = name_from_request(req)
        username = client_name or self.username
        print(f"[authd] username {username!r} ({'from the client' if client_name else '--username'})")
        entry["username"] = username
        # Same rule for the id: the client's cw-mod.json "xuid" wins, --user-id covers an old client.
        client_xuid = xuid_from_request(req)
        user_id = client_xuid or self.user_id
        print(f"[authd] userId 0x{user_id:016X} ({'from the client' if client_xuid else '--user-id'})")
        entry["user_id"] = user_id

        client_ticket = build_auth_ticket(user_id, username, session_key, title_id)
        # server_ticket is stored raw at loginConfig+6792 and relayed to LSG verbatim -- the client
        # never inspects it. Same struct so our own LSG can parse it with one code path.
        server_ticket = build_auth_ticket(user_id, username, session_key, title_id)

        txn = base64.urlsafe_b64encode(os.urandom(12)).decode("ascii").rstrip("=")
        self._persist_tickets(txn, iv_seed, client_ticket, server_ticket, session_key, username, user_id)

        # STRING-TYPED BY DEFAULT, matching the client's own serializer on this same schema: it
        # quotes auth_task, iv_seed and title_id, and this file already had account_type as "0"
        # from the original reverse. That is the best evidence available for what its parser
        # expects back. It is still an inference, so --reply-ints flips the numeric fields to JSON
        # numbers and makes the alternative a restart instead of an edit.
        num = (lambda v: v) if self.reply_ints else str
        reply = {
            "auth_task": num(auth_task),
            "code": num(700),
            "iv_seed": as_int(iv_seed, 1) if self.reply_ints else str(iv_seed),
            "client_ticket": base64.b64encode(client_ticket).decode("ascii"),
            "server_ticket": base64.b64encode(server_ticket).decode("ascii"),
            "lsg_endpoint": self.lsg_endpoint,
            "crossplay_enabled": False,
            "loginqueue_enabled": False,
            "account_type": "0",
        }
        # extra_data is MANDATORY on the 700 path, and its absence is what "Auth task reply
        # contains invalid data" meant. Read off DwAuth_ParseReply (0x7FF729DF01E0) ->
        # sub_7FF729DF0AB0, which is an unguarded && chain, unlike the `if present` treatment
        # client_id and account_type get:
        #
        #     reply["extra_data"]                     string, <= 5120
        #       -> parsed as its own JSON document
        #         -> ["extended_data"]                string, <= 4097
        #           -> sub_7FF729DE9F20 -> sub_7FF729D6C8E0 stores it at loginConfig+29432+16
        #
        # The CONTENT is not validated here: that last call is a string setter that returns 1 for
        # anything up to 4096 bytes. So a minimal object gets us past auth. What the client later
        # does with the stored blob is a separate question -- and a lead worth remembering, since
        # this is account-scoped data arriving from the backend, which is the shape entitlement
        # information would take.
        #
        # Note the nesting: extra_data is a JSON-encoded STRING, not a nested object. The client's
        # own request uses the same convention.
        reply["extra_data"] = json.dumps({"extended_data": self.extended_data},
                                         separators=(",", ":"))

        body = json.dumps(reply, separators=(",", ":")).encode("utf-8")

        # Signature is over the exact bytes the client reads back as the body.
        xsig = dwsign.x_signature_header(self.auth_priv, body)
        self._send(200, body, {"Content-Type": "application/json", "X-Signature": xsig})
        print(f"[authd] -> code 700, txn {txn}, signed {len(body)}B body")

        entry["served"] = "auth"
        entry["reply_code"] = 200
        entry["reply"] = reply

    def _do_umbrella_lsg(self, raw: bytes, entry: dict) -> None:
        """Umbrella legacy login. Answering `{}` here is EVIDENCE-BACKED, not a shrug.

        The reply handler (0x7FF729DF4B90) does exactly two things on the 200 path: parse the body
        as JSON, then call sub_7FF729D46FB0, which is `return *(int *)json == 5` -- a node type tag
        meaning "the root is an object". It reads no field, not one. Then it reports status 5, "Got
        successful Umbrella Legacy Login reply". So HTTP 200 plus any JSON object is the whole
        contract, and inventing fields here would be guessing at a schema the client never reads.

        This is deliberately a NAMED ROUTE rather than --answer-unknown. The distinction is the
        point of the router: this endpoint returns 200 because its parser was read, while an
        unrecognised one still 404s so the next unknown is legible instead of silently satisfied.

        The CROSSPLAY sibling (0x7FF729DF32E0) is a different handler with a real schema --
        `hasUno`, `bannedOnWarzone`, `lsgEndpoint`, `crossPlatformProgressionEnabled`. We take the
        legacy branch because the auth reply sets crossplay_enabled=false, so none of that applies.
        If crossplay is ever turned on, this route is NOT the one that gets used.
        """
        print(f"[authd] {self.command} {self.path} Host={entry['host']} ({len(raw)} bytes)")
        body = b"{}"
        self._send(200, body, {"Content-Type": "application/json"})
        print("[authd] -> umbrella legacy login: 200 {}")
        entry["served"] = "umbrella/lsg"
        entry["reply_code"] = 200

    # --- everything else -------------------------------------------------------------------

    def _do_unknown(self, entry: dict) -> None:
        """An endpoint we have not built yet. Record it, then fail in a way that is legible.

        404 is the default deliberately. The alternative -- returning an empty 200 -- makes the
        client's parser succeed on nothing, and then whatever breaks two layers later breaks
        without reference to this request. A 404 puts the failure at the endpoint that caused it.
        `--answer-unknown` exists for the specific experiment of asking "does this endpoint even
        care about its reply", and is not the mode to leave running.
        """
        print(f"[authd] UNRECORDED ENDPOINT: {self.command} {self.path} "
              f"Host={entry['host']} ({entry['body_len']} bytes) -> "
              f"{'empty 200' if self.answer_unknown else '404'}")

        if self.answer_unknown:
            body = b"{}"
            self._send(200, body, {"Content-Type": "application/json"})
            entry["served"] = "unknown/empty-200"
            entry["reply_code"] = 200
        else:
            body = b'{"error":"not implemented by cw-mod dwserver"}'
            self._send(404, body, {"Content-Type": "application/json"})
            entry["served"] = "unknown/404"
            entry["reply_code"] = 404

    def _persist_tickets(self, txn: str, iv_seed: int, ct: bytes, st: bytes,
                         session_key: bytes, username: str, user_id: int) -> None:
        TICKETS_DIR.mkdir(parents=True, exist_ok=True)
        rec = {
            "txn": txn,
            "iv_seed": iv_seed,
            "client_ticket": base64.b64encode(ct).decode("ascii"),
            "server_ticket": base64.b64encode(st).decode("ascii"),
            # Broken out so the LSG server does not need to re-parse the ticket to get started.
            "session_key": base64.b64encode(session_key).decode("ascii"),
            "user_id": user_id,
            "username": username,
        }
        (TICKETS_DIR / "latest.json").write_text(json.dumps(rec))


class CrlHandler(BaseHTTPRequestHandler):
    """Serves the CRL over PLAIN HTTP, which is not an oversight.

    A CRL distribution point cannot be HTTPS: validating that connection would itself need a
    revocation check, which would need to fetch a CRL, which would need another connection. PKI
    always publishes CRLs over http for exactly this reason.

    This exists because the leaf now carries a CRL DP, and a DP that 404s is worse than no DP at
    all -- Schannel then has somewhere to look and still fails. Measured: without a fetchable CRL
    the handshake dies as CRYPT_E_NO_REVOCATION_CHECK and the game logs `HTTP code [0]`.
    """
    server_version = "dw-crld/0.1"
    crl_bytes = b""

    def log_message(self, fmt: str, *args) -> None:
        print(f"[crld] {self.address_string()} {fmt % args}")

    def _serve(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "application/pkix-crl")
        self.send_header("Content-Length", str(len(self.crl_bytes)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(self.crl_bytes)

    # Any path: the only thing hosted here is the CRL, and answering it regardless of the path
    # means a URL typo in the cert cannot produce the silent handshake failure this fixes.
    do_GET = _serve
    do_HEAD = _serve


def serve_crl(port: int, crl_path: pathlib.Path) -> ThreadingHTTPServer | None:
    if not crl_path.exists():
        print(f"[crld] no {crl_path.name} -- run reissue_cert.py (or gen_keys.py) first. "
              f"TLS handshakes WILL fail with CRYPT_E_NO_REVOCATION_CHECK until it exists.")
        return None
    handler = type("BoundCrlHandler", (CrlHandler,), {"crl_bytes": crl_path.read_bytes()})
    try:
        httpd = ExclusiveHTTPServer(("127.0.0.1", port), handler)
    except OSError as exc:
        print(f"[crld] could not bind 127.0.0.1:{port} ({exc}). Port 80 needs admin, and the "
              f"handshake will fail without it.")
        return None
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    print(f"[crld] serving {crl_path.name} on http://127.0.0.1:{port}/ ({len(handler.crl_bytes)}B)")
    return httpd


class ExclusiveHTTPServer(ThreadingHTTPServer):
    """ThreadingHTTPServer that REFUSES to share its port. This is not a micro-optimisation.

    ThreadingHTTPServer sets allow_reuse_address = 1, and on Windows SO_REUSEADDR does not mean
    what it means on POSIX: a second process is allowed to bind a port another process is already
    listening on, and connections are then split between them unpredictably.

    That cost a full test cycle. Two servers were live on 443 -- one started earlier with
    string-typed replies, one started later with --reply-ints -- and the game's requests landed on
    the older one. The observable result was a flag that appeared to do nothing, which reads
    exactly like "we tested the alternative and it made no difference". It had never run.

    Binding exclusively turns that silent shadowing into an immediate, obvious bind error.
    """
    allow_reuse_address = 0


def make_handler(auth_priv, lsg_endpoint: str, answer_unknown: bool = False,
                 reply_ints: bool = False, extended_data: str = "{}",
                 user_id: int = 1, username: str = "cwmod"):
    return type("BoundAuthHandler", (AuthHandler,), {
        "auth_priv": auth_priv,
        "lsg_endpoint": lsg_endpoint,
        "answer_unknown": answer_unknown,
        "reply_ints": reply_ints,
        "extended_data": extended_data,
        "user_id": user_id,
        "username": username,
    })


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=443)
    ap.add_argument("--priv", default=str(HERE / "material" / "auth_priv.pem"))
    ap.add_argument("--cert", default=str(HERE / "material" / "server_cert.pem"))
    ap.add_argument("--key", default=str(HERE / "material" / "server_key.pem"))
    ap.add_argument("--lsg-endpoint", default="127.0.0.1",
                    help=f"HOST of the LSG listener -- bare hostname, no port. The client hardcodes "
                         f"port {LSG_PORT} (see the LSG_PORT note); a ':port' suffix here is parsed "
                         f"as part of the hostname and does not change the port it dials.")
    ap.add_argument("--crl", default=str(HERE / "material" / "cwmod.crl"))
    ap.add_argument("--crl-port", type=int, default=80,
                    help="plain-HTTP port for the CRL named in the leaf's distribution point")
    ap.add_argument("--no-crl", action="store_true",
                    help="do not serve the CRL. Only useful against a client that skips revocation "
                         "checking -- the game does not.")
    ap.add_argument("--extended-data", default="{}",
                    help="payload for extra_data.extended_data. The parser accepts any string up "
                         "to 4096 bytes and stores it in the login config; this exists so that "
                         "blob can be experimented with without an edit.")
    ap.add_argument("--reply-ints", action="store_true",
                    help="emit auth_task/code/iv_seed as JSON numbers instead of strings. The "
                         "client sends them as strings, so strings is the default; use this if "
                         "the parser rejects the reply anyway.")
    ap.add_argument("--answer-unknown", action="store_true",
                    help="reply to unimplemented endpoints with an empty 200 instead of 404. "
                         "A diagnostic for 'does this endpoint care about its reply' -- not a "
                         "mode to leave on, because it hides which request caused a later failure.")
    ap.add_argument("--user-id", type=int, default=1,
                    help="userId baked into the auth ticket when the client sends no \"xuid\" claim; "
                         "lands in loginConfig+7056 and is the identity every later Demonware call "
                         "is scoped to. A current client sends its cw-mod.json xuid instead, which "
                         "is unique per PC; this fallback is the same for every PC.")
    ap.add_argument("--username", default="cwmod",
                    help="username baked into the auth ticket (max 63 bytes) when the client's "
                         "token carries no name. The client sends its cw-mod.json \"name\".")
    args = ap.parse_args()

    auth_priv = dwsign.load_priv(args.priv)
    handler = make_handler(auth_priv, args.lsg_endpoint, args.answer_unknown, args.reply_ints,
                           args.extended_data, args.user_id, args.username)

    # Must be up BEFORE the first TLS handshake: Schannel fetches the CRL during chain validation,
    # so a listener started afterwards is a listener started too late.
    if not args.no_crl:
        serve_crl(args.crl_port, pathlib.Path(args.crl))

    httpd = ExclusiveHTTPServer((args.host, args.port), handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=args.cert, keyfile=args.key)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    print(f"[authd] listening on https://{args.host}:{args.port}/auth/  lsg={args.lsg_endpoint}")
    print(f"[authd] recording every request to {REQUESTS_PATH}")
    if args.answer_unknown:
        print("[authd] WARNING: unknown endpoints answer empty 200 -- diagnostic mode")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[authd] shutting down")
        httpd.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
