#include <M5Unified.h>
#include "hal/puck.h"

// CoreS3 board bring-up. puck::begin() == the old M5.config()+M5.begin() plus the per-subsystem
// probes (IMU mag, light sensor) that the services used to do inline — services now just read the
// HAL's result. puck::update() == M5.update() (refreshes touch + the power key; the Touch/Buttons
// accessors read that latched per-frame state).
namespace puck {

void begin() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Display::begin();
  Touch::begin();
  Buttons::begin();
  RTC::begin();
  IMU::begin();      // probes the magnetometer + restores its calibration
  Light::begin();    // probes the LTR-553
  Power::begin();
  Audio::begin();    // M5.Speaker.begin() (volume is set by Notify)
}

void update() { M5.update(); }

const char* boardId() { return PUCK_BOARD_ID; }

} // namespace puck
