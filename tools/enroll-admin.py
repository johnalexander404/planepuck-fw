#!/usr/bin/env python3
"""PlanePuck enroll-admin — manage the droplet's TOFU key-pin store (over SSH).

DELIBERATELY SEPARATE from tools/fleet.py. `fleet.py` does MQTT fleet ops with the broker OPERATOR
credential (confined by ACLs); pin admin instead needs DROPLET SSH — a different, more powerful
credential (it edits a root-only file and runs `sudo python3`). Keeping them apart means the fleet
tooling never carries an SSH key, and this tool never needs the MQTT operator account.

Background: the enroll server (tools/enroll-server.py) pins {key,ctr} per friend code on a device's
FIRST signed enroll (TOFU). After that, only a request signed by that key can change the code's MQTT
password. A factory-reset/erased puck regenerates its key, so the server 403s the key-mismatch — until
an operator `unpin`s the code, after which the puck re-TOFUs on its next connect. See tools/OTA-SETUP.md.

The store is a root-only JSON file on the droplet (default /etc/planepuck/enroll-keys.json), edited via
`ssh <target> sudo python3 - …`, so the SSH user must be root or a NOPASSWD sudoer that can run python3.

Auth (env or flags; flags win):
  ENROLL_SSH (required)   e.g. root@mqtt.example.com   (root, or a NOPASSWD-sudo user)
  ENROLL_PIN_FILE         default /etc/planepuck/enroll-keys.json

Examples:
  ENROLL_SSH=root@mqtt.example.com tools/enroll-admin.py pins
  ENROLL_SSH=root@mqtt.example.com tools/enroll-admin.py unpin 00F93030
"""
import argparse, os, re, shutil, subprocess, sys

DEF_PIN_FILE = "/etc/planepuck/enroll-keys.json"
HEX8 = re.compile(r"^[0-9A-Fa-f]{8}$")

# These run ON the droplet via `python3 - <pinfile> [code]` (script on stdin). They mirror the enroll
# server's atomic 0600 write so a concurrent enroll can't corrupt the store.
_PIN_LIST = r'''import json,sys
try: p=json.load(open(sys.argv[1]))
except FileNotFoundError: p={}
for c in sorted(p): print(c+"\t"+str(p[c].get("ctr","?")))
'''
_PIN_UNPIN = r'''import json,os,sys
f=sys.argv[1]; code=sys.argv[2].upper()
try: p=json.load(open(f))
except FileNotFoundError: p={}
if code not in p: print("NOTFOUND"); sys.exit(0)
p.pop(code)
t=f+".tmp.%d"%os.getpid()
with os.fdopen(os.open(t,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),"w") as g: json.dump(p,g)
os.replace(t,f); print("UNPINNED")
'''


def ssh_target(a):
    t = a.ssh or os.environ.get("ENROLL_SSH", "")
    if not t:
        sys.exit("error: no SSH target — the pin store lives on the droplet. "
                 "Set ENROLL_SSH=user@droplet (root or a NOPASSWD sudoer) or pass --ssh.")
    return t


def ssh_run(a, remote_cmd, stdin_script):
    if not shutil.which("ssh"):
        sys.exit("error: 'ssh' not found")
    r = subprocess.run(["ssh", "-T", ssh_target(a), remote_cmd],
                       input=stdin_script, text=True, capture_output=True)
    if r.returncode != 0:
        sys.exit(r.stderr.strip() or f"ssh failed ({r.returncode})")
    return r.stdout


def cmd_pins(a):
    out = ssh_run(a, f"sudo python3 - {a.pin_file}", _PIN_LIST)
    rows = [l for l in out.splitlines() if l.strip()]
    if not rows:
        print("no pinned codes (pin store empty or absent)."); return
    print(f"{'CODE':<10} CTR")
    for l in rows:
        c, _, ctr = l.partition("\t"); print(f"{c:<10} {ctr}")
    print(f"\n{len(rows)} pinned.")


def cmd_unpin(a):
    if not HEX8.match(a.code):
        sys.exit(f"'{a.code}' is not an 8-hex device code")
    code = a.code.upper()
    out = ssh_run(a, f"sudo python3 - {a.pin_file} {code}", _PIN_UNPIN).strip()
    if out == "UNPINNED":
        print(f"-> unpinned {code} — the puck re-TOFUs (re-pins a fresh key) on its next connect (~3s).")
    elif out == "NOTFOUND":
        print(f"-> {code} was not pinned (nothing to do).")
    else:
        print(out or "done")


def main():
    p = argparse.ArgumentParser(description="PlanePuck enroll pin-store admin (over SSH)")
    p.add_argument("--ssh", default=os.environ.get("ENROLL_SSH", ""),
                   help="user@droplet for the pin store (else ENROLL_SSH env); root or a NOPASSWD sudoer")
    p.add_argument("--pin-file", default=os.environ.get("ENROLL_PIN_FILE", DEF_PIN_FILE),
                   help=f"pin-store path on the droplet (default {DEF_PIN_FILE})")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("pins", help="list TOFU-pinned device codes + counters").set_defaults(fn=cmd_pins)
    pu = sub.add_parser("unpin", help="remove a code's pin so a factory-reset/erased puck can re-enroll")
    pu.add_argument("code", help="8-hex device code to unpin")
    pu.set_defaults(fn=cmd_unpin)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
