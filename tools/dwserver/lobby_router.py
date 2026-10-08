#!/usr/bin/env python3
"""The bdLobby service router -- what turns one hard-coded answer into a server.

Milestone 2 needed exactly ONE reply: the client sent a single 0x86 record (bdAntiCheat's
reportExtendedAuthInfo) and owed a single inner-tag-1 answer. lsgserver.py therefore answered every
record it received with the same reply built from command-line knobs. That is correct for one
request and wrong for two, because past login the client talks to many services and each one wants
its own answer.

This module is the dispatch layer: parse a request into (serviceId, taskId, params), look up a
handler, and build the tag-1 reply. Everything unhandled still gets answered -- see THE RULE below --
and every request is journalled, so a boot produces a census instead of a shrug.

--------------------------------------------------------------------------------------------------
THE REQUEST GRAMMAR
--------------------------------------------------------------------------------------------------
Every outbound lobby message gets its header from BdLobbyMsg_WriteHeader (0x7FF729D98EC0), which is
two writes and nothing else:

    bdByteBuffer_WriteRaw(buf, &serviceId, 1)   -> ONE RAW BYTE, no type tag
    bdByteBuffer_WriteUChar8(buf, taskId)       -> a TYPE-TAGGED UChar8 (03 <id>)

so a client->server 0x86 body is

    [raw serviceId][03 taskId][ ...the task's own fields... ][00 NoType]            plain task
    [raw serviceId][03 taskId][17 08 <u32 len>][len bytes of protobuf][00]           StructData task

CORRECTED 2026-09-24. Until then this module called the raw byte the "message type" and the UChar8
the "service id", and held the UInt32 after it "on probation" as the task id. That was backwards.
CodRevamped's IW8 router (docs/codrevamped-notes.md) decodes the same two bytes as (service, task),
and re-read that way the whole census lines up pair for pair with the IW8 and T8 service tables:
(38, 8) bdAntiCheat, (12, 6) bdTitleUtilities.getServerTime, (27, 3) bdDML, (95, 3)
bdPublisherVariables.retrievePublisherVariables, (104, 6) bdMarketingComms.getMessages, (125, 9)
bdAchievementsEngine.getUserState, (255, 10) REST. Six exact pairs is not chance. It also explains the
StructData shape "having no task id": the UChar8 always was the task id. The UInt32 "4" in the login
request is the task's first parameter. Nothing on the wire changed; only our labels did. Old census
rows (no "schema" key) carry the old labels: their msg_type is the service and their service_id is
the task.

Names taken from the IW8/T8 tables carry a trailing "?" until a T9 boot or the IDB confirms them:
task numbers drift between titles (IW8's bdAntiCheat has no task 8).

StructData replies. The task class reads its reply in BdStructTask_ReadStructDataReply
(0x7FF729DCCE20): after the normal envelope (UInt64 handle, UInt32 error, UChar8) it REQUIRES a
StructData (17 08 <len>) and hands those bytes to the result's protobuf parser. The generic
`08 00000000` result fails that type check, which is why service 95 retried four times on the first
boot. An empty StructData (len 0) is a valid protobuf message with every field defaulted --
well-formed, and the honest minimum until the result schema is known.

--------------------------------------------------------------------------------------------------
THE RULE: ANSWER EVERY REQUEST, EXACTLY ONCE, IN ORDER
--------------------------------------------------------------------------------------------------
BdLobby_OnServiceReply_Tag1 (0x7FF729DD8090) pops its pending-request queue (this+112)
UNCONDITIONALLY, before it reads a single byte of the reply. Nothing on the wire identifies which
request a reply belongs to -- not even the handle field, which is read only after the pop.

The consequence is a hard protocol constraint, and it is the opposite of the authserver's rule:
authserver.py deliberately 404s an endpoint it does not implement, because an unanswered HTTP
request is a clean signal. Here, skipping a reply does not produce a clean signal -- it silently
shifts every later reply onto the wrong request and the resulting errors describe requests we
answered correctly. So an unknown service gets the default success rather than silence, and the
loud part happens in the log, not on the wire.

`--unknown-error N` exists for the deliberate experiment of asking whether a specific unhandled
request mattered; it is not a mode to leave on, for the same reason --answer-unknown is not.
"""
from __future__ import annotations

