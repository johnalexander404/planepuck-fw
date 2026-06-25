#pragma once
// puck::Touch — abstracts the touch panel. main.cpp samples one point/frame; FlightApp's radar reads
// count()+get(0/1) for pinch. Edge flags (pressed/clicked) mirror M5's wasPressed()/wasClicked().
namespace puck { namespace Touch {

struct Point {
  int  x = 0, y = 0;
  bool down    = false;   // finger currently on glass (isPressed)
  bool pressed = false;   // rose this frame (wasPressed)
  bool clicked = false;   // tap: down+up with no significant move (wasClicked)
};

void  begin();
int   count();            // 0..maxPoints()
Point get(int i);         // i < count(); out-of-range -> zeroed Point
int   maxPoints();        // capability: 1 (single-touch) or 2 (pinch-capable)

}} // namespace puck::Touch
