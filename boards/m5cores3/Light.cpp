#include <M5Unified.h>
#include "hal/Light.h"

// LTR-553 on the internal I2C bus (0x23). Auto-detected via MANUFAC_ID == 0x05. The Dim service
// owns the FORCE_NO_LIGHT_SENSOR policy + the raw->brightness mapping.
namespace puck { namespace Light {
static const uint8_t ADDR = 0x23, ALS_CONTR = 0x80, MANUFAC = 0x87, CH0_LOW = 0x8A;
static bool gPresent = false;

void begin() {
  uint8_t id = 0;
  if (M5.In_I2C.readRegister(ADDR, MANUFAC, &id, 1, 400000) && id == 0x05) {
    gPresent = true;
    M5.In_I2C.writeRegister8(ADDR, ALS_CONTR, 0x01, 400000);   // wake into active mode
  }
}
bool available() { return gPresent; }
uint16_t rawLux() {
  uint8_t buf[2] = {0, 0};
  if (!M5.In_I2C.readRegister(ADDR, CH0_LOW, buf, 2, 400000)) return 0;
  return (uint16_t)(buf[0] | (buf[1] << 8));
}
}} // namespace puck::Light
