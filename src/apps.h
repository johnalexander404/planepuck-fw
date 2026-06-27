#pragma once
#include "hal/puck.h"
#include "App.h"
#include "services.h"
#include "config.h"
#include "layout.h"
#include <esp_heap_caps.h>     // PSRAM scratch buffer for the Spotify album-art JPEG
#include <esp_random.h>        // esp_random() — matrix-rain glyph/seed jitter
#include <math.h>              // sinf/cosf for the analog face hands + the Spotify mark

// The Spotify mark: a green disc with three black concave-down "sound-wave" bars, scaled to radius R.
// The bars are sampled from our own trig (no dependence on the GFX arc-angle convention), so it renders
// identically on the launcher chip and the now-playing screen, on either board. Designed at R=16.
inline void drawSpotifyMark(lgfx::LovyanGFX* t, int cx, int cy, int R) {
  uint16_t green = t->color565(30, 215, 96);
  uint16_t bar   = t->color565(0, 0, 0);                 // black bars (the real Spotify mark)
  t->fillSmoothCircle(cx, cy, R, green);
  float s = R / 16.0f;
  // Three CONCENTRIC arcs (shared virtual centre cyOff below the logo) at evenly-spaced radii -> the
  // bars stay parallel and clearly separated. Shallow sweep keeps the ends from drooping into the next.
  const float cyOff = 22.0f, rr[3] = { 27.0f, 22.0f, 17.0f }, sweep = 0.46f;   // R=16 design units
  float bw = R / 9.0f; if (bw < 1.3f) bw = 1.3f;        // bar thickness scales with size
  for (int b = 0; b < 3; b++) {
    int px0 = 0, py0 = 0;
    for (int i = 0; i <= 16; i++) {
      float th = -sweep + (2.0f * sweep) * i / 16.0f;    // 0 = top (middle), +/- = ends
      int px = cx + (int)lroundf(rr[b] * s * sinf(th));
      int py = cy + (int)lroundf((cyOff - rr[b] * cosf(th)) * s);
      if (i) t->drawWedgeLine(px0, py0, px, py, bw, bw, bar);
      px0 = px; py0 = py;
    }
  }
}

// ---------------- Clock ----------------
// Big local time that slowly drifts (anti-burn-in) + up to MAX_WORLD_CITIES world-clock rows.
// The time + status are composed off-screen in an puck::Canvas band and pushed in a single blit, so a
// time tick (or a drift step) never shows a clear-then-redraw flash. The city rows (change once a
// minute, fixed position) are drawn directly below the band.
class ClockApp : public App {
  int      lastSec = -1, lastMin = -1;
  uint32_t lastPhase = 0xFFFFFFFF;
  puck::Canvas band;                               // Digital time+status band (PSRAM); blitted each tick
  puck::Canvas face;                               // full-screen canvas for the Analog / Matrix faces
  bool     haveBand = false, haveFace = false;
  bool     h12 = false;                        // 12-hour (AM/PM) vs 24-hour, from Settings (cached on enter)
  int      faceIdx = 0;                        // 0=Digital 1=Analog 2=Matrix (tap the face to cycle; persisted)
  uint32_t lastFrame = 0;                      // Matrix animation pacing
  static const int BAND_TOP = 44, BAND_H = 112;   // screen rows 44..156; cities live at >=156
  // Matrix-rain state
  static const int MAT_CELL = 12;                 // glyph cell size (px)
  static const uint32_t MAT_FRAME_MS = 55;        // ~18 fps when awake
  int  drops[64]; int nDrops = 0; bool matrixInit = false;   // per-column head y

  void drawCities() {                          // up to 4 rows, fixed left column (don't drift -> no clip)
    WorldClock::City cs[MAX_WORLD_CITIES];
    int n = WorldClock::get(cs, MAX_WORLD_CITIES);
    puck::display().setFont(&fonts::Font0); puck::display().setTextSize(2);
    puck::display().setTextDatum(top_left);
    puck::display().setTextColor(CYAN, BLACK);
    for (int i = 0; i < n; i++) {
      int hh, mm; char row[24];
      if (WorldClock::timeFor(cs[i], hh, mm)) {
        if (h12) { int hr = hh % 12; if (hr == 0) hr = 12;
                   snprintf(row, sizeof(row), "%-3s %d:%02d%s", cs[i].label.c_str(), hr, mm, hh >= 12 ? "p" : "a"); }
        else       snprintf(row, sizeof(row), "%-3s %2d:%02d", cs[i].label.c_str(), hh, mm);
      } else
        snprintf(row, sizeof(row), "%-3s --:--", cs[i].label.c_str());
      puck::display().drawString(row, 40, 156 + i * 20);
    }
  }
  void drawName() {                            // user's custom label at the top (fixed, dim) — "" hides it
    String nm = Settings::displayName();
    if (!nm.length()) return;
    puck::display().setFont(&fonts::Font0); puck::display().setTextSize(2);
    puck::display().setTextDatum(top_center);
    puck::display().setTextColor(puck::display().color565(120, 140, 160), BLACK);
    puck::display().drawString(nm, puck::display().width() / 2, 14);
  }
  void drawBackInto(lgfx::LovyanGFX* t) {        // composite the back chip so a full-screen blit doesn't flicker it
    int o = layout::inset();
    t->fillRoundRect(4 + o, 4 + o, 34, 24, 5, DARKGREY);
    t->setTextDatum(middle_center); t->setFont(&fonts::Font0); t->setTextSize(2);
    t->setTextColor(WHITE, DARKGREY); t->drawString("<", 21 + o, 16 + o);
  }

  // ---- Analog face: full-screen dial composed into the `face` canvas (drift applied to the center) ----
  void drawAnalog(int h, int m, int s, int dx, int dy) {
    lgfx::LovyanGFX* t = haveFace ? (lgfx::LovyanGFX*)&face : (lgfx::LovyanGFX*)&puck::display();
    int w = t->width(), ht = t->height();
    t->fillScreen(BLACK);
    int cx = w / 2 + dx, cy = ht / 2 + 12 + dy, R = (w < ht ? w : ht) / 2 - 12;
    for (int i = 0; i < 12; i++) {                       // hour ticks (long every 3rd)
      float a = i * 30 * DEG_TO_RAD; bool major = (i % 3 == 0);
      int r0 = R - (major ? 12 : 6);
      t->drawWedgeLine(cx + (int)(sinf(a) * r0), cy - (int)(cosf(a) * r0),
                       cx + (int)(sinf(a) * R),  cy - (int)(cosf(a) * R),
                       major ? 2.0f : 1.0f, major ? 2.0f : 1.0f, major ? WHITE : DARKGREY);
    }
    float ha = ((h % 12) + m / 60.0f) * 30 * DEG_TO_RAD;
    float ma = (m + s / 60.0f) * 6 * DEG_TO_RAD;
    float sa = s * 6 * DEG_TO_RAD;
    t->drawWedgeLine(cx, cy, cx + (int)(sinf(ha) * R * 0.50f), cy - (int)(cosf(ha) * R * 0.50f), 4.0f, 1.5f, CYAN);
    t->drawWedgeLine(cx, cy, cx + (int)(sinf(ma) * R * 0.78f), cy - (int)(cosf(ma) * R * 0.78f), 3.0f, 1.0f, WHITE);
    t->drawWedgeLine(cx, cy, cx + (int)(sinf(sa) * R * 0.88f), cy - (int)(cosf(sa) * R * 0.88f), 1.5f, 1.0f, RED);
    t->fillSmoothCircle(cx, cy, 4, WHITE);
    String nm = Settings::displayName();
    if (nm.length()) { t->setFont(&fonts::Font0); t->setTextSize(2); t->setTextDatum(top_center);
                       t->setTextColor(t->color565(120, 140, 160), BLACK); t->drawString(nm, w / 2, 6); }
    drawBackInto(t);
    if (haveFace) face.pushSprite(0, 0);
  }

  // ---- Matrix face: falling-glyph rain + legible time; animates ~18fps, throttled when idle-dimmed ----
  void drawMatrix(int h, int m, int s, bool dimmed) {
    lgfx::LovyanGFX* t = haveFace ? (lgfx::LovyanGFX*)&face : (lgfx::LovyanGFX*)&puck::display();
    int w = t->width(), ht = t->height();
    if (!matrixInit) {
      nDrops = w / MAT_CELL; if (nDrops > 64) nDrops = 64;
      for (int i = 0; i < nDrops; i++) drops[i] = (int)(esp_random() % ht);
      matrixInit = true;
    }
    t->fillScreen(BLACK);
    t->setFont(&fonts::Font0); t->setTextSize(1); t->setTextDatum(top_left);
    for (int i = 0; i < nDrops; i++) {
      int x = i * MAT_CELL;
      for (int k = 0; k < 7; k++) {                      // trail: bright head -> fading green
        int y = drops[i] - k * MAT_CELL; if (y < 0 || y >= ht) continue;
        int row = y / MAT_CELL; char c = 33 + ((i * 31 + row * 7) % 90);   // stable per-cell glyph
        int gg = 255 - k * 36; if (gg < 40) gg = 40;
        t->setTextColor(k == 0 ? WHITE : t->color565(0, gg, 40));
        t->drawChar(c, x, y);
      }
      if (!dimmed) { drops[i] += MAT_CELL; if (drops[i] - 7 * MAT_CELL > ht) drops[i] = (int)(esp_random() % MAT_CELL); }
    }
    char buf[8];                                          // HH:MM over a dark backing, drift not applied (centered)
    if (h12) { int hr = h % 12; if (hr == 0) hr = 12; snprintf(buf, sizeof buf, "%d:%02d", hr, m); }
    else       snprintf(buf, sizeof buf, "%02d:%02d", h, m);
    t->setTextDatum(middle_center); t->setFont(&fonts::Font7); t->setTextSize(1);
    int tw = t->textWidth(buf) + 20;
    t->fillRect(w / 2 - tw / 2, ht / 2 - 30, tw, 60, BLACK);
    t->setTextColor(GREEN); t->drawString(buf, w / 2, ht / 2);
    drawBackInto(t);
    if (haveFace) face.pushSprite(0, 0);
  }

public:
  ClockApp() : App("Clock"), band(&puck::display()), face(&puck::display()) {}
  bool dimsWhenIdle() const override { return true; }   // the Clock is the idle/screensaver face -> it auto-dims
  void onEnter() override {
    puck::display().fillScreen(BLACK);
    h12 = Settings::clock12h();                 // cache the format (changes only across a reboot)
    faceIdx = Settings::clockFace(); if (faceIdx < 0 || faceIdx > 2) faceIdx = 0;
    if (faceIdx == 0) drawName();               // Digital: name sits above the band (y<44); persists across ticks
    lastSec = -1; lastMin = -1; lastPhase = 0xFFFFFFFF; lastFrame = 0; matrixInit = false;
    WorldClock::reload();                       // pick up any cities chosen in Settings
    if (!haveBand) {                            // Digital band sprite (PSRAM = ample on CoreS3)
      band.setColorDepth(16); band.setPsram(true);
      haveBand = (band.createSprite(puck::display().width(), BAND_H) != nullptr);
    }
    if (!haveFace) {                            // full-screen sprite for the Analog/Matrix faces
      face.setColorDepth(16); face.setPsram(true);
      haveFace = (face.createSprite(puck::display().width(), puck::display().height()) != nullptr);
    }
  }
  void loop() override {
    int h, m, s; ClockService::getTime(h, m, s);
    if (gTap.pressed) {                          // tap the body cycles the face (main.cpp already consumed the back chip)
      faceIdx = (faceIdx + 1) % 3; Settings::setClockFace(faceIdx);
      puck::display().fillScreen(BLACK);
      lastSec = -1; lastMin = -1; lastPhase = 0xFFFFFFFF; lastFrame = 0; matrixInit = false;
      if (faceIdx == 0) drawName();
      gTap.pressed = false;
    }
    uint32_t phase = millis() / CLOCK_DRIFT_PERIOD_MS;
    int w = puck::display().width(), cx = w / 2;
    int dx = (int)((phase * 7)  % (2 * CLOCK_DRIFT_MAX_PX + 1)) - CLOCK_DRIFT_MAX_PX;
    int dy = (int)((phase * 13) % (2 * CLOCK_DRIFT_MAX_PX + 1)) - CLOCK_DRIFT_MAX_PX;

    if (faceIdx == 2) {                          // Matrix: animate ~18fps (slowed to ~2.5fps when deep-idle-dimmed)
      bool dimmed = Dim::dimmed();
      if (millis() - lastFrame < (dimmed ? 400UL : MAT_FRAME_MS)) return;
      lastFrame = millis(); drawMatrix(h, m, s, dimmed); return;
    }

    if (s == lastSec && phase == lastPhase) return;     // Digital/Analog: redraw on second or drift change
    bool moved     = (phase != lastPhase);
    bool minOrFull = moved || (m != lastMin) || lastSec < 0;
    lastSec = s; lastPhase = phase;

    if (faceIdx == 1) { drawAnalog(h, m, s, dx, dy); return; }   // Analog (full-face)

    char buf[16]; const char* ap = nullptr;       // Digital (face 0): compose the band off-screen
    if (h12) { int hr = h % 12; if (hr == 0) hr = 12; ap = (h >= 12) ? "PM" : "AM";
               snprintf(buf, sizeof(buf), "%d:%02d:%02d", hr, m, s); }
    else     { snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s); }

    if (haveBand) {
      band.fillScreen(BLACK);                          // compose off-screen (no visible flash)
      band.setTextDatum(middle_center);
      band.setFont(&fonts::Font7); band.setTextSize(1); band.setTextColor(WHITE);
      int ty = (88 - BAND_TOP) + dy;
      band.drawString(buf, cx + dx, ty);               // time, sprite-local y
      if (ap) { int tw = band.textWidth(buf);          // AM/PM tag (Font7 can't draw letters) to the right
                band.setFont(&fonts::Font0); band.setTextSize(2); band.setTextColor(CYAN);
                band.setTextDatum(middle_left); band.drawString(ap, cx + dx + tw / 2 + 6, ty); }
      band.pushSprite(0, BAND_TOP);                    // single blit -> zero flicker, incl. on drift
    } else {                                           // fallback if the sprite couldn't allocate
      if (moved) puck::display().fillRect(0, BAND_TOP, w, BAND_H, BLACK);
      puck::display().setTextDatum(middle_center);
      puck::display().setFont(&fonts::Font7); puck::display().setTextSize(1);
      puck::display().setTextColor(WHITE, BLACK);
      puck::display().drawString(buf, cx + dx, 88 + dy);
      if (ap) { int tw = puck::display().textWidth(buf);
                puck::display().setFont(&fonts::Font0); puck::display().setTextSize(2); puck::display().setTextColor(CYAN, BLACK);
                puck::display().setTextDatum(middle_left); puck::display().drawString(ap, cx + dx + tw / 2 + 6, 88 + dy); }
    }

    if (minOrFull) { drawCities(); lastMin = m; }       // cities: fixed, opaque, redraw at most per minute
  }
};

// ---------------- Weather ----------------
// Pure display: the Weather service refreshes in the background every minute.
// We always show the last good reading + how long ago it updated, and an
// "updating..." hint while a refresh is in flight. Never blocks.
class WeatherApp : public App {
  uint32_t shownVer = 0xFFFFFFFF;
  bool     shownUpdating = false;
  bool     shownHas = false;
  puck::Canvas scope{&puck::display()};       // off-screen buffer (PSRAM) -> flicker-free on the ~60s refresh
  bool     haveScope = false;
  lgfx::LovyanGFX* g = &puck::display();  // draw target: the sprite during a full repaint, else the panel
  bool     showDetail = false;       // a city's 7-day forecast page is open (tap a city to enter; back chip to leave)
  int      detailIdx = 0;            // which reading the detail page shows

  void center(const char* line1, uint16_t c1, const char* line2, uint16_t c2) {
    int cx = puck::display().width() / 2, cy = puck::display().height() / 2;
    puck::display().fillScreen(BLACK);
    puck::display().setTextDatum(middle_center);
    puck::display().setFont(&fonts::Font0);
    puck::display().setTextSize(2);
    puck::display().setTextColor(c1, BLACK); puck::display().drawString(line1, cx, cy - 20);
    puck::display().setTextColor(c2, BLACK); puck::display().drawString(line2, cx, cy + 16);
  }

  // Bottom status line — shows the wall-clock time the reading was fetched
  // (static, so no per-second repaint), with an inline "updating" marker.
  void drawStatus(bool has, bool updating, int h, int m) {   // draws to the current target g
    int w = g->width(), y = g->height() - 16;
    g->fillRect(0, y - 10, w, 26, BLACK);
    g->setTextDatum(middle_center);
    g->setFont(&fonts::Font0);
    g->setTextSize(1);

    String s;
    uint16_t col;
    if (has) {
      char t[28];
      snprintf(t, sizeof(t), "updated %d:%02d", h, m);
      s = t;
      if (updating) s += "  -  updating";
      col = updating ? ORANGE : DARKGREY;
    } else {
      s   = updating ? "updating..." : "no update yet";
      col = ORANGE;
    }
    g->setTextColor(col, BLACK);
    g->drawString(s, w / 2, y);
  }

