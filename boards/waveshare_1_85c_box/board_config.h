#pragma once
// Waveshare ESP32-S3-Touch-LCD-1.85C-BOX — round panel. STUB board: structure + build only; the real
// drivers (display/touch/audio/power) need the physical board + schematic (see HAL_REFACTOR_PLAN §6).
#define PUCK_BOARD_ID    "waveshare_1_85c_box"
#define PUCK_OTA_SUFFIX  "-waveshare_1_85c_box"    // board-scoped OTA paths, isolated from CoreS3
#define PUCK_HAS_PSRAM   1
#define PUCK_ROUND       1                          // round screen -> layout.h applies corner insets
