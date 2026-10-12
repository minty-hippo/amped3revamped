# Native renderer, stage 1: the frame replayer

The drawing code described here is the native renderer itself
(`src/native/renderer.cpp`); the replayer is a front end that loads a capture
into a `Frame` (`src/native/frame.h`) and serves its resource files. The game
uses the same renderer for its live preview (`docs/native-renderer-plan.md`,
step 4 stage 3).

`amped3-replay` draws an F10 frame capture with the native renderer's code
and writes the result as a PNG. It uses the translated shaders
(`native-shaders.md`), the CPU texture decoder (`src/native/xenos_texture.cpp`)
and the Xenos render state mapped to Vulkan. This makes it possible to compare
the native path with the game's own output frame by frame, without touching
the running game. It runs on any Vulkan 1.3 device, including a software one,
so captures can also be checked away from the PC they came from.

```
amped3-replay <logs\frame_<time>.txt> [-o <out folder>] [--shaders <logs\shaders>]
              [--glslang <glslangValidator>] [--draws <n>] [--dump] [--device <n>]
              [--steps <a,b,...|all>]
```

**Running it.**

- On Windows, `tools\replay\replay.ps1` finds the tool, glslangValidator
  (Vulkan SDK) and the newest capture.
- The output goes to `<capture>_replay\`:
  - `frame.png`: what was resolved to the front buffer, or the last render
    target if that wasn't reached.
  - `replay.txt`: what was skipped or approximated, with counts.
  - `spv\`: the compiled shaders.
- `--draws n` stops after n draws, which is useful for finding the draw that
  goes wrong.
- `--dump` also writes every render target and every resolved texture, and
  for depth targets a `<name>_stencil.png` (red = stencil bit 0, green =
  bit 1, blue = the higher bits).
- `--steps a,b,...` (numbers or `all`) draws the frame several times, each
  with a fresh renderer stopping after that many draws, and dumps everything
  to `<out>\step_<n>\`, with a `replay.txt` per step and each depth target's
  exact values as `<name>_depth.f32` (float32, row by row). Run on another
  PC's GPU and compared with a replay here, it shows the first stage where
  the two differ.
- `--no-msaa` draws with single-sample render targets, ignoring the guest's
  sample masks like the SDK's emulation does.
- `--depth-bias-scale k` multiplies the polygon offset (diagnostics).
- `--diag a,b` turns on diagnostic variants: `nobias` (no depth bias in the
  pipelines at all), `clamp` (depth clamp on), `list` (non-indexed triangle
  strips drawn as triangle lists). With `--diag-draws a-b` (1-based draw
  numbers) two more apply to those draws only: `show` (colour writes on,
  depth and stencil tests off: shows where a draw lands), `nodepth` (depth
  test off), `novtex` (vertex-shader textures replaced by a blank one) and
  `vtexplain` (vertex-shader textures sampled with a plain point sampler).
  `posdump` makes vertex shaders store each vertex's position (before and
  after the viewport transform), its index and fetch constant to a buffer,
  written to `step_<n>\posdump.txt`; use it with a single draw.
- `--diag swap0` / `swap2` / `swap3` (whole frame) switch the shaders' byte
  swap for guest memory from the default (form 1) to another formulation (see
  `TranslateOptions::swap_variant`; forms 0 and 3 are miscompiled by an
  Intel Arc driver).
- With `--glslang internal` the translated shaders are saved in `<out>\glsl\`.
- `--scale n` draws at n times the guest resolution (the game's Render scale
  setting); `--aniso n` overrides anisotropic filtering (0 off ... 5 16x).
- `--upscaler xess|fsr|test --output-scale m` draws the scene at `--scale`
  and upscales it to m times the guest resolution before the HUD (`test` is
  a plain stretch through the same path, for checking it without the
  vendors' DLLs). The upscaler's status is printed.
- `--repeat n` draws the frame n times with the same renderer, which tests
  its frame-to-frame caches the way the game uses them, and prints each
  repeat's CPU times (vertex data and setup, recording the draws) and its
  first-use work: textures decoded and uploaded, pipelines and shaders
  built (what makes a frame hitch; the first frame is a cold start).
- `--pipeline-cache file` loads and saves the Vulkan pipeline cache there
  (the game keeps `logs\native_pipeline_cache.bin`); `--async-pipelines`
  builds pipelines in the background as the game does with Background
  shader compilation on (draws wait for theirs; counted per frame).
- `--diag profile` times every frame's draws by part (printed with `--repeat`;
  the game samples every 30th frame and logs it); `--diag noixcache` converts
  index buffers per draw instead of keeping them in the vertex arena.
- `--diag nobc` decodes compressed textures on the CPU instead of uploading
  their blocks; `--diag noprefetch` turns off the parallel texture decode;
  `--diag hardalpha` uses the GPU's own alpha to coverage instead of the
  smooth, console-like foliage edges; `--diag nofade` turns off the fade of
  branches near the camera (toward the middle of the screen).
- `--motion-blur s` with `--object-blur` (the characters) or
  `--object-blur-scenery` (every scene draw too: a diagnostic, not offered in
  the game since it costs a few ms of CPU a frame) turns on motion blur.
- `--fog d`, `--god-rays s` and `--speed-blur a` turn on the scene effects
  (the last at a fixed share of full speed).
- `--async-with <other capture>` draws this capture and another one
  alternately through an async renderer (as consecutive game frames) and
  compares each output with the same frame drawn by a fresh renderer.
- `--output-test` checks the presentation output (`Renderer::DrawOutput`):
  the GPU's gamma table and A2B10G10R10 packing against the CPU path the
  game used before, and (Linux, where the device can export memory) the
  shared output image imported into a second Vulkan device, that drawing a
  slot again keeps its image, and three frames through an async renderer
  (as the game runs it).
- `--glslang internal` compiles shaders with the glslang library the game
  uses (when the build has it) instead of glslangValidator.

**Captures** have to come from a build that records `ucode`, `fetch`, `vmem`
and `targets` lines (`frame-capture.md`). Older captures replay as nothing.

## How a frame is replayed

**Vertex data.**

- The capture saves the memory each vertex fetch constant points at
  (`mem_<physical>_<bytes>.bin`).
- The replayer packs those files into one storage buffer and rewrites each
  draw's fetch constant addresses to match.
- Vertex shaders read the data themselves, as in the game: endian swap and
  format unpack happen in the shader.

**Indices.** Index buffers are converted on the CPU to 32-bit little-endian.
0xFFFF becomes the restart index.

**Constants.**

- Each draw's float, bool/loop and fetch constants, plus the XeSystem block,
  go into a uniform ring and are bound with dynamic offsets.
- XeSystem gets the viewport transform (PA_CL_VTE_CNTL, PA_CL_VPORT_*), the
  Direct3D half-pixel offset (PA_SU_VTX_CNTL), the vertex index offset
  (VGT_INDX_OFFSET), the alpha test (RB_COLORCONTROL, RB_ALPHA_REF) and
  ps_param_gen.

**Render targets.**

- There is one image per EDRAM base, format and pitch (RB_SURFACE_INFO,
  RB_COLOR_INFO / RB_COLOR1..3_INFO, RB_DEPTH_INFO). Each is full size: the
  frame is drawn once rather than per EDRAM tile, and the 10 MB EDRAM limit
  doesn't apply.
- Multiple render targets are used when the pixel shader writes oC1..3.

**Render state.**

| Xenos registers | Vulkan state |
|---|---|
| RB_DEPTHCONTROL | depth and stencil |
| RB_STENCILREFMASK(_BF) | stencil reference and masks |
| RB_BLENDCONTROL0..3 | blending |
| RB_BLEND_RED..ALPHA | blend constants |
| RB_COLOR_MASK | write masks |
| PA_SU_SC_MODE_CNTL | culling, front face, primitive restart |
| PA_SC_SCREEN_SCISSOR | scissor |

**Clears and resolves.**

- `ClearF` clears the flagged targets.
- A resolve copies a rectangle of a render target into an image keyed by the
  destination texture's address.
- Depth goes into an R32F image. A depth resolve also keeps the raw EDRAM
  depth/stencil words as an 8_8_8_8 image, used when a draw reads the same
  address with a color format. Amped 3 does this for its sun shadow map: the
  depth+stencil is read back as k_8_8_8_8, blurred, and the stencil byte is
  the shadow mask.
- Draws that later sample that address get the image instead of guest memory.
  This is a first version of step 5.
- A resolve's own clear flags (0x100 color, 0x200 depth) are applied.

**Textures.**

- Decoded on the CPU to RGBA8, with the fetch constant swizzle applied.
- sRGB when the fetch constant's sign is gamma.
- Samplers come from the fetch constant's clamp and filter fields and the
  instruction's overrides.
- The mip chain is decoded too (levels laid out as in the SDK's
  GetGuestTextureLayout: power-of-two strides, tile and 4 KB alignment, the
  smallest levels packed into one tile). The sampler uses the fetch
  constant's mip filter and level range.
- Small tiled textures with packed mips (16 texels or less on the shorter
  side) are read from their offset inside the shared mip-tail tile.
- Captures made before 2026-10-03 18:30 saved too little of the mip chain
  for small textures; those textures stop at the last level that was saved.

**Shaders** are recompiled whenever the translation changes. To try a
hand-edited shader, edit `spv\<stem>.frag` (or `.vert`), compile it to
`<stem>.spv` with glslangValidator and create an empty `<stem>.keep` next to
it: the replayer then uses it as is.

## Known gaps (stage 1)

- **Textures:** 3D, cube and k_24_8_FLOAT textures are
  placeholders.
- **Primitives:** rectangle and quad lists are skipped. Amped 3's captured
  frames use only triangle lists.
- **Not emulated yet:**
  - MSAA (everything is rendered at 1 sample; per-sample passes such as the
    lighting pass, drawn once per sample with PA_SC_AA_MASK, just redraw the
    same pixels);
  - alpha-to-coverage is approximated: a fragment is kept when alpha >= 0.5;
  - points use PA_SU_POINT_SIZE or the exported size; point sprite texture
    coordinates come from gl_PointCoord;
  - polygon offset follows the SDK's conversion (float24 offsets in ULPs of
    the primitive's depth, slope in 1/16 pixel);
  - the window offset;
  - the Xenos gamma curve (standard sRGB is used instead);
  - render target exponent bias.
- **Render targets:** two surfaces sharing EDRAM space are separate images,
  so data the game passes through EDRAM aliasing is lost.
