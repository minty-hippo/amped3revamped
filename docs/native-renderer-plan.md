# Native renderer: step-by-step plan

The goal is a Vulkan renderer that takes over the game's Direct3D calls, with
the Xenia-derived GPU emulation as the fallback. Each step ends with the game
fully playable, and each new part sits behind a setting so it can be compared
with, or switched back to, the emulated path.

| Step | Takes over | Done when |
|---|---|---|
| 0. Capture | Nothing. Frame captures and shader dumps (`frame-capture.md`) | Captures from every scene type; device layout in `d3d-map.md` confirmed |
| 1. Frame pacing | The guest's vertical blank follows the monitor's real refresh (SDK patch 0007, setting `vblank_source`) | One game frame per monitor refresh at 60 Hz; no periodic repeated or dropped frames |
| 2. Resources | Track every texture, vertex buffer and index buffer the draws use: untile, convert formats, upload, invalidate on CPU writes | Captured frames' resources decode correctly (checked offline) |
| 3. Shaders | Translate the game's shaders to SPIR-V ahead of time from the dumps, with a run-time fallback | Every dumped shader translates and passes validation |
| 4. Draws and presentation | The four draw calls, `ClearF`, render targets and depth, using the device state at draw time; `D3DDevice_Swap` presents our own image | Frames match the Direct3D 12 screenshots |
| 5. Resolves and tiling | `Resolve`, `BeginTiling`/`EndTiling` become image copies and a single pass; no 10 MB EDRAM limit | Emulated GPU path no longer used during play |
| 6. Enhancements | Higher internal resolution without EDRAM tricks, MSAA, anisotropic filtering, widescreen fixes | Options in the menu |

Step 1 was first planned as presenting the front buffer with our own Vulkan
code. While drawing is still emulated, the finished frame only exists inside
the emulated GPU, so a separate presenter would have to copy every frame back
to the CPU and up again. Presenting our own image therefore moves to step 4,
when the native renderer draws it; step 1 takes over the timing side of
presentation instead.

Steps 1–3 can be built and checked without touching how frames are drawn,
which is why they come first. Step 4 is the large one; it can be done one
render pass at a time (for example menus first, then the mountain), because
the device state at each draw says which pass it belongs to.

## Step 2 results (offline check, 2026-10-02)

Tool: `tools/capture/texdecode.py <frame folder>` turns a `capture_resources` capture into PNGs.

- **Textures:** menu 16/16 and riding 256/257 decode correctly. That covers DXT1, DXT2_3, DXT4_5, DXN, k_8 and k_8_8_8_8, 2D tiled, with endian 8in16/8in32. The untile formula, endian swap and block decoders are confirmed.
- **Not in guest memory:** surfaces the GPU writes (resolve targets, such as the 1280x720 front buffer and the k_24_8_FLOAT depth texture) read back as zeros or stale data. The emulator keeps resolve output in its own GPU memory. The native renderer must therefore produce these itself (step 5), and must not load them from guest RAM.
- **Vertex/index buffers:** big-endian (vertex fetch endian 2 = 8in32, 16-bit indices). The menu quads decode as 32-byte vertices: float3 position, float3 normal and float2 UV. Projected through VS c0–c3, they cover exactly the 1280x720 viewport. Riding: all 952 VBs and 351 IBs check out once the frame log is used. The one VB that isn't float32 is a stride-12 sign mesh in half floats (half4 position + half2 UV). The one IB with indices above 60000 is a single 64,411-vertex world mesh, referenced by 503 draws through 16-bit indices (stride 28 + 48 streams). Vertex formats therefore have to come from the decl, not be assumed to be float32.

## Step 3 status (2026-10-03)

The translator (`src/native/xenos_shader.cpp`), the offline tool
(`amped3-shader-translate`, `tools/shader-translate/translate.ps1`) and the
renderer interface are described in `native-shaders.md`.

- All 184 creation blobs translate, compile with glslang and pass `spirv-val`.
- Finding: the blobs are templates. Direct3D patches vertex fetches (and
  probably `ps_1_x` texture dimensions) at draw time, so the renderer
  translates the microcode as drawn: vertex shaders right after
  `D3D_PatchVertexShaderFetches` (825A8250, which may patch a command-buffer
  copy), pixel shaders from object +12/+60.
  `capture_shaders` now saves that microcode as `ucode_*.bin`.
