// amped3-launcher - folders, settings and small platform helpers.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace launcher {

namespace fs = std::filesystem;

fs::path HomeDir();
fs::path ConfigDir();          // where launcher.ini lives
fs::path ExePath();            // this launcher
fs::path DefaultInstallDir();  // <home>/Games/Amped 3 Revamped (or an existing install)

// The launcher's own copy in an install folder (what shortcuts point at).
fs::path InstalledLauncher(const fs::path& install_dir);
// Moves this launcher into the install folder as InstalledLauncher (a rename
// when on the same drive; else a copy, the original removed once this process
// is done with it), and removes older launcher downloads kept there. True
// when this launcher now lives in the install folder.
bool RelocateLauncher(const fs::path& install_dir);
// The path without spaces (Windows short 8.3 names) for the build tools, if
// the drive has them; else the path itself. The folder must exist.
fs::path NoSpacePath(const fs::path& dir);

// The game's executable in an install folder.
fs::path GameExe(const fs::path& install_dir);
bool IsInstalled(const fs::path& install_dir);

// Disc images (.iso over 1 GB) in the usual places: the launcher's folder,
// home, Downloads, Documents, Desktop, removable media.
std::vector<fs::path> FindDiscImages();

// Free space on the drive holding `dir` (or its nearest existing parent).
uint64_t FreeSpace(const fs::path& dir);

// key=value lines.
std::map<std::string, std::string> ReadKeyValues(const fs::path& file);
bool WriteKeyValues(const fs::path& file, const std::map<std::string, std::string>& values);

// Holding Shift (Windows) at start.
bool ShiftHeld();

// Running in Steam's Game Mode / a gamescope session (full screen, controller).
bool InGamescope();

std::string PathText(const fs::path& p);  // UTF-8
fs::path PathFrom(const std::string& utf8);

}  // namespace launcher
