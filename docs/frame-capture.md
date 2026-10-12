# Frame capture

A recording of what the game asks Direct3D to do in one frame, plus the
shaders it creates. These captures are the reference data for the native
renderer: they check the device layout in `d3d-map.md` against the running
game, and later serve as test cases.

## Capturing a frame

1. Build as usual (`setup.ps1 -SkipSdk`). The Direct3D function names are
   always applied now, because the capture hooks those functions by name.
2. In game, press **F10** at the moment you want. The next complete frame
   (from one present to the next) is written to `logs\frame_<date>-<time>.txt`
   next to the exe, and the game log says how many draws it contained.
3. Take a screenshot of the same moment. The Direct3D 12 renderer is the
   best reference, since it has neither of the Vulkan issues.

Each capture is a few hundred KB to a few MB of text. The game hitches
briefly while the file is written.

Scenes worth one capture each: an intro FMV, the main menu, the load-a-rider
screen, the mountain map, normal riding (with snow spray and other riders in
view if possible), and anything that looks like render-to-texture (replays,
photo mode, the TV-style screens on the loading pages).

## Resource data

With `capture_resources` on (F4 panel, Amped 3/Debug, on by default), each
capture also gets a folder `logs\frame_<date>-<time>\` holding what the draws
read, exactly as it is in game memory:

| File | What |
|---|---|
| `tex_<hash>.base.bin`, `.mips.bin` | Texture base level and mip chain (tiled, big-endian, as the GPU reads them), keyed by the fetch constant. The mip file covers the whole chain as laid out by the SDK (larger than the base level for small textures) |
| `vb_<address>_<size>.bin`, `ib_…` | Vertex and index buffers |
| `upv_…`, `upi_…` | Vertex and index data passed by pointer (`…UP` draws) |
| `resources.txt` | One line per file: fetch constant or buffer words, sizes, addresses |

Draw lines in the frame file name the files they use (`data=tex_…`), and list
the shader constants that changed since the previous draw (`vs_consts`,
`ps_consts`, `bool_loop`). A capture saves at most 768 MB of resource data; a
riding frame is typically a few hundred MB, so zip the folder before sending.

## Shaders

With the setting `capture_shaders` (F4 panel, category Amped 3/Debug, on by
default), every shader the game creates is saved to `logs\shaders\`, once
each:

| File | What |
|---|---|
| `vs_<hash>.bin`, `ps_<hash>.bin` | The compiled shader exactly as passed to CreateVertexShader / CreatePixelShader: header followed by Xenos microcode, big-endian |
| `ucode_vs_<hash>.bin`, `ucode_ps_<hash>.bin` | The microcode as the GPU ran it, raw, big-endian. Vertex shaders are saved when Direct3D fills in their vertex fetches for a declaration (sometimes in a command-buffer copy), pixel shaders at draw time. This differs from the blob; one file per distinct patched version. The hash is the one the native renderer uses. |
| `asm_<hash>.txt` | Assembly source the game passes to D3DXAssembleShader at run time |
| `index.txt` | One line per creation or newly seen drawn microcode: file, guest object address, source address or blob / declaration |

Play through a few different areas, then zip the whole `shaders` folder.
`tools\shader-translate\translate.ps1` translates the `ucode_` files to GLSL
and SPIR-V and validates them (see `native-shaders.md`).
Frame captures refer to shaders by the same names (`vs=1A2B3C4D(vs_…)`).

## What a frame file contains

```
[12] DrawIndexedVertices prim=4 base_vertex=0 start_index=0 count=1530
  dirty=… … … … … flags=0000
  vs=… vs_set=… ps=… decl=…
  rt0=… ds=…
  stream0: vb=… offset=0 stride=32
  ib=…
  tex0=… type=2 dim=1 fmt=18 endian=2 512x512 pitch=512 tiled=1 base=… mips=… raw=[…]
  viewport=0 0 1280 720 0.000 1.000 scissor=0 0 1280 720
  regs r2200=… r2201=…
  ucode vs=ucode_vs_… ps=ucode_ps_…
  fetch f190=… f191=…
  vmem vf95=mem_177F88C0_128
```

- **Events** are numbered in call order: draws (including `BeginVertices`,
  the game's direct calls to D3D_DrawVerticesUP_Internal 825A0B60 for its
  full-screen post-processing quads; their vertices are saved as `upv_` files
  once the game has written them), `ClearF`, `Resolve`,
  `BeginTiling`/`EndTiling`, `SetRenderTarget`, `SetDepthStencilSurface`, and
  the closing `Swap`.
- **Draw state** is read from the device right after the draw call, once D3D
  has flushed it (vertex fetch constants and the index offset are only
  filled in during the draw), so it is what the GPU receives for that draw.
- **`ucode`** names the microcode the GPU has loaded (files in
  `logs\shaders`). **`fetch`** lists the fetch constant dwords (32 × 6 at
  device+1152; vertex constant vfN = dwords 2N, 2N+1) that changed since the
  previous draw. **`vmem`** names the saved memory each vertex fetch constant
  the shader uses points at, `mem_<physical address>_<bytes>.bin`.
- **`ClearF`** is followed by `targets`: RB_SURFACE_INFO / RB_COLOR_INFO /
  RB_DEPTH_INFO at the time of the clear.
- **`regs`** lists the GPU register copies (register blocks 0x2000–0x23FF)
  that changed since the previous draw; the first draw lists all of them.
  Register numbers are Xenos register indices, as in the SDK's
  `register_table.inc`.
- **`object` lines** give the raw words of each surface, vertex buffer,
  index buffer, declaration and texture header the first time it appears in
  the frame. Their layouts are only partly known; these lines are how the
  rest gets worked out.
- **Texture lines** decode the main fields of the Xenos fetch constant
  (format number, size, tiling, base address). The raw six words follow, so
  nothing depends on the decode being right.

At most 20,000 events are recorded per frame.
