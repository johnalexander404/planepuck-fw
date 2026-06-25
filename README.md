# PlanePuck

A modular ESP32-S3 **"app gadget"**: a touchscreen launcher hosting small apps — **Clock, Emoji,
Weather, Flight, Friends, Settings** — over always-on services (time/NTP, auto-dimming, notifications,
weather + flight fetchers, compass, MQTT friends messaging, OTA). Touch-first with optional
**physical-button navigation**. Runs **fully offline** out of the box; Wi-Fi, timezone, and locations
are set on-device at first boot, and the optional online endpoints are compiled in (empty = off).

Built with **PlatformIO + Arduino**. All hardware sits behind a board HAL (`puck::`), so one source
tree targets the **M5Stack CoreS3** today and other boards via PlatformIO envs —
see [docs/architecture.md](docs/architecture.md).

## Fork it & build your own
Fork the repo and you have your own PlanePuck — **server endpoints are not baked into the source**.

1. **Prereqs:** an M5Stack **CoreS3** (or CoreS3 SE) + [PlatformIO](https://platformio.org) (VS Code
   extension, or `pip install platformio`).
2. **Create your config** from the template:
   ```bash
   cp src/config.h.example src/config.h
   ```
   Replace the `__FW_HOST__` / `__MQTT_HOST__` placeholders with your own domains — **or blank them**
   (`""`) for a fully-offline puck. `config.h` is gitignored (it holds your secrets); CI rebuilds it
   from the example + repo secrets.
3. **Build + flash over USB** (first flash must be USB — OTA can't bootstrap a blank chip):
   ```bash
   pio run -e m5stack-cores3 -t upload
   ```
4. **First boot** opens the on-device **Settings** captive portal (below). No `config.h` edit needed.

Full build/flash/release details: [docs/build-and-deploy.md](docs/build-and-deploy.md).

### How far do you want to take it?
Every tier is optional and additive; a blank value turns that feature off.

| Tier | You provide | Unlocks |
|------|-------------|---------|
| **0 — Offline** | nothing | Clock + world clock + on-device setup. No network at all. |
| **1 — Wi-Fi** | on-device (captive portal) | **Weather** + **Flight** — free, keyless APIs. |
| **2 — Flight routes** | `OPENSKY_CLIENT_ID/SECRET` in `config.h` (free) | Origin/destination for tracked flights. |
| **3 — Friends / Emoji** | your **MQTT broker** (`MQTT_HOST`) + per-device password | Friend-to-friend emoji pings (mutual-approval). |
| **4 — OTA + fleet** | your **firmware host** (`FW_HOST`) + GitHub Actions secrets | Over-the-air updates, version picker, fleet push. |

Tiers 3–4 need a small server (a VPS running **Mosquitto + Caddy**) — see
[docs/backend.md](docs/backend.md) and the copy-pasteable runbook in
[`tools/OTA-SETUP.md`](tools/OTA-SETUP.md).

The **Stocks** app is independent of the tiers above: with Wi-Fi it needs a free Finnhub key
([finnhub.io](https://finnhub.io)), entered **per-device in the captive portal** (Settings →
Stocks) — each gadget calls Finnhub directly with its own key, so nothing is shared or proxied.
(A `FINNHUB_API_KEY` in `config.h` works as a fallback default.) No key = the app shows a "set
the key" prompt and everything else works.

## Apps
| App | What it does | |
|---|---|---|
| **Clock** | big drifting clock + world clocks; the idle/screensaver face | [details](docs/apps/clock.md) |
| **Weather** | live conditions, multi-city grid, 7-day forecast (Open-Meteo) | [details](docs/apps/weather.md) |
| **Stocks** | searchable watchlist with ~5 s price ticks + earnings/day-range detail (Finnhub) | [details](docs/apps/stocks.md) |
| **Flight** | nearby aircraft list + radar, routes, global flight/airport search | [details](docs/apps/flight.md) |
| **Emoji** | one-tap emote to a friend or all (friends-based) | [details](docs/apps/emoji.md) |
| **Friends** | add friends by code with mutual approval + nicknames | [details](docs/apps/friends.md) |
| **Settings** | captive-portal config, build id, OTA check + version picker | [details](docs/apps/settings.md) |

## Documentation
- [Architecture](docs/architecture.md) — the `puck::` HAL, layers, how to add an app or a board
- [Build & deploy](docs/build-and-deploy.md) — envs, build, flash, `release.sh`, CI
- [Backend infrastructure](docs/backend.md) — Mosquitto + Caddy + operator tooling
- [OTA, versions & fleet](docs/ota.md) — updates, per-board isolation, version channels
- [Physical-button navigation](docs/buttons.md)
- [Hardware & boards](docs/hardware.md) — CoreS3, the Waveshare scaffold, first flash
- [Server one-time runbook](tools/OTA-SETUP.md) — DNS, TLS, ACLs, the CI secret list

## On-device setup
First boot (no Wi-Fi saved) opens **Settings** automatically: join the **`PlanePuck-Setup`** hotspot
(or browse **`192.168.4.1`**), set Wi-Fi / timezone / ZIP / weather + world-clock cities / display
name / units, then **Save & restart**. Everything persists to NVS; change it anytime from the
**Settings** app. Details + MQTT enrollment: [docs/apps/settings.md](docs/apps/settings.md).
