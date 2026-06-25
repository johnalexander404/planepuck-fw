#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>               // isdigit/isalnum for the weather geocoder query
#include <esp_random.h>          // esp_random() — HW RNG for the self-enroll password
#include "config.h"
#include "services.h"

// ===================== Settings (persisted in NVS) =====================
namespace Settings {
  static Preferences prefs;

  void begin() { prefs.begin("puck", false); }

  String ssid() {
    String s = prefs.getString("ssid", "");
    return s.length() ? s : String(WIFI_SSID);   // fall back to compile-time default
  }
  String pass() {
    String p = prefs.getString("pass", "");
    return p.length() ? p : String(WIFI_PASS);
  }
  String tz() { return prefs.getString("tz", TZ_STRING); }
  String zip() { return prefs.getString("zip", ""); }
  bool haveWifi() { return ssid().length() > 0; }

  void saveWifi(const String& s, const String& p) {
    prefs.putString("ssid", s);
    prefs.putString("pass", p);
  }
  void saveTz(const String& t) { prefs.putString("tz", t); }
  void saveZip(const String& z) { prefs.putString("zip", z); }
  bool magCalibrated() { return prefs.getBool("magcal", false); }
  void setMagCalibrated(bool v) { prefs.putBool("magcal", v); }
  String friendsBlob() { return prefs.getString("friends", ""); }
  void saveFriendsBlob(const String& j) { prefs.putString("friends", j); }
  String displayName() { return prefs.getString("dname", ""); }
  void saveDisplayName(const String& n) { prefs.putString("dname", n); }
  String emojiTarget() { return prefs.getString("emotgt", "*"); }
  void saveEmojiTarget(const String& t) { prefs.putString("emotgt", t); }
  String mqttPass() { String p = prefs.getString("mqttpass", ""); return p.length() ? p : String(MQTT_PASS); }
  void saveMqttPass(const String& p) { prefs.putString("mqttpass", p); }
  String worldClockBlob() { return prefs.getString("wclock", ""); }
  void saveWorldClockBlob(const String& j) { prefs.putString("wclock", j); }
  String weatherCitiesBlob() { return prefs.getString("wxcity", ""); }
  void saveWeatherCitiesBlob(const String& j) { prefs.putString("wxcity", j); }
  String searchHistory() { return prefs.getString("flthist", ""); }
  void saveSearchHistory(const String& s) { prefs.putString("flthist", s); }
  bool tempF() { return prefs.getBool("tempf", WEATHER_UNITS_F); }   // compile-time default, runtime override
  void saveTempF(bool f) { prefs.putBool("tempf", f); }
  bool clock12h() { return prefs.getBool("clk12", false); }
  void saveClock12h(bool v) { prefs.putBool("clk12", v); }
  bool beta() { return prefs.getBool("beta", false); }              // RC channel flag; set via authenticated MQTT fleet/channel
  void saveBeta(bool v) { prefs.putBool("beta", v); }
}

// ===================== Net =====================
namespace Net {
  static bool started = false;
  void begin() {
    String s = Settings::ssid();
    if (s.length() == 0) return;          // offline mode
    WiFi.mode(WIFI_STA);
    WiFi.begin(s.c_str(), Settings::pass().c_str());
    started = true;
  }
  void loop() { /* WiFi auto-reconnects */ }
  bool online() { return started && WiFi.status() == WL_CONNECTED; }
}

// ===================== Provision (captive portal) =====================
namespace Provision {
  static WebServer  server(80);
  static DNSServer  dns;
  static bool       isActive = false;
  static uint32_t   restartAt = 0;
  static const IPAddress apIP(192, 168, 4, 1);
  static const char* AP_SSID = "PlanePuck-Setup";

  // Timezones offered in the dropdowns (home TZ + world-clock cities). label -> POSIX TZ + a short
  // 3-letter abbr shown on the world-clock rows. Keep existing `tz` strings byte-stable (a saved home
  // TZ is matched by string).
  struct TzOpt { const char* label; const char* tz; const char* abbr; float lat; float lon; };
  static const TzOpt TZS[] = {       // world-clock picker (weather cities are now free-typed + geocoded)
    {"Pacific (US)",   "PST8PDT,M3.2.0,M11.1.0",       "LAX",  34.05f, -118.24f},
    {"Mountain (US)",  "MST7MDT,M3.2.0,M11.1.0",       "DEN",  39.74f, -104.99f},
    {"Central (US)",   "CST6CDT,M3.2.0,M11.1.0",       "CHI",  41.88f,  -87.63f},
    {"Eastern (US)",   "EST5EDT,M3.2.0,M11.1.0",       "NYC",  40.71f,  -74.01f},
    {"Sao Paulo",      "BRT3",                         "SAO", -23.55f,  -46.63f},
    {"UTC",            "UTC0",                         "UTC",  51.48f,    0.00f},
    {"London",         "GMT0BST,M3.5.0/1,M10.5.0",     "LON",  51.51f,   -0.13f},
    {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3",   "CET",  52.52f,   13.40f},
    {"Dubai",          "GST-4",                        "DXB",  25.20f,   55.27f},
    {"India",          "IST-5:30",                     "DEL",  28.61f,   77.21f},
    {"Singapore",      "SGT-8",                        "SIN",   1.35f,  103.82f},
    {"Hong Kong",      "HKT-8",                        "HKG",  22.32f,  114.17f},
    {"Japan",          "JST-9",                        "TYO",  35.68f,  139.69f},
    {"Sydney",         "AEST-10AEDT,M10.1.0,M4.1.0/3", "SYD", -33.87f,  151.21f},
    {"Auckland",       "NZST-12NZDT,M9.5.0,M4.1.0/3",  "AKL", -36.85f,  174.76f},
  };
  static const int TZ_COUNT = sizeof(TZS) / sizeof(TZS[0]);

  // Section heading with an inline (self-contained, no external load) line icon.
  static String sec(const char* icon, const char* title) {
    String s = F("<div class=sec><h2><svg class=ic viewBox='0 0 24 24' fill=none stroke=currentColor "
                 "stroke-width=2 stroke-linecap=round stroke-linejoin=round>");
    s += icon; s += F("</svg>"); s += title; s += F("</h2>");
    return s;
  }
  // 4 city <select>s sharing the TZS catalog, pre-selected from a saved blob, in a 2x2 grid.
  static String citySelects(const char* nm, int maxN, const String& blob) {
    String saved[8];
    { JsonDocument d;
      if (!deserializeJson(d, blob)) { int i = 0;
        for (JsonObject o : d.as<JsonArray>()) { if (i >= maxN) break; saved[i++] = String((const char*)(o["z"] | "")); } } }
    String s = F("<div class=grid>");
    for (int w = 0; w < maxN; w++) {
      s += "<select name="; s += nm; s += w; s += "><option value=''>(none)</option>";
      for (int i = 0; i < TZ_COUNT; i++) {
        s += "<option value='"; s += TZS[i].tz; s += "'";
        if (saved[w] == TZS[i].tz) s += " selected";
        s += ">"; s += TZS[i].label; s += "</option>";
      }
      s += "</select>";
    }
    s += F("</div>");
    return s;
  }
  // N free-text "city or ZIP" inputs (weather), pre-filled from the saved blob [{"q":...}], 2x2 grid.
  static String cityInputs(const char* nm, int maxN, const String& blob) {
    String saved[8];
    { JsonDocument d;
      if (!deserializeJson(d, blob)) { int i = 0;
        for (JsonObject o : d.as<JsonArray>()) { if (i >= maxN) break; saved[i++] = String((const char*)(o["q"] | "")); } } }
    String s = F("<div class=grid>");
    for (int w = 0; w < maxN; w++) {
      s += "<input name="; s += nm; s += w; s += " placeholder='City or ZIP' value='"; s += saved[w]; s += "'>";
    }
    s += F("</div>");
    return s;
  }

  static String page() {
    String cur = Settings::tz();
    String h = F(
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>PlanePuck setup</title><style>"
      "*{box-sizing:border-box}"
      "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;"
      "background:#eef0f3;color:#1c1e21;line-height:1.4}"
      ".wrap{max-width:480px;margin:0 auto;padding:20px 16px 36px}"
      ".card{padding:0}"                                    /* sections are the panels now, not one big card */
      ".hdr{text-align:center;margin-bottom:18px}.logo{width:54px;height:54px}"
      "h1{font-size:22px;font-weight:600;margin:8px 0 2px}.sub{color:#8a8d91;font-size:14px;margin:0}"
      ".sec{background:#fff;border:1px solid #e4e6eb;border-radius:14px;padding:16px 18px;"
      "margin-bottom:14px;box-shadow:0 1px 2px rgba(0,0,0,.05)}"
      ".sec h2{display:flex;align-items:center;font-size:15px;font-weight:600;color:#1c1e21;"
      "margin:0 0 12px;padding-bottom:11px;border-bottom:1px solid #eceef1}"
      ".ic{box-sizing:content-box;width:17px;height:17px;padding:5px;margin-right:10px;"
      "color:#2d7ff9;background:#eaf2ff;border-radius:8px;flex:none}"
      "label{display:block;margin:12px 0 4px;font-size:13px;color:#4b4f56;font-weight:500}"
      "input,select{width:100%;padding:11px 12px;font-size:16px;border:1px solid #d0d3d9;"
      "border-radius:10px;background:#fff;color:inherit}"
      "input:focus,select:focus{outline:none;border-color:#2d7ff9;box-shadow:0 0 0 3px rgba(45,127,249,.15)}"
      ".grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:4px}"
      "button{margin-top:6px;padding:14px;width:100%;font-size:16px;font-weight:600;color:#fff;"
      "background:#2d7ff9;border:0;border-radius:12px}button:active{background:#1b6ae0}"
      ".foot{text-align:center;color:#9a9da1;font-size:12px;margin-top:16px}"
      "@media(prefers-color-scheme:dark){body{background:#0b0d10;color:#e7e9ee}"
      ".sec{background:#1a1d23;border-color:#2a2e36;box-shadow:none}"
      ".sec h2{color:#e7e9ee;border-color:#262a31}.ic{color:#7fb4ff;background:#15243a}"
      "label{color:#aeb2ba}input,select{background:#0f1115;border-color:#343a44}}"
      "</style></head><body><div class=wrap><div class=card>"
      "<div class=hdr><svg class=logo viewBox='0 0 24 24'><circle cx=12 cy=12 r=12 fill='#2d7ff9'/>"
      "<path d='M4 20l16-8L4 4v6l11 2-11 2z' fill='#fff'/></svg>"
      "<h1>PlanePuck</h1><p class=sub>Device setup</p></div><form method=POST action=/save>");

    h += sec("<circle cx=12 cy=8 r=3.5/><path d='M5.5 20a6.5 6.5 0 0 1 13 0'/>", "Name");
    h += F("<label>Shown on the home &amp; clock screens &amp; to friends</label>"
           "<input name=dname maxlength=16 placeholder='e.g. John' value='");
    h += Settings::displayName();
    h += F("'></div>");

    h += sec("<path d='M5 13a11 11 0 0 1 14 0'/><path d='M8.5 16.5a6 6 0 0 1 7 0'/><path d='M12 20h.01'/>",
             "Network");
    h += F("<label>Wi-Fi network</label><input name=ssid list=nets value='");
    h += Settings::ssid();
    h += F("'><datalist id=nets>");
    int n = WiFi.scanComplete();
    for (int i = 0; i < n; i++) { h += "<option value='"; h += WiFi.SSID(i); h += "'>"; }
    h += F("</datalist><label>Password</label>"
           "<input name=pass type=password placeholder='blank = keep current'></div>");

    h += sec("<circle cx=12 cy=12 r=9/><path d='M12 7v5l3 2'/>", "Time &amp; clock");
    h += F("<label>Timezone</label><select name=tz>");
    for (int i = 0; i < TZ_COUNT; i++) {
      h += "<option value='"; h += TZS[i].tz; h += "'";
      if (cur == TZS[i].tz) h += " selected";
      h += ">"; h += TZS[i].label; h += "</option>";
    }
    h += F("</select>");
    h += F("<label>Clock format</label><select name=clk>");
    h += String("<option value=24") + (Settings::clock12h() ? "" : " selected") + ">24-hour</option>";
    h += String("<option value=12") + (Settings::clock12h() ? " selected" : "") + ">12-hour (AM/PM)</option>";
    h += F("</select>");
    h += F("<label>World clock cities (optional)</label>");
    h += citySelects("wc", MAX_WORLD_CITIES, Settings::worldClockBlob());
    h += F("</div>");

    h += sec("<path d='M7 18a4 4 0 0 1 .5-8 5 5 0 0 1 9.5 1.2A3.5 3.5 0 0 1 17 18z'/>", "Weather");
    h += F("<label>Temperature units</label><select name=unit>");
    h += String("<option value=c") + (Settings::tempF() ? "" : " selected") + ">Celsius (&deg;C)</option>";
    h += String("<option value=f") + (Settings::tempF() ? " selected" : "") + ">Fahrenheit (&deg;F)</option>";
    h += F("</select>");
    h += F("<label>Extra cities (optional; city or ZIP — add a country for duplicates, e.g. "
           "&quot;Kochi, IN&quot;; all blank = your location)</label>");
    h += cityInputs("we", MAX_WEATHER_CITIES, Settings::weatherCitiesBlob());
    h += F("<label>Your location ZIP (US, optional)</label><input name=zip inputmode=numeric value='");
    h += Settings::zip();
    h += F("'></div>");

    // MQTT password: only ask when self-enroll is OFF. With ENROLL_URL/ENROLL_TOKEN set, the puck
    // provisions its own broker password automatically, so there's nothing for the user to type.
    if (!(strlen(ENROLL_URL) && strlen(ENROLL_TOKEN))) {
      h += sec("<path d='M5 5h14v10H9l-4 3z'/>", "Friends");
      h += F("<label>MQTT password (optional)</label>"
             "<input name=mpass type=password placeholder='unchanged if left blank'></div>");
    }
    h += F("<button type=submit>Save &amp; restart</button></form>"
           "<p class=foot>PlanePuck &middot; settings are saved on the device</p>"
           "</div></div></body></html>");
    return h;
  }

  static void handleRoot() { server.send(200, "text/html", page()); }

  static void handleSave() {
    String s = server.arg("ssid");
    String p = server.arg("pass");
    String t = server.arg("tz");
    if (s.length() && p.length()) Settings::saveWifi(s, p);   // blank password keeps the saved one
    if (t.length()) Settings::saveTz(t);
    Settings::saveZip(server.arg("zip"));   // saved as-is; blank clears it (back to IP location)
    String mp = server.arg("mpass");
    if (mp.length()) Settings::saveMqttPass(mp);   // blank = keep current (don't wipe the secret)
    Settings::saveDisplayName(server.arg("dname"));  // clock + friends name; pre-filled, so blank = cleared
    { String u = server.arg("unit"); if (u.length()) Settings::saveTempF(u == "f"); }      // temp units
    { String c = server.arg("clk");  if (c.length()) Settings::saveClock12h(c == "12"); }   // clock format
    // world-clock cities: rebuild from the up-to-MAX_WORLD_CITIES selects ("(none)" slots are dropped;
    // all-none clears the list). Unlike Wi-Fi/MQTT password, this is "submit = replace".
    JsonDocument wdoc; JsonArray warr = wdoc.to<JsonArray>();
    for (int i = 0; i < MAX_WORLD_CITIES; i++) {
      String z = server.arg(String("wc") + i);
      if (!z.length()) continue;
      const char* ab = "";
      for (int k = 0; k < TZ_COUNT; k++) if (z == TZS[k].tz) { ab = TZS[k].abbr; break; }
      JsonObject o = warr.add<JsonObject>();
      o["l"] = ab; o["z"] = z;
    }
    String wout; serializeJson(wdoc, wout);
    Settings::saveWorldClockBlob(wout);
    // weather cities: store the raw "city or ZIP" typed per slot (the device geocodes them at runtime;
    // can't geocode here — the captive portal runs on the SoftAP with no upstream internet yet).
    JsonDocument xdoc; JsonArray xarr = xdoc.to<JsonArray>();
    for (int i = 0; i < MAX_WEATHER_CITIES; i++) {
      String q = server.arg(String("we") + i); q.trim();
      if (!q.length()) continue;
      JsonObject o = xarr.add<JsonObject>();
      o["q"] = q;
    }
    String xout; serializeJson(xdoc, xout);
    Settings::saveWeatherCitiesBlob(xout);
    server.send(200, "text/html",
      String(F("<!doctype html><html><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'><style>"
               "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;"
               "min-height:100vh;display:flex;align-items:center;justify-content:center;"
               "background:#f4f5f7;color:#1c1e21}.c{text-align:center;padding:32px}.ck{width:64px;height:64px}"
               "h2{font-weight:600;margin:14px 0 4px;font-size:22px}p{color:#8a8d91;margin:0}"
               "@media(prefers-color-scheme:dark){body{background:#0f1115;color:#e7e9ee}}"
               "</style></head><body><div class=c>"
               "<svg class=ck viewBox='0 0 24 24'><circle cx=12 cy=12 r=12 fill='#22b07d'/>"
               "<path d='M7 12.4l3.5 3.5 7-7' fill=none stroke='#fff' stroke-width=2.4 "
               "stroke-linecap=round stroke-linejoin=round/></svg>"
               "<h2>Saved</h2><p>PlanePuck is restarting&hellip;</p></div></body></html>")));
    restartAt = millis() + 1500;
  }

  static void handleNotFound() {   // captive-portal redirect to the form
    server.sendHeader("Location", String("http://") + apIP.toString(), true);
    server.send(302, "text/plain", "");
  }

  void start() {
    if (isActive) return;
    WiFi.mode(WIFI_AP_STA);                       // AP for the portal, STA so we can scan
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID);
    WiFi.scanNetworks(true);                       // async scan to populate the SSID list
    dns.start(53, "*", apIP);                       // capture all DNS -> us
    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleNotFound);
    server.begin();
    isActive = true;
  }

  void loop() {
    if (!isActive) return;
    dns.processNextRequest();
    server.handleClient();
    if (restartAt && millis() > restartAt) { delay(200); ESP.restart(); }
  }

  void stop() {
    if (!isActive) return;
    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    isActive = false;
  }

  bool active()   { return isActive; }
  String apName() { return String(AP_SSID); }
}

