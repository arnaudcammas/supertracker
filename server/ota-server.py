#!/usr/bin/env python3
"""
Tiny HTTP server for ESP32 OTA over home WiFi.

Serves:
  GET /manifest.json   → {"version": <int>, "url": "http://<host>/firmware.bin"}
  GET /firmware.bin    → the binary

All requests must include header  X-OTA-Token: <token>  or get 403.

The version is read from version.txt (single integer). To roll out a new
firmware: scp the .bin to the firmware directory, bump version.txt, done.

Environment:
  OTA_TOKEN    — shared secret with firmware (also in main.cpp OTA_TOKEN)
  LISTEN_PORT  — default 8090
  FW_DIR       — directory containing firmware.bin and version.txt (default /opt/tracker/ota)
  FW_HOST      — hostname/IP to embed in manifest URL (default = request Host header)
"""
import os
import socket
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN   = os.environ.get("OTA_TOKEN", "CHANGE_ME").encode()
LISTEN  = ("0.0.0.0", int(os.environ.get("LISTEN_PORT", 8090)))
FW_DIR  = os.environ.get("FW_DIR", "/opt/tracker/ota")
FW_HOST = os.environ.get("FW_HOST", "")

VERSION_FILE = os.path.join(FW_DIR, "version.txt")
FIRMWARE_BIN = os.path.join(FW_DIR, "firmware.bin")


def current_version() -> int:
    try:
        with open(VERSION_FILE) as f:
            return int(f.read().strip())
    except Exception:
        return 0


class H(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"[ota] {self.address_string()} {fmt % args}", flush=True)

    def _auth(self) -> bool:
        got = self.headers.get("X-OTA-Token", "").encode()
        if not got or len(got) != len(TOKEN):
            return False
        ok = 0
        for a, b in zip(got, TOKEN):
            ok |= a ^ b
        return ok == 0

    def do_GET(self):
        if not self._auth():
            self.send_response(403); self.end_headers(); self.wfile.write(b"forbidden")
            return
        if self.path.startswith("/manifest.json"):
            ver = current_version()
            host = FW_HOST or self.headers.get("Host") or socket.gethostname()
            body = f'{{"version": {ver}, "url": "http://{host}/firmware.bin"}}'.encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/firmware.bin":
            try:
                size = os.path.getsize(FIRMWARE_BIN)
            except FileNotFoundError:
                self.send_response(404); self.end_headers(); return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            with open(FIRMWARE_BIN, "rb") as f:
                while chunk := f.read(8192):
                    self.wfile.write(chunk)
            return
        self.send_response(404); self.end_headers()


if __name__ == "__main__":
    os.makedirs(FW_DIR, exist_ok=True)
    print(f"[ota] listening on {LISTEN[0]}:{LISTEN[1]}", flush=True)
    print(f"[ota] firmware dir: {FW_DIR}", flush=True)
    print(f"[ota] current version: {current_version()}", flush=True)
    ThreadingHTTPServer(LISTEN, H).serve_forever()