import json
import os
import pathlib
import re
import threading
import time
from dataclasses import dataclass, field
from typing import Callable

import bdbuf

HERE = pathlib.Path(__file__).resolve().parent
MATERIAL = HERE / "material"
CENSUS_PATH = MATERIAL / "lobby_requests.jsonl"
CENSUS_SCHEMA = 2           # 2 = (service, task) labels; rows without the key use the old labels

# bdAntiCheat: the service byte of the only request login needs. Proven: its builder is
# AntiCheat_StartReportExtendedAuthInfo (0x7FF729DF2420), and 0x26 is bdAntiCheat in IW8 and T8.
SERVICE_ANTICHEAT = 38

# Service ids. A plain name is proven on T9; a trailing "?" means "from the IW8/T8 tables, matching
# our census pair, not yet confirmed here". An unknown id prints as its number.
SERVICE_NAMES: dict[int, str] = {
    SERVICE_ANTICHEAT: "bdAntiCheat",
    8: "bdProfiles?",
    12: "bdTitleUtilities?",
    27: "bdDML?",
    95: "bdPublisherVariables?",
    104: "bdMarketingComms?",
    125: "bdAchievementsEngine?",
}

# (serviceId, taskId) -> name, same rule.
TASK_NAMES: dict[tuple[int, int], str] = {
    (SERVICE_ANTICHEAT, 8): "bdAntiCheat::reportExtendedAuthInfo",
    (8, 1): "bdProfiles::getPublicInfos?",
    (8, 3): "bdProfiles::setPublicInfo?",
    (12, 6): "bdTitleUtilities::getServerTime?",
    (27, 3): "bdDML::getUserHierarchicalData?",
    (95, 3): "bdPublisherVariables::retrievePublisherVariables?",
    (104, 6): "bdMarketingComms::getMessages?",
    (125, 3): "bdAchievementsEngine / task 3",
    (125, 9): "bdAchievementsEngine::getUserState?",
}


def service_name(service_id: int) -> str:
    return SERVICE_NAMES.get(service_id, f"service {service_id}")


def task_name(service_id: int, task_id: int | None) -> str:
    if task_id is None:
        return f"{service_name(service_id)} / task ?"
    return TASK_NAMES.get((service_id, task_id),
                          f"{service_name(service_id)} / task {task_id}")


# --------------------------------------------------------------------------------------------------
# Request
# --------------------------------------------------------------------------------------------------
@dataclass
class LobbyRequest:
    """One decoded client->server 0x86 lobby payload."""

    body: bytes
    service_id: int | None          # the raw leading byte; None if the body was empty
    task_id: int | None             # first TLV (UChar8). None if it was not a UChar8.
    fields: list[dict]              # the full TLV walk, service byte excluded, terminator included
    params: list[dict]              # fields after the task id, terminator stripped
    walk_error: str | None          # None only when the walk consumed the buffer exactly
    struct_data: bool = False       # first param is StructData (23, 0x17) => the other message shape
    struct_payload: bytes | None = None   # its protobuf bytes, when struct_data
    terminated: bool = False        # the trailing NoType marker was present

    @property
    def key(self) -> tuple[int | None, int | None]:
        return (self.service_id, self.task_id)

    def describe(self) -> str:
        if self.service_id is None or self.task_id is None:
            return f"UNPARSED request ({len(self.body)} bytes)"
        bits = [task_name(self.service_id, self.task_id)]
        if self.struct_data:
            bits.append("[StructData]")
        if self.walk_error:
            bits.append(f"[walk stopped: {self.walk_error}]")
        return "  ".join(bits)


