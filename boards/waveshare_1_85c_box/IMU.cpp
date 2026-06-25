#include "hal/IMU.h"
// No magnetometer on this board -> Compass reports unavailable, radar stays north-up (zero caller change).
namespace puck { namespace IMU {
void begin() {}
bool available() { return false; }
bool hasMagnetometer() { return false; }
void update() {}
bool getMag(float&, float&, float&) { return false; }
bool getAccel(float&, float&, float&) { return false; }
void calibrationStart() {}
void calibrationSave() {}
void calibrationLoad() {}
}} // namespace puck::IMU
