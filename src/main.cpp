#include "hal/puck.h"
#include <cstring>
#include "config.h"
#include "App.h"
#include "services.h"
#include "apps.h"
#include "layout.h"

Tap gTap;   // definition (declared extern in App.h)

ClockApp clockApp;
EmojiApp emojiApp;
WeatherApp weatherApp;
StocksApp stocksApp;
SpotifyApp spotifyApp;
FlightApp flightApp;
SetupApp setupApp;
FriendsApp friendsApp;
App* apps[] = { &clockApp, &emojiApp, &weatherApp, &stocksApp, &spotifyApp, &flightApp, &setupApp, &friendsApp };
const int APP_COUNT = sizeof(apps) / sizeof(apps[0]);

// Launcher is a radial ring of circular chips. Geometry is computed in one
// place (launcherChipPos) so draw + hit-test can't drift.
static const int CHIP_R  = 30;   // chip radius
static const int RING_R  = 64;   // distance of chips from the ring center

App* active = nullptr;

// Incoming friend-ping overlay: a full-screen emoji shown on ANY screen, with Reply / Mute
// buttons. gPingFrom is the sender's friend code (the reply target).
String   gPingFrom, gPingName, gPingEmote;
uint32_t gPingUntil  = 0;
bool     gPingActive = false;

// OTA "update available" / flash-progress overlay, drawn over ANY screen (precedence over the ping).
uint32_t gOtaShownVer = 0xFFFFFFFF;   // last Ota::version() we rendered (lazy redraw)
bool     gOtaOverlay  = false;        // an OTA overlay (confirm / progress / failed) owns the screen
// Flash progress is an animated spiral composed off-screen (flicker-free) and free-run off millis().
puck::Canvas gOtaSprite(&puck::display());
int8_t   gOtaSpriteState = 0;         // 0=untried, 1=ready, 2=unavailable (fall back to a plain bar)
bool     gOtaBarInit     = false;     // fallback bar: static frame drawn once

void launcherChipPos(int i, int& cx, int& cy) {
  int ox = puck::display().width() / 2;
  int oy = puck::display().height() / 2 + 18;     // nudged down to clear the title
  if (APP_COUNT <= 1) { cx = ox; cy = oy; return; }
  float a = -PI / 2 + (2.0f * PI * i) / APP_COUNT;   // start at top, clockwise
  cx = ox + (int)lroundf(RING_R * cosf(a));
  cy = oy + (int)lroundf(RING_R * sinf(a));
}

void mqttRouter(const String& t, const String& p) {
  if (Friends::handleSocial(t, p)) return;   // social messages handled even when the app is closed
  if (t == OTA_PUSH_TOPIC) { Ota::checkNow(); return; }   // fleet "recheck the manifest now" nudge
  if (t.startsWith("fleet/ota/")) { Ota::onFleetCmd(p); return; }   // targeted version push to this device
  if (t.startsWith("fleet/channel/")) { Ota::onChannel(p); return; }   // RC/prod channel marker (retained, operator-set)
  if (active) active->onMqtt(t, p);
}

