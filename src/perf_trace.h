// amped3 - performance trace: where each frame's time goes, written to logs.
//
// Start one with F11 (bind_perf_trace), or the options menu's "Record
// performance trace" (setting perf_trace, which switches itself back off).
// For perf_trace_seconds (20 by default) it records:
//
// - logs/perf_<time>.csv, one row per game frame (D3DDevice_Swap to the next):
//   frame time, the CPU time of the thread that swaps, and the time spent in
//   each zone below, plus the native renderer worker's CPU time, the GPU time
//   and the time between frames reaching the screen.
// - logs/perf_<time>_threads.csv: every thread of the process with its CPU
//   time over the trace (as a share of one core) and its busiest second.
// - logs/perf_<time>_summary.txt (also in the log): frame time percentiles,
//   stutters, zone averages and the busiest threads.
//
// Zones are timed only while a trace runs; otherwise each costs one relaxed
// atomic load.

#pragma once

#include <cstdint>

namespace amped3::trace {

enum class Zone : int {
  kD3D,         // inside the game's Direct3D draw / clear / resolve / tiling code
  kFlushState,  // D3D_FlushDirtyState (part of kD3D): writing changed state as GPU commands
  kRecord,      // the native renderer recording the frame in our hooks
  kRecTextures, // part of kRecord, draws: naming the bound textures
  kRecVertices, // part of kRecord, draws: vertex memory
  kRecIndices,  // part of kRecord, draws: indices and user-pointer data
  kRecState,    // part of kRecord, draws: state changes since the last draw
  kFenceWait,   // D3D_BlockOnFence / BlockUntilIdle / BlockUntilNotBusy: waiting on the GPU
  kMakeSpace,   // D3D_MakeSpace: command buffer full, kicking and waiting
  kSwap,        // inside the game's D3DDevice_Swap (VdSwap)
  kLimiter,     // the frame rate limiter's sleep
  kCount
};

const char* ZoneName(Zone zone);

bool Active();

// Adds to a zone (any thread) while a trace runs.
void Add(Zone zone, int64_t ns);

class Scope {
 public:
  explicit Scope(Zone zone);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Zone zone_;
  int64_t start_ = 0;  // 0: not timing
};

// Starts a trace (any thread). Ignored while one runs.
void Start();

// The thread that calls D3DDevice_Swap, at the start of the hook: closes the
// previous frame's row.
void OnSwap();

// Native renderer worker: its CPU time for a frame, the GPU time of a
// finished frame (< 0 unknown), and a frame reaching the screen.
void OnRenderFrame(double worker_ms);
void OnGpuFrame(double gpu_ms);
void OnPresented();

// The thread issuing Direct3D draws (any draw hook), named in the thread list.
void NoteDrawThread();

// Registers the key and the setting watcher (UI thread, once).
void Install();

// This thread's CPU time in nanoseconds (not counting time it waits).
int64_t ThreadCpuNs();

// Heap allocations (operator new) made by this thread so far.
uint64_t ThreadAllocations();

}  // namespace amped3::trace
