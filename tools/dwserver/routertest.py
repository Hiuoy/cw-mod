#!/usr/bin/env python3
"""Offline proof for lobby_router -- pinned to the REAL captured login exchange, no game needed.

The anchor is the 2026-08-02 session that reached `[status 27] Login Complete`: its request body and
the reply we sent are both in material/lsg_frames.jsonl, and both are reproduced here verbatim. The
point of this file is that the router cannot quietly change what the login path puts on the wire --
if a refactor moves a byte, this fails before a boot does.

Run:  python routertest.py      (expect: ALL PASS)
"""
from __future__ import annotations

import sys

import bdbuf
import lobby_router

# --------------------------------------------------------------------------------------------------
# Ground truth, copied out of material/lsg_frames.jsonl (peer 127.0.0.1:27977, 2026-08-02T06:46).
# --------------------------------------------------------------------------------------------------
# The client's 0x86 body: bdAntiCheat::reportExtendedAuthInfo, 62 bytes.
REQUEST_HEX = (
    "26"                    # raw serviceId 38 = bdAntiCheat, untagged (BdLobbyMsg_WriteHeader's WriteRaw)
    "0308"                  # UChar8 taskId = 8 (reportExtendedAuthInfo)
    "0804000000"            # UInt32 4         = first param
    "087a010000"            # UInt32 378       = client version, matches the /auth/ request body
    "0a0000000000000000"    # UInt64 0
    "0a0000000000000000"    # UInt64 0
    "0a0000000000000000"    # UInt64 0
    "130806000000bb442e6d5002"  # Blob(6) = a MAC address (nested typed length: 13 08 <u32>)
    "107b7d00"              # String "{}" = extra_data, NUL-terminated
    "0700000000"            # Int32 0
    "00"                    # NoType terminator
)

# The reply that boot sent, which the client accepted.
REPLY_HEX = (
    "0a0000000000000000"    # UInt64 handle = 0
    "0800000000"            # UInt32 errorCode = 0
    "0300"                  # UChar8 flag = 0
    "0800000000"            # UInt32 0 -- the result-row count the task deserializer reads
    "00"                    # NoType
)

# First post-login census, 2026-09-16 17:52 (seq 3/7/8/9, identical). BdLobbyMsg_WriteStructDataPayload.
STRUCT_REQUEST_HEX = (
    "5f"                    # raw serviceId 95 (bdPublisherVariables?)
    "0303"                  # UChar8 taskId = 3
    "170828000000"          # StructData, typed UInt32 length 40
    "0a00"                  # pb field 1 = ""
    "1206636c69656e74"      # pb field 2 = "client"
    "1204626e6574"          # pb field 2 = "bnet"
    "120b636c69656e745f74753334"  # pb field 2 = "client_tu34"
    "1209626e65745f74753334"      # pb field 2 = "bnet_tu34"
    "00"                    # NoType terminator
)

# service 255 / task 10: PublisherObjectsResource LPC list, census 2026-09-16 18:23.
LPC_LIST_REQUEST_HEX = (
    "ff030a08010000001308d00100000ab9828000080010001800a006b0ea01c20c8002"
    + "00" * 256 +
    "e212185075626c69736865724f626a656374735265736f75726365ea120c6765745f6d65746164617461100150d48a9d025a782f76322f636f72652f7075626c6973686572732f74726579617263682f6f626a656374732f3f636c69656e743d26636f6e746578743d353833365f626e65742663617465676f72793d747533345f33343063383366336233333334386635266c696d69743d35302665787472613d636f6e74656e7455524caa060b6f626a65637473746f7265b00601010013080000000000"
)

PASS = 0
FAIL = 0


def check(name: str, ok: bool, detail: str = "") -> None:
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}" + (f"\n        {detail}" if detail else ""))


