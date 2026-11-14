// amped3 - camera scan (F8, diagnostics): finds where the game keeps its
// camera in memory, for the noclip freecam. See camera_scan.cpp.

#pragma once

#include <filesystem>

namespace amped3::camera_scan {

void Install(const std::filesystem::path& log_dir);
// Scans in the background and writes logs\amped3_camera_scan_<time>.txt.
void Run();

}  // namespace amped3::camera_scan
