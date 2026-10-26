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

## Stage 1, step 2: no draw packets

`D3DDevice_DrawVertices` (`825A1288`) and `D3DDevice_DrawIndexedVertices`
(`825A14B8`) do three things: note the base vertex (device+11540; when it
changes, bit 61 of the +40 mask), flush the dirty state, and write the draw
packet (`DRAW_INDX`, split when the count passes 65535, plus a per-tile patch
list entry while tiling). With `native_skip_draw_commands` (on) and the
emulated GPU not drawing, the hooks do the first two and skip the packet, so
the command stream carries only shader loads, clears, resolves, fences and
the swap. User-pointer draws (`DrawVerticesUP`, `BeginVertices`) still write
theirs: their vertices live in the command buffer and the recording reads
them there.

What the command processor still does per frame then decides whether
replacing it is worth it: the trace's GPU Commands thread share, and a new
summary section (SDK patch 0020) listing the packets it executed per frame
by kind and size, with the busiest register blocks.

### Step 2 results and what the command stream carries

Draw packets are gone (the `DRAW_INDX` left, 60-150 a frame, come from
clears, resolves and user-pointer draws). Per frame the command processor
still executes 5,000-9,000 packets, 120-530 KB:

| Packets | Per frame (light / busy scene) | Share of the bytes |
|---|---|---|
| `IM_LOAD_IMMEDIATE` (vertex shaders patched for a declaration, microcode in the command buffer) | 92 / 287, 61 / 434 KB | 51-83% |
| `IM_LOAD` + `INVALIDATE_STATE` (shader binds) | 980 / 3,166 each | 11-16% |
| Register writes (type 0) | 1,300-1,400 | 3-18% |
| Everything else (bin masks, constants, events, waits, the swap) | a few hundred | < 10% |

So the emulated GPU's remaining work is mostly shader loads, and on the
console side writing them (the immediate loads copy the patched microcode
into the command buffer). The recording needs that copy: the native renderer
reads the patched vertex shader from it.

Two changes follow:

- SDK patch 0021: while host drawing is off, the command processor only
  remembers the last shader load per stage instead of hashing and looking up
  every one, and loads it when host drawing comes back on.
- The recorder's check for "is this microcode new" (on the drawing thread at
  every shader change) was a serial checksum over byte-swapped words; it is
  now a 4-lane checksum over raw bytes.

One trace also had a 128 ms frame during a scene change: the game's main
thread was busy for 78 ms (loading), and new textures were hashed in the
same frame.

Result (PC, three traces): the GPU Commands thread went from 1.5-4.7% of a
core to 1.5-3.1% (the busiest second from 9% to 6%); nothing looked
different. The game's own threads now lead the list (main thread 19-22%,
drawing thread 17-18% in a ride), with the recorder at 11-12% and the
renderer worker at 10-12%. Frames over 25 ms in these traces were the game
loading (main thread CPU 1 s in a 1.1 s frame when a challenge starts).

## Armada (AYN Odin 3, Adreno 830), first traces

Five 20 s traces in a ride, scene at 1920x1080, frame limit 60: two with
deferred naming, three without.

| | Deferred naming on | Off |
|---|---|---|
| Game frames | 60 fps, p99 21.3-22.2 ms, 0 stutters | 56-60 fps, p99 21.8-23.3 ms, 4-8 stutters |
| Presented | 50-52 fps, 0 uneven | 46-53 fps, 0-30 uneven |
| Drawing thread | 64-75% of a core | 51-65% |
| Recorder thread | 34-46% | - |
| Renderer worker | 65-70% | 54-69% |
| GPU | 18.8-19.3 ms per frame | 18.1-18.9 ms |

- **GPU-bound.** The Adreno takes 18-19 ms for a 1080p scene, so about 52
  frames reach the screen while the game runs at 60. Render scale 67-75%
  should bring it under 16.7 ms. The renderer worker's own CPU time is
  13-15 ms a frame (draws 7.5, vertex data 2.7, states 2.3), so once the GPU
  is faster the worker is next: recording its draws in parallel (stage 4).
