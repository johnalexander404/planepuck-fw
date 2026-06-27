#include "hal/puck.h"
// TODO(Phase D): bring up ES8311 audio + battery/power. Display + touch are live.
namespace puck {
namespace Touch { void pollFrame(); }   // board-local per-frame sampler (Touch.cpp)
void begin() {
  Display::begin(); Touch::begin(); Buttons::begin();
  RTC::begin(); IMU::begin(); Light::begin(); Power::begin(); Audio::begin();
}
void update() { Touch::pollFrame(); }   // refresh touch once per frame (== M5.update() on CoreS3)
const char* boardId() { return PUCK_BOARD_ID; }
} // namespace puck
