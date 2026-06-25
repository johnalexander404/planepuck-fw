#include "hal/Power.h"
// TODO(§6): battery sense ADC divider (GPIO unconfirmed) + ETA6098 charger has no I2C. Stub: unsupported.
namespace puck { namespace Power {
void  begin() {}
float batteryVolts()   { return NAN; }
int   batteryPercent() { return -1; }
bool  isCharging()     { return false; }
bool  isOnUsb()        { return true; }
}} // namespace puck::Power
