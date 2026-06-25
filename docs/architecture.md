# Architecture

PlanePuck is an ESP32-S3 "app gadget": a launcher hosting small apps over always-on services, with
all hardware access behind a board HAL so one source tree runs on more than one board.

## Layers

```
boards/<id>/*.cpp   ── puck:: HAL implementation for one board (selected at build time)
      ▲  (puck::Display/Touch/Audio/Power/RTC/IMU/Light/Buttons)
src/hal/*.h         ── board-agnostic HAL interfaces (namespace puck::)
      ▲
src/services.* ── always-on subsystems (time, dim, weather, flight, friends, MQTT, OTA, …)
      ▲
src/apps.h     ── the apps (Clock/Emoji/Weather/Flight/Friends/Settings); read services, draw UI
      ▲
src/main.cpp   ── setup()/loop(), the launcher, input, and dispatch to the active app
```

**Dependency rule (strict):** apps depend on services; **services and `main.cpp` never depend on
apps** (only `main.cpp` registers them in the `apps[]` array). Keep it that way.

## The `puck::` HAL

`src/` contains **zero `M5.*` references** — every hardware call goes through `puck::`
([`src/hal/puck.h`](../src/hal/puck.h) pulls in the interfaces):

- `puck::display()` → `lgfx::LGFX_Device&` (the panel). All drawing is `puck::display().fillRect(…)`
  etc.; the off-screen sprite type is `puck::Canvas`. Both the panel and sprites derive from
  `lgfx::LovyanGFX`, so the apps' shared `g` draw-target pointer works for either.
- `puck::Touch / Audio / RTC / IMU / Light / Power / Buttons` — small static-method namespaces.
- `puck::begin()` brings the board up; `puck::update()` is the per-frame input poll.

A board's concrete implementation lives in `boards/<id>/*.cpp` and is chosen in
[`platformio.ini`](../platformio.ini) via `build_src_filter = +<*> +<../boards/<id>/>` plus
`-D PUCK_BOARD_*`. `boards/<id>/board_config.h` sets `PUCK_BOARD_ID`, `PUCK_OTA_SUFFIX`,
`PUCK_HAS_PSRAM`, `PUCK_ROUND`. Today: **`m5stack-cores3`** (real, wraps M5Unified) and
**`waveshare_185c_box`** (compiling stubs — see [hardware.md](hardware.md)).

## Draw model

Immediate-mode against the panel. Screens that repaint on a periodic data refresh (Clock band,
Weather, Flight radar) **compose off-screen into a PSRAM `puck::Canvas` and push in one blit** (via a
`g` target pointer / `scope` sprite) so a refresh never flashes; interactive screens repaint only on a
tap/event. Keep redraws gated by dirty flags / change detection — the loop runs ~60 fps and
**nothing may block it** (network/TLS/HTTP all run on FreeRTOS background tasks on core 0).

## The loop

`main.cpp loop()` each frame: `puck::update()` → sample one tap into the global `gTap` + read button
events → pump every service's `loop()` → handle global overlays (OTA confirm, friend ping) → dispatch
to the active app (or the launcher when `active == nullptr`). See [buttons.md](buttons.md) for the
focus-nav layer and [`layout.h`](../src/layout.h) for proportional/round-screen helpers.

## Add an app

1. Subclass `App` in [`src/apps.h`](../src/apps.h) (`onEnter/loop/onExit/onMqtt`, optional `needsNet`,
   `onBack`, and the [focus hooks](buttons.md)).
2. Instantiate it and add it to the `apps[]` array in [`src/main.cpp`](../src/main.cpp) — that array
   drives both the launcher ring and indexed dispatch.
3. Add a `drawAppIcon()` glyph branch keyed by the app's `name`. Nothing else changes.

## Add a board

1. Create `boards/<id>/` with a `.cpp` per HAL interface (implement every `puck::` symbol) and a
   `board_config.h`. The cleanest start is to copy `boards/waveshare_1_85c_box/` (stubs) and fill in
   the real drivers.
2. Add an `[env:<id>]` to `platformio.ini` (board, `build_src_filter`, `-D PUCK_BOARD_<ID>`, lib_deps).
3. `pio run -e <id>` must build with **no `M5.` symbols** unless that board genuinely uses M5Unified.

See [build-and-deploy.md](build-and-deploy.md) for builds/CI and [ota.md](ota.md) for per-board OTA.
