# PlanePuck

A modular ESP32-S3 **"app gadget"** for the **M5Stack CoreS3**: a touchscreen launcher hosting small
apps — **Clock, Emoji Ping, Weather, Flight, Friends, Settings** — over always-on services (time/NTP,
auto-dimming, notifications, weather + flight fetchers, compass, MQTT friends messaging, OTA updates).
It runs **fully offline** out of the box; Wi-Fi, timezone, and locations are set on-device at first
boot, and the optional online endpoints are compiled in (with empty = off).

Built with **PlatformIO + Arduino** (display/touch/audio/RTC via `M5Unified`).

## Fork it & build your own
This repo is self-contained — fork it and you have your own PlanePuck. **Server endpoints are not
baked into the source:** `src/config.h.example` ships with `__FW_HOST__` / `__MQTT_HOST__`
placeholders, so a fork points at *your* infrastructure, or none.

1. **Prereqs:** an M5Stack **CoreS3** (or CoreS3 SE) + [PlatformIO](https://platformio.org) — the VS
   Code extension, or `pip install platformio` for the CLI.
2. **Fork** on GitHub, then clone your fork.
3. **Create your config** from the template:
   ```bash
   cp src/config.h.example src/config.h
   ```
   In `src/config.h`, replace the host placeholders (`__FW_HOST__`, `__MQTT_HOST__`) with your own
   domains — **or blank them** (`""`) to build a fully-offline puck. Empty string = that feature off.
   (`config.h` is gitignored; it holds your secrets. The CI build reconstructs it from the example +
   repo secrets — see [`tools/OTA-SETUP.md`](tools/OTA-SETUP.md).)
4. **Build + flash over USB** (the first flash must be USB — OTA can't bootstrap a blank chip):
   ```bash
   pio run -e m5stack-cores3 -t upload      # then: pio device monitor   (115200 baud)
   ```
   …or click **Build** then **Upload** in the PlatformIO VS Code toolbar. No toolchain at all? Build
   the merged image + serve the **web installer** (`tools/webinstall/`) from your firmware host and
   flash from desktop Chrome/Edge — see `tools/OTA-SETUP.md`. After the first USB flash, updates go OTA.
5. **First boot** opens the on-device **Settings** captive portal (next section) — Wi-Fi, timezone,
   ZIP, cities, display name. No `config.h` edit needed for those.

### How far do you want to take it?
Every tier is optional and additive; a blank value turns that feature off.

| Tier | You provide | Unlocks |
|------|-------------|---------|
| **0 — Offline** | nothing | Clock + world clock + on-device setup. No network needed at all. |
| **1 — Wi-Fi** | on-device (captive portal) | **Weather** + **Flight** — both use free, keyless APIs. |
| **2 — Flight routes** | `OPENSKY_CLIENT_ID/SECRET` in `config.h` (free account) | Origin/destination for tracked flights. |
| **3 — Friends / Emoji** | your **MQTT broker** (`MQTT_HOST`) + a per-device password | Friend-to-friend emoji pings (mutual-approval). |
| **4 — OTA + fleet** | your **firmware host** (`FW_HOST`) + GitHub Actions secrets | Over-the-air updates, on-device version picker, fleet push. |

Tiers 3–4 need a small server (a VPS/droplet running **Mosquitto + Caddy**). The complete,
copy-pasteable server **and** GitHub Actions setup — DNS, TLS, ACLs, the repo secrets/variables, and
the release flow — is in **[`tools/OTA-SETUP.md`](tools/OTA-SETUP.md)**. The secrets a full CI build
expects: `FW_HOST`, `MQTT_HOST`, `OPENSKY_CLIENT_ID/SECRET`, `DEPLOY_SSH_KEY`, `DROPLET_HOST` (plus
optional `MQTT_OPERATOR_USER/PASS`, `ENROLL_TOKEN`; and repo **variables** `FLEET_GROUPS`,
`OTA_MIN_VERSION`).

## On-device setup / Settings
On a fresh puck (no Wi-Fi saved yet) the **Settings** screen opens automatically:
1. On your phone/laptop, join the hotspot **`PlanePuck-Setup`**.
2. A setup page opens (or browse to **`192.168.4.1`**).
3. Pick your network + password, choose your **timezone**, optionally enter a US **ZIP code**
   (for accurate weather), then tap **Save & restart**.

Everything is saved to flash and survives reboots. Change it anytime from the **Settings**
entry in the launcher. (No need to edit `config.h`.)

> Friends/Emoji messaging needs an MQTT broker. With **zero-touch enrollment** configured
> (`ENROLL_URL`/`ENROLL_TOKEN`), the puck provisions its own broker password automatically right
> after Wi-Fi connects — nobody types one. Otherwise, enter the per-device **MQTT password** in the
> same setup page. See `tools/OTA-SETUP.md`.

## What you'll see
- Boots into a **launcher** (a ring of chips): tap **Clock**, **Emoji**, **Weather**, **Flight**, or **Settings**.
- **Clock**: large time. Shows "local time" offline, "NTP synced" once Wi-Fi is set.
- **Weather**: live conditions from Open-Meteo (no API key). Location comes from your
  ZIP code if set in Settings, otherwise auto-detected by IP. Shows city, temperature,
  condition, wind, and how long ago it updated. Refreshes in the background every minute,
  so reopening the app shows the last reading instantly.
- **Flight**: live aircraft near you from adsb.lol (no API key), using the same location.
  - **List**: nearest planes (callsign, altitude, distance, heading arrow).
  - **Radar** (button): top-down scope plotting planes as dots by bearing + distance,
    colored by altitude, with a heading tick. Tap a dot to open its detail; tap empty to go back.
  - **Detail** (tap a plane): origin/destination airports, heading, altitude (with an up/down
    climb arrow), ground speed, distance/bearing, climbing/descending. Refreshes every ~15s.
  - **Search** (button at the bottom of the list): an on-screen keyboard to type a flight
    number (the IATA number you'd Google, e.g. `SQ31`, is auto-converted to the ICAO callsign
    `SIA31`; ICAO callsigns also work). Search is global — it finds the flight anywhere in
    ADS-B coverage, then tracks it live.
  - Origin/destination come from **OpenSky Network** (free account; set `OPENSKY_CLIENT_ID`/
    `OPENSKY_CLIENT_SECRET` in `config.h`), derived from the aircraft's actual tracked movement
    rather than a stale callsign→route table. Note: OpenSky only has *completed* flights, so a
    plane still en route shows its **most recent finished leg** ("last tracked flight"), not the
    live destination — spot-on for a just-landed or searched flight. Blank keys → "route unknown".
    ETA/schedule isn't available from free data.
- **Emoji**: tap left/right thirds to change the emote, center to "send" (beep + flash).
  With MQTT set, sending publishes to paired devices and incoming pings beep + show.
- A **"<" chip** (top-left) returns to the launcher.
- The screen **auto-dims** at night (time-based for now) and **brightens on touch**.

## Going online (optional)
Wi-Fi and timezone are set on-device (see above). Emoji messaging is **friends-based** over your own
MQTT broker (not the deprecated `GROUP_ID` broadcast): point `MQTT_HOST` at your broker in
`src/config.h`, then add friends by code on the **Friends** screen — only mutually-confirmed friends
exchange pings. Each device's MQTT **username is its friend code**; its **password** is per-device
(typed in Settings, or self-enrolled via `ENROLL_URL`/`ENROLL_TOKEN` — zero-touch). `WIFI_SSID` /
`WIFI_PASS` / `TZ_STRING` in `config.h` are only optional fallback defaults.

## Notes
- `USE_LIGHT_SENSOR` is `0` (time-of-day brightness schedule). The CoreS3 LTR-553 driver is implemented and ready — set it to `1` and tune `LUX_DARK`/`LUX_BRIGHT` only on a **full CoreS3**. The **CoreS3 SE has no ambient sensor**, so leave it `0` there.
- Emotes are ASCII for font safety; swap for bitmap glyphs later.