// Per-app launcher glyph: a small anti-aliased vector icon drawn in black on top
// of the colored chip. Keyed by app name (not index) so the launcher order is free
// to change. Add an app -> add a branch here (or it falls back to its first letter).
void drawAppIcon(const char* name, int cx, int cy, uint16_t chip) {
  const uint16_t ink = BLACK;
  if (!strcmp(name, "Clock")) {
    puck::display().fillArc(cx, cy, 13, 15, 0, 360, ink);                   // bezel ring
    puck::display().drawWedgeLine(cx, cy, cx, cy - 8, 1.6f, 1.6f, ink);     // hour hand (up)
    puck::display().drawWedgeLine(cx, cy, cx + 9, cy + 1, 1.3f, 1.3f, ink); // minute hand
    puck::display().fillSmoothCircle(cx, cy, 2, ink);                       // hub
  } else if (!strcmp(name, "Emoji")) {
    puck::display().fillSmoothCircle(cx - 6, cy - 4, 6, ink);              // heart lobes
    puck::display().fillSmoothCircle(cx + 6, cy - 4, 6, ink);
    puck::display().fillTriangle(cx - 11, cy - 2, cx + 11, cy - 2, cx, cy + 12, ink);
  } else if (!strcmp(name, "Weather")) {
    puck::display().fillSmoothCircle(cx, cy, 7, ink);                     // sun disc
    for (int k = 0; k < 8; k++) {                                    // rays
      float a = k * (float)(PI / 4);
      int x0 = cx + (int)lroundf(cosf(a) * 11), y0 = cy + (int)lroundf(sinf(a) * 11);
      int x1 = cx + (int)lroundf(cosf(a) * 16), y1 = cy + (int)lroundf(sinf(a) * 16);
      puck::display().drawWedgeLine(x0, y0, x1, y1, 1.3f, 1.3f, ink);
    }
  } else if (!strcmp(name, "Flight")) {
    puck::display().fillSmoothRoundRect(cx - 2, cy - 14, 5, 26, 2, ink);            // fuselage (nose up)
    puck::display().fillTriangle(cx, cy - 5, cx - 15, cy + 5, cx - 1, cy + 5, ink); // left wing
    puck::display().fillTriangle(cx, cy - 5, cx + 15, cy + 5, cx + 1, cy + 5, ink); // right wing
    puck::display().fillTriangle(cx, cy + 4, cx - 7, cy + 12, cx - 1, cy + 12, ink);// left tailplane
    puck::display().fillTriangle(cx, cy + 4, cx + 7, cy + 12, cx + 1, cy + 12, ink);// right tailplane
  } else if (!strcmp(name, "Settings")) {
    int ys[3] = { cy - 8, cy, cy + 8 };
    int kx[3] = { cx + 5, cx - 6, cx + 2 };                          // slider knob positions
    for (int r = 0; r < 3; r++) {
      puck::display().fillSmoothRoundRect(cx - 14, ys[r] - 1, 28, 3, 1, ink); // track
      puck::display().fillSmoothCircle(kx[r], ys[r], 4, ink);                 // knob ring
      puck::display().fillSmoothCircle(kx[r], ys[r], 2, chip);                // knob center (chip color)
    }
  } else if (!strcmp(name, "Friends")) {
    puck::display().fillSmoothCircle(cx - 8, cy - 6, 5, ink);                // two "person" glyphs
    puck::display().fillSmoothCircle(cx + 8, cy - 6, 5, ink);
    puck::display().fillSmoothRoundRect(cx - 15, cy + 1, 14, 12, 5, ink);    // shoulders (left)
    puck::display().fillSmoothRoundRect(cx + 1, cy + 1, 14, 12, 5, ink);     // shoulders (right)
  } else if (!strcmp(name, "Stocks")) {
    // rising trend line with an up-right arrowhead (zig-zag stock graph)
    puck::display().drawWedgeLine(cx - 15, cy + 8, cx - 6, cy,      1.6f, 1.6f, ink);
    puck::display().drawWedgeLine(cx - 6,  cy,     cx + 1, cy + 5,  1.6f, 1.6f, ink);
    puck::display().drawWedgeLine(cx + 1,  cy + 5, cx + 8, cy - 3,  1.6f, 1.6f, ink);
    puck::display().drawWedgeLine(cx + 8,  cy - 3, cx + 16, cy - 11, 1.6f, 1.6f, ink);
    puck::display().fillTriangle(cx + 16, cy - 12, cx + 16, cy - 3, cx + 7, cy - 12, ink);  // arrowhead
  } else if (!strcmp(name, "Spotify")) {
    // a two-note music glyph (beam + two stems + two note heads)
    puck::display().fillRect(cx - 8, cy - 12, 2, 20, ink);                   // left stem
    puck::display().fillRect(cx + 9, cy - 16, 2, 20, ink);                   // right stem
    puck::display().fillRect(cx - 8, cy - 16, 19, 4, ink);                   // beam joining the stems
    puck::display().fillSmoothCircle(cx - 10, cy + 8, 4, ink);               // left note head
    puck::display().fillSmoothCircle(cx + 7,  cy + 4, 4, ink);               // right note head
  } else {
    char c[2] = { name[0], 0 };                                      // fallback: first letter
    puck::display().setTextDatum(middle_center);
    puck::display().setFont(&fonts::FreeSansBold12pt7b);
    puck::display().setTextSize(1);
    puck::display().setTextColor(ink);
    puck::display().drawString(c, cx, cy);
  }
}

uint32_t gLauncherIdleSince = 0;   // millis() of last launcher activity (drives idle -> Clock)
bool     gButtonMode    = false;   // a physical button was used -> show focus highlights (a touch hides them)
int      gLauncherFocus = 0;       // focused launcher chip while in button mode

