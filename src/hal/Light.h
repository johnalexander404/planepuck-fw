#pragma once
#include <stdint.h>
// puck::Light — ambient light sensor (CoreS3 LTR-553). Dim maps rawLux() to brightness; if
// available()==false it falls back to the time-of-day schedule.
namespace puck { namespace Light {

void begin();
bool available();
uint16_t rawLux();    // raw CH0 count, mapped by the Dim service

}} // namespace puck::Light
