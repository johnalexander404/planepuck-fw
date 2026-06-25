#pragma once
// puck::IMU — magnetometer (+ accel for tilt comp) for the heading-up radar. Compass reads this and
// keeps its own heading math. available()==false -> Compass stays north-up (zero change to callers).
namespace puck { namespace IMU {

void begin();
bool available();                         // a usable magnetometer is present
bool hasMagnetometer();
void update();                            // pump sensors (and accumulate calibration when active)
bool getMag(float& x, float& y, float& z);
bool getAccel(float& x, float& y, float& z);
void calibrationStart();                  // begin figure-8 min/max tracking
void calibrationSave();                   // freeze + persist offsets to NVS
void calibrationLoad();                   // restore saved offsets

}} // namespace puck::IMU
