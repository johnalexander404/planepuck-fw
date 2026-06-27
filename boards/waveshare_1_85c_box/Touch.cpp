#include <Arduino.h>
#include <LovyanGFX.hpp>
#include "hal/Display.h"   // puck::display()
#include "hal/Touch.h"

// CST816 single-touch on I2C0 (SDA 11 / SCL 10, addr 0x15), driven by LovyanGFX (Touch_CST816S
// attached to the panel in Display.cpp). M5Unified gives the CoreS3 edge flags for free; here we
// synthesize them: pollFrame() samples the panel once per frame (called from Board.cpp's
// puck::update()) and computes pressed/clicked/down, and count()/get() serve the cached state so
// repeated reads within a frame stay consistent (matches the M5.update() model main.cpp expects).

namespace puck { namespace Touch {

namespace {
  int  curX = 0, curY = 0;
  bool curDown = false, prevDown = false;
  bool edgePressed = false, edgeClicked = false;
  int  downX = 0, downY = 0;
  bool moved = false;
  const int MOVE_SLOP = 12;            // px of travel that turns a tap into a drag (no click)
}

// Called once per frame from puck::update() (Board.cpp), before the active app reads touch.
void pollFrame() {
  lgfx::touch_point_t tp;
  bool down = (puck::display().getTouch(&tp, 1) > 0);
  if (down) { curX = tp.x; curY = tp.y; }
  curDown = down;

  edgePressed = (down && !prevDown);
  if (edgePressed) { downX = curX; downY = curY; moved = false; }
  if (down && (abs(curX - downX) > MOVE_SLOP || abs(curY - downY) > MOVE_SLOP)) moved = true;
  edgeClicked = (!down && prevDown && !moved);   // released without significant travel == tap
  prevDown = down;
}

void begin() {}
int  maxPoints() { return 1; }                   // CST816 is single-touch (no pinch)
int  count()     { return curDown ? 1 : 0; }

Point get(int i) {
  Point p;
  if (i != 0) return p;
  p.x = curX; p.y = curY;
  p.down    = curDown;
  p.pressed = edgePressed;
  p.clicked = edgeClicked;
  return p;
}

}} // namespace puck::Touch
