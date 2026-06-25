#include <M5Unified.h>
#include "hal/RTC.h"

namespace puck { namespace RTC {
void begin() {}
bool available() { return true; }

bool getDateTime(struct tm& out) {
  auto dt = M5.Rtc.getDateTime();          // raw RTC fields; ClockService validates/clamps
  out.tm_year = dt.date.year - 1900;
  out.tm_mon  = dt.date.month - 1;
  out.tm_mday = dt.date.date;
  out.tm_hour = dt.time.hours;
  out.tm_min  = dt.time.minutes;
  out.tm_sec  = dt.time.seconds;
  return true;
}

void setDateTime(const struct tm& t) { M5.Rtc.setDateTime(&t); }
}} // namespace puck::RTC
