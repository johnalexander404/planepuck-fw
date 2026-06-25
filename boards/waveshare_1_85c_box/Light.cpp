#include "hal/Light.h"
// No ambient light sensor on this board -> Dim falls back to the time-of-day brightness schedule.
namespace puck { namespace Light {
void begin() {}
bool available() { return false; }
uint16_t rawLux() { return 0; }
}} // namespace puck::Light
