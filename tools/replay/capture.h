// Reads an F10 frame capture (logs\frame_<time>.txt plus its resource folder)
// into a Frame (src/native/frame.h) with the full GPU state at each draw.
// Format: docs/frame-capture.md.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "../../src/native/frame.h"

namespace replay {

using amped3::native::Clear;
using amped3::native::Draw;
using amped3::native::Event;
using amped3::native::EventType;
using amped3::native::FrameTexture;
using amped3::native::GpuState;
using amped3::native::Resolve;
using amped3::native::Tiling;

struct Capture : amped3::native::Frame {
  std::filesystem::path txt;
  std::filesystem::path resources;  // frame_<time>/
  std::vector<std::string> warnings;
};

// Throws std::runtime_error on unreadable input.
Capture LoadCapture(const std::filesystem::path& txt);

}  // namespace replay