- **Deferred naming** costs more CPU in total on this device (the recorder
  plus a drawing thread that isn't lighter), but frame pacing is clearly
  steadier with it, so it stays on.
- **Two threads spun.** The Audio Worker used 98% of a core in every trace
  (SDK patch 0023: waits on several handles busy-looped on Linux), and the
  GPU Commands thread 14% (likely patch 0024's case: it spun 500 times at
  every idle moment). Both are changed for the next build.

## Renderer: fewer render passes (GPU)

On a tile-based GPU (Adreno on Armada, Mali) every render pass writes its
attachments out to memory at the end and reads them back at the start of the
next pass on them. The renderer broke passes more often than the game's
drawing needs:

- **Texture uploads ended the pass in progress.** A texture seen for the
  first time (and every video frame) was copied into place in the middle of
  the frame's commands, which can't happen inside a pass. Uploads now go to a
  command buffer of their own that runs before the frame's.
- **Every clear was a pass of its own** (the game clears its render targets
  and, with EDRAM tiling, each tile after resolving it). A clear now waits
  for the next pass on its image and becomes that pass's load op, or runs
  inside the pass already drawing to it (`vkCmdClearAttachments`). Tile
  clears are collected until they cover the image. Clears of render targets
  after their last use carry over to the next frame's first pass on them.
  Setting `native_merge_clears` (on) switches it off for comparison.

Replays of five captures (`amped3-replay`, SwiftShader; also with motion,
speed and object blur, fog, god rays and a 1.5x scale; and with the frame
drawn three times so carried clears are used) give pixel-identical pictures.
A ride capture's first frame went from 198 render passes to 63 (135 had been
broken by uploads); in every capture the 11-22 clear passes per frame are
gone: 5 clears become load ops.

New in the log every 5 s: render passes and clears per frame, and the GPU
time by part (`GPU by part`: what drew into each render target up to its
resolve, then the output passes), measured with timestamps. That shows which
passes cost the most on the Armada's GPU.

### Armada results, and the renderer's commands on their own thread

On the Armada (scene at 960x540 with 2x MSAA, picture at 1080p, frame limit
120): 71-79 render passes a frame, 6-7 clears folded into load ops and 3-4
left as passes; the GPU takes 9.3-10.8 ms a frame (the scene up to its depth
resolve 5.2-6.6 ms, the full-resolution HUD and composite about 2.1, depth at
1080p about 1.0, the rest under 1). Games ran at about 118 fps; the limit is
now the CPU: the drawing thread 83-85% of a core, the renderer worker 79-90%
(8-10 ms a frame recording draws: Vulkan commands 3.0, uniforms 1.9).

