# Stocks

> A searchable stock watchlist whose prices tick every few seconds, with a per-ticker detail page (day high/low, open, prev close, next earnings) — from Finnhub.

## What it does

Keeps a small **watchlist** (up to `MAX_STOCKS`, default 8) of tickers you add on-device.
The list shows each ticker's current price + percent change, refreshed continuously from
the always-on `Stocks` service — the app never fetches on the UI thread. Tapping a ticker
opens a **detail** page; an **+ Add ticker** button opens an on-screen keyboard that
searches Finnhub's symbol directory so you can add by name or symbol.

## Using it

Touch:

- **LIST** — one row per watchlist ticker: symbol (left), percent change + price (right),
  the price/change **green when up, red when down**. A row shows `...` until its first
  quote lands. **+ Add ticker** at the bottom opens SEARCH. Empty list → a "Tap + to add"
  prompt. **No Finnhub key set → opening Stocks shows a "Finish setup / Add a Finnhub key"
  prompt and a tap jumps straight to Settings** (same flow as opening a network app before
  Wi-Fi is configured), so you never land on an empty/dead screen.
- **Tap a row → DETAIL** — big price + absolute/percent change (colored), then **Day High**,
  **Day Low**, **Open**, **Prev Close**, and **Next earnings** date. A **Remove** button
  deletes the ticker. The detail price keeps updating while it's open.
- **SEARCH** — type a symbol (A–Z/0–9, up to 6 chars); **SEARCH** runs a Finnhub symbol
  lookup → **RESULTS** lists matches (symbol + company); tap one to add it and return to
  the list. **DEL** backspaces.
- **Back chip** (top-left): DETAIL → LIST, RESULTS → SEARCH, SEARCH → LIST, LIST → launcher
  (`onBack()`).

Physical-button focus nav (see [../buttons.md](../buttons.md)): **LIST** cycles the rows +
the Add button, **DETAIL** focuses Remove, **RESULTS** cycles the matches; SELECT activates
the focused item (same as a tap). SEARCH is touch-only (`focusCount()` returns 0 — typing a
symbol with one button is impractical). On the single-button CoreS3 that's power-key
**click = NEXT**, **double-click = SELECT**.

## What it needs

- **Wi-Fi** — `needsNet()` is `true`; until Wi-Fi is configured `main.cpp` shows a
  "Finish setup" prompt instead of the app.
- **A free Finnhub API key** ([finnhub.io](https://finnhub.io)), set **per-device on the
  gadget** — the captive portal (Settings → **Stocks**) saves it to NVS, so every user
  provisions their own key with no rebuild and no shared key. A compile-time
  `FINNHUB_API_KEY` in `config.h` still works as a fallback default (same pattern as
  `MQTT_PASS`); leave it blank for shared/CI builds. No key set anywhere → opening Stocks
  routes the user to Settings to add one (see "Using it" above). **Each device calls
  finnhub.io directly with its own key**, so
  each gets its own 60 req/min budget (nothing is shared or proxied).
- The watchlist persists in NVS (`Settings::stocksBlob`, comma-separated tickers).

## How it works

- **Background updater** (`Stocks::begin`): a FreeRTOS task pinned to **core 0** with a
  **context-aware cadence** so it stays well under Finnhub's **60/min** free cap:
  - **LIST screen** — round-robins one ticker at a time every **1.5 s** (`LIST_MS`, ≈40
    req/min), leaving headroom for the occasional earnings/search call. Each ticker thus
    refreshes every `N × 1.5 s`.
  - **DETAIL screen** — polls **only** the open ticker every **1.1 s** (`DETAIL_MS`, ≈55
    req/min) for finer updates on the one you're watching.
  - **App closed** — idles entirely (no polling); the last-fetched prices stay cached, so
    reopening shows them instantly and they refresh from there.

  Driven by `setActive()` (app open/closed) + `setFocus()` (detail ticker). Blocking HTTPS
  never touches the UI loop. Repaints fire only when a price actually moves, so a static
  (market-closed) quote causes no needless redraw.
- **Endpoints** (all `https://finnhub.io/api/v1/…`, `WiFiClientSecure` + `setInsecure()`
  like the other non-OTA data fetches): `/quote` (current `c` + day high `h` / low `l` /
  open `o` / prev-close `pc` / change `d` / percent `dp`); `/calendar/earnings`
  (soonest upcoming date within ~120 days, fetched lazily the first time you open a
  ticker's detail); `/search` (symbol lookup for Add). All parsed with ArduinoJson.
- **Watchlist + cache**: `add()`/`remove()` mutate the list under a mutex and persist it;
  the task writes quotes into the same mutex-guarded cache and bumps `version()`. The UI
  reads via `count()` / `get(i,out)` / `searchResults()` and redraws **lazily**, gated on
  `version()`.
- **Flicker-free**: the LIST, DETAIL, and RESULTS screens compose off-screen into a PSRAM
  `puck::Canvas` (`scope`, drawn via the `g` target pointer) and push in one blit, so the
  periodic price refresh never flashes. The SEARCH keyboard is interactive, so it draws
  directly and repaints only on a key tap.

## Notes

- Finnhub's free `/quote` is one symbol per call (no batch), which is why the watchlist
  polls round-robin rather than refreshing every row simultaneously.
- Earnings shows `--` when there's no upcoming report in the window, `...` until the lazy
  fetch returns.
- Quotes are **real-time-ish** during market hours; outside them the values are the last
  session's close (Finnhub free has no extended-hours data).
- The published `.bin` embeds `FINNHUB_API_KEY` (same low-value exposure as the OpenSky
  secret — see the [main config notes](../../CLAUDE.md)); proxy via your own host if it matters.
