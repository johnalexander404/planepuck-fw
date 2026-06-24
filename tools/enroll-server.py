#!/usr/bin/env python3
"""
PlanePuck zero-touch MQTT enrollment endpoint.

A fresh puck with no MQTT password POSTs {"code","pw"} here with an
"Authorization: Bearer <ENROLL_TOKEN>" header. We verify the token (constant-time), then register
the (username=code, password=pw) pair in the Mosquitto password file and reload Mosquitto. The puck
then connects as user=<code>. See tools/OTA-SETUP.md ("Zero-touch enrollment") and src/config.h
(ENROLL_URL / ENROLL_TOKEN).

Runs behind Caddy on localhost (Caddy terminates TLS and proxies /enroll here). Stdlib only — no
third-party deps. Never logs the token or the password.

Config via environment (set by the systemd unit / EnvironmentFile):
  PLANEPUCK_ENROLL_TOKEN   required — must equal ENROLL_TOKEN in the firmware's config.h
  PLANEPUCK_PASSWD_FILE    default /etc/mosquitto/passwd
  PLANEPUCK_RELOAD_CMD     default "systemctl reload mosquitto"  (use "sudo systemctl ..." if non-root)
  PLANEPUCK_BIND           default 127.0.0.1:8090
"""
import json, os, re, hmac, subprocess, sys, time, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN       = os.environ.get("PLANEPUCK_ENROLL_TOKEN", "")
PASSWD_FILE = os.environ.get("PLANEPUCK_PASSWD_FILE", "/etc/mosquitto/passwd")
RELOAD_CMD  = os.environ.get("PLANEPUCK_RELOAD_CMD", "systemctl reload mosquitto").split()
BIND        = os.environ.get("PLANEPUCK_BIND", "127.0.0.1:8090")
MAX_BODY    = 1024

CODE_RE = re.compile(r"^[0-9a-fA-F]{6,16}$")    # friend code = 8 hex; allow a small range
PW_RE   = re.compile(r"^[0-9A-Za-z]{8,64}$")    # the puck sends 32 hex; reject anything odd

# Coarse global rate limit (token bucket ~1/s, burst 10) as a backstop. Do per-IP limiting at the
# edge (Caddy rate_limit plugin or fail2ban on this log) — see OTA-SETUP.md.
_lock = threading.Lock()
_tokens = 10.0
_last = time.monotonic()
def allow():
    global _tokens, _last
    with _lock:
        now = time.monotonic()
        _tokens = min(10.0, _tokens + (now - _last) * 1.0)
        _last = now
        if _tokens >= 1.0:
            _tokens -= 1.0
            return True
        return False

def log(msg):
    print(f"[enroll] {msg}", flush=True)

class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_POST(self):
        if not allow():
            return self._send(429, {"error": "rate limited"})
        auth = self.headers.get("Authorization", "")
        presented = auth[7:] if auth.startswith("Bearer ") else ""
        if not TOKEN or not hmac.compare_digest(presented, TOKEN):
            log("rejected: bad or missing token")
            return self._send(401, {"error": "unauthorized"})
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        if n <= 0 or n > MAX_BODY:
            return self._send(400, {"error": "bad length"})
        try:
            data = json.loads(self.rfile.read(n).decode())
            code = str(data["code"]); pw = str(data["pw"])
        except Exception:
            return self._send(400, {"error": "bad json"})
        if not CODE_RE.match(code) or not PW_RE.match(pw):
            log(f"rejected: bad code/pw format (code={code!r})")
            return self._send(400, {"error": "bad code or pw"})
        # No shell=True and list args -> no shell injection; the regexes are belt-and-suspenders.
        try:
            subprocess.run(["mosquitto_passwd", "-b", PASSWD_FILE, code, pw],
                           check=True, capture_output=True, timeout=10)
            subprocess.run(RELOAD_CMD, check=True, capture_output=True, timeout=10)
        except subprocess.CalledProcessError as e:
            log(f"mosquitto_passwd/reload failed for {code}: {e.stderr.decode().strip()}")
            return self._send(500, {"error": "server error"})
        except Exception as e:
            log(f"error for {code}: {e}")
            return self._send(500, {"error": "server error"})
        log(f"enrolled {code}")
        return self._send(200, {"ok": True})

    def do_GET(self):   # unauthenticated health check (no secrets exposed)
        self._send(200, {"ok": True, "service": "planepuck-enroll"})

    def log_message(self, *a):   # silence default access logging; we emit our own [enroll] lines
        pass

def main():
    if not TOKEN:
        sys.exit("PLANEPUCK_ENROLL_TOKEN not set (put it in /etc/planepuck/enroll.env)")
    host, _, port = BIND.partition(":")
    srv = ThreadingHTTPServer((host, int(port)), Handler)
    log(f"listening on {BIND} passwd={PASSWD_FILE} reload={' '.join(RELOAD_CMD)}")
    srv.serve_forever()

if __name__ == "__main__":
    main()