void drawLauncher() {
  static const uint16_t palette[] = { CYAN, ORANGE, GREEN, MAGENTA, YELLOW, RED, BLUE };
  const int pn = sizeof(palette) / sizeof(palette[0]);

  puck::display().fillScreen(BLACK);
  puck::display().setTextDatum(top_center);
  puck::display().setFont(&fonts::FreeSansBold12pt7b);   // anti-aliased title (was pixel Font0)
  puck::display().setTextSize(1);
  puck::display().setTextColor(WHITE, BLACK);
  String title = Settings::displayName();                 // user's name (Settings) replaces the default
  puck::display().drawString(title.length() ? title : String("PlanePuck"), puck::display().width() / 2, 6);

  for (int i = 0; i < APP_COUNT; i++) {
    int cx, cy; launcherChipPos(i, cx, cy);
    uint16_t col = palette[i % pn];
    puck::display().fillSmoothCircle(cx, cy, CHIP_R, col);   // solid, anti-aliased chip
    drawAppIcon(apps[i]->name, cx, cy, col);            // vector glyph on top
  }
  if (gButtonMode) {                                    // button nav: ring the focused chip
    int cx, cy; launcherChipPos(gLauncherFocus, cx, cy);
    puck::display().drawCircle(cx, cy, CHIP_R + 3, WHITE);
    puck::display().drawCircle(cx, cy, CHIP_R + 4, WHITE);
  }
  gLauncherIdleSince = millis();                        // (re)start the idle -> Clock countdown
}

static bool gSetupShown = false;   // "finish setup" screen drawn once per entry (avoids per-frame flicker)

// An app may need on-device setup before it can run: Wi-Fi (needsNet, no creds saved) or other config
// like an API key (needsSetup). Either way, show the setup prompt and route a tap straight to Settings.
static bool needsOnDeviceSetup(App* a) {
  return (a->needsNet() && !Settings::haveWifi()) || a->needsSetup();
}
void enterApp(int i) {
  active = apps[i];
  gSetupShown = false;
  gTap.pressed = false;   // don't let the launcher chip-tap leak into the new app's onEnter() (e.g. Weather opened a grid cell)
  if (needsOnDeviceSetup(active)) return;   // dispatch shows the setup prompt, not an empty app
  active->onEnter();
}
void backToLauncher() { if (active) active->onExit(); active = nullptr; drawLauncher(); }

void drawBackChip() {
  int o = layout::inset();                              // round panels: inset off the clipped corner (0 on CoreS3)
  puck::display().fillRoundRect(4 + o, 4 + o, 34, 24, 5, DARKGREY);
  puck::display().setTextDatum(middle_center);
  puck::display().setFont(&fonts::Font0);
  puck::display().setTextSize(2);
  puck::display().setTextColor(WHITE, DARKGREY);
  puck::display().drawString("<", 21 + o, 16 + o);
}

// Full-screen prompt shown when a network app is opened before Wi-Fi is set up — instead of empty
// "no data" screens. Tapping anywhere (except the back chip) jumps straight to Settings.
void drawSetupNeeded() {
  int w = puck::display().width(), cx = w / 2;
  puck::display().fillScreen(BLACK);
  puck::display().setTextDatum(top_center);
  puck::display().setFont(&fonts::FreeSansBold12pt7b);
  puck::display().setTextColor(WHITE, BLACK);
  puck::display().drawString("Finish setup", cx, 40);
  bool wifiCase = !active || (active->needsNet() && !Settings::haveWifi());   // Wi-Fi missing takes priority
  const char* url = (!wifiCase && active) ? active->setupUrl() : nullptr;     // e.g. the Spotify login page
  const char* l1 = wifiCase ? "Connect Wi-Fi"   : (active->setupHint() ? active->setupHint() : "Open Settings");
  puck::display().setFont(&fonts::Font0); puck::display().setTextSize(2);
  puck::display().setTextColor(0xC618, BLACK);
  if (url) {                                                                  // app with a login URL: show the steps
    puck::display().drawString(l1, cx, 74);
    puck::display().setTextSize(1);
    puck::display().setTextColor(DARKGREY, BLACK); puck::display().drawString("1. log in at", cx, 100);
    puck::display().setTextColor(GREEN, BLACK);    puck::display().drawString(url, cx, 116);
    puck::display().setTextColor(DARKGREY, BLACK); puck::display().drawString("2. paste the token in Settings", cx, 132);
  } else {
    puck::display().drawString(l1, cx, 84);
    puck::display().drawString(wifiCase ? "to use this app" : "in Settings", cx, 108);
  }
  int bw = 184, bh = 46, bx = cx - bw / 2, by = 148;
  puck::display().fillRoundRect(bx, by, bw, bh, 9, CYAN);
  puck::display().setTextDatum(middle_center);
  puck::display().setFont(&fonts::Font0); puck::display().setTextSize(2);
  puck::display().setTextColor(BLACK, CYAN);
  puck::display().drawString("Open Settings", cx, by + bh / 2);
  puck::display().setTextDatum(top_center);
  puck::display().setFont(&fonts::Font0); puck::display().setTextSize(1);
  puck::display().setTextColor(DARKGREY, BLACK);
  puck::display().drawString(wifiCase ? "tap anywhere to set up Wi-Fi" : "tap anywhere to open Settings", cx, by + bh + 16);
}

