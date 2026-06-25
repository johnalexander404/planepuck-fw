# Emoji

> One-tap emote to a remembered friend (or all confirmed friends) over MQTT.

## What it does

The Emoji app (`EmojiApp` in `src/apps.h`) sends a single emote to a **remembered
recipient** — either one specific friend or **All** confirmed friends — in one tap.
The recipient persists across reboots (`Settings::emojiTarget`, `"*"` = All).

Sending is the app's only job; **receiving is always-on** and independent of which
screen is showing. Inbound pings are routed by the `Friends` service and surfaced by
the global ping overlay in `main.cpp` (`drawPing`), with an audible `Notify::alert`
beep — so a ping interrupts the Clock, Weather, the launcher, anything.

Emotes are drawn as **anti-aliased vector glyphs** by the shared `drawEmote()` helper
(`src/apps.h`), reused by both the picker and the incoming-ping overlay. The six built-in
codes are `"<3"`, `":)"`, `":D"`, `"zZ"`, `"GM"`, `"GN"`. The ASCII code is only the **wire
format** (and the default-font fallback inside `drawEmote()`); it is never shown when a
glyph exists.

## Using it

**Touch** (the picker, `drawPicker()`):

- Tap the **top strip** (`y < 50`) to cycle the recipient: All → friend0 → friend1 → … → All.
- Tap the **left third** for the previous emote, the **right third** for the next.
- Tap the **center** to **SEND** to the remembered recipient.

**Physical-button focus nav** (optional; touch always works — see [`../buttons.md`](../buttons.md)):
the app exposes three focus items — recipient → emote → send (`focusCount() == 3`). The ring
moves focus; activating depends on the focused item — recipient = change who, emote = next
emote, send = send (the center tap is synthesized). On the **CoreS3** the only physical key is
the power button: short **click = NEXT**, **double-click = SELECT**.

## What it needs

- `needsNet()` returns **true** — Wi-Fi must be configured, or `main.cpp` shows a
  "finish setup" prompt instead of the picker.
- A reachable **MQTT broker** and **at least one mutually-confirmed friend**. Without a
  broker the header reads `no MQTT` (or `connecting...` while `Broker::configured()` but not
  yet connected); with no friends a SEND shows `no friends yet`.
- Recipient comes from `Settings::emojiTarget()`; the friend label resolves nick → name → code.

Set up friends first ([`../apps/friends.md`](../apps/friends.md)); broker/credentials are
covered in [`../backend.md`](../backend.md).

## How it works

SEND calls `Friends::sendEmote(emote)` (All) or `Friends::sendEmoteTo(code, emote)` (one).
Both publish to MQTT topic **`puck/<dst>/emo/<src>`** (`<src>` = your friend code),
**non-retained** (`pub(..., retain=false)`). `sendEmoteTo` drops the send if the target
isn't `CONFIRMED`; `sendEmote` only publishes to `CONFIRMED` entries — so **only
mutually-confirmed friends exchange emotes**.

Inbound `emo` messages are consumed by `Friends::handleSocial()` (called from `main.cpp`'s
`mqttRouter` before the active app sees the message). It **drops emotes from non-confirmed
senders silently**, otherwise stashes the sender + glyph for `takeIncomingEmote()` and fires
`Notify::alert("ping")`. The main loop pops it into the full-screen overlay with two buttons:

- **Reply** — saves the sender as `emojiTarget` and opens this app aimed at them.
- **Mute 1h** / **Unmute** — toggles `Notify::mute(3600000)` (silences the beep only; the
  overlay still shows).

## Notes

- Friends-based, **not broadcast** — there is no "send to everyone on the broker" path.
- `GROUP_ID` is deprecated/unused; messaging is addressed per-friend.
