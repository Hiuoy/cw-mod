#!/usr/bin/env python3
"""One command for the local Demonware backend: preflight, then auth + LSG in one console.

    python run.py                 preflight, start both servers, one combined timestamped log
    python run.py --check         preflight only
    python run.py --test          run every offline test (no game, no ports needed)
    python run.py --auth-args "--reply-ints" --lsg-args "--unknown-error 5"

WHY THIS EXISTS. The client reports every setup mistake the same way: `[status code=25] Auth task
failed with HTTP code [0]`. No server listening, a CA missing from the machine store, a CRL
Schannel cannot fetch, and a stale key in the game folder all produce that one line. The
2026-09-16 boots hit it and the server journals had not been written since 2026-08-02, so no
server was running. Each check below rules out one cause, by name, before the game is launched.

The servers are started as child processes, unmodified, so their own flags keep working and
running authserver.py / lsgserver.py by hand still works. Output from both is prefixed, timestamped
and appended to material/server.log, so a boot can be read back in order afterwards.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import pathlib
import shlex
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
MATERIAL = HERE / "material"
REPO = HERE.parent.parent
DIRECTORIES_JSON = REPO / "tools" / "game" / "directories.json"
SERVER_LOG = MATERIAL / "server.log"

AUTH_PORT = 443
CRL_PORT = 80
LSG_PORT = 3074
PROBE_HOST = "t9-bnet-auth3.prod.demonware.net"

TESTS = ["selftest", "smoketest_http", "recordtest", "lsgtest", "bdbuftest", "routertest"]

REQUIRED_MATERIAL = [
    "auth_priv.pem", "auth_pub.der", "lsg_priv.pem", "lsg_pub.der",
    "ca_cert.pem", "server_cert.pem", "server_key.pem", "cwmod.crl",
]


# --- output --------------------------------------------------------------------------------------

class Preflight:
    def __init__(self) -> None:
        self.failed = 0
        self.warned = 0

    def ok(self, msg: str) -> None:
        print(f"  [ ok ] {msg}")

    def warn(self, msg: str, fix: str = "") -> None:
        self.warned += 1
        print(f"  [warn] {msg}")
        if fix:
            print(f"         -> {fix}")

    def fail(self, msg: str, fix: str = "") -> None:
        self.failed += 1
        print(f"  [FAIL] {msg}")
        if fix:
            print(f"         -> {fix}")


# --- checks --------------------------------------------------------------------------------------

def find_game_dir(override: str | None) -> pathlib.Path | None:
    if override:
        return pathlib.Path(override)
    try:
        dirs = json.loads(DIRECTORIES_JSON.read_text())
    except (OSError, ValueError):
        return None
    for path in dirs.values():
        if path and pathlib.Path(path).is_dir():
            return pathlib.Path(path)
    return None


def check_material(p: Preflight) -> bool:
    missing = [n for n in REQUIRED_MATERIAL if not (MATERIAL / n).exists()]
    if missing:
        p.fail(f"material/ is missing {', '.join(missing)}", "python gen_keys.py")
        return False
    p.ok("material/ has keys, CA, leaf and CRL")
    return True


def check_certs(p: Preflight) -> str | None:
    """Leaf and CRL against the CA. Returns the CA's SHA-1 thumbprint for the store check."""
    try:
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes
    except ImportError:
        p.fail("python package `cryptography` is not installed", "pip install cryptography")
        return None
    import gen_keys

    ca = x509.load_pem_x509_certificate((MATERIAL / "ca_cert.pem").read_bytes())
    leaf = x509.load_pem_x509_certificate((MATERIAL / "server_cert.pem").read_bytes())
    crl = x509.load_der_x509_crl((MATERIAL / "cwmod.crl").read_bytes())
    now = datetime.datetime.now(datetime.timezone.utc)

    try:
        leaf.verify_directly_issued_by(ca)
        if not (leaf.not_valid_before_utc <= now <= leaf.not_valid_after_utc):
            p.fail(f"leaf certificate is outside its validity window (until {leaf.not_valid_after_utc:%Y-%m-%d})",
                   "python reissue_cert.py")
        else:
            p.ok(f"leaf certificate is signed by our CA, valid until {leaf.not_valid_after_utc:%Y-%m-%d}")
    except Exception as exc:  # noqa: BLE001 -- any verification error means the same fix
        p.fail(f"leaf certificate is not signed by material/ca_cert.pem ({exc})", "python reissue_cert.py")

    try:
        cdp = leaf.extensions.get_extension_for_class(x509.CRLDistributionPoints).value
        urls = [n.value for dp in cdp for n in (dp.full_name or [])]
    except x509.ExtensionNotFound:
        urls = []
    if gen_keys.CRL_URL not in urls:
        p.fail(f"leaf has no CRL distribution point at {gen_keys.CRL_URL} (has {urls or 'none'}); "
               "Schannel fails the handshake with CRYPT_E_NO_REVOCATION_CHECK",
               "python reissue_cert.py")
    else:
        p.ok(f"leaf names the CRL at {gen_keys.CRL_URL}")

    try:
        sans = leaf.extensions.get_extension_for_class(x509.SubjectAlternativeName).value.get_values_for_type(x509.DNSName)
    except x509.ExtensionNotFound:
        sans = []
    if PROBE_HOST not in sans:
        p.warn(f"leaf SAN does not cover {PROBE_HOST} ({len(sans)} names)", "python reissue_cert.py")

    if not crl.is_signature_valid(ca.public_key()):
        p.fail("cwmod.crl is not signed by our CA", "python reissue_cert.py")
    elif crl.next_update_utc is not None and crl.next_update_utc < now:
        p.fail(f"cwmod.crl expired {crl.next_update_utc:%Y-%m-%d}", "python reissue_cert.py")
    else:
        p.ok(f"CRL is signed by our CA, next update {crl.next_update_utc:%Y-%m-%d}")

    return ca.fingerprint(hashes.SHA1()).hex().upper()