static const int PING_BTN_Y = 198, PING_BTN_H = 32;   // Reply / Mute button row

void drawPing(const String& emote, const String& name) {
  int w = puck::display().width(), h = puck::display().height();
  puck::display().fillScreen(BLACK);
  drawEmote(emote.c_str(), w / 2, h / 2 - 36);          // shared with the Emoji app
  puck::display().setTextDatum(top_center);
  puck::display().setFont(&fonts::FreeSansBold12pt7b); puck::display().setTextSize(1);
  puck::display().setTextColor(WHITE, BLACK);
  puck::display().drawString(name.length() ? name : String("friend"), w / 2, h / 2 + 20);
  // Reply | Mute 1h
  int bw = w / 2 - 12;
  puck::display().setFont(&fonts::Font0); puck::display().setTextSize(2);
  puck::display().setTextDatum(middle_center);
  puck::display().drawRoundRect(8, PING_BTN_Y, bw, PING_BTN_H, 6, GREEN);
  puck::display().setTextColor(GREEN, BLACK);
  puck::display().drawString("Reply", 8 + bw / 2, PING_BTN_Y + PING_BTN_H / 2);
  bool isMuted = Notify::muted();
  uint16_t mcol = isMuted ? CYAN : ORANGE;
  puck::display().drawRoundRect(w / 2 + 4, PING_BTN_Y, bw, PING_BTN_H, 6, mcol);
  puck::display().setTextColor(mcol, BLACK);
  puck::display().drawString(isMuted ? "Unmute" : "Mute 1h", w / 2 + 4 + bw / 2, PING_BTN_Y + PING_BTN_H / 2);
}

static const int OTA_BTN_Y = 198, OTA_BTN_H = 32;   // Update / Later button row

// Word-wrap centered text in the current font/size; returns lines drawn. Wraps on spaces so the
// changelog never runs off the screen edges; marks the last line with "..." if it overflows maxLines.
static int drawWrappedCentered(const String& s, int cx, int yTop, int maxW, int lineH, int maxLines) {
  int n = s.length(), i = 0, drawn = 0;
  String line = "";
  bool truncated = false;
  while (i < n) {
    int sp = s.indexOf(' ', i);
    if (sp < 0) sp = n;
    String word = s.substring(i, sp);
    i = sp + 1;
    if (!word.length()) continue;
    String trial = line.length() ? line + " " + word : word;
    if (line.length() && puck::display().textWidth(trial) > maxW) {   // current line is full -> flush it
      if (drawn >= maxLines - 1) { truncated = true; break; }
      puck::display().drawString(line, cx, yTop + drawn * lineH); drawn++;
      line = word;
    } else {
      line = trial;
    }
  }
  if (truncated) line += " ...";
  puck::display().drawString(line, cx, yTop + drawn * lineH);
  return drawn + 1;
}

void drawOtaConfirm(int ver, const String& notes) {
  int w = puck::display().width();
  puck::display().fillScreen(BLACK);
  puck::display().setTextDatum(top_center);
  puck::display().setFont(&fonts::FreeSansBold12pt7b); puck::display().setTextSize(1);
  puck::display().setTextColor(CYAN, BLACK);
  char t[28]; snprintf(t, sizeof(t), "Firmware v%d", ver);
  puck::display().drawString(t, w / 2, 26);
  puck::display().setFont(&fonts::Font0); puck::display().setTextSize(2);
  puck::display().setTextColor(WHITE, BLACK);
  puck::display().drawString("available", w / 2, 60);
  // changelog: drop the redundant "release: firmware vN — " prefix release.sh adds, then wrap big
  String body = notes; body.trim();
  if (body.startsWith("release:")) {
    int d = body.indexOf("\xE2\x80\x94");           // em dash separator
    if (d >= 0)              body = body.substring(d + 3);
    else { int h = body.indexOf(" - "); if (h >= 0) body = body.substring(h + 3); }
    body.trim();
  }
  if (!body.length()) body = notes;
  if (body.length()) {
    puck::display().setFont(&fonts::Font0); puck::display().setTextSize(2);
    puck::display().setTextColor(puck::display().color565(190, 190, 190), BLACK);
    drawWrappedCentered(body, w / 2, 92, w - 16, 18, 5);
  }
  int bw = w / 2 - 12;
  puck::display().setFont(&fonts::Font0); puck::display().setTextSize(2);
  puck::display().setTextDatum(middle_center);
  puck::display().drawRoundRect(8, OTA_BTN_Y, bw, OTA_BTN_H, 6, GREEN);
  puck::display().setTextColor(GREEN, BLACK);
  puck::display().drawString("Update", 8 + bw / 2, OTA_BTN_Y + OTA_BTN_H / 2);
  puck::display().drawRoundRect(w / 2 + 4, OTA_BTN_Y, bw, OTA_BTN_H, 6, ORANGE);
  puck::display().setTextColor(ORANGE, BLACK);
  puck::display().drawString("Later", w / 2 + 4 + bw / 2, OTA_BTN_Y + OTA_BTN_H / 2);
}

