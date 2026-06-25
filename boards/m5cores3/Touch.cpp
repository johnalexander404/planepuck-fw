#include <M5Unified.h>
#include "hal/Touch.h"

namespace puck { namespace Touch {
void begin() {}
int  count()     { return M5.Touch.getCount(); }
int  maxPoints() { return 2; }                 // CoreS3 capacitive panel is multi-touch
Point get(int i) {
  Point p;
  if (i < 0 || i >= (int)M5.Touch.getCount()) return p;
  auto d = M5.Touch.getDetail(i);
  p.x = d.x; p.y = d.y;
  p.down = d.isPressed(); p.pressed = d.wasPressed(); p.clicked = d.wasClicked();
  return p;
}
}} // namespace puck::Touch