def check_ca_installed(p: Preflight, thumb: str) -> None:
    if os.name != "nt":
        p.warn("not Windows; skipping the LocalMachine\\Root check")
        return
    ps = f"Test-Path 'Cert:\\LocalMachine\\Root\\{thumb}'"
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
                             capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.TimeoutExpired) as exc:
        p.warn(f"could not query the certificate store ({exc})")
        return
    if out == "True":
        p.ok(f"CA {thumb[:12]}... is in LocalMachine\\Root")
    else:
        p.fail(f"CA {thumb} is NOT in LocalMachine\\Root; every TLS handshake will fail",
               f"elevated PowerShell: Import-Certificate -FilePath \"{MATERIAL / 'ca_cert.pem'}\" "
               "-CertStoreLocation Cert:\\LocalMachine\\Root")


def check_game(p: Preflight, game: pathlib.Path | None) -> None:
    if game is None:
        p.warn(f"game folder unknown; skipping the key and marker checks",
               f"fill {DIRECTORIES_JSON.relative_to(REPO)} or pass --game")
        return
    cwmod = game / "cw-mod"
    for name in ("auth_pub.der", "lsg_pub.der"):
        deployed = cwmod / "dwserver" / name
        if not deployed.exists():
            p.fail(f"{deployed} is missing, so the client keeps its embedded key and backend mode is off",
                   f"copy material\\{name} there")
        elif deployed.read_bytes() != (MATERIAL / name).read_bytes():
            p.fail(f"{deployed} differs from material\\{name}; the client will reject our signatures",
                   f"copy material\\{name} there (regenerated keys?)")
        else:
            p.ok(f"game has the current {name}")

    # cw-mod.json replaced the marker files; the client writes it from them on the first boot that
    # has none, so a missing file means the old markers still decide the next boot.
    settings_path = cwmod / "cw-mod.json"
    kinds = {"offline": "OFFLINE", "lan": "LAN", "lanlobby": "online nibble + LAN lobby", "online": "ONLINE (live)"}
    if not settings_path.exists():
        p.warn(f"no {settings_path}; the client writes it from the old marker files on the next boot")
        return
    try:
        settings = json.loads(settings_path.read_text(encoding="utf-8-sig"))
        if not isinstance(settings, dict):
            raise ValueError("not a JSON object")
    except ValueError as exc:
        p.fail(f"{settings_path.name} does not parse ({exc}); the client boots with every setting at its default",
               f"fix {settings_path}")
        return
    mode = str(settings.get("mode", "offline")).lower()
    if mode not in kinds:
        p.fail(f"{settings_path.name}: mode {mode!r} is not one of {', '.join(kinds)}", f"fix {settings_path}")
    elif settings.get("backend", True) is False:
        p.fail(f"{settings_path.name}: \"backend\" is false, so the client ignores this server",
               f"set \"backend\": true in {settings_path}")
    else:
        name = settings.get("name") or "(Windows account name)"
        p.ok(f"{settings_path.name}: mode {mode}, so the next boot is {kinds[mode]} + backend, as {name!r}")