// full=true clears + draws the static frame once; subsequent ticks only repaint the % band + bar
// fill (no fillScreen), so the percent doesn't strobe the whole screen during the download.
void drawOtaProgress(int pct, bool rebooting, bool full) {
  int w = puck::display().width(), h = puck::display().height();
  int bw = w - 60, bx = 30, by = h / 2 + 4, bh = 16;
  puck::display().setTextDatum(middle_center);
  if (rebooting) {
    puck::display().fillScreen(BLACK);
    puck::display().setFont(&fonts::FreeSansBold12pt7b); puck::display().setTextSize(1);
    puck::display().setTextColor(CYAN, BLACK);
    puck::display().drawString("Updated", w / 2, h / 2 - 18);
    puck::display().setFont(&fonts::Font0); puck::display().setTextSize(2);
    puck::display().setTextColor(GREEN, BLACK);
    puck::display().drawString("rebooting...", w / 2, h / 2 + 18);
    return;
  }
  if (full) {                                    // static frame, once (clears the confirm screen)
    puck::display().fillScreen(BLACK);
    puck::display().drawRoundRect(bx, by, bw, bh, 4, DARKGREY);
    puck::display().setFont(&fonts::Font0); puck::display().setTextSize(1);
    puck::display().setTextColor(DARKGREY, BLACK);
    puck::display().drawString("do not unplug", w / 2, by + 34);
  }
  puck::display().fillRect(0, h / 2 - 40, w, 28, BLACK);   // repaint just the % title band
  puck::display().setFont(&fonts::FreeSansBold12pt7b); puck::display().setTextSize(1);
  puck::display().setTextColor(CYAN, BLACK);
  char t[20]; snprintf(t, sizeof(t), "Updating %d%%", pct);
  puck::display().drawString(t, w / 2, h / 2 - 26);
  if (pct > 0) puck::display().fillRoundRect(bx + 2, by + 2, (bw - 4) * pct / 100, bh - 4, 3, GREEN);
}

