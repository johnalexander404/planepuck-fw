# Friends

> Manage the social layer for emoji: share your code, add friends by code with mutual approval, and approve/deny incoming requests.

## What it does

The Friends app is the on-device face of the always-on `Friends` service. It lets you:

- **Show your friend code** — the 8-hex string others add you by (also your MQTT username).
- **Add a friend by their code** — starts (or auto-completes) a mutual-approval handshake.
- **Set local per-friend nicknames** — a private alias shown only on your device, never published.
- **Approve / deny incoming requests** — each request row shows the sender's display **name** plus their **device id (code)** so you can verify identity before a one-tap accept.
- **List / remove confirmed friends.**

Only mutually-confirmed friends exchange emotes (the Emoji app sends through `Friends::sendEmote*`); anyone else is dropped.

## Using it

`FriendsApp` (in `src/apps.h`) is a small mode machine: `HOME`, `ADD`, `NAME`, `RENAME`.

**Touch — HOME** (`drawHome` / `handleHomeTap`):

- Top shows `Friends::myCode()` + `myName()` (with `(offline)` / `(no MQTT)` when the broker is down).
- **Add friend** and **Set name** buttons.
- **Request rows** (orange, two lines: name + `id <code>`) each have **OK** (`Friends::approve`) and **X** (`Friends::deny`).
- **Friend rows** (green): tap the name to **rename** (set a nickname), tap the trailing **x** to **remove** (`Friends::removeFriend`).

**Keyboards** — `ADD` uses a hex keypad, `NAME`/`RENAME` use an alpha keypad (`drawKeyboard` / `handleKbTap`; `DEL` / `BACK` / `ADD`|`SAVE`). Adding a friend chains straight into `RENAME` so you can nickname them right away (`BACK` skips, empty clears).

**Physical-button focus nav on HOME** (`focusItem` / `focusMove` / `focusSelect` / `drawFocus`): the focus ring cycles **Add → Set name → request-accept → friend rows**; select activates the focused item (accepting a request, opening Add/Set-name, or renaming a friend). The keypads stay **touch-only** — one-button typing isn't practical. On the CoreS3 the only physical control is the power key: **single-click = NEXT**, **double-click = SELECT**. See [`../buttons.md`](../buttons.md).

## What it needs

- **`needsNet()` returns true** — until Wi-Fi is configured, `main.cpp` shows a "Finish setup" prompt instead of the app.
- **An MQTT broker** (`Broker`) for the social protocol; offline, the UI still shows your code and list but nothing is exchanged.
- **Identity** = an 8-hex friend code derived from `ESP.getEfuseMac()` (`Friends::begin`), which is also the device's MQTT username, plus an editable display name (`Settings::displayName`, `myName()`).

See [`../backend.md`](../backend.md) for the broker and ACL setup.

## How it works

The `Friends` service (`src/services.cpp`, declared in `src/services.h`) is always-on and owns inbound routing via `handleSocial()`, so requests and pings work on any screen — `main.cpp` routes every inbound MQTT message through it first.

- **One wildcard inbox:** `Broker::subscribe("puck/<code>/#")`.
- **Message topics:** `puck/<dst>/<type>/<src>`, `type ∈ {req, ok, no, emo}` (`pub()` helper).
- **State machine:** `SENT` → `RECEIVED` → `CONFIRMED`. `addFriend` publishes a `req`; `req`/`ok` are **retained** for offline delivery and **cleared** (`clearRetained`) once handled. If both sides add each other it **auto-confirms** (`req` arriving while we're `SENT`).
- **Persistence:** the list is serialized to JSON in NVS (`saveList` → `Settings::saveFriendsBlob`). The local nickname is NVS key `k` and is **never published**.
- **Display label:** `labelFor()` resolves **nick → their name → code**.
- **Notifications:** a new request fires `Notify::alert`, an `emo` from a confirmed friend latches the incoming ping (`takeIncomingEmote`) for the full-screen overlay in `main.cpp`.

## Notes

Friend codes come from the chip MAC, so they aren't easily enumerable, and `handleSocial()` drops emotes from non-confirmed senders — but a **public broker offers no confidentiality**: anyone with topic access can see traffic and the (spoofable) display names in `req`/`ok` payloads. For real privacy, run the self-hosted Mosquitto with per-device passwords and the `%u` pattern ACL described in [`../backend.md`](../backend.md), so a device can only read `puck/%u/#` and publish as itself.
