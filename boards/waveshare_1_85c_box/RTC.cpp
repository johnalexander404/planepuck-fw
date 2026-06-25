#include "hal/RTC.h"
// TODO(Phase C): PCF85063 I2C driver (backup-battery header). Stub: no RTC -> ClockService waits for NTP.
namespace puck { namespace RTC {
void begin() {}
bool available() { return false; }
bool getDateTime(struct tm&) { return false; }
void setDateTime(const struct tm&) {}
}} // namespace puck::RTC
