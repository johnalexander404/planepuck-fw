#!/usr/bin/env python3
"""PlanePuck fleet CLI — see the fleet and push firmware, by device / board / kind.

Wraps the `mosquitto_sub`/`mosquitto_pub` clients (no pip deps), using an MQTT *operator*
credential. Devices publish retained telemetry this reads:

    fleet/status/<code>   {"v":<fw>,"n":"<name>","b":"<board>"}   (retained)
    fleet/online/<code>   "1" on connect / "0" via LWT on drop    (retained)
    fleet/channel/<code>  "<kind>"  (retained — the device's update channel/kind, e.g. test/prod/xyz)

and obeys commands this publishes:

    fleet/ota/<code>      {"v":<ver>,"force":0|1}                 (targeted, NOT retained)
    fleet/channel/<code>  "<kind>"                                (retained)
    puck/all/ota          "<ver>"                                 (retained broadcast nudge)

CONCEPTS
  * board = device hardware type (PUCK_BOARD_ID), e.g. m5cores3 / waveshare_1_85c_box.
  * kind  = the device's `fleet/channel` marker, an arbitrary label (test/prod/xyz/…; none ⇒ prod).
            On-device VISIBILITY: `test` (or `beta`) sees ALL builds for its board; every other kind
            sees only RELEASED builds for its board.
  Targets/kinds are read live from the broker's retained markers — no local list needed. (`groups`
  and `sync-channels` still use the optional local allow-list as a *bulk* set-markers helper.)

LEAK NOTE: device codes are MQTT usernames. `list` prints them, so run it LOCALLY or in a PRIVATE
ops repo's Actions — never in the public repo. `send --quiet` prints counts only (no codes/boards),
safe for public CI. `--json` (codes included) is for local/FE use.

Auth (operator account; ACLs: `topic read fleet/#`, `topic write fleet/ota/+` + `fleet/channel/+`,
`topic write puck/all/ota`):
  env  FLEET_HOST (required)  FLEET_PORT (8883)  FLEET_USER  FLEET_PASS  FLEET_CAFILE
  or flags --host/--port/--user/--pass/--cafile/--groups  (flags win over env)

Examples:
  tools/fleet.py list                          # CODE NAME VER BOARD KIND STATUS
  tools/fleet.py list --board m5cores3 --kind test
  tools/fleet.py list --json                   # machine-readable (for a FE backend)
  tools/fleet.py send 00F93030 18 --force      # one device, silent
  tools/fleet.py send all 18 --board m5cores3  # all CoreS3 devices (online)
  tools/fleet.py send test 18 --board m5cores3 # CoreS3 devices on the 'test' kind
  tools/fleet.py channel 00F93030 test         # mark a device's kind (retained)
  tools/fleet.py broadcast 18                  # nudge the whole fleet to recheck the manifest

The module is import-friendly: `fetch_fleet(conn, wait)`, `resolve(dev, target, board)`, and
`publish_ota(conn, codes, version, force)` take plain args and return data (a future FE backend reuses
them). `conn` is anything with .host/.port/.user/.pw/.cafile (e.g. argparse.Namespace / SimpleNamespace).
"""
import argparse, json, os, re, shutil, subprocess, sys

DEF_HOST = ""          # no hardcoded host — set FLEET_HOST env or pass --host
DEF_PORT = 8883
HEX8   = re.compile(r"^[0-9A-Fa-f]{8}$")
KIND_RE = re.compile(r"^[a-z0-9_-]{2,16}$")


def conn_args(c):
    args = ["-h", c.host, "-p", str(c.port)]
    if c.user: args += ["-u", c.user]
    if c.pw:   args += ["-P", c.pw]
    if c.cafile:         args += ["--cafile", c.cafile]
    elif c.port == 8883: args += ["--capath", "/etc/ssl/certs"]   # TLS by default on 8883
    return args


def require(tool):
    if not shutil.which(tool):
        sys.exit(f"error: '{tool}' not found — install mosquitto-clients")


