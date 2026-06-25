#include <M5Unified.h>
#include "hal/IMU.h"

// Magnetometer lives at IMU instance 1 (null on units without one). Calibration uses M5Unified's
// built-in offset system; Compass keeps the heading math + the figure-8 timing.
namespace puck { namespace IMU {
static bool gHasMag = false;

void begin() {
  if (!M5.Imu.isEnabled()) { gHasMag = false; return; }
  gHasMag = (M5.Imu.getImuInstancePtr(1) != nullptr);
  if (gHasMag) M5.Imu.loadOffsetFromNVS();    // restore saved hard-iron offsets
}
bool available()       { return gHasMag; }
bool hasMagnetometer() { return gHasMag; }
void update()          { M5.Imu.update(); }    // calibration accumulates here when active
bool getMag(float& x, float& y, float& z)   { return M5.Imu.getMag(&x, &y, &z); }
bool getAccel(float& x, float& y, float& z) { return M5.Imu.getAccel(&x, &y, &z); }
void calibrationStart(){ M5.Imu.setCalibration(0, 0, 128); }
void calibrationSave() { M5.Imu.setCalibration(0, 0, 0); M5.Imu.saveOffsetToNVS(); }
void calibrationLoad() { M5.Imu.loadOffsetFromNVS(); }
}} // namespace puck::IMU