// Animated flash-progress: a glowing spiral arm winds outward as the download fills (pct),
// studded with "station" dots that light up, led by a pulsing comet head, with a shimmer of
// energy racing along the trail. Free-runs off millis() so it stays alive between % jumps; the
// whole frame composes into a PSRAM sprite and blits once (no flicker). Falls back to the plain
// bar if the sprite won't allocate. `done` paints the completed, all-collected spiral.
void drawOtaSpiral(int pct, bool done) {
  const int w = puck::display().width(), h = puck::display().height();
  if (pct < 0) pct = 0; else if (pct > 100) pct = 100;

  if (gOtaSpriteState == 0) {                         // try once to grab the off-screen canvas
    gOtaSprite.setColorDepth(16);
    gOtaSprite.setPsram(true);
    gOtaSpriteState = (gOtaSprite.createSprite(w, h) != nullptr) ? 1 : 2;
  }
  if (gOtaSpriteState != 1) {                         // no PSRAM sprite -> plain bar, flicker-guarded
    drawOtaProgress(pct, done, !gOtaBarInit);
    gOtaBarInit = true;
    return;
  }

  auto rgb = [](int r, int g, int b) -> uint16_t {
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  };

  const float cx = w / 2.0f, cy = h / 2.0f - 6.0f;    // spiral center (room for label below)
  const float TURNS = 4.5f, R0 = 30.0f, RMAX = 96.0f; // 4.5 turns, clear inner hub for the %
  const int   STEPS = 180;                            // arm resolution
  const int   DOTS  = 30;                             // station dots along the spiral
  const float frac  = done ? 1.0f : pct / 100.0f;     // how far the arm has wound
  const uint32_t ms = millis();
  const float spin  = (ms % 24000u) / 24000.0f * TWO_PI;        // gentle full turn / 24s
  const float shim  = fmodf(ms / 1500.0f, 1.0f);               // 0..1 energy sweep along the arm
  const float pulse = 0.5f + 0.5f * sinf(ms / 130.0f);         // comet-head breathing

  puck::Canvas& g = gOtaSprite;
  g.fillSprite(BLACK);

  auto posAt = [&](float f, float& x, float& y) {
    float th = f * TURNS * TWO_PI + spin;
    float r  = R0 + (RMAX - R0) * f;
    x = cx + r * cosf(th);
    y = cy + r * sinf(th);
  };

  // 1) faint full guide spiral (the track ahead)
  { float px, py, x, y; posAt(0, px, py);
    for (int s = 1; s <= STEPS; ++s) { posAt((float)s / STEPS, x, y);
      g.drawLine((int)px, (int)py, (int)x, (int)y, rgb(28, 34, 44)); px = x; py = y; } }

  // 2) bright filled arm cyan->green, with a moving shimmer highlight
  if (frac > 0.0f) {
    int sfill = (int)(STEPS * frac + 0.5f);
    float px, py, x, y; posAt(0, px, py);
    for (int s = 1; s <= sfill; ++s) {
      float f = (float)s / STEPS; posAt(f, x, y);
      float rel = (frac > 0.001f) ? f / frac : 0.0f;            // 0..1 along the filled part
      int r = (int)(0   + 120 * rel);
      int gg = (int)(200 + 55  * rel);
      int b = (int)(255 - 135 * rel);
      float d = fabsf(rel - shim); if (d > 0.5f) d = 1.0f - d;   // wrap-around distance
      float boost = d < 0.10f ? (0.10f - d) / 0.10f : 0.0f;      // brighten toward white near sweep
      r += (int)((255 - r) * boost); gg += (int)((255 - gg) * boost); b += (int)((255 - b) * boost);
      uint16_t c = rgb(r, gg, b);
      g.drawLine((int)px, (int)py, (int)x, (int)y, c);
      g.drawLine((int)px + 1, (int)py, (int)x + 1, (int)y, c);   // 2px so the arm reads as a ribbon
      px = x; py = y;
    }
  }

  // 3) station dots: dim ahead, lit once the arm passes them
  for (int i = 0; i < DOTS; ++i) {
    float f = (i + 0.5f) / DOTS, x, y; posAt(f, x, y);
    if (f <= frac) g.fillCircle((int)x, (int)y, 3, rgb(120 + (int)(80 * f), 255, 160 - (int)(60 * f)));
    else           g.fillCircle((int)x, (int)y, 2, rgb(46, 54, 66));
  }

  // 4) pulsing comet head at the leading edge of the arm
  if (frac > 0.0f) {
    float hx, hy; posAt(frac, hx, hy);
    int R = 5 + (int)(3 * pulse);
    g.fillCircle((int)hx, (int)hy, R + 4, rgb(20, 80 + (int)(40 * pulse), 40));
    g.fillCircle((int)hx, (int)hy, R + 1, rgb(70, 220, 110));
    g.fillCircle((int)hx, (int)hy, R - 1, rgb(210, 255, 220));
  }

  // 5) centered % in the clear hub + labels
  g.setTextDatum(middle_center);
  g.setFont(&fonts::FreeSansBold12pt7b); g.setTextSize(1);
  g.setTextColor(done ? GREEN : CYAN);
  if (done) g.drawString("Done!", (int)cx, (int)cy);
  else { char t[8]; snprintf(t, sizeof(t), "%d%%", pct); g.drawString(t, (int)cx, (int)cy); }
  g.setFont(&fonts::Font0); g.setTextSize(1);
  g.setTextColor(rgb(120, 140, 160));
  g.drawString("updating firmware", w / 2, 14);
  g.setTextColor(done ? rgb(120, 200, 140) : rgb(150, 120, 90));
  g.drawString(done ? "rebooting..." : "do not unplug", w / 2, h - 14);

  g.pushSprite(0, 0);
}

void drawOtaFailed(const String& err) {
  int w = puck::display().width(), h = puck::display().height();
  puck::display().fillScreen(BLACK);
  puck::display().setTextDatum(middle_center);
  puck::display().setFont(&fonts::FreeSansBold12pt7b); puck::display().setTextSize(1);
  puck::display().setTextColor(ORANGE, BLACK);
  puck::display().drawString("Update failed", w / 2, h / 2 - 18);
  puck::display().setFont(&fonts::Font0); puck::display().setTextSize(1);
  puck::display().setTextColor(DARKGREY, BLACK);
  puck::display().drawString(err.length() ? err : String("try again later"), w / 2, h / 2 + 12);
  puck::display().drawString("tap to dismiss", w / 2, h - 16);
}

