# Native renderer: shader translation (step 3)

The native renderer runs the game's Xenos shaders as Vulkan shaders.
`src/native/xenos_shader.cpp` turns Xenos microcode into GLSL 4.50, which
glslang compiles to SPIR-V. The same code serves two purposes:

- **Offline:** `amped3-shader-translate`, built with the game, translates
  captured shaders and checks them.
- **In the game:** step 4 uses it as the translator for shaders seen at draw
  time.

The bit layouts follow `rex/graphics/format/ucode.h`. Instruction behaviour
follows the comments there and the SDK's own SPIR-V translator. The code
itself has no SDK dependency.

## Which microcode to translate

The blobs the game passes to `CreateVertexShader` are **templates**:

- **Vertex shaders:** every vertex fetch has stride 0 and a placeholder
  constant (vf95 counting down). The 79 compiled blobs also have format 0;
  the 12 assembled at run time by D3DXAssembleShader carry a format of
  32_FLOAT. `D3D_CommitShaders` (825A8C98) fills them in for the current
  declaration with `D3D_PatchVertexShaderFetches` (825A8250, arguments
  device, shader, microcode, declaration). It does that in one of two places:
  - In the shader's own microcode (object +40, size +600), when the GPU is
    done with it.
  - Otherwise, in a copy: `D3D_LoadVertexShaderImmediate` (825A8830) copies
    the microcode into the command buffer (IM_LOAD_IMMEDIATE) and patches the
    copy. The shader's own microcode is left as it was, sometimes still the
    unfilled template.

  So the patched code is only reliably visible right after
  `D3D_PatchVertexShaderFetches` returns. That is where the capture saves it,
  and where the renderer will pick it up.
- **Pixel shaders:** loaded unchanged from object +12 (size +60). The
  `ps_1_x`-style `tex` fetches (D3DXAssembleShader shaders) really are 1D
  fetches in the microcode the GPU runs. The SDK's emulation treats them as
  1D too, so the translator does the same.

Translations are keyed by `HashMicrocode()` (FNV-1a of the host-order
dwords). The `capture_shaders` setting saves exactly this microcode as
`logs\shaders\ucode_vs_<hash>.bin` / `ucode_ps_<hash>.bin`, one file per
distinct patched version (see `frame-capture.md`).

## Interface with the renderer

A translated shader expects the following resources.

| Set | Binding | Contents |
|---|---|---|
| 0 | 0 | `XeSystem` uniform block (below), shared by both stages |
| 0 | 1 | Vertex shader float constants `c0..c255` (device+1920), `vec4[256]` |
| 0 | 2 | Pixel shader float constants `c0..c255` (device+6016), `vec4[256]` |
| 0 | 3 | Bool and loop constants (device+10112): `uvec4 b[2]` (b0–b255), `uvec4 l[8]` (i0–i31), raw |
| 0 | 4 | Fetch constants (device+1152): 32 × 6 dwords as `uvec4[48]`, raw. Vertex fetch constant vfN = dwords 2N, 2N+1. |
| 1 | 0 | Vertex shaders: guest physical memory as one `uint[]` storage buffer (byte address / 4) |
| 2 | n | Vertex shader textures: combined image samplers, `n` = `ShaderInfo::textures[i].binding` |
| 3 | n | Pixel shader textures, same scheme |

**Vertex input.** There is no fixed-function vertex input.

- Vertex shaders read vertices themselves from set 1, the way the Xenos does:
  address from the fetch constant, endian swap from its low bits, then format
  unpacking. Any declaration and any Xenos vertex format therefore works, with
  no conversion.
- `r0.x` holds the vertex index, computed as
  `(gl_VertexIndex + vertex_index_offset) & vertex_index_mask`.
- With `XE_FLAG_LOAD_INDEX`, the index is first read from guest memory at
  `index_load_address` (16- or 32-bit, with endian swap). This lets big-endian
  index buffers be drawn without conversion.

**Interpolators.**

- The vertex shader always writes locations 0–15. Ones the shader leaves
  unwritten are 0.
- The pixel shader reads locations 0..N−1 into `r0..rN−1`.
- `ps_param_gen` goes into register `param_gen_register` when that is below 16.

**Outputs.**

- **Position:** the exported position goes through the epilogue below.
- **Colors:** `oC0..oC3` go to locations 0–3. Only the written ones are
  declared; see `ShaderInfo::colors_written`.
- **Depth:** `oDepth` goes to `gl_FragDepth`, clamped to 0..1.
- **Alpha test:** done in the pixel shader epilogue (`alpha_func`, `alpha_ref`),
  because Vulkan has none.

**XeSystem block (std140)**