- Done (2026-10-03): all 68 shaders drawn in the menus and a ride
  translate, compile and pass `spirv-val`.

## Step 4, stage 1: the frame replayer (started 2026-10-03)

`amped3-replay` (`native-replay.md`) draws an F10 capture with the native
renderer's code through Vulkan and writes a PNG. It runs on any Vulkan 1.3
device, including a software one. A first check was run on an older title
screen capture, with vertex fetches filled in by hand. The title screen
came out with the right layout, textures, blending and tile resolves.
It also showed that the `ps_1_x` `tex` fetches (encoded as 1D) must sample 2D;
the translator now does that.

**Riding frame (capture 2026-10-03 12:39).** The replay now matches the
in-game screenshot: sunset lighting, sun shadows from riders and terrain,
characters, trees and sky. Getting there needed:

- capture of Direct3D's `BeginVertices` quads (post-processing, lighting and
  the final composite);
- depth resolves that keep the raw depth/stencil words, for the sun shadow
  map that the game reads back as a color texture;
- packed-mip offsets for small tiled textures (a 16x16 texture blended over
  the rider's albedo came out black);
- a defined point size (an undefined one turned a 200-point draw into large
  rectangles over the sky);
- an approximation of alpha-to-coverage.

Remaining differences are MSAA edges and the frame's own timing (particles,
slideshow). Stage 1 is done; next is stage 2 (riding frames as a regression
set) and stage 3 (presenting our own image in game).

**Stage 2, first set (captures 17:55–17:57).** Three more riding frames
(the lift area, the trees, a ridge with the Grab icon) replay with no
skipped draws. They added point sprites with a shader-exported size (falling
snow) and showed that the base level alone makes distant snow shimmer, so the
replayer now decodes and samples the mip chain. The capture now saves the
whole mip chain (it used to stop at the base level's size, which cut off the
small levels of small textures).

**Stage 2 done (2026-10-03 18:22).** Two more captures (the pause menu over
gameplay and a run past the lift) replay with no skipped draws. Seven
captures from the title, menus and four parts of the mountain now replay
correctly.

## Step 4, stage 3: the native image in the game (started 2026-10-03)

The replayer's drawing code is now the native renderer itself
(`src/native/renderer.cpp`, `Renderer`), shared by the replay tool and the
game. It keeps its caches from frame to frame (shaders, pipelines, decoded
textures, render targets, resolved images) and frees textures that go
unused.

In the game, the setting **Graphics > Native renderer (preview) > Show the
native renderer** (`native_renderer`, off by default, can be switched while
playing) turns on `src/live_renderer.cpp`:

- **Recording** (game thread, in the Direct3D hooks of `frame_capture.cpp`):
  each frame's draws, clears, resolves and tiling brackets go into a `Frame`,
  the same structure a capture file loads into. The GPU state is diffed
  against the previous draw in 32-byte chunks straight from guest memory, so
  only what changed is copied. Vertex and index data is copied once per
  frame per address range. Textures are only named (fetch constant plus a
  sampled hash of their data, so the cache sees content changes); the
  renderer reads them from guest memory when it first needs them.
- **Drawing** (a worker thread): the latest finished frame is drawn with the
  native renderer on its own Vulkan device, and the front-buffer image is read
  back.
- **Presenting**: SDK patch 0008 lets the application take over the window's
  image. While the preview is on, the emulated GPU's swap no longer refreshes
  it; the worker uploads its image instead, with the API the GPU plugin uses
  (Direct3D 12 or Vulkan). Switching the setting off hands the window back to
  the emulated GPU at the next frame.
- **Shaders** are translated from the microcode the GPU ran and compiled in
  the game with glslang (the SDK checkout's copy, built into amped3.exe by
  `setup.ps1`), with a disk cache in `logs\native_shaders`.

The emulated GPU keeps running underneath (the game waits on it), so the
preview costs extra CPU and GPU time and runs a frame behind. It is a
comparison tool until step 5 removes the emulated path. The three gaps found
in the first tests are closed (see "Finishing step 4" below): the gamma ramp,
MSAA, and reuse of resolved images across frames.