  void drawAll(const Weather::Reading& r, bool updating) {
    g = haveScope ? (lgfx::LovyanGFX*)&scope : (lgfx::LovyanGFX*)&puck::display();   // compose off-screen
    int cx = g->width() / 2, cy = g->height() / 2;
    g->fillScreen(BLACK);

    // City
    g->setTextDatum(top_center);
    g->setFont(&fonts::Font0);
    g->setTextSize(2);
    g->setTextColor(CYAN, BLACK);
    g->drawString(r.city.length() ? r.city : String("Weather"), cx, 16);

    // Today's high / low under the city (orange high, cyan low)
    if (r.hasDay) {
      g->setFont(&fonts::Font0); g->setTextSize(2);
      char hi[8], lo[8];
      snprintf(hi, sizeof(hi), "H %d", (int)lroundf(r.tmax));
      snprintf(lo, sizeof(lo), "L %d", (int)lroundf(r.tmin));
      int hw = g->textWidth(hi), gap = 18;
      int sx = cx - (hw + gap + g->textWidth(lo)) / 2;
      g->setTextDatum(middle_left);
      g->setTextColor(ORANGE, BLACK); g->drawString(hi, sx, 44);
      g->setTextColor(CYAN,   BLACK); g->drawString(lo, sx + hw + gap, 44);
    }

    // Big temperature (7-seg digits) + unit, centered as a group
    char num[8];
    snprintf(num, sizeof(num), "%d", (int)lroundf(r.temp));
    g->setFont(&fonts::Font7); g->setTextSize(1);
    int nw = g->textWidth(num);
    g->setFont(&fonts::Font0); g->setTextSize(3);
    const char* unit = Settings::tempF() ? "F" : "C";
    int uw = g->textWidth(unit);
    int startx = cx - (nw + 8 + uw) / 2;

    g->setTextDatum(middle_left);
    g->setTextColor(WHITE, BLACK);
    g->setFont(&fonts::Font7); g->setTextSize(1);
    g->drawString(num, startx, cy - 14);
    g->setFont(&fonts::Font0); g->setTextSize(3);
    g->drawString(unit, startx + nw + 8, cy - 14);

    // Condition + wind
    g->setTextDatum(middle_center);
    g->setFont(&fonts::Font0); g->setTextSize(2);
    g->setTextColor(GREEN, BLACK);
    g->drawString(Weather::describe(r.code), cx, cy + 38);

    g->setTextColor(DARKGREY, BLACK);
    char w[24];
    snprintf(w, sizeof(w), "wind %d %s", (int)lroundf(r.wind), Settings::tempF() ? "mph" : "km/h");
    g->drawString(w, cx, cy + 64);

    drawStatus(true, updating, r.atHour, r.atMin);   // uses g (= the sprite here)
    if (haveScope) scope.pushSprite(0, 0);
    g = &puck::display();
  }

  // Multi-city view: up to 4 cities in a 2x2 grid of quarters (city, temp, H/L, condition).
  void drawGrid(int n) {
    g = haveScope ? (lgfx::LovyanGFX*)&scope : (lgfx::LovyanGFX*)&puck::display();   // compose off-screen
    int w = g->width(), H = g->height();
    int cw = w / 2, ch = H / 2;
    g->fillScreen(BLACK);
    g->drawFastVLine(cw, 0, H, 0x2104);
    g->drawFastHLine(0, ch, w, 0x2104);
    for (int i = 0; i < n && i < 4; i++) {
      Weather::Reading r;
      if (!Weather::get(i, r)) continue;
      int x0 = (i % 2) * cw, y0 = (i / 2) * ch, cx = x0 + cw / 2;

      g->setTextDatum(top_center);
      g->setFont(&fonts::Font0); g->setTextSize(2);
      g->setTextColor(CYAN, BLACK);
      g->drawString(r.city.length() ? r.city : String("--"), cx, y0 + 8);

      if (!r.ok) {
        g->setTextDatum(middle_center);
        g->setTextColor(DARKGREY, BLACK);
        g->drawString("...", cx, y0 + ch / 2 + 8);
        continue;
      }

      char t[10]; snprintf(t, sizeof(t), "%d%s", (int)lroundf(r.temp), Settings::tempF() ? "F" : "C");
      g->setTextDatum(middle_center);
      g->setFont(&fonts::Font0); g->setTextSize(3);
      g->setTextColor(WHITE, BLACK);
      g->drawString(t, cx, y0 + 46);

      if (r.hasDay) {                                  // H (orange) / L (cyan)
        g->setFont(&fonts::Font0); g->setTextSize(2);
        char hi[8], lo[8];
        snprintf(hi, sizeof(hi), "H%d", (int)lroundf(r.tmax));
        snprintf(lo, sizeof(lo), "L%d", (int)lroundf(r.tmin));
        int hw = g->textWidth(hi), gap = 10;
        int sx = cx - (hw + gap + g->textWidth(lo)) / 2;
        g->setTextDatum(middle_left);
        g->setTextColor(ORANGE, BLACK); g->drawString(hi, sx, y0 + 74);
        g->setTextColor(CYAN,   BLACK); g->drawString(lo, sx + hw + gap, y0 + 74);
      }

      g->setTextDatum(middle_center);
      g->setFont(&fonts::Font0); g->setTextSize(1);
      g->setTextColor(GREEN, BLACK);
      g->drawString(Weather::describe(r.code), cx, y0 + 96);
    }
    if (haveScope) scope.pushSprite(0, 0);
    g = &puck::display();
  }

  // A city's detail page: current conditions + a 7-day forecast (composed off-screen).
  void drawCityDetail(int idx) {
    g = haveScope ? (lgfx::LovyanGFX*)&scope : (lgfx::LovyanGFX*)&puck::display();
    int w = g->width();
    g->fillScreen(BLACK);
    Weather::Reading r;
    bool has = Weather::get(idx, r);

    g->setTextDatum(top_center);
    g->setFont(&fonts::Font0); g->setTextSize(2);
    g->setTextColor(CYAN, BLACK);
    g->drawString(r.city.length() ? r.city : String("Weather"), w / 2, 6);

    if (!has || !r.ok) {
      g->setTextDatum(middle_center); g->setTextColor(DARKGREY, BLACK);
      g->drawString("no data yet", w / 2, 110);
      if (haveScope) scope.pushSprite(0, 0);
      g = &puck::display();
      return;
    }

    // current conditions, one compact line
    g->setTextDatum(middle_center); g->setTextSize(2); g->setTextColor(WHITE, BLACK);
    char cur[40];
    snprintf(cur, sizeof(cur), "%d%s  %s", (int)lroundf(r.temp), Settings::tempF() ? "F" : "C", Weather::describe(r.code));
    g->drawString(cur, w / 2, 34);

    // 7-day rows: day (white) | condition (green, small) | hi (orange) / lo (cyan), right-aligned
    static const char* DOW[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    int top = 58, rh = 26;
    for (int i = 0; i < r.nDays && i < Weather::FC_DAYS; i++) {
      int y = top + i * rh;
      g->setTextSize(2); g->setTextDatum(middle_left); g->setTextColor(WHITE, BLACK);
      g->drawString(i == 0 ? "Today" : (r.fcWday[i] >= 0 ? DOW[r.fcWday[i]] : "-"), 10, y);
      g->setTextSize(1); g->setTextDatum(middle_center); g->setTextColor(GREEN, BLACK);
      g->drawString(Weather::describe(r.fcCode[i]), w / 2 + 6, y);
      g->setTextSize(2); g->setTextDatum(middle_right);
      char hi[6], lo[6];
      snprintf(hi, sizeof(hi), "%d", (int)lroundf(r.fcMax[i]));
      snprintf(lo, sizeof(lo), "%d", (int)lroundf(r.fcMin[i]));
      int rx = w - 10;
      g->setTextColor(CYAN, BLACK);     g->drawString(lo, rx, y);                 int lw = g->textWidth(lo);
      g->setTextColor(DARKGREY, BLACK); g->drawString("/", rx - lw - 3, y);       int sw = g->textWidth("/");
      g->setTextColor(ORANGE, BLACK);   g->drawString(hi, rx - lw - 3 - sw - 3, y);
    }

    if (haveScope) scope.pushSprite(0, 0);
    g = &puck::display();
  }
  int focusIdx = 0;            // button nav: focused grid cell (multi-city view)

public:
  WeatherApp() : App("Weather") {}
  bool needsNet() const override { return true; }
  bool onBack() override {                        // detail page -> back to the city list/view
    if (showDetail) { showDetail = false; shownVer = 0xFFFFFFFF; shownHas = false; return true; }
    return false;
  }
  void onExit() override { Weather::setActive(false); }   // stop background fetches when the app closes

  void onEnter() override {
    shownVer = 0xFFFFFFFF; shownUpdating = false; shownHas = false; showDetail = false;
    if (!haveScope) {                            // allocate the off-screen buffer once
      scope.setColorDepth(16); scope.setPsram(true);
      haveScope = (scope.createSprite(puck::display().width(), puck::display().height()) != nullptr);
    }
    puck::display().fillScreen(BLACK);
    Weather::setActive(true); // resume fetching while the app is open
    Weather::refreshNow();    // ask for a fresh pull on open...
    loop();                   // ...but paint whatever we already have right now
  }

  void loop() override {
    uint32_t ver = Weather::version();
    bool upd = Weather::updating();
    int  n   = Weather::count();

    // tap a city -> open its 7-day detail (grid: tapped quarter; single: index 0). A tap on the
    // detail (or the back chip) returns to the list/view.
    if (gTap.pressed) {
      if (!showDetail) {
        int idx = 0;
        if (n > 1) {
          int cw = puck::display().width() / 2, ch = puck::display().height() / 2;
          idx = (gTap.y < ch ? 0 : 2) + (gTap.x < cw ? 0 : 1);
        }
        if (idx >= 0 && idx < n) { showDetail = true; detailIdx = idx; shownVer = 0xFFFFFFFF; }
      } else {
        showDetail = false; shownVer = 0xFFFFFFFF; shownHas = false;
      }
      gTap.pressed = false;
    }

    if (showDetail) {                            // 7-day forecast page
      if (ver != shownVer) { drawCityDetail(detailIdx); shownVer = ver; }
      return;
    }

    if (n > 1) {                                 // multi-city grid
      if (ver != shownVer) { drawGrid(n); shownVer = ver; shownHas = true; }
      return;
    }

    // single location (home, or one configured city) — detailed view
    Weather::Reading r;
    bool has = Weather::latest(r);

    // New reading (or data first appeared) -> full repaint.
    if (ver != shownVer || has != shownHas) {
      if (has) {
        drawAll(r, upd);
      } else {
        g = haveScope ? (lgfx::LovyanGFX*)&scope : (lgfx::LovyanGFX*)&puck::display();
        int cx = g->width() / 2, cy = g->height() / 2;
        g->fillScreen(BLACK);
        g->setTextDatum(middle_center);
        g->setFont(&fonts::Font0); g->setTextSize(2);
        g->setTextColor(CYAN, BLACK);
        g->drawString("Weather", cx, cy - 20);
        drawStatus(false, upd, -1, -1);
        if (haveScope) scope.pushSprite(0, 0);
        g = &puck::display();
      }
      shownVer = ver; shownHas = has; shownUpdating = upd;
      return;
    }

    // Only the "updating" marker can change between readings -> repaint status line.
    if (upd != shownUpdating) {
      drawStatus(has, upd, r.atHour, r.atMin);
      shownUpdating = upd;
    }
  }

  // ---- button nav: city-grid cells (multi-city view; single/detail use the back chip) ----
  bool focusCell(int i, int& x, int& y, int& w, int& h) {
    int n = Weather::count();
    if (showDetail || n <= 1 || i < 0 || i >= n) return false;
    int cw = puck::display().width() / 2, ch = puck::display().height() / 2;
    x = (i % 2) * cw; y = (i / 2) * ch; w = cw; h = ch; return true;
  }
  int  focusCount() override { int x, y, w, h, n = 0; while (focusCell(n, x, y, w, h)) n++; return n; }
  void focusMove(int d) override { int n = focusCount(); if (!n) return; focusIdx = (focusIdx + d + n) % n; shownVer = 0xFFFFFFFF; }
  void focusSelect() override { int x, y, w, h; if (focusCell(focusIdx, x, y, w, h)) { gTap.pressed = true; gTap.x = x + w / 2; gTap.y = y + h / 2; } }
  void drawFocus() override { int x, y, w, h; if (focusCell(focusIdx, x, y, w, h)) puck::display().drawRoundRect(x + 2, y + 2, w - 4, h - 4, 4, WHITE); }
};

// ---------------- Stocks ----------------
// LIST: watchlist rows (ticker + price + % change), refreshed every few seconds from the Stocks
// service; tap a row for DETAIL (day high/low, open, prev close, next earnings). "+ Add ticker"
// opens a keyboard SEARCH -> RESULTS (Finnhub symbol search) -> tap a match to add. The LIST/DETAIL
// screens compose off-screen so the periodic price refresh never flickers. Needs FINNHUB_API_KEY.
class StocksApp : public App {
  enum Mode { LIST, DETAIL, SEARCH, RESULTS };
  Mode mode = LIST;

  puck::Canvas scope{&puck::display()};        // off-screen buffer (PSRAM) -> flicker-free refresh
  bool haveScope = false;
  lgfx::LovyanGFX* g = &puck::display();        // draw target: the sprite during a repaint, else the panel
  uint32_t shownVer = 0xFFFFFFFF;
  bool dirty = true;

  String selSym;                                // DETAIL: the focused ticker
  String typed;                                 // SEARCH: typed symbol
  int    focusIdx = 0;                          // button-nav cursor
  int    order[MAX_STOCKS]; int orderN = 0;     // LIST display order (indices into the watchlist), sorted by top movers