def parse_request(body: bytes) -> LobbyRequest:
    """Decode a 0x86 body. NEVER raises: an unparsable request still owes a reply (THE RULE)."""
    if not body:
        return LobbyRequest(body=body, service_id=None, task_id=None,
                            fields=[], params=[], walk_error="empty body")

    fields, err = bdbuf.walk(body, bdbuf.REQUEST_HEADER_LEN)

    task_id = None
    struct_data = False
    struct_payload = None
    params = fields
    if fields and fields[0]["type"] == bdbuf.T_UCHAR8:
        task_id = fields[0]["value"]
        params = fields[1:]
        if params and params[0]["type"] == bdbuf.T_STRUCTDATA:   # BdLobbyMsg_WriteStructDataPayload
            struct_data = True
            struct_payload = bytes.fromhex(params[0]["value"])

    # The trailing NoType is a terminator, not a value -- the client writes one at the end of every
    # payload it sends. Kept in `fields` (which is the faithful record of the wire) and stripped
    # from `params` (which is what a handler reads), so nobody indexes params[-1] and gets an end
    # marker where a field was expected.
    terminated = bool(params) and params[-1]["type"] == bdbuf.T_NOTYPE
    if terminated:
        params = params[:-1]

    return LobbyRequest(body=body, service_id=body[0], task_id=task_id, fields=fields, params=params,
                        walk_error=err, struct_data=struct_data, struct_payload=struct_payload,
                        terminated=terminated)


# --------------------------------------------------------------------------------------------------
# Reply
# --------------------------------------------------------------------------------------------------
@dataclass
class Reply:
    """An inner-tag-1 service reply.

    The defaults are not a neutral choice -- they are the exact reply that reached
    `[status 27] Login Complete` on 2026-08-02, byte for byte:

        0a 0000000000000000   UInt64 handle   = 0
        08 00000000           UInt32 error    = 0
        03 00                 UChar8 flag     = 0
        08 00000000           UInt32          = 0   <- the result-row count
        00                    NoType

    That trailing UInt32 matters and is easy to drop by accident. errorCode 0 sends the client down
    the success branch, where the remainder of the buffer goes to the task's own result deserializer
    (BdLobbyTask_OnReplyPayload_SetErr4, 0x7FF729D6B930); a deserializer reading a typed UInt32 out
    of an exhausted buffer returns false, and the client reports the hardcoded error 4. A reply that
    ends after the flag is well-formed at the envelope level and still fails the task.

    The UChar8 `flag`: IW8's server echoes the task id there. We send 0 and every reply so far has
    been accepted, so it stays 0 until a boot shows it matters.
    """

    handle: int = 0
    error_code: int = 0
    flag: int = 0
    results: bytes = b""
    send: bool = True               # False = deliberate silence; desyncs the queue, see THE RULE

    def to_bytes(self) -> bytes:
        return bdbuf.build_service_reply(handle=self.handle, error_code=self.error_code,
                                         flag=self.flag, results=self.results)

    def describe(self) -> str:
        if not self.send:
            return "SILENT (no reply -- desyncs the pending queue)"
        if self.error_code:
            return f"errorCode={self.error_code}"
        return (f"errorCode=0 flag={self.flag} handle={self.handle} "
                f"results={len(self.results)}B")


def empty_success(handle: int = 0) -> Reply:
    """errorCode 0 with zero result rows -- the proven-good shape."""
    return Reply(handle=handle, error_code=0, flag=0, results=bdbuf.w_u32(0))


def rows_success(*rows: bytes) -> Reply:
    """errorCode 0 with `UInt32 count, UInt32 total, rows...` -- the generic task row list."""
    return Reply(results=bdbuf.w_u32(len(rows)) + bdbuf.w_u32(len(rows)) + b"".join(rows))


