# PlanePuck OTA — one-time server setup

Each puck pulls firmware over **HTTPS from your droplet** (auto-poll at boot + every
`OTA_POLL_HOURS`, plus a Settings "Check Updates" button) and, optionally, can be **nudged
instantly over MQTT**. When the manifest's `version` is greater than the device's compiled
`FW_VERSION`, the puck shows an **Update / Later** prompt and, on confirm, flashes the inactive
OTA slot and reboots. The pucks already ship OTA-capable (dual 6.5 MB app slots) — no partition
work needed.

This file is the **one-time** server setup. Day to day you just run `tools/release.sh`.

---

## 1. DNS
Add an `A`/`AAAA` record `fw.example.com` → your droplet's IP. The firmware already pins the Let's
Encrypt roots (`MQTT_CA_CERT` in `config.h`), so a Let's Encrypt cert on this host validates with
**no firmware change**.

## 2. Caddy — firmware file server
Your droplet already runs Caddy (with the `layer4` block terminating MQTT TLS on 8883 — leave that
untouched). Add a normal HTTPS file-server vhost to the Caddyfile:

```
fw.example.com {
    root * /var/www/planepuck-fw
    file_server
}
```

Then:

```bash
sudo mkdir -p /var/www/planepuck-fw/planepuck
sudo systemctl reload caddy        # Caddy auto-issues the LE cert on first request
```

This must match `OTA_MANIFEST_URL` in `config.h`:
`https://fw.example.com/planepuck/version.json`.

## 3. Mosquitto ACL — only if you want the instant MQTT push
The push is a broadcast on `puck/all/ota`. Every device must be allowed to **read** it; only your
operator account may **publish** it (device accounts — their friend codes — must NOT get publish).

Edit `/etc/mosquitto/aclfile` (Mosquitto rejects **trailing inline comments** — keep `#` lines on
their own line):

```
# OTA broadcast: every authenticated device may read this topic
topic read puck/all/ota

# Fleet telemetry: a device writes its own presence + version; reads its own targeted command + channel
pattern write fleet/status/%u
pattern write fleet/online/%u
pattern read fleet/ota/%u
pattern read fleet/channel/%u

# the operator account that rolls out updates: broadcast, plus see the fleet + push/channel to one device
user ota-operator
topic write puck/all/ota
topic read fleet/#
topic write fleet/ota/+
topic write fleet/channel/+
```

Create that operator account:

```bash
sudo mosquitto_passwd -b /etc/mosquitto/passwd ota-operator <a-strong-password>
sudo systemctl reload mosquitto
```

`puck/all/ota` is 3 topic levels, so it does **not** match the per-device `pattern write
puck/+/+/%u` rule — devices can't publish it. Good. Likewise `fleet/ota/+` is operator-only, so a
device can only *read* commands aimed at it, never send one to another device. The same holds for
`fleet/channel/+` (operator-only write; a device reads only its own `fleet/channel/%u`), so the
test/RC channel is set by you over authenticated MQTT — **device codes never appear on any public URL**.

> Skip step 3 entirely and OTA still works fully via the auto-poll; you just won't have instant
> push or fleet visibility. The push only tells pucks to "re-check the manifest now" — the manifest
> stays the single source of truth.

## Fleet management (see devices, push a specific version)

Once the `fleet/*` ACLs above are in place, every connected puck publishes retained telemetry
(`fleet/status/<code>` = `{v,n,b}` — version, name, **board id** — `fleet/online/<code>` = `1`/`0` via
Last-Will), and `tools/fleet.py` (wraps `mosquitto_sub`/`mosquitto_pub`, no pip deps) reads it and
sends targeted, **board-safe** updates (each device resolves its own board's bin from its compiled
suffix, so a push can never cross-flash):