  const char* kbRows[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
  static const int KW = 32, KH = 30; int KB_TOP = 58;  // KB_TOP voff'd per-app in onEnter
  static const int LIST_TOP = 34, ROW_H = 22;   // watchlist rows
  static const int RES_TOP  = 30, RES_H = 26;   // search-result rows
  int addY()  { return puck::display().height() - 28; }   // +Add / Remove button row
  static uint16_t dirColor(float d) { return d > 0 ? GREEN : (d < 0 ? RED : DARKGREY); }

  void beginScope() {
    if (!haveScope) {
      scope.setColorDepth(16); scope.setPsram(true);
      haveScope = (scope.createSprite(puck::display().width(), puck::display().height()) != nullptr);
    }
  }
  void statRow(int& ry, const char* k, const String& v, uint16_t vc) {   // draws a label/value line to g
    g->setFont(&fonts::Font0); g->setTextSize(2);
    g->setTextDatum(middle_left);  g->setTextColor(DARKGREY, BLACK); g->drawString(k, 16, ry);
    g->setTextDatum(middle_right); g->setTextColor(vc, BLACK);       g->drawString(v, g->width() - 16, ry);
    ry += 18;
  }

  void drawBackInto(lgfx::LovyanGFX* t) {        // composite the back chip into the off-screen target -> atomic blit (no flicker)
    int o = layout::inset();
    t->fillRoundRect(4 + o, 4 + o, 34, 24, 5, DARKGREY);
    t->setTextDatum(middle_center); t->setFont(&fonts::Font0); t->setTextSize(2);
    t->setTextColor(WHITE, DARKGREY); t->drawString("<", 21 + o, 16 + o);
  }

  // LIST display order: sort the watchlist by |% change| descending (biggest movers, up or down, on
  // top); tickers with no quote yet sink to the bottom. drawList + the tap/focus hit-test all read order[].
  void computeOrder() {
    int n = Stocks::count();
    Stocks::Quote q[MAX_STOCKS];
    for (int i = 0; i < n; i++) { Stocks::get(i, q[i]); order[i] = i; }
    for (int a = 1; a < n; a++) {                       // insertion sort by |dp| desc (stable for ties)
      int key = order[a];
      float ka = q[key].valid ? fabsf(q[key].dp) : -1.0f;
      int b = a - 1;
      while (b >= 0) {
        float kb = q[order[b]].valid ? fabsf(q[order[b]].dp) : -1.0f;
        if (kb >= ka) break;
        order[b + 1] = order[b]; b--;
      }
      order[b + 1] = key;
    }
    orderN = n;
  }

  // ---- LIST (off-screen) ----
  void drawList() {
    g = haveScope ? (lgfx::LovyanGFX*)&scope : (lgfx::LovyanGFX*)&puck::display();
    int w = g->width(), h = g->height();
    g->fillScreen(BLACK);
    g->setTextDatum(top_center); g->setFont(&fonts::Font0); g->setTextSize(2);
    g->setTextColor(CYAN, BLACK); g->drawString("Stocks", w / 2, 8);   // centered, clear of the top-left back chip

    if (!Stocks::configured()) {
      g->setTextDatum(middle_center); g->setTextSize(2); g->setTextColor(ORANGE, BLACK);
      g->drawString("set", w / 2, h / 2 - 16);
      g->setTextSize(1); g->drawString("FINNHUB_API_KEY", w / 2, h / 2 + 10);
    } else if (Stocks::count() == 0) {
      g->setTextDatum(middle_center); g->setTextSize(2); g->setTextColor(DARKGREY, BLACK);
      g->drawString("Tap + to add", w / 2, h / 2 - 8);
      g->drawString("a ticker", w / 2, h / 2 + 18);
    } else {
      computeOrder();                            // sort by top movers (|% change| desc)
      for (int k = 0; k < orderN; k++) {
        Stocks::Quote q; if (!Stocks::get(order[k], q)) continue;
        int y = LIST_TOP + k * ROW_H + ROW_H / 2;
        g->setFont(&fonts::Font0); g->setTextSize(2);
        g->setTextDatum(middle_left); g->setTextColor(WHITE, BLACK); g->drawString(q.sym, 12, y);
        if (q.valid) {
          char pr[16]; snprintf(pr, sizeof pr, "$%.2f", q.price);
          char ch[12]; snprintf(ch, sizeof ch, "%+.2f%%", q.dp);
          uint16_t col = dirColor(q.change);
          g->setTextDatum(middle_right); g->setTextSize(2); g->setTextColor(col, BLACK);
          g->drawString(pr, w - 12, y);
          int pw = g->textWidth(pr);
          g->setTextSize(1); g->drawString(ch, w - 12 - pw - 10, y);
        } else {
          g->setTextDatum(middle_right); g->setTextSize(2); g->setTextColor(DARKGREY, BLACK);
          g->drawString("...", w - 12, y);
        }
      }
    }
    if (Stocks::configured()) {                 // + Add ticker
      int ay = addY();
      g->drawRoundRect(8, ay, w - 16, 24, 5, GREEN);
      g->setTextDatum(middle_center); g->setFont(&fonts::Font0); g->setTextSize(2);
      g->setTextColor(GREEN, BLACK); g->drawString("+ Add ticker", w / 2, ay + 12);
    }
    drawBackInto(g);                            // chip in the composition -> no flicker on refresh
    if (haveScope) scope.pushSprite(0, 0);
    g = &puck::display();
  }

  // ---- DETAIL (off-screen; refreshes with the price) ----
  void drawDetail() {
    g = haveScope ? (lgfx::LovyanGFX*)&scope : (lgfx::LovyanGFX*)&puck::display();
    int w = g->width(), h = g->height();
    g->fillScreen(BLACK);

    Stocks::Quote q; bool ok = false;
    int n = Stocks::count();
    for (int i = 0; i < n; i++) { Stocks::Quote t; if (Stocks::get(i, t) && t.sym == selSym) { q = t; ok = true; break; } }

    g->setTextDatum(top_center); g->setFont(&fonts::Font0); g->setTextSize(3);
    g->setTextColor(CYAN, BLACK); g->drawString(selSym, w / 2, 24);   // below the top-left back chip

    if (!ok || !q.valid) {
      g->setTextDatum(middle_center); g->setTextSize(2); g->setTextColor(DARKGREY, BLACK);
      g->drawString("loading...", w / 2, h / 2);
      drawBackInto(g);
      if (haveScope) scope.pushSprite(0, 0); g = &puck::display(); return;
    }

    uint16_t col = dirColor(q.change);
    char pr[16]; snprintf(pr, sizeof pr, "$%.2f", q.price);
    g->setTextDatum(top_center); g->setTextSize(3); g->setTextColor(WHITE, BLACK);
    g->drawString(pr, w / 2, 56);
    char ch[28]; snprintf(ch, sizeof ch, "%+.2f (%+.2f%%)", q.change, q.dp);
    g->setTextSize(2); g->setTextColor(col, BLACK); g->drawString(ch, w / 2, 92);

    int ry = 124; char b[16];
    snprintf(b, sizeof b, "$%.2f", q.high);      statRow(ry, "Day High",   b, WHITE);
    snprintf(b, sizeof b, "$%.2f", q.low);       statRow(ry, "Day Low",    b, WHITE);
    snprintf(b, sizeof b, "$%.2f", q.open);      statRow(ry, "Open",       b, DARKGREY);
    snprintf(b, sizeof b, "$%.2f", q.prevClose); statRow(ry, "Prev Close", b, DARKGREY);
    bool haveEarn = q.earnings.length() && q.earnings != "none";
    String e = (q.earnings.length() == 0) ? String("...") : (haveEarn ? q.earnings : String("--"));
    statRow(ry, "Next earnings", e, haveEarn ? ORANGE : DARKGREY);

    int ay = addY();                            // Remove
    g->drawRoundRect(8, ay, w - 16, 24, 5, RED);
    g->setTextDatum(middle_center); g->setFont(&fonts::Font0); g->setTextSize(2);
    g->setTextColor(RED, BLACK); g->drawString("Remove", w / 2, ay + 12);

    drawBackInto(g);                            // chip in the composition -> no flicker on refresh
    if (haveScope) scope.pushSprite(0, 0);
    g = &puck::display();
  }

  // ---- SEARCH keyboard (interactive -> drawn directly) ----
  void drawSearch() {
    int w = puck::display().width();
    puck::display().fillScreen(BLACK);
    puck::display().setTextDatum(top_center); puck::display().setFont(&fonts::Font0); puck::display().setTextSize(1);
    puck::display().setTextColor(DARKGREY, BLACK); puck::display().drawString("type a ticker symbol", w / 2, 4);
    puck::display().setTextDatum(middle_center); puck::display().setTextSize(3); puck::display().setTextColor(WHITE, BLACK);
    puck::display().drawString(typed.length() ? typed : String("_"), w / 2, 28);
    puck::display().setTextSize(2);
    for (int r = 0; r < 4; r++) {
      const char* row = kbRows[r];
      for (int i = 0; row[i]; i++) {
        int x = i * KW, y = KB_TOP + r * KH;
        puck::display().drawRoundRect(x + 1, y + 1, KW - 2, KH - 2, 3, DARKGREY);
        char s[2] = { row[i], 0 };
        puck::display().setTextColor(CYAN, BLACK); puck::display().drawString(s, x + KW / 2, y + KH / 2);
      }
    }
    int by = KB_TOP + 4 * KH;
    const char* labels[2] = { "DEL", "SEARCH" }; uint16_t cols[2] = { ORANGE, GREEN };
    for (int i = 0; i < 2; i++) {
      int x = i * (w / 2);
      puck::display().drawRoundRect(x + 2, by + 1, w / 2 - 4, 34, 4, cols[i]);
      puck::display().setTextColor(cols[i], BLACK); puck::display().drawString(labels[i], x + (w / 2) / 2, by + 18);
    }
  }
  void handleSearchTap(int x, int y) {
    int w = puck::display().width(); int by = KB_TOP + 4 * KH;
    if (y >= by) {
      int b = x / (w / 2);
      if (b == 0) { if (typed.length()) typed.remove(typed.length() - 1); dirty = true; }
      else if (b == 1 && typed.length()) { Stocks::requestSearch(typed); mode = RESULTS; focusIdx = 0; shownVer = 0xFFFFFFFF; dirty = true; }
      return;
    }
    int r = (y - KB_TOP) / KH;
    if (r >= 0 && r < 4) {
      const char* row = kbRows[r]; int i = x / KW;
      if (i >= 0 && i < (int)strlen(row)) { if (typed.length() < 6) typed += row[i]; dirty = true; }
    }
  }

  // ---- RESULTS (off-screen; matches arrive async so it redraws on version bump) ----
  void drawResults() {
    g = haveScope ? (lgfx::LovyanGFX*)&scope : (lgfx::LovyanGFX*)&puck::display();
    int w = g->width(), h = g->height();
    g->fillScreen(BLACK);
    g->setTextDatum(top_center); g->setFont(&fonts::Font0); g->setTextSize(2); g->setTextColor(CYAN, BLACK);
    g->drawString(typed, w / 2, 6);
    if (Stocks::searchPending()) {
      g->setTextDatum(middle_center); g->setTextSize(2); g->setTextColor(DARKGREY, BLACK);
      g->drawString("searching...", w / 2, h / 2);
    } else {
      Stocks::Match m[8]; int n = Stocks::searchResults(m, 8);
      if (n == 0) {
        g->setTextDatum(middle_center); g->setTextSize(2); g->setTextColor(DARKGREY, BLACK);
        g->drawString("no matches", w / 2, h / 2);
      }
      for (int i = 0; i < n; i++) {
        int y = RES_TOP + i * RES_H + RES_H / 2;
        g->setTextDatum(middle_left); g->setFont(&fonts::Font0); g->setTextSize(2);
        g->setTextColor(WHITE, BLACK); g->drawString(m[i].sym, 12, y);
        String d = m[i].desc; if (d.length() > 24) d = d.substring(0, 24);
        g->setTextDatum(middle_right); g->setTextSize(1); g->setTextColor(DARKGREY, BLACK);
        g->drawString(d, w - 12, y);
      }
    }
    drawBackInto(g);
    if (haveScope) scope.pushSprite(0, 0);
    g = &puck::display();
  }
  void handleResultsTap(int x, int y) {
    if (Stocks::searchPending() || y < RES_TOP) return;
    Stocks::Match m[8]; int n = Stocks::searchResults(m, 8);
    int i = (y - RES_TOP) / RES_H;
    if (i >= 0 && i < n) { Stocks::add(m[i].sym); typed = ""; mode = LIST; focusIdx = 0; shownVer = 0xFFFFFFFF; dirty = true; }
  }

public:
  StocksApp() : App("Stocks") {}
  bool needsNet() const override { return true; }
  bool needsSetup() const override { return !Stocks::configured(); }      // no Finnhub key -> route to Settings on open
  const char* setupHint() const override { return "Add a Finnhub key"; }

  void onEnter() override {
    mode = LIST; selSym = ""; typed = ""; focusIdx = 0;
    beginScope();
    shownVer = 0xFFFFFFFF; dirty = true;
    Stocks::setActive(true);                 // resume polling while the app is open
    puck::display().fillScreen(BLACK);
  }
  void onExit() override { Stocks::setFocus(""); Stocks::setActive(false); }   // idle polling when closed (cached prices persist)

  bool onBack() override {
    if (mode == DETAIL)  { mode = LIST;   Stocks::setFocus(""); focusIdx = 0; shownVer = 0xFFFFFFFF; dirty = true; return true; }
    if (mode == RESULTS) { mode = SEARCH; focusIdx = 0; dirty = true; return true; }
    if (mode == SEARCH)  { mode = LIST;   focusIdx = 0; shownVer = 0xFFFFFFFF; dirty = true; return true; }
    return false;
  }

  void loop() override {
    uint32_t ver = Stocks::version();
    if (gTap.pressed) {
      int x = gTap.x, y = gTap.y;
      if (mode == LIST) {
        if (Stocks::configured()) {
          int ay = addY();
          if (y >= ay && y < ay + 24) { mode = SEARCH; typed = ""; focusIdx = 0; dirty = true; }
          else if (y >= LIST_TOP && y < LIST_TOP + orderN * ROW_H) {   // rows are in sorted (top-mover) order
            int k = (y - LIST_TOP) / ROW_H; Stocks::Quote q;
            if (k >= 0 && k < orderN && Stocks::get(order[k], q)) { selSym = q.sym; Stocks::setFocus(selSym); mode = DETAIL; focusIdx = 0; shownVer = 0xFFFFFFFF; dirty = true; }
          }
        }
      } else if (mode == DETAIL) {
        int ay = addY();
        if (y >= ay && y < ay + 24) { Stocks::remove(selSym); Stocks::setFocus(""); mode = LIST; focusIdx = 0; shownVer = 0xFFFFFFFF; dirty = true; }
      } else if (mode == SEARCH)  handleSearchTap(x, y);
      else if (mode == RESULTS)   handleResultsTap(x, y);
      gTap.pressed = false;
    }

    bool dataMode = (mode != SEARCH);            // SEARCH redraws only on key taps (no price-tick flicker)
    if (dirty || (dataMode && ver != shownVer)) {
      switch (mode) {
        case LIST:    drawList();    break;
        case DETAIL:  drawDetail();  break;
        case SEARCH:  drawSearch();  break;
        case RESULTS: drawResults(); break;
      }
      shownVer = ver; dirty = false;
    }
  }

  // ---- button nav ----
  int focusCount() override {
    if (mode == LIST)    return Stocks::configured() ? Stocks::count() + 1 : 0;   // rows + Add
    if (mode == DETAIL)  return 1;                                                // Remove
    if (mode == RESULTS) { Stocks::Match m[8]; return Stocks::searchPending() ? 0 : Stocks::searchResults(m, 8); }
    return 0;                                                                     // SEARCH = touch only
  }
  void focusMove(int d) override { int n = focusCount(); if (!n) return; focusIdx = (focusIdx + d + n) % n; dirty = true; }
  void focusSelect() override {
    int fc = focusCount(); if (!fc) return; if (focusIdx >= fc) focusIdx = 0;
    int w = puck::display().width();
    if (mode == LIST) {
      int n = Stocks::count();
      if (focusIdx < n) { gTap.pressed = true; gTap.x = 40; gTap.y = LIST_TOP + focusIdx * ROW_H + ROW_H / 2; }
      else              { gTap.pressed = true; gTap.x = w / 2; gTap.y = addY() + 12; }       // Add
    } else if (mode == DETAIL)  { gTap.pressed = true; gTap.x = w / 2; gTap.y = addY() + 12; } // Remove
    else if (mode == RESULTS)   { gTap.pressed = true; gTap.x = 40; gTap.y = RES_TOP + focusIdx * RES_H + RES_H / 2; }
  }
  void drawFocus() override {
    int fc = focusCount(); if (!fc) return; if (focusIdx >= fc) focusIdx = 0;
    int w = puck::display().width();
    if (mode == LIST) {
      int n = Stocks::count();
      if (focusIdx < n) puck::display().drawRect(4, LIST_TOP + focusIdx * ROW_H, w - 8, ROW_H, WHITE);
      else              puck::display().drawRoundRect(8, addY(), w - 16, 24, 5, WHITE);
    } else if (mode == DETAIL)  puck::display().drawRoundRect(8, addY(), w - 16, 24, 5, WHITE);
    else if (mode == RESULTS)   puck::display().drawRect(4, RES_TOP + focusIdx * RES_H, w - 8, RES_H, WHITE);
  }
};

// ---------------- Spotify (Now Playing) ----------------
// Display-first: reads the Spotify service's mtx-guarded cache (track/artist/album/progress) and decodes
// the album-art JPEG here on the UI thread into a PSRAM sprite when it changes. The whole screen is
// composed off-screen and blitted once -> flicker-free on the ~4s poll. Three touch buttons drive
// prev / play-pause / next. The setup gate (no linked token) routes the user to Settings.
class SpotifyApp : public App {
  puck::Canvas scope{&puck::display()};        // full-screen compose buffer (PSRAM)
  puck::Canvas art{&puck::display()};          // decoded album art (PSRAM)
  bool haveScope = false, haveArt = false;
  lgfx::LovyanGFX* g = &puck::display();
  uint8_t* artBuf = nullptr;                    // scratch for copying the service's JPEG bytes out of the mtx
  int artSide = 0;                              // album-art sprite side (= max screen dim) -> fills the screen
  static const size_t ARTBUF_MAX = 96 * 1024;
  uint32_t shownVer = 0xFFFFFFFF, shownArtVer = 0xFFFFFFFF;
  bool artOk = false;                           // the current track's art decoded
  bool dirty = true;
  uint32_t lastDraw = 0;
  int  progBase = 0, dur = 0; uint32_t progAt = 0; bool playing = false;  // progress interpolation
  int  focusIdx = 1;                            // button-nav cursor: 0=prev 1=play 2=next

  void beginScope() {
    if (!haveScope) { scope.setColorDepth(16); scope.setPsram(true);
      haveScope = (scope.createSprite(puck::display().width(), puck::display().height()) != nullptr); }
    if (!artSide) { int W = puck::display().width(), H = puck::display().height(); artSide = (W > H ? W : H); }
    if (!haveArt) { art.setColorDepth(16); art.setPsram(true);
      haveArt = (art.createSprite(artSide, artSide) != nullptr); }
    if (!artBuf) artBuf = (uint8_t*)heap_caps_malloc(ARTBUF_MAX, MALLOC_CAP_SPIRAM);
  }
  void drawBackInto(lgfx::LovyanGFX* t) {
    int o = layout::inset();
    t->fillRoundRect(4 + o, 4 + o, 34, 24, 5, DARKGREY);
    t->setTextDatum(middle_center); t->setFont(&fonts::Font0); t->setTextSize(2);
    t->setTextColor(WHITE, DARKGREY); t->drawString("<", 21 + o, 16 + o);
  }
  String fit(lgfx::LovyanGFX* t, const String& s, int maxw) {   // trim + ellipsis to fit the current font/size
    if (!s.length() || t->textWidth(s) <= maxw) return s;
    String r = s; while (r.length() > 1 && t->textWidth(r + "...") > maxw) r.remove(r.length() - 1);
    return r + "...";
  }
  int ctlY() { return puck::display().height() - 18; }   // controls sit in the bottom overlay band
  int ctlX(int i) { return puck::display().width() / 2 + (i - 1) * 84; }
  int ctlHit(int x, int y) { if (abs(y - ctlY()) > 24) return -1;
    for (int i = 0; i < 3; i++) if (abs(x - ctlX(i)) <= 40) return i; return -1; }
  void drawCtl(lgfx::LovyanGFX* t, int i, bool isPlaying, uint16_t col) {
    int cx = ctlX(i), cy = ctlY();
    if (i == 0) {                                 // prev: bar + two left triangles
      t->fillRect(cx - 14, cy - 9, 3, 18, col);
      t->fillTriangle(cx + 1, cy - 9, cx + 1, cy + 9, cx - 9, cy, col);
      t->fillTriangle(cx + 12, cy - 9, cx + 12, cy + 9, cx + 2, cy, col);
    } else if (i == 2) {                           // next: two right triangles + bar
      t->fillTriangle(cx - 12, cy - 9, cx - 12, cy + 9, cx - 2, cy, col);
      t->fillTriangle(cx - 1, cy - 9, cx - 1, cy + 9, cx + 9, cy, col);
      t->fillRect(cx + 11, cy - 9, 3, 18, col);
    } else if (isPlaying) {                         // pause: two bars
      t->fillRect(cx - 9, cy - 11, 6, 22, col); t->fillRect(cx + 3, cy - 11, 6, 22, col);
    } else {                                        // play: right triangle
      t->fillTriangle(cx - 8, cy - 12, cx - 8, cy + 12, cx + 13, cy, col);
    }
  }
  static void fmtTime(int ms, char* out, size_t n) {
    if (ms < 0) ms = 0; int s = ms / 1000; snprintf(out, n, "%d:%02d", s / 60, s % 60);
  }