// ===================== ClockService =====================
// Shared TZ/localtime lock: WorldClock::timeFor briefly flips the global TZ on the UI thread, while the
// Weather task also calls ClockService::getTime -> localtime_r. This serializes those two readers so a
// city's zone can't leak into a home-time read.
static SemaphoreHandle_t gTzMtx = nullptr;

namespace ClockService {
  static bool ntpStarted = false;
  static bool isSynced = false;

  static void seedFromRtc() {
    auto dt = M5.Rtc.getDateTime();
    int year = dt.date.year;
    if (year < 2020 || year > 2099) year = 2025;  // fresh/garbage -> default
    struct tm tmv = {};
    tmv.tm_year = year - 1900;
    tmv.tm_mon  = (dt.date.month >= 1 && dt.date.month <= 12) ? dt.date.month - 1 : 0;
    tmv.tm_mday = (dt.date.date >= 1 && dt.date.date <= 31) ? dt.date.date : 1;
    tmv.tm_hour = dt.time.hours % 24;
    tmv.tm_min  = dt.time.minutes % 60;
    tmv.tm_sec  = dt.time.seconds % 60;
    time_t t = mktime(&tmv);
    struct timeval tv;
    tv.tv_sec = t;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
  }

  void begin() {
    if (!gTzMtx) gTzMtx = xSemaphoreCreateMutex();
    setenv("TZ", Settings::tz().c_str(), 1);
    tzset();
    seedFromRtc();
    WorldClock::reload();
  }

  void loop() {
    if (!Net::online()) return;
    if (!ntpStarted) { configTzTime(Settings::tz().c_str(), NTP_SERVER); ntpStarted = true; }
    if (!isSynced) {
      struct tm tmnow;
      if (getLocalTime(&tmnow, 0) && (tmnow.tm_year + 1900) >= 2024) isSynced = true;
    }
  }

  void getTime(int& hour, int& minute, int& second) {
    time_t now = time(nullptr);
    struct tm tmv;
    if (gTzMtx) xSemaphoreTake(gTzMtx, portMAX_DELAY);
    localtime_r(&now, &tmv);
    if (gTzMtx) xSemaphoreGive(gTzMtx);
    hour = tmv.tm_hour; minute = tmv.tm_min; second = tmv.tm_sec;
  }
  bool synced() { return isSynced; }
}

namespace WorldClock {
  static City gCities[MAX_WORLD_CITIES];
  static int  gN = 0;

  void reload() {                                  // re-parse the NVS blob: [{"l":abbr,"z":posixTZ},...]
    gN = 0;
    String j = Settings::worldClockBlob();
    if (!j.length()) return;
    JsonDocument doc;
    if (deserializeJson(doc, j)) return;
    for (JsonObject o : doc.as<JsonArray>()) {
      if (gN >= MAX_WORLD_CITIES) break;
      String z = String((const char*)(o["z"] | ""));
      if (!z.length()) continue;
      gCities[gN].tz    = z;
      gCities[gN].label = String((const char*)(o["l"] | ""));
      gN++;
    }
  }
  int count() { return gN; }
  int get(City* out, int maxN) {
    int n = gN < maxN ? gN : maxN;
    for (int i = 0; i < n; i++) out[i] = gCities[i];
    return n;
  }
  bool timeFor(const City& c, int& hour, int& minute) {
    time_t now = time(nullptr);
    if (now < 1700000000) return false;            // clock not NTP-set yet
    struct tm tmv;
    if (gTzMtx) xSemaphoreTake(gTzMtx, portMAX_DELAY);
    setenv("TZ", c.tz.c_str(), 1); tzset();
    localtime_r(&now, &tmv);
    setenv("TZ", Settings::tz().c_str(), 1); tzset();   // restore the home zone before returning
    if (gTzMtx) xSemaphoreGive(gTzMtx);
    hour = tmv.tm_hour; minute = tmv.tm_min;
    return true;
  }
}

// ===================== Dim =====================
namespace Dim {
  static int cur = BRIGHT_DAY;
  static uint32_t lastWake = 0;
  static bool gSensor = false;          // LTR-553 detected at boot? (false on the SE)
  static const uint32_t WAKE_FULL_MS = 60000;  // stay at full for ~1 min after a touch before dimming
  static volatile bool gDimmed = false;        // true once the screen has dropped below the active level
  static bool gAllowIdleDim = true;            // only the Clock/launcher auto-dim; active apps hold steady

  // CoreS3's LTR-553 ambient light sensor, on the internal I2C bus. Compiled in always; whether it's
  // actually USED is decided at runtime by the boot probe below (so one binary covers CoreS3 + SE).
  static constexpr uint8_t LTR553_ADDR      = 0x23;
  static constexpr uint8_t LTR553_ALS_CONTR = 0x80;   // bit0: 1 = ALS active
  static constexpr uint8_t LTR553_MANUFAC   = 0x87;   // MANUFAC_ID == 0x05 when present
  static constexpr uint8_t LTR553_CH0_LOW   = 0x8A;   // visible+IR, 16-bit little-endian
  static uint16_t lastLux  = BRIGHT_DAY;              // raw CH0 reading (lux proxy)
  static uint32_t lastRead = 0;

  static void sensorBegin() {
    M5.In_I2C.writeRegister8(LTR553_ADDR, LTR553_ALS_CONTR, 0x01, 400000);  // wake into active mode
  }
  static uint16_t readLux() {
    uint8_t buf[2] = {0, 0};
    if (!M5.In_I2C.readRegister(LTR553_ADDR, LTR553_CH0_LOW, buf, 2, 400000)) return lastLux;
    return (uint16_t)(buf[0] | (buf[1] << 8));
  }

  void begin() {
    cur = BRIGHT_DAY; M5.Display.setBrightness(cur); lastWake = millis();
#if !FORCE_NO_LIGHT_SENSOR
    uint8_t id = 0;                     // auto-detect: present iff MANUFAC_ID reads 0x05
    if (M5.In_I2C.readRegister(LTR553_ADDR, LTR553_MANUFAC, &id, 1, 400000) && id == 0x05) {
      gSensor = true; sensorBegin();
    }
#endif
  }
  void wake()   { lastWake = millis(); }
  bool dimmed() { return gDimmed; }     // screen currently below the active level (tap should just wake it)
  void allowIdleDim(bool a) { gAllowIdleDim = a; }   // main.cpp: true only on the Clock/launcher

  bool isNight() {
    int h, m, s; ClockService::getTime(h, m, s);
    return (h < DAY_START_HOUR) || (h >= NIGHT_START_HOUR);
  }

  void loop() {
    int normal, idleDeep;
    if (gSensor) {                      // ambient-driven (full CoreS3)
      if (millis() - lastRead > 250) { lastRead = millis(); lastLux = readLux(); }  // poll a few times/sec
      long span = (long)LUX_BRIGHT - LUX_DARK; if (span < 1) span = 1;
      long t = BRIGHT_NIGHT + ((long)lastLux - LUX_DARK) * (BRIGHT_DAY - BRIGHT_NIGHT) / span;
      normal   = (int)constrain(t, (long)BRIGHT_NIGHT, (long)BRIGHT_DAY);
      idleDeep = (int)constrain((long)normal * IDLE_DIM_SCALE_PCT / 100, (long)BRIGHT_IDLE_NIGHT, (long)normal);
    } else {                            // time-of-day (SE, or forced off)
      bool night = isNight();
      normal   = night ? BRIGHT_NIGHT      : BRIGHT_DAY;
      idleDeep = night ? BRIGHT_IDLE_NIGHT : BRIGHT_IDLE_DAY;
    }
    uint32_t idle = millis() - lastWake;
    int target;
    if      (!gAllowIdleDim)                          target = normal;      // active app in use -> hold steady, never idle-dim
    else if (idle < WAKE_FULL_MS)                     target = BRIGHT_DAY;  // wake boost: full for ~1 min
    else if (idle < (uint32_t)IDLE_DIM_SEC * 1000UL)  target = normal;      // normal tier
    else                                              target = idleDeep;    // deep idle-dim
    gDimmed = gAllowIdleDim && (idle >= (uint32_t)IDLE_DIM_SEC * 1000UL);   // only the Clock deep-dims -> tap-to-wake there
    if (cur < target) cur++;
    else if (cur > target) cur--;
    M5.Display.setBrightness((uint8_t)cur);
  }
}