| Member | Use |
|---|---|
| `vec4 ndc_scale`, `vec4 ndc_offset` | `pos.xyz = pos.xyz * ndc_scale + ndc_offset * pos.w`. Viewport-transform-off cases and the half-pixel offset are folded in here. Identity for normal clip-space output. |
| `uint flags` | `XE_FLAG_XY_DIVIDED` (1), `XE_FLAG_Z_DIVIDED` (2), `XE_FLAG_W_RECIPROCAL` (4) mirror PA_CL_VTE_CNTL. Also `XE_FLAG_LOAD_INDEX` (8), `XE_FLAG_INDEX_32BIT` (16), index endian in bits 5–6, `XE_FLAG_POINT` (128). |
| `uint vertex_index_offset`, `vertex_index_mask` | VGT_INDX_OFFSET and the 24-bit index mask |
| `uint index_load_address` | Byte address of the index buffer when the shader loads indices |
| `uint alpha_func`, `float alpha_ref` | RB_COLORCONTROL alpha test; 7 = always |
| `uint param_gen_register` | SQ_CONTEXT_MISC param_gen_pos when SQ_PROGRAM_CNTL.param_gen is set, else ≥ 16 |
| `float point_size` | Used when a point list doesn't export a size |

**Textures.**

- 1D textures are bound as 2D images that are 1 texel high.
- The fetch constant decides the format conversion, swizzle, sign and gamma.
  These belong to the image view the renderer creates, not to the shader.
- Filter overrides in the instruction give a separate binding with the
  override recorded, so the renderer can make a matching sampler.

## How the translation works

- **Guest state:** registers are a `vec4 r[N]` array. `p0`, `a0`, the `aL`
  stack, the loop counter stack and the previous scalar result are globals.
  The program body sits in `xe_program()`, and `main()` adds the prologue and
  epilogue.
- **Control flow:**
  - Shaders without jumps, calls or loops become straight-line code. Execs
    with conditions become `if`s, and `exece` becomes `return`.
  - Otherwise the body runs as `switch (pc)` inside a bounded loop. The cases
    are the jump, call and loop targets.
- **ALU:**
  - The vector and scalar halves read all their operands before either half
    writes.
  - Multiplication follows the Direct3D 9 rule that 0 × anything is 0 (in
    `mul`, `mad`, `dp*` and `dst`). Results are `precise`, so nothing is fused.
  - Export masks follow the Xenos rules (vector, scalar, constant 0, constant
    1).
- **Kill:** uses `demote` (GL_EXT_demote_to_helper_invocation) so derivatives
  stay valid.
- **Listing:** every instruction appears as a comment in the GLSL and in the
  `.txt` listing.

**Approximations**, recorded per shader in `ShaderInfo::approximations`:

- `getBCF` returns 0.
- `getWeights` ignores the mip level.
- An LOD bias is dropped when register gradients are used.
- Texel offsets are applied at mip 0.
- Half floats use IEEE rules, not the Xenos extended range.
- Interpolator flat shading and centroid sampling are not handled yet.
- A vertex fetch still in template form (format 0) reads 0, with 1 where the
  swizzle asks for a constant 1. This is only a safety net; patched
  microcode shouldn't contain any.
- Memory export is reported as an error; none of the shaders use it.

## The offline tool

```
amped3-shader-translate <logs\shaders folder | file.bin> ... -o <out folder>
                        [--glslang glslangValidator.exe] [--spirv-val spirv-val.exe] [--blobs]
```

**Output files.** For each shader the tool writes:

- `<name>.vert` or `<name>.frag`: the GLSL.
- `<name>.txt`: the listing and an interface summary (constants, fetch
  constants, formats, textures, exports).
- `<name>.spv`: compiled for Vulkan 1.1, when glslang is given.
- `summary.txt`: one line for every shader, plus totals.

**Inputs.** Folders contribute their `ucode_*.bin` files. `--blobs` adds the
creation blobs, and translates their placeholder vertex fetches as 4 × float32
so everything else in them can still be checked.

**On Windows.** `tools\shader-translate\translate.ps1` finds the tool in
`out\build\...`, and finds glslangValidator and spirv-val in the Vulkan SDK.
It then translates the newest build's `logs\shaders` into
`logs\shaders\translated\`.

## Results so far

All **184** creation blobs from the first capture session translate. They
compile with glslang (Vulkan 1.1) and pass `spirv-val`: 91 vertex shaders and
93 pixel shaders. The vertex shaders are templates, so their fetch addresses
and formats are not meaningful yet: 79 went through the placeholder option,
and 12 have stride-0 fetches.

Spot checks against the game's own assembly sources (`asm_*.txt`) give the
same operations, swizzles, constant indices and interpolator assignments. One
example is the four-light vertex lighting shader `vs_EDC5973E293356FF`.

Features in use across the dumped shaders:

- **Control flow:** no jumps, loops or bool constants (straight-line programs).
- **Pixel kill:** 4 pixel shaders.
- **Relative addressing:** `c[a0+n]` constant indexing in 10 pixel shaders.
- **Textures:** 2D only, plus the three `tex` templates that say 1D.

First drawn-microcode run (menus and a ride, 52 shaders): 49 valid. The 3
failures were vertex shaders saved unpatched: those shaders were patched in
command-buffer copies, which the draw-time save couldn't see. Vertex shaders
are now saved from the patch function instead.

Second run with the patch-function capture (menus and a ride): all **68**
drawn shaders (23 pixel, 45 vertex) translate, compile and pass `spirv-val`,
with no template fetches left. Step 3's "done when" is met.
