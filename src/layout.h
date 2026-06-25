#pragma once
#include "hal/puck.h"
// Proportional layout over the LIVE screen size, so the one UI adapts to a second panel (incl. round).
// On the 320x240 CoreS3 every helper returns the original value, so the layout is pixel-identical;
// on a round panel, corner-anchored chrome (back chip, notify dot) is inset toward the chord so it
// isn't clipped. Deeper tuning — band/list/font scaling for a specific round resolution — is best
// done with the board in hand (HAL_REFACTOR_PLAN Phase C), so the fixed top-anchored constants
// (LIST_TOP, BAND_TOP, ...) are left as-is for now; they already work top-down on any size.
namespace layout {
  inline int W() { return puck::display().width(); }
  inline int H() { return puck::display().height(); }
  // Corner inset: round panels clip the corners, so push corner chrome inward. 0 on a rectangular
  // panel (CoreS3) -> identical layout. TODO(Phase C): tune the round value to the real panel radius.
  inline int inset() { return puck::Display::isRound() ? (W() * 13 / 100) : 0; }
}