// ===================== Compass (IMU magnetometer heading) =====================
namespace Compass {
  static bool     gHasMag     = false;
  static bool     gCalibrated = false;
  static float    gHeading    = -1.0f;
  static bool     gCalActive  = false;
  static uint32_t gCalEnd     = 0;
  static const uint32_t CAL_MS = 15000;

  void begin() {
    if (!M5.Imu.isEnabled()) return;
    // _imu_instance[1] holds the magnetometer; it stays null on boards without one.
    gHasMag = (M5.Imu.getImuInstancePtr(1) != nullptr);
    if (!gHasMag) return;
    M5.Imu.loadOffsetFromNVS();                  // restore saved hard-iron offsets, if any
    gCalibrated = Settings::magCalibrated();
  }

  void startCal() {
    if (!gHasMag) return;
    M5.Imu.setCalibration(0, 0, 128);            // mag-only auto-cal: track min/max while moving
    gCalActive = true;
    gCalEnd = millis() + CAL_MS;
  }

  static void finishCal() {
    M5.Imu.setCalibration(0, 0, 0);              // freeze offsets
    M5.Imu.saveOffsetToNVS();
    Settings::setMagCalibrated(true);
    gCalibrated = true;
    gCalActive  = false;
  }

  bool available()      { return gHasMag; }
  bool calibrated()     { return gCalibrated; }
  bool calibrating()    { return gCalActive; }
  int  calSecondsLeft() { return gCalActive ? (int)((gCalEnd - millis() + 999) / 1000) : 0; }
  float heading()       { return gHasMag ? gHeading : -1.0f; }

  void loop() {
    if (!gHasMag) return;
    M5.Imu.update();                             // the calibration runs inside update() when active
    if (gCalActive && (int32_t)(millis() - gCalEnd) >= 0) finishCal();

    float ax, ay, az, mx, my, mz;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return;
    if (!M5.Imu.getMag(&mx, &my, &mz))   return;

    // Tilt-compensated heading of the device's +Y axis (top edge), done with vectors so
    // it doesn't depend on the accel sign convention: up = accel direction; project the
    // (calibrated) mag and the forward/right axes onto the horizontal plane; the heading
    // is the bearing of "forward" relative to horizontal magnetic north.
    float an = sqrtf(ax * ax + ay * ay + az * az);
    if (an < 1e-3f) return;
    float ux = ax / an, uy = ay / an, uz = az / an;

    float md = mx * ux + my * uy + mz * uz;                       // mag along up
    float hx = mx - md * ux, hy = my - md * uy, hz = mz - md * uz; // horizontal mag -> magnetic N

    float fwd = uy;                                               // forward = (0,1,0)
    float fx = -fwd * ux, fy = 1 - fwd * uy, fz = -fwd * uz;
    float rgt = ux;                                               // right = (1,0,0)
    float rx = 1 - rgt * ux, ry = -rgt * uy, rz = -rgt * uz;

    float hf = hx * fx + hy * fy + hz * fz;                       // north component along forward
    float hr = hx * rx + hy * ry + hz * rz;                       // north component along right
    if (fabsf(hf) < 1e-6f && fabsf(hr) < 1e-6f) return;

    float hdg = degrees(atan2f(-hr, hf));                         // 0=N, 90=E, clockwise
    hdg += HEADING_OFFSET_DEG + MAG_DECLINATION_DEG;              // axis fix + magnetic->true
    while (hdg < 0)    hdg += 360;
    while (hdg >= 360) hdg -= 360;

    if (gHeading < 0) gHeading = hdg;                             // first reading
    else {                                                        // smooth the needle a touch
      float d = hdg - gHeading;
      while (d > 180)  d -= 360;
      while (d < -180) d += 360;
      gHeading += d * 0.2f;
      if (gHeading < 0) gHeading += 360; else if (gHeading >= 360) gHeading -= 360;
    }
  }
}

// ===================== Notify =====================
namespace Notify {
  static uint32_t dotUntil = 0;
  static uint32_t muteUntil = 0;

  void begin() { M5.Speaker.begin(); M5.Speaker.setVolume(200); }
  void loop()  {}

  void mute(uint32_t ms) { muteUntil = millis() + ms; }
  void unmute() { muteUntil = millis(); }                        // expire it now
  bool muted() { return (int32_t)(muteUntil - millis()) > 0; }   // wrap-safe

  void gentle(const String& title) {                  // ambient: silent at night or while muted
    (void)title;
    dotUntil = millis() + 2500;
    if (!Dim::isNight() && !muted()) M5.Speaker.tone(880, 120);   // soft chirp
  }

  void alert(const String& title) {                   // friend ping: audible unless muted
    (void)title;
    dotUntil = millis() + 4000;
    if (!muted()) M5.Speaker.tone(1568, 220);          // bright tone, heard from any screen
  }

  void draw() {
    if (millis() > dotUntil) return;
    int x = M5.Display.width() - 18, y = 18;
    M5.Display.fillCircle(x, y, 8, ORANGE);
    M5.Display.fillCircle(x, y, 5, RED);
  }
}

// ===================== Geo (shared location resolver) =====================
namespace Geo {
  static bool   haveLoc = false;
  static float  gLat = 0, gLon = 0;
  static String gCity = "";

  // ZIP -> lat/lon via Zippopotam.us (free, plain HTTP, no key; US ZIPs).
  static bool geocodeZip(const String& zip) {
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    String url = "http://api.zippopotam.us/us/" + zip;
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    bool ok = false;
    if (code == 200) {
      String body = http.getString();
      JsonDocument doc;
      if (!deserializeJson(doc, body)) {
        JsonObject p = doc["places"][0];
        if (!p.isNull()) {
          gLat  = atof((const char*)(p["latitude"]  | "0"));
          gLon  = atof((const char*)(p["longitude"] | "0"));
          gCity = String((const char*)(p["place name"] | ""));
          haveLoc = true;
          ok = true;
        }
      }
    }
    Serial.printf("[geo] zip %s http=%d ok=%d\n", zip.c_str(), code, ok);
    http.end();
    return ok;
  }

  // IP geolocation via ip-api.com (free, plain HTTP, no key).
  static bool geolocate() {
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    if (!http.begin(client, "http://ip-api.com/json/?fields=status,lat,lon,city")) return false;
    int code = http.GET();
    bool ok = false;
    if (code == 200) {
      String body = http.getString();   // decodes chunked transfer correctly
      JsonDocument doc;
      if (!deserializeJson(doc, body) && doc["status"] == "success") {
        gLat  = doc["lat"].as<float>();
        gLon  = doc["lon"].as<float>();
        gCity = String((const char*)(doc["city"] | ""));
        haveLoc = true;
        ok = true;
      }
    }
    Serial.printf("[geo] geolocate http=%d ok=%d\n", code, ok);
    http.end();
    return ok;
  }

  // Resolve once and cache. On failure, out params get the config.h fallback.
  bool resolve(float& lat, float& lon, String& city) {
    if (!haveLoc) {
      String zip = Settings::zip();
      bool ok = false;
      if (zip.length()) ok = geocodeZip(zip);
      if (!ok) geolocate();
    }
    if (haveLoc) { lat = gLat; lon = gLon; city = gCity; return true; }
    lat = atof(WEATHER_LAT); lon = atof(WEATHER_LON); city = WEATHER_CITY;
    return false;
  }
}

// ===================== Weather (Open-Meteo) =====================
namespace Weather {
  // Readings shared with the UI, owned by the background updater task. [0] = home location when no
  // weather cities are configured; otherwise one entry per configured city (chosen in Settings).
  static const uint32_t   UPDATE_MS  = 60000;   // refresh cadence: every minute
  static SemaphoreHandle_t mtx        = nullptr;
  static TaskHandle_t      taskH      = nullptr;
  static Reading           gReadings[MAX_WEATHER_CITIES];
  static volatile int      gN         = 0;       // active readings (1 = home, else # cities)
  static volatile bool     gUpdating  = false;
  static volatile uint32_t gVersion   = 0;

  // A configured weather location: the raw "city or ZIP" the user typed, resolved on-device to
  // lat/lon + a tidy display name the first time it's fetched (then cached for the session).
  struct WCity { String query; String label; float lat = 0, lon = 0; bool resolved = false; };
  static WCity gCities[MAX_WEATHER_CITIES];
  static int   gNCities = 0;

  static void reloadCities() {                   // parse the saved blob: [{"q":"<city or zip>"},...]
    gNCities = 0;
    String j = Settings::weatherCitiesBlob();
    JsonDocument doc;
    if (j.length() && !deserializeJson(doc, j)) {
      for (JsonObject o : doc.as<JsonArray>()) {
        if (gNCities >= MAX_WEATHER_CITIES) break;
        String q = String((const char*)(o["q"] | ""));
        q.trim();
        if (!q.length()) continue;
        gCities[gNCities] = WCity();
        gCities[gNCities].query = q;
        gCities[gNCities].label = q;             // shown until geocoding resolves a nicer name
        gNCities++;
      }
    }
    for (int i = 0; i < gNCities; i++) { gReadings[i] = Reading(); gReadings[i].city = gCities[i].label; }
    gN = gNCities;                                // 0 -> home mode (the task sets gN=1 after fetching home)
  }

  static String wxUrlEnc(const String& s) {      // minimal percent-encoding for the geocoder query
    String o; char b[4];
    for (size_t i = 0; i < s.length(); i++) {
      char c = s[i];
      if (isalnum((int)(uint8_t)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
      else { sprintf(b, "%%%02X", (uint8_t)c); o += b; }
    }
    return o;
  }

  // Resolve a free-typed "city or ZIP" to lat/lon + a display name. A US 5-digit ZIP goes through
  // Zippopotam; anything else is a name search via Open-Meteo's geocoder. Plain HTTP (no TLS stalls);
  // an ArduinoJson filter bounds memory (the geocoder reply carries a big postcodes[] we don't need).
  static bool geocodeQuery(const String& q0, float& lat, float& lon, String& name) {
    if (!Net::online()) return false;
    String q = q0; q.trim();
    // Optional "City, CC" / "City, Country" suffix disambiguates same-named cities (e.g. "Kochi, IN"
    // -> India, not Japan). Without it we pick the most-populous match, which is right far more often
    // than the geocoder's default first hit (its #1 for "Kochi" is the smaller Japanese one).
    String want, nameq = q;
    int comma = q.lastIndexOf(',');
    if (comma >= 0) { nameq = q.substring(0, comma); nameq.trim(); want = q.substring(comma + 1); want.trim(); want.toUpperCase(); }

    bool isZip = (nameq.length() == 5);
    for (int i = 0; isZip && i < 5; i++) if (!isdigit((int)(uint8_t)nameq[i])) isZip = false;

    WiFiClient client; HTTPClient http;
    http.setConnectTimeout(6000); http.setTimeout(6000);
    String url = isZip ? ("http://api.zippopotam.us/us/" + nameq)
                       : ("http://geocoding-api.open-meteo.com/v1/search?count=5&language=en&format=json&name=" + wxUrlEnc(nameq));
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    bool ok = false;
    if (code == 200) {
      JsonDocument filter;
      filter["results"][0]["name"] = true;        // Open-Meteo geocoder shape (filter[0] -> every element)
      filter["results"][0]["latitude"] = true;
      filter["results"][0]["longitude"] = true;
      filter["results"][0]["country_code"] = true;
      filter["results"][0]["country"] = true;
      filter["results"][0]["population"] = true;
      filter["places"][0]["place name"] = true;   // Zippopotam shape
      filter["places"][0]["latitude"] = true;
      filter["places"][0]["longitude"] = true;
      JsonDocument doc;
      if (!deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter))) {
        if (isZip) {
          JsonObject p = doc["places"][0];
          if (!p.isNull()) {
            lat  = atof((const char*)(p["latitude"]  | "0"));
            lon  = atof((const char*)(p["longitude"] | "0"));
            name = String((const char*)(p["place name"] | ""));
            ok = (lat != 0 || lon != 0);
          }
        } else {
          JsonArray rs = doc["results"];
          JsonObject pick;
          if (!rs.isNull() && rs.size()) {
            if (want.length()) {                    // a country was given -> first result in that country
              for (JsonObject r : rs) {
                String cc = String((const char*)(r["country_code"] | "")); cc.toUpperCase();
                String cn = String((const char*)(r["country"]      | "")); cn.toUpperCase();
                if (cc == want || cn == want) { pick = r; break; }
              }
            }
            if (pick.isNull()) {                    // else the most-populous candidate (fallback: first)
              long best = -1;
              for (JsonObject r : rs) { long pop = r["population"] | 0L; if (pop > best) { best = pop; pick = r; } }
              if (pick.isNull()) pick = rs[0];
            }
            lat  = pick["latitude"]  | 0.0f;
            lon  = pick["longitude"] | 0.0f;
            name = String((const char*)(pick["name"] | ""));
            ok = true;
          }
        }
      }
    }
    Serial.printf("[weather] geocode '%s' http=%d ok=%d\n", q.c_str(), code, ok);
    http.end();
    return ok;
  }