Next step (stage 4): the worker's Vulkan commands are recorded on a thread of
their own (`src/native/cmd_stream.*`, setting `native_parallel_commands`).
The vk:: command functions become stubs that, for the frame's command
buffer, copy the call and its arrays into a queue; a recording thread makes
the real calls in order while the worker prepares the next draws. Nothing at
the call sites changes; the upscaler (which records into the command buffer
itself) waits for the queue to empty first. The frame's command buffers come
from a pool of their own, so the worker can keep allocating upload command
buffers meanwhile. Replays (five captures, repeated frames, effects at 1.5x,
and the async renderer's own check) are pixel-identical.

## Queueing draws without a wake each

The Armada traces of 2026-10-20 (FSR on, about 61 fps, GPU 12.6 ms) put the
game's draw thread at 88% of a core, 10.6 ms a frame of it in recording; the
biggest part, `rec_indices` (5.5 ms), was not copying indices (the recorder
thread does that) but queueing each draw for naming: a lock, a push and a
condition-variable wake per draw. The recorder named each draw at once and went
back to sleep, so nearly every draw paid a futex wake (a system call, about
3 us on the handheld, for 1,500 draws a frame).

Now the recorder takes everything queued in one go and is woken only when it
sleeps and 32 draws are waiting, at the end of a frame, or when a fence waits
for the names (it then reports its progress draw by draw while anyone waits).
A stress test of the queue (3,000 frames of 50 to 450 draws with fence waits
in between) found no fence let through before its draws were named, and
ThreadSanitizer found no races. Expected on Armada: `rec_indices` from 5.5 ms
to well under 1 ms. The next limit is then the renderer's worker thread
(12.9 ms a frame).

## Renderer worker: texture descriptor sets kept between frames

The worker wrote every texture descriptor set again each frame: the sets
came from the per-frame pool, reset in `BeginFrame`. On a riding frame that is
about 224 distinct sets (4,300 draws share them), and writing them was the
biggest single part of the draw loop: about 3 ms of the replayer's 10.9 ms
on this machine's CPU (SwiftShader).

Now `TextureSet` keeps them, by content (set, bindings, samplers, views), in a
pool of their own. All of them are dropped at once when a view one of them
uses is destroyed (`Gpu::DestroyImage` lists destroyed views) or the pool
fills up; the old pool is destroyed three frames later. A depth resolve
packed to an address the game then resolves color to (every frame) made a
new image each frame, which would have dropped the sets every frame: that
image is now kept aside with the other spare resolves.

Replayer, warm repeats (record / prepass, ms):

| capture | before | after |
|---|---|---|
| riding 112830 | 10.9 / 3.6 | 7.6 / 3.3 |
| 150214 | 9.0 / 3.9 | 6.0 / 4.1 |
| 142416 | 13.7 / 6.2 | 6.5 / 4.1 |
| 083545 | 6.8 / 2.6 | 4.1 / 2.4 |

The pictures are identical to before.

## The depth pre-pass at the scene's resolution

Armada, riding with FSR (trace 20261020-172312): 82 fps, GPU 13 ms a frame,
of which about 5.5 ms went to a pass drawn before the scene: about 500 draws
into a 1280x720 surface, resolved to a texture as packed depth, blurred, and
sampled by the scene. It was drawn at the output's scale (1920x1080) while
the scene, with the upscaler, is at half that each way (960x540).

`TargetScale` now gives screen-sized surfaces drawn before the frame's tiled
pass (same pitch and height as the last tiled area, no MSAA, a 3D frame) the
scene's scale. Without an upscaler the two scales are the same and nothing
changes (replayer: identical picture). With one (replayer, scale 0.75 output
1.5): the pass is a quarter of the pixels; the final picture differs in 0.03%
of its pixels.

## MSAA off with the same lighting

Amped 3 uses its two MSAA samples as two layers for the scene's lighting:
it resolves sample 0, lights sample 0 alone (PA_SC_AA_MASK 0x1111),
resolves sample 1, lights sample 1 alone (0x2222), and the final resolve
averages the two. With one sample per pixel both lighting passes landed on
the same pixel: the snow lit twice, much darker (replayer: mean 105 -> 73).

On a single-sample target standing for the guest's two, the one sample is
guest sample 0: draws that write only other samples are skipped, and a
resolve of another sample is skipped when its destination already holds
sample 0 of the same source and rectangle with no draw to every sample
since (before the lighting the samples are equal). Both layers would be the
same, so their average is the sample-0 layer: the image matches.

Foliage drawn with alpha to coverage (and the camera fade) used the smooth
coverage path only when multisampled; it now also runs with one sample,
dithered over pixels, the dither stepping each frame so the upscaler's
accumulation sees the real coverage.

Replayer (riding capture, MSAA on vs off): mean brightness 104.8 vs 104.0
(was 72.9); pixels off by more than 32: 1.6% averaged over four frames
(foliage dither). Two other captures without an upscaler: 0.7% and 0.8%.
Armada: MSAA off saved about 0.8 ms of GPU time per frame.

## FSR reactive mask (snow without trails)

Falling snow and other particles are blended and don't write depth, so the
motion vectors FSR gets at their pixels are the ground's behind them: FSR
reprojected history along the wrong motion and the flakes left trails. FSR
takes a reactive mask for this (1: trust this frame, not history).

Scene draws after the lighting that blend and don't write depth are queued
(`QueueReactive`) and drawn again at the upscale (`DrawReactive`) into an R8
image with their own shaders, no depth test, summing the red output scaled by
the game's own source factor (alpha for alpha blending, one for additive).
Opaque scenery and alpha-tested foliage that writes depth stay out of it.

The trails stayed over textured scenery: they are where a flake was, not
where it is. FSR's history there still holds the flake (reprojected with
the scenery's motion), this frame shows scenery (reactive 0), and over a
textured surface FSR's history clamp lets it through (over the plain sky
it didn't). The mask FSR gets is now this frame's blended draws merged with
last frame's mask carried along the motion vectors (3x3 maximum, fading by a
quarter a frame): the flake's old pixels are reactive too.

MSAA with an upscaler is its own setting, `native_upscaler_msaa` (off by
default; Image quality, under the upscaler): with FSR or XeSS on the game
draws one sample per pixel and the upscaler smooths the edges. Without an
upscaler `native_msaa` decides, as before.

Faint trails were left: this frame's mask now counts 1.5x (faint flakes),
last frame's fades 15% a frame (was 25%), and the mask is also FSR's
transparency & composition mask, which keeps FSR from locking a flake's
bright pixels into its history.

The debug view (`native_reactive_debug`) showed the flakes marked, but softly:
a flake is a pixel or two at the render size, half covered at its edges, and
FSR keeps history in proportion to what the mask leaves, so faint trails
stayed over bright snow and terrain. This frame's mask is now widened by a
pixel, and small things in it (little around them three pixels out) go
nearly all-or-nothing; broad soft particles (a spray cloud, a glow) keep
their own strength so FSR doesn't drop its history over a whole area.

