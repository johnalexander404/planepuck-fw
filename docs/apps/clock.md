# Clock

> Big drifting local time plus up to `MAX_WORLD_CITIES` world-clock rows — the idle/screensaver face.

## What it does

Shows the current local time as large 7-segment digits, an optional user name above it, and a column of world-clock rows below. It is the default app (`apps[0]`) and the screen the launcher falls back to when idle. `ClockApp` is display-only: it reads `ClockService` and `WorldClock` and never does any I/O.

## Using it

The Clock has **no focusable items** — `ClockApp` doesn't override `focusCount()`, so it stays at the base default of `0` and takes no part in physical-button focus navigation. There is nothing to tap on the face; any touch only wakes the screen from idle-dim (then the next touch returns to the launcher via the usual back path).

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
- **Flicker-free band:** the time + AM/PM are composed off-screen into a PSRAM `puck::Canvas` (`band`, screen rows `BAND_TOP=44`..`BAND_H=112`) and pushed in a single `pushSprite` blit, so a tick or drift step never flashes. A fallback path draws straight to the panel if the sprite can't allocate. The name (above) and city rows (below) draw directly.
- **Auto-open:** `main.cpp` opens the Clock from the launcher after `LAUNCHER_IDLE_MS` of no activity.
- **Auto-dim:** `ClockApp` is the **only** app that overrides `dimsWhenIdle()` → `true`; `main.cpp` gates `Dim::allowIdleDim()` on it, so the deep idle-dim happens on the Clock/launcher but not while you're using other apps.

## Notes

- `h12` is cached in `onEnter()`, so the 12/24-hour change only takes effect after re-entering the Clock (or a reboot).
- `Font7` (7-segment) can't render letters, so AM/PM is drawn separately in `Font0` beside the digits.
- A world-clock row shows `--:--` until the clock is NTP-set (`timeFor` returns false for `time() < 1700000000`).
- No seconds/zone display on the city rows — they show `HH:MM` only.
