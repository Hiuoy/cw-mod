# Demonware login: reverse-engineering notes

How the auth → umbrella → LSG → lobby chain was reverse-engineered, including the wrong turns, which
are the useful part. Addresses are dump VAs in the IDB (`E:\tools\Reverseing\bocw_fixed_renamed.i64`,
imagebase `0x7FF71CBC0000`, build 1.34.0.15931218). Setup and run steps are in
`tools/dwserver/README.md`; status is in `docs/ROADMAP.md`, milestone detail in `docs/backend-roadmap.md`.

Treat anything here as a finding, not a fact, until an in-game boot has confirmed it. The sections
marked historical describe decisions that later measurements moved past.

## The wall, and what it actually was

For several sessions the client would complete the handshake, send exactly one encrypted record
(inner tag `0x86`, its "Extended Auth Info" report at login status 18), and then wait forever — no
error, no teardown, just a zero-length keepalive every 40 seconds. Two beliefs explained that
silence, and **both were wrong**:

1. *"The reply we owe is inner tag `0x86` with a bdBitBuffer body."* The inner-tag spaces are
   **asymmetric**. The client sends `0x86` (lobby payload) and `0x88` (migrate ack); it only ever
   *dispatches* `1`, `2`, `3` and `5`. `BdLobbyConnection_PumpRecv` (`0x7FF729DD7850`) does
   `movzx eax,[rbp+arg_10]` then `sub 1 / sub 1 / sub 1 / cmp 2` — so a reply tagged `0x86` is
   decrypted, MAC-checked, installed in the payload buffer, reported up as received, and then
   **discarded in silence**. The reply owed to a `0x86` request is **inner tag 1**.
2. *"The lobby body is a bit-packed stream with 5-bit type tags."* It is byte-aligned TLV with
   one-byte type tags. `bdByteBuffer_CheckTypeTag` reads one whole byte; `bdByteBuffer_ReadRaw` is a
   plain byte cursor with no bit position anywhere in the object. The byte-aligned reading had
   already been found and was then dismissed as "a coincidence" — the dismissal was the error. The
   25-entry type table is a literal string array in `bdByteBuffer_TypeName` (`0x7FF729D3BEF0`).

The captured 62-byte `0x86` body now decodes exactly, anchored to known plaintext rather than to
plausibility:

```
0x26  (one client->server header byte)
UChar8 8 | UInt32 4 | UInt32 378 | UInt64 0 x3 | Blob(6) bb442e6d5002 | String "{}" | Int32 0 | NoType
```

`378` is the client version the `/auth/` request body carried, the 6-byte blob is a MAC address, and
`{}` is the `extra_data` JSON. `bdbuftest.py` pins all of it, including a byte-for-byte round-trip of
the real frame through our own writers.

**The lesson worth keeping:** for three consecutive sessions a silent client was read as evidence
about the *body encoding*. It never was — a wrong inner tag is discarded before the body is looked at,
so the silence carried no information about the bytes inside it at all.

Tag 1's layout, from `BdLobby_OnServiceReply_Tag1` (`0x7FF729DD8090`), in read order:

| Field | Type | Note |
|---|---|---|
| handle | `UInt64` (`0x0A`) | ours to choose — pending requests are matched by **queue order**, not by this |
| errorCode | `UInt32` (`0x08`) | `0` → success, `200` → handler-map dispatch, else surfaced as an error event |
| flag | `UChar8` (`0x03`) | only when `errorCode == 0`; the remainder then goes to the task decoder |

There is **no leading header byte in this direction** — `BdLobbyService_SetPayloadBuffer` points the
client's read cursor at `payload[0]`, so byte 0 of the reply must be the `0x0A` type tag itself.

## What is actually proven, and what is not