**Debugging the preview.** With the preview on, F10 also saves the frame the
preview recorded, in the capture format: `logs\frame_<time>_native.txt` and
its folder (resources and microcode included). `amped3-replay` draws it like
any capture, so a difference between the normal capture and the native one
shows whether a problem is in recording or in drawing. The writer is
`src/native/frame_writer.cpp`; `amped3-replay --rewrite <out.txt>` round-trips
a capture through it (identical images on the riding captures).

**First preview test (19:50 captures).** The preview recorded the frame
exactly: its saved frame and the normal capture of the same frame draw
identically offline. The differences seen in the game came from the GPU, not
the recording: board trails and snow spray looked right on the software
Vulkan device used offline but broken on a real GPU. Two GPU-dependent gaps
were fixed: polygon offset was ignored (the trails are stencil volumes laid on
the snow with a depth offset; without it they fail the depth test on most
GPUs), and the `largePoints` feature wasn't enabled (drivers may then draw
every point sprite at one pixel).

**Second preview test.** The trails were still missing. Each trail is drawn
in five passes over the same ribbon: two stencil passes, then two passes that
write the ribbon's depth inside the stencil (Z LESS and Z GREATER) and a
colour pass with Z EQUAL. EQUAL only passes when every pipeline gives the
ribbon bit-identical depth. Xenos guarantees that, and so does the software
device, but a real driver compiles each pipeline separately and may fuse or
reorder the position maths differently. Vertex shaders now declare
`invariant gl_Position` (and the final position transform is `precise`), as
the SDK's own translator does.

