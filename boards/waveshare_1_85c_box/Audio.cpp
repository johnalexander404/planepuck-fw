#include "hal/Audio.h"
// TODO(Phase D): ES8311 codec init (I2C) + I2S TX + PA_CTRL GPIO15. Stub: silent (callers skip beeps).
namespace puck { namespace Audio {
void begin() {}
void setVolume(uint8_t) {}
void tone(uint16_t, uint32_t) {}
bool available() { return false; }
}} // namespace puck::Audio