| Claim | Status |
|---|---|
| Our PSS signature verifies under the client's exact reversed params | proven, offline |
| Both TLS stacks trust one local CA | **proven in-game** — auth completes over TLS |
| The client resolves a Demonware name through our choke point | **proven in-game** |
| The client sends us an auth request | **proven in-game** — see `material/requests.jsonl` |
| The LSG handshake completes and the session key matches | **proven in-game** |
| Client 0x85 records decrypt with matching HMACs | **proven in-game**, byte-identical over 7 logins |
| The lobby payload is byte-aligned TLV | **proven** — from the accessors, and a known-plaintext decode |
| Server→client inner tags are 1/2/3/5, never 0x86 | **proven** — instruction-level, not inferred |
| A tag-1 reply is accepted by the client | **proven in-game** — `[status 27] Login Complete` |
| The request header is `[raw msgType][UChar8 serviceId]` | **proven** — `BdLobbyMsg_WriteHeader`, instruction-level |
| The `UInt32` after the service id is a **task id** | **candidate** — the idiom fits, but the serializer is behind Arxan thunks |
| Which services the client asks for after login | **unknown** — nothing has ever been observed past login |

Note the shape of the two bottom rows: they are what the census boot is for. Everything above them is
either offline-verifiable or has been seen on the wire.

## Milestone 1.7 — the measuring boot

One launch, no new code, and it decides what gets built next:

1. Boot online (`"mode": "online"` in `cw-mod/cw-mod.json`) with **no** `cw-mod/dwserver/` directory. Journalling is on,
   blocking is on, so nothing can reach retail.
2. Open the overlay's **Demonware** tab. Click a mode tile. Press **Save journal**.
3. Read `cw-mod/dw_journal.txt`.

What the answer decides:

- **No sightings at all** — expected, given `nodw=true`. It means the client is not even trying, and
  the next question is what it takes to let the login driver run without the watchdog killing the
  boot. Do not build server endpoints for traffic that will never arrive.
- **Sightings on port 443 only** — playlist/publisher data rides plain HTTPS. Milestone 3 can be
  built **without** the LSG handshake, and M2 stops being a blocker for the frontend.
- **Sightings on port 3075** — the LSG socket is on the critical path and Milestone 2 must land
  first. No amount of HTTPS endpoint work substitutes.

Then repeat with `cw-mod/dwserver/` present and the server running, and read
`material/requests.jsonl`: that turns "which hosts" into "which paths, with which bodies".

## Why Milestone 3 is parked rather than promoted

An earlier plan promoted M3 ahead of M2 on the theory that the online frontend dies because the
publisher dataset (`playlists` / `playlistschedule` / `motd`, fetched from
`objectstore.prod.demonware.net`) is empty on this machine, which would have made the objectstore
the only milestone that mattered and would have forced us to *synthesize* publisher blobs, since
that data is Activision's and is not redistributable.

Two things happened to that theory, in this order:

1. The instrumented boot came back with `attempt to index a nil value` from a stripped chunk — which
   names nothing, and so neither confirms nor refutes the publisher story. T9 compiles LUI with
   local/upvalue names stripped, so Lua *cannot* produce its usual `(field 'x')`; the message is
   generic by construction.
2. Real LPC publisher data was obtained for this machine and is now installed
   (`.manifest` + `core_playlists` / `core_ffotd` fastfiles, TU35). The synthesis blocker is
   therefore gone — and, more importantly, if the client can read that data off disk, the
   objectstore may not be on the critical path for the frontend *at all*.

That measurement has since been taken, and it moved the question rather than settling it. The
`luaL_traceback` fix turned the generic nil into readable Lua errors, the online frontend now builds
clean, and the mode tiles are clickable — but the LobbyVM sits in `director_lan` with no native edge
to `director_online_pregame`. So the frontend is no longer blocked on *publisher data being absent*;
it is blocked on there being no session at all. See `docs/dwemu-pivot-brief.md`.

M3's priority is therefore decided by Milestone 1.7 above — specifically by which port shows up in
the journal — and not by argument. Until that boot happens, neither M2 nor M3 should be started.