def struct_success(payload: bytes = b"", handle: int = 0) -> Reply:
    """errorCode 0 with a StructData result (17 08 <len> <protobuf>) -- see StructData replies."""
    return Reply(handle=handle, error_code=0, flag=0, results=bdbuf.w_structdata(payload))


# --------------------------------------------------------------------------------------------------
# Connection state
# --------------------------------------------------------------------------------------------------
@dataclass
class Session:
    """Per-connection state a handler may read or extend. One LSG connection, one Session."""

    peer: str = ""
    seq: int = 0                                    # requests seen on this connection
    state: dict = field(default_factory=dict)       # handler scratch space


# --------------------------------------------------------------------------------------------------
# Registry
# --------------------------------------------------------------------------------------------------
Handler = Callable[[LobbyRequest, Session], Reply]

_BY_TASK: dict[tuple[int, int], Handler] = {}
_BY_SERVICE: dict[int, Handler] = {}

# Set by lsgserver's --unknown-error, for the one experiment described in THE RULE.
unknown_error: int = 0


def handler(service_id: int, task_id: int | None = None) -> Callable[[Handler], Handler]:
    """Register a handler for one task, or for a whole service when task_id is None."""

    def register(fn: Handler) -> Handler:
        if task_id is None:
            _BY_SERVICE[service_id] = fn
        else:
            _BY_TASK[(service_id, task_id)] = fn
        return fn

    return register


def lookup(req: LobbyRequest) -> tuple[Handler | None, str]:
    """Most specific first: exact task, then service-wide, then nothing."""
    if req.service_id is None or req.task_id is None:
        return None, "unparsed"
    fn = _BY_TASK.get((req.service_id, req.task_id))
    if fn is not None:
        return fn, "task"
    fn = _BY_SERVICE.get(req.service_id)
    if fn is not None:
        return fn, "service"
    return None, "none"


def dispatch(req: LobbyRequest, session: Session) -> tuple[Reply, str]:
    """Return (reply, how) where `how` names which registration answered: task|service|default."""
    fn, how = lookup(req)
    if fn is not None:
        return fn(req, session), how
    if unknown_error:
        return Reply(error_code=unknown_error), "default"
    if req.struct_data:
        # The result must be a StructData or the task fails its type check; empty = all-default.
        return struct_success(), "default"
    return empty_success(), "default"


# --------------------------------------------------------------------------------------------------
# Handlers
# --------------------------------------------------------------------------------------------------
@handler(SERVICE_ANTICHEAT, 8)
def anticheat_report_extended_auth_info(req: LobbyRequest, session: Session) -> Reply:
    """bdAntiCheat::reportExtendedAuthInfo -- the login gate, and the first PROVEN answer we owned.

    Registered explicitly even though it returns exactly what the default returns, so that the one
    reply with a boot behind it is a named thing rather than a coincidence of the fallback. If the
    default ever changes, this stays pinned to what worked.

    The request's own fields, from the 62-byte capture: UInt32 4, UInt32 clientVersion(378), three
    UInt64 (all zero), Blob(6) = a MAC address, String = the `extra_data` JSON ("{}"), Int32 0.
    """
    return empty_success()


# --------------------------------------------------------------------------------------------------
# Service 8 / task 1: the per-user blob fetch that flooded once the content slots reached 2,2
# --------------------------------------------------------------------------------------------------
# (Labelled "service 1 / msgType 8" before the 2026-09-24 relabel. T8 calls (8, 1)
# bdProfiles.getPublicInfos and (8, 3) setPublicInfo, which fits what the client does below.)
#
# Sent from LiveUser_UpdateSigninState -> 0x7FF71E74C420 with one UInt64 = 1, only once
# OnlineContent_AllSlotsLoaded. Boot 2026-09-16 23:20: 2129 requests in 40 s (one per frame).
#
# Why it looped: the success handler (0x7FF71E74AD40) returns at once when the task has ZERO result
# rows, before it sets the per-controller "fetched" byte (0x7FF72AA45C32 via 0x7FF71E74AC20). The
# frame check (0x7FF71E74AB50) then sees "not in flight, never fetched" and resends. The failure
# handler (0x7FF71E74B050) treats errorCode 800 -- not found -- as "no blob yet": it resets the blob
# at LiveUser+40064 to defaults, sets "fetched", and posts the changed event. We store no such blob,
# so 800 is the honest answer. PASSED 2026-09-17 19:19: one request per boot, then the client writes
# its default blob back with (8, 3) twice.
BD_ERROR_NOT_FOUND = 800


