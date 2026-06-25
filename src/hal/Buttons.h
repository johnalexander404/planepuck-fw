#pragma once
// puck::Buttons — physical-button navigation as a capability. The focus/selection nav layer
// (App focus hooks + launcher highlight) consumes the edge events below; it stays dormant until the
// first button event, so touch-only UX is unchanged on boards where buttons are absent/limited.
//
// CoreS3 has no real face buttons: A/B/C are touch-emulated bottom-screen zones and the only true
// physical key is the power button (long-hold = hardware power-off). So on CoreS3 the scheme is
// single-button: power short-click = NEXT, double-click = SELECT (PREV/BACK unavailable via button;
// use the touch back-chip). A board with >=2 real buttons maps PREV/NEXT/SELECT/BACK directly.
namespace puck { namespace Buttons {

void begin();
int  count();              // usable physical buttons (CoreS3: 1 = power key)

// Edge events for THIS frame (consumed after puck::update()):
bool nextPressed();        // advance focus
bool prevPressed();        // retreat focus (false on single-button boards)
bool selectPressed();      // activate the focused item
bool backPressed();        // back / up (false on single-button boards)
bool any();                // any of the above fired this frame -> enter "button mode"

}} // namespace puck::Buttons