  void drawNow() {
    g = haveScope ? (lgfx::LovyanGFX*)&scope : (lgfx::LovyanGFX*)&puck::display();
    int w = g->width(), h = g->height();
    g->fillScreen(BLACK);
    Spotify::Now n; bool has = Spotify::get(n);
    if (!has) {
      bool tok = Spotify::tokenOk(); int hc = Spotify::nowHttp();   // diagnose why there's no track
      const char* head; const char* hint;
      if (!Spotify::authed())   { head = "Sign-in failed"; hint = "re-link in Settings"; }   // token actually rejected
      else if (!tok || hc < 0)  { head = "Connecting";     hint = "reaching Spotify"; }       // refreshing/fetching (startup)
      else if (hc == 403)       { head = "Needs Premium";  hint = "for Spotify playback"; }
      else                      { head = "Nothing playing"; hint = "play a track in Spotify"; }
      drawSpotifyMark(g, w / 2, 58, 32);                            // green Spotify mark (disc + sound-wave bars)
      g->setTextDatum(top_center); g->setFont(&fonts::Font0);
      g->setTextSize(3); g->setTextColor(WHITE, BLACK);
      g->drawString(head, w / 2, 110);                             // bigger headline
      g->setTextSize(2); g->setTextColor(g->color565(150, 160, 170), BLACK);
      g->drawString(hint, w / 2, 150);
      char dbg[24]; snprintf(dbg, sizeof dbg, "tok=%d  http=%d", tok ? 1 : 0, hc);   // faint support footer
      g->setTextDatum(bottom_center); g->setTextSize(1); g->setTextColor(g->color565(46, 52, 60), BLACK);
      g->drawString(dbg, w / 2, h - 4);
      drawBackInto(g); if (haveScope) scope.pushSprite(0, 0); g = &puck::display(); return;
    }
    // full-bleed album art: cover-crop the square art over the whole screen
    int offx = (w - artSide) / 2, offy = (h - artSide) / 2;
    if (artOk && haveArt) art.pushSprite(g, offx, offy);
    else { g->fillScreen(g->color565(18, 20, 24)); drawSpotifyMark(g, w / 2, h / 2 - 26, 30); }

    // bottom overlay: a dark band carrying track / artist, a progress bar, and the controls
    const int bandH = 88, by = h - bandH;
    uint16_t bandbg = g->color565(8, 10, 12);
    g->fillRect(0, by, w, bandH, bandbg);
    g->drawFastHLine(0, by, w, g->color565(40, 44, 50));            // subtle top edge
    g->setTextDatum(top_center); g->setFont(&fonts::Font0);
    g->setTextSize(2); g->setTextColor(WHITE, bandbg);
    g->drawString(fit(g, n.track, w - 16), w / 2, by + 8);
    g->setTextSize(1); g->setTextColor(g->color565(150, 160, 170), bandbg);
    g->drawString(fit(g, n.artist, w - 16), w / 2, by + 28);
    int cur = progBase + (playing ? (int)(millis() - progAt) : 0);
    if (dur > 0 && cur > dur) cur = dur; if (cur < 0) cur = 0;
    int bx = 18, bw = w - 36, pby = by + 44;                        // progress bar
    g->drawRoundRect(bx, pby, bw, 4, 2, g->color565(70, 76, 84));
    if (dur > 0) g->fillRoundRect(bx, pby, (int)((long)bw * cur / dur), 4, 2, g->color565(30, 215, 96));
    for (int i = 0; i < 3; i++) drawCtl(g, i, n.playing, WHITE);    // Prev / Play-Pause / Next
    if (Spotify::noDevice()) {
      g->setTextDatum(top_right); g->setTextSize(1); g->setTextColor(ORANGE, bandbg);
      g->drawString("no device", w - 6, by + 4);
    }
    drawBackInto(g);
    if (haveScope) scope.pushSprite(0, 0);
    g = &puck::display();
  }

public:
  SpotifyApp() : App("Spotify") {}
  bool needsNet() const override { return true; }
  bool needsSetup() const override { return !Spotify::configured(); }   // not linked -> route to Settings
  const char* setupHint() const override { return "Link Spotify"; }
  const char* setupUrl()  const override { return SPOTIFY_LOGIN_URL; }  // shown on the setup prompt: where to log in

  void onEnter() override {
    beginScope();
    shownVer = 0xFFFFFFFF; shownArtVer = 0xFFFFFFFF; artOk = false; dirty = true; lastDraw = 0; focusIdx = 1;
    Spotify::setActive(true);
    puck::display().fillScreen(BLACK);
  }
  void onExit() override { Spotify::setActive(false); }

  void loop() override {
    uint32_t av = Spotify::artVersion();          // decode new album art on the UI thread when it changes
    if (av != shownArtVer) {
      shownArtVer = av; artOk = false;
      if (haveArt && artBuf) {
        size_t nb = Spotify::artCopy(artBuf, ARTBUF_MAX);
        if (nb) { Spotify::Now n; Spotify::get(n);
          art.fillScreen(BLACK);
          float sc = (n.imgW > 0) ? (float)artSide / n.imgW : 0.5f;  // cover the whole screen
          artOk = art.drawJpg(artBuf, nb, 0, 0, artSide, artSide, 0, 0, sc, sc); }
      }
      dirty = true;
    }
    if (gTap.pressed) {                           // controls
      int hit = ctlHit(gTap.x, gTap.y);
      if (hit == 0) Spotify::prev(); else if (hit == 1) Spotify::playPause(); else if (hit == 2) Spotify::next();
      if (hit >= 0) dirty = true;
      gTap.pressed = false;
    }
    uint32_t ver = Spotify::version();            // re-sync the progress base on a data change
    if (ver != shownVer) {
      Spotify::Now n; Spotify::get(n);
      progBase = n.progressMs; dur = n.durationMs; playing = n.playing; progAt = millis();
      shownVer = ver; dirty = true;
    }
    if (dirty || (playing && millis() - lastDraw >= 1000)) { drawNow(); dirty = false; lastDraw = millis(); }
  }

  // ---- button nav (the 3 controls) ----
  int focusCount() override { Spotify::Now n; return Spotify::get(n) ? 3 : 0; }
  void focusMove(int d) override { int n = focusCount(); if (!n) return; focusIdx = (focusIdx + d + n) % n; dirty = true; }
  void focusSelect() override {
    if (focusIdx == 0) Spotify::prev(); else if (focusIdx == 1) Spotify::playPause(); else Spotify::next(); dirty = true;
  }
  void drawFocus() override {
    Spotify::Now n; if (!Spotify::get(n)) return; if (focusIdx < 0 || focusIdx > 2) focusIdx = 1;
    puck::display().drawRoundRect(ctlX(focusIdx) - 42, ctlY() - 22, 84, 44, 6, WHITE);
  }
};

// ---------------- Flight ----------------
// LIST: nearest aircraft (tap a row for detail). SEARCH: on-screen keyboard to
// type a flight number, then track it. DETAIL: speed/altitude/heading/route for
// a nearby plane or the searched flight; tap anywhere to go back.
class FlightApp : public App {
  enum Mode { LIST, RADAR, DETAIL, SEARCH, HISTORY };
  static const int MAXN = 7;
  int focusIdx = 0;             // button nav: focused LIST item (rows + Radar/Search buttons)
  Flight::Plane planes[MAXN];
  int      count = 0;
  Mode     mode = LIST;
  Mode     detailFrom = LIST;       // where DETAIL was opened from -> where back/tap returns (LIST or RADAR)
  bool     searched = false;
  uint32_t searchStart = 0;        // when the current search began (locate-timeout clock)
  bool     searchAcquired = false; // the searched flight has been seen at least once
  bool     searchTimedOut = false; // gave up locating it -> show "not found"
  static const uint32_t SEARCH_TIMEOUT_MS = 30000;   // ~2 fetch cycles before giving up
  bool     aptMode = false;        // airport-centered radar (searched an airport code)
  uint32_t aptStart = 0;           // when the airport lookup began (timeout)
  String   aptTyped;               // the code being looked up (shown while locating)
  String   selFlight;
  String   typed;
  static const int HIST_MAX = 7;   // recent searches kept (fits above the bottom buttons)
  String   hist[HIST_MAX];
  int      histN = 0;
  bool     histLoaded = false;
  uint32_t shownVer = 0xFFFFFFFF;
  bool     dirty = true;

  // Radar pinch-to-zoom state. The radar reads puck::Touch directly (the global one-tap
  // gTap abstraction can't express a two-finger gesture).
  float    radarRange = FLIGHT_RADIUS_NM;     // shown range (nm); shrinks as you zoom in
  float    pinchPrev  = 0;                     // previous two-finger distance (px), 0 = none
  bool     suppressTap = false;               // swallow taps until all fingers lift (pinch/entry)
  static constexpr float RADAR_MIN_NM = 3.0f;   // closest zoom (pinch fingers apart)
  static constexpr float RADAR_MAX_NM = 150.0f; // farthest zoom (pinch fingers together)
  int      panX = 0, panY = 0;                 // drag-to-pan offset of the scope center (px)
  int      dragLastX = 0, dragLastY = 0;       // last touch pos while dragging
  bool     dragging = false;
  int      appliedFetchNm = (int)FLIGHT_RADIUS_NM;   // fetch radius last pushed to the Flight task (zoom-driven)
  bool     autoFit = false;                    // on a fresh radar open: zoom to spread the planes out, once
  uint32_t lastRadarPull = 0;                  // throttle the while-on-radar fast refresh
  static const uint32_t RADAR_PULL_MS = 5000;  // refresh plane data every 5s while the radar is shown
  puck::Canvas scope{&puck::display()};                 // off-screen radar buffer (PSRAM) -> single blit, flicker-free
  bool     haveScope = false;
  lgfx::LovyanGFX* g = &puck::display();            // current draw target: the sprite during drawRadar, else the panel
  float    lastHdg = -999;                     // last heading we redrew at (for live rotation)
  int      lastCalSec = -1;                    // last calibration countdown we drew
  bool     lastCalState = false;               // were we calibrating last frame

  static const int LIST_ROWH = 25; int LIST_TOP = 30, SEARCH_BTN_Y = 206;  // LIST_TOP/btn voff'd in onEnter
  static const int RADAR_RB_W = 54, RADAR_RB_H = 22;   // reset-zoom button (top-right)
  // Radar fills the panel: on the round 360 panel the scope centers at h/2 with bigger rings; on the
  // CoreS3 it keeps the original (116, 100). Used by both drawRadar and the touch hit-test so they match.
  int radarCy()   { return puck::Display::isRound() ? puck::display().height() / 2 : 116; }
  int radarMaxR() { return puck::Display::isRound() ? puck::display().height() / 2 - 30 : 100; }

