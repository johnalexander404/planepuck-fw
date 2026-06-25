#include "hal/puck.h"
// TODO(Phase C/D): bring up the ST77916 panel + CST816S touch + ES8311 audio. Stub lifecycle.
namespace puck {
void begin() {
  Display::begin(); Touch::begin(); Buttons::begin();
  RTC::begin(); IMU::begin(); Light::begin(); Power::begin(); Audio::begin();
}
void update() { /* TODO(Phase C): poll CST816S touch + any buttons */ }
const char* boardId() { return PUCK_BOARD_ID; }
} // namespace puck
