#pragma once
#include <Arduino.h>

// Persisted user config (Wi-Fi creds + timezone), stored in flash (NVS).
// Values set on-device override the compile-time defaults in config.h.
namespace Settings {
  void begin();                       // open the store; call first in setup()
  bool haveWifi();                    // true if an SSID is configured (saved or compiled)
  String ssid();
  String pass();
  String tz();                        // POSIX TZ string
  String zip();                       // US ZIP for weather location ("" = use IP)
  void saveWifi(const String& ssid, const String& pass);
  void saveTz(const String& tz);
  void saveZip(const String& zip);
  bool magCalibrated();               // a figure-8 compass calibration has been saved
  void setMagCalibrated(bool v);
  String friendsBlob();               // friends list serialized as JSON ("" if none)
  void saveFriendsBlob(const String& json);
  String displayName();               // user-set name shown to friends ("" if unset)
  void saveDisplayName(const String& name);
  String emojiTarget();               // last emoji recipient: "*" = all friends, else a code
  void saveEmojiTarget(const String& codeOrAll);
  String mqttPass();                  // per-device MQTT password (NVS; falls back to config MQTT_PASS)
  void saveMqttPass(const String& pass);
  String worldClockBlob();            // world-clock cities as JSON ("" if none)
  void saveWorldClockBlob(const String& json);
  String weatherCitiesBlob();         // weather cities as JSON ("" = use home location)
  void saveWeatherCitiesBlob(const String& json);
  String searchHistory();             // recent flight/airport searches, comma-separated ("" if none)
  void saveSearchHistory(const String& csv);
  bool tempF();                       // true = Fahrenheit/mph, false = Celsius/km-h (default = WEATHER_UNITS_F)
  void saveTempF(bool f);
  bool clock12h();                    // true = 12-hour clock (AM/PM), false = 24-hour (default)
  void saveClock12h(bool v);
}

// On-device Wi-Fi setup: a SoftAP + captive portal. Pick network + timezone in a
// browser; creds are saved and the device restarts into normal mode.
namespace Provision {
  void start();
  void loop();        // service the portal; self-restarts the device after a save
  void stop();
  bool active();
  String apName();    // the hotspot SSID to join, for on-screen instructions
}

namespace Net {
  void begin();
  void loop();
  bool online();
}

namespace ClockService {
  void begin();
  void loop();
  void getTime(int& hour, int& minute, int& second);
  bool synced();
}

// World clock: up to MAX_WORLD_CITIES extra cities (POSIX TZ strings, chosen in the captive portal),
// rendered by ClockApp under the local time. timeFor() briefly switches the global TZ on the UI thread
// guarded by a mutex shared with ClockService::getTime (the Weather task also reads local time).
namespace WorldClock {
  struct City { String label; String tz; };
  void reload();                          // re-parse the saved blob (call when ClockApp opens)
  int  count();                           // number of configured cities (0..MAX_WORLD_CITIES)
  int  get(City* out, int maxN);          // copy configured cities -> out[]; returns count
  bool timeFor(const City& c, int& hour, int& minute);  // city wall-clock now; false if clock unset
}

namespace Dim {
  void begin();
  void loop();
  void wake();          // call on any interaction
  bool dimmed();        // true when the screen has dimmed below the active level (tap should just wake it)
  void allowIdleDim(bool allow);  // gate auto-dim to the Clock/launcher only (active apps stay lit)
  bool isNight();
}

// Tilt-compensated compass heading from the IMU's magnetometer (CoreS3 units that have
// one; auto-detected). Drives the Flight radar's heading-up rotation. On a board with no
// magnetometer available() is false and the radar stays north-up.
namespace Compass {
  void begin();          // detect the magnetometer, restore any saved calibration
  void loop();           // pump the IMU each frame; runs the calibration when active
  bool available();      // a usable magnetometer was found
  bool calibrated();     // a figure-8 calibration has been saved
  float heading();       // 0..360 deg (0=N, 90=E); -1 if unavailable
  void startCal();       // begin a timed figure-8 calibration
  bool calibrating();    // true while a calibration is running
  int  calSecondsLeft(); // countdown shown during calibration
}

namespace Notify {
  void begin();
  void loop();
  void gentle(const String& title);   // soft beep (muted at night) + on-screen dot
  void alert(const String& title);    // beep (ignores night, respects mute) + dot; for friend pings
  void draw();                        // overlay the dot (called each frame, on every screen)
  void mute(uint32_t ms);             // silence beeps for a while (e.g. 1 hour)
  void unmute();                      // cancel an active mute
  bool muted();                       // true while a mute window is active
}

// Shared location resolver: saved ZIP -> IP geolocation -> WEATHER_* fallback.
// Blocks on first call (HTTP); cached afterwards. Call from a background task.
namespace Geo {
  bool resolve(float& lat, float& lon, String& city);   // true if zip/IP used; false = fallback
}