@handler(8, 1)
def profiles_get_public_infos(req: LobbyRequest, session: Session) -> Reply:
    return Reply(error_code=BD_ERROR_NOT_FOUND)


# --------------------------------------------------------------------------------------------------
# Service 12 / task 6: getServerTime
# --------------------------------------------------------------------------------------------------
# Empty request. Until 2026-09-24 it got the zero-row default, so the client had no server time.
# Reply shape from CodRevamped's IW8 server, which is the generic task row list with one bdTimeStamp
# row (UInt32 seconds): count 1, total 1, UInt32 now. Untested on T9: if the census shows (12, 6)
# retrying, the row shape is wrong and the default comes back.
@handler(12, 6)
def title_utilities_get_server_time(req: LobbyRequest, session: Session) -> Reply:
    return rows_success(bdbuf.w_u32(int(time.time()) & 0xFFFFFFFF))


# --------------------------------------------------------------------------------------------------
# Service 255 / task 10: HTTP tunnelled through the lobby (objectstore, umbrella, presence)
# --------------------------------------------------------------------------------------------------
# (Labelled "service 10 / task 1" before the 2026-09-24 relabel; the UInt32 1 is the first param.)
# REQUEST (BdRemoteHttp_SendLobbyRequest_Svc10, 0x7FF729DD17C0):
#   UInt32 1
#   Blob  protobuf {1: {.., 300: resource, 301: method}, 2: 1, 10: .., 11: url, 101: service, 102: 1}
#   Bool  hasBody,  Blob body
#
# REPLY. The task's generic row reader (0x7FF729D6B4F0) reads UInt32 count, UInt32 total, then one row
# per pending HTTP entry. The row is BdRemoteHttp_ReadResponse (0x7FF729DC8C50):
#   UInt32 1                      version, must be 1
#   Blob   header protobuf        BdRemoteHttp_ReadResponseHeader (0x7FF729DC8390), forward-only:
#            1   varint  HTTP status (anything unknown maps to 500; 2xx = success)
#            203 double  (timing)          302 varint content type, 1 = JSON (body parsed only then)
#            400 varint  bool              1000 optional group (retry/redirect), omitted
#   Bool   hasBody
#   Blob   body                   the JSON; the client NUL-terminates it in place, so a byte (our
#                                 NoType) must follow it
#
# PublisherObjectsResource JSON (parser 0x7FF729D90170, per-object 0x7FF729D70BD0):
#   {"objects": [{"metadata": {...}}], "nextPageToken"?}
# REQUIRED metadata keys, string lengths are the client's buffer sizes (value must be shorter):
#   checksum <33 (base64 MD5, becomes the manifest hash)   objectVersion <33 (manifest uuid)
#   expiresOn/created/modified (number or numeric string)  acl "public"|"private"
#   contentLength (number)  context <16  name <65  owner <30  category <65 or null
# optional: contentURL <512, summaryContentURL, extraData(+Size), hasSummary, description, owners, objectID
#
# What the game does with it (Lpc_SyncFrame 0x7FF727B36860): Lpc_WriteManifest builds .manifest from
# this list, then every entry's local file under ProgramData\...\LPC is MD5-checked; all match ->
# Lpc_IsManifestLoaded, which the content slots (core_playlists / core_ffotd) wait on. A mismatch goes
# to download via contentURL, which this server does not serve yet.
SERVICE_REST = 255
SERVICE_NAMES[SERVICE_REST] = "bdRESTLegacy (tunnelled HTTP)"
TASK_NAMES[(SERVICE_REST, 10)] = "bdRESTLegacy::request"

