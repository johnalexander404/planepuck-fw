# Spotify

> Now Playing — current track + album art with play/pause and skip, from your own Spotify account.

## What it does

Shows what's playing on your Spotify account: album art, track, artist, a live progress bar, and
**Prev / Play-Pause / Next** controls. The display + controls read/drive the always-on `Spotify`
service — the app never does network I/O on the UI thread. Linking is **per-device** via a one-time
browser login (no client secret in firmware); the device stores a refresh token and auto-refreshes.

## Using it

- **Now Playing** — album art (top), track + artist, a progress bar with elapsed/duration, and three
  touch buttons at the bottom: **◀◀ Prev**, **▶ / ❚❚ Play-Pause**, **▶▶ Next**. The bar advances
  smoothly between the ~4 s polls (interpolated locally).
- **Nothing playing** → a "Nothing playing" prompt (start a track in any Spotify client).
- **Not linked** → opening Spotify shows a **"Link Spotify (Settings)"** prompt; a tap jumps to
  Settings, same flow as a network app before Wi-Fi (see [stocks.md](stocks.md)).
- **Back chip** (top-left) → launcher.

Physical-button focus nav ([../buttons.md](../buttons.md)) cycles the three controls; SELECT acts on
the focused one. On the single-button CoreS3 that's power-key **click = NEXT**, **double = SELECT**.

## What it needs

- **Wi-Fi** — `needsNet()` is `true`.
- **A linked Spotify account.** Register a free app at
  [developer.spotify.com/dashboard](https://developer.spotify.com/dashboard) for a **Client ID**, add
  the redirect URI `https://<your-fw-host>/spotify/`, and build with `SPOTIFY_CLIENT_ID` set (the CI
  secret). Then **each user links their own account**: open `https://<your-fw-host>/spotify/`, log in
  (Authorization Code + **PKCE** — no secret anywhere), copy the refresh token, and paste it into the
  captive portal (Settings → **Spotify**). It's saved to NVS (`Settings::spotifyRefresh`, key `sprt`).
- **An active Spotify Connect device** for the controls — play/pause/skip target whatever's currently
  connected to Spotify. With nothing connected, controls return `404` and the app shows
  **"no active device"** (Now-Playing display still works).
- **`config.h`:** `SPOTIFY_CLIENT_ID` (public PKCE id), `SPOTIFY_POLL_MS` (poll cadence while open),
  `SPOTIFY_CA_CERT` (DigiCert Global Root G2 — pinned).

## How it works

- **Background task** (`Spotify::begin`): a FreeRTOS task on **core 0**. While the app is open it
  refreshes the access token, polls `GET /v1/me/player/currently-playing` every `SPOTIFY_POLL_MS`
  (~4 s), drains any queued control command, and fetches new album art. Closed → idle (no polling).
  `setActive()` gates it; `suspend()/resume()` quiesce it during an OTA flash.
- **OAuth (PKCE, rotating)**: `refresh_token` → `access_token` via `POST accounts.spotify.com/api/token`
  (public client, no secret). Spotify **rotates** refresh tokens, so when the response carries a new
  one the service re-persists it to NVS. A dead token (`invalid_grant`) is cleared, so the app falls
  back to the "Link Spotify" prompt.
- **TLS**: `WiFiClientSecure` **CA-pinned** to `SPOTIFY_CA_CERT` (DigiCert Global Root G2 — covers
  accounts/api/`i.scdn.co`). Never `setInsecure()` here (this carries the account token + control),
  unlike the public-data fetchers (Stocks/OpenSky).
- **Album art**: the service caches the raw cover **JPEG bytes** in PSRAM (fetched only when the
  track's image URL changes); the app copies them out under the lock and **decodes** into its own
  PSRAM sprite with `drawJpg` (scaled to fit) — so GFX stays in the app and there's no cross-thread
  race on the decode.
- **Flicker-free**: the whole screen composes off-screen into a PSRAM `puck::Canvas` and pushes in one
  blit; the back chip is composited in too. Redraws are lazy (on `version()` change) plus ~1/s to
  advance the progress bar while playing.

## Notes

- Controls need the `user-modify-playback-state` scope; display needs `user-read-currently-playing` /
  `user-read-playback-state`. The login page requests all three.
- A Spotify app in **dev mode** is capped at **25 manually-added users**; a larger fleet needs
  Spotify's extended-quota review. (Deployment prerequisite, not a code limit.)
- Podcasts/ads: an ad (no `item`) is skipped; episodes show their title like a track.
- The published `.bin` carries only the **public** client id (PKCE has no secret), so there's no
  secret exposure here — unlike the OpenSky/Finnhub keys.
