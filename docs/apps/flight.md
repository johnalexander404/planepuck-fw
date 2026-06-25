# Flight
> Live nearby aircraft on a list, a pannable/zoomable radar, and a per-flight tracker — powered by free ADS-B feeds, no map tiles.

## What it does
Shows real aircraft around you (or around a searched airport/flight). A background task pulls a
nearest-first list from adsb.lol every ~15 s; the UI reads the cache and never blocks. From the list
you open a heading-up **radar** scope, tap a plane for **detail** (route + altitude/speed/climb), or
**search** a flight number to track that aircraft or an airport code to recenter the scope there.

The class is `FlightApp` in [`src/apps.h`](../../src/apps.h); the data lives in the `Flight` service
(`namespace Flight` in [`src/services.cpp`](../../src/services.cpp) /
[`src/services.h`](../../src/services.h)), with `Compass` driving heading-up and `Geo` providing the
radar center (there is **no GPS** — center is the IP/ZIP location).

## Modes
`enum Mode { LIST, RADAR, DETAIL, SEARCH, HISTORY }`.

- **LIST** — nearest planes: callsign, altitude, distance, a heading tick. Bottom buttons `Radar` / `Search`.
- **RADAR** — polar scope (`drawRadar`). Each plane is an aircraft glyph at its `dist`/`bearing`,
  **colored by altitude** (`altColor`: grey ≤0 / yellow <10k / green <25k / cyan above), with a
  heading tick + callsign. Nearby **airports** are hollow light-blue squares with IATA labels
  (reference markers only). Two-finger **pinch-to-zoom** ranges `RADAR_MIN_NM` (3) → `RADAR_MAX_NM`
  (150 nm); zooming out **widens the live fetch** (`Flight::setRange`, debounced to gesture-end).
  One-finger **drag re-centers**: the range rings + crosshair stay *fixed* at the screen center and
  the world slides under them, so distances then read from the new spot. **RESET** (top-right) clears
  zoom+pan and re-fits. Scope is **heading-up** when `Compass` is calibrated, else **north-up**; a
  `CAL` button (bottom-right, only if a magnetometer is present) starts figure-8 calibration. The
  whole scope composes off-screen into a PSRAM `M5Canvas` (`scope`) and pushes in one blit, so a data
  refresh or heading turn never flickers (the cal screen is the one part drawn directly).
- **DETAIL** — one plane (`drawDetail`): big callsign, route, heading arrow + type, altitude with a
  climb/descend arrow (from `vrate`), ground speed, lat/lon, distance/bearing. Tap or back returns
  to wherever it was opened from (LIST or RADAR).
- **SEARCH** — on-screen keyboard (`drawSearch`). `GO` auto-detects the input (`runSearch`): an
  all-letters **3/4-char code** → `Flight::searchAirport` (airport-centered radar); anything else →
  `Flight::toCallsign` (IATA→ICAO, e.g. `SQ31`→`SIA31`) then `Flight::track` (plane-centered radar).
  `DEL` / `HIST` round out the bottom row.
- **HISTORY** — recent searches (`HIST_MAX` = 7), persisted in NVS via `Settings::searchHistory`.
  Tap a row to re-run it; `Keys` returns to the keyboard, `Clear` empties the list.

### The three radar "centers"
1. **You-centered** (default) — you at the scope center over the nearby snapshot; may be heading-up.
2. **Plane-centered** — `searched && Flight::tracked()`: the searched aircraft sits at center
   (forced north-up), airports near *it* are the markers (`planeAirports`), a `you` marker shows your
   bearing/distance from it, and `drawTrackedOverlay` cards the callsign/route/alt/speed/distance.
3. **Airport-centered** — `aptMode && Flight::airportCenter()`: the airport at center, planes around
   it, a `you` reference marker, code/your-distance card.

## Using it
Touch-driven. The radar is the one screen that reads the touch panel **directly**
(`puck::Touch::count()` + `get(0/1)`) because a pinch needs two points and the global one-tap `gTap`
carries only one; a drag is told from a tap via the `clicked` edge flag.

