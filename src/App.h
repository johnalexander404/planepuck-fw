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
  virtual bool dimsWhenIdle() const { return false; }  // true = let the screen auto-dim on this app (Clock only)
  virtual bool onBack() { return false; }              // back chip: true = handled in-app (e.g. radar->list); false = exit to launcher
  virtual ~App() {}
};
