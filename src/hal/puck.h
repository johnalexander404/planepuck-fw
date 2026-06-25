#pragma once
// puck:: — the board Hardware Abstraction Layer. src/ talks only to puck::*; the concrete
// implementation is compiled from boards/<board_id>/*.cpp (selected by build_src_filter +
// PUCK_BOARD_* in platformio.ini). board_config.h (per board) defines PUCK_BOARD_ID,
// PUCK_OTA_SUFFIX, PUCK_HAS_PSRAM, PUCK_ROUND.
#include "board_config.h"
#include "Display.h"
#include "Touch.h"
#include "Audio.h"
#include "Power.h"
#include "RTC.h"
#include "IMU.h"
#include "Light.h"
#include "Buttons.h"

namespace puck {
  void begin();            // bring the board up (display/touch/audio/rtc/imu/light/power/buttons)
  void update();           // per-frame input poll (touch + buttons)
  const char* boardId();   // PUCK_BOARD_ID
}
