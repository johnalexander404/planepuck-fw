#!/usr/bin/env python3
"""PlanePuck fleet CLI — see the fleet and push firmware to devices, the test group, or prod.

Wraps the `mosquitto_sub`/`mosquitto_pub` clients (no pip deps), using an MQTT *operator*
credential. Devices publish retained telemetry that this reads:

    fleet/status/<code>   {"v":<fw>,"n":"<name>"}   (retained)
    fleet/online/<code>   "1" on connect / "0" via LWT on drop   (retained)

and obey commands this publishes:

    fleet/ota/<code>      {"v":<ver>,"force":0|1}
    fleet/channel/<code>  "test"|"prod"  (retained — the device's update channel; replaces the public test-devices.json)

GROUPS — there are two buckets:
  * test = an explicit allow-list you maintain (codes you use for release candidates).
  * prod = the DEFAULT: every reporting device NOT in test (so a freshly-registered device is prod
           automatically — no list edit needed). `prod` is computed live from telemetry, never stored.
The 'test' list comes from (in order) --groups <file>, the FLEET_GROUPS env var (inline JSON), or
tools/fleet-groups.json next to this script:   { "test": ["DEADBEEF", "CAFEF00D"] }
`send` accepts: a group NAME (test / prod), or a single 8-hex device CODE.

Auth (operator account on the broker — needs ACL `topic read fleet/#` + `topic write fleet/ota/+`
+ `topic write fleet/channel/+`, plus `topic write puck/all/ota` for broadcast):
  env  FLEET_HOST (required: your broker domain)  FLEET_PORT (8883)  FLEET_USER  FLEET_PASS  FLEET_CAFILE
  or flags --host/--port/--user/--pass/--cafile/--groups  (flags win over env)

Examples:
  tools/fleet.py list                       # CODE / NAME / VER / STATUS / GROUP(test|prod)
  tools/fleet.py groups                     # the test allow-list (prod = everything else)
  tools/fleet.py send DEADBEEF 18           # one device  -> Update/Later prompt
  tools/fleet.py send test 18 --force       # the test group, silent
  tools/fleet.py send prod 18               # every online device not in test
  tools/fleet.py broadcast 18               # nudge the whole fleet to recheck the manifest
  tools/fleet.py channel DEADBEEF test      # put one device on the RC channel (retained, authenticated)
  tools/fleet.py sync-channels              # publish the whole 'test' list as channel markers (replaces sync-test)
"""
import argparse, json, os, re, shutil, subprocess, sys

DEF_HOST = ""          # no hardcoded host — set the FLEET_HOST env var or pass --host <broker-domain>
DEF_PORT = 8883
HEX8 = re.compile(r"^[0-9A-Fa-f]{8}$")


def conn_args(a):
    args = ["-h", a.host, "-p", str(a.port)]
    if a.user: args += ["-u", a.user]
    if a.pw:   args += ["-P", a.pw]
    if a.cafile:         args += ["--cafile", a.cafile]
    elif a.port == 8883: args += ["--capath", "/etc/ssl/certs"]   # TLS by default on 8883
    return args


def require(tool):
    if not shutil.which(tool):
        sys.exit(f"error: '{tool}' not found — install mosquitto-clients")


def test_set(a):
    """Uppercased set of codes in the 'test' allow-list (from --groups / FLEET_GROUPS / file)."""
    src = {}
    if a.groups:
        with open(a.groups) as f: src = json.load(f)
    elif os.environ.get("FLEET_GROUPS", "").strip():
        try: src = json.loads(os.environ["FLEET_GROUPS"])
        except ValueError as e: sys.exit(f"FLEET_GROUPS is not valid JSON: {e}")
    else:
        default = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fleet-groups.json")
        if os.path.exists(default):
            with open(default) as f: src = json.load(f)
    return {str(c).upper() for c in src.get("test", [])}