  // Fetch current conditions for one lat/lon into `out`. No Geo lookup (caller supplies coords).
  // weekday (0=Sun..6=Sat) from an ISO "YYYY-MM-DD" date via Sakamoto's algorithm (tz-correct, no clock needed)
  static int8_t wdayFromISO(const char* s) {
    if (!s || strlen(s) < 10) return -1;
    int y = atoi(s), m = atoi(s + 5), d = atoi(s + 8);
    if (m < 1 || m > 12) return -1;
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (int8_t)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
  }

  static bool fetchInto(float lat, float lon, const String& cityLabel, Reading& out) {
    if (!Net::online()) return false;
    String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4)
               + "&longitude=" + String(lon, 4)
               + "&current=temperature_2m,weather_code,wind_speed_10m"
               + "&daily=temperature_2m_max,temperature_2m_min,weather_code&timezone=auto&forecast_days=7";
    if (Settings::tempF())             // runtime unit (Settings); metric is Open-Meteo's default (C, km/h)
      url += "&temperature_unit=fahrenheit&wind_speed_unit=mph";
    WiFiClient client;                 // plain HTTP — Open-Meteo serves it, avoids TLS stalls
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    bool ok = false;
    if (code == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getString())) {
        JsonObject cur = doc["current"];
        if (!cur.isNull()) {
          out.temp = cur["temperature_2m"] | 0.0f;
          out.wind = cur["wind_speed_10m"] | 0.0f;
          out.code = cur["weather_code"]   | -1;
          out.city = cityLabel;
          out.ok = true; ok = true;
        }
        JsonObject day = doc["daily"];           // 7-day forecast (local tz); [0] == today
        if (!day.isNull()) {
          JsonArray hi = day["temperature_2m_max"], lo = day["temperature_2m_min"],
                    wc = day["weather_code"],       tm = day["time"];
          if (!hi.isNull() && hi.size() && !lo.isNull() && lo.size()) {
            out.tmax = hi[0] | 0.0f; out.tmin = lo[0] | 0.0f; out.hasDay = true;
            int n = hi.size(); if (n > FC_DAYS) n = FC_DAYS;
            out.nDays = n;
            for (int i = 0; i < n; i++) {
              out.fcMax[i]  = hi[i] | 0.0f;
              out.fcMin[i]  = (i < (int)lo.size()) ? (lo[i] | 0.0f) : 0.0f;
              out.fcCode[i] = (!wc.isNull() && i < (int)wc.size()) ? (wc[i] | -1) : -1;
              const char* ds = (!tm.isNull() && i < (int)tm.size()) ? (tm[i] | "") : "";
              out.fcWday[i] = wdayFromISO(ds);
            }
          }
        }
      }
    }
    Serial.printf("[weather] %s http=%d ok=%d\n", cityLabel.c_str(), code, ok);
    http.end();
    return ok;
  }

  static void store(int i, const Reading& r) {   // stamp fetch time + publish reading i
    int h, m, s; ClockService::getTime(h, m, s);
    xSemaphoreTake(mtx, portMAX_DELAY);
    gReadings[i] = r; gReadings[i].atHour = h; gReadings[i].atMin = m;
    gVersion++;
    xSemaphoreGive(mtx);
  }

  const char* describe(int c) {
    switch (c) {
      case 0:  return "Clear";
      case 1:  return "Mainly clear";
      case 2:  return "Partly cloudy";
      case 3:  return "Overcast";
      case 45: case 48: return "Fog";
      case 51: case 53: case 55: return "Drizzle";
      case 56: case 57: return "Freezing drizzle";
      case 61: case 63: case 65: return "Rain";
      case 66: case 67: return "Freezing rain";
      case 71: case 73: case 75: return "Snow";
      case 77: return "Snow grains";
      case 80: case 81: case 82: return "Rain showers";
      case 85: case 86: return "Snow showers";
      case 95: return "Thunderstorm";
      case 96: case 99: return "Thunderstorm, hail";
      default: return "--";
    }
  }

  // ---- background updater (runs on core 0, never blocks the UI) ----
  static void updaterTask(void*) {
    for (;;) {
      if (Net::online()) {
        gUpdating = true;
        if (gNCities > 0) {                          // multi-city: geocode (once) then fetch each
          for (int i = 0; i < gNCities; i++) {
            if (!gCities[i].resolved) {
              float la, lo; String nm;
              if (geocodeQuery(gCities[i].query, la, lo, nm)) {
                gCities[i].lat = la; gCities[i].lon = lo;
                if (nm.length()) { if (nm.length() > 16) nm = nm.substring(0, 16); gCities[i].label = nm; }
                gCities[i].resolved = true;
              } else continue;                       // couldn't resolve -> leave it, retry next cycle
            }
            Reading r;
            if (fetchInto(gCities[i].lat, gCities[i].lon, gCities[i].label, r)) store(i, r);
          }
        } else {                                     // home: saved ZIP / IP / config fallback
          float lat, lon; String city; Geo::resolve(lat, lon, city);
          Reading r;
          if (fetchInto(lat, lon, city, r)) { store(0, r); gN = 1; }
        }
        gUpdating = false;
      }
      // sleep until the next cycle, or wake early on refreshNow(); poll faster until Wi-Fi is up.
      uint32_t wait = Net::online() ? UPDATE_MS : 3000;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
    }
  }

  void begin() {
    if (taskH) return;
    mtx = xSemaphoreCreateMutex();
    reloadCities();
    xTaskCreatePinnedToCore(updaterTask, "weather", 16384, nullptr, 1, &taskH, 0);
  }
  void suspend() { if (taskH) vTaskSuspend(taskH); }   // paused during an OTA flash (frees heap/TLS)
  void resume()  { if (taskH) vTaskResume(taskH); }

  int count() { return gN; }
  bool get(int i, Reading& out) {                      // copy reading i; out.ok = has data yet
    if (i < 0 || i >= gN) return false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    out = gReadings[i];
    xSemaphoreGive(mtx);
    return true;
  }
  bool latest(Reading& out) {                          // home / first reading, only if it has data
    xSemaphoreTake(mtx, portMAX_DELAY);
    bool ok = (gN > 0 && gReadings[0].ok);
    if (ok) out = gReadings[0];
    xSemaphoreGive(mtx);
    return ok;
  }

  bool updating()      { return gUpdating; }
  uint32_t version()   { return gVersion; }
  void refreshNow()    { if (taskH) xTaskNotifyGive(taskH); }
}

// ===================== Flight (adsb.lol live + adsbdb routes) =====================
namespace Flight {
  static const int        FLIGHT_SCAN = 48;     // max aircraft parsed per fetch (for sorting)
  static const int        FLIGHT_KEEP = 12;     // nearest kept for the UI
  static const int        ROUTE_CACHE = 10;     // callsign->route LRU entries
  static const uint32_t   UPDATE_MS   = 15000;  // planes move fast: refresh ~15s
  static volatile int      gFetchNm   = (int)FLIGHT_RADIUS_NM;   // fetch radius (nm); follows the radar zoom-out
  // airport-centered radar: when set, fetch planes around a chosen airport instead of your location
  static volatile bool     gCenterOverride = false;
  static float             gCenterLat = 0, gCenterLon = 0;
  static String            gAptReq = "", gAptCode = "", gAptName = "";   // pending lookup / resolved code+name (mtx-guarded)
  static volatile bool     gAptOk = false, gAptFail = false;
  static float             gAptYouDist = 0, gAptYouBearing = 0;          // your position relative to the airport
  static SemaphoreHandle_t mtx        = nullptr;
  static TaskHandle_t      taskH      = nullptr;

  static Plane             gPlanes[FLIGHT_KEEP];
  static int               gCount     = 0;
  static Airport           gAirports[MAX_AIRPORTS];   // nearby airports, fetched once
  static int               gAirportN  = 0;
  static bool              gAirportsDone = false;
  static volatile bool     gUpdating  = false;
  static volatile uint32_t gVersion   = 0;

  // Actively-tracked specific flight (from search).
  static String            gTrackCs   = "";
  static Plane             gTracked;
  static volatile bool     gTrackedOk = false;
  // Airports near the tracked plane (re-fetched as it moves; dist/bearing kept relative to it).
  static Airport           gPlaneAirports[MAX_AIRPORTS];
  static int               gPlaneAirportN = 0;
  static float             gPlaneApLat = 0, gPlaneApLon = 0;   // location the list was fetched for
  static String            gPlaneApCs  = "";                   // callsign the list belongs to

  // Route cache (ring buffer).
  static RouteInfo         gRoutes[ROUTE_CACHE];
  static String            gRouteCs[ROUTE_CACHE];
  static int               gRouteHead = 0;
  static String            gPendingRoute = "";

  // Great-circle distance (nm) + bearing (deg) from our location to a point.
  // great-circle distance (nm) + initial bearing (deg) from an arbitrary origin to a point
  static void relFrom(float oLat, float oLon, float plat, float plon, float& distNm, float& brg) {
    float la1 = radians(oLat), la2 = radians(plat);
    float dLa = radians(plat - oLat), dLo = radians(plon - oLon);
    float a = sinf(dLa/2)*sinf(dLa/2) + cosf(la1)*cosf(la2)*sinf(dLo/2)*sinf(dLo/2);
    distNm = 3440.065f * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
    float y = sinf(dLo) * cosf(la2);
    float x = cosf(la1)*sinf(la2) - sinf(la1)*cosf(la2)*cosf(dLo);
    brg = fmodf(degrees(atan2f(y, x)) + 360.0f, 360.0f);
  }
  static void relTo(float plat, float plon, float& distNm, float& brg) {   // relative to YOUR location
    float lat, lon; String c; Geo::resolve(lat, lon, c);
    relFrom(lat, lon, plat, plon, distNm, brg);
  }

  static void buildPlaneFilter(JsonDocument& filter) {
    const char* keys[] = {"hex","flight","t","alt_baro","gs","track","baro_rate","lat","lon","dst","dir"};
    for (auto k : keys) filter["ac"][0][k] = true;
  }

  static void parsePlane(JsonObject p, Plane& pl, bool haveRel) {
    pl.flight = String((const char*)(p["flight"] | "")); pl.flight.trim();
    if (!pl.flight.length()) pl.flight = "(no id)";
    pl.hex   = String((const char*)(p["hex"] | "")); pl.hex.trim(); pl.hex.toLowerCase();
    pl.type  = String((const char*)(p["t"] | ""));
    pl.gs    = p["gs"]        | 0.0f;
    pl.track = p["track"]     | 0.0f;
    pl.vrate = p["baro_rate"] | 0.0f;
    pl.lat   = p["lat"]       | 0.0f;
    pl.lon   = p["lon"]       | 0.0f;
    JsonVariant a = p["alt_baro"];                    // number, or "ground"
    pl.alt = a.is<int>() ? a.as<int>() : (a.is<float>() ? (int)a.as<float>() : 0);
    if (haveRel) { pl.dist = p["dst"] | 0.0f; pl.bearing = p["dir"] | 0.0f; }
    else         { relTo(pl.lat, pl.lon, pl.dist, pl.bearing); }   // callsign feed has no dst/dir
  }