void setup() {
  puck::begin();
  disableCore0WDT();          // Weather/Flight/Broker tasks do blocking network I/O on core 0;
                              // that cooperative blocking must not trip the task watchdog.

  Settings::begin();          // load saved Wi-Fi/timezone before anything reads them
  Notify::begin();
  Dim::begin();
  Compass::begin();           // detect magnetometer for the heading-up radar (no-op if absent)
  ClockService::begin();
  Net::begin();
  Broker::begin();
  Broker::onMessage(mqttRouter);
  Friends::begin();           // derive my friend code, restore list, subscribe my inbox
  Fleet::begin();             // set the MQTT will (fleet presence) before the first connect
  Weather::begin();           // start the background weather updater
  Flight::begin();            // start the background flight tracker
  Stocks::begin();            // start the background stock-quote updater
  Spotify::begin();           // start the Spotify now-playing task (idles until the app opens + linked)
  Ota::begin();               // start the OTA updater (subscribes the push topic; idles until online)

  if (!Settings::haveWifi()) {   // first boot: go straight to on-device Wi-Fi setup
    active = &setupApp;
    active->onEnter();
  } else {
    drawLauncher();
  }
}

void loop() {
  puck::update();

  // one tap per frame
  gTap.pressed = false;
  auto d = puck::Touch::get(0);
  if (d.pressed) { gTap.pressed = true; gTap.x = d.x; gTap.y = d.y; }
  if (gTap.pressed) {
    gButtonMode = false;                 // touch -> hide the focus highlight (touch is the active input)
    bool wasDimmed = Dim::dimmed();      // check before waking
    Dim::wake();
    if (wasDimmed) gTap.pressed = false; // screen was dimmed -> this tap only brightens it, no app action
  }

  // physical-button events for this frame (additive to touch; dormant until the first button press)
  bool bN = false, bP = false, bS = false, bB = false;
  {
    bool n = puck::Buttons::nextPressed(),   p = puck::Buttons::prevPressed(),
         s = puck::Buttons::selectPressed(), b = puck::Buttons::backPressed();
    if (n || p || s || b) {
      bool wasDimmed = Dim::dimmed();
      Dim::wake();
      gButtonMode = true;
      if (!wasDimmed) { bN = n; bP = p; bS = s; bB = b; }  // when dimmed, the first press only wakes
    }
  }

  // always-on services
  Net::loop();
  ClockService::loop();
  Broker::loop();
  Dim::allowIdleDim(!active || active->dimsWhenIdle());   // auto-dim only on the Clock/launcher; active apps stay lit
  Dim::loop();
  Compass::loop();
  Friends::loop();
  Fleet::loop();
  Notify::loop();
  Ota::loop();

  // OTA: "update available" confirm + flash progress, over ANY screen (precedence over the ping).
  {
    Ota::Phase ph = Ota::phase();
    if (!gOtaOverlay && ph == Ota::AVAILABLE) {        // newer firmware found -> pop the confirm
      gOtaOverlay = true; drawOtaConfirm(Ota::availableVersion(), Ota::releaseNotes());
      gOtaShownVer = Ota::version();
    } else if (!gOtaOverlay && (ph == Ota::FLASHING || ph == Ota::DONE_REBOOTING)) {
      gOtaOverlay = true; gOtaShownVer = 0xFFFFFFFF;   // forced/silent push -> still show flash progress
    }
    if (gOtaOverlay) {
      if (ph == Ota::FLASHING || ph == Ota::DONE_REBOOTING) {       // progress screen owns the loop
        drawOtaSpiral(Ota::percent(), ph == Ota::DONE_REBOOTING);   // animate every frame (free-running)
        delay(16); return;                                          // ignore touch while flashing
      }
      if (ph == Ota::FAILED) {                                      // show the error; dismiss on tap
        if (gOtaSpriteState == 1) { gOtaSprite.deleteSprite(); gOtaSpriteState = 0; }  // reclaim the flash-anim PSRAM
        if (Ota::version() != gOtaShownVer) { drawOtaFailed(Ota::lastError()); gOtaShownVer = Ota::version(); }
        if (gTap.pressed) { gTap.pressed = false; gOtaOverlay = false; Ota::dismiss(); if (active) active->onEnter(); else drawLauncher(); }
        delay(16); return;
      }
      if (ph == Ota::AVAILABLE) {                                   // confirm: Update | Later
        if (gTap.pressed) {
          int w = puck::display().width();
          bool onRow = (gTap.y >= OTA_BTN_Y && gTap.y <= OTA_BTN_Y + OTA_BTN_H);
          if (onRow && gTap.x < w / 2) { Ota::confirmUpdate(); drawOtaSpiral(0, false); gOtaShownVer = Ota::version(); }
          else if (onRow)              { Ota::dismiss(); gOtaOverlay = false; if (active) active->onEnter(); else drawLauncher(); }
          gTap.pressed = false;
        }
        delay(16); return;                                          // confirm overlay owns the frame
      }
      gOtaOverlay = false;                                          // CHECKING/IDLE -> tear down, restore
      if (active) active->onEnter(); else drawLauncher();
    }
  }

  // Incoming friend ping -> full-screen emoji + Reply/Mute, on whatever screen you're on.
  // Mute only silences the beep (handled in Notify::alert) — the overlay still shows.
  if (!gPingActive) {
    String fc, fn, em;
    if (Friends::takeIncomingEmote(fc, fn, em)) {
      gPingFrom = fc; gPingName = fn; gPingEmote = em;
      gPingUntil = millis() + 6000; gPingActive = true;
      drawPing(gPingEmote, gPingName);
    }
  }
  if (gPingActive) {
    bool done = false, doReply = false, doMute = false;
    if (gTap.pressed) {
      int w = puck::display().width();
      bool onRow = (gTap.y >= PING_BTN_Y && gTap.y <= PING_BTN_Y + PING_BTN_H);
      if (onRow && gTap.x < w / 2)        { doReply = true; done = true; }
      else if (onRow && gTap.x >= w / 2)  { doMute  = true; done = true; }
      else                                { done = true; }      // tap elsewhere = dismiss
      gTap.pressed = false;
    } else if (millis() >= gPingUntil) {
      done = true;                                              // auto-dismiss
    }
    if (done) {
      gPingActive = false;
      if (doMute) { if (Notify::muted()) Notify::unmute(); else Notify::mute(3600000UL); }  // toggle
      if (doReply) { Settings::saveEmojiTarget(gPingFrom); active = &emojiApp; emojiApp.onEnter(); }
      else if (active) active->onEnter();                       // restore where we were
      else drawLauncher();
    }
    delay(16);
    return;
  }

  if (active) {
    if (gTap.pressed && gTap.x < 40 + layout::inset() && gTap.y < 30 + layout::inset()) {   // back chip (always available)
      gTap.pressed = false;
      if (!active->onBack()) backToLauncher();          // let the app step back a level first (e.g. radar->list)
    } else if (bB) {                                    // physical back button (>=2-button boards)
      if (!active->onBack()) backToLauncher();
    } else if (needsOnDeviceSetup(active)) {  // unconfigured -> prompt, not empty data
      if (!gSetupShown) { drawSetupNeeded(); drawBackChip(); gSetupShown = true; }
      Notify::draw();
      if (gTap.pressed) { gTap.pressed = false; active = &setupApp; gSetupShown = false; active->onEnter(); }
    } else {
      if (active->focusCount() > 0) {                   // button focus nav (no-op for touch-only screens)
        if (bP) active->focusMove(-1);
        if (bN) active->focusMove(1);
        if (bS) active->focusSelect();
      }
      active->loop();
      drawBackChip();
      if (gButtonMode && active->focusCount() > 0) active->drawFocus();
      Notify::draw();
    }
  } else {
    if (gTap.pressed) {                                  // launcher chip pick (touch)
      gLauncherIdleSince = millis();                     // any tap resets the idle countdown
      for (int i = 0; i < APP_COUNT; i++) {
        int cx, cy; launcherChipPos(i, cx, cy);
        int dx = gTap.x - cx, dy = gTap.y - cy;
        if (dx * dx + dy * dy <= CHIP_R * CHIP_R) { enterApp(i); break; }
      }
      gTap.pressed = false;
    } else if (bS) {                                     // button: open the focused chip
      gLauncherIdleSince = millis(); enterApp(gLauncherFocus);
    } else if (bN || bP) {                               // button: rotate focus around the ring
      gLauncherIdleSince = millis();
      gLauncherFocus = (gLauncherFocus + (bN ? 1 : APP_COUNT - 1)) % APP_COUNT;
      drawLauncher();                                    // redraw to move the highlight
    } else if (millis() - gLauncherIdleSince >= LAUNCHER_IDLE_MS) {
      enterApp(0);                                       // idle on home -> open the Clock (apps[0])
    }
    Notify::draw();   // show the friend-ping dot on the launcher too
  }

  delay(16);   // ~60 fps
}