  // on-screen keyboard layout
  const char* kbRows[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
  static const int KW = 32, KH = 30; int KB_TOP = 58;  // KB_TOP voff'd per-app in onEnter

  void drawArrow(int cx, int cy, float deg, int len, uint16_t col) {
    float r = deg * PI / 180.0f;
    int hx = cx + (int)(sinf(r) * len), hy = cy - (int)(cosf(r) * len);
    int tx = cx - (int)(sinf(r) * len), ty = cy + (int)(cosf(r) * len);
    g->drawLine(tx, ty, hx, hy, col);
    int bl = len * 2 / 3; if (bl < 3) bl = 3;     // arrowhead scales with shaft (no fat head on short arrows)
    for (int k = -1; k <= 1; k += 2) {
      float ba = r + k * 2.5f;
      g->drawLine(hx, hy, hx + (int)(sinf(ba) * bl), hy - (int)(cosf(ba) * bl), col);
    }
  }

  // Aircraft glyph (top-down silhouette): a fuselage, swept main wings near the nose, and a smaller
  // swept tailplane at the back. The wing layout shows heading, so no arrowhead is needed. `seg` takes
  // plane-local coords (forward +y toward the nose at `deg`, right +x) and rotates them onto the scope.
  void drawPlane(int cx, int cy, float deg, int len, uint16_t col) {
    float r = deg * PI / 180.0f, c = cosf(r), s = sinf(r), L = (float)len;
    auto seg = [&](float ax, float ay, float bx, float by) {
      g->drawLine(cx + (int)lroundf(ax * c + ay * s), cy + (int)lroundf(ax * s - ay * c),
                  cx + (int)lroundf(bx * c + by * s), cy + (int)lroundf(bx * s - by * c), col);
    };
    seg(0,  L,        0,       -L);          // fuselage: nose -> tail
    seg(0,  0.15f*L, -0.95f*L, -0.30f*L);    // left main wing (swept back)
    seg(0,  0.15f*L,  0.95f*L, -0.30f*L);    // right main wing
    seg(0, -0.70f*L, -0.45f*L, -1.0f*L);     // left tailplane
    seg(0, -0.70f*L,  0.45f*L, -1.0f*L);     // right tailplane
  }

  // Vertical climb indicator: up=climbing (green), down=descending, bar=level.
  void drawVArrow(int x, int y, int dir) {
    if (dir > 0)      g->fillTriangle(x, y - 8, x - 6, y + 6, x + 6, y + 6, GREEN);
    else if (dir < 0) g->fillTriangle(x, y + 8, x - 6, y - 6, x + 6, y - 6, ORANGE);
    else              g->fillRect(x - 6, y - 2, 12, 4, DARKGREY);
  }

  uint16_t altColor(int alt) {
    if (alt <= 0)      return DARKGREY;   // ground/unknown
    if (alt < 10000)   return YELLOW;
    if (alt < 25000)   return GREEN;
    return CYAN;
  }

  void drawList() {
    g = haveScope ? (lgfx::LovyanGFX*)&scope : (lgfx::LovyanGFX*)&puck::display();   // off-screen -> no refresh flash
    int w = g->width();
    g->fillScreen(BLACK);
    g->setTextDatum(top_center);
    g->setFont(&fonts::Font0); g->setTextSize(2);   // size 2 throughout -> readable at arm's length
    g->setTextColor(CYAN, BLACK);
    char hdr[28]; snprintf(hdr, sizeof(hdr), "Planes nearby: %d", count);
    g->drawString(hdr, w / 2, 8 + layout::voff());

    if (count == 0) {
      g->setTextColor(DARKGREY, BLACK);
      g->drawString(Flight::updating() ? "scanning..." : "none in range", w / 2, 110 + layout::voff());
    } else {
      for (int i = 0; i < count; i++) {
        int y = LIST_TOP + i * LIST_ROWH;
        const Flight::Plane& p = planes[i];
        g->setTextDatum(middle_left);
        g->setTextColor(WHITE, BLACK);
        g->drawString(p.flight, 8 + layout::inset(), y + 8);
        g->setTextColor(GREEN, BLACK);
        char a[10]; snprintf(a, sizeof(a), "%dft", p.alt);
        g->drawString(a, 110, y + 8);
        g->setTextColor(DARKGREY, BLACK);
        char d[12]; snprintf(d, sizeof(d), "%.0fnm", p.dist);
        g->setTextDatum(middle_right);
        g->drawString(d, w - 30 - layout::inset(), y + 8);
        drawArrow(w - 12 - layout::inset(), y + 8, p.track, 7, CYAN);
      }
    }
    // bottom buttons: Radar | Search
    g->setTextDatum(middle_center);
    g->drawRoundRect(20, SEARCH_BTN_Y, w / 2 - 26, 28, 6, GREEN);
    g->setTextColor(GREEN, BLACK);
    g->drawString("Radar", 20 + (w / 2 - 26) / 2, SEARCH_BTN_Y + 14);
    g->drawRoundRect(w / 2 + 6, SEARCH_BTN_Y, w / 2 - 26, 28, 6, CYAN);
    g->setTextColor(CYAN, BLACK);
    g->drawString("Search", w / 2 + 6 + (w / 2 - 26) / 2, SEARCH_BTN_Y + 14);
    if (haveScope) scope.pushSprite(0, 0);
    g = &puck::display();
  }

  bool radarHeadingUp() {     // compass rotation active (mag present, calibrated, not mid-cal)
    return Compass::available() && Compass::calibrated() && !Compass::calibrating();
  }
  float radarRot() {          // degrees to rotate the scope; 0 = north-up
    if (radarHeadingUp()) { float h = Compass::heading(); if (h >= 0) return h; }
    return 0;
  }

  void drawRadarCal() {       // figure-8 calibration screen (while Compass::calibrating())
    int cx = puck::display().width() / 2, cy = puck::display().height() / 2;
    puck::display().fillScreen(BLACK);
    puck::display().setTextDatum(middle_center);
    puck::display().setFont(&fonts::Font0);
    puck::display().setTextSize(2); puck::display().setTextColor(CYAN, BLACK);
    puck::display().drawString("Calibrating compass", cx, cy - 44);
    puck::display().setTextColor(WHITE, BLACK);
    puck::display().drawString("rotate in a figure-8", cx, cy - 16);
    puck::display().setTextSize(5); puck::display().setTextColor(GREEN, BLACK);
    char s[6]; snprintf(s, sizeof(s), "%d", Compass::calSecondsLeft());
    puck::display().drawString(s, cx, cy + 34);
  }

  // Flight details for the tracked plane: a solid panel BELOW the back chip (top-left) so it stays
  // readable over the range rings, in a larger font than before (callsign + alt + speed at size 2).
  void drawTrackedOverlay(const Flight::Plane& tp) {
    uint16_t pbg = g->color565(12, 16, 22);   // near-black card so text reads over the rings
    int px = 2, py = 31 + layout::voff(), pw = 132, ph = 102, tx = px + 8;
    g->fillRoundRect(px, py, pw, ph, 5, pbg);
    g->drawRoundRect(px, py, pw, ph, 5, DARKGREY);
    g->setTextDatum(top_left);

    g->setFont(&fonts::Font0); g->setTextSize(2);
    g->setTextColor(CYAN, pbg);
    g->drawString(selFlight, tx, py + 6);

    Flight::RouteInfo ri;
    g->setTextSize(1);
    if (Flight::route(selFlight, ri) && ri.ok) {
      char r[26]; snprintf(r, sizeof(r), "%s > %s",
        ri.origIata.length() ? ri.origIata.c_str() : "?",
        ri.destIata.length() ? ri.destIata.c_str() : "?");
      g->setTextColor(0xC618, pbg); g->drawString(r, tx, py + 26);
    } else {
      g->setTextColor(DARKGREY, pbg); g->drawString("route ...", tx, py + 26);
    }

    char l[16];
    g->setTextSize(2);
    snprintf(l, sizeof(l), "%dft", tp.alt);
    g->setTextColor(altColor(tp.alt), pbg); g->drawString(l, tx, py + 40);
    int dir = tp.vrate > 100 ? 1 : (tp.vrate < -100 ? -1 : 0);
    drawVArrow(tx + g->textWidth(l) + 12, py + 48, dir);   // climb/descend

    snprintf(l, sizeof(l), "%dkt", (int)lroundf(tp.gs));
    g->setTextColor(WHITE, pbg); g->drawString(l, tx, py + 62);

    g->setTextSize(1);
    char d[18]; snprintf(d, sizeof(d), "%dnm from you", (int)lroundf(tp.dist));
    g->setTextColor(DARKGREY, pbg); g->drawString(d, tx, py + 84);
  }

  // Airport-centered radar overlay: a small card (code + your distance) below the back chip.
  void drawAirportOverlay(const String& code, const String& name, float youDist) {
    (void)name;                                   // name dropped to keep the card small; the code identifies it
    uint16_t pbg = g->color565(12, 16, 22);
    int px = 2, py = 31 + layout::voff(), pw = 92, ph = 42, tx = px + 7;
    g->fillRoundRect(px, py, pw, ph, 5, pbg);
    g->drawRoundRect(px, py, pw, ph, 5, DARKGREY);
    g->setTextDatum(top_left);
    g->setFont(&fonts::Font0); g->setTextSize(2);
    g->setTextColor(CYAN, pbg);
    g->drawString(code, tx, py + 5);
    g->setTextSize(1);
    char l[16]; snprintf(l, sizeof(l), "you %dnm", (int)lroundf(youDist));
    g->setTextColor(DARKGREY, pbg); g->drawString(l, tx, py + 26);
  }

  // Auto-fit zoom: pick a range that pushes the farthest plotted target near the edge so the (up to
  // FLIGHT_KEEP) planes spread across the scope instead of stacking in the middle. Returns false until
  // there's data to fit. Plane-centered scope fits to the airports around the plane; else to the dots.
  bool computeFit(float& outRange) {
    float mx = 0;
    Flight::Plane tp;
    if (searched && Flight::tracked(tp)) {
      Flight::Airport aps[Flight::MAX_AIRPORTS];
      int n = Flight::planeAirports(aps, Flight::MAX_AIRPORTS);
      for (int i = 0; i < n; i++) if (aps[i].dist > mx) mx = aps[i].dist;
    } else {
      for (int i = 0; i < count; i++) if (planes[i].dist > mx) mx = planes[i].dist;
    }
    if (mx <= 0) return false;
    outRange = constrain(mx * 1.15f, RADAR_MIN_NM, RADAR_MAX_NM);   // 15% margin so the farthest isn't on the rim
    return true;
  }

  // Reset the radar VIEW to a fresh open — centered, default zoom, auto-fit armed, fetch reset, 5s
  // pull restarted. Keeps the current center mode (you / airport / tracked plane). Caller sets mode.
  void radarFreshView() {
    pinchPrev = 0; suppressTap = true; dragging = false;
    panX = panY = 0; radarRange = FLIGHT_RADIUS_NM;
    appliedFetchNm = (int)FLIGHT_RADIUS_NM; Flight::setRange(FLIGHT_RADIUS_NM);
    autoFit = true; lastRadarPull = millis();
  }

  void drawRadar() {
    if (Compass::calibrating()) { drawRadarCal(); return; }   // cal screen draws direct (no sprite)
    g = haveScope ? (lgfx::LovyanGFX*)&scope : (lgfx::LovyanGFX*)&puck::display();  // compose off-screen, blit once
    int w = g->width(), cx = w / 2 + panX, cy = radarCy() + panY, maxR = radarMaxR();  // cx/cy = panned world origin
    int fcx = w / 2, fcy = radarCy();                  // FIXED scope center: rings + crosshair live here
    float scale = maxR / radarRange;
    g->fillScreen(BLACK);

    Flight::Plane tp;
    bool planeCtr = searched && Flight::tracked(tp);  // a searched flight -> center the scope on IT
    String aCode, aName; float aYouD = 0, aYouB = 0;
    bool aptCtr = aptMode && Flight::airportCenter(aCode, aName, aYouD, aYouB);  // a searched airport -> center on IT
    float rot = (planeCtr || aptCtr) ? 0.0f : radarRot();   // centered scopes are north-up; nearby may be heading-up

    // Range rings + crosshair stay FIXED at the screen center; the world (you/plane/airports, plotted
    // at the panned origin cx/cy) is what moves under a one-finger drag — so a drag RE-CENTERS the
    // scope on a new spot rather than just sliding a picture. No pan -> cx/cy == fcx/fcy (you centered).
    for (int k = 1; k <= 3; k++) g->drawCircle(fcx, fcy, maxR * k / 3, 0x2104);  // dim grey

    // North marker rides around the ring as you turn (sits at top when north-up)
    float na = radians(-rot);
    int nx = fcx + (int)(sinf(na) * (maxR + 9)), ny = fcy - (int)(cosf(na) * (maxR + 9));
    g->setTextDatum(middle_center);
    g->setFont(&fonts::Font0); g->setTextSize(1);
    g->setTextColor(CYAN, BLACK);
    g->drawString("N", nx, ny);
    g->setTextColor(DARKGREY, BLACK);
    char rl[10]; snprintf(rl, sizeof(rl), "%dnm", (int)lroundf(radarRange));  // outer ring = range
    g->drawString(rl, fcx + maxR - 14, fcy - 8);

    // reset-zoom button (top-right), mirrors the back chip on the left; lit when zoomed in
    int rbx = w - RADAR_RB_W - 4;
    bool moved = radarRange != (float)FLIGHT_RADIUS_NM || panX || panY;   // zoomed (in or out) and/or panned
    g->drawRoundRect(rbx, 4, RADAR_RB_W, RADAR_RB_H, 5, DARKGREY);
    g->setTextColor(moved ? CYAN : DARKGREY, BLACK);
    g->drawString("RESET", rbx + RADAR_RB_W / 2, 4 + RADAR_RB_H / 2);

    // reference airports (hollow squares + IATA): near the plane when plane-centered, else near you
    Flight::Airport aps[Flight::MAX_AIRPORTS];
    int an = planeCtr ? Flight::planeAirports(aps, Flight::MAX_AIRPORTS)
                      : ((searched || aptMode) ? 0 : Flight::airports(aps, Flight::MAX_AIRPORTS));
    uint16_t apCol = g->color565(120, 170, 255);   // light blue, distinct from plane dots
    g->setFont(&fonts::Font0); g->setTextSize(1);
    for (int i = 0; i < an; i++) {
      if (aps[i].dist > radarRange) continue;              // only within the current zoom range
      float a = radians(aps[i].bearing - rot);
      float rr = aps[i].dist * scale; if (rr > maxR) rr = maxR;
      int x = cx + (int)(sinf(a) * rr), y = cy - (int)(cosf(a) * rr);
      g->drawRect(x - 3, y - 3, 6, 6, apCol);
      g->setTextColor(apCol, BLACK);
      g->setTextDatum(middle_left);
      g->drawString(aps[i].code, x + 5, y);
    }

    if (planeCtr) {
      // you are a reference marker out at your bearing (reciprocal of your bearing TO the plane)
      float a = radians((tp.bearing + 180.0f) - rot);
      float rr = tp.dist * scale; if (rr > maxR) rr = maxR;
      int yx = cx + (int)(sinf(a) * rr), yy = cy - (int)(cosf(a) * rr);
      g->drawCircle(yx, yy, 4, WHITE); g->fillCircle(yx, yy, 1, WHITE);
      g->setTextColor(0xC618, BLACK); g->setTextDatum(middle_left);
      g->drawString("you", yx + 7, yy);
      // the searched plane itself, at center, colored by altitude — aircraft glyph (arrow + wing)
      uint16_t col = altColor(tp.alt);
      drawPlane(cx, cy, tp.track - rot, 11, col);
      drawTrackedOverlay(tp);
    } else if (searched) {                       // searched flight not acquired (locating, or timed out)
      g->setTextDatum(middle_center);
      if (searchTimedOut) {
        g->setFont(&fonts::Font0); g->setTextSize(2);
        g->setTextColor(ORANGE, BLACK);
        g->drawString(selFlight + " not found", w / 2, 100 + layout::voff());
        g->setTextSize(1); g->setTextColor(DARKGREY, BLACK);
        g->drawString("not airborne or out of range", w / 2, 126 + layout::voff());
        g->drawString("tap to go back", w / 2, 146 + layout::voff());
      } else {
        g->setFont(&fonts::Font0); g->setTextSize(1);
        g->setTextColor(ORANGE, BLACK);
        g->drawString("locating " + selFlight + "...", w / 2, 116 + layout::voff());
      }
    } else if (aptMode && !aptCtr) {             // airport lookup in progress / not found
      g->setTextDatum(middle_center);
      if (Flight::airportFailed() || millis() - aptStart > SEARCH_TIMEOUT_MS) {
        g->setFont(&fonts::Font0); g->setTextSize(2);
        g->setTextColor(ORANGE, BLACK);
        g->drawString(aptTyped + " not found", w / 2, 100 + layout::voff());
        g->setTextSize(1); g->setTextColor(DARKGREY, BLACK);
        g->drawString("unknown airport code", w / 2, 126 + layout::voff());
        g->drawString("< back", w / 2, 146 + layout::voff());
      } else {
        g->setFont(&fonts::Font0); g->setTextSize(1);
        g->setTextColor(ORANGE, BLACK);
        g->drawString("locating airport " + aptTyped + "...", w / 2, 116 + layout::voff());
      }
    } else {
      if (aptCtr) {                              // the airport at center + a "you" reference marker
        g->drawRect(cx - 5, cy - 5, 10, 10, CYAN);
        g->drawRect(cx - 3, cy - 3, 6, 6, CYAN);
        float a = radians(aYouB - rot); float rr = aYouD * scale; if (rr > maxR) rr = maxR;
        int yx = cx + (int)(sinf(a) * rr), yy = cy - (int)(cosf(a) * rr);
        g->drawCircle(yx, yy, 4, WHITE); g->fillCircle(yx, yy, 1, WHITE);
        g->setTextColor(0xC618, BLACK); g->setTextDatum(middle_left);
        g->setFont(&fonts::Font0); g->setTextSize(1);
        g->drawString("you", yx + 7, yy);
      } else {
        g->fillCircle(cx, cy, 3, WHITE);   // you, at center
      }
      // plane dots (rotated by heading when heading-up), colored by altitude, heading tick + id
      for (int i = 0; i < count; i++) {
        const Flight::Plane& p = planes[i];
        float rr = p.dist * scale; if (rr > maxR) rr = maxR;
        float a = radians(p.bearing - rot);
        int x = cx + (int)(sinf(a) * rr), y = cy - (int)(cosf(a) * rr);
        uint16_t col = altColor(p.alt);
        drawPlane(x, y, p.track - rot, 8, col);         // aircraft glyph (arrow + wing), rotates with the scope
        g->setTextColor(col, BLACK);
        if (x > cx) { g->setTextDatum(middle_right); g->drawString(p.flight, x - 7, y); }
        else        { g->setTextDatum(middle_left);  g->drawString(p.flight, x + 7, y); }
      }
    }

    if (aptCtr) drawAirportOverlay(aCode, aName, aYouD);   // code/name/your-distance card (over the dots)

    // fixed crosshair cursor (does NOT pan): the stationary crosswire you drag the world under
    {
      uint16_t cur = g->color565(90, 110, 130);    // brighter than the panning range rings
      g->drawLine(fcx - maxR, fcy, fcx + maxR, fcy, cur);
      g->drawLine(fcx, fcy - maxR, fcx, fcy + maxR, cur);
      g->drawCircle(fcx, fcy, 4, WHITE);           // exact cursor point
    }

    // bottom-left: orientation / hint
    g->setTextDatum(middle_left);
    g->setFont(&fonts::Font0); g->setTextSize(1);
    if (planeCtr || aptCtr) {
      g->setTextColor(DARKGREY, BLACK);
      g->drawString("north up   < back", 6, g->height() - 11);
    } else if (Compass::available() && Compass::calibrated()) {
      g->setTextColor(CYAN, BLACK);
      char hb[12]; snprintf(hb, sizeof(hb), "HDG %03d", (int)lroundf(rot));
      g->drawString(hb, 6, g->height() - 11);
    } else if (Compass::available()) {
      g->setTextColor(ORANGE, BLACK);
      g->drawString("compass: tap CAL", 6, g->height() - 11);
    } else {
      g->setTextColor(DARKGREY, BLACK);
      g->drawString("north up", 6, g->height() - 11);
    }
    // bottom-right: CAL button (nearby heading-up scope only; a magnetometer must be present)
    if (!planeCtr && !aptCtr && Compass::available()) {   // no CAL on the north-up centered scopes
      int cbx = w - RADAR_RB_W - 4, cby = g->height() - 22;
      g->drawRoundRect(cbx, cby, RADAR_RB_W, 20, 5, DARKGREY);
      g->setTextDatum(middle_center);
      g->setTextColor(GREEN, BLACK);
      g->drawString("CAL", cbx + RADAR_RB_W / 2, cby + 10);
    }

    if (haveScope) scope.pushSprite(0, 0);   // single blit -> flicker-free even on a data/heading refresh
    g = &puck::display();                          // restore the default target for the other (direct) screens
  }

  // Radar input: two fingers = pinch zoom, single tap = select plane / exit / reset / cal.
  // Reads the touch panel directly because a pinch needs both points, which gTap omits.
  void handleRadar() {
    int w = puck::display().width(), cx = w / 2, cy = radarCy(), maxR = radarMaxR(), hgt = puck::display().height();
    if (Compass::calibrating()) { gTap.pressed = false; return; }   // ignore taps mid-cal
    int tc = puck::Touch::count();

    if (tc >= 2) {                                   // pinch: scale range by finger spread
      auto t0 = puck::Touch::get(0);
      auto t1 = puck::Touch::get(1);
      float dx = t0.x - t1.x, dy = t0.y - t1.y;
      float d  = sqrtf(dx * dx + dy * dy);
      if (pinchPrev > 1.0f && d > 1.0f) {
        radarRange /= (d / pinchPrev);               // fingers apart -> zoom in -> smaller range
        radarRange = constrain(radarRange, RADAR_MIN_NM, RADAR_MAX_NM);
        dirty = true;
      }
      pinchPrev = d;
      suppressTap = true;
      Dim::wake();
      gTap.pressed = false;
      return;
    }
    pinchPrev = 0;

    if (tc == 1) {                                   // one finger -> drag to re-center (world moves under fixed rings)
      auto t = puck::Touch::get(0);
      if (!dragging) { dragging = true; dragLastX = t.x; dragLastY = t.y; }
      else if (t.x != dragLastX || t.y != dragLastY) {
        panX = constrain(panX + (t.x - dragLastX), -maxR, maxR);
        panY = constrain(panY + (t.y - dragLastY), -maxR, maxR);
        dragLastX = t.x; dragLastY = t.y;
        Dim::wake(); dirty = true;
      }
    }

    auto td = puck::Touch::get(0);
    if (td.clicked && !suppressTap) {
      int tx = td.x, ty = td.y;
      if (tx >= w - RADAR_RB_W - 4 && ty <= 4 + RADAR_RB_H) {                       // RESET pan + re-fit zoom
        panX = 0; panY = 0; autoFit = true; dirty = true;
      } else if (Compass::available() && tx >= w - RADAR_RB_W - 4 && ty >= hgt - 22) {  // CAL
        Compass::startCal(); lastCalState = true; dirty = true;
      } else if (!searched) {                        // nearby scope: tap a plane -> its detail (a miss does nothing)
        float scale = maxR / radarRange, rot = radarRot();
        int pcx = cx + panX, pcy = cy + panY;        // hit-test against the panned scope center
        int best = -1, bestd = 28 * 28;              // generous tap target (dots are small)
        for (int i = 0; i < count; i++) {
          float rr = planes[i].dist * scale; if (rr > maxR) rr = maxR;
          float a = radians(planes[i].bearing - rot);
          int x = pcx + (int)(sinf(a) * rr), y = pcy - (int)(cosf(a) * rr);
          int ddx = tx - x, ddy = ty - y, dd = ddx * ddx + ddy * ddy;
          if (dd < bestd) { bestd = dd; best = i; }
        }
        if (best >= 0) { mode = DETAIL; detailFrom = RADAR; selFlight = planes[best].flight; dirty = true; }
      }
      // plane-centered / airport-centered scopes ignore taps; use the back (<) chip to return to the list
      gTap.pressed = false;
    }
    if (tc == 0) {                                  // gesture fully ended
      suppressTap = false; dragging = false;
      int want = (int)ceilf(radarRange);            // zoom changed -> widen/narrow the plane search to match
      if (want != appliedFetchNm) { appliedFetchNm = want; Flight::setRange(want); Flight::refreshNow(); }
    }
  }

  void drawRoute() {                 // route line + cities, used by detail (draws to the current target g)
    int w = g->width(), cx = w / 2;
    Flight::RouteInfo ri;
    bool have = Flight::route(selFlight, ri);
    g->setTextDatum(top_center);
    if (!have) {
      g->setFont(&fonts::Font0); g->setTextSize(1);
      g->setTextColor(DARKGREY, BLACK);
      g->drawString("route loading...", cx, 50 + layout::voff());
      return;
    }
    if (ri.ok) {                        // OpenSky: the aircraft's most recent tracked flight
      char r[24];
      snprintf(r, sizeof(r), "%s  >  %s",
               ri.origIata.length() ? ri.origIata.c_str() : "?",
               ri.destIata.length() ? ri.destIata.c_str() : "?");
      g->setFont(&fonts::Font0); g->setTextSize(2);
      g->setTextColor(CYAN, BLACK);
      g->drawString(r, cx, 46 + layout::voff());
    } else {
      g->setFont(&fonts::Font0); g->setTextSize(1);
      g->setTextColor(DARKGREY, BLACK);
      g->drawString("route unknown", cx, 54 + layout::voff());
    }
  }

  void drawDetail() {
    g = haveScope ? (lgfx::LovyanGFX*)&scope : (lgfx::LovyanGFX*)&puck::display();   // off-screen -> no refresh flash
    int w = g->width(), cx = w / 2;
    Flight::Plane tp;
    const Flight::Plane* p = nullptr;
    if (searched) { if (Flight::tracked(tp)) p = &tp; }
    else { for (int i = 0; i < count; i++) if (planes[i].flight == selFlight) { p = &planes[i]; break; } }

    g->fillScreen(BLACK);
    g->setTextDatum(top_center);
    g->setFont(&fonts::Font0);
    g->setTextSize(3); g->setTextColor(WHITE, BLACK);
    g->drawString(selFlight, cx, 8 + layout::voff());

    if (!p) {
      g->setTextSize(2); g->setTextColor(ORANGE, BLACK);
      g->drawString(searched && Flight::updating() ? "acquiring..." : "no signal", cx, 96 + layout::voff());
      if (!(searched && Flight::updating())) {     // a tracked plane drops off ADS-B on landing / out of range
        g->setTextSize(1); g->setTextColor(DARKGREY, BLACK);
        g->drawString("landed or out of range", cx, 122 + layout::voff());
      }
      g->setTextSize(1); g->setTextColor(DARKGREY, BLACK);
      g->drawString("tap to go back", cx, g->height() - 16);
      if (haveScope) scope.pushSprite(0, 0);
      g = &puck::display();
      return;
    }

    drawRoute();

    drawArrow(cx, 108 + layout::voff(), p->track, 22, CYAN);
    g->setTextDatum(top_center);
    g->setFont(&fonts::Font0); g->setTextSize(1);
    g->setTextColor(CYAN, BLACK);
    char hd[16]; snprintf(hd, sizeof(hd), "hdg %03d  %s", (int)lroundf(p->track),
                          p->type.length() ? p->type.c_str() : "");
    g->drawString(hd, cx, 134 + layout::voff());

    g->setTextDatum(middle_center);
    g->setTextSize(2);
    char line[32];
    g->setTextColor(GREEN, BLACK);
    snprintf(line, sizeof(line), "%d ft", p->alt);
    int tw = g->textWidth(line);
    g->drawString(line, cx, 160 + layout::voff());
    int dir = p->vrate > 100 ? 1 : (p->vrate < -100 ? -1 : 0);
    drawVArrow(cx + tw / 2 + 16, 160 + layout::voff(), dir);     // climb/descend indicator
    g->setTextColor(WHITE, BLACK);
    snprintf(line, sizeof(line), "%.0f kt", p->gs);
    g->drawString(line, cx, 184 + layout::voff());

    g->setTextSize(1); g->setTextColor(DARKGREY, BLACK);
    snprintf(line, sizeof(line), "pos %.2f, %.2f", p->lat, p->lon);
    g->drawString(line, cx, 208 + layout::voff());
    const char* climb = p->vrate > 100 ? "climbing" : (p->vrate < -100 ? "descending" : "level");
    snprintf(line, sizeof(line), "%.0f nm @ %03d  -  %s", p->dist, (int)lroundf(p->bearing), climb);
    g->drawString(line, cx, g->height() - 12);
    if (haveScope) scope.pushSprite(0, 0);
    g = &puck::display();
  }

  // --- search history (recent flight nos / airport codes, persisted in NVS, most-recent first) ---
  void loadHistory() {
    histN = 0;
    String s = Settings::searchHistory();
    int start = 0;
    while (start < (int)s.length() && histN < HIST_MAX) {
      int c = s.indexOf(',', start); if (c < 0) c = s.length();
      String tok = s.substring(start, c);
      if (tok.length()) hist[histN++] = tok;
      start = c + 1;
    }
    histLoaded = true;
  }
  void saveHistoryNvs() {
    String s;
    for (int i = 0; i < histN; i++) { if (i) s += ','; s += hist[i]; }
    Settings::saveSearchHistory(s);
  }
  void pushHistory(const String& code) {           // dedup + move to front + cap + persist
    if (!code.length()) return;
    if (!histLoaded) loadHistory();
    int wj = 0;                                     // drop any existing copy
    for (int i = 0; i < histN; i++) if (hist[i] != code) hist[wj++] = hist[i];
    histN = wj;
    if (histN > HIST_MAX - 1) histN = HIST_MAX - 1; // make room for the new front entry
    for (int i = histN; i > 0; i--) hist[i] = hist[i - 1];
    hist[0] = code; histN++;
    saveHistoryNvs();
  }

  // Run a search (from the keyboard GO or a tapped history row): flight number -> track that
  // plane (plane-centered radar); 3/4-letter airport code -> center the radar on the airport.
  void runSearch(const String& raw) {
    if (!raw.length()) return;
    String s = raw; s.toUpperCase();
    pushHistory(s);                                 // remember it (uppercased)
    bool isApt = (s.length() == 3 || s.length() == 4);   // 3-letter IATA / 4-letter ICAO, all letters
    for (int i = 0; isApt && i < (int)s.length(); i++) { char ch = s[i]; if (ch < 'A' || ch > 'Z') isApt = false; }
    mode = RADAR; radarFreshView();                 // centered + auto-fit; search state set below
    if (isApt) {                                    // airport code -> center the radar on the airport
      aptMode = true; searched = false; aptTyped = s; aptStart = millis();
      Flight::track("");                            // drop any prior flight track
      Flight::searchAirport(s);
    } else {                                        // flight number -> track that plane (plane-centered)
      aptMode = false; Flight::clearCenter();       // drop any prior airport center
      String cs = Flight::toCallsign(raw);          // SQ31 -> SIA31
      selFlight = cs; searched = true;
      Flight::track(cs);
      searchStart = millis(); searchAcquired = false; searchTimedOut = false;
    }
    Flight::refreshNow();                           // acquire ASAP (don't wait ~15s)
    dirty = true;
  }

  void drawHistory() {                              // event-driven screen -> draw direct (no off-screen needed)
    int w = puck::display().width();
    puck::display().fillScreen(BLACK);
    puck::display().setTextDatum(top_center);
    puck::display().setFont(&fonts::Font0); puck::display().setTextSize(1);
    puck::display().setTextColor(CYAN, BLACK);
    puck::display().drawString("Recent searches", w / 2, 10 + layout::voff());
    if (histN == 0) {
      puck::display().setTextColor(DARKGREY, BLACK);
      puck::display().drawString("no recent searches", w / 2, 110 + layout::voff());
    } else {
      puck::display().setTextSize(2);
      puck::display().setTextDatum(middle_left);
      for (int i = 0; i < histN; i++) {
        int y = LIST_TOP + i * LIST_ROWH;
        puck::display().setTextColor(WHITE, BLACK);
        puck::display().drawString(hist[i], 16, y + 9);
      }
      puck::display().setTextSize(1);
    }
    // bottom row: Keys (back to keyboard) | Clear
    puck::display().setTextDatum(middle_center);
    puck::display().setTextSize(2);
    puck::display().drawRoundRect(20, SEARCH_BTN_Y, w / 2 - 26, 28, 6, CYAN);
    puck::display().setTextColor(CYAN, BLACK);
    puck::display().drawString("Keys", 20 + (w / 2 - 26) / 2, SEARCH_BTN_Y + 14);
    uint16_t clr = histN ? ORANGE : DARKGREY;
    puck::display().drawRoundRect(w / 2 + 6, SEARCH_BTN_Y, w / 2 - 26, 28, 6, clr);
    puck::display().setTextColor(clr, BLACK);
    puck::display().drawString("Clear", w / 2 + 6 + (w / 2 - 26) / 2, SEARCH_BTN_Y + 14);
  }

  void handleHistoryTap(int x, int y) {
    int w = puck::display().width();
    if (y >= SEARCH_BTN_Y) {
      if (x < w / 2) { mode = SEARCH; }            // Keys -> back to the keyboard
      else if (histN) { histN = 0; saveHistoryNvs(); }   // Clear
      dirty = true;
      return;
    }
    int row = (y - LIST_TOP) / LIST_ROWH;
    if (y >= LIST_TOP && row >= 0 && row < histN) runSearch(hist[row]);   // re-run a past search
  }

  void drawSearch() {
    int w = puck::display().width();
    puck::display().fillScreen(BLACK);
    // hint: a flight number tracks a plane; an airport code (JFK / KJFK) centers the radar there
    puck::display().setTextDatum(top_center);
    puck::display().setFont(&fonts::Font0); puck::display().setTextSize(1);
    puck::display().setTextColor(DARKGREY, BLACK);
    puck::display().drawString("flight no. or airport code", w / 2, 4 + layout::voff());
    // typed text
    puck::display().setTextDatum(middle_center);
    puck::display().setFont(&fonts::Font0); puck::display().setTextSize(3);
    puck::display().setTextColor(WHITE, BLACK);
    puck::display().drawString(typed.length() ? typed : String("_"), w / 2, 28);
    // character keys
    puck::display().setTextSize(2);
    for (int r = 0; r < 4; r++) {
      const char* row = kbRows[r];
      for (int i = 0; row[i]; i++) {
        int x = i * KW, y = KB_TOP + r * KH;
        puck::display().drawRoundRect(x + 1, y + 1, KW - 2, KH - 2, 3, DARKGREY);
        char s[2] = { row[i], 0 };
        puck::display().setTextColor(CYAN, BLACK);
        puck::display().drawString(s, x + KW / 2, y + KH / 2);
      }
    }
    // bottom row: DEL | HIST | GO
    int by = KB_TOP + 4 * KH;
    const char* labels[3] = { "DEL", "HIST", "GO" };
    uint16_t cols[3] = { ORANGE, CYAN, GREEN };
    for (int i = 0; i < 3; i++) {
      int x = i * (w / 3);
      puck::display().drawRoundRect(x + 2, by + 1, w / 3 - 4, 34, 4, cols[i]);
      puck::display().setTextColor(cols[i], BLACK);
      puck::display().drawString(labels[i], x + (w / 3) / 2, by + 18);
    }
  }

  // returns: 0..n-1 char appended handled internally; here we mutate state.
  void handleSearchTap(int x, int y) {
    int w = puck::display().width();
    int by = KB_TOP + 4 * KH;
    if (y >= by) {                              // bottom row
      int b = x / (w / 3);
      if (b == 0) { if (typed.length()) typed.remove(typed.length() - 1); dirty = true; }
      else if (b == 1) { loadHistory(); mode = HISTORY; dirty = true; }   // HIST -> previous searches
      else if (b == 2 && typed.length()) runSearch(typed);                // GO (records history, sets dirty)
      return;
    }
    int r = (y - KB_TOP) / KH;
    if (r >= 0 && r < 4) {
      const char* row = kbRows[r];
      int i = x / KW;
      if (i >= 0 && i < (int)strlen(row)) {
        if (typed.length() < 8) typed += row[i];
        dirty = true;
      }
    }
  }

public:
  FlightApp() : App("Flight") {}
  bool needsNet() const override { return true; }
  bool onBack() override {                  // back chip: step RADAR/DETAIL/SEARCH -> LIST before leaving the app
    if (mode == LIST) return false;
    if (mode == HISTORY) { mode = SEARCH; dirty = true; return true; }   // history -> keyboard
    if (mode == DETAIL && detailFrom == RADAR) {     // picked a plane on the radar -> reopen that radar fresh
      mode = RADAR; radarFreshView(); dirty = true; return true;
    }
    if (searched) Flight::track("");
    if (aptMode)  Flight::clearCenter();
    mode = LIST; searched = false; aptMode = false; searchAcquired = false; searchTimedOut = false;
    Flight::setRange(FLIGHT_RADIUS_NM);
    dirty = true;
    return true;
  }

  void onEnter() override {
    mode = LIST; searched = false; aptMode = false; searchAcquired = false; searchTimedOut = false; selFlight = ""; typed = "";
    LIST_TOP = 30 + layout::voff(); SEARCH_BTN_Y = 206 + layout::voff(); KB_TOP = 58 + layout::voff();  // center on round panel
    Flight::clearCenter();                  // start at your-location radar (drop any prior airport center)
    shownVer = 0xFFFFFFFF; dirty = true;
    radarRange = FLIGHT_RADIUS_NM; pinchPrev = 0; suppressTap = false; panX = panY = 0; dragging = false;
    appliedFetchNm = (int)FLIGHT_RADIUS_NM; Flight::setRange(FLIGHT_RADIUS_NM);   // back to the default search radius
    lastHdg = -999; lastCalSec = -1; lastCalState = false;
    if (!haveScope) {                            // allocate the off-screen radar buffer once (PSRAM = ample)
      scope.setColorDepth(16); scope.setPsram(true);
      haveScope = (scope.createSprite(puck::display().width(), puck::display().height()) != nullptr);
    }
    Flight::setActive(true);                // resume fetching while the app is open
    Flight::refreshNow();
  }
  void onExit() override { Flight::track(""); Flight::setRange(FLIGHT_RADIUS_NM); Flight::setActive(false); }   // stop tracking + idle fetches

  void loop() override {
    uint32_t ver = Flight::version();
    if (ver != shownVer) {
      bool ui = (mode == SEARCH || mode == HISTORY);                // static keyboard/history screens
      if (!ui) count = Flight::snapshot(planes, MAXN);   // refresh nearby in LIST/RADAR/DETAIL (so detail auto-updates)
      shownVer = ver;
      if (!ui) dirty = true;                       // live refresh, but don't fight the keyboard/history
    }

    if (mode == RADAR) {
      if (autoFit) {                               // fresh open: zoom to spread the planes out, once data arrives
        float r; if (computeFit(r)) { radarRange = r; autoFit = false; dirty = true; }
      }
      if (millis() - lastRadarPull >= RADAR_PULL_MS) {   // planes move fast on the radar -> pull every 5s
        lastRadarPull = millis(); Flight::refreshNow();
      }
      bool cal = Compass::calibrating();           // keep the rotating scope / cal countdown live
      if (cal != lastCalState) { lastCalState = cal; dirty = true; }
      if (cal) {
        int sl = Compass::calSecondsLeft();
        if (sl != lastCalSec) { lastCalSec = sl; dirty = true; }
      } else if (radarHeadingUp()) {
        float h = Compass::heading();
        float d = h - lastHdg; while (d > 180) d -= 360; while (d < -180) d += 360;
        if (h >= 0 && fabsf(d) > 2.0f) { lastHdg = h; dirty = true; }   // redraw on real turn
      }
      // Locate timeout: a searched flight that never appears -> stop "locating", show "not found".
      if (searched && !searchAcquired && !searchTimedOut) {
        Flight::Plane tp;
        if (Flight::tracked(tp)) searchAcquired = true;
        else if (millis() - searchStart > SEARCH_TIMEOUT_MS) { searchTimedOut = true; Flight::track(""); dirty = true; }
      }
      handleRadar();                               // pinch-zoom + taps (owns the touch panel)
    } else if (gTap.pressed) {
      int w = puck::display().width();
      if (mode == LIST) {
        if (gTap.y >= SEARCH_BTN_Y) {
          if (gTap.x < w / 2) { mode = RADAR; radarFreshView(); }   // open RADAR fresh: centered + auto-fit
          else { mode = SEARCH; typed = ""; }
          dirty = true;
        } else {
          int row = (gTap.y - LIST_TOP) / LIST_ROWH;
          if (gTap.y >= LIST_TOP && row >= 0 && row < count) {
            mode = DETAIL; detailFrom = LIST; searched = false; selFlight = planes[row].flight; dirty = true;
          }
        }
      } else if (mode == SEARCH) {
        handleSearchTap(gTap.x, gTap.y);
      } else if (mode == HISTORY) {
        handleHistoryTap(gTap.x, gTap.y);
      } else {                                     // DETAIL: tap returns to where you came from
        if (detailFrom == RADAR) { mode = RADAR; radarFreshView(); }   // reopen the radar fresh (centered + auto-fit)
        else {
          if (searched) Flight::track("");
          if (aptMode)  Flight::clearCenter();
          mode = LIST; searched = false; aptMode = false;
        }
        dirty = true;
      }
      gTap.pressed = false;
    }

    if (dirty) {
      if (mode == LIST)         drawList();
      else if (mode == RADAR)   drawRadar();
      else if (mode == SEARCH)  drawSearch();
      else if (mode == HISTORY) drawHistory();
      else                      drawDetail();
      dirty = false;
    }
  }

  // ---- button nav: LIST rows + the Radar/Search buttons (RADAR/SEARCH/DETAIL stay touch/gesture) ----
  bool focusItem(int i, int& x, int& y, int& w, int& h) {
    if (mode != LIST) return false;
    int W = puck::display().width();
    if (i >= 0 && i < count)  { x = 0;         y = LIST_TOP + i * LIST_ROWH; w = W;         h = LIST_ROWH; return true; }
    if (i == count)           { x = 0;         y = SEARCH_BTN_Y;             w = W / 2 - 2; h = 30; return true; }   // Radar
    if (i == count + 1)       { x = W / 2 + 2; y = SEARCH_BTN_Y;             w = W / 2 - 2; h = 30; return true; }   // Search
    return false;
  }
  int  focusCount() override { int x, y, w, h, n = 0; while (focusItem(n, x, y, w, h)) n++; return n; }
  void focusMove(int d) override { int n = focusCount(); if (!n) return; focusIdx = (focusIdx + d + n) % n; dirty = true; }
  void focusSelect() override { int x, y, w, h; if (focusItem(focusIdx, x, y, w, h)) { gTap.pressed = true; gTap.x = x + w / 2; gTap.y = y + h / 2; } }
  void drawFocus() override { int x, y, w, h; if (focusItem(focusIdx, x, y, w, h)) puck::display().drawRoundRect(x, y, w, h, 3, WHITE); }
};

// ---------------- Settings ----------------
// Brings up the captive portal (Wi-Fi, timezone, ZIP) and shows on-screen
// instructions. The device restarts itself once saved (inside Provision::loop()).
class SetupApp : public App {
  uint32_t shownOta = 0xFFFFFFFF;
  bool     showVers = false;                         // the on-device version picker is open
  int      focusIdx = 0;                             // button nav: focused button / picker row
  uint32_t shownVerVer = 0xFFFFFFFF;                 // Ota::version() snapshot for the picker's lazy redraw
  static const int OTA_Y = 204, OTA_H = 30;          // bottom button row (Check | Versions)
  static const int VTOP = 56, VROWH = 26;            // version-picker rows