  static int fetchNearby(Plane* out, int maxN) {
    if (!Net::online()) return -1;
    float lat, lon;
    if (gCenterOverride) { lat = gCenterLat; lon = gCenterLon; }   // airport-centered: fetch around the airport
    else { String city; Geo::resolve(lat, lon, city); }
    String url = "http://api.adsb.lol/v2/lat/" + String(lat, 4) + "/lon/" + String(lon, 4)
               + "/dist/" + String(gFetchNm);                  // dynamic: widens when the radar is zoomed out
    WiFiClient client; HTTPClient http;
    http.setConnectTimeout(8000); http.setTimeout(8000);
    if (!http.begin(client, url)) return -1;
    int code = http.GET(), n = -1;
    if (code == 200) {
      String body = http.getString();
      JsonDocument filter; buildPlaneFilter(filter);
      JsonDocument doc;
      if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        n = 0;
        for (JsonObject p : doc["ac"].as<JsonArray>()) {
          if (n >= maxN) break;
          Plane pl;
          parsePlane(p, pl, true);
          if (pl.alt <= 0) continue;     // skip on-ground / unknown-altitude aircraft
          out[n++] = pl;
        }
      }
    }
    Serial.printf("[flight] nearby http=%d n=%d\n", code, n);
    http.end();
    return n;
  }

  static bool fetchCallsign(const String& cs, Plane& out) {
    if (!Net::online()) return false;
    String url = "http://api.adsb.lol/v2/callsign/" + cs;
    WiFiClient client; HTTPClient http;
    http.setConnectTimeout(8000); http.setTimeout(8000);
    if (!http.begin(client, url)) return false;
    int code = http.GET(); bool ok = false;
    if (code == 200) {
      String body = http.getString();
      JsonDocument filter; buildPlaneFilter(filter);
      JsonDocument doc;
      if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        JsonArray ac = doc["ac"].as<JsonArray>();
        if (ac.size() > 0) { parsePlane(ac[0], out, false); ok = true; }
      }
    }
    Serial.printf("[flight] callsign %s http=%d ok=%d\n", cs.c_str(), code, ok);
    http.end();
    return ok;
  }

  // ---- Route lookup via OpenSky Network (free, OAuth2) ----
  // OpenSky derives an aircraft's actual departure/arrival from observed takeoffs/landings,
  // keyed by ICAO24 hex. It only has COMPLETED flights, so for an en-route plane this shows
  // its most recent finished leg, not the live destination. Needs OPENSKY_CLIENT_ID/SECRET.
  static String   gOsToken = "";
  static uint32_t gOsTokenExp = 0;   // millis() when the cached token should be refreshed

  static String dispAirport(const String& icao) {        // KMDW -> MDW (US); others left as ICAO
    if (icao.length() == 4 && icao[0] == 'K') return icao.substring(1);
    return icao;
  }

  static String hexForCallsign(const String& cs) {        // resolve callsign -> ICAO24 from live data
    String hex = "";
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < gCount; i++) if (gPlanes[i].flight == cs) { hex = gPlanes[i].hex; break; }
    if (!hex.length() && gTracked.flight == cs) hex = gTracked.hex;
    xSemaphoreGive(mtx);
    return hex;
  }

  static bool osEnsureToken() {
    if (gOsToken.length() && (int32_t)(gOsTokenExp - millis()) > 0) return true;   // still valid
    if (strlen(OPENSKY_CLIENT_ID) == 0) return false;
    WiFiClientSecure tls; tls.setInsecure(); tls.setHandshakeTimeout(8);
    HTTPClient http; http.setConnectTimeout(8000); http.setTimeout(10000);
    if (!http.begin(tls, "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token"))
      return false;
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String body = String("grant_type=client_credentials&client_id=") + OPENSKY_CLIENT_ID
                + "&client_secret=" + OPENSKY_CLIENT_SECRET;
    int code = http.POST(body); bool ok = false;
    if (code == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getString())) {
        gOsToken = String((const char*)(doc["access_token"] | ""));
        long exp = doc["expires_in"] | 0;
        gOsTokenExp = millis() + (uint32_t)(exp > 90 ? exp - 60 : 60) * 1000UL;   // refresh ~1 min early
        ok = gOsToken.length() > 0;
      }
    }
    Serial.printf("[flight] opensky token http=%d ok=%d\n", code, ok);
    http.end();
    return ok;
  }

  static bool fetchRoute(const String& cs, RouteInfo& out) {
    out.ok = false; out.confirmed = false;
    if (!Net::online() || strlen(OPENSKY_CLIENT_ID) == 0) return true;   // not configured -> unknown
    String hex = hexForCallsign(cs);
    if (!hex.length()) return true;                                     // unknown aircraft hex
    if (!osEnsureToken()) return true;
    time_t now = time(nullptr);
    if (now < 1700000000) return true;                                  // clock not NTP-synced yet
    // OpenSky rejects a window spanning >2 day-partitions (HTTP 400). time() is UTC epoch, so align
    // the start to 00:00 UTC yesterday -> the window touches exactly 2 UTC days (24-48h of history).
    long begin = ((long)now - (long)now % 86400) - 86400;
    String url = "https://opensky-network.org/api/flights/aircraft?icao24=" + hex
               + "&begin=" + String(begin) + "&end=" + String((long)now);
    WiFiClientSecure tls; tls.setInsecure(); tls.setHandshakeTimeout(8);
    HTTPClient http; http.setConnectTimeout(8000); http.setTimeout(12000);
    if (!http.begin(tls, url)) return true;
    http.addHeader("Authorization", "Bearer " + gOsToken);
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      JsonDocument filter;
      filter[0]["estDepartureAirport"] = true;
      filter[0]["estArrivalAirport"]   = true;
      filter[0]["lastSeen"]            = true;
      JsonDocument doc;
      if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        long bestSeen = -1; String dep, arr;
        for (JsonVariant f : doc.as<JsonArray>()) {        // most recent flight that has airport data
          String d = String((const char*)(f["estDepartureAirport"] | ""));
          String a = String((const char*)(f["estArrivalAirport"]   | ""));
          long ls = f["lastSeen"] | 0;
          if ((d.length() || a.length()) && ls >= bestSeen) { bestSeen = ls; dep = d; arr = a; }
        }
        if (dep.length() || arr.length()) {
          out.origIata = dispAirport(dep);
          out.destIata = dispAirport(arr);
          out.ok = true;
        }
      }
    } else if (code == 401 || code == 403) {
      gOsToken = "";                                                    // force token refresh next time
    }
    Serial.printf("[flight] opensky %s hex=%s http=%d ok=%d\n", cs.c_str(), hex.c_str(), code, out.ok);
    http.end();
    return true;
  }

  // Airports (with IATA codes) within FLIGHT_RADIUS_NM of (oLat,oLon) from the OpenStreetMap Overpass
  // API (free, no key) into out[], nearest first, dist/bearing relative to that origin. Returns the
  // count, or -1 on HTTP failure. Does NO locking — operates on the caller's buffer.
  static int fetchAirportsAt(float oLat, float oLon, Airport* out, int maxOut) {
    if (!Net::online()) return -1;
    String q = String("[out:json][timeout:20];nwr[\"aeroway\"=\"aerodrome\"][\"iata\"](around:")
             + (int)(gFetchNm * 1852) + "," + String(oLat, 5) + "," + String(oLon, 5)
             + ");out center tags 40;";
    WiFiClient client; HTTPClient http;
    http.setConnectTimeout(8000); http.setTimeout(12000);
    if (!http.begin(client, "http://overpass-api.de/api/interpreter")) return -1;
    http.addHeader("Content-Type", "text/plain");
    int code = http.POST(q), n = 0, keep = 0;
    if (code == 200) {
      String body = http.getString();
      JsonDocument filter;                              // bound memory: keep only what we plot
      filter["elements"][0]["lat"] = true;
      filter["elements"][0]["lon"] = true;
      filter["elements"][0]["center"]["lat"] = true;
      filter["elements"][0]["center"]["lon"] = true;
      filter["elements"][0]["tags"]["iata"] = true;
      JsonDocument doc;
      if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        Airport tmp[40];
        for (JsonObject e : doc["elements"].as<JsonArray>()) {
          if (n >= 40) break;
          float alat = e["lat"] | (e["center"]["lat"] | 0.0f);   // node has lat/lon; way/rel has center
          float alon = e["lon"] | (e["center"]["lon"] | 0.0f);
          String iata = String((const char*)(e["tags"]["iata"] | ""));
          if (!iata.length() || (alat == 0 && alon == 0)) continue;
          tmp[n].code = iata; tmp[n].lat = alat; tmp[n].lon = alon;
          relFrom(oLat, oLon, alat, alon, tmp[n].dist, tmp[n].bearing);
          n++;
        }
        std::sort(tmp, tmp + n, [](const Airport& a, const Airport& b){ return a.dist < b.dist; });
        keep = n < maxOut ? n : maxOut;
        for (int i = 0; i < keep; i++) out[i] = tmp[i];
      }
    }
    Serial.printf("[flight] airports@%.2f,%.2f http=%d n=%d\n", oLat, oLon, code, n);
    http.end();
    return code == 200 ? keep : -1;
  }

  // Resolve a single airport CODE (3-letter IATA or 4-letter ICAO) -> lat/lon + name via Overpass.
  static bool lookupAirport(const String& code, float& lat, float& lon, String& name) {
    if (!Net::online()) return false;
    String c = code; c.trim(); c.toUpperCase();
    // Match the code against IATA *or* ICAO (don't rely on length); Overpass is rate-limited/slow, so retry.
    String q = String("[out:json][timeout:25];(nwr[\"aeroway\"=\"aerodrome\"][\"iata\"=\"") + c
             + "\"];nwr[\"aeroway\"=\"aerodrome\"][\"icao\"=\"" + c + "\"];);out center tags 5;";
    for (int attempt = 0; attempt < 3; attempt++) {
      if (attempt) vTaskDelay(pdMS_TO_TICKS(1500));
      WiFiClient client; HTTPClient http;
      http.setConnectTimeout(8000); http.setTimeout(15000);
      if (!http.begin(client, "http://overpass-api.de/api/interpreter")) continue;
      http.addHeader("Content-Type", "text/plain");
      int code_ = http.POST(q); bool got = false;
      if (code_ == 200) {
        JsonDocument filter;
        filter["elements"][0]["lat"] = true;
        filter["elements"][0]["lon"] = true;
        filter["elements"][0]["center"]["lat"] = true;
        filter["elements"][0]["center"]["lon"] = true;
        filter["elements"][0]["tags"]["name"] = true;
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter))) {
          for (JsonObject e : doc["elements"].as<JsonArray>()) {     // first element that actually has coords
            float la = e["lat"] | (e["center"]["lat"] | 0.0f);
            float lo = e["lon"] | (e["center"]["lon"] | 0.0f);
            if (la != 0 || lo != 0) { lat = la; lon = lo; name = String((const char*)(e["tags"]["name"] | "")); got = true; break; }
          }
        }
      }
      Serial.printf("[flight] lookup airport %s try=%d http=%d ok=%d\n", c.c_str(), attempt, code_, got);
      http.end();
      if (got) return true;
    }
    return false;
  }

  // Airports near YOU, fetched once (airports don't move); radar reference markers.
  static bool fetchAirports() {
    float lat, lon; String city; Geo::resolve(lat, lon, city);
    Airport tmp[MAX_AIRPORTS];
    int k = fetchAirportsAt(lat, lon, tmp, MAX_AIRPORTS);
    if (k < 0) return false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < k; i++) gAirports[i] = tmp[i];
    gAirportN = k; gVersion++;
    xSemaphoreGive(mtx);
    return true;
  }

  static void updaterTask(void*) {
    Plane tmp[FLIGHT_SCAN];
    for (;;) {
      if (Net::online()) {
        gUpdating = true;

        // pending airport lookup -> set the radar's fetch center (fetchNearby below then uses it)
        String aptReq; xSemaphoreTake(mtx, portMAX_DELAY); aptReq = gAptReq; xSemaphoreGive(mtx);
        if (aptReq.length()) {
          float alat, alon; String aname; bool ok = lookupAirport(aptReq, alat, alon, aname);
          float yd = 0, yb = 0;
          if (ok) { float ylat, ylon; String yc; Geo::resolve(ylat, ylon, yc); relFrom(alat, alon, ylat, ylon, yd, yb); }
          xSemaphoreTake(mtx, portMAX_DELAY);
          if (ok) {
            gCenterLat = alat; gCenterLon = alon; gCenterOverride = true;
            gAptCode = aptReq; gAptName = aname; gAptYouDist = yd; gAptYouBearing = yb;
            gAptOk = true; gAptFail = false;
          } else { gAptFail = true; gAptOk = false; }
          gAptReq = ""; gVersion++;
          xSemaphoreGive(mtx);
        }

        int n = fetchNearby(tmp, FLIGHT_SCAN);
        if (n >= 0) {
          std::sort(tmp, tmp + n, [](const Plane& a, const Plane& b){ return a.dist < b.dist; });
          int keep = n < FLIGHT_KEEP ? n : FLIGHT_KEEP;
          xSemaphoreTake(mtx, portMAX_DELAY);
          for (int i = 0; i < keep; i++) gPlanes[i] = tmp[i];
          gCount = keep; gVersion++;
          xSemaphoreGive(mtx);
        }

        String cs;                                   // tracked specific flight
        xSemaphoreTake(mtx, portMAX_DELAY); cs = gTrackCs; xSemaphoreGive(mtx);
        if (cs.length()) {
          Plane tp; bool ok = fetchCallsign(cs, tp);
          xSemaphoreTake(mtx, portMAX_DELAY);
          if (ok) gTracked = tp;
          gTrackedOk = ok; gVersion++;
          xSemaphoreGive(mtx);

          if (ok) {   // keep a fresh airport set around the tracked plane (plane-centered radar)
            bool newTrack = (gPlaneApCs != cs);
            float moved = 1e9f, b;
            if (!newTrack) relFrom(gPlaneApLat, gPlaneApLon, tp.lat, tp.lon, moved, b);
            if (newTrack || gPlaneAirportN == 0 || moved > 25.0f) {
              Airport tmp[MAX_AIRPORTS];
              int k = fetchAirportsAt(tp.lat, tp.lon, tmp, MAX_AIRPORTS);
              xSemaphoreTake(mtx, portMAX_DELAY);
              if (k >= 0) {
                for (int i = 0; i < k; i++) gPlaneAirports[i] = tmp[i];
                gPlaneAirportN = k; gPlaneApLat = tp.lat; gPlaneApLon = tp.lon; gPlaneApCs = cs;
              } else if (newTrack) { gPlaneAirportN = 0; gPlaneApCs = cs; }  // drop the prior plane's list
              xSemaphoreGive(mtx);
            }
            xSemaphoreTake(mtx, portMAX_DELAY);       // re-point markers at the live plane position
            for (int i = 0; i < gPlaneAirportN; i++)
              relFrom(tp.lat, tp.lon, gPlaneAirports[i].lat, gPlaneAirports[i].lon,
                      gPlaneAirports[i].dist, gPlaneAirports[i].bearing);
            gVersion++;
            xSemaphoreGive(mtx);
          }
        }

        String pr;                                   // pending route lookup
        xSemaphoreTake(mtx, portMAX_DELAY); pr = gPendingRoute; xSemaphoreGive(mtx);
        if (pr.length()) {
          RouteInfo ri; fetchRoute(pr, ri);          // cache even on miss (negative cache)
          xSemaphoreTake(mtx, portMAX_DELAY);
          gRouteCs[gRouteHead] = pr; gRoutes[gRouteHead] = ri;
          gRouteHead = (gRouteHead + 1) % ROUTE_CACHE;
          if (gPendingRoute == pr) gPendingRoute = "";
          gVersion++;
          xSemaphoreGive(mtx);
        }

        if (!gAirportsDone && fetchAirports()) gAirportsDone = true;   // nearby airports, once

        gUpdating = false;
      }
      uint32_t wait = Net::online() ? UPDATE_MS : 3000;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
    }
  }

  void begin() {
    if (taskH) return;
    mtx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(updaterTask, "flight", 20480, nullptr, 1, &taskH, 0);  // larger: TLS
  }
  void suspend() { if (taskH) vTaskSuspend(taskH); }   // paused during an OTA flash (frees heap/TLS)
  void resume()  { if (taskH) vTaskResume(taskH); }

  int snapshot(Plane* out, int maxN) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    int n = gCount < maxN ? gCount : maxN;
    for (int i = 0; i < n; i++) out[i] = gPlanes[i];
    xSemaphoreGive(mtx);
    return n;
  }

  int airports(Airport* out, int maxN) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    int n = gAirportN < maxN ? gAirportN : maxN;
    for (int i = 0; i < n; i++) out[i] = gAirports[i];
    xSemaphoreGive(mtx);
    return n;
  }

  int planeAirports(Airport* out, int maxN) {          // airports near the tracked plane
    xSemaphoreTake(mtx, portMAX_DELAY);
    int n = gPlaneAirportN < maxN ? gPlaneAirportN : maxN;
    for (int i = 0; i < n; i++) out[i] = gPlaneAirports[i];
    xSemaphoreGive(mtx);
    return n;
  }

  // Common airline IATA -> ICAO prefixes (extend as needed).
  struct Airline { const char* iata; const char* icao; };
  static const Airline AIRLINES[] = {
    {"AA","AAL"},{"AC","ACA"},{"AD","AZU"},{"AF","AFR"},{"AH","DAH"},{"AI","AIC"},{"AM","AMX"},
    {"AR","ARG"},{"AS","ASA"},{"AT","RAM"},{"AV","AVA"},{"AY","FIN"},{"AZ","ITY"},{"A3","AEE"},
    {"BA","BAW"},{"BG","BBC"},{"BR","EVA"},{"BT","BTI"},{"B6","JBU"},
    {"CA","CCA"},{"CI","CAL"},{"CM","CMP"},{"CX","CPA"},{"CZ","CSN"},
    {"DL","DAL"},{"DY","NAX"},{"D8","IBK"},
    {"EI","EIN"},{"EK","UAE"},{"ET","ETH"},{"EW","EWG"},{"EY","ETD"},
    {"FM","CSH"},{"FR","RYR"},{"FZ","FDB"},{"F9","FFT"},
    {"GA","GIA"},{"GF","GFA"},{"G3","GLO"},
    {"HA","HAL"},{"HO","DKH"},{"HU","CHH"},{"HV","TRA"},{"HX","CRK"},
    {"IB","IBE"},
    {"JL","JAL"},{"JQ","JST"},{"JU","ASL"},
    {"KE","KAL"},{"KL","KLM"},{"KQ","KQA"},{"KU","KAC"},
    {"LA","LAN"},{"LH","DLH"},{"LO","LOT"},{"LX","SWR"},{"LY","ELY"},
    {"ME","MEA"},{"MF","CXA"},{"MH","MAS"},{"MS","MSR"},{"MU","CES"},
    {"NH","ANA"},{"NK","NKS"},{"NZ","ANZ"},
    {"OA","OAL"},{"OS","AUA"},{"OU","CTN"},{"OZ","AAR"},
    {"PC","PGT"},{"PK","PIA"},{"PR","PAL"},{"PS","AUI"},
    {"QF","QFA"},{"QR","QTR"},
    {"RJ","RJA"},{"RO","ROT"},
    {"SA","SAA"},{"SK","SAS"},{"SN","BEL"},{"SQ","SIA"},{"SU","AFL"},{"SV","SVA"},{"SG","SEJ"},
    {"TG","THA"},{"TK","THY"},{"TO","TVF"},{"TP","TAP"},{"TU","TAR"},
    {"UA","UAL"},{"UK","VTI"},{"UL","ALK"},{"UX","AEA"},{"U2","EZY"},
    {"VA","VOZ"},{"VN","HVN"},{"VS","VIR"},{"VY","VLG"},
    {"WN","SWA"},{"WS","WJA"},{"WY","OMA"},{"W6","WZZ"},
    {"3U","CSC"},{"5J","CEB"},{"6E","IGO"},{"9W","JAI"},
  };
  static const int AIRLINE_N = sizeof(AIRLINES) / sizeof(AIRLINES[0]);

  String toCallsign(const String& in) {
    String s = in; s.trim(); s.toUpperCase();
    if (s.length() >= 3) {
      String rest = s.substring(2);                 // IATA = 2-char code + all digits
      bool digits = rest.length() > 0;
      for (unsigned i = 0; i < rest.length(); i++) if (!isDigit(rest[i])) { digits = false; break; }
      if (digits) {
        String pre = s.substring(0, 2);
        for (int i = 0; i < AIRLINE_N; i++)
          if (pre == AIRLINES[i].iata) return String(AIRLINES[i].icao) + rest;
      }
    }
    return s;   // already ICAO (3-letter prefix) or unknown airline -> use as typed
  }

  void track(const String& callsign) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (gTrackCs != callsign) { gTrackCs = callsign; gTrackedOk = false; }
    xSemaphoreGive(mtx);
    refreshNow();
  }

  bool tracked(Plane& out) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    bool ok = gTrackedOk;
    if (ok) out = gTracked;
    xSemaphoreGive(mtx);
    return ok;
  }

  bool route(const String& callsign, RouteInfo& out) {
    bool found = false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < ROUTE_CACHE; i++)
      if (gRouteCs[i] == callsign) { out = gRoutes[i]; found = true; break; }
    if (!found && gPendingRoute != callsign) gPendingRoute = callsign;
    xSemaphoreGive(mtx);
    if (!found) refreshNow();
    return found;
  }

  bool updating()    { return gUpdating; }
  uint32_t version() { return gVersion; }
  void refreshNow()  { if (taskH) xTaskNotifyGive(taskH); }
  void setRange(int nm) { gFetchNm = constrain(nm, (int)FLIGHT_RADIUS_NM, 150); }   // never below default; capped

  void searchAirport(const String& code) {        // UI: center the radar on this airport (async lookup on the task)
    xSemaphoreTake(mtx, portMAX_DELAY); gAptReq = code; gAptCode = ""; gAptName = ""; xSemaphoreGive(mtx);
    gAptOk = false; gAptFail = false;
    if (taskH) xTaskNotifyGive(taskH);
  }
  void clearCenter() {                            // UI: back to your-location radar
    xSemaphoreTake(mtx, portMAX_DELAY); gAptReq = ""; gAptCode = ""; gAptName = ""; xSemaphoreGive(mtx);
    gCenterOverride = false; gAptOk = false; gAptFail = false;
    if (taskH) xTaskNotifyGive(taskH);            // refetch at your location
  }
  bool airportCenter(String& code, String& name, float& youDist, float& youBearing) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    bool ok = gAptOk; code = gAptCode; name = gAptName; youDist = gAptYouDist; youBearing = gAptYouBearing;
    xSemaphoreGive(mtx);
    return ok;
  }
  bool airportFailed() { return gAptFail; }
}