def test_set(a):
    """Uppercased set of codes in the local 'test' allow-list (for `groups`/`sync-channels` only)."""
    src = {}
    if getattr(a, "groups", ""):
        with open(a.groups) as f: src = json.load(f)
    elif os.environ.get("FLEET_GROUPS", "").strip():
        try: src = json.loads(os.environ["FLEET_GROUPS"])
        except ValueError as e: sys.exit(f"FLEET_GROUPS is not valid JSON: {e}")
    else:
        default = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fleet-groups.json")
        if os.path.exists(default):
            with open(default) as f: src = json.load(f)
    return {str(c).upper() for c in src.get("test", [])}


# ---- importable core (plain args + return data; CLI wrappers below print) --------------------------

def fetch_fleet(conn, wait):
    """code -> {code,name,ver,board,online(bool|None),channel} from retained fleet/* telemetry."""
    require("mosquitto_sub")
    out = subprocess.run(
        ["mosquitto_sub", *conn_args(conn), "-t", "fleet/#", "-v", "-W", str(wait)],
        capture_output=True, text=True)
    if out.returncode not in (0, 27):    # 27 = -W timeout (expected, that's how we stop)
        sys.exit(out.stderr.strip() or f"mosquitto_sub failed ({out.returncode})")
    dev = {}
    for line in out.stdout.splitlines():
        if " " not in line: continue
        topic, _, payload = line.partition(" ")
        parts = topic.split("/")
        if len(parts) != 3 or parts[0] != "fleet": continue
        kind, code = parts[1], parts[2]
        d = dev.setdefault(code, {"code": code, "name": "", "ver": "?", "board": "?",
                                  "online": None, "channel": None})
        if kind == "status":
            try:
                j = json.loads(payload)
                d["ver"] = str(j.get("v", "?")); d["name"] = str(j.get("n", "")); d["board"] = str(j.get("b", "?"))
            except ValueError: pass
        elif kind == "online":
            d["online"] = (payload.strip() == "1")
        elif kind == "channel":
            d["channel"] = payload.strip()    # the device's retained kind (source of truth)
    return dev


def kind_of(d):
    return d.get("channel") or "prod"


def resolve(dev, target, board=None):
    """(online_codes, label, offline_count) for target = 8-hex CODE | 'all' | a kind, optionally ∩ board.
    A bare CODE is returned as-is (sent regardless of online); 'all'/kind return ONLINE matches only
    (a targeted fleet/ota is not retained, so offline devices can't act on it)."""
    if HEX8.match(target):
        return [target.upper()], f"device {target.upper()}", 0
    bf = (lambda d: True) if not board else (lambda d: d.get("board") == board)
    if target == "all":
        match = [(c, d) for c, d in dev.items() if bf(d)]
    else:
        match = [(c, d) for c, d in dev.items() if kind_of(d) == target and bf(d)]
    online  = sorted(c for c, d in match if d["online"])
    offline = sum(1 for c, d in match if not d["online"])
    lbl = ("all" if target == "all" else f"kind={target}") + (f" board={board}" if board else "")
    return online, lbl, offline


def publish_ota(conn, codes, version, force):
    """Publish fleet/ota/<code> {v,force} to each code; return (sent, failed) code lists."""
    require("mosquitto_pub")
    payload = json.dumps({"v": int(version), "force": 1 if force else 0})
    sent, failed = [], []
    for c in codes:
        r = subprocess.run(["mosquitto_pub", *conn_args(conn), "-t", f"fleet/ota/{c}", "-m", payload])
        (sent if r.returncode == 0 else failed).append(c)
    return sent, failed


def set_channel(conn, code, kind):
    return subprocess.run(["mosquitto_pub", *conn_args(conn),
                           "-t", f"fleet/channel/{code.upper()}", "-m", kind, "-r"]).returncode


# ---- CLI commands ---------------------------------------------------------------------------------