Physical-button focus nav (see [`../buttons.md`](../buttons.md)) applies to **LIST only**: the focus
ring cycles plane rows + the `Radar`/`Search` buttons (`focusItem`/`focusMove`/`focusSelect`), and
select acts as a tap. RADAR (gesture), SEARCH/HISTORY (keyboard), and DETAIL (tap-to-back) are
touch-only. On the CoreS3 the single physical key is the power button: **short-click = NEXT**,
**double-click = SELECT** (there is no button BACK — use the on-screen back chip).

## What it needs
- **Wi-Fi.** `needsNet()` returns `true`, so until Wi-Fi is configured the launcher shows a
  "Finish setup" prompt instead of the app.
- **Nearby planes:** adsb.lol — **no API key**.
- **Routes:** OpenSky Network (free, OAuth2). Set `OPENSKY_CLIENT_ID` / `OPENSKY_CLIENT_SECRET` in
  `config.h`; **blank → every route shows "route unknown"** (planes still appear).
- **Airports:** OpenStreetMap Overpass API — **no key**.
- **`FLIGHT_RADIUS_NM`** (default 30) is the base search radius; the radar's zoom-out widens it
  (never below the default, capped at 150).
- **Optional magnetometer** for heading-up. Not all CoreS3 units have one (the IMU is the 6-axis
  BMI270); `Compass::available()` is false on those → radar stays north-up. Tune with
  `HEADING_OFFSET_DEG` (axis fix) and `MAG_DECLINATION_DEG` (magnetic→true) in `config.h`.

## How it works
The `Flight` background task (`updaterTask`, pinned to **core 0**, 20 KB stack for TLS) runs every
`UPDATE_MS` (~15 s, nudged immediately by `refreshNow()`) **only while the Flight app is open**
(`setActive(true)` in `onEnter`, `false` in `onExit` — it idles and keeps the last snapshot cached
otherwise, so it never fetches in the background) and does up to four things per cycle:

1. **Nearby list** — `fetchNearby` GETs `adsb.lol /v2/lat/lon/dist` over **HTTP** with radius
   `gFetchNm`. On-ground / `alt <= 0` aircraft are dropped, the rest sorted by distance, nearest
   `FLIGHT_KEEP` (12) kept.
2. **Tracked callsign** — if `track()` is set, `fetchCallsign` reads `/v2/callsign/{cs}`; that feed
   has no `dst`/`dir`, so distance/bearing are computed locally via `relTo`.
3. **Route** — keyed by the aircraft's **ICAO24 hex** (`hexForCallsign`, captured from the adsb.lol
   feed). `osEnsureToken` fetches & caches an OAuth2 client-credentials bearer from
   `auth.opensky-network.org` (refreshed ~1 min early), then `/api/flights/aircraft?icao24=&begin=&end=`
   is read and the most recent flight with airport data wins (`dispAirport` maps US `K###` → 3-letter).
   **Window pin:** OpenSky rejects a span touching >2 day-partitions (HTTP 400, which silently made
   *every* route unknown), so `begin` is pinned to 00:00 UTC of the previous day → exactly two UTC
   days. Cached per callsign (incl. a negative cache).
4. **Airports** — `fetchAirportsAt` POSTs an Overpass OQL query: **once near you** (`airports()`) and,
   while tracking, a set **around the plane** (`planeAirports()`, re-fetched when it moves >25 nm,
   each marker's dist/bearing recomputed relative to the live plane each cycle via `relFrom`).

The UI reads the cache lock-free through a mutex: `snapshot()` / `airports()` / `planeAirports()` /
`tracked()` / `route()` / `airportCenter()`, and redraws only when `version()` changes (it bumps on
every successful update). `onExit` calls `Flight::track("")` and resets the fetch radius.

### Route semantics
OpenSky has only *completed* legs, so an **en-route** plane shows `ORIG > ?` (where it came from;
arrival is null until it lands), while a **landed or searched** flight shows the full `ORIG > DEST`.

## Notes
- **No ETA or live destination** — free ADS-B + route data has no schedule or flight plan.
- **On-ground / `alt <= 0` aircraft are dropped** from the list and radar.
- **Pinch needs two touch points.** CST816S-style single-touch boards report
  `puck::Touch::maxPoints() == 1` and can't pinch — zoom is unavailable there, but drag/tap still work.
