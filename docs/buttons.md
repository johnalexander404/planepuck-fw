# Physical-button navigation

PlanePuck is touch-first, but every screen can also be driven by **physical buttons** — a
capability layered on top of touch, so it lights up only on boards that have buttons and never
changes the touch experience.

## How it behaves

- **Touch is unchanged.** Nothing about button nav alters tapping; the focus highlight only appears
  *after* the first button press and disappears again the moment you touch the screen
  (`gButtonMode` in [`src/main.cpp`](../src/main.cpp)).
- **Launcher** — a white ring highlights the focused app chip. **NEXT** rotates it around the ring,
  **SELECT** opens that app.
- **Inside an app** — NEXT moves a white focus ring between the items on the current screen, SELECT
  activates the focused item (exactly as a tap on it would). A physical **BACK** (on boards that have
  one) steps back a level / returns to the launcher; on single-button boards use the on-screen `<`
  chip.

## What maps to a button per board

Buttons are a capability exposed by `puck::Buttons` ([`src/hal/Buttons.h`](../src/hal/Buttons.h)).

| Board | Real buttons | Mapping |
|---|---|---|
| **M5Stack CoreS3** | the **power key** only (A/B/C are touch-emulated zones, not wired to nav) | **short-click = NEXT**, **double-click = SELECT**. No PREV/BACK via button (long-hold is the hardware power-off) — use the `<` chip. The ring therefore steps forward and wraps. |
| **≥2-button board** (e.g. a future Waveshare with real buttons) | TBD — pins/count unconfirmed (`HAL_REFACTOR_PLAN.md` §6) | PREV / NEXT / SELECT / BACK mapped directly. Lights up automatically once `boards/<id>/Buttons.cpp` reports them. |

## Which screens are focusable

| App | Focusable | Touch-only |
|---|---|---|
| Clock | — (it's the idle face) | whole screen |
| Weather | the multi-city grid cells | single-city view, 7-day detail |
| Flight | LIST rows + Radar/Search buttons | radar (gesture), search/history keyboards, detail |
| Emoji | recipient / emote / send | — |
| Friends | Add / Set-name / request-accept / friend rows | the ADD/NAME/RENAME keypads |
| Settings | Check / Versions + version-picker rows | — |

Keyboards and the radar stay touch/gesture — one-button typing or panning isn't practical.

## Adding focus to a new app

Implement the optional hooks on your `App` subclass (defaults are no-ops in
[`src/App.h`](../src/App.h)):

```cpp
int  focusCount();              // number of focusable items on the current screen (0 = touch-only)
void focusMove(int delta);      // advance (+1) / retreat (-1); wrap; trigger a redraw
void focusSelect();             // activate the focused item — usually synthesize the tap it maps to
void drawFocus();               // draw the highlight (a drawRoundRect) on the focused item
```

The convention used by the built-in apps: a `focusItem(i, x, y, w, h)` helper returns each item's
rect for the current mode; `focusSelect()` sets `gTap` to that rect's centre so the existing tap
handler runs unchanged; `focusMove()` flips the app's dirty flag so the screen repaints with the ring
moved. The main loop calls these only when `focusCount() > 0`, so touch-only screens need nothing.
