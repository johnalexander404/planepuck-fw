# Build & deploy

How to build the firmware, flash a board, and ship updates over the air.

## Prerequisites
- An M5Stack **CoreS3** (or CoreS3 SE).
- [PlatformIO](https://platformio.org) — the VS Code extension, or `pip install platformio` for the CLI.
- For the optional online features + OTA: a server (see [backend.md](backend.md)).

## Configure
`src/config.h` is gitignored (it holds your secrets). Create it from the template and fill it in:
```bash
cp src/config.h.example src/config.h
```
Replace the `__FW_HOST__` / `__MQTT_HOST__` placeholders with your own domains, or blank them (`""`)
for a fully-offline puck. Add `OPENSKY_CLIENT_ID/SECRET` for flight routes if you want them. Empty
string = that feature off. (CI rebuilds `config.h` from the example + repo secrets — see below — so
the real values never live in tracked source.)

## Build & flash
```bash
pio run -e m5stack-cores3 -t upload    # build + flash over USB
pio device monitor                     # serial @ 115200
```
…or use the PlatformIO **Build** / **Upload** buttons in VS Code. The first USB flash is required
(OTA can't bootstrap a blank chip); after that, updates go OTA. No toolchain? Build the merged image
and serve the **web installer** (`tools/webinstall/`) from your firmware host — see
[`tools/OTA-SETUP.md`](../tools/OTA-SETUP.md).

## Environments (one tree, multiple boards)
[`platformio.ini`](../platformio.ini) defines one env per board; `build_src_filter` pulls in that
board's `boards/<id>/*.cpp` and the shared `src/` ([architecture.md](architecture.md)):

| Env | Board | Notes |
|---|---|---|
| `m5stack-cores3` (default) | M5Stack CoreS3 | real; wraps M5Unified |
| `waveshare_185c_box` | Waveshare 1.85C-BOX | **compiling stubs only** until the drivers land ([hardware.md](hardware.md)) |

```bash
pio run -e m5stack-cores3        # build the CoreS3 firmware
pio run -e waveshare_185c_box    # build the (stub) Waveshare firmware
```
A clean CoreS3 build is ~RAM 18% / Flash 19%.

## Releasing (shipping an OTA update)
The version is a single integer `FW_VERSION` in the tracked [`src/version.h`](../src/version.h).
`tools/release.sh` only bumps it, commits, and pushes a `fw-v<N>` tag — **it does not build or
deploy**; a GitHub Action does that.

```bash
tools/release.sh "notes"            # FINAL: auto-bump to (latest final tag)+1, tag, push
tools/release.sh <N> "notes"        # FINAL: force version N
tools/release.sh rc "notes"         # RC: candidate fw-v<N>-rc<M> (staged; not promoted to the fleet)
```

- An **RC** publishes only `firmware-v<N>.bin` + `versions.json` (installable on test devices via the
  picker / `fleet.py send`), and auto-pushes to the `test` group. It does **not** move the fleet's
  `version.json` or broadcast.
- A **FINAL** of the same `N` promotes it: writes the polled `version.json`, the merged image + web
  installer, and (if MQTT operator secrets are set) nudges the fleet.

## CI (GitHub Actions)
`.github/workflows/release.yml` (triggered by the `fw-v*` tag, or run from the Actions tab) checks
out, **reconstructs `config.h` from `config.h.example` + vault secrets**, builds with PlatformIO, and
publishes the firmware + manifest to your host (per-board paths; CoreS3 stays unsuffixed — see
[ota.md](ota.md)). One-time setup + the exact secret/variable list is in
[`tools/OTA-SETUP.md`](../tools/OTA-SETUP.md); the backend it deploys to is in [backend.md](backend.md).
Other workflows: `fleet.yml` (operator ops) and `sync-channels.yml` (publish test-device channels).

## Verify before you ship
There's no auto-rollback for a bad-but-bootable image. **Flash + bench-test the exact build** (every
app via touch, then the [buttons](buttons.md)) before cutting the FINAL that reaches the fleet.
