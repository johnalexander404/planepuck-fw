#include <M5Unified.h>
#include "hal/Audio.h"

namespace puck { namespace Audio {
void begin()                          { M5.Speaker.begin(); }
void setVolume(uint8_t v)             { M5.Speaker.setVolume(v); }
void tone(uint16_t hz, uint32_t ms)   { M5.Speaker.tone(hz, ms); }
bool available()                      { return true; }
}} // namespace puck::Audio