## Ambient occlusion

Screen-space ambient occlusion (Revamped Graphics: Ambient occlusion, with
a strength slider), applied to the finished scene before the fog so the fog
covers it in the distance.

- **Depth:** the world's depth as the game's lighting pass reads it,
  copied aside then, with the riders added: after the lighting the game
  draws depth-only character passes (render target 0 untouched, depth test
  less and greater in pairs: shadow volumes or similar) whose depth lands
  where nothing shows, then the riders and their boards, then see-through
  draws that write depth (snow spray, in-world labels and icons). The depth
  is copied before and after the depth-only passes and again before the
  first see-through draw; a pixel takes the late depth where the riders
  changed it after those passes, else the depth from before them. The
  end-of-frame depth (which has all of it) only keeps the shading off what
  was drawn in front afterwards.
- **Estimate (half size):** 12 points on a spiral within 2.5 m, projected
  onto the screen (at most a quarter of its height, so up close the world
  radius shrinks); each counts by how far it rises above the surface's
  horizon (normal from the nearer neighbours). Points under a 30 degree
  horizon, or less than an eighth of the radius proud of the surface, don't
  count: the folds between the terrain's flat triangles, the foot of a snow
  ramp and the board trails laid on the snow otherwise draw lines across the
  slope; trunks, riders, walls and corners do. Stippled foliage (depth
  broken up at every pixel) is left out. None past 150 m.
- **Blur and upsampling:** a 9-tap blur each way across points at about the
  same distance, then brought to full size from the four neighbours at the
  pixel's own distance.
- **Depth read by whole texels:** the half-size pass reads depth with
  `texelFetch` at integer positions (and places each at its texel's
  centre). Sampled at the half-size pixel's centre, the coordinate falls
  exactly between two depth texels; at render scales that aren't a whole
  multiple of the game's 1280 x 720 (70% of 1080p is 1344 x 756) the
  sampler picked one or the other row by row, and wrong normals drew
  evenly spaced horizontal lines across the snow (seen on Intel Arc with
  XeSS; reproduced in the replayer with `--scale 1.05`).
- The debug view (`native_ambient_occlusion_debug`, or `--ao <strength>
  --ao-debug` in the replayer) shows the occlusion alone.

## Sun shadows: resolution and reach

How the game does them (from captures): the first ~460 draws of a riding
frame go into a 1280 x 720 depth/stencil surface **from the sun**, through a
perspective warp that follows the camera (light-space perspective): the
terrain, objects, rails and riders, with a stencil ID per kind of object. The
depth is resolved, read back as 8_8_8_8 and blurred twice (5 taps 1.25 texels
apart, `ps 7A84B7E8B62F305E`); the lighting pass (`ps C17CD159D811B298`)
projects each pixel's world position into it (rows c6, c7 over c9: u, v) and
fades the shadows out between raw scene depths c11.x and c10.x (0.02 to
0.015: about 30-40 m). The trees' and mountains' long shadows are painted
into a 4096 x 4096 map of the slopes (the lighting's tf2), not drawn.

- **Shadow resolution** (`native_shadow_resolution`, percent of 1280 x 720,
  default 200): the shadow map's scale, apart from the scene's (it was the
  scene's: 1.05x at XeSS 70% of 1080p). The blur's step (c255.xy) is divided
  by the same factor, so edges get sharper rather than staying the game's
  width.
- **Shadow distance** (`native_shadow_distance`, metres, 0 = the game's):
  the map's v is remapped, v' = av (v - 1) + 1 (the edge at the camera
  kept), in the shadow draws' viewport transform and in the lighting's c7
  (c7' = av (c7 - c9) + c9); c10.x and c11.x are scaled to the raw depth at
  that distance. av comes from the last frame's lighting constants: the v of
  points that far ahead along the view (and to the sides). Only what the game
  draws into the map casts shadows; in the captures some riders, rails and
  objects lie beyond the game's own reach and now cast them.
- Both are part of Revamped Graphics (Effects tab): with it off the map is
  drawn at 1x (1280 x 720, whatever the scene's resolution) with the game's
  reach.
- The painted slope shadows: a 4096 x 4096 BC1 mask over the whole mountain
  (about 0.95 m per texel, u, v = world x, z times c5.xy plus c5.zw), from
  the sun as the level's artists baked it.
- The replayer: `--shadow-res <x> --shadow-distance <m>` (with `--repeat 2`:
  the map is recognised from the previous frame).