def cmd_list(a):
    dev = fetch_fleet(a, a.wait)
    rows = list(dev.values())
    if a.board: rows = [d for d in rows if d.get("board") == a.board]
    if a.kind:  rows = [d for d in rows if kind_of(d) == a.kind]
    if a.json:
        print(json.dumps([{"code": d["code"], "name": d["name"], "ver": d["ver"],
                           "board": d.get("board") or "?", "kind": kind_of(d), "online": d["online"]}
                          for d in sorted(rows, key=lambda d: d["code"])]))
        return
    if not rows:
        print("no devices match (telemetry needs firmware >= the fleet build + operator `topic read fleet/#`).")
        return
    rows.sort(key=lambda d: (d["online"] is not True, d["name"].lower(), d["code"]))
    nw = max([4] + [len(d["name"]) for d in rows])
    bw = max([5] + [len(d.get("board") or "?") for d in rows])
    print(f"{'CODE':<10} {'NAME':<{nw}} {'VER':>4}  {'BOARD':<{bw}} {'KIND':<6} STATUS")
    for d in rows:
        st = "online" if d["online"] else ("offline" if d["online"] is False else "?")
        print(f"{d['code']:<10} {d['name']:<{nw}} {('v'+d['ver']):>4}  {(d.get('board') or '?'):<{bw}} {kind_of(d):<6} {st}")
    print(f"\n{len(rows)} device(s).")


def cmd_groups(a):
    test = sorted(test_set(a))
    print(f"local 'test' allow-list ({len(test)}): {', '.join(test) or '(empty)'}")
    print("(used only by `sync-channels` to bulk-set markers; live kinds come from `fleet/channel`)")


def cmd_send(a):
    dev = fetch_fleet(a, a.wait)
    codes, label, offline = resolve(dev, a.target, a.board)
    if not codes:
        sys.exit(f"no online devices match {label!r} — set kinds with `fleet.py channel`, check `fleet.py list`")
    sent, failed = publish_ota(a, codes, a.version, a.force)
    how = "silent flash" if a.force else "Update/Later prompt"
    if a.json:
        print(json.dumps({"version": a.version, "force": bool(a.force),
                          "sent": sent, "failed": failed, "offline_skipped": offline}))
    elif a.quiet:        # public CI: counts only, never codes or boards
        print(f"-> install v{a.version} ({how}): {len(sent)}/{len(codes)} sent" +
              (f", {offline} offline skipped" if offline else ""))
    else:
        byboard = {}
        for c in codes:
            b = dev.get(c, {}).get("board", "?"); byboard[b] = byboard.get(b, 0) + 1
        brk = ", ".join(f"{n}x {b}" for b, n in sorted(byboard.items()))
        print(f"-> {label}: install v{a.version} ({how})  [{brk}" +
              (f"; {offline} offline skipped" if offline else "") + "]")
        for c in sent:   print(f"   {c}: sent")
        for c in failed: print(f"   {c}: FAILED")
        unknown = [c for c in codes if dev.get(c, {}).get("board", "?") == "?"]
        if unknown:
            print(f"   warning: {len(unknown)} device(s) have an unknown board (no telemetry) — "
                  "can't confirm a matching bin was published for them")
        print("done — devices must be online to receive (a targeted push isn't retained).")
    if failed: sys.exit(f"{len(failed)}/{len(codes)} publishes failed")


def cmd_broadcast(a):
    require("mosquitto_pub")
    r = subprocess.run(["mosquitto_pub", *conn_args(a), "-t", "puck/all/ota", "-m", str(a.version), "-r"])
    if r.returncode: sys.exit(r.returncode)
    print(f"-> broadcast: whole fleet rechecks the manifest (payload v{a.version}, retained). "
          "Each device resolves its own board's bin.")


def cmd_channel(a):
    require("mosquitto_pub")
    if not HEX8.match(a.code): sys.exit(f"'{a.code}' is not an 8-hex device code")
    kind = a.kind.lower()
    if not KIND_RE.match(kind): sys.exit(f"'{a.kind}' is not a valid kind (^[a-z0-9_-]{{2,16}}$)")
    if set_channel(a, a.code, kind): sys.exit("publish failed")
    sees = "all builds" if kind in ("test", "beta") else "released builds"
    msg = f"channel set to {kind} (sees {sees}, retained)"
    print(f"-> {msg}" if a.quiet else f"-> {a.code.upper()}: {msg}")


