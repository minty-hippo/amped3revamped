// amped3-launcher - running the build scripts and tools as child processes.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace launcher {

// Runs argv[0] (searched on PATH) with stdin empty and stdout + stderr read
// line by line (terminal color codes removed). Blocks until it exits; returns
// its exit code, or -1 if it couldn't start (*error says why). While it runs,
// CancelRunning() from another thread stops it and everything it started.
int RunProcess(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
               const std::function<void(const std::string&)>& on_line, std::string* error);

// Same, collecting stdout + stderr into *output.
int RunCapture(const std::vector<std::string>& argv, std::string* output, std::string* error);

void CancelRunning();

// Starts a program and doesn't wait (shortcuts, file managers).
bool StartDetached(const std::vector<std::string>& argv, const std::filesystem::path& cwd);

// Replaces this process with the game (Linux: so Steam follows the game), or
// starts it and waits for it to end (Windows). Returns only on failure.
bool RunGame(const std::filesystem::path& exe, const std::filesystem::path& cwd,
             std::string* error);

// Removes terminal color codes and control characters.
std::string CleanLine(const std::string& in);

}  // namespace launcher
