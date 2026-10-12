// amped3 - scheduling hints for hybrid CPUs (performance + efficiency cores).
//
// Windows may run threads it thinks are background work on efficiency cores
// at low clocks ("EcoQoS" / power throttling), especially on laptops. The
// game's threads and the native renderer's worker are the frame's critical
// path, so the process opts out of that, and the renderer's worker also runs
// at above-normal priority. No effect on other systems.

#pragma once

namespace amped3::qos {

// The whole process: no execution-speed throttling (call once at start-up).
void PreferPerformanceProcess();
// The calling thread: no execution-speed throttling, and above-normal
// priority when `raise_priority`; `name` shows in debuggers and profilers.
void PreferPerformanceThread(const wchar_t* name, bool raise_priority);

}  // namespace amped3::qos