  static String otaStatusText() {
    if (!Ota::configured()) return "updates off";
    switch (Ota::phase()) {
      case Ota::CHECKING:  return "checking...";
      case Ota::AVAILABLE: return "update available!";
      case Ota::FAILED:    return "update failed";
      default: { String e = Ota::lastError(); return e.length() ? e : String("up to date"); }
    }
  }

  // The Check-Updates button is live only when OTA is on and no check is
  // already in flight (a tap during CHECKING is ignored + the button greys out).
  static bool otaBtnReady() { return Ota::configured() && Ota::phase() != Ota::CHECKING; }

  void draw() {
    int w = puck::display().width(), cx = w / 2;
    puck::display().fillScreen(BLACK);
    puck::display().setTextDatum(top_center);
    puck::display().setFont(&fonts::Font0);

    puck::display().setTextSize(2);
    puck::display().setTextColor(WHITE, BLACK);
    puck::display().drawString("Settings", cx, 16);

    puck::display().setTextColor(CYAN, BLACK);
    puck::display().drawString("1. Join hotspot:", cx, 50);
    puck::display().setTextColor(GREEN, BLACK);
    puck::display().drawString(Provision::apName(), cx, 72);
    puck::display().setTextColor(CYAN, BLACK);
    puck::display().drawString("2. Browse to", cx, 102);
    puck::display().setTextColor(GREEN, BLACK);
    puck::display().drawString("192.168.4.1", cx, 124);

    char fw[28]; snprintf(fw, sizeof(fw), "Firmware v%d", FW_VERSION);
    puck::display().setTextColor(WHITE, BLACK);
    puck::display().drawString(fw, cx, 156);
    puck::display().setTextSize(1);
    puck::display().setTextColor(puck::display().color565(120, 140, 160), BLACK);   // exact build (tells RCs apart)
    puck::display().drawString(String("build ") + FW_BUILD, cx, 176);
    Ota::Phase ph = Ota::phase();
    uint16_t sc = (ph == Ota::AVAILABLE) ? GREEN
                : (ph == Ota::FAILED || (ph == Ota::IDLE && Ota::lastError().length())) ? ORANGE
                : DARKGREY;
    puck::display().setTextColor(sc, BLACK);
    puck::display().drawString(otaStatusText(), cx, 190);

    // bottom row: Check (left) | Versions (right)
    puck::display().setTextDatum(middle_center);
    puck::display().setTextSize(2);
    int bw = w / 2 - 26;
    bool checking = Ota::configured() && Ota::phase() == Ota::CHECKING;
    uint16_t bc = otaBtnReady() ? CYAN : DARKGREY;
    puck::display().drawRoundRect(20, OTA_Y, bw, OTA_H, 6, bc);
    puck::display().setTextColor(bc, BLACK);
    puck::display().drawString(checking ? "Checking" : "Check", 20 + bw / 2, OTA_Y + OTA_H / 2);
    uint16_t vc = Ota::configured() ? GREEN : DARKGREY;
    puck::display().drawRoundRect(w / 2 + 6, OTA_Y, bw, OTA_H, 6, vc);
    puck::display().setTextColor(vc, BLACK);
    puck::display().drawString("Versions", w / 2 + 6 + bw / 2, OTA_Y + OTA_H / 2);
  }

