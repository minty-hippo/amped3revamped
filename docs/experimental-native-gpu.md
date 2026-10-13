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
  - `record`: our recording for the native renderer, split for draws into
    `rec_textures`, `rec_vertices`, `rec_indices` and `rec_state`;
  - `fence_wait`: `D3D_BlockOnFence`, `BlockUntilIdle`, `BlockUntilNotBusy`;
  - `make_space`: `D3D_MakeSpace`;
  - `swap`: inside `D3DDevice_Swap`;
  - `limiter`: the frame rate limiter's sleep;
  plus the renderer worker's CPU time, the GPU time, frames presented and the
  swapping thread's heap allocations.
- `perf_<time>_threads.csv`: every thread's CPU time as a share of one core,
  average and busiest second.
- `perf_<time>_summary.txt` (also in the log): percentiles, stutters, zone
  averages and the busiest threads.

Also in this stage: the F3 overlay's CPU frame time on Linux was the wall
clock (waits included); it is now the thread's CPU time, as on Windows.

## Results

### PC, first traces (2026-10-13, Intel laptop, Direct3D 12 + native renderer, 60 fps)

| | Trace 1 | Trace 2 |
|---|---|---|
| Frame time p50 / p99 | 16.65 / 19.41 ms | 16.69 / 18.90 ms |
| Presents uneven | 3 | 84 (GPU 16.25 ms/frame: GPU-bound) |
| Swapping thread CPU | 7.14 ms | 6.35 ms |
| of which `record` | 5.34 ms | 4.81 ms |
| of which `d3d` | 1.25 ms | 1.11 ms |
| Renderer worker | 7.50 ms | 6.44 ms |
| Busiest thread (game logic, XThread F800009C) | 65% of a core | 57% |
| GPU Commands (emulated command processor) | 17% | 15% |
| rex::thread::TimerQueue | 11% | 10% |

- Our recording is about 75% of the swapping thread's CPU time: the largest
  single cost we add to the game's critical path. On the Armada the log's
  own breakdown showed 24 ms per frame (textures 12, vertex data 9.5).
- The timer thread spun on an empty queue: SDK patch 0018.
- Changes after these traces: short resource names that fit a string's
  inline buffer (no heap allocation per copy); per-part recording zones and
  allocation counts in the trace, to confirm where the rest goes.

### PC, second traces (after the first changes)

- Recording: 4.1-5.1 ms per frame (textures 1.4-1.7, vertices 1.0-1.2,
  indices 0.4, state 0.4-0.5); 18-20 heap allocations per frame on the
  swapping thread, so allocations weren't the cost. Next: per-slot caches of
  the previous draw's texture and vertex names (consecutive draws mostly keep
  their bindings) and of the last shader looked up per stage.
- The first 0018 polled the last millisecond before each timer; the kernel's
  1 ms timestamp timer kept it polling: the timer thread at 99% of a core.
  Fixed with a high-resolution waitable timer on Windows and a plain
  condition variable wait elsewhere.
- GPU 15.7-15.9 ms per frame: this laptop is near its GPU limit at these
  settings; the presented-frame intervals stay even (1-6 uneven in 20 s).

### PC, third traces (binding caches, fixed timer thread)

| | Before (first traces) | Now |
|---|---|---|
| Swapping thread CPU | 6.4-7.8 ms | 5.6-6.3 ms |
| `record` | 4.8-5.3 ms | 3.0-3.8 ms (textures 1.0-1.2, vertices 0.7-0.9) |
| `d3d` | 1.1-1.4 ms | 1.0-1.2 ms |
| Renderer worker | 6.4-7.7 ms | 5.4-6.3 ms |
| rex::thread::TimerQueue | 10-11% of a core | 1.3-2.4% |
| GPU Commands | 15-19% | 16-19% |

The scenes differ between traces (GPU 7.5-8.2 ms now against 8-16 ms), so
compare the CPU columns. On the Armada, about 5x slower per core, the
swapping thread is the frame-rate limit: recording and Direct3D's command
writing are the two parts we can remove there. The worker hands its image to
the window through the CPU with Direct3D 12 (`present` 0.9 ms); with the
Vulkan backend it stays on the GPU.

### PC, Vulkan for the emulated GPU and the window

With `gpu_backend = vulkan` the native renderer's picture reaches the window
through a shared image instead of the CPU: the worker's `present` went from
0.90 ms to 0.07 ms, the worker thread from 36-43% to 27-35% of a core, and
frame pacing stayed even (p99 18.4-19.0 ms, 1 uneven present in 20 s).
`gpu_backend` now defaults to `auto`: Vulkan while the native renderer draws,
Direct3D 12 on Windows when the emulation draws.

## Stage 1, step 1: no state commands while the emulation doesn't draw

`D3D_FlushDirtyState` (`825A9268`) runs before every draw. Its structure (from
the recompiled code):

