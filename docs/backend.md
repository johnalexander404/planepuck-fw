# Backend infrastructure

PlanePuck runs **fully offline** with no backend at all. The online features each need a piece of
server-side infra — run as many as you want. This page is the map; the copy-pasteable, one-time
runbook (DNS, TLS, ACLs, systemd, the CI secret list) is [`tools/OTA-SETUP.md`](../tools/OTA-SETUP.md).

Everything below fits comfortably on **one small VPS / droplet** running **Caddy** + **Mosquitto**.

## The three pieces

| Piece | Enables | Firmware config | Runs |
|---|---|---|---|
| **1. Firmware host** (HTTPS file server) | OTA updates, on-device version picker, the web installer | `FW_HOST` → `OTA_MANIFEST_URL` / `OTA_BIN_BASE` / `OTA_VERSIONS_URL` | **Caddy** `file_server` vhost at `fw.<you>` serving `/var/www/planepuck-fw/…` |
| **2. MQTT broker** | Friends/Emoji messaging, fleet telemetry, OTA push, version channels | `MQTT_HOST` (+ `MQTT_TLS 1`, port 8883) | **Mosquitto** (localhost:1883) behind Caddy's `layer4` TLS on :8883 |
| **3. Operator tooling** | rolling out releases + fleet ops | — (uses GitHub secrets) | GitHub Actions + `tools/fleet.py` on your machine |

You can do (1) without (2) — OTA works, messaging is off. Or (2) without (1) — messaging works, no OTA.

## 1. Firmware host (Caddy)
A plain HTTPS `file_server` whose Let's Encrypt cert validates against the roots already pinned in the
firmware (`MQTT_CA_CERT`). DNS `A`/`AAAA` for `fw.<you>` → the droplet, then a Caddy vhost. CI
(`release.yml`) scp's the built artifacts there: `version{suffix}.json`, `versions{suffix}.json`,
`firmware{suffix}-v<N>.bin`, the merged image, and the installer `index.html`. See
[ota.md](ota.md) for the per-board file scheme. Runbook: OTA-SETUP.md §1–2 + "First burn".

## 2. MQTT broker (Mosquitto + TLS)
Self-hosted Mosquitto with **per-device credentials** and `%u` pattern ACLs:
- **username = the device's friend code** (public, derived from the chip MAC); **password is
  per-device, stored in NVS** — never compiled in. Typed in the captive portal, or **self-provisioned**
  by the optional zero-touch **enroll** endpoint (`ENROLL_URL`/`ENROLL_TOKEN` → a small service runs
  `mosquitto_passwd`).
- TLS is terminated by Caddy's `layer4` block on :8883 → plaintext Mosquitto on localhost:1883.
- A device may read/clear only its own inbox (`pattern readwrite puck/%u/#`) and publish only as
  itself; the **`ota-operator`** account does broadcasts, fleet reads, targeted pushes, and channels.

The exact `aclfile` (incl. the `fleet/*` lines for telemetry / `fleet/ota` / `fleet/channel`) is in
OTA-SETUP.md §3 — copy it verbatim (Mosquitto rejects trailing inline comments).

## 3. Operator tooling
- **GitHub Actions** build + publish from the vault (`release.yml`), run fleet ops (`fleet.yml`), and
  publish the test-device channels (`sync-channels.yml`). Required secrets/variables (`FW_HOST`,
  `MQTT_HOST`, `OPENSKY_*`, `DEPLOY_SSH_KEY`, `DROPLET_HOST`, optional `MQTT_OPERATOR_*` /
  `ENROLL_TOKEN`; vars `FLEET_GROUPS`, `OTA_MIN_VERSION`) are listed in OTA-SETUP.md "One-time setup".
- **`tools/fleet.py`** (a `mosquitto_*` wrapper, no pip deps) — `list` / `send <code|test|prod> <ver>`
  / `broadcast` / `channel` / `sync-channels`. Set `FLEET_HOST`/`FLEET_USER`/`FLEET_PASS`
  (+ `FLEET_CAFILE=/etc/ssl/cert.pem` on macOS). Device codes are MQTT usernames — keep them out of
  public repos (the test list lives in the `FLEET_GROUPS` **secret**, never printed).

## Pointers
- Step-by-step server setup → [`tools/OTA-SETUP.md`](../tools/OTA-SETUP.md)
- How a puck updates / channels → [ota.md](ota.md)
- Releasing + CI → [build-and-deploy.md](build-and-deploy.md)
