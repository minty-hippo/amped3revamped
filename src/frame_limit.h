// amped3 - frame rate cap (setting frame_rate_limit).
//
// The default, auto, is 60 with Revamped Graphics and 30 without.
// 30 keeps the console's own pacing: gameplay at every second 60 Hz vertical
// blank (menus at every one), following the monitor (vblank_source). Any
// other choice turns that
// throttle off (setting vsync) and paces the game itself at D3DDevice_Swap:
// Amped 3 times its gameplay by elapsed time, so it plays at normal speed at
// any frame rate.

#pragma once

#include <string>

namespace amped3::frame_limit {

// The cap a frame_rate_limit value means: "auto" (the default) is 60 with
// Revamped Graphics on and 30 (the console's pacing) with it off; any other
// value is returned as is.
std::string Effective(const std::string& value);

// Called by the D3DDevice_Swap hook on the game thread, before the swap:
// applies a changed setting and waits until the frame's slot.
void OnSwap();

// Real time between the last swaps, smoothed over a few frames (seconds; 0
// until measured). Game code that only sees the game's own time step (which
// some modes scale) uses this to tell the frame rate.
double FrameSeconds();

}  // namespace amped3::frame_limit
