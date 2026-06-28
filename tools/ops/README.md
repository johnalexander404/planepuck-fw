# Private fleet-ops repo

Fleet **management** (list / send / channel / broadcast) prints and targets **device codes** — which
are MQTT usernames. On a public repo, Actions logs (and artifacts) are world-readable, so these ops
**must not run there**. The split:

| Repo | Visibility | Does |
|---|---|---|
| `planepuck-fw` (firmware) | **public** | builds every board (`release.yml` matrix), publishes OTA artifacts, and the **leak-safe** build-time nudges (final broadcast, RC→test push, `--quiet` so no codes hit the log) |
| `planepuck-ops` (this) | **private** | interactive fleet management — **one workflow per action** (`fleet-list` / `fleet-send` / `fleet-channel` / `fleet-broadcast` / `fleet-sync-channels`) + `enroll-pins` / `enroll-unpin` for key-pin recovery — private logs, so full codes are fine. Also the home for a future web-FE backend |

The tools (`fleet.py`, `enroll-admin.py`) stay in the **public** repo (they hold no secrets — creds come
from env). These private workflows check them out from there, so the CLIs never need syncing — but the
workflow *files* do: re-copy the `tools/ops/*.yml` you use when they change upstream.

## One-time setup
1. Create a **private** repo, e.g. `planepuck-ops`.
2. Copy the per-action workflow files you want from [`tools/ops/`](.) → `.github/workflows/` in it —
   each is **one focused action** showing only the inputs it needs:
   `fleet-list.yml`, `fleet-send.yml`, `fleet-channel.yml`, `fleet-broadcast.yml`,
   `fleet-sync-channels.yml`, and `enroll-pins.yml` / `enroll-unpin.yml` (separate creds — see their
   section below). If you forked the firmware repo, edit each file's `repository:` line to your fork.
3. Add repo **secrets** (Settings → Secrets and variables → Actions):
   - `MQTT_HOST` — broker domain (e.g. `mqtt.example.com`)
   - `MQTT_OPERATOR_USER` / `MQTT_OPERATOR_PASS` — the `ota-operator` account
   - `FLEET_GROUPS` *(optional)* — `{"test":["<code>",…]}`, only for `fleet-sync-channels`
4. The operator account needs the ACLs from [`../OTA-SETUP.md`](../OTA-SETUP.md) §3:
   `topic read fleet/#`, `topic write fleet/ota/+`, `topic write fleet/channel/+`, `topic write puck/all/ota`.

## Use
Each action is its **own** entry in the Actions sidebar (Run workflow → only that action's inputs):
- **fleet-list** — fleet table; optional `board` / `kind` filters. Codes shown (private log).
- **fleet-send** — install a version: `target` = device code | `all` | kind; `version`; optional `board`
  + `force`. Each device resolves its own board's bin, so a push is board-safe.
- **fleet-channel** — set one device's `kind` (`code` + `kind`). `test`/`beta` see all builds; any other
  kind sees released only.
- **fleet-broadcast** — nudge the whole fleet to recheck the manifest (`version`).
- **fleet-sync-channels** — bulk-set from the `FLEET_GROUPS` secret: mark every `test` code `test` and
  demote any other reporting device to `prod`. Edit the secret + re-run to change membership.

Locally you can run the same CLI directly: `tools/fleet.py list` / `send` / `channel` / `broadcast` /
`sync-channels` (set `FLEET_HOST`/`FLEET_USER`/`FLEET_PASS`, `FLEET_CAFILE=/etc/ssl/cert.pem` on macOS),
and `fleet.py list --json` / `send --json` for machine-readable output — the basis for a future dashboard.

## Enroll pin admin (separate tool + workflows)
Managing the enroll **key-pin store** uses its own tool (`enroll-admin.py`) and its own per-action
workflows (`enroll-pins`, `enroll-unpin`) because it needs **droplet SSH**, not the MQTT operator account
— a different and more powerful credential. Keeping it separate means the fleet workflows never carry an
SSH key.

1. Copy [`enroll-pins.yml`](./enroll-pins.yml) and/or [`enroll-unpin.yml`](./enroll-unpin.yml) →
   `.github/workflows/`.
2. Add secrets: `ENROLL_SSH` = `user@droplet` (root or a NOPASSWD sudoer that can run `python3`) and
   `ENROLL_SSH_KEY` = a private key authorized for that user.
3. Actions → **enroll-pins** (no inputs — lists pinned codes) or **enroll-unpin** (`code` input).

`unpin <code>` clears a code's pin so a factory-reset/erased puck (which regenerated its key → 403 key
mismatch) can re-enroll on its next connect. Locally: `ENROLL_SSH=root@droplet tools/enroll-admin.py pins`
/ `unpin <code>`.

> Heads-up: an SSH key with root/sudo on the droplet is far more powerful than the ACL-confined MQTT
> operator account. For a rare recovery action, prefer running `enroll-admin.py` **locally**; only wire
> the CI secrets if you need remote operators — ideally with a `command="…"` forced-command (or scoped
> sudoers) restricting that key to just the pin-store edit.

## Future frontend
The FE's backend lives here (or alongside): it imports `fleet.py` (`fetch_fleet` / `resolve` /
`publish_ota` take plain args and return data), reads the broker's retained `fleet/*` for live state,
and serves JSON to the dashboard — keeping broker creds and device codes server-side. The public
firmware repo is unaffected.