  // On-device version picker: install (or downgrade to) any published version.
  void drawVersions() {
    int w = puck::display().width(), cx = w / 2;
    puck::display().fillScreen(BLACK);
    puck::display().setTextDatum(top_center); puck::display().setFont(&fonts::Font0);
    puck::display().setTextSize(2); puck::display().setTextColor(WHITE, BLACK);
    puck::display().drawString(Ota::isBeta() ? "Firmware (beta)" : "Firmware", cx, 12);
    puck::display().setTextSize(1); puck::display().setTextColor(DARKGREY, BLACK);
    char cur[36]; snprintf(cur, sizeof(cur), "current v%d  -  tap to install", FW_VERSION);
    puck::display().drawString(cur, cx, 36);

    if (!Ota::versionsReady()) {
      puck::display().setTextSize(2); puck::display().setTextColor(DARKGREY, BLACK);
      puck::display().drawString("loading...", cx, 120);
      return;
    }
    int vers[24]; int n = Ota::versions(vers, 24);
    if (n == 0) {
      puck::display().setTextSize(2); puck::display().setTextColor(DARKGREY, BLACK);
      puck::display().drawString("none found", cx, 120);
      return;
    }
    int maxRows = (236 - VTOP) / VROWH;
    puck::display().setTextSize(2);
    for (int i = 0; i < n && i < maxRows; i++) {
      int y = VTOP + i * VROWH; bool isCur = (vers[i] == FW_VERSION);
      char lbl[10]; snprintf(lbl, sizeof(lbl), "v%d", vers[i]);
      puck::display().setTextDatum(middle_left);  puck::display().setTextColor(isCur ? GREEN : WHITE, BLACK);
      puck::display().drawString(lbl, 24, y);
      puck::display().setTextDatum(middle_right); puck::display().setTextColor(isCur ? GREEN : CYAN, BLACK);
      puck::display().drawString(isCur ? "current" : "install", w - 24, y);
    }
  }
public:
  SetupApp() : App("Settings") {}
  void onEnter() override { Provision::start(); showVers = false; shownOta = Ota::version(); draw(); }
  bool onBack() override { if (showVers) { showVers = false; draw(); return true; } return false; }
  void loop() override {
    Provision::loop();                                 // self-restarts after a save
    int w = puck::display().width();

    if (showVers) {                                    // version picker
      if (gTap.pressed) {
        int vers[24]; int n = Ota::versions(vers, 24), maxRows = (236 - VTOP) / VROWH;
        int row = (gTap.y - VTOP) / VROWH;
        if (Ota::versionsReady() && gTap.y >= VTOP && row >= 0 && row < n && row < maxRows
            && vers[row] != FW_VERSION) {
          Ota::pushVersion(vers[row], false);          // -> confirm overlay (main.cpp) pops over us
          showVers = false;
        }
        gTap.pressed = false;
      }
      if (Ota::version() != shownVerVer) { shownVerVer = Ota::version(); drawVersions(); }   // loading -> list
      return;
    }

    if (gTap.pressed && gTap.y >= OTA_Y && gTap.y <= OTA_Y + OTA_H) {
      int bw = w / 2 - 26;
      if (gTap.x >= 20 && gTap.x <= 20 + bw) {                       // Check
        if (otaBtnReady()) Ota::checkNow(true);                      // manual: ignores the Later snooze
      } else if (gTap.x >= w / 2 + 6 && gTap.x <= w / 2 + 6 + bw && Ota::configured()) {  // Versions
        showVers = true; shownVerVer = 0xFFFFFFFF; Ota::requestVersions(); drawVersions();
      }
      gTap.pressed = false;
    }
    if (Ota::version() != shownOta) { shownOta = Ota::version(); draw(); }   // refresh status lazily
  }
  void onExit() override  { Provision::stop(); }

  // ---- button nav: home Check/Versions buttons, or the version-picker rows ----
  bool focusItem(int i, int& x, int& y, int& w, int& h) {
    int W = puck::display().width();
    if (showVers) {
      if (!Ota::versionsReady()) return false;
      int vers[24]; int n = Ota::versions(vers, 24), maxRows = (236 - VTOP) / VROWH;
      if (n > maxRows) n = maxRows;
      if (i < 0 || i >= n) return false;
      x = 8; y = VTOP + i * VROWH - VROWH / 2; w = W - 16; h = VROWH; return true;
    }
    int bw = W / 2 - 26;
    if (i == 0) { x = 20;        y = OTA_Y; w = bw; h = OTA_H; return true; }   // Check
    if (i == 1) { x = W / 2 + 6; y = OTA_Y; w = bw; h = OTA_H; return true; }   // Versions
    return false;
  }
  int  focusCount() override { int x, y, w, h, n = 0; while (focusItem(n, x, y, w, h)) n++; return n; }
  void focusMove(int d) override { int n = focusCount(); if (!n) return; focusIdx = (focusIdx + d + n) % n; if (showVers) drawVersions(); else draw(); }
  void focusSelect() override { int x, y, w, h; if (focusItem(focusIdx, x, y, w, h)) { gTap.pressed = true; gTap.x = x + w / 2; gTap.y = y + h / 2; } }
  void drawFocus() override { int x, y, w, h; if (focusItem(focusIdx, x, y, w, h)) puck::display().drawRoundRect(x, y, w, h, 4, WHITE); }
};

// ---------------- Emoji Ping ----------------
// ---------------- Emoji Ping ----------------
// Sends an emote to the remembered recipient (a specific friend, or All confirmed
// friends) in one tap. Incoming pings come from the always-on Friends service, so a
// friend's emote beeps from any screen; this app shows it as a banner when open.
// Vector "emoji" graphics — shared by the Emoji picker and the incoming-ping overlay in
// main.cpp. The MQTT payload stays the ASCII code ("<3", ":)", ...); this is display only.
inline void drawEmote(const char* code, int cx, int cy) {
  const int R = 38;
  auto arc = [&](int ax, int ay, float rad, int d0, int d1, float th, uint16_t col) {
    int px = 0, py = 0; bool first = true;
    for (int d = d0; d <= d1; d += 12) {
      float a = radians((float)d);
      int x = ax + (int)(cosf(a) * rad), y = ay + (int)(sinf(a) * rad);
      if (!first) puck::display().drawWedgeLine(px, py, x, y, th, th, col);
      px = x; py = y; first = false;
    }
  };
  if (!strcmp(code, "<3")) {                              // heart
    puck::display().fillSmoothCircle(cx - 17, cy - 12, 20, RED);
    puck::display().fillSmoothCircle(cx + 17, cy - 12, 20, RED);
    puck::display().fillTriangle(cx - 35, cy - 6, cx + 35, cy - 6, cx, cy + 34, RED);
  } else if (!strcmp(code, ":)")) {                       // smiley
    puck::display().fillSmoothCircle(cx, cy, R, YELLOW);
    puck::display().fillSmoothCircle(cx - 14, cy - 10, 5, BLACK);
    puck::display().fillSmoothCircle(cx + 14, cy - 10, 5, BLACK);
    arc(cx, cy - 2, R * 0.55f, 28, 152, 2.5f, BLACK);     // smile (lower arc)
  } else if (!strcmp(code, ":D")) {                       // grin
    puck::display().fillSmoothCircle(cx, cy, R, YELLOW);
    puck::display().fillSmoothCircle(cx - 14, cy - 12, 5, BLACK);
    puck::display().fillSmoothCircle(cx + 14, cy - 12, 5, BLACK);
    int mr = 19, my = cy + 7;
    puck::display().fillSmoothCircle(cx, my, mr, BLACK);                            // open mouth
    puck::display().fillRect(cx - mr - 2, my - mr - 2, 2 * mr + 4, mr + 2, YELLOW); // mask upper half
    puck::display().fillRect(cx - mr + 3, my - 1, 2 * mr - 6, 3, WHITE);            // teeth
  } else if (!strcmp(code, "zZ")) {                       // sleeping
    puck::display().fillSmoothCircle(cx, cy, R, YELLOW);
    puck::display().drawWedgeLine(cx - 21, cy - 8, cx - 7, cy - 8, 2.0f, 2.0f, BLACK);  // closed eyes
    puck::display().drawWedgeLine(cx + 7, cy - 8, cx + 21, cy - 8, 2.0f, 2.0f, BLACK);
    puck::display().drawWedgeLine(cx - 6, cy + 14, cx + 6, cy + 14, 2.0f, 2.0f, BLACK); // calm mouth
    auto Z = [&](int zx, int zy, int s, uint16_t col) {                          // a "z"
      puck::display().drawWedgeLine(zx, zy, zx + s, zy, 1.6f, 1.6f, col);
      puck::display().drawWedgeLine(zx + s, zy, zx, zy + s, 1.6f, 1.6f, col);
      puck::display().drawWedgeLine(zx, zy + s, zx + s, zy + s, 1.6f, 1.6f, col);
    };
    Z(cx + 26, cy - 40, 10, CYAN);
    Z(cx + 20, cy - 26, 7, CYAN);
  } else if (!strcmp(code, "GM")) {                       // good morning: sun
    puck::display().fillSmoothCircle(cx, cy, 22, YELLOW);
    for (int k = 0; k < 12; k++) {
      float a = k * (float)(PI / 6);
      int x0 = cx + (int)(cosf(a) * 30), y0 = cy + (int)(sinf(a) * 30);
      int x1 = cx + (int)(cosf(a) * 40), y1 = cy + (int)(sinf(a) * 40);
      puck::display().drawWedgeLine(x0, y0, x1, y1, 2.2f, 2.2f, ORANGE);
    }
  } else if (!strcmp(code, "GN")) {                       // good night: crescent moon + star
    puck::display().fillSmoothCircle(cx - 6, cy, R, YELLOW);
    puck::display().fillSmoothCircle(cx + 12, cy - 6, R, BLACK);   // carve the crescent
    int sx = cx + 20, sy = cy - 22;                          // little star
    puck::display().drawWedgeLine(sx - 6, sy, sx + 6, sy, 1.4f, 1.4f, YELLOW);
    puck::display().drawWedgeLine(sx, sy - 6, sx, sy + 6, 1.4f, 1.4f, YELLOW);
  } else {                                                // fallback: the ASCII code
    puck::display().setTextDatum(middle_center);
    puck::display().setFont(&fonts::Font0); puck::display().setTextSize(6);
    puck::display().setTextColor(CYAN, BLACK);
    puck::display().drawString(code, cx, cy);
  }
}

class EmojiApp : public App {
  // ASCII "emotes" so they render in the default font. Swap for bitmaps later.
  const char* set[6] = {"<3", ":)", ":D", "zZ", "GM", "GN"};
  int sel = 0;
  int focusIdx = 0;             // button nav: 0 = recipient, 1 = emote, 2 = send
  bool dirty = true;
  String banner = "";
  uint32_t bannerUntil = 0;
  bool     bannerDrawn = false;  // draw the "sent" banner once (not every frame) to avoid flicker
  String target = "*";          // "*" = all friends, else a specific friend code (remembered)
  uint32_t shownVer = 0xFFFFFFFF;

  String targetName() {
    if (target == "*") return "All";
    Friends::Friend fr[Friends::MAX_FRIENDS];
    int n = Friends::get(fr, Friends::MAX_FRIENDS);
    for (int i = 0; i < n; i++)
      if (fr[i].code == target) return fr[i].nick.length() ? fr[i].nick : (fr[i].name.length() ? fr[i].name : fr[i].code);
    return "All";               // saved friend is gone -> fall back to All
  }

  void cycleTarget() {          // All -> friend0 -> friend1 -> ... -> All
    Friends::Friend fr[Friends::MAX_FRIENDS];
    int n = Friends::get(fr, Friends::MAX_FRIENDS);
    if (target == "*") { target = n ? fr[0].code : String("*"); }
    else {
      int idx = -1;
      for (int i = 0; i < n; i++) if (fr[i].code == target) { idx = i; break; }
      target = (idx < 0 || idx + 1 >= n) ? String("*") : fr[idx + 1].code;
    }
    Settings::saveEmojiTarget(target);
    dirty = true;
  }

