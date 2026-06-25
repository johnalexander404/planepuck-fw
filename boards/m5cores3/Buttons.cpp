#include <M5Unified.h>
#include "hal/Buttons.h"

// CoreS3 single-button scheme: the power key is the only real button (A/B/C are touch-emulated
// bottom-screen zones, so they're left to the touch layer). Power short-click = NEXT,
// double-click = SELECT. Long-hold is left to the hardware power-off. M5.update() (puck::update())
// latches the click state; these read it.
namespace puck { namespace Buttons {
void begin() {}
int  count() { return 1; }
bool nextPressed()   { return M5.BtnPWR.wasSingleClicked(); }   // disambiguated so a double-click doesn't also step
bool prevPressed()   { return false; }
bool selectPressed() { return M5.BtnPWR.wasDoubleClicked(); }
bool backPressed()   { return false; }
bool any()           { return nextPressed() || selectPressed(); }
}} // namespace puck::Buttons