// ===================== Broker (MQTT) =====================
namespace Broker {
#if MQTT_TLS
  static WiFiClientSecure netClient;   // verifies the broker against MQTT_CA_CERT (CA-pinned)
#else
  static WiFiClient netClient;
#endif
  static PubSubClient client(netClient);
  static void (*userCb)(const String&, const String&) = nullptr;
  static const int MAX_SUBS = 5;          // friends inbox + OTA broadcast + fleet ota/channel (+ headroom)
  static String    subs[MAX_SUBS];
  static int       nSubs = 0;
  static TaskHandle_t taskH = nullptr;
  static volatile bool gUp     = false;   // connection state; flips ownership of `client` task<->UI
  static volatile bool needSub = false;   // re-apply the subscription after a (re)connect
  static String gWillTopic = "", gWillMsg = "";   // Last Will (fleet/online presence); applied at connect
  static bool   gWillRetain = false;

  static bool enabled() { return strlen(MQTT_HOST) > 0; }

  static void rawCb(char* topic, byte* payload, unsigned int len) {
    String t(topic);
    String p; p.reserve(len);
    for (unsigned int i = 0; i < len; i++) p += (char)payload[i];
    if (userCb) userCb(t, p);          // runs from client.loop() on the UI thread
  }

  static bool enrollConfigured() { return strlen(ENROLL_URL) > 0 && strlen(ENROLL_TOKEN) > 0; }