def main() -> int:
    body = bytes.fromhex(REQUEST_HEX)

    print("parse the real login request")
    req = lobby_router.parse_request(body)
    check("body is the captured 62 bytes", len(body) == 62, f"got {len(body)}")
    check("serviceId is 0x26 = 38 (bdAntiCheat)", req.service_id == lobby_router.SERVICE_ANTICHEAT,
          f"got {req.service_id!r}")
    check("taskId is 8", req.task_id == 8, f"got {req.task_id!r}")
    check("first param is UInt32 4", req.params[0]["type"] == bdbuf.T_UINT32 and req.params[0]["value"] == 4,
          f"got {req.params[0]!r}")
    check("not the StructData message shape", req.struct_data is False)
    check("walk consumed the buffer exactly", req.walk_error is None, f"got {req.walk_error!r}")
    check("10 fields after the header byte (terminator included)",
          len(req.fields) == 10, f"got {len(req.fields)}")
    check("params exclude taskId and the terminator",
          len(req.params) == 8, f"got {len(req.params)}")
    check("terminator seen", req.terminated is True)
    check("no NoType leaks into params",
          all(f["type"] != bdbuf.T_NOTYPE for f in req.params), f"got {req.params!r}")
    check("second param is the client version 378",
          req.params[1]["value"] == 378, f"got {req.params[1]!r}")
    check("extra_data param is the JSON '{}'",
          any(f["value"] == "{}" for f in req.params), f"got {req.params!r}")
    check("names resolve", lobby_router.task_name(38, 8) == "bdAntiCheat::reportExtendedAuthInfo")

    print("dispatch reproduces the reply that reached Login Complete")
    session = lobby_router.Session(peer="test")
    reply, how = lobby_router.dispatch(req, session)
    check("a registered handler answered, not the fallback", how == "task", f"got {how!r}")
    check("reply is byte-for-byte the accepted one", reply.to_bytes().hex() == REPLY_HEX,
          f"got  {reply.to_bytes().hex()}\n        want {REPLY_HEX}")

    print("the default answers an unknown service (THE RULE: never skip a reply)")
    unknown = lobby_router.parse_request(bytes.fromhex("2603630801000000") + bdbuf.w_end())
    check("unknown taskId parses anyway", unknown.key == (0x26, 0x63), f"got {unknown.key!r}")
    dflt, how = lobby_router.dispatch(unknown, session)
    check("falls back to the default", how == "default", f"got {how!r}")
    check("default still SENDS", dflt.send is True)
    check("default is the proven success shape", dflt.to_bytes().hex() == REPLY_HEX,
          f"got  {dflt.to_bytes().hex()}\n        want {REPLY_HEX}")

    print("--unknown-error scopes to unhandled services only")
    lobby_router.unknown_error = 5
    try:
        err_reply, _ = lobby_router.dispatch(unknown, session)
        check("unknown service gets the error code", err_reply.error_code == 5,
              f"got {err_reply.error_code}")
        check("a non-zero code carries no body past the code",
              err_reply.to_bytes().hex() == "0a0000000000000000" + "0805000000" + "00",
              f"got {err_reply.to_bytes().hex()}")
        login_reply, _ = lobby_router.dispatch(req, session)
        check("the LOGIN task is untouched by it", login_reply.to_bytes().hex() == REPLY_HEX,
              f"got {login_reply.to_bytes().hex()}")
    finally:
        lobby_router.unknown_error = 0

    print("malformed input still yields a reply")
    for name, raw in (("empty body", b""),
                      ("header byte only", b"\x26"),
                      ("truncated mid-field", bytes.fromhex("260308080400")),
                      ("task id is not a UChar8", bytes.fromhex("2608040000000300"))):
        broken = lobby_router.parse_request(raw)
        r, _ = lobby_router.dispatch(broken, session)
        check(f"{name}: answered", r.send is True and len(r.to_bytes()) > 0)

    print("registration: a service-wide handler catches the tasks nobody registered")

    @lobby_router.handler(lobby_router.SERVICE_ANTICHEAT)
    def _whole_service(request, sess):                       # noqa: ANN001, ANN202
        return lobby_router.Reply(handle=7, error_code=0, flag=1, results=bdbuf.w_u32(0))

    try:
        r, how = lobby_router.dispatch(unknown, session)
        check("service-wide handler catches any task under it", how == "service", f"got {how!r}")
        check("its reply is used", r.handle == 7 and r.flag == 1)
        # A task-specific registration must still win over the service-wide one.
        r2, how2 = lobby_router.dispatch(req, session)
        check("task registration beats service registration", how2 == "task", f"got {how2!r}")
    finally:
        lobby_router._BY_SERVICE.pop(lobby_router.SERVICE_ANTICHEAT, None)

    print("StructData request: the 2026-09-16 capture (service 95, task 3)")
    sreq = lobby_router.parse_request(bytes.fromhex(STRUCT_REQUEST_HEX))
    check("StructData shape detected", sreq.struct_data is True)
    check("service 95, task 3",
          sreq.key == (95, 3), f"got {sreq.key!r}")
    check("walk consumed the buffer exactly", sreq.walk_error is None, f"got {sreq.walk_error!r}")
    check("terminator seen", sreq.terminated is True)
    check("payload is the declared 40 bytes", len(sreq.struct_payload or b"") == 40)
    pb, pb_err = bdbuf.pb_decode(sreq.struct_payload or b"")
    check("payload is protobuf that decodes exactly", pb_err is None, f"got {pb_err!r}")
    check("protobuf values are '', client, bnet, client_tu34, bnet_tu34",
          [f["value"] for f in pb] == ["", "client", "bnet", "client_tu34", "bnet_tu34"],
          f"got {pb!r}")
    sr, show = lobby_router.dispatch(sreq, session)
    check("default reply carries a StructData result (17 08 <len>)",
          sr.to_bytes().hex() == "0a0000000000000000" "0800000000" "0300" "170800000000" "00",
          f"got {sr.to_bytes().hex()}")
    check("plain requests keep the proven UInt32 result",
          lobby_router.dispatch(req, session)[0].to_bytes().hex() == REPLY_HEX)

    print("service 255: tunnelled objectstore LPC list (2026-09-16 capture)")
    import base64, hashlib, json, pathlib, tempfile
    hreq = lobby_router.parse_request(bytes.fromhex(LPC_LIST_REQUEST_HEX))
    check("service 255 / task 10", hreq.key == (255, 10), f"got {hreq.key!r}")
    check("URL read out of the request protobuf",
          (lobby_router.http_request_url(hreq) or "").startswith(
              "/v2/core/publishers/treyarch/objects/?client=&context=5836_bnet&category=tu34_"))
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        (d / "core_ffotd_tu34_100_340c83f3b33348f5.ff").write_bytes(b"ffotd excluded by default")
        (d / "core_playlists_tu34_100_340c83f3b33348f5.ff").write_bytes(b"playlists")
        (d / "en_core_playlists_tu34_100_340c83f3b33348f5.ff").write_bytes(b"en")
        (d / "core_ffotd_tu35_100_340c83f3b33348f5.ff").write_bytes(b"wrong tu")
        import os
        os.environ["CWMOD_LPC_DIR"] = tmp
        try:
            er, _ = lobby_router.dispatch(hreq, session)
            os.environ["CWMOD_LPC_FILES"] = "1"
            hr, hhow = lobby_router.dispatch(hreq, session)
        finally:
            os.environ.pop("CWMOD_LPC_DIR")
            os.environ.pop("CWMOD_LPC_FILES", None)
        efields, _ = bdbuf.walk(er.to_bytes())
        ebody = json.loads(bytes.fromhex(efields[8]["value"]))
        check("default LPC list is empty, with nextPageToken null (TU35 files cannot load in TU34)",
              ebody == {"objects": [], "nextPageToken": None}, f"got {ebody!r}")
    check("answered by the REST handler", hhow == "task", f"got {hhow!r}")
    out = hr.to_bytes()
    fields, err = bdbuf.walk(out)
    check("reply walks cleanly", err is None, f"got {err!r}")
    types = [f["type"] for f in fields]
    check("envelope + count/total + row shape",
          types == [bdbuf.T_UINT64, bdbuf.T_UINT32, bdbuf.T_UCHAR8, bdbuf.T_UINT32, bdbuf.T_UINT32,
                    bdbuf.T_UINT32, bdbuf.T_BLOB, bdbuf.T_BOOL, bdbuf.T_BLOB, bdbuf.T_NOTYPE],
          f"got {types!r}")
    hdr, hdr_err = bdbuf.pb_decode(bytes.fromhex(fields[6]["value"]))
    check("header: status 200, JSON, ascending field order",
          hdr_err is None and [(f["field"], f["value"]) for f in hdr if f["field"] in (1, 302)]
          == [(1, 200), (302, 1)] and [f["field"] for f in hdr] == sorted(f["field"] for f in hdr),
          f"got {hdr!r}")
    body = json.loads(bytes.fromhex(fields[8]["value"]))
    metas = [o["metadata"] for o in body["objects"]]
    check("LPC list carries nextPageToken null (the list parser fails without it)",
          "nextPageToken" in body and body["nextPageToken"] is None)
    check("only the tu34 playlists are listed (ffotd excluded, tu35 ignored)",
          sorted(m["name"] for m in metas) == ["core_playlists_tu34_100_340c83f3b33348f5.ff",
                                               "en_core_playlists_tu34_100_340c83f3b33348f5.ff"],
          f"got {[m['name'] for m in metas]!r}")
    m = next(m for m in metas if m["name"].startswith("core_"))
    check("checksum is base64 MD5", base64.b64decode(m["checksum"]) == hashlib.md5(b"playlists").digest())
    check("string fields fit the client's buffers",
          all(len(m[k]) < n for m in metas for k, n in (("checksum", 33), ("objectVersion", 33),
              ("context", 16), ("name", 65), ("owner", 30), ("category", 65), ("contentURL", 512))))
    check("every required key present",
          all(k in m for m in metas for k in ("checksum", "objectVersion", "expiresOn", "created",
              "modified", "acl", "contentLength", "context", "name", "owner", "category")))
    other = lobby_router.parse_request(bytes.fromhex(LPC_LIST_REQUEST_HEX.replace(
        "63617465676f72793d7475", "63617465676f72793d7878")))   # category=tu -> category=xx
    blob_req = lobby_router.parse_request(bytes.fromhex("0803010a010000000000000000"))
    blob_reply, blob_how = lobby_router.dispatch(blob_req, session)
    check("service 8 / task 1 answers errorCode 800 (not found stops the per-frame resend)",
          blob_reply.error_code == 800 and blob_how != "default", f"got {blob_reply.describe()} via {blob_how}")
    ofields, _ = bdbuf.walk(lobby_router.dispatch(other, session)[0].to_bytes())
    check("a non-tu publisher category gets an honest empty list",
          json.loads(bytes.fromhex(ofields[8]["value"])) == {"objects": [], "nextPageToken": None},
          f"got {ofields!r}")

    def rest(url: str) -> "lobby_router.LobbyRequest":
        u = url.encode()
        pb = bytes([0x5A]) + bdbuf.pb_varint(len(u)) + u                     # field 11, string
        return lobby_router.parse_request(bytes([0xFF]) + bdbuf.w_u8(10) + bdbuf.w_u32(1) + bdbuf.w_blob(pb)
                                          + bdbuf.w_bool(False) + bdbuf.w_blob(b"") + bdbuf.w_end())

    ufields, _ = bdbuf.walk(lobby_router.dispatch(rest("/v2/core/users/uno-1/objects/?client=&context=5836&limit=30"),
                                                  session)[0].to_bytes())
    check("the user objectstore list gets an honest empty list",
          len(ufields) == 10 and json.loads(bytes.fromhex(ufields[8]["value"])) == {"objects": [], "nextPageToken": None},
          f"got {ufields!r}")
    check("unread REST schemas (regulations) keep the zero-row default",
          lobby_router.dispatch(rest("/v1.0/regulations/?client="), session)[0].to_bytes().hex() == REPLY_HEX)

    print("service 12 / task 6: getServerTime")
    import time as _time
    treq = lobby_router.parse_request(bytes.fromhex("0c030600"))
    check("parses as (12, 6)", treq.key == (12, 6), f"got {treq.key!r}")
    tr, thow = lobby_router.dispatch(treq, session)
    tfields, terr = bdbuf.walk(tr.to_bytes())
    check("answered by its handler", thow == "task", f"got {thow!r}")
    check("rows: count 1, total 1, UInt32 now",
          terr is None and [f["type"] for f in tfields[3:6]] == [bdbuf.T_UINT32] * 3
          and tfields[3]["value"] == 1 and tfields[4]["value"] == 1
          and abs(tfields[5]["value"] - int(_time.time())) < 5, f"got {tfields!r}")

    print(f"\n{PASS} passed, {FAIL} failed")
    if FAIL == 0:
        print("ALL PASS")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
