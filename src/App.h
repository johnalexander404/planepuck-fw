#pragma once
#include <Arduino.h>

// One tap per frame, filled by main.cpp, read by apps.
struct Tap { bool pressed = false; int x = 0; int y = 0; };
extern Tap gTap;

// Every "app" implements this. Add an app = add a class + register it in main.cpp.
class App {
public:
  const char* name;
  App(const char* n) : name(n) {}
  virtual void onEnter() {}
  virtual void loop() {}
  virtual void onExit() {}
  virtual void onMqtt(const String& topic, const String& payload) {}
  virtual bool needsNet() const { return false; }   // true = show a "finish setup" prompt until Wi-Fi is set
  virtual bool needsSetup() const { return false; }  // true = needs on-device config beyond Wi-Fi (e.g. an API key) before it runs
  virtual const char* setupHint() const { return nullptr; }  // first reason line on the setup prompt for the needsSetup case
  virtual const char* setupUrl() const { return nullptr; }   // optional URL shown on the setup prompt (e.g. the Spotify login page)
  virtual bool dimsWhenIdle() const { return false; }  // true = let the screen auto-dim on this app (Clock only)
  virtual bool onBack() { return false; }              // back chip: true = handled in-app (e.g. radar->list); false = exit to launcher

  // ---- Physical-button navigation (optional; default = touch-only) ----
  // focusCount()==0 means the app doesn't take part in button nav; touch keeps working regardless.
  virtual int  focusCount() { return 0; }              // focusable items on the current screen
  virtual void focusMove(int delta) {}                 // advance (+1) / retreat (-1); wraps
  virtual void focusSelect() {}                        // activate the focused item (same as a tap on it)
  virtual void drawFocus() {}                          // overlay a highlight on the focused item (button mode)
  virtual ~App() {}
};
