# Settings

> On-device configuration: brings up the captive portal, shows the build id, and hosts the OTA update controls.

## What it does

`SetupApp` (class `SetupApp` in `src/apps.h`, titled **"Settings"** in the launcher) is the device's
configuration screen. On enter it starts the `Provision` captive portal so you can join the puck's
hotspot and set everything up from a browser. The screen itself shows the join instructions, the
firmware version + exact build id, and the OTA status line, plus two buttons: **Check** (manual OTA
check) and **Versions** (the on-device version picker).

## Using it

**Touch.** The screen lists the two on-screen steps (join hotspot → browse to `192.168.4.1`).
Tap **Check** to poll for an update now (manual checks bypass the *Later* snooze via
`Ota::checkNow(true)`; the button greys out while a check is in flight). Tap **Versions** to open the
picker — it lists installable versions newest-first; tap any non-current row to install or **downgrade**
to it (`Ota::pushVersion(v, false)` pops the standard confirm overlay from `main.cpp`). The back chip
steps the picker → home → launcher (`onBack`).

**Physical-button focus nav** (boards with buttons; no-op on touch-only). The ring cycles **Check → Versions**,
then the picker's version rows once it's open (`focusMove`/`focusSelect`/`focusItem`); double-click activates
the focused item. On the CoreS3 the **power key** drives it: a click = NEXT, a double-click = SELECT. See
[../buttons.md](../buttons.md).

## The captive portal

Join the SoftAP **`PlanePuck-Setup`** (`Provision::apName()`), then browse to **`192.168.4.1`**
(captive DNS redirects any host there). The form (`Provision::page()` → `/save`) sets:

- **Display name** — shown on the home/clock screens and to friends (`dname`, max 16 chars).
- **Wi-Fi** — SSID (async scan populates the list) + **password**.
- **Timezone** — dropdown from the built-in `TZS[]` POSIX-TZ catalog.
- **Clock format** — 12-hour (AM/PM) or 24-hour.
- **World-clock cities** — up to `MAX_WORLD_CITIES`, picked from `TZS[]`.
- **Temperature units** — °F/mph or °C/km-h.
- **Weather cities** — up to `MAX_WEATHER_CITIES`, free-typed as a city name or US ZIP (geocoded
  on-device at runtime, *not* in the portal — the SoftAP has no upstream internet yet).
- **Your location ZIP** — US ZIP for the home weather location (blank = IP geolocation).
- **MQTT password** — only shown when zero-touch enroll is **off** (`ENROLL_URL`/`ENROLL_TOKEN` unset).

On **Save & restart**, `handleSave()` writes the fields to NVS via `Settings` and `ESP.restart()`s
(~1.5 s later) into normal mode.

> **Gotcha — "blank = keep".** A blank **Wi-Fi password** keeps the saved one (Wi-Fi is only
> rewritten when both SSID and password are non-empty); a blank **MQTT password** likewise keeps the
> stored secret. So editing one field never wipes another's credential. (Name, ZIP, units, clock
> format, and the city lists are "submit = replace" — blank clears them.)

## What it needs

Nothing to start — it works fully offline (the portal runs on the puck's own AP). `main.cpp`
**auto-enters this app on first boot** when no Wi-Fi is saved (`Settings::haveWifi()` is false). The
portal uses the Arduino core's built-in `WebServer` + `DNSServer` — no extra `lib_deps`.

## How it works

- **`Provision`** (`src/services.cpp`) — the SoftAP + captive portal: a `WebServer` on port 80, a
  `DNSServer` capturing all lookups to `192.168.4.1`, and `WIFI_AP_STA` mode so it can scan for
  networks while serving the AP. `SetupApp::onEnter` calls `Provision::start()`; `loop()` pumps
  `Provision::loop()` (which performs the deferred restart); `onExit` calls `Provision::stop()`.
- **`Settings`** (`src/services.cpp`) — persisted user config in NVS via `Preferences`, namespace
  **`"puck"`**. On-device values override the `config.h` compile-time fallbacks, and `Net`,
  `ClockService`, and `Weather` all read through it. `Settings::begin()` must run first in `setup()`.
- **`Ota`** (`src/services.cpp`) — the version picker fetches `OTA_VERSIONS_URL` via
  `Ota::requestVersions()` (background task) and reads the result with `Ota::versionsReady()` /
  `Ota::versions(out, maxN)`; `Ota::configured()` gates the buttons, `Ota::phase()`/`Ota::lastError()`
  drive the status line. With OTA off (`OTA_MANIFEST_URL` blank) both buttons grey out.
- **Build id** — the big `Firmware v<FW_VERSION>` line plus `build <FW_BUILD>` (from `git describe`,
  injected by `tools/buildinfo.py`; falls back to `"?"`). The build id is what distinguishes RC builds
  that share an `FW_VERSION`.

## Notes

The version picker's channel is **operator-controlled**: a *prod* device sees released versions only,
while a *test/beta* device (`Ota::isBeta()`, header reads "Firmware (beta)") also sees RC builds. The
channel is set via the authenticated, retained MQTT marker `fleet/channel/<code>` (`Ota::onChannel`),
cached in NVS (`Settings::beta()`) so the picker knows it before MQTT reconnects — device codes never
hit a public URL. See [../ota.md](../ota.md) and [../backend.md](../backend.md).