```bash
export FLEET_USER=ota-operator FLEET_PASS=<password>      # FLEET_HOST defaults to mqtt.example.com:8883
tools/fleet.py list                          # CODE / NAME / VER / BOARD / KIND / STATUS
tools/fleet.py list --board m5cores3          # filter to one board type
tools/fleet.py list --kind test --json        # filter by kind; --json for machines / a future FE
tools/fleet.py send CAFEF00D 16               # one device -> pops Update/Later there
tools/fleet.py send CAFEF00D 16 --force        # silent flash (device must be online; downgrade allowed)
tools/fleet.py send all 18 --board m5cores3    # every online CoreS3
tools/fleet.py send test 18 --board m5cores3   # every online CoreS3 whose kind is 'test'
tools/fleet.py channel CAFEF00D test           # set a device's kind (test/prod/xyz; test/beta see RCs)
tools/fleet.py broadcast 16                    # nudge the whole fleet to recheck the manifest
```

> These commands print and target **device codes** (= MQTT usernames). Run them **locally** or from a
> **private** ops repo's Actions — never the public firmware repo, whose logs are world-readable. The
> ready-to-copy private workflow + setup is in [`ops/README.md`](ops/README.md).

### Kinds: a per-device label (test / prod / …)
A device's **kind** is a **retained, authenticated** marker `fleet/channel/<code>` it reads over its
own connection (default **`prod`** when unset). It drives two things: which devices a `send <kind>`
targets, and — binary — which builds its on-device picker shows: **`test`/`beta` see every build for
their board; any other kind sees released only**. `list` shows it in the KIND column.

Set one device's kind directly, then target it:

```bash
tools/fleet.py channel CAFEF00D test    # arbitrary label ^[a-z0-9_-]{2,16}$; 'prod' clears the marker
tools/fleet.py send test 18 --force     # every online device whose kind is 'test'
```

`send <kind>` resolves from the **live** channel markers (not a stored list), so it always matches what
`list` shows; add `--board <id>` to scope to one board type.

**Bulk helper (optional).** To set many devices at once, keep a `test` allow-list and run
`fleet.py sync-channels`: it marks every listed code `test` and demotes any reporting non-listed device
to `prod`. The list is read from `--groups <file>`, the `FLEET_GROUPS` env var (inline JSON), or
`tools/fleet-groups.json`:

```json
{ "test": ["DEADBEEF", "CAFEF00D"] }
```

`tools/fleet-groups.json` is **gitignored** (device codes are MQTT usernames — keep them out of the
public repo); copy `tools/fleet-groups.json.example` to start. In the **private ops repo's** Actions,
put the same JSON in a `FLEET_GROUPS` secret so `sync-channels` resolves there too.

### On-device version picker: who sees RC builds, and the floor
`Settings → Versions` lets a device install a specific version. To keep prod devices off in-progress
RCs, the picker filters by channel + a floor:
- **prod** (default) → only **released** versions (those with a final `fw-v<N>` tag), `≥ min`.
- **test/beta** → the full version list incl. the in-progress RC, `≥ min`. The picker header reads
  `Firmware (beta)`.

