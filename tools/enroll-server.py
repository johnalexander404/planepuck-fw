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
import json, os, re, hmac, hashlib, subprocess, sys, time, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN       = os.environ.get("PLANEPUCK_ENROLL_TOKEN", "")
PASSWD_FILE = os.environ.get("PLANEPUCK_PASSWD_FILE", "/etc/mosquitto/passwd")
RELOAD_CMD  = os.environ.get("PLANEPUCK_RELOAD_CMD", "systemctl reload mosquitto").split()
BIND        = os.environ.get("PLANEPUCK_BIND", "127.0.0.1:8090")
MAX_BODY    = 1024
# TOFU key-pin store: maps friend code -> {"key": <64hex>, "ctr": <int>}. The first enroll for a code
# pins its per-device HMAC key; afterwards only a request SIGNED by that key can change the password.
PIN_FILE    = os.environ.get("PLANEPUCK_PIN_FILE", "/etc/planepuck/enroll-keys.json")
# Migration flag: while 0, legacy unsigned enrolls (old firmware) are still accepted for UNPINNED codes
# (and not pinned). Set to 1 once the whole fleet is on signed-enroll firmware to reject all unsigned.
REQUIRE_SIG = os.environ.get("PLANEPUCK_REQUIRE_SIG", "0") == "1"

CODE_RE = re.compile(r"^[0-9a-fA-F]{6,16}$")    # friend code = 8 hex; allow a small range
PW_RE   = re.compile(r"^[0-9A-Za-z]{8,64}$")    # the puck sends 32 hex; reject anything odd
KEY_RE  = re.compile(r"^[0-9a-fA-F]{64}$")      # per-device enroll key K (256-bit hex)
SIG_RE  = re.compile(r"^[0-9a-fA-F]{64}$")      # HMAC-SHA256 hex

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

# ---- TOFU key-pin store -------------------------------------------------------------------------
_pin_lock = threading.Lock()   # serialize read-modify-write of the pin store + the passwd write

def load_pins():
    try:
        with open(PIN_FILE) as f:
            return json.load(f)
    except FileNotFoundError:
        return {}
    except Exception as e:
        log(f"WARN: pin store unreadable ({e}); treating as empty")
        return {}

def save_pins(pins):
    # Atomic replace + tight perms (root-only: this file maps codes -> per-device secret keys).
    tmp = f"{PIN_FILE}.tmp.{os.getpid()}"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(pins, f)
            f.flush(); os.fsync(f.fileno())
        os.replace(tmp, PIN_FILE)          # atomic on the same filesystem
    finally:
        if os.path.exists(tmp):
            try: os.remove(tmp)
            except OSError: pass

def verify_sig(key_hex, code, pw, ctr, sig_hex):
    # Canonical message MUST match the firmware byte-for-byte: code + "\n" + pw + "\n" + decimal(ctr).
    msg = (f"{code}\n{pw}\n{ctr}").encode()
    want = hmac.new(bytes.fromhex(key_hex), msg, hashlib.sha256).hexdigest()
    return hmac.compare_digest(want, sig_hex.lower())

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
        code = code.upper()                      # pin store key + MQTT username are uppercase

        # Optional signed-enroll fields; a request is "signed" if it carries any of them. Validate the
        # formats up front so a MALFORMED signed request 400s instead of silently falling back to legacy.
        key = str(data.get("key", "")); sig = str(data.get("sig", "")); ctr_raw = data.get("ctr", None)
        signed = bool(key or sig or ctr_raw is not None)
        if signed:
            if not KEY_RE.match(key) or not SIG_RE.match(sig):
                log(f"rejected: bad key/sig format ({code})")
                return self._send(400, {"error": "bad key or sig"})
            try:
                ctr = int(ctr_raw)
            except (TypeError, ValueError):
                return self._send(400, {"error": "bad ctr"})
            if ctr < 0:
                return self._send(400, {"error": "bad ctr"})

        # Read-modify-write the pin store + the broker password atomically per code.
        with _pin_lock:
            pins = load_pins()
            pin = pins.get(code)
            if pin is None:
                # ---- code not yet pinned ----
                if signed:                                   # TOFU: trust + pin the key the device presents
                    if not verify_sig(key, code, pw, ctr, sig):
                        log(f"rejected: TOFU sig invalid ({code})")
                        return self._send(403, {"error": "bad signature"})
                    pins[code] = {"key": key.lower(), "ctr": ctr}
                    save_pins(pins)
                    log(f"TOFU-pinned {code}")
                elif REQUIRE_SIG:
                    log(f"rejected: unsigned enroll but REQUIRE_SIG ({code})")
                    return self._send(403, {"error": "signature required"})
                else:
                    log(f"legacy unsigned enroll (unpinned) {code}")   # migration: accept but do NOT pin
            else:
                # ---- code already pinned: require a valid signed re-enroll ----
                if not signed:
                    log(f"rejected: pinned code needs a signed enroll ({code})")
                    return self._send(403, {"error": "signature required"})
                if not hmac.compare_digest(key.lower(), pin["key"].lower()):
                    log(f"rejected: key mismatch for pinned {code} (re-flash? operator must unpin)")
                    return self._send(403, {"error": "key mismatch"})
                if ctr <= pin["ctr"]:
                    log(f"rejected: stale ctr {ctr} <= {pin['ctr']} ({code})")
                    return self._send(403, {"error": "stale counter"})
                if not verify_sig(pin["key"], code, pw, ctr, sig):
                    log(f"rejected: sig invalid for pinned {code}")
                    return self._send(403, {"error": "bad signature"})
                pin["ctr"] = ctr
                save_pins(pins)
                log(f"signed re-enroll {code} ctr={ctr}")

            # Authorized (or legacy-allowed): set the broker password. List args, no shell -> no injection.
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
        self._send(200, {"ok": True, "service": "planepuck-enroll", "require_sig": REQUIRE_SIG})

    def log_message(self, *a):   # silence default access logging; we emit our own [enroll] lines
        pass

def main():
    if not TOKEN:
        sys.exit("PLANEPUCK_ENROLL_TOKEN not set (put it in /etc/planepuck/enroll.env)")
    host, _, port = BIND.partition(":")
    srv = ThreadingHTTPServer((host, int(port)), Handler)
    log(f"listening on {BIND} passwd={PASSWD_FILE} reload={' '.join(RELOAD_CMD)} "
        f"pins={PIN_FILE} require_sig={int(REQUIRE_SIG)}")
    srv.serve_forever()

if __name__ == "__main__":
    main()
