// amped3 - performance overlay (F3, rebindable as bind_performance_overlay).
//
// Replaces the SDK's debug overlay on the same key with a performance panel:
// frame rate and frame time, the CPU's and the GPU's time per frame, a live
// frame time graph (frame, CPU and GPU lines), and the CPU's and GPU's usage,
// clock speed and power draw.
//
// Where the numbers come from:
// - Frame time: real time between the game's D3DDevice_Swap calls.
// - CPU frame time: the game thread's own CPU time between swaps (its thread
//   cycle count, so time spent waiting doesn't count), not counting the frame
//   rate limiter. The native renderer's worker thread is shown next to it.
// - GPU frame time: GPU timestamps around each native renderer frame (none
//   while the emulated GPU draws).
// - CPU usage, clock, power: Windows performance counters (Processor
//   Information, Energy Meter where the platform has one).
// - GPU usage: Windows' GPU Engine counters (the busiest adapter's 3D
//   engine, as Task Manager shows it); clock and power from the vendor's
//   library (NVIDIA NVML, Intel IGCL), else n/a.
//
// The system counters are sampled twice a second on a background thread,
// only while the panel is open.

#pragma once

#include <string>

namespace rex::ui {
class ImGuiDrawer;
}

namespace amped3::perf {

// Game thread, D3DDevice_Swap: before the frame rate limiter waits, and after.
void OnSwapBegin();
void OnSwapEnd();

// Native renderer worker: its CPU time for a frame (ms), and the GPU time of
// a finished frame (ms; < 0 unknown).
void ReportRenderFrame(double worker_ms);
void ReportGpuFrame(double gpu_ms);
// Native renderer worker: a frame reached the screen.
void ReportPresented();
// The GPU the native renderer draws on (picks the vendor library).
void SetGpuName(const std::string& name);

// UI thread: registers the key (replacing the SDK's debug overlay on it).
void Install(rex::ui::ImGuiDrawer* drawer);
void Shutdown();

}  // namespace amped3::perf