def listeners_on(port: int) -> list[str]:
    """Who is LISTENING on a TCP port, named by process. A bind test alone is not enough on Windows:
    a socket bound to 0.0.0.0 without SO_EXCLUSIVEADDRUSE does not stop a later 127.0.0.1 bind, and
    connections are then split between the two servers unpredictably."""
    if os.name != "nt":
        return []
    try:
        out = subprocess.run(["netstat", "-ano", "-p", "TCP"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    found = []
    for line in out.splitlines():
        cols = line.split()
        if len(cols) >= 5 and cols[3].upper() == "LISTENING" and cols[1].rsplit(":", 1)[-1] == str(port):
            pid = cols[4]
            name = "?"
            try:
                t = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
                                   capture_output=True, text=True, timeout=30).stdout.strip()
                if t.startswith('"'):
                    name = t.split('","')[0].strip('"')
            except (OSError, subprocess.TimeoutExpired):
                pass
            found.append(f"{cols[1]} pid {pid} ({name})")
    return found


def check_ports(p: Preflight) -> None:
    for port, role in ((AUTH_PORT, "auth HTTPS"), (CRL_PORT, "CRL HTTP"), (LSG_PORT, "LSG")):
        holders = listeners_on(port)
        if holders:
            p.fail(f"port {port} ({role}) is already taken: {'; '.join(holders)}",
                   "stop that process (a stale server shadows the new one)")
            continue
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        try:
            s.bind(("127.0.0.1", port))
            p.ok(f"port {port} ({role}) is free")
        except OSError as exc:
            fix = "run elevated, or free the URL reservation (netsh http show urlacl)" if port < 1024 else ""
            p.fail(f"cannot bind 127.0.0.1:{port} ({role}): {exc}", fix)
        finally:
            s.close()


def preflight(game_override: str | None, ports: bool = True) -> bool:
    print("[run] preflight")
    p = Preflight()
    if check_material(p):
        thumb = check_certs(p)
        if thumb:
            check_ca_installed(p, thumb)
    check_game(p, find_game_dir(game_override))
    if ports:
        check_ports(p)
    verdict = "PASS" if not p.failed else f"{p.failed} FAILURE(S)"
    print(f"[run] preflight: {verdict}" + (f", {p.warned} warning(s)" if p.warned else ""))
    return p.failed == 0


# --- live probe ----------------------------------------------------------------------------------

def live_probe(say, deadline_s: float = 10.0) -> bool:
    """After start: do what Schannel will do. Fetch the CRL over HTTP, complete a TLS handshake to
    443 that verifies against our CA under a real Demonware hostname, and see 3074 listening.
    Passing this means the next `HTTP code [0]` is not a setup problem.

    3074 is checked with netstat rather than a connect: lsgserver journals every connection, and a
    probe that says nothing would land in lsg_frames.jsonl as a fake client."""
    ok = True
    end = time.monotonic() + deadline_s

    def retry(fn):
        last = None
        while time.monotonic() < end:
            try:
                return fn(), None
            except Exception as exc:  # noqa: BLE001
                last = exc
                time.sleep(0.25)
        return None, last

    body, err = retry(lambda: urllib.request.urlopen(f"http://127.0.0.1:{CRL_PORT}/cwmod.crl", timeout=2).read())
    if err:
        ok = False
        say(f"probe FAIL: CRL fetch on :{CRL_PORT}: {err}")
    else:
        say(f"probe ok:   CRL served on :{CRL_PORT} ({len(body)}B)")

    def tls():
        ctx = ssl.create_default_context(cafile=str(MATERIAL / "ca_cert.pem"))
        with socket.create_connection(("127.0.0.1", AUTH_PORT), timeout=2) as raw:
            with ctx.wrap_socket(raw, server_hostname=PROBE_HOST) as tls_sock:
                return tls_sock.version()
    ver, err = retry(tls)
    if err:
        ok = False
        say(f"probe FAIL: TLS to :{AUTH_PORT} as {PROBE_HOST}: {err}")
    else:
        say(f"probe ok:   TLS to :{AUTH_PORT} as {PROBE_HOST} verifies against our CA ({ver})")

    def lsg_listening():
        if os.name == "nt" and not listeners_on(LSG_PORT):
            raise OSError("nothing listening")
    _, err = retry(lsg_listening)
    if err:
        ok = False
        say(f"probe FAIL: LSG connect to :{LSG_PORT}: {err}")
    else:
        say(f"probe ok:   LSG listening on :{LSG_PORT}")
    return ok


# --- serve ---------------------------------------------------------------------------------------

class CombinedLog:
    def __init__(self, path: pathlib.Path) -> None:
        self.lock = threading.Lock()
        self.file = path.open("a", encoding="utf-8")
        self.write("run", f"===== server session start {datetime.datetime.now():%Y-%m-%d %H:%M:%S} =====")

    def write(self, tag: str, line: str) -> None:
        stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        text = f"{stamp} [{tag}] {line}"
        with self.lock:
            print(text, flush=True)
            self.file.write(text + "\n")
            self.file.flush()


def pump(proc: subprocess.Popen, tag: str, log: CombinedLog) -> None:
    for raw in proc.stdout:
        log.write(tag, raw.rstrip("\r\n"))


def serve(auth_args: list[str], lsg_args: list[str]) -> int:
    log = CombinedLog(SERVER_LOG)
    env = dict(os.environ, PYTHONUNBUFFERED="1", PYTHONIOENCODING="utf-8")
    procs: dict[str, subprocess.Popen] = {}
    for tag, script, extra in (("auth", "authserver.py", auth_args), ("lsg", "lsgserver.py", lsg_args)):
        cmd = [sys.executable, "-u", str(HERE / script), *extra]
        log.write("run", f"starting {tag}: {' '.join([script, *extra])}")
        procs[tag] = subprocess.Popen(cmd, cwd=HERE, env=env, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                      errors="replace", bufsize=1)
        threading.Thread(target=pump, args=(procs[tag], tag, log), daemon=True).start()

    if live_probe(lambda msg: log.write("run", msg)):
        log.write("run", "READY: launch the game. Ctrl+C stops both servers.")
    else:
        log.write("run", "NOT READY: a live probe failed (see above); the client will report HTTP code [0].")

    code = 0
    try:
        while True:
            dead = [(t, p.returncode) for t, p in procs.items() if p.poll() is not None]
            if dead:
                for t, rc in dead:
                    log.write("run", f"{t} server EXITED with code {rc}; stopping the other one")
                code = 1
                break
            time.sleep(0.5)
    except KeyboardInterrupt:
        log.write("run", "Ctrl+C: stopping servers")
    finally:
        for p in procs.values():
            if p.poll() is None:
                p.terminate()
        for p in procs.values():
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        time.sleep(0.2)  # let the pump threads flush the last lines
        log.write("run", "===== server session end =====")
    return code


# --- tests ---------------------------------------------------------------------------------------

def run_tests() -> int:
    results = []
    for name in TESTS:
        print(f"[run] --- {name}")
        t0 = time.monotonic()
        rc = subprocess.run([sys.executable, "-u", str(HERE / f"{name}.py")], cwd=HERE).returncode
        results.append((name, rc, time.monotonic() - t0))
    print("[run] test summary")
    for name, rc, dt in results:
        print(f"  [{'pass' if rc == 0 else 'FAIL'}] {name} ({dt:.1f}s)")
    failed = sum(1 for _, rc, _ in results if rc)
    print(f"[run] {'ALL PASS' if not failed else f'{failed} FAILED'}")
    return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="preflight only")
    mode.add_argument("--test", action="store_true", help="run every offline test")
    ap.add_argument("--game", help="game folder (default: first existing entry in tools/game/directories.json)")
    ap.add_argument("--force", action="store_true", help="start the servers even if preflight fails")
    ap.add_argument("--auth-args", default="", help="extra flags for authserver.py, one quoted string")
    ap.add_argument("--lsg-args", default="", help="extra flags for lsgserver.py, one quoted string")
    args = ap.parse_args()

    if args.test:
        return run_tests()
    ok = preflight(args.game)
    if args.check:
        return 0 if ok else 1
    if not ok and not args.force:
        print("[run] not starting: fix the failures above, or pass --force")
        return 1
    return serve(shlex.split(args.auth_args, posix=False), shlex.split(args.lsg_args, posix=False))


if __name__ == "__main__":
    raise SystemExit(main())
