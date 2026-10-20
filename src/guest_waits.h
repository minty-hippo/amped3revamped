// amped3 - guest wait diagnostics for hang reports.
//
// For every game (guest) thread, shows the PowerPC argument registers and
// link register as they were when it entered its current kernel call, and
// resolves the wait target: a handle to its kernel object (event, semaphore,
// thread, ...) with name and state, or a pointer to an in-memory dispatcher
// header (KeWait*) with its type and signal state.

#pragma once

#include <string>

namespace rex::system {
class KernelState;
}

namespace amped3::diag {

std::string DescribeGuestWaits(rex::system::KernelState* kernel_state);

// One line per game thread: name, link register and r3 as of its last kernel
// call (stall watchdog: where a thread that stopped presenting is).
std::string GuestThreadSnapshot(rex::system::KernelState* kernel_state);

}  // namespace amped3::diag
