#!/usr/bin/env python3
"""
HMAC-verifying reverse proxy in front of Traccar's OsmAnd listener.

Tracker → bore.pub:<port> → this proxy (verify HMAC + replay guard) → Traccar:5055

The shared secret must match firmware/src/main.cpp HMAC_SECRET.
Set via environment variable HMAC_SECRET, or edit the SECRET fallback below.

Verifies:
  1. ?sig= matches HMAC-SHA256(secret, "id|lat|lon|boot|fixes")[:16]
  2. (boot, fixes) is strictly greater than the last accepted (boot, fixes)
     for that device id — prevents replay of captured packets.

Strips sig/boot/fixes before forwarding so Traccar's OsmAnd parser doesn't
complain about extra parameters.
"""
import hashlib
import hmac
import json
import os
import threading
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlencode, urlparse

SECRET    = os.environ.get("HMAC_SECRET", "CHANGE_ME_USE_A_LONG_RANDOM_STRING").encode()
LISTEN    = ("0.0.0.0", int(os.environ.get("LISTEN_PORT", 5056)))
TRACCAR   = os.environ.get("TRACCAR_URL", "http://localhost:5055")
STATE_FN  = os.environ.get("STATE_FILE", "/var/lib/hmac-proxy/seen.json")

# (boot, fixes) per device id, persisted across restarts to prevent replay.
_lock = threading.Lock()
_seen: dict = {}


def _load_state():
    global _seen
    try:
        with open(STATE_FN) as f:
            _seen = json.load(f)
    except FileNotFoundError:
        _seen = {}


def _save_state():
    os.makedirs(os.path.dirname(STATE_FN), exist_ok=True)
    tmp = STATE_FN + ".tmp"
    with open(tmp, "w") as f:
        json.dump(_seen, f)
    os.replace(tmp, STATE_FN)


def _verify_and_record(params: dict) -> tuple[bool, str]:
    """Return (ok, reason)."""
    try:
        dev   = params["id"][0]
        lat   = params["lat"][0]
        lon   = params["lon"][0]
        boot  = int(params["boot"][0])
        fixes = int(params["fixes"][0])
        sig   = params["sig"][0].lower()
    except (KeyError, ValueError, IndexError):
        return False, "missing/invalid auth fields"

    # Normalize lat/lon to match firmware's %.6f
    try:
        lat = f"{float(lat):.6f}"
        lon = f"{float(lon):.6f}"
    except ValueError:
        return False, "bad lat/lon"

    msg = f"{dev}|{lat}|{lon}|{boot}|{fixes}".encode()
    expected = hmac.new(SECRET, msg, hashlib.sha256).hexdigest()[:16]
    if not hmac.compare_digest(sig, expected):
        return False, "bad signature"

    with _lock:
        prev = _seen.get(dev, (-1, -1))
        if [boot, fixes] <= list(prev):
            return False, f"replay (got boot={boot} fixes={fixes}, last={prev})"
        _seen[dev] = [boot, fixes]
        _save_state()
    return True, ""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # Custom prefix
        print(f"[proxy] {self.address_string()} {fmt % args}", flush=True)

    def do_GET(self):
        parsed = urlparse(self.path)
        params = parse_qs(parsed.query)

        # Static read-only files in /opt/tracker/static/ (no auth required).
        # Used for things like HTTPTOFS probes that don't need to be private.
        if parsed.path.startswith("/static/"):
            fpath = os.path.join("/opt/tracker/static", os.path.basename(parsed.path))
            try:
                size = os.path.getsize(fpath)
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(size))
                self.end_headers()
                with open(fpath, "rb") as f:
                    while chunk := f.read(8192):
                        self.wfile.write(chunk)
            except FileNotFoundError:
                self.send_response(404); self.end_headers()
            return

        ok, reason = _verify_and_record(params)
        if not ok:
            self.send_response(403)
            self.end_headers()
            self.wfile.write(f"forbidden: {reason}".encode())
            print(f"[proxy] REJECT id={params.get('id', ['?'])[0]} reason={reason}", flush=True)
            return

        # Strip auth fields before forwarding
        forward = {k: v[0] for k, v in params.items() if k not in ("sig", "boot", "fixes")}
        # Keep boot/fixes? Forward them since Traccar treats unknown keys as attributes.
        # We strip only sig (Traccar would ignore but cleaner).
        forward = {k: v[0] for k, v in params.items() if k != "sig"}

        try:
            url = f"{TRACCAR}/?{urlencode(forward)}"
            with urllib.request.urlopen(url, timeout=10) as r:
                self.send_response(r.status)
                self.end_headers()
        except Exception as e:
            print(f"[proxy] traccar forward failed: {e}", flush=True)
            self.send_response(502)
            self.end_headers()


if __name__ == "__main__":
    _load_state()
    print(f"[proxy] listening on {LISTEN[0]}:{LISTEN[1]} → {TRACCAR}", flush=True)
    print(f"[proxy] secret length: {len(SECRET)} bytes (HMAC-SHA256)", flush=True)
    print(f"[proxy] state file: {STATE_FN} ({len(_seen)} devices loaded)", flush=True)
    ThreadingHTTPServer(LISTEN, Handler).serve_forever()