HTTP_CONTENT_JSON = 1


def lpc_dir() -> pathlib.Path:
    """The game's own LPC folder -- the files never live in the repo (legal guardrail)."""
    override = os.environ.get("CWMOD_LPC_DIR")
    if override:
        return pathlib.Path(override)
    return (pathlib.Path(os.environ.get("ProgramData", r"C:\ProgramData"))
            / "Activision" / "Call Of Duty Black Ops Cold War" / "LPC")


def http_reply(status: int, body: bytes | None, content_type: int = HTTP_CONTENT_JSON) -> Reply:
    header = (bdbuf.pb_uint(1, status) + bdbuf.pb_double(203, 0.0)
              + bdbuf.pb_uint(302, content_type) + bdbuf.pb_uint(400, 0))
    row = bdbuf.w_u32(1) + bdbuf.w_blob(header) + bdbuf.w_bool(body is not None) \
        + bdbuf.w_blob(body or b"")
    return rows_success(row)


def http_request_url(req: LobbyRequest) -> str | None:
    """Field 11 of the first Blob param (the request protobuf)."""
    blob = next((p for p in req.params if p["type"] == bdbuf.T_BLOB), None)
    if blob is None:
        return None
    fields, _ = bdbuf.pb_decode(bytes.fromhex(blob["value"]))
    for f in fields:
        if f["field"] == 11 and isinstance(f["value"], str):
            return f["value"]
    return None


def lpc_excluded() -> tuple[str, ...]:
    """Name fragments left out of the LPC list. Default "ffotd": the files we have are TU35 renamed to
    tu34, and on boot 2026-09-16 23:10 the list loaded and the game ERR_DROPped one second later inside
    the fastfile loader's asset-entry allocator (DB_AllocXAssetEntry, 0x7FF727EC1310), which is what a
    TU35 ffotd zone in a TU34 exe would do. The playlists are the files the content slots need.
    CWMOD_LPC_EXCLUDE="" serves everything again."""
    raw = os.environ.get("CWMOD_LPC_EXCLUDE", "ffotd")
    return tuple(p for p in raw.split(",") if p)


def publisher_objects(category: str, context: str, directory: pathlib.Path) -> list[dict]:
    """One metadata object per LPC file whose name carries `category` (tu<N>_<buildId>)."""
    import base64
    import hashlib
    objects = []
    # Default: an EMPTY list. Boots 2026-09-16 23:10 and 23:14 proved the TU35 files we hold (renamed
    # tu34) cannot load in this TU34 exe: with the full set, and again with only the two 28 KB
    # playlists, the zone loader ERR_DROPped ("Uniform 58 Guerrilla Boa") in DB_AllocXAssetEntry's
    # empty-pool check one second after LPC loaded. An empty list still writes a 0-entry .manifest,
    # and Lpc_VerifyNextFile then sets Lpc_IsManifestLoaded with nothing to load.
    # CWMOD_LPC_FILES=1 serves the folder's files again (minus lpc_excluded()).
    if os.environ.get("CWMOD_LPC_FILES") != "1" or not directory.is_dir():
        return objects
    tu, _, build_id = category.partition("_")
    for path in sorted(directory.glob(f"*_{tu}_100_{build_id}.ff")):
        if any(part in path.name for part in lpc_excluded()):
            continue
        data = path.read_bytes()
        md5 = hashlib.md5(data)
        objects.append({"metadata": {
            "name": path.name,
            "owner": "treyarch",
            "checksum": base64.b64encode(md5.digest()).decode(),
            "objectVersion": md5.hexdigest(),
            "expiresOn": 0,
            "created": int(path.stat().st_mtime),
            "modified": int(path.stat().st_mtime),
            "acl": "public",
            "contentLength": len(data),
            "context": context,
            "category": category,
            "contentURL": f"https://objectstore.prod.demonware.net/lpc/{path.name}",
        }})
    return objects


