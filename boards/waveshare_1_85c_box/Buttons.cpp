#include "hal/Buttons.h"
// TODO(§6): BOOT/user buttons (count + pins unconfirmed). Stub: no buttons -> touch-only until the
// schematic is known. Once confirmed, wire GPIO reads here and the focus nav lights up automatically.
namespace puck { namespace Buttons {
void begin() {}
int  count() { return 0; }
bool nextPressed()   { return false; }
bool prevPressed()   { return false; }
bool selectPressed() { return false; }
bool backPressed()   { return false; }
bool any()           { return false; }
}} // namespace puck::Buttons
