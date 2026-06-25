#include "hal/Display.h"   // -> <LovyanGFX.hpp> on this board
// TODO(Phase C): real ST77916 QSPI panel (schematic §6) + TCA9554 reset/backlight (Expander.cpp).
// This stub is a bare LGFX_Device so the one source tree compiles + links for this env; it does NOT
// drive a panel yet — needs the board on the bench.
namespace puck {
static lgfx::LGFX_Device gPanel;
lgfx::LGFX_Device& display() { return gPanel; }
namespace Display {
  void begin() {}            // TODO(Phase C): gPanel.init() once the panel class is configured
  bool isRound() { return true; }
}
} // namespace puck