def object_list(objects: list[dict]) -> Reply:
    # nextPageToken is REQUIRED by the category-list parser (0x7FF729D8B130): it succeeds only when the
    # key is null or a string, so omitting it fails the whole list after every object parsed (boot
    # 2026-09-16 23:06, errorCode 4, 15 s retry). null = last page.
    return http_reply(200, json.dumps({"objects": objects, "nextPageToken": None}).encode())


# A user's own objectstore listing, e.g. /v2/core/users/uno-1/objects/?context=5836&limit=30.
USER_OBJECTS_PATH = re.compile(r"^/v2/core/users/[^/]+/objects/?$")


@handler(SERVICE_REST, 10)
def rest_request(req: LobbyRequest, session: Session) -> Reply:
    """Object lists get a real (possibly empty) list; everything else keeps the zero-row default.

    Before 2026-09-24 only the tu* publisher list was answered and every other URL got zero rows, i.e.
    no HTTP response at all. An empty list is what a real objectstore returns for a category or a user
    with no objects, so the two list shapes the client asks for now get that. Per-object metadata,
    umbrella /v1.0/regulations/ and the presence PUT still get zero rows: their success schema is
    unread, and a wrong 200 is worse than no answer.
    """
    from urllib.parse import parse_qs, urlsplit
    url = http_request_url(req)
    if not url:
        return empty_success()
    parts = urlsplit(url)
    query = {k: v[0] for k, v in parse_qs(parts.query, keep_blank_values=True).items()}

    if parts.path.startswith("/v2/core/publishers/treyarch/objects"):
        category = query.get("category", "")
        if category.startswith("tu"):
            objects = publisher_objects(category, query.get("context", ""), lpc_dir())
            names = [o["metadata"]["name"] for o in objects]
            print(f"[router] objectstore LPC list category={category}: {len(objects)} file(s) {names}")
            return object_list(objects)
        print(f"[router] objectstore publisher list category={category or '<none>'}: empty")
        return object_list([])

    if USER_OBJECTS_PATH.match(parts.path):
        print(f"[router] objectstore user list {parts.path}: empty")
        return object_list([])

    print(f"[router] REST {url}: no handler, zero rows")
    return empty_success()


# --------------------------------------------------------------------------------------------------
# Census
# --------------------------------------------------------------------------------------------------
_census_lock = threading.Lock()


def journal(req: LobbyRequest, reply: Reply, how: str, session: Session) -> None:
    """One JSON object per request. This file is the M3 equivalent of requests.jsonl.

    Written per line and flushed, because the boots that matter most are the ones that die.
    """
    entry = {
        "schema": CENSUS_SCHEMA,
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "peer": session.peer,
        "seq": session.seq,
        "service_id": req.service_id,
        "task_id": req.task_id,
        "name": task_name(req.service_id, req.task_id) if req.service_id is not None else None,
        "handled_by": how,
        "struct_data": req.struct_data,
        "walk_error": req.walk_error,
        "fields": req.fields,
        "request_body": req.body.hex(),
        "reply": {
            "sent": reply.send,
            "handle": reply.handle,
            "error_code": reply.error_code,
            "flag": reply.flag,
            "body": reply.to_bytes().hex() if reply.send else None,
        },
    }
    try:
        MATERIAL.mkdir(parents=True, exist_ok=True)
        with _census_lock, CENSUS_PATH.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(entry) + "\n")
            fh.flush()
    except OSError as exc:                      # never let journalling kill a live session
        print(f"[router] census write failed: {exc}")


def registered() -> list[str]:
    """For the startup banner: what this server actually claims to implement."""
    out = [f"{task_name(s, t)}  (task)" for (s, t) in sorted(_BY_TASK)]
    out += [f"{service_name(s)}  (service-wide)" for s in sorted(_BY_SERVICE)]
    return out