The intro videos were black in the preview (with sound). Two causes are
handled. First, a frame that doesn't end with a resolve to the front buffer
it swaps is shown emulated: every frame the game draws itself does (all
captures so far), while video frames can reach the front buffer without one.
Before, such frames kept the previous preview image on screen. Frames too
long to record also fall back to emulation now. Second, the texture decoder
lacked the packed 4:2:2 video formats (`k_Y1_Cr_Y0_Cb_REP`,
`k_Cr_Y1_Cb_Y0_REP`, sampled as Cr, Y, Cb like the SDK's host formats), and
several plain ones (`k_8_A`/`_B`, `k_6_5_5`, `k_8_8_8_8_A`, the 10/11-bit
packed formats, 16-bit unorm, half and full float). Those decode to RGBA8
for now, so float textures are clamped to 0..1.

**Third preview test (22:13 captures, Intel Arc B390, Vulkan).** Neither
problem was fixed. The log shows the game swapping during the intro and no
draw skipped, so video frames are recorded and drawn but come out black. One
more cause is handled: resolved images stay keyed by address across frames,
and a texture read the stale resolve whenever it reused that memory. A
texture now uses a resolve only when the formats have the same texel size;
YUV and compressed formats never do. The trail capture's main trail is drawn
by draws 4339-4343: stencil mark, depth/normal writes with Z LESS and
Z GREATER, then colour with Z EQUAL. On the software device each stage
changes the expected targets. `replay_trail_steps.bat` in the capture folder
draws the same stages on the PC's GPU (`--steps`), to find the stage that
fails there.

**Intro videos: found.** An F10 capture of a video frame showed a single
user-pointer quad sampling three linear `k_8` planes (Y 1280x720, U/V 640x360)
through a YUV->RGB pixel shader. It was black offline too. The preview
pointed user-pointer draws at their data by rewriting the vertex fetch
constant recorded for the draw (`vmem`). Direct3D doesn't update its shadow
copy of that constant for these draws, though, and at start-up no vertex
buffer had ever been set, so there was no `vmem` entry and the shader read
garbage. User-pointer draws now set every vertex fetch constant the shader
uses to their data (type 3, endian 8-in-32 like all of Direct3D's vertex
buffers). The video frame replays correctly.

**Trails on the Intel GPU (step results).** On the Arc B390 the trail's
stencil pass (draws 4339-4340) changes no pixel at all, while the software
device marks the whole band. The polygon offset doesn't matter on the
software device (0x, 1x and 100x mark the same pixels), and glslang 11 (the
game's compiler) gives the same result there as glslangValidator. The
shadow-map pass, which also uses a polygon offset (slope 7.2), differs on
the GPU too. `replay_trail_diag.bat` repeats the steps on the GPU with the
offset at 0x, 1x and 100x, and saves exact depth (`*_depth.f32`), a log per
step and the GLSL the Windows build generated (to rule out an MSVC/GCC
translation difference).

Results: the Windows GLSL is identical to the GCC build's; nothing was
skipped; the GPU's depth buffer matches the software device's to about 1e-6;
and the stencil pass marks nothing near the board at 0x, 1x or 100x offset.
The ribbon sits about 1e-3 in front of the terrain (reversed Z), far more
than any precision difference, and the pass can't discard (no alpha test).
494 of the strip's 699 vertices are behind the camera (w < 0), so most of
its triangles are clipped at the camera plane. The remaining suspects are
there: slope-scaled bias on clipped triangles (0 x an infinite slope is NaN,
which would explain the 0x run), clipping itself, and strip handling.
`replay_trail_diag2.bat` tries `--diag nobias`, `clamp`, `list` and
`nobias,list` on the GPU. None of them brought the mark back, so bias,
clamping and strip handling are ruled out. `replay_trail_diag3.bat` redraws
just the stencil pass with `show` (where the ribbon lands) and `nodepth`.
On the GPU `show` painted nothing: the ribbon's triangles never reach the
screen there. Its vertex shader is the only one in the frame that samples a
texture (the snow shader that also misbehaves on that GPU does too), so
`replay_trail_diag4.bat` tries `show` with that texture replaced (`novtex`)
or sampled plainly (`vtexplain`). Neither drew anything on the GPU either,
so the texture is ruled out. `replay_trail_diag5.bat` records the positions
the vertex shader computes for draw 4340 (`posdump`), for a direct
comparison with the software device.

**Cause found: a miscompiled byte swap.** On the GPU every trail vertex's
x came out as 0xC52F2FC5 (-2802.99) where the data holds 0xC52FCCCD
(-2812.8) and so on. y, z, the vertex index and the fetch address were right.
The top half of the swapped word is correct, but the bottom half is the
unswapped load's bottom half; the result is
`(swap(v) & 0xFFFF0000) | (v & 0xFFFF)`. So every x collapses to one value
and the ribbon lands off-screen. The GLSL (`xe_swap`, 8-in-32) is right, and
glslang 11's SPIR-V is right on the software device, so the Intel driver
miscompiles that function in this shader (probably also in the snow
shader). `TranslateOptions::swap_variant` adds three other formulations
(selects, `bitfieldExtract` bytes, vector-shifted bytes); all are exact on the
software device. `replay_trail_diag6.bat` tries them on the GPU so the default
can be switched to one the driver compiles correctly.

Result: on the Arc B390 (driver 101.8992) forms 1 (selects) and 2
(`bitfieldExtract`) give the right positions, form 3 fails like form 0, and
with form 1 the GPU draws the trail behind the board. Form 1, close to how
the SDK's translator writes the swap, is now the default (`--diag swap0`
brings back the old form for testing). All regression captures replay
pixel-identically with it on the software device.

## Finishing step 4 (2026-10-04)

**Gamma ramp.** The preview now applies the guest's gamma ramp before
presenting, as the emulated swap does: the 256-entry table for 8-bit front
buffers (Amped 3's case), the piecewise-linear ramp for 2_10_10_10 ones
(evaluated from the 8-bit readback). The ramp comes from the SDK's command
processor (`gamma_ramp_256_entry_table` / `gamma_ramp_pwl_rgb`, protected
there; `amped3_app.cpp` reaches them through a derived class's
using-declarations) and is applied on the CPU to the image read back for
presentation. Offline replays stay without it (captures don't record the
ramp).

**MSAA.** Render targets now have the guest's sample count
(RB_SURFACE_INFO msaa_samples: the riding scene is 2x, the front-end menu
4x). Pipelines use it, with PA_SC_AA_MASK as the sample mask (bit i = sample
i) and hardware alpha to coverage. Resolves follow D3DRESOLVE_FRAGMENT* (flags
bits 4..6: one sample, a pair, or the average of all) through small copy
shaders; multisampled depth resolves take the selected sample. Where the
device lacks a count (SwiftShader has 4 but not 2), each guest sample uses
two host samples, so masks and resolves keep working. Readback of
multisampled images for dumps goes through a single-sample copy (stencil of
multisampled depth isn't dumped yet).

This changes how the riding scene looks, and the new look is what the game
asks for. The lighting is deferred and done per sample: the game resolves
sample 0 of the G-buffer (depth, albedo, normals), draws a full-screen
lighting pass with sample mask 0x1111 (sample 0), resolves sample 1, and
draws the pass again with mask 0x2222. The pass replaces (blend ONE/ZERO) and
takes its albedo from the resolved G-buffer. The SDK's emulation ignores
PA_SC_AA_MASK, so its first pass writes both samples, its sample-1 resolve
returns lit colour, and the second pass lights it again: the emulated image
(and the single-sample preview before this) shows the snow lit twice, darker
and more orange with stronger texture. With the mask honoured each sample is
lit once from its own G-buffer, giving brighter, cleaner snow plus
anti-aliased edges. The setting **Multisampling (MSAA)** (`native_msaa`, on)
can switch it off, which matches the emulated image again (`--no-msaa` in
`amped3-replay`); with it off, the replays are pixel-identical to before.

**Resolved images across frames.** Resolved images are still kept by address
after their frame, since games sample last frame's resolves, but the first
later frame that samples one notes the texture's content name (fetch
constant plus a sampled hash of guest memory). If that changes without a new
resolve to the address, the CPU has rewritten the memory and the resolved
image is dropped; the texture is then read from memory.

**Robustness and regression check.** A draw whose vertex data is missing, or
whose vertex shader reads a fetch constant that wasn't recorded, is now
skipped instead of drawn (its shader would read far outside the vertex
buffer; SwiftShader crashed on a capture without its resource folder), and
the device enables `robustBufferAccess` where available. All 13 captures
with their data replay without skipped draws; with `--no-msaa` they are
pixel-identical to the build before these changes.

**Coverage pass (2026-10-04).** Nine new scenes, each captured with the
preview off and on: the load-rider and customise menus, a loading screen, the
Serpentine Fire and White Rabbit challenge intros (sunset sky, 2D overlays), two
chairlift rides, a story video (letterboxed) and free riding at Lower Resort.
All 18 captures replay every draw (1 to 3446 per frame) with MSAA on and off,
and look right. The only problem found was in the replayer: an indexed UP draw
took its indices from the stale device index buffer listed after it instead
of its own `up_indices=` file (the live path was already correct).

**Coverage still to check:** the other mountains, replays, other weather, and
multiplayer split screen.

## Step 5: the emulated GPU stops drawing (started 2026-10-04)

**SDK patch 0009** (`patches/rexglue/0009-host-drawing-switch.patch`) adds
`CommandProcessor::SetHostDrawingEnabled`. With host drawing off the command
processor still executes the whole command stream the game waits on
(register writes, fences, `WAIT_REG_MEM`, interrupts, shader loads), but
draws, resolves (`IssueCopy` is reached through `IssueDraw`) and swaps do no
host GPU work. Occlusion queries take the base class's fake sample count,
since the emulated GPU no longer draws anything to count. The switch is
latched at the swap packet, so the emulated GPU starts or stops on a frame
boundary.

**The live renderer** (`src/live_renderer.cpp`) turns host drawing off at
the first swap after the native image is on screen, and back on when the
`native_renderer` setting is switched off, when the native renderer fails or
cannot present, or with the new setting `native_keep_emulation` ("Keep the
emulation drawing", for comparisons). Without patch 0009 in the SDK (a
`-SkipSdk` build) the switch is unavailable and the emulated GPU keeps
drawing as before.

**Swapping without the emulated GPU.** The fallback "frames without a
resolve to the front buffer are shown emulated" is gone when the emulated
GPU is off. The swap now records the front buffer's texture (fetch constant
from the texture object, plus a data name), and the renderer's
`FinalImage()` is the front buffer's picture: this frame's resolve to it,
else an earlier resolve at its address, else the texture decoded from guest
memory (front buffers the CPU fills, such as boot screens). Only if none of
those exist (or a frame is too long to record) does the previous native
image stay up. Capture format: `Swap front_buffer=X data=tex_...` plus an
`object texture` line, written by `frame_writer` and read by the replayer
(checked with a capture whose resolve was removed: the front buffer decodes
from memory).

**Tiling.** Nothing to do: the hooks record each draw once at the Direct3D
level, before the predicated per-tile command buffers exist, so the native
renderer already draws a tiled frame in one pass with full-size render
targets and has no EDRAM size limit.

**Known limits.**
- `readback_resolve` (GPU readback) does nothing while the emulated GPU is
  off; the native renderer doesn't write resolves back to guest memory. The
  game hasn't shown a need for it so far.
- Switching the native renderer on mid-game: images the game resolved once
  before that (if any) are missing until it resolves them again. Starting
  the game with the setting on avoids this.
- The worker draws one frame behind the game (see "Performance" below for
  the presentation path).

## Performance pass (2026-10-04)

First report: with the emulated GPU off, the game ran no faster than with
the preview on top of the emulation. Changes:

- **Index data was copied per draw.** `DoDraw` read the whole index buffer
  through `FrameResources::Read` (a copy of the vector, or in the replayer a
  file read) for every draw, and the riding scenes draw hundreds of times
  from one 128-550 KB buffer: hundreds of MB copied per frame. Resources are
  now read once per frame (`FrameResources::Peek` hands the live recorder's
  blobs over without a copy). Replayer, riding capture (SwiftShader, CPU
  time): drawing 210 ms -> 23 ms per frame.
- **Indices** are byte-swapped straight into the index ring, 16-bit ones kept
  16-bit (`VK_INDEX_TYPE_UINT16`), without intermediate vectors.
- **Texture descriptor sets** are shared by draws binding the same images and
  samplers (one per distinct set per frame instead of two per draw), and the
  shader constants are written once per state change rather than per draw.
- **Presentation without the CPU.** `Renderer::DrawOutput` draws the front
  buffer through the gamma ramp (a 256 x 1 table, `builtin_output`) into an
  A2B10G10R10 image, the presenters' guest output format. With the Vulkan GPU
  backend and SDK patch 0010, that image's memory is exported
  (`VK_KHR_external_memory_win32`; three slots, imported once each by the
  presenter's device, which copies it into the window's image with
  `vkCmdCopyImage`). The renderer picks the presenter's GPU by deviceUUID.
  Otherwise (Direct3D 12 backend, an SDK without patch 0010) the packed
  image is read back from cached memory and copied row by row: the CPU no
  longer converts or gamma-corrects pixels. The log says which path is used
  when the renderer starts.
- **Recording on the game thread** reuses last frame's buffers for the vertex
  and index copies (no fresh 10 MB of allocations every frame).
- **Timings:** every 5 s the log has a line like
  `Native renderer: 60.0 frames/s shown (300 recorded); per frame:
  game-thread recording 2.10 ms, worker 9.80 ms = states ... + vertex data
  ... + draws ... + GPU ... + output ... + present ... (shared image)`.
  "frames/s shown" below the game's rate means the worker can't keep up;
  a large game-thread figure means recording is what slows the game.

Checked: all captures replay identically to before; `--output-test` on two
captures matches the CPU path within 1/1023 per channel, and the shared image
read back by a second device is identical to the readback (SwiftShader,
`VK_KHR_external_memory_fd`).

## Scrambled colours after a while (2026-10-04)

Report: after a minute or two of play the native image took on psychedelic,
posterized colours (screenshots: black became brown, yellow dark blue, snow
red), every element including the HUD, while the emulated image stayed
right; switching MSAA or the native renderer off and on fixed it for a while.
The captures from that moment replay correctly, and the final passes are
plain copies, so the colours were changed after drawing: by the gamma ramp
applied in `DrawOutput`. Comparing a correct and a broken screenshot of the
same screen gives one fixed, scrambled mapping per channel, i.e. a scrambled
ramp table.

The ramp was read from the command processor's live registers on the worker
thread, a frame or more after the swap, while the GPU thread could be in the
middle of the game rewriting them; and the table texture was only uploaded
when the read changed. Now SDK patch 0011 copies the ramp at each swap packet
on the GPU thread (the same moment the emulated swap uses it), the native
renderer reads that copy, and the table is uploaded every frame. The log
lists each new ramp (`Native renderer: gamma ramp (table) R:... G:... B:...`,
flagged `NOT MONOTONIC` if scrambled) so a repeat can be diagnosed.

**Correction (same day).** The real cause was found while reworking the
renderer for speed: a bad edit had put the output resources' clean-up
(shared output images, the gamma table texture, the readback buffer) in
`EndFrame` instead of the destructor, so they were destroyed after every
frame. Each frame re-created them: the shared images were exported and
imported again every frame (slow), and the gamma texture came back
uninitialised, but was only refilled when the ramp changed, so it showed
whatever that memory held. The every-frame upload (above) hid it; the
clean-up is now back in the destructor, and `--output-test` checks that
drawing a slot again keeps its image. Patch 0011's swap-time ramp stays: it
is still the right ramp to use.

## Performance pass 2 (2026-10-04)

Logs from a ride (game ~90-110 fps uncapped): the native image was shown at
25-45 frames/s, the worker taking 22-35 ms a frame (draw recording 8-18 ms,
GPU 6 ms, output 2.4 ms, states 1.3-2.8 ms, vertex data 1.3-2.7 ms), and the
recording hooks cost the game thread 6-10 ms a frame.

- **Two frames in flight.** `RendererOptions::async` (the game) makes
  `Render` submit with a fence and return: per-frame buffers, descriptor
  pool, uploads and replaced images belong to one of two frame slots and are
  freed when its fence signals. The worker submits frame N, then shows frame
  N-1, whose GPU work ran while N was being recorded (one more frame of
  latency; a frame left waiting is shown after 3 ms without a new one).
- **The output pass is part of the frame's submission** (`Render`'s
  `OutputRequest`): no separate submit and wait per frame.