  // Zero-touch provisioning: a fresh puck has no MQTT password. Generate a random one, register it
  // with the droplet's enroll endpoint over HTTPS (CA-pinned to MQTT_CA_CERT, bearer = ENROLL_TOKEN),
  // and store it in NVS so every future connect just uses it. One success ends enrollment forever
  // (mqttPass() is non-empty afterwards). Runs on the connect task, off the UI thread.
  static bool enrollOnce() {
    char pw[33];                                   // 32 hex chars from the HW RNG (radio on -> true entropy)
    for (int i = 0; i < 4; i++) snprintf(pw + i * 8, 9, "%08x", (unsigned)esp_random());
    pw[32] = '\0';
    String code = Friends::myCode();
    String body = String("{\"code\":\"") + code + "\",\"pw\":\"" + pw + "\"}";
    WiFiClientSecure tls; tls.setCACert(MQTT_CA_CERT); tls.setHandshakeTimeout(8);   // NEVER setInsecure
    HTTPClient http; http.setConnectTimeout(8000); http.setTimeout(10000);
    if (!http.begin(tls, ENROLL_URL)) { log_e("[enroll] http begin failed"); return false; }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + ENROLL_TOKEN);
    int rc = http.POST(body);
    http.end();
    if (rc == 200 || rc == 201) {
      Settings::saveMqttPass(String(pw));
      Serial.printf("[enroll] provisioned code=%s (rc=%d)\n", code.c_str(), rc);
      return true;
    }
    log_e("[enroll] failed rc=%d", rc);             // USB-visible (Serial.printf only reaches UART0)
    Serial.printf("[enroll] failed code=%s rc=%d\n", code.c_str(), rc);
    return false;
  }

  // The TLS connect can block for seconds; doing it on the UI loop froze touch. It now runs on
  // its own task, OFF the UI thread. `client` is used by exactly one thread at a time, gated by
  // gUp with no lock: the task touches it ONLY while disconnected (gUp false); the UI touches it
  // ONLY while connected (gUp true). Each side flips gUp to hand off, so they never overlap.
  static void connectTask(void*) {
    for (;;) {
      if (enabled() && Net::online() && !gUp) {
        String pass = Settings::mqttPass();
        if (pass.length() == 0 && enrollConfigured()) {
          // Fresh puck, no password yet: self-provision one over HTTPS (TLS needs a synced clock).
          // On success NVS holds a password and the next cycle (3s) connects normally.
          if (ClockService::synced()) enrollOnce();
        } else {
          // client id is unique per chip; username = friend code (for broker %u ACLs); password is
          // the per-device secret from NVS (self-enrolled or typed in Settings, never compiled in).
          String cid  = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
          String user = Friends::myCode();
          Serial.printf("[mqtt] connecting as %s (pwlen=%d)\n", user.c_str(), pass.length());
          bool ok = gWillTopic.length()
            ? client.connect(cid.c_str(), user.c_str(), pass.c_str(),                 // LWT: broker marks us
                             gWillTopic.c_str(), 0, gWillRetain, gWillMsg.c_str())     //  offline on an ungraceful drop
            : client.connect(cid.c_str(), user.c_str(), pass.c_str());
          if (ok) {
            needSub = true; gUp = true;
            Serial.println("[mqtt] connected");
          } else {
            Serial.printf("[mqtt] connect failed (state=%d)\n", client.state());
          }
        }
      }
      vTaskDelay(pdMS_TO_TICKS(3000));   // retry cadence, off the UI thread
    }
  }

  void begin() {
    if (!enabled()) return;
#if MQTT_TLS
    netClient.setCACert(MQTT_CA_CERT);   // CA-pin: reject any broker cert not chaining to this root
    netClient.setHandshakeTimeout(8);    // bound a TLS stall on the connect task
#endif
    client.setServer(MQTT_HOST, MQTT_PORT);
    client.setBufferSize(512);           // headroom over the 256B default for friend payloads
    client.setCallback(rawCb);
    xTaskCreatePinnedToCore(connectTask, "mqtt", 16384, nullptr, 1, &taskH, 0);  // big stack for TLS
  }

  bool configured() { return enabled(); }

  void onMessage(void (*cb)(const String&, const String&)) { userCb = cb; }

  void loop() {   // UI thread: only the cheap, non-blocking client work, and only while connected
    if (!enabled() || !gUp) return;
    if (needSub) { needSub = false; for (int i = 0; i < nSubs; i++) client.subscribe(subs[i].c_str()); }
    client.loop();
    if (!client.connected()) gUp = false;   // dropped -> hand back to the connect task
  }

  bool connected() { return gUp; }

  void publish(const String& topic, const String& payload, bool retain) {
    if (gUp) client.publish(topic.c_str(), payload.c_str(), retain);   // UI thread only
  }
  void subscribe(const String& topic) {   // register a topic; (re)applied by loop() after (re)connect
    for (int i = 0; i < nSubs; i++) if (subs[i] == topic) return;   // already registered
    if (nSubs < MAX_SUBS) subs[nSubs++] = topic;
    needSub = true;                        // re-apply on next loop; no-op until connected
  }
  void setWill(const String& topic, const String& payload, bool retain) {   // applied on the next connect
    gWillTopic = topic; gWillMsg = payload; gWillRetain = retain;
  }
  void suspend() { if (taskH) vTaskSuspend(taskH); }   // paused during an OTA flash (quiesce MQTT)
  void resume()  { if (taskH) vTaskResume(taskH); }
}

// ===================== Friends (social: mutual-approval emoji) =====================
namespace Friends {
  static Friend   gList[MAX_FRIENDS];
  static int      gCount = 0;
  static String   gCode = "";
  static volatile uint32_t gVersion = 0;
  static String   gInCode = "", gInName = "", gInEmote = "";   // one-shot incoming-emote latch (sender code/name + emote)

  static int findIdx(const String& code) {
    for (int i = 0; i < gCount; i++) if (gList[i].code == code) return i;
    return -1;
  }

  static void saveList() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < gCount; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["c"] = gList[i].code; o["n"] = gList[i].name; o["s"] = gList[i].status;
      if (gList[i].nick.length()) o["k"] = gList[i].nick;   // local alias (only stored if set)
    }
    String out; serializeJson(doc, out);
    Settings::saveFriendsBlob(out);
  }

  static void loadList() {
    gCount = 0;
    String j = Settings::friendsBlob();
    if (!j.length()) return;
    JsonDocument doc;
    if (deserializeJson(doc, j)) return;
    for (JsonObject o : doc.as<JsonArray>()) {
      if (gCount >= MAX_FRIENDS) break;
      String c = String((const char*)(o["c"] | ""));
      if (!c.length()) continue;
      gList[gCount].code   = c;
      gList[gCount].name   = String((const char*)(o["n"] | ""));
      gList[gCount].nick   = String((const char*)(o["k"] | ""));
      gList[gCount].status = (uint8_t)(o["s"] | (int)CONFIRMED);
      gCount++;
    }
  }

  static int upsert(const String& code, const String& name, uint8_t status) {
    int i = findIdx(code);
    if (i < 0) {
      if (gCount >= MAX_FRIENDS) return -1;
      i = gCount++;
      gList[i].code = code;
    }
    if (name.length()) gList[i].name = name;
    gList[i].status = status;
    return i;
  }

  static void erase(const String& code) {
    int i = findIdx(code);
    if (i < 0) return;
    for (int k = i; k < gCount - 1; k++) gList[k] = gList[k + 1];
    gCount--;
  }

  String myCode() { return gCode; }
  String myName() { String n = Settings::displayName(); return n.length() ? n : gCode; }
  void setName(const String& name) {
    String n = name; n.trim(); if (n.length() > 16) n = n.substring(0, 16);
    Settings::saveDisplayName(n);
    gVersion++;
  }
  void setNick(const String& code, const String& nick) {     // local alias; never published
    int i = findIdx(code); if (i < 0) return;
    String n = nick; n.trim(); if (n.length() > 16) n = n.substring(0, 16);
    gList[i].nick = n; saveList(); gVersion++;
  }
  String labelFor(const String& code) {                      // what to show: nick -> their name -> code
    int i = findIdx(code); if (i < 0) return code;
    if (gList[i].nick.length()) return gList[i].nick;
    if (gList[i].name.length()) return gList[i].name;
    return code;
  }

  // ---- MQTT helpers: topic = puck/<dst>/<type>/<me> ----
  static void pub(const String& dst, const char* type, const String& payload, bool retain) {
    Broker::publish("puck/" + dst + "/" + type + "/" + gCode, payload, retain);
  }
  static void clearRetained(const String& topic) { Broker::publish(topic, "", true); }

  void begin() {
    uint32_t low = (uint32_t)ESP.getEfuseMac();        // stable, unique per chip
    char buf[9]; snprintf(buf, sizeof(buf), "%08X", low);
    gCode = String(buf);
    loadList();
    Broker::subscribe("puck/" + gCode + "/#");         // single wildcard inbox
  }

  void loop() { /* Broker re-applies our one subscription on reconnect; nothing else to do */ }

  int count() { int n = 0; for (int i = 0; i < gCount; i++) if (gList[i].status == CONFIRMED) n++; return n; }
  int get(Friend* out, int maxN) {
    int n = 0;
    for (int i = 0; i < gCount && n < maxN; i++) if (gList[i].status == CONFIRMED) out[n++] = gList[i];
    return n;
  }
  int pending(Friend* out, int maxN) {
    int n = 0;
    for (int i = 0; i < gCount && n < maxN; i++) if (gList[i].status == RECEIVED) out[n++] = gList[i];
    return n;
  }
  bool known(const String& code) { return code == gCode || findIdx(code) >= 0; }

  bool addFriend(const String& codeIn) {
    String code = codeIn; code.trim(); code.toUpperCase();
    if (!code.length() || code == gCode) return false;
    int i = findIdx(code);
    if (i >= 0 && gList[i].status == RECEIVED) return approve(code);   // both added -> confirm now
    if (i >= 0) return true;                                           // SENT/CONFIRMED -> idempotent
    if (gCount >= MAX_FRIENDS) return false;
    upsert(code, "", SENT);
    pub(code, "req", myName(), true);                                  // retained -> offline delivery
    saveList(); gVersion++;
    return true;
  }

  bool approve(const String& code) {
    int i = findIdx(code);
    if (i < 0 || gList[i].status != RECEIVED) return false;
    gList[i].status = CONFIRMED;
    pub(code, "ok", myName(), true);
    clearRetained("puck/" + gCode + "/req/" + code);   // wipe the retained request from the broker
    saveList(); gVersion++;
    return true;
  }

  bool deny(const String& code) {
    int i = findIdx(code);
    if (i < 0) return false;
    pub(code, "no", myName(), true);
    if (gList[i].status == RECEIVED) clearRetained("puck/" + gCode + "/req/" + code);
    erase(code); saveList(); gVersion++;
    return true;
  }

  bool removeFriend(const String& code) {
    int i = findIdx(code);
    if (i < 0) return false;
    pub(code, "no", myName(), true);
    erase(code); saveList(); gVersion++;
    return true;
  }

  bool sendEmoteTo(const String& code, const String& emote) {
    int i = findIdx(code);
    if (i < 0 || gList[i].status != CONFIRMED) return false;
    pub(code, "emo", emote, false);
    return true;
  }
  bool sendEmote(const String& emote) {
    bool any = false;
    for (int i = 0; i < gCount; i++) if (gList[i].status == CONFIRMED) {
      pub(gList[i].code, "emo", emote, false); any = true;
    }
    return any;
  }

  uint32_t version() { return gVersion; }

  bool takeIncomingEmote(String& fromCode, String& fromName, String& emote) {
    if (!gInEmote.length()) return false;
    fromCode = gInCode; fromName = gInName; emote = gInEmote;
    gInCode = ""; gInName = ""; gInEmote = "";
    return true;
  }

  // Inbound: topic = puck/<myCode>/<type>/<src>. Returns true if it's one of ours.
  bool handleSocial(const String& topic, const String& payload) {
    if (!topic.startsWith("puck/" + gCode + "/")) return false;   // not addressed to us
    int a = topic.indexOf('/');                                   // puck |
    int b = topic.indexOf('/', a + 1);                            // <me> |
    int c = topic.indexOf('/', b + 1);                            // <type> |
    if (b < 0 || c < 0) return true;                              // malformed but ours -> consume
    String type = topic.substring(b + 1, c);
    String src  = topic.substring(c + 1);
    if (!src.length() || src == gCode) return true;
    if (!payload.length()) return true;                           // cleared retainer -> ignore

    if (type == "req") {                                          // someone wants to add us
      int i = findIdx(src);
      if (i >= 0 && gList[i].status == SENT) {                    // we'd already asked them -> auto-confirm
        gList[i].status = CONFIRMED; gList[i].name = payload;
        pub(src, "ok", myName(), true);
        clearRetained(topic);
        saveList(); gVersion++; Notify::gentle("friend added");
      } else if (i < 0) {                                         // new pending request
        if (upsert(src, payload, RECEIVED) >= 0) { saveList(); gVersion++; Notify::alert("friend request"); }
      } else {                                                    // already known -> refresh name
        gList[i].name = payload; gVersion++;
        if (gList[i].status == CONFIRMED) clearRetained(topic);
      }
    } else if (type == "ok") {                                    // they approved our request
      int i = findIdx(src);
      if (i >= 0 && gList[i].status != CONFIRMED) {
        gList[i].status = CONFIRMED; gList[i].name = payload;
        saveList(); gVersion++; Notify::gentle("friend added");
      }
      clearRetained(topic);
    } else if (type == "no") {                                    // denied or unfriended
      if (findIdx(src) >= 0) { erase(src); saveList(); gVersion++; }
      clearRetained(topic);
    } else if (type == "emo") {                                   // a ping
      int i = findIdx(src);
      if (i >= 0 && gList[i].status == CONFIRMED) {               // only from confirmed friends
        gInCode = src;
        gInName = gList[i].nick.length() ? gList[i].nick : (gList[i].name.length() ? gList[i].name : src);
        gInEmote = payload;
        gVersion++; Notify::alert("ping");
      }                                                           // non-friend -> drop silently
    }
    return true;
  }
}

// ===================== Ota (over-the-air firmware updates) =====================
namespace Ota {
  static TaskHandle_t      taskH      = nullptr;
  static SemaphoreHandle_t mtx        = nullptr;     // guards the String fields below
  static volatile Phase    gPhase     = IDLE;
  static volatile int      gAvailVer  = 0;
  static volatile int      gPercent   = 0;
  static volatile uint32_t gVersion   = 0;           // redraw counter
  static volatile bool     gWantCheck = false;
  static volatile bool     gManual    = false;   // pending check is user-initiated (bypasses the snooze)
  static volatile bool     gWantFlash = false;
  static String            gNotes     = "";          // mtx-guarded
  static String            gBinUrl    = "";          // mtx-guarded
  static String            gError     = "";          // mtx-guarded
  static uint32_t          gNextPoll  = 0;
  static bool              gBootDone  = false;
  static volatile uint32_t gSnoozeUntil = 0;       // "Later" snooze (epoch); RAM-only -> cleared on reboot
  static volatile bool     gWantVersions = false;  // on-device picker asked for the version list
  static const int         MAX_VERS   = 24;
  static int               gVerList[MAX_VERS];      // mtx-guarded; newest first
  static volatile int      gVerCount  = 0;
  static volatile bool     gVerReady  = false;
  static volatile bool     gBeta      = false;       // this device is in the server's test list -> sees RC versions

  bool configured() { return strlen(OTA_MANIFEST_URL) > 0; }

