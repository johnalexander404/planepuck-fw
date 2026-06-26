# Clock

> Big drifting local time plus up to `MAX_WORLD_CITIES` world-clock rows — the idle/screensaver face. **Tap the face to switch between Digital, Analog, and Matrix watch faces.**

## What it does

Shows the current local time in one of three selectable **faces** — **Digital** (large 7-segment digits + optional name + world-clock rows), **Analog** (a full dial with hour/minute/second hands), or **Matrix** (falling-glyph "rain" with the time legible over it). It is the default app (`apps[0]`) and the screen the launcher falls back to when idle. `ClockApp` is display-only: it reads `ClockService` and `WorldClock` and never does any I/O.

## Using it

**Tap the clock face to cycle the watch face** — Digital → Analog → Matrix → Digital. The choice is remembered (`Settings::clockFace`, NVS key `face`). The first touch while the screen is deep-idle-dimmed only **wakes** it (it isn't counted as a face-cycle); the next tap cycles. The top-left **back chip** always returns to the launcher (main.cpp consumes a chip tap before the face-cycle, so the two never conflict). `ClockApp` has no per-item focus list, so physical-button focus nav doesn't apply on this screen.

On the CoreS3 the single physical key is the power button: short-click = NEXT, double-click = SELECT (PREV/BACK are unavailable via button — use the on-screen back chip). Because the Clock exposes no focus items, those button events do nothing here. See [`../buttons.md`](../buttons.md).

## What it needs

- **Network:** none to run. NTP improves accuracy but the time still shows from the RTC seed offline.
- **Services read:** `ClockService::getTime()` for local time; `WorldClock::timeFor()` for each city; `Settings::displayName()`, `Settings::clock12h()`, `Settings::tz()`.
- **On-device (captive portal):** display **name** (blank = hidden), **timezone** (POSIX TZ → `Settings::tz()`), **12/24-hour** toggle (`Settings::clock12h()`, default 24h), and up to `MAX_WORLD_CITIES` **world-clock cities** (label + POSIX TZ, stored as a JSON blob in NVS and parsed by `WorldClock::reload()`).
- **`config.h`:** `MAX_WORLD_CITIES`, `CLOCK_DRIFT_PERIOD_MS`, `CLOCK_DRIFT_MAX_PX`, `LAUNCHER_IDLE_MS`, `NTP_SERVER`, and the night window `DAY_START_HOUR`/`NIGHT_START_HOUR` (used by `Dim`).

## How it works

- **Time source:** `ClockService::begin()` sets `TZ`/`tzset()` and `seedFromRtc()` so a time is available immediately offline; once `Net::online()`, `loop()` calls `configTzTime(..., NTP_SERVER)` and flips `synced()` true when NTP takes over. `getTime()` does a mutex-guarded `localtime_r`.
- **Anti-burn-in drift:** the whole time block shifts every `CLOCK_DRIFT_PERIOD_MS` (phase = `millis()/CLOCK_DRIFT_PERIOD_MS`) by a deterministic `±CLOCK_DRIFT_MAX_PX` offset in X/Y, so the pixels never sit still. `loop()` redraws only on a second change or a drift step.
- **World clocks:** `WorldClock::timeFor()` briefly swaps the global `TZ` to the city's zone, reads `localtime_r`, then restores the home zone — all under `gTzMtx`, a mutex **shared with `ClockService::getTime`** because the Weather background task also reads local time. City rows are fixed-position and repaint at most once per minute (`drawCities`).
- **Faces:** the face index (`Settings::clockFace()`, cached in `onEnter`) selects the draw path; a tap in `loop()` increments it mod 3 and persists. **Digital** keeps the existing `band` path. **Analog** and **Matrix** compose into a separate lazily-allocated full-screen PSRAM `puck::Canvas` (`face`) blitted in one `pushSprite`; both apply the same anti-burn-in drift, and the back chip is composited into the blit so it never flickers under the full-screen push. The analog dial draws hands with `drawWedgeLine` + a `fillSmoothCircle` hub; it shows the name but drops the world-clock rows (full-face). **Matrix** is the one face that *animates*: it bypasses the per-second redraw gate and repaints at ~18 fps (`MAT_FRAME_MS`), with a stable per-cell glyph grid and a moving bright head per column. When `Dim::dimmed()` (deep idle), the rain slows to ~2.5 fps to save power and cut burn-in while the time stays legible.
- **Flicker-free band:** the time + AM/PM are composed off-screen into a PSRAM `puck::Canvas` (`band`, screen rows `BAND_TOP=44`..`BAND_H=112`) and pushed in a single `pushSprite` blit, so a tick or drift step never flashes. A fallback path draws straight to the panel if the sprite can't allocate. The name (above) and city rows (below) draw directly.
- **Auto-open:** `main.cpp` opens the Clock from the launcher after `LAUNCHER_IDLE_MS` of no activity.
- **Auto-dim:** `ClockApp` is the **only** app that overrides `dimsWhenIdle()` → `true`; `main.cpp` gates `Dim::allowIdleDim()` on it, so the deep idle-dim happens on the Clock/launcher but not while you're using other apps.

## Notes

- `h12` is cached in `onEnter()`, so the 12/24-hour change only takes effect after re-entering the Clock (or a reboot).
- `Font7` (7-segment) can't render letters, so AM/PM is drawn separately in `Font0` beside the digits.
- A world-clock row shows `--:--` until the clock is NTP-set (`timeFor` returns false for `time() < 1700000000`).
- No seconds/zone display on the city rows — they show `HH:MM` only.
