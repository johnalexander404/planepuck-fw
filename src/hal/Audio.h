#pragma once
#include <stdint.h>
// puck::Audio — beeper. Notify uses tone() for gentle/alert chirps. available()==false -> caller
// skips the beep (the on-screen dot/overlay still shows).
namespace puck { namespace Audio {

void begin();
void setVolume(uint8_t v);          // 0..255
void tone(uint16_t hz, uint32_t ms);
bool available();

}} // namespace puck::Audio