  static void bump() { gVersion++; }
  static void setPhase(Phase p) { gPhase = p; bump(); }
  static void setError(const String& e) { xSemaphoreTake(mtx, portMAX_DELAY); gError = e; xSemaphoreGive(mtx); }

  Phase    phase()            { return gPhase; }
  bool     updateAvailable()  { return gPhase == AVAILABLE; }
  int      availableVersion() { return gAvailVer; }
  int      percent()          { return gPercent; }
  uint32_t version()          { return gVersion; }
  String   releaseNotes()     { String s; xSemaphoreTake(mtx, portMAX_DELAY); s = gNotes; xSemaphoreGive(mtx); return s; }
  String   lastError()        { String s; xSemaphoreTake(mtx, portMAX_DELAY); s = gError; xSemaphoreGive(mtx); return s; }

  static bool snoozed() {                          // true while a "Later" snooze is still in effect
    time_t now = time(nullptr);
    return now > 1700000000 && (uint32_t)now < gSnoozeUntil;
  }
  void checkNow(bool manual) { gManual = manual; gWantCheck = true; if (taskH) xTaskNotifyGive(taskH); }
  void confirmUpdate() { if (gPhase == AVAILABLE) { gWantFlash = true; if (taskH) xTaskNotifyGive(taskH); } }
  void dismiss() {                                 // "Later": snooze auto-prompts for a day — RAM-only,
    if (gPhase == AVAILABLE) {                      //   so any reboot (incl. after a firmware upgrade) clears it
      time_t now = time(nullptr);
      if (now > 1700000000) gSnoozeUntil = (uint32_t)now + 24UL * 3600UL;
    }
    if (gPhase == AVAILABLE || gPhase == FAILED) setPhase(IDLE);
  }

  // Install a SPECIFIC version (operator push / on-device picker). Builds the URL from the compiled
  // base + version, so only a *published* binary can be selected (never an arbitrary URL). Downgrades
  // allowed (no newer-than-current gate). force -> flash silently; else -> pop the Update/Later overlay.
  void pushVersion(int v, bool force) {
    if (v <= 0 || gPhase == FLASHING || gPhase == DONE_REBOOTING) return;
    String url = String(OTA_BIN_BASE) + v + ".bin";
    xSemaphoreTake(mtx, portMAX_DELAY); gBinUrl = url; gNotes = String("version ") + v; xSemaphoreGive(mtx);
    gAvailVer = v; setError("");
    if (force) { gWantFlash = true; if (taskH) xTaskNotifyGive(taskH); }   // straight to doFlash()
    else       { setPhase(AVAILABLE); }                                     // confirm overlay in main.cpp
  }

  void onFleetCmd(const String& payload) {       // MQTT fleet/ota/<code>: {"v":N,"force":0|1}
    JsonDocument d;
    if (deserializeJson(d, payload)) return;     // malformed -> ignore
    int v = d["v"] | 0; bool force = (int)(d["force"] | 0) != 0;
    if (v > 0) pushVersion(v, force);
  }

  // MQTT fleet/channel/<code> (retained, operator-set, authenticated): "test"/"beta" = RC channel, else prod.
  // Cached in NVS so the picker knows the channel even before MQTT reconnects — and device codes never hit a public URL.
  void onChannel(const String& payload) {
    bool b = payload.equalsIgnoreCase("test") || payload.equalsIgnoreCase("beta");
    if (b != gBeta) { gBeta = b; Settings::saveBeta(b); bump(); }
  }

  void requestVersions() { gVerReady = false; gWantVersions = true; if (taskH) xTaskNotifyGive(taskH); }
  bool versionsReady()   { return gVerReady; }
  bool isBeta()          { return gBeta; }   // device is in the server's test list (picker shows RC builds)
  int  versions(int* out, int maxN) {
    int n = 0; xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < gVerCount && n < maxN; i++) out[n++] = gVerList[i];
    xSemaphoreGive(mtx); return n;
  }

  // Fetch + parse the manifest; fills outputs and returns true on a valid read.
  static bool fetchManifest(int& outVer, String& outUrl, String& outNotes) {
    if (!Net::online() || !ClockService::synced()) return false;     // TLS needs a real clock
    WiFiClientSecure tls; tls.setCACert(MQTT_CA_CERT); tls.setHandshakeTimeout(15);
    HTTPClient http; http.setConnectTimeout(10000); http.setTimeout(12000);
    if (!http.begin(tls, OTA_MANIFEST_URL)) return false;
    int code = http.GET(); bool ok = false;
    if (code == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getString())) {
        outVer   = doc["version"] | 0;
        outUrl   = String((const char*)(doc["url"]   | ""));
        outNotes = String((const char*)(doc["notes"] | ""));
        ok = (outVer > 0 && outUrl.length());
      }
    }
    log_e("[ota] manifest http=%d ok=%d ver=%d cur=%d heap=%u", code, ok, outVer, FW_VERSION, (unsigned)ESP.getFreeHeap());
    http.end();
    return ok;
  }

  // Fetch the installable-version list (OTA_VERSIONS_URL: {"versions":[...]}) for the on-device picker.
  // GET a CA-pinned JSON. Returns true + fills `doc` on HTTP 200 + valid parse.
  static bool getJson(const char* url, JsonDocument& doc) {
    if (!strlen(url) || !Net::online() || !ClockService::synced()) return false;
    WiFiClientSecure tls; tls.setCACert(MQTT_CA_CERT); tls.setHandshakeTimeout(15);
    HTTPClient http; http.setConnectTimeout(10000); http.setTimeout(12000);
    if (!http.begin(tls, url)) return false;
    int code = http.GET();
    bool ok = (code == 200) && !deserializeJson(doc, http.getString());
    http.end();
    return ok;
  }

  static void fetchVersions() {
    // 1) channel: set from the authenticated, retained MQTT marker fleet/channel/<code> (cached in NVS via
    //    onChannel) — never a public URL, so device codes stay private. Default (no marker) = prod.
    bool beta = gBeta;

    // 2) version list: prod sees "released" (finals), beta sees all "versions"; both floored at "min".
    int tmp[MAX_VERS]; int n = 0;
    { JsonDocument doc;
      if (getJson(OTA_VERSIONS_URL, doc)) {
        int mn = doc["min"] | 1;
        JsonArray rel = doc["released"].as<JsonArray>();
        JsonArray use = (beta || rel.isNull()) ? doc["versions"].as<JsonArray>() : rel;  // fallback: all
        for (JsonVariant x : use) { int v = x | 0; if (v >= mn && n < MAX_VERS) tmp[n++] = v; }
      } }

    for (int i = 1; i < n; i++) { int k = tmp[i], j = i - 1;            // sort descending (newest first)
      while (j >= 0 && tmp[j] < k) { tmp[j + 1] = tmp[j]; j--; } tmp[j + 1] = k; }
    log_e("[ota] versions beta=%d n=%d", beta, n);
    xSemaphoreTake(mtx, portMAX_DELAY);
    gVerCount = n; for (int i = 0; i < n; i++) gVerList[i] = tmp[i];
    xSemaphoreGive(mtx);
    gVerReady = true; bump();   // gBeta is owned by onChannel()/begin() (MQTT marker) — don't write it here
  }

  static void suspendNet() { Weather::suspend(); Flight::suspend(); Broker::suspend(); }
  static void resumeNet()  { Broker::resume();  Flight::resume();  Weather::resume(); }

  // Download + flash the inactive slot. Runs only on the task, only after confirmUpdate().
  static void doFlash() {
    String url; xSemaphoreTake(mtx, portMAX_DELAY); url = gBinUrl; xSemaphoreGive(mtx);
    if (!url.length() || !Net::online() || !ClockService::synced()) { setError("not ready"); setPhase(FAILED); return; }
    suspendNet();                          // free heap + avoid concurrent TLS during the flash
    gPercent = 0; setPhase(FLASHING);
    WiFiClientSecure tls; tls.setCACert(MQTT_CA_CERT); tls.setHandshakeTimeout(8);  // NEVER setInsecure for OTA
    httpUpdate.rebootOnUpdate(false);      // we reboot ourselves so the UI can paint "rebooting"
    httpUpdate.onProgress([](int cur, int total){ gPercent = total ? (int)((int64_t)cur * 100 / total) : 0; bump(); });
    t_httpUpdate_return r = httpUpdate.update(tls, url);
    if (r == HTTP_UPDATE_OK) {
      setPhase(DONE_REBOOTING);
      delay(700);                          // let core-1 paint the "rebooting" frame
      ESP.restart();                       // boots the freshly-written slot
    } else {
      setError(httpUpdate.getLastErrorString());
      log_e("[ota] flash failed (%d): %s", (int)r, httpUpdate.getLastErrorString().c_str());
      resumeNet();                         // running firmware untouched -> bring services back
      setPhase(FAILED);
    }
  }

  static void otaTask(void*) {
    for (;;) {
      if (gWantFlash) {                     // user confirmed -> highest priority
        gWantFlash = false;
        doFlash();                          // returns only on failure (success reboots)
      } else if (gWantVersions) {           // on-device picker asked for the version list
        gWantVersions = false;
        fetchVersions();
      } else if (gWantCheck) {
        gWantCheck = false;
        bool manual = gManual; gManual = false;
        if (gPhase != FLASHING && gPhase != DONE_REBOOTING) {
          setPhase(CHECKING);
          if (!Net::online())               { setError("no Wi-Fi");      setPhase(IDLE); }
          else if (!ClockService::synced()) { setError("clock not set"); setPhase(IDLE); }
          else {
            int v = 0; String u, n; bool got = false;
            for (int a = 0; a < 3 && !got; a++) {           // TLS can fail transiently under heap pressure -> retry
              if (a) vTaskDelay(pdMS_TO_TICKS(1500));
              got = fetchManifest(v, u, n);
            }
            if (got) {
              if (v > FW_VERSION && (manual || !snoozed())) {
                xSemaphoreTake(mtx, portMAX_DELAY); gBinUrl = u; gNotes = n; xSemaphoreGive(mtx);
                gAvailVer = v; setError(""); setPhase(AVAILABLE);     // -> confirm overlay pops in main.cpp
              } else { setError(""); setPhase(IDLE); }                // up to date, or postponed by "Later"
            } else { setError("server unreachable"); setPhase(IDLE); }
          }
        }
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));      // wake on checkNow/confirm; else idle
    }
  }

  void begin() {
    if (taskH || !configured()) return;
    mtx = xSemaphoreCreateMutex();
    gBeta = Settings::beta();                                                // restore RC-channel flag (set via MQTT, cached in NVS)
    xTaskCreatePinnedToCore(otaTask, "ota", 24576, nullptr, 1, &taskH, 0);   // big stack: TLS + flash
    if (Broker::configured()) {
      Broker::subscribe(OTA_PUSH_TOPIC);                                     // fleet "recheck now" nudge
      Broker::subscribe("fleet/ota/" + Friends::myCode());                   // targeted version push to this device
      Broker::subscribe("fleet/channel/" + Friends::myCode());              // RC/prod channel marker (retained, operator-set)
    }
  }

  void loop() {   // core-1: schedule the boot + interval checks (cheap millis math; work is on the task)
    if (!configured() || !taskH) return;
    if (!gBootDone && Net::online() && ClockService::synced()) {            // one check once net+clock ready
      gBootDone = true;
      gNextPoll = millis() + (uint32_t)OTA_POLL_HOURS * 3600000UL;
      if (!snoozed()) checkNow();                                          // respect a "Later" snooze
    }
    if (OTA_POLL_HOURS && gBootDone && (int32_t)(millis() - gNextPoll) >= 0) {
      gNextPoll = millis() + (uint32_t)OTA_POLL_HOURS * 3600000UL;
      if (gPhase == IDLE && !snoozed()) checkNow();   // never interrupt an offer/flash; skip while snoozed
    }
  }
}

// ===================== Fleet (telemetry: firmware version + presence) =====================
namespace Fleet {
  static bool   gWasUp = false;
  static String gLastName = "\x01";          // sentinel so the first connect always publishes

  static void publishStatus() {              // retained -> the operator sees last-known even when offline
    JsonDocument d; d["v"] = FW_VERSION; d["n"] = Friends::myName();
    String s; serializeJson(d, s);
    Broker::publish("fleet/status/" + Friends::myCode(), s, true);
  }

  void begin() {
    if (!Broker::configured()) return;
    Broker::setWill("fleet/online/" + Friends::myCode(), "0", true);   // broker flips us offline on a drop
  }

  void loop() {
    if (!Broker::configured()) return;
    bool up = Broker::connected();
    if (up && !gWasUp) {                      // just (re)connected -> announce presence + version
      Broker::publish("fleet/online/" + Friends::myCode(), "1", true);
      publishStatus();
      gLastName = Friends::myName();
    } else if (up) {
      String nm = Friends::myName();
      if (nm != gLastName) { gLastName = nm; publishStatus(); }   // name changed -> refresh status
    }
    gWasUp = up;
  }
}