// Live nearby aircraft via adsb.lol (no API key). A background task refreshes
// every ~15s; the UI reads the cached nearest-first list without blocking.
namespace Flight {
  struct Plane {
    String flight;       // callsign
    String hex;          // ICAO24 address (e.g. ac41ab) — keys the OpenSky route lookup
    String type;         // ICAO type code (e.g. B77W)
    float  dist    = 0;  // distance from you (nm)
    float  bearing = 0;  // bearing from you (deg)
    float  track   = 0;  // heading / direction of travel (deg)
    float  gs      = 0;  // ground speed (kt)
    float  vrate   = 0;  // climb/descend rate (ft/min)
    int    alt     = 0;  // altitude (ft); 0 = on ground/unknown
    float  lat     = 0;
    float  lon     = 0;
  };
  struct RouteInfo {     // origin/destination airports
    String origIata, origCity, destIata, destCity;
    bool   ok = false;        // a route was found (from at least one source)
    bool   confirmed = false; // two independent sources agree
  };
  struct Airport { String code; float lat = 0, lon = 0, dist = 0, bearing = 0; };  // nearby airport
  static const int MAX_AIRPORTS = 10;
  void begin();
  int  snapshot(Plane* out, int maxN);   // copy nearest planes into out[]; returns count
  int  airports(Airport* out, int maxN); // copy nearby airports into out[] (fetched once); returns count
  int  planeAirports(Airport* out, int maxN); // copy airports near the tracked plane (dist/bearing rel. to it)
  String toCallsign(const String& typed); // IATA flight no. (SQ31) -> ICAO callsign (SIA31)
  void track(const String& callsign);    // actively track this specific callsign ("" = stop)
  bool tracked(Plane& out);              // latest state of the tracked flight; false if not seen
  bool route(const String& callsign, RouteInfo& out);  // cached route; async-fetches if missing
  bool updating();
  uint32_t version();
  void refreshNow();
  void setRange(int nm);                  // fetch radius (nm); the radar widens it on zoom-out (clamped >= FLIGHT_RADIUS_NM)
  void searchAirport(const String& code); // center the radar on an airport (3-letter IATA / 4-letter ICAO); async
  void clearCenter();                     // back to your-location radar
  bool airportCenter(String& code, String& name, float& youDist, float& youBearing);  // resolved airport center + your offset
  bool airportFailed();                   // the last airport lookup failed (code not found)
  void suspend();                        // pause the background task (during an OTA flash)
  void resume();
}

// Live weather via Open-Meteo (no API key). A background task refreshes every
// minute; the UI reads the latest cached reading without ever blocking.
// Location priority: saved ZIP -> IP geolocation -> WEATHER_* config.h defaults.
namespace Weather {
  static const int FC_DAYS = 7;            // days of daily forecast kept per city
  struct Reading {
    bool   ok    = false;
    float  temp  = 0;     // in the configured unit (F or C)
    float  wind  = 0;     // mph or km/h
    float  tmax  = 0;     // today's high (configured unit); valid only when hasDay
    float  tmin  = 0;     // today's low
    bool   hasDay = false;// daily max/min were fetched
    int    code  = -1;    // WMO weather code
    String city  = "";
    int    atHour = -1;   // local time this reading was fetched
    int    atMin  = -1;
    int    nDays = 0;             // # of valid forecast days (0..FC_DAYS); [0] == today
    float  fcMax[FC_DAYS]  = {0}; // per-day high
    float  fcMin[FC_DAYS]  = {0}; // per-day low
    int    fcCode[FC_DAYS] = {0}; // per-day WMO code
    int8_t fcWday[FC_DAYS] = {0}; // per-day weekday (0=Sun..6=Sat) from the forecast date
  };
  void begin();                            // start the background updater task
  bool latest(Reading& out);               // home / first city reading; false if no data yet
  int  count();                            // active readings: 1 = home, else # of configured cities
  bool get(int i, Reading& out);           // copy reading i (0..count-1); out.ok = has data yet
  bool updating();                         // true while a refresh is in flight
  uint32_t version();                      // bumps on every successful update
  void refreshNow();                       // request an immediate refresh
  const char* describe(int wmoCode);       // short condition text
  void suspend();                          // pause the background task (during an OTA flash)
  void resume();
}

namespace Broker {
  void begin();
  void loop();
  bool connected();
  bool configured();    // true if MQTT_HOST is set (feature available, may not be connected yet)
  void publish(const String& topic, const String& payload, bool retain = false);
  void subscribe(const String& topic);
  void setWill(const String& topic, const String& payload, bool retain);  // LWT, applied on next connect
  void onMessage(void (*cb)(const String&, const String&));
  void suspend();       // pause the connect task (during an OTA flash)
  void resume();
}

