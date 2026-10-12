// Writes a Frame in the F10 capture text format (docs/frame-capture.md), with
// its resources as files, so tools/replay can load it again. Used by the game
// to save what the native renderer preview recorded.

#pragma once

#include <filesystem>
#include <string>

#include "frame.h"

namespace amped3::native {

// `txt`: the capture file; `resources`: its resource folder (the replayer
// looks for <txt stem>/ next to it). Microcode goes into the resource folder.
bool WriteFrameCapture(const Frame& frame, FrameResources& resources,
                       const std::filesystem::path& txt,
                       const std::filesystem::path& resource_dir, std::string* error);

}  // namespace amped3::native