  // drawEmote() is a file-scope helper (above) so the incoming-ping overlay can reuse it.

  void drawPicker() {
    int w = puck::display().width(), cx = w / 2, cy = puck::display().height() / 2;
    puck::display().fillScreen(BLACK);

    // connection / friend count
    puck::display().setTextDatum(top_center);
    puck::display().setFont(&fonts::Font0); puck::display().setTextSize(1);
    String st = Broker::connected() ? (String(Friends::count()) + " friends")
                                    : (Broker::configured() ? "connecting..." : "no MQTT");
    if (Notify::muted()) st += "   muted";
    puck::display().setTextColor(Broker::connected() ? GREEN : ORANGE, BLACK);
    puck::display().drawString(st, cx, 6);

    // recipient (tap the top strip to change)
    puck::display().setTextSize(2); puck::display().setTextColor(CYAN, BLACK);
    puck::display().drawString("To: " + targetName(), cx, 24);

    // big emote (vector graphic)
    drawEmote(set[sel], cx, cy + 6);

    // controls
    puck::display().setTextSize(2); puck::display().setTextColor(DARKGREY, BLACK);
    puck::display().drawString("<prev   SEND   next>", cx, puck::display().height() - 22);
  }

public:
  EmojiApp() : App("Emoji") {}
  bool needsNet() const override { return true; }

  void onEnter() override {
    target = Settings::emojiTarget();
    shownVer = 0xFFFFFFFF;
    dirty = true;
  }

  void loop() override {
    uint32_t v = Friends::version();           // list/name changes affect the target label
    if (v != shownVer) { shownVer = v; dirty = true; }
    // incoming pings are shown by the global overlay in main.cpp (works on any screen)

    if (gTap.pressed) {
      int w = puck::display().width();
      if (gTap.y < 50) {                       // top strip -> change recipient
        cycleTarget();
      } else if (gTap.x < w / 3)      { sel = (sel + 5) % 6; dirty = true; }
      else if (gTap.x > (w * 2) / 3)  { sel = (sel + 1) % 6; dirty = true; }
      else {                                   // SEND to the remembered target (one tap)
        bool ok = (target == "*") ? Friends::sendEmote(set[sel])
                                  : Friends::sendEmoteTo(target, set[sel]);
        Notify::gentle(String("sent ") + set[sel]);
        banner = ok ? (String("sent ") + set[sel] + " to " + targetName())
                    : String("no friends yet");
        bannerUntil = millis() + 1500;
        bannerDrawn = false;             // paint the new banner once; no full redraw (that was the flicker)
      }
      gTap.pressed = false;   // consume the tap
    }

    if (banner.length() && millis() >= bannerUntil) {        // expired -> erase only the banner strip (no flicker)
      banner = "";
      int y = puck::display().height() - 52;
      puck::display().fillRect(0, y - 12, puck::display().width(), 24, BLACK);
    }

    if (dirty) { drawPicker(); dirty = false; bannerDrawn = false; }

    if (banner.length() && !bannerDrawn) {   // draw the "sent" banner ONCE over the picker (no per-frame flicker)
      int cx = puck::display().width() / 2, y = puck::display().height() - 52;
      puck::display().fillRect(0, y - 12, puck::display().width(), 24, BLACK);
      puck::display().setTextDatum(middle_center);
      puck::display().setFont(&fonts::Font0); puck::display().setTextSize(2);
      puck::display().setTextColor(WHITE, BLACK);
      puck::display().drawString(banner, cx, y);
      bannerDrawn = true;
    }
  }

  // ---- button nav: recipient / emote / send ----
  int  focusCount() override { return 3; }
  void focusMove(int d) override { focusIdx = (focusIdx + d + 3) % 3; dirty = true; }
  void focusSelect() override {
    if (focusIdx == 0)      cycleTarget();                       // change recipient
    else if (focusIdx == 1) { sel = (sel + 1) % 6; dirty = true; }   // next emote
    else { gTap.pressed = true; gTap.x = puck::display().width() / 2;   // SEND (synthesize the center tap)
           gTap.y = puck::display().height() / 2; }
  }
  void drawFocus() override {
    int w = puck::display().width(), h = puck::display().height(), x, y, ww, hh;
    if (focusIdx == 0)      { x = 4;          y = 14;       ww = w - 8; hh = 24; }   // recipient strip
    else if (focusIdx == 1) { x = w / 2 - 44; y = h / 2-30; ww = 88;    hh = 72; }   // big emote
    else                    { x = w / 2 - 64; y = h - 34;   ww = 128;   hh = 24; }   // SEND control
    puck::display().drawRoundRect(x, y, ww, hh, 4, WHITE);
  }
};

// ---------------- Friends ----------------
// Manage friends: show your shareable code, add a friend by code, approve/deny incoming
// requests, list/remove confirmed friends. Sending lives in the Emoji app; the protocol
// lives in the always-on Friends service.
class FriendsApp : public App {
  enum Mode { HOME, ADD, NAME, RENAME };
  Mode     mode = HOME;
  uint32_t shownVer = 0xFFFFFFFF;
  bool     dirty = true;
  String   typed;
  String   renameCode;            // friend whose local nickname we're editing (RENAME mode)

  Friends::Friend req[Friends::MAX_FRIENDS]; int reqN = 0;   // incoming requests (rebuilt each frame)
  Friends::Friend fr[Friends::MAX_FRIENDS];  int frN  = 0;   // confirmed friends

  static const int ROW_TOP = 92, ROWH = 23;
  static const int REQH = 34;            // incoming requests are 2 lines (name + device id) -> taller
  int focusIdx = 0;                      // button nav: focused HOME item (Add / Set-name / request / friend)
  static const int KW = 32, KH = 30; int KB_TOP = 58;  // KB_TOP voff'd per-app in onEnter

  // ----- shared on-screen keyboard (ADD = hex, NAME = alpha) -----
  void drawKeyboard(const char* const* rows, int nRows, const char* title, const char* okLabel) {
    int w = puck::display().width();
    puck::display().fillScreen(BLACK);
    puck::display().setTextDatum(top_center);
    puck::display().setFont(&fonts::Font0); puck::display().setTextSize(1);
    puck::display().setTextColor(DARKGREY, BLACK);
    puck::display().drawString(title, w / 2, 6);
    puck::display().setTextDatum(middle_center); puck::display().setTextSize(3);
    puck::display().setTextColor(WHITE, BLACK);
    puck::display().drawString(typed.length() ? typed : String("_"), w / 2, 32);
    puck::display().setTextSize(2);
    for (int r = 0; r < nRows; r++) {
      const char* row = rows[r];
      for (int i = 0; row[i]; i++) {
        int x = i * KW, y = KB_TOP + r * KH;
        puck::display().drawRoundRect(x + 1, y + 1, KW - 2, KH - 2, 3, DARKGREY);
        char s[2] = { row[i], 0 };
        puck::display().setTextColor(CYAN, BLACK);
        puck::display().drawString(s, x + KW / 2, y + KH / 2);
      }
    }
    int by = KB_TOP + nRows * KH;
    const char* labels[3] = { "DEL", "BACK", okLabel };
    uint16_t cols[3] = { ORANGE, DARKGREY, GREEN };
    for (int i = 0; i < 3; i++) {
      int x = i * (w / 3);
      puck::display().drawRoundRect(x + 2, by + 1, w / 3 - 4, 34, 4, cols[i]);
      puck::display().setTextColor(cols[i], BLACK);
      puck::display().drawString(labels[i], x + (w / 3) / 2, by + 18);
    }
  }

  // returns true if OK was pressed
  bool handleKbTap(int x, int y, const char* const* rows, int nRows, int maxLen) {
    int w = puck::display().width(), by = KB_TOP + nRows * KH;
    if (y >= by) {
      int b = x / (w / 3);
      if (b == 0) { if (typed.length()) typed.remove(typed.length() - 1); }
      else if (b == 1) { mode = HOME; }
      else if (b == 2) return true;
      dirty = true;
      return false;
    }
    int r = (y - KB_TOP) / KH;
    if (r >= 0 && r < nRows) {
      const char* row = rows[r];
      int i = x / KW;
      if (i >= 0 && i < (int)strlen(row) && (int)typed.length() < maxLen) { typed += row[i]; dirty = true; }
    }
    return false;
  }

  void drawHome() {
    int w = puck::display().width();
    puck::display().fillScreen(BLACK);
    puck::display().setTextDatum(top_center);
    puck::display().setFont(&fonts::Font0);
    puck::display().setTextSize(2); puck::display().setTextColor(WHITE, BLACK);
    puck::display().drawString("Friends", w / 2, 6);

    // shareable code + name/status
    puck::display().setTextColor(CYAN, BLACK);
    puck::display().drawString(Friends::myCode(), w / 2, 28);
    puck::display().setTextSize(1);
    String sub = Friends::myName();
    if (!Broker::connected()) sub += Broker::configured() ? "  (offline)" : "  (no MQTT)";
    puck::display().setTextColor(DARKGREY, BLACK);
    puck::display().drawString(sub, w / 2, 50);

    // Add / Set name buttons
    int bw = 132, bh = 22, by = 62, ax = w / 2 - bw - 6, nx = w / 2 + 6;
    puck::display().setTextDatum(middle_center);
    puck::display().setTextSize(2);                        // readable button labels (wider boxes fit them)
    puck::display().drawRoundRect(ax, by, bw, bh, 5, GREEN);
    puck::display().setTextColor(GREEN, BLACK);
    puck::display().drawString("Add friend", ax + bw / 2, by + bh / 2);
    puck::display().drawRoundRect(nx, by, bw, bh, 5, CYAN);
    puck::display().setTextColor(CYAN, BLACK);
    puck::display().drawString("Set name", nx + bw / 2, by + bh / 2);

    if (reqN == 0 && frN == 0) {
      puck::display().setTextDatum(top_center); puck::display().setTextSize(1); puck::display().setTextColor(DARKGREY, BLACK);
      puck::display().drawString("share your code, then Add a friend", w / 2, ROW_TOP + 18);
      return;
    }

    int y = ROW_TOP;
    for (int i = 0; i < reqN && y < puck::display().height() - REQH; i++, y += REQH) {     // incoming requests: name + device id
      puck::display().setFont(&fonts::Font0);
      puck::display().setTextSize(2); puck::display().setTextDatum(top_left); puck::display().setTextColor(ORANGE, BLACK);
      puck::display().drawString(req[i].name.length() ? req[i].name : String("(no name)"), 6, y + 1);
      puck::display().setTextSize(1); puck::display().setTextColor(DARKGREY, BLACK);    // verify this id before accepting
      puck::display().drawString(String("id ") + req[i].code, 6, y + 20);
      int bh2 = 22, by2 = y + (REQH - bh2) / 2;
      puck::display().setTextDatum(middle_center); puck::display().setTextSize(2);
      puck::display().drawRoundRect(w - 86, by2, 36, bh2, 4, GREEN);
      puck::display().setTextColor(GREEN, BLACK); puck::display().drawString("OK", w - 86 + 18, by2 + bh2 / 2);
      puck::display().drawRoundRect(w - 44, by2, 36, bh2, 4, RED);
      puck::display().setTextColor(RED, BLACK);   puck::display().drawString("X", w - 44 + 18, by2 + bh2 / 2);
    }
    for (int i = 0; i < frN && y < puck::display().height() - ROWH; i++, y += ROWH) {      // confirmed friends
      puck::display().setTextSize(2);                                       // name big...
      puck::display().setTextDatum(middle_left); puck::display().setTextColor(GREEN, BLACK);
      puck::display().drawString(fr[i].nick.length() ? fr[i].nick : (fr[i].name.length() ? fr[i].name : fr[i].code), 6, y + ROWH / 2);
      puck::display().setTextSize(1);                                       // ...code + remove-x stay small (avoid collision)
      puck::display().setTextDatum(middle_right); puck::display().setTextColor(DARKGREY, BLACK);
      puck::display().drawString(fr[i].code, w - 50, y + ROWH / 2);
      puck::display().drawRoundRect(w - 40, y + 2, 32, ROWH - 4, 4, DARKGREY);
      puck::display().setTextDatum(middle_center);
      puck::display().drawString("x", w - 40 + 16, y + ROWH / 2);
    }
  }

  void handleHomeTap(int x, int y) {
    int w = puck::display().width();
    int bw = 132, bh = 22, by = 62, ax = w / 2 - bw - 6, nx = w / 2 + 6;
    if (y >= by && y <= by + bh) {
      if (x >= ax && x <= ax + bw) { mode = ADD;  typed = "";               dirty = true; return; }
      if (x >= nx && x <= nx + bw) { mode = NAME; typed = Friends::myName(); dirty = true; return; }
    }
    int yy = ROW_TOP;
    for (int i = 0; i < reqN; i++, yy += REQH) {
      if (y >= yy && y < yy + REQH) {
        if (x >= w - 86 && x < w - 50) { Friends::approve(req[i].code); dirty = true; }  // OK -> accept (rename later from the list)
        else if (x >= w - 44)          { Friends::deny(req[i].code);    dirty = true; }
        return;
      }
    }
    for (int i = 0; i < frN; i++, yy += ROWH) {
      if (y >= yy && y < yy + ROWH) {
        if (x >= w - 40) { Friends::removeFriend(fr[i].code); dirty = true; }   // x -> remove
        else { renameCode = fr[i].code; typed = fr[i].nick; mode = RENAME; dirty = true; }  // tap name -> rename
        return;
      }
    }
  }

public:
  FriendsApp() : App("Friends") {}
  bool needsNet() const override { return true; }

  void onEnter() override { mode = HOME; typed = ""; shownVer = 0xFFFFFFFF; dirty = true; }
  bool onBack() override { if (mode != HOME) { mode = HOME; dirty = true; return true; } return false; }

  void loop() override {
    static const char* HEXROWS[]   = { "0123456789", "ABCDEF" };
    static const char* ALPHAROWS[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };

    uint32_t v = Friends::version();
    if (v != shownVer) { shownVer = v; dirty = true; }
    reqN = Friends::pending(req, Friends::MAX_FRIENDS);
    frN  = Friends::get(fr, Friends::MAX_FRIENDS);

    if (gTap.pressed) {
      if (mode == HOME) handleHomeTap(gTap.x, gTap.y);
      else if (mode == ADD) {
        if (handleKbTap(gTap.x, gTap.y, HEXROWS, 2, 8)) {
          if (typed.length()) { String c = typed; c.toUpperCase();   // added -> chain to an optional nickname
            if (Friends::addFriend(c)) { renameCode = c; typed = ""; mode = RENAME; } else mode = HOME;
          } else mode = HOME;
          dirty = true;
        }
      } else if (mode == NAME) {
        if (handleKbTap(gTap.x, gTap.y, ALPHAROWS, 4, 16)) {
          Friends::setName(typed); mode = HOME; dirty = true;
        }
      } else {   // RENAME -> save a local nickname for renameCode (BACK = skip, GO = save; empty clears)
        if (handleKbTap(gTap.x, gTap.y, ALPHAROWS, 4, 16)) {
          Friends::setNick(renameCode, typed); mode = HOME; dirty = true;
        }
      }
      gTap.pressed = false;
    }

    if (dirty) {
      if (mode == HOME)      drawHome();
      else if (mode == ADD)  drawKeyboard(HEXROWS, 2, "Enter friend code", "ADD");
      else if (mode == NAME) drawKeyboard(ALPHAROWS, 4, "Set your name", "SAVE");
      else { String t = "Nickname for " + renameCode; drawKeyboard(ALPHAROWS, 4, t.c_str(), "SAVE"); }
      dirty = false;
    }
  }

  // ---- button nav: HOME (Add / Set-name / request-OK / friend-row). Keypads stay touch. ----
  bool focusItem(int i, int& x, int& y, int& w, int& h) {
    if (mode != HOME) return false;
    int W = puck::display().width(), bw = 132, bh = 22, by = 62;
    if (i == 0) { x = W / 2 - bw - 6; y = by; w = bw; h = bh; return true; }   // Add friend
    if (i == 1) { x = W / 2 + 6;      y = by; w = bw; h = bh; return true; }   // Set name
    int ri = i - 2;
    if (ri >= 0 && ri < reqN) { x = W - 90; y = ROW_TOP + ri * REQH + (REQH - 22) / 2; w = 40; h = 22; return true; }  // request OK
    int fi = ri - reqN;
    if (fi >= 0 && fi < frN)  { x = 4; y = ROW_TOP + reqN * REQH + fi * ROWH; w = W - 48; h = ROWH; return true; }     // friend row
    return false;
  }
  int  focusCount() override { int x, y, w, h, n = 0; while (focusItem(n, x, y, w, h)) n++; return n; }
  void focusMove(int d) override { int n = focusCount(); if (!n) return; focusIdx = (focusIdx + d + n) % n; dirty = true; }
  void focusSelect() override {
    int i = focusIdx;
    if (i == 0)      { mode = ADD;  typed = "";                dirty = true; }
    else if (i == 1) { mode = NAME; typed = Friends::myName(); dirty = true; }
    else {
      int ri = i - 2;
      if (ri >= 0 && ri < reqN) { Friends::approve(req[ri].code); dirty = true; }   // accept the request
      else { int fi = ri - reqN; if (fi >= 0 && fi < frN) { renameCode = fr[fi].code; typed = fr[fi].nick; mode = RENAME; dirty = true; } }
    }
  }
  void drawFocus() override { int x, y, w, h; if (focusItem(focusIdx, x, y, w, h)) puck::display().drawRoundRect(x, y, w, h, 4, WHITE); }
};
