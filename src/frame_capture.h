// amped3 - frame capture for the native-renderer work.
//
// Press F10 (bind_frame_capture) in game: the next whole frame - from one
// D3DDevice_Swap to the next - is written to logs\frame_<time>.txt with every
// draw, clear, resolve and render-target change, and the Direct3D device
// state each draw used. See docs\frame-capture.md.
//
// Shaders are saved as the game creates them (setting capture_shaders, on by
// default) to logs\shaders\.
//
// Hooks the Direct3D functions by the names in amped3_d3d_names.toml, so the
// build needs those names (setup.ps1 always includes them now).

#pragma once

#include <cstdint>
#include <filesystem>

namespace amped3::capture {

// Where frame files and the shaders folder go (the exe's logs folder).
void Install(const std::filesystem::path& log_dir);

// Capture the next complete frame. Safe to call from any thread.
void RequestFrame();

// Game frame rate, measured at D3DDevice_Swap (one call per game frame):
// average over the last half second. frame_count is 0 until frames arrive.
struct FrameRate {
  double fps = 0;
  double frame_time_ms = 0;
  uint64_t frame_count = 0;
};
FrameRate GetFrameRate();

}  // namespace amped3::capture
