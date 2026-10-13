// amped3 - crash reporter.
//
// The SDK has no handler for native crashes: an access violation in
// recompiled code just ends the process with nothing in the log. This logs
// the exception, the faulting address (also as a guest address when it falls
// in guest memory) and a stack trace of recompiled functions, whose names
// (sub_XXXXXXXX) are the original Xbox 360 addresses. It also writes a
// minidump next to the log. Calls to exit()/abort()/std::terminate, which
// end the process without an exception, are reported with a stack too.
//
// Function names need debug info: build with -Config RelWithDebInfo.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace amped3::crash {

// Call once logging is up. `log_dir` receives amped3_crash_<time>.dmp.
void Install(const std::filesystem::path& log_dir);

// Call once the runtime is set up (Linux: reports faults no SDK handler
// claims, so it must come after the SDK's own; Windows: nothing to do).
void InstallFaultHandler();

// Call when the user is quitting, so the normal exit() isn't reported.
void MarkCleanShutdown();

// Writes amped3_hang_<time>.txt (every thread's stack) and a minidump. For
// when the game is stuck without crashing. Bound to F9.
void WriteHangReport();

// Extra text appended to hang reports (guest wait targets, etc.).
void SetHangReportExtra(std::function<std::string()> extra);

// Lets the report translate host addresses back to guest addresses.
void SetGuestMemory(const uint8_t* virtual_base, const uint8_t* physical_base);

}  // namespace amped3::crash