- **Fewer Vulkan calls per draw:** pipeline, descriptor sets, viewport,
  scissor, blend constants, depth bias, stencil state and index buffer are
  only set when they differ from the previous draw in the render pass.
- **Cheaper lookups per draw:** pipeline and sampler keys are binary (no
  string formatting); each distinct texture (fetch constant + data + filter
  override) is looked up once per frame, again after a resolve.
- **States:** `BuildStates` decodes only the changed 32-byte chunks into a
  running state and copies it where a draw changed something, instead of
  decoding all ~13 KB of state for each.
- **Game thread:** state chunks compared inline (no `memcmp` call per
  chunk), a word-at-a-time hash for texture contents, and vertex / index
  ranges named and copied once per frame by integer key.

Checked: all 22 captures replay pixel-identical to before; `BuildStates`
gives identical states to the old version on random deltas; `--output-test`
now also renders three frames with an async renderer (output read after
`WaitFrame`) and compares them with the synchronous readback.

**Broken world-map geometry (fixed).** The redundant-state skip kept "index
buffer bound" across render passes and command buffers, so a frame whose
first indexed draw came after a non-indexed one in a new pass drew with no
index buffer bound in that command buffer (Intel: stretched triangles; the
replayer's SwiftShader crashed on the second frame of the world-map capture).
The cached state is now cleared completely at every render pass and frame.
The regression run now draws every capture twice with the same renderer
(`--repeat 2`) and compares the second frame, and `--async-with <capture>`
draws two captures alternately through an async renderer and checks each
output against a fresh renderer (world map: identical over 8 frames).

The timing line now splits the game-thread recording of draws into textures,
vertex data, indices and state, to choose what to speed up next there.

## Performance pass 3: the game thread (2026-10-04)

The game-thread recording was 5-7 ms a frame (state 2.4, textures 1.5,
vertex data 1.4, indices 0.5).

- **State: only what the shaders read.** Each translated shader records
  which float constants (in 32-byte chunks), fetch constants, bool and loop
  constants it reads (`ShaderUse`, cached per microcode). A draw compares
  only those chunks with the previous draw, instead of all ~13 KB; a shader
  that indexes constants with `a0`/`aL`, or one not translated yet, still
  compares everything.
- **Textures: only the fetch constants the shaders sample** are named and
  hashed (all bound ones when either shader is unknown).
- **Vertex cache** (setting `native_vertex_cache`, on by default). A copied
  vertex buffer's physical pages are write-watched with the same mechanism
  the emulated GPU uses for its copy of guest memory; the buffer is copied
  again only after one of its pages is written. Buffers written every frame
  (dirty three times in a row) stop being watched and are copied each frame,
  with another chance after ~10 s; every cached buffer is refreshed every
  4-5 s as a safety net. The log's new line gives vertex megabytes per frame
  and the share reused from the cache. Turning the setting off copies
  everything every frame, as before.

## Step 6: enhancements (started 2026-10-04)

- **Render scale** (`resolution_scale`, the existing Display > Image quality
  slider, 1x-3x). The native renderer applies it at once (the renderer is
  recreated): every render target and resolve copy is that many times
  larger, so post-processing (bloom, depth of field, the HUD) is scaled with
  the 3D instead of being shifted. Shaders still see guest pixels: the
  viewport transform, `gl_FragCoord`-based pixel positions, point sizes and
  `textureSize` of scaled render-target textures are divided back. The
  output image is the scaled front buffer; the presenter fits it to the
  window as before.
- **Anisotropic filtering** (`anisotropic_override`, Graphics > Textures):
  overrides the guest's setting on mipmapped textures, as the emulated GPU
  does; applied at once.
- MSAA was already a native setting (`native_msaa`).
- Widescreen: the game already renders 16:9; wider aspect ratios would need
  the game's projection patched and are not started.

Checked: all 22 captures are pixel-identical at scale 1 (both frames of
`--repeat 2`); at scale 2 (`amped3-replay --scale 2 --aniso 5`) the chairlift,
resort, world-map and colour-bug captures render correctly at 2560x1440,
including the second frame, and `--output-test` passes at that size.

## Performance pass 4: the worker (2026-10-04)

Logs at 2x render scale: the game ran at ~70 frames/s but only 30-50 were
shown. The worker took 15-25 ms a frame: states 2.5-4, vertex data 3-3.5,
draws 8-10. Profiled in the replayer under callgrind
(`AMPED3_PROFILE_NO_SUBMIT=1` records without submitting, so SwiftShader's
rasterizer doesn't run; the repeats are counted from the second frame on):

- **State blocks are shared** (`StateBlock`, copy on write): a draw's state
  copies five pointers, and only a block that changes (mostly a few VS
  constants) is copied, instead of ~13 KB per draw (~30 MB a frame).
- **Uniforms are written once per content**: each block has a version; a
  draw reuses the upload of an earlier draw with the same constants and
  shader, and a new upload copies only the constants the shader reads.
  Fetch, bool/loop and system constants are written only when they differ
  from the previous draw's. (Was ~21 MB of uniform writes a frame.)
- **Vertex data stays on the GPU**: one buffer holds every vertex buffer
  across frames; a buffer is copied only when its version changes
  (`FrameResources::Version`, from the vertex cache in the game; the replayer
  hashes the bytes). Replaced and unused regions are freed three frames
  later. (Was a 12 MB copy every frame.)
- **Resolve images are kept**: a blur chain resolving several sizes to the
  same address made new images every frame; the other sizes are now kept
  aside and taken back.
- Fewer allocations per draw (pipeline and texture-set keys built on the
  stack, scratch vectors, last-shader lookup).

Game thread: index buffers go through the vertex cache too (they are in
physical memory), the frame's vectors are reserved from the previous frame's
sizes, and the timing line splits the old "state" part into state and store.

Checked: all captures pixel-identical (both frames), the async alternate test
(two captures through one renderer: exercises the vertex buffer versions),
the output test, and 2x scale identical to before. Under callgrind the
worker's own instructions per gameplay frame went from ~72 M to ~44 M, most
of the rest being SwiftShader's descriptor writes.

## Video (FMV) textures (2026-10-05)

Videos are drawn as one quad sampling three CPU-written textures (Y 1280x720,
and U and V). Two faults showed in the 8-bit "Hunter" video (black frames with
small sprites): sprites stuck where they had been, and colour dropping out.

- Texture contents were named by a hash of 64 samples of 64 bytes for
  anything over 16 KB, so a frame whose changes fell between the samples kept
  the previous frame's texture. Textures are now hashed in full, but only
  when a page of theirs was written since the last hash (the vertex cache's
  write watch); textures rewritten every frame stop being watched and are
  hashed each frame.
- The worker read texture bytes from guest memory after the game had moved
  on to decoding the next video frame into the same buffers, mixing planes of
  different frames. A texture that changed is now copied into the recorded
  frame (up to 16 MB a frame) and decoded from that copy.
- A changed texture replaces its previous version in the renderer's cache
  right away, instead of one image per video frame being kept for 120 frames.
