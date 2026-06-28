# Private fleet-ops repo

Fleet **management** (list / send / channel / broadcast) prints and targets **device codes** — which
are MQTT usernames. On a public repo, Actions logs (and artifacts) are world-readable, so these ops
**must not run there**. The split:

| Repo | Visibility | Does |
|---|---|---|
| `planepuck-fw` (firmware) | **public** | builds every board (`release.yml` matrix), publishes OTA artifacts, and the **leak-safe** build-time nudges (final broadcast, RC→test push, `--quiet` so no codes hit the log) |
| `planepuck-ops` (this) | **private** | interactive fleet management via `fleet.yml` — private logs, so full codes are fine. Also the home for a future web-FE backend |

`fleet.py` itself stays in the **public** repo (it holds no secrets — creds come from env). This private
workflow checks it out from there, so the CLI never needs syncing. (The workflow *file* does: when
`tools/ops/fleet.yml` changes upstream — e.g. the new `pins`/`unpin` commands — re-copy it into this
repo's `.github/workflows/fleet.yml`.)

## One-time setup
1. Create a **private** repo, e.g. `planepuck-ops`.
2. Copy [`fleet.yml`](./fleet.yml) → `.github/workflows/fleet.yml` in it. If you forked the firmware
   repo, edit the `repository:` line to your fork.
3. Add repo **secrets** (Settings → Secrets and variables → Actions):
   - `MQTT_HOST` — broker domain (e.g. `mqtt.example.com`)
   - `MQTT_OPERATOR_USER` / `MQTT_OPERATOR_PASS` — the `ota-operator` account
   - `FLEET_GROUPS` *(optional)* — `{"test":["<code>",…]}`, only for the `sync-channels` bulk helper
   - `FLEET_SSH` / `FLEET_SSH_KEY` *(optional)* — only for `pins`/`unpin`: `user@droplet` (root or a
     NOPASSWD sudoer) + a private SSH key authorized there. These edit the droplet's enroll key-pin
     store over **SSH** (not MQTT), so they need droplet access, not the operator MQTT account.
4. The operator account needs the ACLs from [`../OTA-SETUP.md`](../OTA-SETUP.md) §3:
   `topic read fleet/#`, `topic write fleet/ota/+`, `topic write fleet/channel/+`, `topic write puck/all/ota`.

## Use
Actions → **fleet** → **Run workflow**, pick a `command`:
- **list** — fleet table (`--board` / `--kind` filters). Codes shown (private log).
- **send** — `target` = a device code, `all`, or a kind (`test`/`prod`/`xyz`); optional `board` filter +
  `force`. Each device resolves its own board's bin, so a push is board-safe.
- **channel** — set a device's `kind` (`target` = code, `kind` = label). `test`/`beta` see all builds;
  any other kind sees released only.
- **broadcast** — nudge the whole fleet to recheck the manifest.
- **sync-channels** — bulk-set from the `FLEET_GROUPS` secret: mark every `test` code `test` and demote
  any other reporting device to `prod`. Edit the secret + re-run to change membership.
- **pins** — list TOFU-pinned device codes + counters (reads the droplet enroll pin store over SSH).
- **unpin** — `target` = device code; clears its enroll pin so a factory-reset/erased puck (new key →
  403 key mismatch) can re-enroll on its next connect. Needs the `FLEET_SSH`/`FLEET_SSH_KEY` secrets.

Locally you can run the same tool directly: `tools/fleet.py list` / `send` / `channel` / `broadcast`
(set `FLEET_HOST`/`FLEET_USER`/`FLEET_PASS`, `FLEET_CAFILE=/etc/ssl/cert.pem` on macOS), plus
`FLEET_SSH=user@droplet tools/fleet.py pins` / `unpin <code>` for the enroll pin store, and
`fleet.py list --json` / `send --json` for machine-readable output — the basis for a future dashboard.

## Future frontend
The FE's backend lives here (or alongside): it imports `fleet.py` (`fetch_fleet` / `resolve` /
`publish_ota` take plain args and return data), reads the broker's retained `fleet/*` for live state,
and serves JSON to the dashboard — keeping broker creds and device codes server-side. The public
firmware repo is unaffected.
