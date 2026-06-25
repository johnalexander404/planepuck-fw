# Weather

> Display-only current conditions + 7-day forecast for your location or a handful of cities, from Open-Meteo.

## What it does

Shows live weather read from the always-on `Weather` service cache — the app never
fetches on the UI thread. One location gets a big detailed face; 2–`MAX_WEATHER_CITIES`
(default 4) configured cities get an equal-quarter grid. Every view also offers a
per-city 7-day forecast.

## Using it

Touch:

- **Single location** (home, or exactly one configured city): big view — city, today's
  H/L, large temperature + unit, condition, wind, and an `updated H:MM` status line.
- **2×2 grid** (2–`MAX_WEATHER_CITIES` cities): one quarter per city (city, temp, H/L,
  condition).
- **Tap a grid cell** → that city's 7-day detail (current line + day / condition / hi·lo
  rows). In the single view, a tap opens detail for index 0.
- **Back chip** (top-left) returns from detail to the list/grid (`onBack()`); from the
  list it exits to the launcher.

Physical-button focus nav (see [../buttons.md](../buttons.md)): only the **grid** takes
part. The ring cycles cells (`focusMove`), and SELECT opens the focused city's detail
(`focusSelect`). On the single-button CoreS3 that's power-key **click = NEXT**,
**double-click = SELECT**. The single-location and detail views are touch/back-chip only
(`focusCount()` returns 0 there).

## What it needs

- **Wi-Fi** — `needsNet()` is `true`; until Wi-Fi is configured `main.cpp` shows a
  "Finish setup" prompt instead of the app.
- **Open-Meteo** — no API key, plain HTTP.
- **Location** (home mode): saved **ZIP** → **IP** geolocation → `WEATHER_*` `config.h`
  fallback, resolved by `Geo::resolve()`.
- **Cities** (multi mode): free-typed in the captive portal as a city name **or** US ZIP
  (not the timezone dropdown), stored as a `[{"q":...}]` blob in `Settings`.
- **Units** — °F/°C (and mph/km·h) from `Settings::tempF()`; metric is Open-Meteo's
  default.

## How it works

- **Background updater** (`Weather::begin`): a FreeRTOS task pinned to **core 0**,
  refreshing every **60 s** (`UPDATE_MS`) or immediately on `refreshNow()` (which
  `onEnter()` calls). Blocking HTTP never touches the UI loop.
- **On-device geocoding** (`geocodeQuery`, once per city, then cached on the `WCity`): a
  5-digit US ZIP → Zippopotam; any other name → Open-Meteo's geocoder. Same-named cities
  resolve to the **most-populous** hit; a trailing `City, CC` qualifier (e.g. `Kochi, IN`)
  forces a country. Home mode skips this and uses `Geo`.
- **Fetch** (`fetchInto`): one Open-Meteo call per location for `current` +
  `daily` (`temperature_2m_max`/`_min`, `weather_code`, 7 days); parsed with ArduinoJson,
  stamped with the local fetch time by `store()`, and `version()` is bumped.
- **Display-only UI**: reads the cache via `count()` / `get(i,out)` / `latest(out)`;
  `describe()` maps a WMO `weather_code` to text. Redraws are **lazy**, gated on
  `version()` (plus the `updating()` marker), so a 60 s refresh causes no needless repaint.
- **Flicker-free**: the detailed view, grid, and detail page each compose off-screen into
  a **PSRAM `M5Canvas`** (the `scope` member, drawn via the `g` target pointer) and push
  in one `pushSprite` blit.

## Notes

- No fetching happens on the UI thread — open the app and it paints whatever is already
  cached, then a fresh pull lands on the next task cycle.
- Each `Reading` carries today's hi/lo plus parallel 7-day `fcMax`/`fcMin`/`fcCode`/`fcWday`
  arrays (`FC_DAYS` = 7; index 0 is today).