def cmd_sync_channels(a):
    """Bulk-set markers from the local 'test' allow-list (retained); demote reporting non-test to prod.
    Counts only — never codes — so it's safe in public CI logs."""
    require("mosquitto_pub")
    test = test_set(a)
    if not test: sys.exit("the local 'test' list is empty — add codes to fleet-groups.json / FLEET_GROUPS")
    n_test = sum(set_channel(a, c, "test") == 0 for c in sorted(test))
    others = [c for c in fetch_fleet(a, a.wait) if c.upper() not in test]
    n_prod = sum(set_channel(a, c, "prod") == 0 for c in others)
    print(f"-> channels synced: {n_test} test, {n_prod} prod (retained; new/offline non-test devices default to prod).")


def main():
    p = argparse.ArgumentParser(description="PlanePuck fleet CLI")
    p.add_argument("--host", default=os.environ.get("FLEET_HOST", DEF_HOST))
    p.add_argument("--port", type=int, default=int(os.environ.get("FLEET_PORT", DEF_PORT)))
    p.add_argument("--user", default=os.environ.get("FLEET_USER", ""))
    p.add_argument("--pass", dest="pw", default=os.environ.get("FLEET_PASS", ""))
    p.add_argument("--cafile", default=os.environ.get("FLEET_CAFILE", ""))
    p.add_argument("--groups", default="", help="local groups JSON (else FLEET_GROUPS env / tools/fleet-groups.json)")
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("list", help="show the fleet (code, name, version, board, kind, status)")
    pl.add_argument("--board", default="", help="filter by board (e.g. m5cores3)")
    pl.add_argument("--kind",  default="", help="filter by kind (e.g. test/prod/xyz)")
    pl.add_argument("--json",  action="store_true", help="machine-readable output (for a FE backend)")
    pl.add_argument("--wait",  type=int, default=3, help="seconds to collect retained telemetry")
    pl.set_defaults(fn=cmd_list)

    pg = sub.add_parser("groups", help="show the local 'test' allow-list (for sync-channels)")
    pg.set_defaults(fn=cmd_groups)

    ps = sub.add_parser("send", help="install a version on a device CODE, 'all', or a kind")
    ps.add_argument("target", help="8-hex device code | all | kind (test/prod/xyz)")
    ps.add_argument("version", type=int)
    ps.add_argument("--board", default="", help="restrict 'all'/kind targets to this board")
    ps.add_argument("--force", action="store_true", help="silent flash (no on-device confirm)")
    ps.add_argument("--quiet", action="store_true", help="counts only, no codes/boards (public CI logs)")
    ps.add_argument("--json",  action="store_true", help="machine-readable result (codes included; local/FE)")
    ps.add_argument("--wait",  type=int, default=3, help="seconds to collect telemetry")
    ps.set_defaults(fn=cmd_send)

    pb = sub.add_parser("broadcast", help="nudge the whole fleet to recheck the manifest")
    pb.add_argument("version", type=int)
    pb.set_defaults(fn=cmd_broadcast)

    pc = sub.add_parser("channel", help="set one device's kind/channel (authenticated, retained)")
    pc.add_argument("code", help="8-hex device code")
    pc.add_argument("kind", help="kind label: test/beta (see all builds) | prod | any [a-z0-9_-]{2,16}")
    pc.add_argument("--quiet", action="store_true", help="don't print the device code")
    pc.set_defaults(fn=cmd_channel)

    psc = sub.add_parser("sync-channels", help="bulk-set the local 'test' list as retained markers")
    psc.add_argument("--wait", type=int, default=3, help="seconds to collect telemetry (to demote non-test to prod)")
    psc.set_defaults(fn=cmd_sync_channels)

    a = p.parse_args()
    if not a.host:
        sys.exit("error: no broker host — set the FLEET_HOST env var or pass --host <broker-domain>")
    a.fn(a)


if __name__ == "__main__":
    main()