| Mask (device+, 64-bit) | Bits | Call | Kind |
|---|---|---|---|
| +48 | 0x400 | `D3D_CommitVertexFetch` (`825A8130`): fetch constants from the streams and declaration, into the device shadow | CPU state (kept) |
| +48 | 0x1E0 | `D3D_CommitShaders` (`825A8C98`): patches the vertex shader for the declaration, IM_LOAD | CPU state + commands (kept) |
| +48 | high bits | `D3D_WriteRegisterBlock` x2 (regs 0x2300, 0x2380) | commands |
| +16 / +24 | all | `D3D_WriteAluConstants` (0x4000 / 0x4400 from +1920 / +6016) | commands |
| +32 | low 32 | `D3D_WriteFetchConstants` x2 (0x4800 from +1152) | commands |
| +32 | 0x80000000 | stores 0xFFFFFFFFFF000000 at device+11440, then a block at 0x4900 | CPU state (kept) + commands |
| +32 | high bits | block at 0x2000 | commands |
| +40 | various | four register blocks (0x2100, 0x2180, 0x2200, 0x2280) | commands |

Each mask is cleared after its part. With `native_skip_state_writes` (on) and
the emulated GPU not drawing, the hook runs only the kept parts, in the same
order, and clears the masks; the register and constant commands are never
written, so neither Direct3D nor the command processor spends time on them.
When the emulation draws again, all five masks are set once and the original
flush writes the whole state.

### Step 1 results (PC, Vulkan)

| | Before | After |
|---|---|---|
| `flush_state` | 0.60-0.74 ms | 0.36 ms |
| `d3d` | 0.91-1.14 ms | 0.66-0.68 ms |
| GPU Commands thread | 13-16% of a core | 5.6-6.2% |

No visible problems in a test ride. The traces also show the draws come from
the game's busiest thread (XThread F800009C, 41-43% of a core), not the
thread that calls `D3DDevice_Swap`: the recording (2.7 ms) sits on it.

## Recording without allocations

The swapping thread made ~18 allocations a frame, but the draws are recorded
on another thread. Per draw the recorder still allocated: the draw's texture
list (a vector), its vertex stream map (a node per stream), and the two
microcode names (`ucode_vs_<16 hex>`, longer than a string's inline buffer).
Now `Draw::textures` and `Draw::vmem` keep up to 6 / 4 entries inline
(`SmallVector`, `SmallMap` in `frame.h`), and the recorder names microcode by
12-character aliases (`V<11>` / `P<11>`) that the live resources map back to
the stems. The renderer's one check of shader identity by name (event icons)
now compares microcode hashes. The trace counts the recorder's allocations.
Frame dumps (F10) from the live renderer keep the aliases, so the replay
tool's debug filters by shader hash don't match them.

Result: recording made 4,000-5,900 heap allocations a frame even so (the
per-frame name maps: a node per texture, buffer and version), and the larger
`Draw` cost 0.85 ms a frame to move into the frame (`store`), eating the gain.
Draws are now built in place (`emplace_back`), with 4 textures and 2 streams
inline.

## Naming off the drawing thread

Most of what remains on the drawing thread (F800009C) is naming: hashing
the textures a draw uses, looking up the vertex cache and copying index data
into the frame. With `native_deferred_naming` (on; needs a restart) the
drawing thread only copies each draw's fetch constants and index buffer
address and queues them; a new recorder thread (`amped3 recorder`) names
them, and at the end of the frame puts the names into the draws and hands
the frame to the prep thread. The trace shows its time as `rec_naming`.

The recorder reads guest memory after the draw, so the game must not have
rewritten that memory yet. The game already waits for a GPU fence before it
rewrites memory a draw used (as on the console, where the GPU reads it
later). Direct3D inserts a fence (`sub_825A3330`, value: the device's CPU
fence at +10396, +2 each time) as an `EVENT_WRITE_SHD` packet that makes the
GPU store it to the fence word (+10384 points to it); in some phases
(device+20416 = 0 and flag 0x04 at +10433) it also stores it from the CPU at
once. Each queued draw carries the CPU fence current when it was drawn, and a
fence is let through only once every draw at or before it is named:

- the command processor calls back before an `EVENT_WRITE_SHD` stores to the
  fence word (SDK patch 0019) and waits there;
- the hook on `sub_825A3330` waits before Direct3D's own CPU store.

A wait gives up after 2 s with a warning in the log, so a mistake shows up as
a hitch rather than a hang. With the setting off, naming runs on the drawing
thread as before (for comparing traces).

### Results (PC, Vulkan, two 20 s rides)

| | Before | After |
|---|---|---|
| Drawing thread (F800009C) | 41-43% of a core | 23-29% |
| Recorder thread | (on the drawing thread) | 16-18% |
| Heap allocations while recording | 4,000-5,900 a frame | 114-250 |
| `d3d` | 0.62-0.85 ms | 0.34-0.44 ms |
| GPU Commands thread | 4.6-6% | 2.7-3.7% |

60 fps with p99 frame time 17.6-17.7 ms, no stutters, at most one uneven
present, and no fence waited for the recorder. Nothing broken in testing.
