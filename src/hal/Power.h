#pragma once
#include <math.h>
// puck::Power — battery / charge state (NEW; was never exposed to the app). Read-only for now;
// shutdown/sleep paths are declared but deferred (idle deep-sleep is a separate, bench-tested pass).
namespace puck { namespace Power {

void  begin();
float batteryVolts();     // NAN if unsupported
int   batteryPercent();   // -1 if unsupported
bool  isCharging();       // false if unsupported
bool  isOnUsb();

// --- deferred (not wired this pass) ---
// void shutdown();
// void lightSleep(uint32_t ms);
// void deepSleepUntilTouch();

}} // namespace puck::Power
