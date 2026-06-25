#pragma once
#include <time.h>
// puck::RTC — battery-backed clock, seeded into the system clock at boot before NTP. The wrapper
// maps the board RTC's fields into a struct tm (raw; ClockService validates/clamps). available()
// ==false -> caller skips and waits for NTP.
namespace puck { namespace RTC {

void begin();
bool getDateTime(struct tm& out);   // fills out (raw); returns false if no RTC
void setDateTime(const struct tm& t); // post-NTP write-back so the cell survives reboot
bool available();

}} // namespace puck::RTC