// Fleet telemetry: publishes this device's firmware version + presence so an operator can see the
// fleet. Retained `fleet/status/<code>` {v,n} on connect; `fleet/online/<code>` 1 on connect / 0 via LWT.
namespace Fleet {
  void begin();   // set the LWT (call after Friends::begin + Broker::begin)
  void loop();    // pumped from the main loop: announce on (re)connect, refresh on name change
}

// Social layer: add friends by code with MUTUAL approval; only mutually-confirmed
// friends can exchange emojis (others are dropped). Always-on and owns the inbound
// MQTT routing (via handleSocial, called from main.cpp's mqttRouter) so requests and
// pings are handled even when no app — or a different app — is open. No-op when MQTT
// is not configured. Each device subscribes to one wildcard inbox: puck/<myCode>/#.
namespace Friends {
  enum Status : uint8_t { SENT = 1, RECEIVED = 2, CONFIRMED = 3 };  // outgoing / incoming-pending / mutual
  struct Friend { String code; String name; String nick; uint8_t status; };  // nick = local alias (never sent)
  static const int MAX_FRIENDS = 12;

  void begin();          // derive my code, restore the saved list, subscribe puck/<me>/#
  void loop();           // light housekeeping (Broker re-applies the subscription on reconnect)

  String myCode();                         // this device's shareable friend code
  String myName();                         // editable display name (defaults to myCode)
  void   setName(const String& name);
  void   setNick(const String& code, const String& nick);  // local alias for a friend ("" clears it)
  String labelFor(const String& code);     // display name: nick -> their name -> code

  int    count();                          // number of confirmed friends
  int    get(Friend* out, int maxN);       // copy confirmed friends -> out[]; returns count
  int    pending(Friend* out, int maxN);   // copy incoming (RECEIVED) requests -> out[]; returns count
  bool   addFriend(const String& code);    // start (or auto-complete) mutual approval
  bool   approve(const String& code);      // approve an incoming request -> confirmed
  bool   deny(const String& code);         // reject an incoming / cancel an outgoing request
  bool   removeFriend(const String& code); // drop a confirmed friend

  bool   sendEmote(const String& emote);                       // -> all confirmed friends
  bool   sendEmoteTo(const String& code, const String& emote); // -> one confirmed friend

  uint32_t version();                                          // bumps on any state change (lazy redraw)
  bool   takeIncomingEmote(String& fromCode, String& fromName, String& emote);  // one-shot pop of the latest ping
  bool   handleSocial(const String& topic, const String& payload);  // returns true if it consumed the message
}

// Over-the-air firmware updates. A core-0 task polls a JSON manifest over HTTPS (boot + every
// OTA_POLL_HOURS), is nudged by the OTA_PUSH_TOPIC MQTT broadcast or the Settings "Check for
// updates" button, and — only after the user confirms on-screen — flashes the inactive OTA slot
// and reboots. Never blocks the UI loop. No-op until Net::online() && ClockService::synced()
// (TLS needs a real clock) and only when OTA_MANIFEST_URL is set.
namespace Ota {
  enum Phase : uint8_t {
    IDLE = 0,        // nothing pending
    CHECKING,        // manifest fetch in flight
    AVAILABLE,       // a newer version was found, awaiting the user's confirm
    FLASHING,        // download + write in progress (see percent())
    DONE_REBOOTING,  // success; the device reboots into the new image momentarily
    FAILED           // the last flash attempt failed (see lastError())
  };
  void     begin();             // start the core-0 task (call last in setup())
  void     loop();              // core-1 housekeeping: schedules the boot + interval checks
  void     checkNow(bool manual = false);  // (re)check now; manual=true (Settings button) ignores the Later snooze
  void     confirmUpdate();     // user tapped Update: begin the flash
  void     dismiss();           // user tapped Later: clear the AVAILABLE offer
  Phase    phase();
  bool     updateAvailable();   // phase()==AVAILABLE
  int      availableVersion();  // manifest version (valid when AVAILABLE)
  String   releaseNotes();      // manifest "notes" ("" if none)
  int      percent();           // 0..100 during FLASHING
  String   lastError();         // reason after FAILED
  uint32_t version();           // bumps on any state change (lazy redraw)
  bool     configured();        // OTA_MANIFEST_URL is non-empty
  // Targeted / picked install of a specific version (OTA_BIN_BASE + v + ".bin"). Downgrades allowed.
  // force=false -> pops the Update/Later overlay; force=true -> flashes silently. No "newer-only" gate.
  void     pushVersion(int v, bool force);
  void     onFleetCmd(const String& payload);   // MQTT fleet/ota/<code> JSON {"v":N,"force":0|1}
  // On-device version picker: ask the task to fetch OTA_VERSIONS_URL, then read the result.
  void     requestVersions();
  bool     versionsReady();     // true once a fetch has populated the list (or failed -> empty)
  int      versions(int* out, int maxN);   // copy available versions (newest first); returns count
  bool     isBeta();            // device is in the server's test list -> the picker includes RC builds
}