A device learns its kind from a **retained, authenticated MQTT marker** `fleet/channel/<code>` that it
reads over its own logged-in connection (ACL: a device reads only its own `fleet/channel/%u`) and
caches in NVS. **Device codes never appear on a public URL** — this replaces the old public
`test-devices.json`. Set the markers from the **private ops repo** (or locally): `fleet.py channel
<code> <kind>` for one device, or the `sync-channels` bulk helper for a whole `FLEET_GROUPS.test` list;
demote one device any time with `tools/fleet.py channel <code> prod`. The board-suffixed
`versions{suffix}.json` carries `released` + `min`; CI computes `released` from the final tags (∩ that
board's published bins) and `min` from the **`OTA_MIN_VERSION`** repo variable (blank → 1). So the
picker locks down with no per-device setup, and nothing leaks publicly.

A targeted `send` is **not retained** (it's an immediate command), so the device must be online —
check `list` first. Versions come from the board-suffixed `versions{suffix}.json` (CI regenerates each
board's from its own bins), and the device builds the download URL from the compiled `OTA_BIN_BASE` +
suffix + version, so only a *published* binary for that board can ever be installed. On-device, the
same picker lives in **Settings → Versions**.

### Broadcast not arriving? two server-side causes
The firmware path is correct (it subscribes `puck/all/ota` and routes it to the updater). If a release
doesn't pop the prompt fleet-wide, check:
1. **CI didn't publish it** — the Action only runs the push when `MQTT_OPERATOR_USER`/`MQTT_OPERATOR_PASS`
   secrets exist (else the job logs "skipping push"). Set them (see CI release below) and create the
   `ota-operator` account.
2. **The ACL denies the read** — without `topic read puck/all/ota` Mosquitto silently drops the
   device's subscription. Verify from a laptop with a device's creds:
   `mosquitto_sub -h mqtt.example.com -p 8883 --capath /etc/ssl/certs -u <code> -P <pw> -t puck/all/ota`
   then publish from the operator and confirm it arrives.

---

## Rolling out an update
From the repo:

```bash
tools/release.sh "short notes"        # auto-bump to (latest final fw-v tag)+1, commit, push the tag
tools/release.sh 7 "short notes"      # or force a specific version
```

### From GitHub Actions instead of a local shell
The **public** firmware repo runs only the **build/release** workflow; all fleet *management* (which
prints/targets device codes) moved to a **private** ops repo — see [`ops/README.md`](ops/README.md).

- **release** (public) → *Run workflow*: inputs `version` (blank = next), `rc` (checkbox), `notes`. It
  bumps `version.h` + tags, a **per-board matrix** builds + publishes every board, then a finalise step
  runs (RC → auto-push to the `test` kind with `--quiet` so no codes hit the log; final → fleet
  broadcast + the board-chooser installer page). RC-gated exactly like a pushed tag. Equivalent to
  `tools/release.sh [rc] [version] "notes"`. CLI: `gh workflow run release -f version=18 -f rc=true -f notes=…`.
- **fleet** (private `planepuck-ops` repo) → *Run workflow*: `command` = `list` / `send` / `channel` /
  `broadcast`. Runs `tools/fleet.py` (checked out from this public repo) against the broker with the
  operator secrets from that repo's vault. `list` prints full codes — fine, the logs are private. Copy
  the template + set secrets per [`ops/README.md`](ops/README.md).

### Staged release candidates (test before promoting to the fleet)
A normal cut above is a **final** — it moves the fleet's `version.json` to the new version, so every
device is offered it. To test a build on a few devices first, cut a **release candidate**:

```bash
tools/release.sh rc "short notes"     # tag fw-v<N>-rc<M> for the next version N
```

Each board's RC build publishes its `firmware{suffix}-v<N>.bin` + adds N to that board's
`versions{suffix}.json` (it does **not** move the fleet's `version{suffix}.json`/manifest, and sends no
broadcast) — and then the finalise job **auto-deploys to the `test` kind**: CI runs `fleet.py send test
N --force --quiet` so every online device marked `test` flashes it immediately (each resolves its own
board's bin, so one push is board-safe). Prereqs: the `MQTT_OPERATOR_*` secrets + at least one device
marked `test` (set with `fleet.py channel`/`sync-channels` from the private ops repo; else the RC is
still published, just not auto-pushed — install it manually). So the two-job loop:

1. **Cut RC → test:** `tools/release.sh rc "notes"` (or `gh workflow run release -f rc=true -f notes=…`).
   Tags `fw-v<N>-rc<M>`, builds every board, publishes the bins, and flashes the **test** kind across
   all boards. The rest of the fleet is untouched. (Re-run to continue: `release.sh rc` → the next
   `rc<M>` on the same N.)
2. **Promote → prod:** when happy, `tools/release.sh "notes"` (no `rc`) or
   `gh workflow run release -f notes="promote"`. With no version given it **finalises the in-flight
   candidate** (cuts `fw-v<N>` for the same N, the same tested commit) → CI sets each board's
   `version{suffix}.json` = N + installer + broadcast → the whole fleet (incl. test) auto-updates.
   *(Gradual prod: `fleet.py send prod N` after the final cut.)*

> Auto-push reaches only test devices that are **online** when CI runs (targeted commands aren't
> retained). Offline ones get it on the next `fleet.py send test N` or, once promoted, the normal poll.

`release.sh` only bumps `src/version.h`, commits, and pushes a `fw-v<N>` tag — it does **not** build
or deploy. The tag triggers the **`release` GitHub Action**, which builds in the cloud and publishes
to the droplet: `firmware-v<N>.bin` + `version.json` (OTA), plus `planepuck-merged.bin` +
`manifest.json` + `index.html` (the web installer), and — if the MQTT operator secrets are set — the
instant push. See "CI release" below for the one-time secret setup.

**Always bench-test the exact `.bin` on one unit before publishing the manifest** — a download
failure is safe (the running firmware is untouched), but a flashed-but-crashing image needs USB
recovery (`pio run -t upload`). There is no automatic rollback in v1.

## Security note (v1)
HTTPS + the pinned CA authenticate the **server**, not the **binary** — anyone who can write the
droplet's web root could serve a bad image. The hardening path is **signed OTA** (sign the `.bin`
locally, ship a public key in firmware, verify before the boot-switch). Not in v1.

> Also note: the published `firmware-v<N>.bin` has the **OpenSky client secret compiled in** (it's a
> `#define` string), so anyone who downloads it can `strings` it out. The fw URL is unlisted (no
> directory index) and the secret is a low-value rate-limited key, but if that matters, proxy OpenSky
> through the droplet instead of baking the secret into firmware. (This is independent of CI — it's
> true of any build.)

---

# CI release (GitHub Actions) — build in the cloud, secrets from the vault

Instead of building/publishing locally, `tools/release.sh` just bumps `src/version.h`, commits, and
pushes a `fw-v<N>` tag; the **`.github/workflows/release.yml`** Action (triggered by that tag) checks
out, reconstructs `config.h` from `config.h.example` + vault secrets, builds with PlatformIO, and
publishes the firmware + manifest to the droplet (same paths as the manual flow).

## One-time setup
1. **Create the repo on GitHub** and point the local repo at it:
   `git remote add origin git@github.com:<you>/planepuck.git`
2. **Deploy SSH key** (lets the Action write to the droplet): generate a dedicated keypair, add the
   **public** key to the droplet's `~/.ssh/authorized_keys`, keep the **private** key for the vault:
   `ssh-keygen -t ed25519 -f deploy_key -N "" -C planepuck-ci`
   `ssh-copy-id -i deploy_key.pub root@fw.example.com`   (or paste deploy_key.pub into authorized_keys)
3. **Add repo secrets** (Settings → Secrets and variables → Actions → New repository secret):
   - `FW_HOST` — your firmware/HTTPS host (e.g. `fw.example.com`); CI substitutes it for the
     `__FW_HOST__` placeholder, building the OTA + enroll URLs in `config.h`.
   - `MQTT_HOST` — your broker domain (e.g. `mqtt.example.com`); replaces `__MQTT_HOST__`. Both host
     secrets keep the real endpoints out of the tracked source — omit one and that feature's URLs
     come out blank (so OTA / MQTT is simply off in that build).
   - `OPENSKY_CLIENT_ID`, `OPENSKY_CLIENT_SECRET` — injected into `config.h` at build.
   - `DEPLOY_SSH_KEY` — the **private** key from step 2 (whole file, incl. BEGIN/END lines).
   - `DROPLET_HOST` — e.g. `root@fw.example.com` (must resolve to the droplet; a bare apex with no DNS won't work).
   - *(optional, for instant push)* `MQTT_OPERATOR_USER` + `MQTT_OPERATOR_PASS` — an account allowed to
     publish `puck/all/ota`. Omit to let pucks pick the update up on their next poll.
   - *(optional, for zero-touch enrollment)* `ENROLL_TOKEN` — the **same** value as the enroll
     server's `/etc/planepuck/enroll.env`. Set it to compile self-enroll into CI builds; omit and
     pucks fall back to a typed MQTT password. Generate with `openssl rand -hex 32`. See
     "Zero-touch enrollment" below.
   - *(optional, for the Spotify app)* `SPOTIFY_CLIENT_ID` — your Spotify app's **Client ID** (public,
     PKCE — there is no secret). Injected into `config.h` and the published login page. Omit and the
     Spotify app shows "Link Spotify". See "Spotify linking" below.

## Cutting a release
```
tools/release.sh "world clock + screensaver"   # auto-bumps to (latest fw-v* tag)+1, commits, pushes the tag
```
Then watch the **Actions** tab: the `release` job builds v<N>, publishes `firmware-v<N>.bin` +
`version.json`, and (if MQTT secrets are set) nudges the fleet. Pass an explicit number to force one:
`tools/release.sh 9 "notes"`.

Notes:
- The Action reconstructs `config.h` **from `config.h.example`** — so keep non-secret config there
  (anything you change in your local `config.h` that isn't a secret must also land in the example, or
  CI will build with the old value).
- Host-key trust uses `ssh-keyscan` (trust-on-first-use). To pin it, add a `DROPLET_KNOWN_HOSTS`
  secret and write it to `~/.ssh/known_hosts` in the workflow instead.
- The deploy key can write the droplet web root — scope it (a dedicated user / restricted key) if you
  don't want a broad root key in the vault.

---

# First burn — the web installer (ESP Web Tools)

OTA can't flash a **blank** CoreS3 (it only replaces an already-running PlanePuck image), so the very
first flash is over USB. The repeatable, no-toolchain way to do it is the browser installer:
`tools/webinstall/index.html` uses ESP Web Tools to flash the **merged** image over Web Serial.

It needs three files in the droplet web root, all published automatically by the `release` Action:
- `https://fw.example.com/` → `index.html` (the installer page, from `tools/webinstall/index.html`)
- `https://fw.example.com/planepuck/manifest.json` (ESP Web Tools manifest)
- `https://fw.example.com/planepuck/planepuck-merged.bin` (bootloader + partition table + app in one image)

Caddy already serves these once the `fw.example.com` site exists — use the block in
`tools/enroll.Caddyfile` (it serves the static files **and** adds the `/enroll` route below), then
`sudo systemctl reload caddy`.

**To flash a fresh puck:** open `https://fw.example.com/` in **desktop Chrome or Edge**, plug the CoreS3
in with a **data** USB-C cable, click **Install PlanePuck**, pick the serial port, and accept
**Erase** on a new device (~1–2 min). It reboots into the captive portal; the recipient joins
`PlanePuck-Setup` and enters Wi-Fi — and with enrollment on (below), nothing else.

> The merged `.bin` is downloadable (it's what the browser flashes) and, like every published build,
> contains the compiled-in secrets (OpenSky key, and the enroll token if enabled). Keep the installer
> URL unlisted if that matters; this is the same exposure as the OTA `.bin` (see the security note above).

---

# Zero-touch enrollment — the MQTT password sets itself

With `ENROLL_URL`/`ENROLL_TOKEN` set in the firmware, a fresh puck provisions its own MQTT password:
once Wi-Fi + the clock are up it generates a random password and `POST`s `{code, pw}` (bearer
`ENROLL_TOKEN`) to `https://fw.example.com/enroll`; a small service registers it with `mosquitto_passwd`
and the puck stores it in NVS and connects as user `<code>`. **Nobody types an MQTT password.** Skip
this whole section to keep the manual model (type the password in the Settings captive portal).

## 1. Pick the enroll token
Generate one and use the **same** value in two places:
```bash
openssl rand -hex 32
```
- firmware: `ENROLL_TOKEN` in `config.h` (or the `ENROLL_TOKEN` repo secret for CI builds)
- server:   `/etc/planepuck/enroll.env` (next step)

## 2. Install the enroll service
```bash
sudo install -d -m 755 /opt/planepuck
sudo install -m 755 tools/enroll-server.py /opt/planepuck/enroll-server.py
sudo install -d -m 700 /etc/planepuck
printf 'PLANEPUCK_ENROLL_TOKEN=%s\n' "<your-token>" | sudo tee /etc/planepuck/enroll.env >/dev/null
sudo chmod 600 /etc/planepuck/enroll.env
sudo touch /etc/planepuck/enroll-keys.json   # TOFU key-pin store (root-only; the service writes it atomically)
sudo chmod 600 /etc/planepuck/enroll-keys.json
sudo install -m 644 tools/planepuck-enroll.service /etc/systemd/system/planepuck-enroll.service
sudo systemctl daemon-reload
sudo systemctl enable --now planepuck-enroll
systemctl status planepuck-enroll --no-pager
```
Make sure the Mosquitto password file exists and is owned correctly. Newer Mosquitto **refuses to
load a passwd file that isn't owned by `root`** (it warns "owner is not root"), so set owner `root`
(the root enroll service still writes it) with group `mosquitto` + mode `640` so the broker can read
it on reload:
```bash
sudo touch /etc/mosquitto/passwd
sudo chown root:mosquitto /etc/mosquitto/passwd
sudo chmod 640 /etc/mosquitto/passwd
```
(Running enroll as a non-root user instead? Use `chmod 660` and add that user to the `mosquitto`
group — see the locked-down note in `planepuck-enroll.service`.)

## 3. Route /enroll through Caddy
Use the `fw.example.com` block in `tools/enroll.Caddyfile` (adds `handle /enroll → reverse_proxy
127.0.0.1:8090` in front of the file server), then `sudo systemctl reload caddy`. Verify:
```bash
curl https://fw.example.com/enroll                                                  # {"ok": true, ...}  (health)
curl -X POST https://fw.example.com/enroll -H 'Authorization: Bearer WRONG' -d '{}' # 401 unauthorized
```
End to end: flash a puck, set Wi-Fi, then watch `journalctl -u planepuck-enroll -f` for
`[enroll] enrolled <code>`. The puck's Friends screen flips to connected within a few seconds.

## Locked-down alternative (don't run the service as root)
Run it as a dedicated user and grant only what it needs:
```bash
sudo useradd -r -s /usr/sbin/nologin puckenroll
sudo usermod -aG mosquitto puckenroll            # write the (group-writable) passwd file
sudo visudo -f /etc/sudoers.d/planepuck-enroll   # add the line below:
#   puckenroll ALL=(root) NOPASSWD: /usr/bin/systemctl reload mosquitto
```
Then in the unit set `User=puckenroll`, `Environment="PLANEPUCK_RELOAD_CMD=sudo systemctl reload
mosquitto"` (quote the WHOLE NAME=VALUE — systemd splits `Environment=` on spaces, so an unquoted value
keeps only `sudo` and the broker never reloads → silent enroll, every login fails MQTT state=5), and
change `ReadWritePaths=/etc/mosquitto` to the passwd file as needed.

## Security model (read this)
Enrollment is **TOFU key-pinned + signed**. Each puck generates a per-device HMAC key `K` (256-bit) on
first boot, stored only in NVS, and **signs** every enroll request (`sig = HMAC-SHA256(K, code\npw\nctr)`).
The server pins `{key, ctr}` per friend code on the **first** enroll; afterwards it only accepts a request
**signed by that pinned key**, with a strictly-increasing counter. The pin store is `/etc/planepuck/enroll-keys.json`
(root-only).
- **What the shared token is now**: just a coarse gate + rate-limit/fail2ban anchor — it no longer
  authorizes setting *any* code's password. So even though it's **compiled into the (downloadable)
  firmware** and thus semi-public, a leaked token can no longer reset an already-enrolled puck: that needs
  `K`, which is per-device and never in the shared binary. Still worth rotating post-provisioning.
- **What a leaked token *can* still do (residual TOFU race)**: pre-pin a code that has **never enrolled**,
  *before* that device's first boot (the attacker would need to know its MAC and beat it to `/enroll`).
  Bounds: the token gate + rate-limiting slow it, the `%u` ACL confines each account to `puck/<code>/#`,
  and the operator can **unpin** to recover. This shrinks exposure from "any code, any time" to "only
  never-yet-enrolled codes, before first boot." Fully closing it needs factory key provisioning
  (flash-encryption / DS-peripheral), which was rejected to keep the web installer working.
- **Replay**: the strictly-increasing `ctr` rejects a replayed signed request (HTTPS already prevents capture).
- **Re-flash / full-erase recovery**: OTA and a normal USB app-flash keep NVS, so `K` survives and the puck
  reconnects with no re-enroll. A full `erase_flash` wipes `K` → the device makes a new `K` → the server sees
  a **key mismatch** for the pinned code and returns **403**. Run the **unpin** below, then it re-TOFUs on its
  next connect cycle:
  ```bash
  # list pinned codes:
  sudo python3 -c "import json;print('\n'.join(json.load(open('/etc/planepuck/enroll-keys.json'))))"
  # unpin ONE code (replace 00F93030) so an erased/re-flashed puck can re-pin:
  sudo python3 - <<'EOF'
  import json,os; f="/etc/planepuck/enroll-keys.json"
  p=json.load(open(f)); p.pop("00F93030",None)
  t=f+".tmp"; json.dump(p,open(os.open(t,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),"w")); os.replace(t,f)
  EOF
  ```
- **Migration (no flag day)**: the server starts with `PLANEPUCK_REQUIRE_SIG=0` — already-deployed
  old-firmware pucks keep working with their existing passwords (legacy *unsigned* enrolls are accepted for
  unpinned codes and **not** pinned). Each puck TOFU-pins itself the first time it sends a signed enroll
  (its proactive pin-on-upgrade on the first boot of the new firmware). Once `journalctl -u planepuck-enroll`
  shows the whole fleet has produced signed enrolls, set `PLANEPUCK_REQUIRE_SIG=1` in the unit + restart to
  **reject all unsigned** enrolls. Recover a stuck unit via the captive-portal MQTT-password override.
- **Rate limiting**: the service has a coarse global limiter; for per-IP limits use the caddy-ratelimit
  plugin or fail2ban on its `journalctl -u planepuck-enroll` "bad or missing token" / "rejected:" lines.
- **Back up** `/etc/planepuck/enroll-keys.json` alongside `enroll.env`: losing it re-opens the TOFU window
  (every device re-pins its genuine `K` on the next signed enroll, which is benign for real devices).

---

# Spotify linking — the "Now Playing" app

The Spotify app needs each user's account linked once. There's **no client secret** anywhere: the device
uses Authorization Code + **PKCE** and stores only a per-device **refresh token** in NVS.

## 1. Register a Spotify app (one-time, operator)
At [developer.spotify.com/dashboard](https://developer.spotify.com/dashboard) create an app and note its
**Client ID** (public — there is no secret to keep). Under the app's settings, add the **Redirect URI**
exactly:
```
https://fw.example.com/spotify/
```
(your `FW_HOST`, trailing slash required). Request scopes are set by the page itself:
`user-read-currently-playing user-read-playback-state user-modify-playback-state`.

> **Dev-mode cap:** a new Spotify app is in *development mode* and only works for up to **25 users you
> add manually** in the dashboard. For a wider fleet, apply for Spotify's **extended quota** review.

## 2. Set the CI secret
Add `SPOTIFY_CLIENT_ID` to the repo's Actions secrets (the value is public, but kept with the others so
`config.h` + the login page are filled at build). Omit it and the Spotify app simply shows "Link Spotify".
CI injects it into `config.h` and `sed`-fills `tools/spotifylogin/index.html`, publishing it to
`https://<FW_HOST>/spotify/` alongside the web installer (the `release` Action's page-publish step).

## 3. Each user links their account
1. Open `https://fw.example.com/spotify/` in any browser and tap **Log in with Spotify**. (The captive
   portal's **Spotify** section links straight to this page once `SPOTIFY_CLIENT_ID` is set — but the
   `PlanePuck-Setup` AP has no internet, so open the link on another device or after rejoining Wi-Fi.)
2. After approving, the page shows a **refresh token** — tap **Copy**.
3. On the puck: open the **Settings** app (this starts the `PlanePuck-Setup` hotspot; opening the
   **Spotify** app while unlinked also routes there). Join `PlanePuck-Setup` → `192.168.4.1` →
   **Spotify** → paste the token → **Save** (the puck restarts).

The device refreshes its own access tokens from there (and re-saves the rotated refresh token). To
**re-link** (revoked/expired token), just repeat step 3 — the app falls back to the "Link Spotify" prompt
automatically when its token is rejected. **Playback controls need an active Spotify Connect device** (a
phone/desktop/speaker currently playing); with none, play/pause/skip show "no active device".
