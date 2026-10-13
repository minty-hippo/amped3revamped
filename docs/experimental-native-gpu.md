# Experimental branch: native GPU path

Branch `experimental`. Goal: less overhead, steadier frame times and more
work off the game's critical threads, on every build target (Windows,
Linux x86_64, Linux arm64). In this branch the native renderer is the only
renderer: the Xbox 360 GPU emulation is removed as each stage replaces what
it still does.

## Where the time goes today

With the native renderer on, a frame still takes this path:

1. The game calls Direct3D. Direct3D writes every draw as GPU commands
   (PM4) into its command buffer (`D3D_FlushDirtyState`, the draw calls,
   `D3D_MakeSpace` when the buffer fills).
2. Our hooks record the same draw for the native renderer (`live_renderer.cpp`).
3. The emulated GPU's command processor thread reads the whole command stream,
   answers fences and interrupts, and skips the drawing (SDK patch 0009).
4. The native renderer's worker draws the recorded frame on its own Vulkan
   device and hands the image to the emulated GPU's presenter (a second
   device, shared image), which shows it.

Steps 1 and 3 exist only to keep the emulated GPU in step; step 4 needs two
devices and an extra copy.

## Stages

| Stage | Replaces | Done when |
|---|---|---|
| 0. Measure | Nothing. Performance trace (F11, `perf_trace.cpp`) | Traces from a PC and the Armada show what each thread spends per frame |
| 1. Native GPU plugin + command bypass | The command stream (steps 1 and 3): hooks feed the renderer only, Direct3D's command writing is skipped, fences are signalled natively, `D3D_MakeSpace` reports the buffer consumed. A small GPU plugin (`rexgpu-amped3`) gives the game its vertical blank interrupts and the `VdSwap` handshake | No command processor thread; game plays through menus, rides, videos, cutscenes |
| 2. Native presenter | The SDK presenter: the renderer owns the swapchain on its single device; the options menu, F3 overlay and Graphics panel draw with our own ImGui Vulkan backend | One Vulkan device; no shared-image copy |
| 3. Frame pacing | Timer-based pacing: present timing from `VK_KHR_present_wait` / `present_id`, a chosen frame queue depth, pipelines compiled in the background and warmed from the disk cache at start | Present intervals in the trace even at p99 |
| 4. Parallel work | Copies on the game thread: recording moves off it; render passes record in parallel; textures decode and upload on workers | Game thread time per frame is the game's own logic |

Each stage is compared against the trace from the stage before it.

## Stage 0: the performance trace

F11 in game, or Options > Enhancements > Performance > Record performance
trace. For `perf_trace_seconds` (20 s) it writes to `logs/`:

- `perf_<time>.csv`: one row per game frame. `frame_ms` (swap to swap),
  `swap_thread_cpu_ms` (CPU time of the thread that calls `D3DDevice_Swap`),
  and the zones:
  - `d3d`: inside Direct3D's draw, clear, resolve and tiling code (what the
    command bypass removes);
  - `flush_state`: `D3D_FlushDirtyState`, part of `d3d`;
  - `record`: our recording for the native renderer;
  - `fence_wait`: `D3D_BlockOnFence`, `BlockUntilIdle`, `BlockUntilNotBusy`;
  - `make_space`: `D3D_MakeSpace`;
  - `swap`: inside `D3DDevice_Swap`;
  - `limiter`: the frame rate limiter's sleep;
  plus the renderer worker's CPU time, the GPU time and frames presented.
- `perf_<time>_threads.csv`: every thread's CPU time as a share of one core,
  average and busiest second.
- `perf_<time>_summary.txt` (also in the log): percentiles, stutters, zone
  averages and the busiest threads.

Also in this stage: the F3 overlay's CPU frame time on Linux was the wall
clock (waits included); it is now the thread's CPU time, as on Windows.
