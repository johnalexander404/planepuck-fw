#include "hal/Touch.h"
// TODO(Phase C): CST816S over I2C 0x15 (single-touch; §6 unconfirmed). Stub: no touch.
namespace puck { namespace Touch {
void  begin() {}
int   count() { return 0; }
int   maxPoints() { return 1; }
Point get(int) { return Point(); }
}} // namespace puck::Touch
