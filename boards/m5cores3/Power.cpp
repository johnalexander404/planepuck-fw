#include <M5Unified.h>
#include "hal/Power.h"

// AXP2101 PMIC via M5.Power. Read-only this pass (battery exposed to the app for the first time);
// shutdown/sleep paths are deferred.
namespace puck { namespace Power {
void  begin() {}                                              // AXP2101 up via M5.begin()
float batteryVolts()   { return M5.Power.getBatteryVoltage() / 1000.0f; }   // mV -> V
int   batteryPercent() { return M5.Power.getBatteryLevel(); }               // 0..100 (-1 if n/a)
bool  isCharging()     { return M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging; }
bool  isOnUsb()        { return M5.Power.isCharging() != m5::Power_Class::is_charging_t::is_discharging; }
}} // namespace puck::Power
