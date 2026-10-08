# Local Demonware backend

Stand up a local, self-hosted Demonware server so the BOCW client reports a live connection
(`Dw_GetConnectionState == 2`) against a server **we** control, instead of patching each
offline progression predicate one at a time. All keys and certs are generated locally and
never leave the machine. Status: [docs/ROADMAP.md](../../docs/ROADMAP.md#3-local-demonware-backend); milestone
detail: [docs/backend-roadmap.md](../../docs/backend-roadmap.md). Reverse-engineering
notes and the history of how each wall was broken: [docs/re/demonware-login.md](../../docs/re/demonware-login.md).

Everything here runs on the user's own machine for a game they own (offline/LAN revival).

## How it works

The client authenticates to Demonware in stages; the two that gate everything are:

1. **Auth** — `POST https://auth3.<env>.demonware.net/auth/`. The reply is JSON plus an
   `X-Signature` header the client verifies as **RSASSA-PSS(SHA-256, MGF1, salt=0)** over the
   raw body, using an **RSA-2048 public key baked into the client image**.
2. **LSG** — a bdSecureSocket TCP handshake (`CLIENTCHAL` → `BDDATA`) guarded by a **second
   embedded RSA-2048 key**; reaching a valid `BDDATA` is what flips the connection state.

Because both keys live *in the client*, we don't need Demonware's private keys. We generate
our own keypairs, patch our **public** keys over the embedded ones at runtime
(`client/game/dw_backend.cpp`), and sign replies with our **private** keys here.

**Redirection is done in-process, not by editing `hosts`.** `client/game/dw_net.cpp` hooks the
game's WS2_32 imports (`getaddrinfo`, `gethostbyname`, `connect`) and answers anything under
`demonware.net` from loopback. This replaced the hosts file for two reasons: it needs no elevated
edit of a system-wide file that outlives the game, and it makes "the modded client cannot reach
retail Activision servers" a structural property rather than a promise — with the hook installed a
Demonware name has no route to the network at all. It also journals every resolve and connect,
which is how we find out what an online boot actually asks for.

**TLS trust is still a local root CA**, and this was checked rather than assumed. The obvious
saving would be shield/project-bo4's trick of disabling curl's certificate verification and
downgrading HTTPS→HTTP. Measured against this image, that does not cover T9: it carries **two
independent TLS clients** — libcurl with the **Schannel** backend (CRYPT32's
`CertGetCertificateChain` / `CertVerifyCertificateChainPolicy` / `CertCreateCertificateChainEngine`
are imported, which is curl's `schannel.c` doing the verifying), and **WinHTTP**, imported and
called from four separate functions, with its own TLS that no curl option touches. Both delegate to
the OS certificate chain engine, so one CA in the Windows store satisfies both while a curl patch
would satisfy neither cleanly.

## Files

| File | Role |
|------|------|
| `gen_keys.py` | Generate RSA keypairs, the 294-byte DER public-key blobs, a local CA, and a leaf cert. |
| `dwsign.py` | PSS signing/verify matching the client's verifier exactly. |
| `authserver.py` | The `/auth/` HTTPS service (Milestone 1) **and the recorder for every other endpoint**. |
| `selftest.py` | Proves our signed reply verifies under the reversed PSS params — no game needed. |
| `smoketest_http.py` | Starts the auth server over TLS and validates a full request/reply against the CA. |
| `recordtest.py` | Proves routing + the recorder: unknown endpoints 404 rather than getting the auth blob. |
| `lsgserver.py` | The LSG listener on 3074: handshake, record layer, frame recorder, and the reply. |
| `lsgcrypto.py` | The bdSecureSocket key schedule and the 0x85 record layer (AES-128-CBC + HMAC-SHA1). |
| `bdbuf.py` | `bdByteBuffer` — the byte-aligned TLV the lobby payload is written in, plus the reply builder. |
| `lobby_router.py` | **The service router**: parses each request into (serviceId, taskId), dispatches to a handler, journals the lot. |
| `lsgtest.py` / `bdbuftest.py` / `routertest.py` | Offline proofs for the three above, all pinned against real captured frames. |
| `material/` | Generated keys, certs, `hosts_entries.txt`, `dw_embedded_keys.hpp`, `requests.jsonl`. (gitignored) |

## The recorder — how the schema gets extracted

Once the redirect is on, **every** Demonware endpoint arrives at this one socket: objectstore,
umbrella, uno, loginqueue, auth3. `authserver.py` routes `/auth*` to the signed reply and writes
every request — served or not — to `material/requests.jsonl`, one JSON object per line, with its
`Host`, path, headers and body.

That file **is** the request contract, observed instead of guessed, which is what the pivot brief
asks for and what disassembly is worst at producing. The `Host` header is the field to read first:
post-redirect every endpoint shares the address `127.0.0.1`, so `Host` is the only thing that says
which service the client thought it was talking to.

Unimplemented endpoints get a **404, deliberately**. The previous version of this file ignored
`self.path` and answered everything — GET included — with the signed auth blob, which under the
redirect hands an auth reply to objectstore and gets discarded without complaint. That presents as
"the client silently does nothing", with no way to tell which request caused it. `--answer-unknown`
switches unknowns to an empty `200` for the specific experiment of asking whether an endpoint cares
about its reply; it is not a mode to leave running, because it re-hides that signal.

## Setup (Milestone 0 → 1)

1. **Generate material** (once):
   ```
   python gen_keys.py
   ```
2. **Prove the server side** (no game needed):
   ```
   python run.py --test       # selftest, smoketest_http, recordtest, lsgtest, bdbuftest, routertest; expect ALL PASS
   ```
3. **Install the CA** (elevated PowerShell):
   ```
   Import-Certificate -FilePath "material\ca_cert.pem" -CertStoreLocation Cert:\LocalMachine\Root
   ```
4. **Redirect the hostnames** — nothing to do. The client redirects itself: `DwNet::Init` turns the
   winsock hook on when `<game>/cw-mod/dwserver/` exists, which step 5 creates anyway. Watch the log
   for `DwNet: Winsock choke point: redirect=true` and then a `resolve <host> -> 127.0.0.1` line per
   endpoint. (`material/hosts_entries.txt` is kept only as the list of hostnames, for reference.)
5. **Give the client our public keys** — copy `material/auth_pub.der` and `material/lsg_pub.der`
   into `<game working dir>/cw-mod/dwserver/`. Their presence is the client's opt-in: on next
   launch `DwBackend::PatchEmbeddedKeys` replaces the embedded keys (watch the log for
   `DwBackend: key replaced and verified`) and the startup path leaves the DW connection active
   instead of forcing `com_noDW`.

   > The client change adds new source files, so re-run `generate.bat` (premake) before building
   > so `dw_backend.cpp` is included, then build the `.sln` (see memory `cw-mod-build-toolset`).

6. **Run the backend**:
   ```
   python run.py
   ```
   Preflight first: CA in `LocalMachine\Root`, leaf and CRL valid, the game's `*_pub.der` match
   `material/`, and ports 443/80/3074 free (it names the process holding one). Then it starts
   `authserver.py` and `lsgserver.py` as child processes, probes them the way Schannel will (CRL
   fetch, TLS verified under a real Demonware hostname), prints `READY`, and appends both servers'
   output to `material/server.log`. Server flags pass through: `--auth-args "--reply-ints"`,
   `--lsg-args "--unknown-error 5"`. `python run.py --check` runs the preflight alone.

   Every setup mistake shows up in-game as the same `Auth task failed with HTTP code [0]`, so run
   the preflight rather than guessing. The servers can still be started by hand, as below.

   The auth server binds 443 for TLS *and* 80 for the CRL:
   ```
   python -u authserver.py
   ```
   The defaults are the measured-correct values. In particular `--lsg-endpoint` defaults to the
   **bare host** `127.0.0.1`: the client hardcodes the LSG port at 3074 and parses this field as a
   hostname, so a `:port` suffix does not move the port, it corrupts the name. An earlier version of
   this line said `127.0.0.1:3075` and cost a test cycle.
7. **The LSG server**, if starting by hand, in a second console:
   ```
   python -u lsgserver.py
   ```

## Verify

After a boot, `python tools/bootlog.py` (from the repo root) prints the login transcript, err_drops,
crashes and redirect lines from the last session of `client.log`, with the matching `server.log`
session underneath.

- **Milestone 0** — `python smoketest_http.py` passes; after launch the client log shows
  `DwBackend: key replaced and verified` for `auth` (and `lsg`).
- **Milestone 1** — with the server running and the CA in place, the client log reaches
  `Authenticated to Demonware`; `authserver.py` logs `POST /auth/ -> code 700`.
- **Milestone 1.5** — the client log shows `DwNet: Winsock choke point: redirect=true` and at least
  one `resolve navyblue-auth3.prod.demonware.net -> 127.0.0.1`. This is what proves the redirect is
  actually load-bearing: before it, "auth was never reached" and "auth was reached and failed" looked
  identical from here. Also visible in the overlay's **Demonware** tab, and written live to
  `cw-mod/dw_journal.txt` so a boot that dies still leaves its record.
- **Milestone 1.6** — `python recordtest.py` passes, and after a boot with the server running,
  `material/requests.jsonl` has one line per request with a populated `Host`.
- **Milestone 2** — the client log reaches `[status 27] Login Complete` and the frontend comes up.
  `lsgserver.py` logs the handshake, one `innerTag=0x86` record, and our tag-1 reply.
- **Milestone 3a** — `python routertest.py` passes, and a live boot prints
  `[lsgd] request #N: <service>/<task>` per request instead of one anonymous reply, with
  `material/lobby_requests.jsonl` gaining a line each.

## The router

Milestone 2 needed exactly **one** answer, so `lsgserver.py` gave every record the same reply, built
from command-line knobs. That is right for one request and wrong for two. `lobby_router.py` is the
dispatch layer that replaces it.

**The request grammar is read out of the engine, not inferred from the capture.** `BdLobbyMsg_Ctor`
(`0x7FF729DD2300`) builds every outbound lobby message and immediately calls `BdLobbyMsg_WriteHeader`
(`0x7FF729D98EC0`), which is two writes and nothing else — `WriteRaw(&msgType, 1)` then
`WriteUChar8(serviceId)`. So a client→server `0x86` body is:

```
[raw msgType][03 serviceId][ ...the task's own fields... ][00 NoType]
```

That settles two things previously read off a single frame: the leading `0x26` is the **message
type** and is genuinely untagged, and the first TLV is the **service id** rather than a task field.
They are separate ctor arguments held at `buf+56`/`+57`. `serviceId 8 = bdAntiCheat` is proven twice
— the builder call in `AntiCheat_StartReportExtendedAuthInfo` passes `LOBYTE=8`, and the captured
login request carries `UChar8 8` in exactly that slot.

**`taskId` is the one piece still on probation.** In the only request ever seen, the field after the
service id is `UInt32 4`, and reading it as a task id is the standard bdLobby idiom — but that
serializer sits behind Arxan return-gadget thunks and could not be read. So handlers may register
against a **whole service** as well as a task, and the journal records the full field list either
way. The first census boot showing two different tasks under one service settles it in one line.

### The rule: answer every request, exactly once, in order

`BdLobby_OnServiceReply_Tag1` pops its pending-request queue (`this+112`) **unconditionally, before
reading a single byte of the reply**. Nothing on the wire says which request a reply belongs to — not
even the handle, which is read after the pop.

This is the exact opposite of the HTTP side's rule, and the contrast is the point: `authserver.py`
deliberately **404s** an endpoint it does not implement, because an unanswered HTTP request is a
clean signal. Here, skipping a reply is not a signal at all — it slides every later reply onto the
wrong request, and the resulting errors describe requests we answered correctly. So an unhandled
service gets the default success and the loud part happens in the log:

```
[lsgd]    !! UNHANDLED service 12 / task 3 -- answered with the default (errorCode=0 ...)
```

`--unknown-error N` answers *unhandled services only* with an error code, for the deliberate
experiment of asking whether a request mattered; handled services (including login) are untouched,
so the boot still gets to the menu. Like `--answer-unknown` on the HTTP side, it is not a mode to
leave running.

### Adding a handler

```python
@lobby_router.handler(service_id=8, task_id=4)     # omit task_id to catch a whole service
def my_task(req, session):
    return lobby_router.empty_success()            # or Reply(error_code=..., results=...)
```

`req.params` is the field list with the service id, task id and trailing `NoType` already stripped;
`session` is per-connection state. Every request and reply lands in
`material/lobby_requests.jsonl` — the M3 equivalent of `requests.jsonl`, and the file the census
boot produces.

### The knobs are now an override

`--sweep` and `--reply-*` still exist and still do what they did, but they now speak for **every**
service when passed, so they are opt-in and announced at startup. A plain `python -u lsgserver.py`
goes through the router. Nothing is lost: the router's default reply is byte-for-byte the one the
sweep had already found (`0:0:0:u32`).

Offline proof: `python routertest.py` → `ALL PASS`. It is pinned to the real captured login exchange,
so the login path cannot silently change shape.

## Risk notes

- The key replacement writes to `.rdata`. If Arxan checksums that region it may revert the
  patch (the log's read-back check will report `read-back MISMATCH`); the fallback is to hook
  `Crypto_RsaImportPubKey` (0x7FF729D3AE60) instead. Only in-game runtime can confirm.
- Build-locked to 1.34.0.15931218 (dump imagebase `0x7FF71CBC0000`); the patch refuses to write
  unless it finds a real RSA-2048 SPKI header at the target, so a build mismatch is a safe no-op.
