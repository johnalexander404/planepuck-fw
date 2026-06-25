#include <M5Unified.h>
#include "hal/Display.h"

namespace puck {
lgfx::LGFX_Device& display() { return M5.Display; }   // M5GFX : lgfx::LGFX_Device : lgfx::LovyanGFX
namespace Display {
  void begin() {}                                   // panel brought up by M5.begin()
  bool isRound() { return false; }
}
} // namespace puck
