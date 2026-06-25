#pragma once
// puck::Display — board-agnostic 2D drawing. Both the panel and the off-screen sprites are
// lgfx::LovyanGFX, so the existing code (which already targets `lgfx::LovyanGFX*` via the `g`
// pointer) works unchanged: puck::display() returns the panel ref, puck::Canvas is the sprite type.
// The board's GFX header is pulled in here so fonts::/colors/datum enums resolve in src/ without
// referencing the `M5` object (that lives only in boards/m5cores3/*.cpp).
#if defined(PUCK_BOARD_M5CORES3)
  #include <M5GFX.h>                       // provides lgfx::LovyanGFX, M5Canvas, fonts::, colors, datum
#else
  #include <LovyanGFX.hpp>
  // LovyanGFX (unlike M5GFX) doesn't define the TFT-style color-name constants the app draws with.
  // Provide them (RGB565, matching M5GFX/TFT_eSPI) so the board-agnostic src/ compiles unchanged.
  static constexpr uint16_t BLACK = 0x0000, WHITE = 0xFFFF, RED = 0xF800, GREEN = 0x07E0,
                            CYAN  = 0x07FF, MAGENTA = 0xF81F, YELLOW = 0xFFE0, ORANGE = 0xFDA0,
                            DARKGREY = 0x7BEF;
#endif

namespace puck {

lgfx::LGFX_Device& display();             // the panel (M5.Display on CoreS3). LGFX_Device : LovyanGFX,
                                          // so drawing + the `g`-pointer casts work AND setBrightness/
                                          // setRotation (panel-only methods) resolve.

#if defined(PUCK_BOARD_M5CORES3)
  using Canvas = M5Canvas;                 // off-screen sprite (PSRAM-capable)
#else
  using Canvas = lgfx::LGFX_Sprite;
#endif

namespace Display {
  void begin();
  inline int  width()                 { return puck::display().width(); }
  inline int  height()                { return puck::display().height(); }
  inline void setRotation(uint8_t r)  { puck::display().setRotation(r); }
  inline void setBrightness(uint8_t b){ puck::display().setBrightness(b); }
  bool isRound();                          // false on CoreS3 (rectangular); true on round panels
}

} // namespace puck