def fetch_fleet(a, wait):
    """code -> {code,name,ver,online(bool|None)} from retained fleet/* telemetry."""
    require("mosquitto_sub")
    out = subprocess.run(
        ["mosquitto_sub", *conn_args(a), "-t", "fleet/#", "-v", "-W", str(wait)],
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
        d = dev.setdefault(code, {"code": code, "name": "", "ver": "?", "online": None})
        if kind == "status":
            try:
                j = json.loads(payload); d["ver"] = str(j.get("v", "?")); d["name"] = str(j.get("n", ""))
            except ValueError: pass
        elif kind == "online":
            d["online"] = (payload.strip() == "1")
    return dev


def cmd_list(a):
    test = test_set(a)
    dev = fetch_fleet(a, a.wait)
    if not dev:
        print("no devices reporting (telemetry needs firmware >= the fleet build, "
              "and the operator account needs `topic read fleet/#`).")
        return
    rows = sorted(dev.values(), key=lambda d: (d["online"] is not True, d["name"].lower(), d["code"]))
    nw = max([4] + [len(d["name"]) for d in rows])
    print(f"{'CODE':<10} {'NAME':<{nw}} {'VER':>4}  {'STATUS':<8} GROUP")
    for d in rows:
        st = "online" if d["online"] else ("offline" if d["online"] is False else "?")
        grp = "test" if d["code"].upper() in test else "prod"
        print(f"{d['code']:<10} {d['name']:<{nw}} {('v'+d['ver']):>4}  {st:<8} {grp}")
    print(f"\n{len(rows)} device(s).")


def cmd_groups(a):
    test = sorted(test_set(a))
    print(f"test ({len(test)}): {', '.join(test) or '(empty)'}")
    print("prod: every reporting device NOT in 'test' (the default — new devices land here automatically)")


def cmd_send(a):
    require("mosquitto_pub")
    test = test_set(a)
    t = a.target
    if t == "prod":
        dev = fetch_fleet(a, 3)
        codes = sorted(c for c, d in dev.items() if d["online"] and c.upper() not in test)
        offline = sum(1 for c, d in dev.items() if d["online"] is False and c.upper() not in test)
        if not codes: sys.exit("no online prod devices (prod = reporting devices not in 'test')")
        label = f"prod ({len(codes)} online" + (f", {offline} offline skipped)" if offline else ")")
    elif t == "test":
        codes = sorted(test)
        if not codes: sys.exit("the 'test' group is empty — add codes to fleet-groups.json / FLEET_GROUPS")
        label = f"test ({len(codes)})"
    elif HEX8.match(t):
        codes = [t.upper()]; label = f"device {codes[0]}"
    else:
        sys.exit(f"'{t}' is not 'test', 'prod', or an 8-hex device code")
    payload = json.dumps({"v": a.version, "force": 1 if a.force else 0})
    how = "silent flash" if a.force else "Update/Later prompt"
    # --quiet (CI / public logs): never print device codes — only counts.
    if not a.quiet:
        print(f"-> {label}: install v{a.version} ({how})")
    # NOT retained: a targeted command is immediate (a retained one would re-fire on every reconnect).
    fail = 0
    for c in codes:
        r = subprocess.run(["mosquitto_pub", *conn_args(a), "-t", f"fleet/ota/{c}", "-m", payload])
        if not a.quiet:
            print(f"   {c}: {'sent' if r.returncode == 0 else 'FAILED'}")
        if r.returncode: fail += 1
    if a.quiet:
        print(f"-> install v{a.version} ({how}): {len(codes) - fail}/{len(codes)} device(s) sent")
    if fail: sys.exit(f"{fail}/{len(codes)} publishes failed")
    if not a.quiet:
        print("done — devices must be online to receive (check `fleet.py list`).")


def cmd_broadcast(a):
    require("mosquitto_pub")
    # retained nudge: any device rechecks version.json on receipt (and on next reconnect)
    r = subprocess.run(["mosquitto_pub", *conn_args(a), "-t", "puck/all/ota",
                        "-m", str(a.version), "-r"])
    if r.returncode: sys.exit(r.returncode)
    print(f"-> broadcast: whole fleet rechecks the manifest (payload v{a.version}, retained).")


def _set_channel(a, code, val):
    return subprocess.run(["mosquitto_pub", *conn_args(a),
                           "-t", f"fleet/channel/{code.upper()}", "-m", val, "-r"]).returncode


def cmd_channel(a):
    require("mosquitto_pub")
    if not HEX8.match(a.code): sys.exit(f"'{a.code}' is not an 8-hex device code")
    val = "test" if a.channel in ("test", "beta") else "prod"
    if _set_channel(a, a.code, val): sys.exit("publish failed")
    print(f"-> channel set to {val} (retained)" if a.quiet
          else f"-> {a.code.upper()}: channel set to {val} (retained)")


def cmd_sync_channels(a):
    """Authenticated replacement for the public test-devices.json: mark every 'test' device 'test'
    (retained, so it applies even when offline) and demote any reporting non-test device to 'prod'.
    Only counts are printed — never the codes — so it's safe in public CI logs."""
    require("mosquitto_pub")
    test = test_set(a)
    if not test: sys.exit("the 'test' group is empty — add codes to fleet-groups.json / FLEET_GROUPS")
    n_test = sum(_set_channel(a, c, "test") == 0 for c in sorted(test))
    others = [c for c in fetch_fleet(a, a.wait) if c.upper() not in test]   # reporting non-test devices
    n_prod = sum(_set_channel(a, c, "prod") == 0 for c in others)
    print(f"-> channels synced: {n_test} test, {n_prod} prod (retained; new/offline non-test devices default to prod).")


def main():
    p = argparse.ArgumentParser(description="PlanePuck fleet CLI")
    p.add_argument("--host", default=os.environ.get("FLEET_HOST", DEF_HOST))
    p.add_argument("--port", type=int, default=int(os.environ.get("FLEET_PORT", DEF_PORT)))
    p.add_argument("--user", default=os.environ.get("FLEET_USER", ""))
    p.add_argument("--pass", dest="pw", default=os.environ.get("FLEET_PASS", ""))
    p.add_argument("--cafile", default=os.environ.get("FLEET_CAFILE", ""))
    p.add_argument("--groups", default="", help="path to a groups JSON file (else FLEET_GROUPS env / tools/fleet-groups.json)")
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("list", help="show the fleet (code, name, version, status, group)")
    pl.add_argument("--wait", type=int, default=3, help="seconds to collect retained telemetry")
    pl.set_defaults(fn=cmd_list)

    pg = sub.add_parser("groups", help="show the 'test' allow-list (prod = everything else)")
    pg.set_defaults(fn=cmd_groups)

    ps = sub.add_parser("send", help="install a version on 'test', 'prod', or a device CODE")
    ps.add_argument("target", help="test | prod | 8-hex device code")
    ps.add_argument("version", type=int)
    ps.add_argument("--force", action="store_true", help="silent flash (no on-device confirm)")
    ps.add_argument("--quiet", action="store_true", help="don't print device codes (for CI / public Actions logs)")
    ps.set_defaults(fn=cmd_send)

    pb = sub.add_parser("broadcast", help="nudge the whole fleet to recheck the manifest")
    pb.add_argument("version", type=int)
    pb.set_defaults(fn=cmd_broadcast)

    pc = sub.add_parser("channel", help="set one device's update channel (authenticated, retained)")
    pc.add_argument("code", help="8-hex device code")
    pc.add_argument("channel", choices=["test", "beta", "prod"])
    pc.add_argument("--quiet", action="store_true", help="don't print the device code")
    pc.set_defaults(fn=cmd_channel)

    psc = sub.add_parser("sync-channels", help="publish the 'test' list as retained channel markers (replaces public test-devices.json)")
    psc.add_argument("--wait", type=int, default=3, help="seconds to collect telemetry (to find prod devices to demote)")
    psc.set_defaults(fn=cmd_sync_channels)

    a = p.parse_args()
    if not a.host:
        sys.exit("error: no broker host — set the FLEET_HOST env var or pass --host <broker-domain>")
    a.fn(a)


if __name__ == "__main__":
    main()
