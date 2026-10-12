# Amped 3: Direct3D layer map

This maps the Xbox 360 Direct3D runtime that is statically linked into Amped 3's
`default.xex` (XDK 2.0.2099, November 2005). The xex has no symbols, so every
name here was reconstructed from behaviour. Each function has a confidence
level and the evidence it rests on, so anything shaky can be rechecked.

This is phase 1 of a native renderer: finding where the game talks to the GPU.
Phase 2 (actually rendering through Vulkan) builds on the hook points listed
at the end.

## How the game uses Direct3D

The usual picture of a 360 game is "D3D calls everywhere, half of them
inlined". Amped 3 is much tidier than that:

- **About 75 D3D functions are called from game code at all.** They cover
  device creation, resource creation and release, the non-inline setters
  (textures, streams, indices, shaders, declarations, render targets,
  viewport, scissor), the four draw calls, clear, resolve, tiling and
  present.
- **Render states and sampler states go through two dispatch tables**, not
  inline writes. The tables live in `.data` at `0x82850E40` (89 render states)
  and `0x8285126C` (20 sampler states). Each entry is a pair of tiny get/set
  functions plus a default value. Apart from D3D's own default-state setup,
  the code that indexes these tables is in the D3DX library, which suggests
  most states arrive through D3DX effects (`.fx` passes).
- **Only a handful of game functions write device fields directly** (four
  writes into the register shadow range were found in the whole binary).
- **Every draw first calls `D3D_FlushDirtyState` (`825A9268`).** At that
  moment the device object holds a shadow copy of every GPU register the
  draw depends on, plus the bound textures, streams, index buffer, shaders and
  render targets. A native renderer can hook the four draw calls, read the
  device object, and never needs to see the inlined code.

So per-function hooking of inlined state changes, the expensive part of the
plan, is not needed for this game. The state is collected at draw time instead.

## Where things are

| Range / address | What |
|---|---|
| `0x82593000`–`0x825AEF00` | D3D runtime (about 320 functions) |
| `0x8259D1F8`–`0x8259F700` | Render/sampler state get/set leaves. They are not in `.pdata`; the static scan registered them from the tables. |
| `0x825AEF10`– | D3DX (shader assembler/compiler, effects) |
| `0x82850E40` | Render-state table: 89 × `{Get, Set, Default}` |
| `0x8285126C` | Sampler-state table: 20 × `{Default, Get, Set}` |
| `0x82000648` / `0x8200064C` | Pointers to the global device pointer (title / system process) |
| `0x820007A0` | Device caps block (1104 bytes) |
| `0x8207A5BC` | Vtable of the game's own device wrapper (11 slots) |

## The device object (0x5080 bytes)

All offsets are decimal, from the start of the object `Direct3D_CreateDevice`
returns. "Shadow" means a copy of GPU registers in Xenos format, laid out
register by register.

| Offset | Contents | Evidence |
|---|---|---|
| +0, +8 | Command buffer write pointer, limit | Every packet writer; `D3D_MakeSpace` when +0 ≥ +8 |
| +16, +24, +32, +40, +48 | 64-bit dirty masks | `D3D_FlushDirtyState` |
| +1152 | Fetch constants, regs 0x4800+ (32 × 24 bytes) | SetTexture writes +1152+24·sampler |
| +1920 | Float constants, regs 0x4000+ (vertex shader, 256 × 16 bytes) | Flush, dirty mask +16 |
| +6016 | Float constants, regs 0x4400+ (pixel shader) | Flush, dirty mask +24 |
| +10112 | Bool/loop constants, regs 0x4900+ | Flush |
| +10384 | Pointer to the GPU-written fence value | `D3D_BlockOnFence` |
| +10396 | Current CPU fence | Resource release stamps it |
| +10432, +10433 | Flag bytes (predicated tiling active, etc.) | Draws, flush |
| +11408 | Current vertex declaration | SetVertexDeclaration |
| +11456 | Register shadow, block 0x2000 (RB_SURFACE_INFO, RB_COLOR_INFO, …) | Flush |
| +11532 | Register shadow, block 0x2100 (VGT_*; +11540 = VGT_INDX_OFFSET) | Flush; DrawVertices |
| +11616 | Register shadow, block 0x2180 (SQ_PROGRAM_CNTL, …) | Flush |
| +11636 | Register shadow, block 0x2200 (RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL, PA_SU_SC_MODE_CNTL, …) | Flush; render-state setters |
| +11684 | Register shadow, block 0x2280 | Flush |
| +11768 | Register shadow, block 0x2300 | Flush |
| +11920 | Register shadow, block 0x2380 | Flush |
| +12046, +12072, +12098 | Per-sampler state bytes merged into fetch constants | SetTexture |
| +12528 | Current FVF | SetFVF |
| +12532 | Index buffer | SetIndices |
| +12536 | Render targets [4] | SetRenderTarget |
| +12552 | Depth/stencil surface | SetDepthStencilSurface |
| +12556 | Streams [n]: {OffsetInBytes, pVertexBuffer}, 8 bytes each | SetStreamSource |
| +12688 | Stream stride ÷ 4, one byte per stream | SetStreamSource |
| +12704 | Textures [sampler] | SetTexture |
| +12808 | Viewport (D3DVIEWPORT9, 6 dwords) | SetViewport, Present |
| +12832 | Scissor rect | SetScissorRect, Present |
| +12848 | Clip planes [n], 16 bytes each | SetClipPlane |
| +12944 | Current pixel shader | SetPixelShader, CommitShaders |
| +12948 | Current vertex shader as loaded (declaration-patched copy) | CommitShaders |
| +12972.. | Tiling state (tile count, rects) | BeginTiling |
| +13520 | Secondary command buffer used during predicated tiling | DrawVertices |
| +13944 | Front buffer texture | Present |
| +13948 | Back buffer surface | Present |
| +13960 | Front buffer handed to the last Swap | Swap |
| +20440 | Vertex shader as set by the game | SetVertexShader |
| +20560 | BehaviorFlags | CreateDevice |

**Resources** start with `Common` at +0. Its low 8 bits are the reference
count; higher bits count bindings, which the setters adjust in steps of
0x80000. The
fence is at +4. Texture headers carry the Xenos fetch constant at +16
(24 bytes); +20 is the base address and +36 the mip address. Surfaces created
from a texture point to their parent at +44.

## Functions

Confidence: **high** means the behaviour pins the name (only caller of a kernel
call, unique packet, args confirmed at call sites). **medium** means role and
arguments are clear but the exact XDK name is inferred. **low** means the role is
plausible but unconfirmed.

| Address | Name | Arguments | Conf. | Evidence | Hook note |
|---|---|---|---|---|---|
| `825A43D8` | `Direct3D_CreateDevice` | `(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice)` | high | Allocates the 0x5080-byte device (128-byte aligned), stores BehaviorFlags at +0x5050, calls device init; returns E_OUTOFMEMORY 0x8007000E. Only caller is the game renderer init 824A6FC0. | Device creation seam: create the native device/swapchain here. |
| `825AAED8` | `D3D_InitializeDevice` | `(pDevice, pPresentationParameters)` | high | Only caller of VdInitializeEDRAM / VdIsHSIOTrainingSucceeded; calls ring-buffer and engine init. |  |
| `825AA940` | `D3D_InitializeEngines` | `(pDevice)` | high | Calls VdInitializeEngines and VdSetGraphicsInterruptCallback. |  |
| `825A2980` | `D3D_GraphicsInterruptCallback` | `(source, pDevice)` | high | Address the runtime log reports for SetInterruptCallback(825A2980, ...). |  |
| `825A3EC0` | `D3D_InitializeRingBuffer` | `(pDevice)` | high | Only caller of VdInitializeRingBuffer / VdEnableRingBufferRPtrWriteBack; emits ME_INIT. |  |
| `825AB0F0` | `D3D_ShutdownDevice` | `(pDevice)` | high | Calls VdShutdownEngines and clears the interrupt callback. |  |
| `825A4330` | `D3D_DestroyDevice` | `(pDevice)` | medium | Shutdown + free of the device block; called on CreateDevice failure and by game teardown. |  |
| `825AAA00` | `D3D_SetDefaultStates` | `(pDevice)` | medium | Walks the render-state table at 0x82850DE0 and calls every setter with its default. |  |
| `825931C0` | `Direct3D_GetDeviceCaps` | `(Adapter, DeviceType, pCaps)` | medium | Copies a fixed 1104-byte block (D3DCAPS9 size) from 0x820007A0. |  |
| `825A3BA0` | `D3D_MakeSpace` | `(pDevice)` | high | Called (52 sites) whenever device+0 (write pointer) reaches device+8 (limit); kicks the segment and waits on fences. | Not needed by a native renderer; command buffer is bypassed. |
| `825A3818` | `D3D_ReservePredicated` | `(pTilingBuffer)` | medium | Reserve in the secondary buffer at device+13520 used while predicated tiling is active. |  |
| `825A3550` | `D3D_BlockOnFence` | `(pDevice, Fence, ...)` | high | Spins until *(device+10384) (GPU-written fence) passes Fence. | Native fences replace this. |
| `825A3DF0` | `D3DDevice_BlockUntilIdle` | `(pDevice)` | medium | Inserts a fence (type-0 write to reg 1480) and blocks on it. |  |
| `825A37B8` | `D3DResource_BlockUntilNotBusy` | `(pResource)` | medium | Blocks on the resource fence at +4 using the global device. |  |
| `825AB500` | `D3D_WaitForPresentInterval` | `(pDevice)` | low | WAIT_REG_MEM on a vblank counter; called from Present and two game sites. |  |
| `825A9268` | `D3D_FlushDirtyState` | `(pDevice)` | high | Called at the start of every draw. Walks the 64-bit dirty masks at device+16..+48 and writes each dirty register block from its device shadow. | Key seam: at this point the device shadows hold the complete pipeline state for the draw. |
| `825A8C98` | `D3D_CommitShaders` | `(pDevice)` | high | Reads current VS (device+12948), PS (+12944) and vertex declaration (+11408); emits IM_LOAD / INVALIDATE_STATE. |  |
| `825A8130` | `D3D_CommitVertexFetch` | `(pDevice)` | medium | Builds vertex fetch constants from streams (+12556) and declaration (+11408). |  |
| `825A7E58` | `D3D_WriteRegisterBlock` | `(pDevice, Mask, FirstRegister, pShadow)` | medium | Type-0 packet writer used by the flush for each register block. |  |
| `825A8008` | `D3D_WriteAluConstants` | `(pDevice, Mask, FirstRegister, pShadow)` | medium | Writes float constants 0x4000/0x4400 from device+1920 / +6016. |  |
| `825A7F18` | `D3D_WriteFetchConstants` | `(pDevice, Mask, FirstRegister, pShadow)` | medium | Writes fetch constants 0x4800 from device+1152. |  |
| `825ABC50` | `D3DDevice_Present` | `(pDevice)` | high | Saves viewport/scissor, sets back buffer (device+13948), resolves it into the front buffer (device+13944), restores state, calls Swap. | Present seam #1. |
| `825AB670` | `D3DDevice_Swap` | `(pDevice, pFrontBuffer, pSwapParameters)` | high | Only caller of VdSwap; stores the new front buffer at device+13960. | Present seam #2. The game calls this directly (via 824A6D08), not Present. |
| `825A1278` | `D3DDevice_DrawVertices` | `(pDevice, PrimitiveType, StartVertex, VertexCount)` | high | StartVertex goes to VGT_INDX_OFFSET (device+11540); emits DRAW_INDX header 0xC0012201 (auto-index). | Draw seam. |
| `825A14B8` | `D3DDevice_DrawIndexedVertices` | `(pDevice, PrimitiveType, BaseVertexIndex, StartIndex, IndexCount)` | high | Like DrawVertices but reads the index buffer (device+12532); emits 0xC003xxxx DRAW_INDX with index base/size. | Draw seam. |
| `825A0E00` | `D3DDevice_DrawVerticesUP` | `(pDevice, PrimitiveType, VertexCount, pVertexData, Stride)` | medium | Copies user vertices into the command buffer, then calls 825A0B60. | Draw seam (user pointer). |
| `825A0B60` | `D3D_DrawVerticesUP_Internal` | `(pDevice, PrimitiveType, VertexCount, Stride)` | medium | Shared tail of the UP draws. |  |
| `825A1198` | `D3DDevice_DrawIndexedVerticesUP` | `(pDevice, PrimitiveType, MinIndex, NumVertices, IndexCount, pIndexData, IndexFormat, pVertexData, Stride)` | medium | UP indexed variant; calls 825A0E88. | Draw seam (user pointer). |
| `825A0E88` | `D3D_DrawIndexedVerticesUP_Internal` | `` | medium | Shared tail of the indexed UP draw. |  |
| `825A25F0` | `D3DDevice_Clear` | `(pDevice, Count, pRects, Flags, Color, Z, Stencil)` | high | Converts the ARGB colour to a float4 and calls ClearF once per rect. | Clear seam. |
| `825A0B60` | `D3D_DrawVerticesUP_Internal` (BeginVertices) | `(pDevice, PrimitiveType, VertexCount, Stride)` → vertex pointer | high | Flushes state, emits the draw and returns command-buffer space for the vertices. Called by DrawVerticesUP and directly by the game (8218C894, 8243420C) for its post-processing quads. | Draw seam for those quads. |
| `825A1F30` | `D3DDevice_ClearF` | `(pDevice, Flags, pRect, pColor, Z, Stencil)` | high | Draws a clear quad with immediately-loaded shaders (IM_LOAD_IMMEDIATE). | Clear seam. |
| `825A6CA8` | `D3DDevice_Resolve` | `(pDevice, Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel, DestSliceOrFace, pClearColor, ClearZ, ClearStencil, pParameters)` | high | Largest D3D function; programs RB_COPY_* and draws the copy; called by Present, EndTiling and 4 game sites. | Resolve seam: becomes a native RT-to-texture copy. |
| `825A0528` | `D3DDevice_BeginTiling` | `(pDevice, Flags, Count, pTileRects, pClearColor, ClearZ, ClearStencil)` | medium | Stores tile count/rects at device+12972.., starts predicated tiling (EVENT_WRITE), clears. | Predicated tiling only exists because of 10 MB EDRAM; a native renderer can turn these into a single pass. |
| `825A0850` | `D3DDevice_EndTiling` | `(pDevice, Flags, pResolveRects, pDestTexture, pClearColor, ClearZ, ClearStencil, pParameters)` | medium | Ends tiling and resolves each tile (calls Resolve). |  |
| `825A0458` | `D3D_SetTilePredication` | `(pDevice, ...)` | low | SET_BIN_MASK packet; used by EndTiling and two game sites. |  |
| `82597BD8` | `D3DDevice_SetTexture` | `(pDevice, Sampler, pTexture)` | high | Stores pTexture at device+12704+4*s, copies the texture fetch constant (texture+16, 24 bytes) into device+1152+24*s merged with sampler state, sets the fetch dirty bit. | State seam (or read device+12704 at draw time). |
| `8259F9C8` | `D3DDevice_SetStreamSource` | `(pDevice, StreamNumber, pVertexBuffer, OffsetInBytes, Stride)` | high | Stream table at device+12556+8*n {Offset, pVB}; Stride/4 as a byte at device+12688+n; sets dirty bit 0x400 at +48. |  |
| `8259FA68` | `D3DDevice_SetIndices` | `(pDevice, pIndexBuffer)` | high | Stores at device+12532 with bind-count/fence bookkeeping. |  |
| `82599D08` | `D3DDevice_SetVertexShader` | `(pDevice, pVertexShader)` | medium | Stores at device+20440, then 82599BB0 binds the declaration-patched copy at device+12948. |  |
| `82599BB0` | `D3D_BindVertexShaderForDeclaration` | `(pDevice, pVertexShader)` | medium | Writes device+12948 (the shader actually loaded by CommitShaders). |  |
| `82599998` | `D3DDevice_SetPixelShader` | `(pDevice, pPixelShader)` | high | Stores at device+12944, AddRef/Release, sets dirty bits 0x120 at +48. |  |
| `8259AF88` | `D3DDevice_SetVertexDeclaration` | `(pDevice, pDecl)` | high | Stores at device+11408, sets dirty bits 0x480. |  |
| `8259B698` | `D3DDevice_SetFVF` | `(pDevice, FVF)` | medium | Stores the FVF at device+12528, builds a declaration (82597D60), then SetVertexDeclaration. |  |
| `8259FF48` | `D3DDevice_SetRenderTarget` | `(pDevice, RenderTargetIndex, pSurface)` | high | Render targets at device+12536+4*i, bind-count bookkeeping, updates surface/viewport state (8259FE70). | RT seam. |
| `825A02E0` | `D3DDevice_SetDepthStencilSurface` | `(pDevice, pSurface)` | high | Stores at device+12552. | RT seam. |
| `8259FAE8` | `D3DDevice_GetRenderTarget` | `(pDevice, Index)` | medium | Returns device+12536+4*i with AddRef. |  |
| `8259FB30` | `D3DDevice_GetDepthStencilSurface` | `(pDevice)` | medium | Returns device+12552 with AddRef. |  |
| `8259F7D8` | `D3DDevice_SetViewport` | `(pDevice, pViewport)` | high | Copies a 6-dword D3DVIEWPORT9 to device+12808; Present saves/restores the same block. |  |
| `8259F710` | `D3DDevice_SetScissorRect` | `(pDevice, pRect)` | medium | Writes device+12832..12844; called right after SetViewport in Present. |  |
| `8259FC90` | `D3DDevice_SetClipPlane` | `(pDevice, Index, pPlane)` | medium | Copies 4 floats to device+12848+16*i. |  |
| `8259FB70` | `D3DDevice_SetGammaRamp` | `(pDevice, Flags, pRamp)` | medium | Copies a 1536-byte ramp and compares it with device+14044. |  |
| `8259FE70` | `D3D_UpdateSurfaceState` | `(pDevice)` | low | Shared by SetRenderTarget / SetDepthStencilSurface. |  |
| `825976C8` | `D3DDevice_CreateTexture` | `(Width, Height, Depth, Levels, Usage, Format, Pool, ResourceType)` | medium | Allocates a 40-byte texture header, fills it via 825971C8, allocates physical memory for base and mips. Format 0x18280186 (A8R8G8B8) and type 3 at game sites. | Resource seam (only textures the game asks D3D to create; most game textures are loaded into game-allocated memory). |
| `825971C8` | `D3D_SetTextureHeader` | `(ResourceType, Width, Height, ...)` | medium | Fills the texture fetch constant and returns base/mip sizes (XGSetTextureHeader-style). |  |
| `825977E8` | `D3DDevice_CreateSurface` | `(Width, Height, Format, MultiSample, pParameters)` | medium | 64-byte surface; allocates EDRAM tiles (825A4DD0, 2048-tile limit). Called with A8R8G8B8 and D24S8. | Render-target / depth creation seam. |
| `825974B0` | `D3DTexture_GetSurfaceLevel` | `(pTexture, Level)` | medium | 64-byte surface object that AddRefs its parent. |  |
| `82597570` | `D3DCubeTexture_GetCubeMapSurface` | `(pCubeTexture, Face, Level)` | medium | Three-argument variant of the above. |  |
| `82597620` | `D3DVolumeTexture_GetVolumeLevel` | `(pVolumeTexture, Level)` | low | Uses the volume size helper 82596430. |  |
| `82597AF0` | `D3DBaseTexture_GetLevelDesc` | `(pTexture, Level, pDesc)` | low | Writes a desc with Type=1. |  |
| `82597B70` | `D3DSurface_GetDesc` | `(pSurface, pDesc)` | low | Writes a desc with Type=2. |  |
| `825A4500` | `D3DResource_AddRef` | `(pResource)` | high | Increments the reference count in the low 8 bits of Common (+0); recurses to the parent at +44. |  |
| `825A4A80` | `D3DResource_Release` | `(pResource)` | high | Decrements; on zero calls 825A4618 to destroy. 44 call sites. | Resource destroy seam. |
| `825A4618` | `D3D_DestroyResource` | `(pResource)` | medium | Waits on the resource fence, frees memory with D3D attributes. |  |
| `825A4B00` | `D3DResource_GetDevice` | `(pResource, ppDevice)` | medium | Returns the global device with AddRef. |  |
| `825A4DD0` | `D3D_AllocateEdram` | `(TileCount, pBase)` | low | EDRAM tile allocator used by CreateSurface. |  |
| `825A4D80` | `D3D_FreeEdram` | `(Base, TileCount)` | low |  |  |
| `8259AE90` | `D3DDevice_CreateVertexShader` | `(pFunction)` | medium | Allocates header + physical microcode (attrs 0xA5800000); the game calls it right after an ID3DXBuffer GetBufferPointer. The matching release frees +40 with 0xA1800000. | Shader seam: receives Xenos microcode. |
| `8259AD80` | `D3DDevice_CreatePixelShader` | `(pFunction)` | medium | Same pattern, physical attrs 0xB5800000; release frees +12 with 0xB1800000. | Shader seam: receives Xenos microcode. |
| `82599890` | `D3DVertexShader_Release` | `(pShader)` | medium | Refcount at +4; frees microcode at +40. |  |
| `825997D8` | `D3DPixelShader_Release` | `(pShader)` | medium | Refcount at +4; frees microcode at +12. |  |
| `82599D70` | `D3DDevice_CreateVertexDeclaration` | `(pVertexElements)` | medium | Counts 12-byte elements up to Stream 0xFF (D3DDECL_END). |  |
| `82599E30` | `D3DVertexDeclaration_Release` | `(pDecl)` | medium |  |  |
| `825AF298` | `D3DXAssembleShader` | `(pSrcData, SrcDataLen, pDefines, pInclude, Flags, ppShader, ppErrorMsgs)` | medium | 7-argument wrapper that builds a memory-source object and calls the assembler (825AF1D0). Shader-assembler strings live in .rdata. | Shader-source seam: sees the D3D9-style assembly text before it becomes microcode. |
| `825AF9A8` | `D3DX_ShaderBuildB` | `(...)` | low | Second D3DX shader entry with the same shape; 3 game sites. |  |
| `82593698` | `D3DDevice_CreateQuery` | `(pDevice, Type, ppQuery)` | medium | 148-byte object; switches on Type 8/9/10. |  |
| `82593778` | `D3DQuery_Release` | `(pQuery)` | medium |  |  |
| `825938C0` | `D3DQuery_Issue` | `(pQuery, IssueFlags)` | medium | EVENT_WRITE / EVENT_WRITE_ZPD. |  |
| `82593A48` | `D3DQuery_GetData` | `(pQuery, pData, Size, Flags)` | medium |  |  |
| `824A6FC0` | `Game_CreateRenderDevice` | `(...)` | high | Fills D3DPRESENT_PARAMETERS and calls Direct3D_CreateDevice; wraps the device in a 364-byte object (vtable 0x8207A5BC, device at +4). |  |
| `824A6D08` | `Game_RenderDevice_Swap` | `(this)` | high | Vtable slot +40: Swap(device, frontBuffer[+196]); then swaps +196/+200 (double-buffered front buffers). |  |

## Render-state table (`0x82850E40`)

Entries 0, 1 and 63–68 have placeholder getters or setters. Names are inferred
from the register field each setter writes, matched against the order and
defaults of the D3D9 enum. "Writes" uses PowerPC bit numbering (bit 0 = MSB).
`dev+N` is a device field outside the register-shadow blocks.

| # | Setter | Getter | Default | State (inferred `D3DRS_`) | Writes |
|---|---|---|---|---|---|
| 2 | `8259D8E8` | `8259D930` | `00000001` | ZENABLE | dev+11972 bits30-30 RB_DEPTHCONTROL |
| 3 | `8259D970` | `8259D9A8` | `00000003` | ZFUNC | bits25-27 RB_DEPTHCONTROL |
| 4 | `8259D938` | `8259D960` | `00000001` | ZWRITEENABLE | bits29-29 RB_DEPTHCONTROL |
| 5 | `8259D230` | `8259D258` | `00000000` | FILLMODE | bits21-28 PA_SU_SC_MODE_CNTL |
| 6 | `8259D1F8` | `8259D220` | `00000006` | CULLMODE | bits29-31 PA_SU_SC_MODE_CNTL |
| 7 | `8259D2B0` | `8259D340` | `00000000` | ALPHABLENDENABLE | bits0-0 dev+11452 RB_BLENDCONTROL0 RB_BLENDCONTROL1 RB_BLENDCONTROL2 RB_BLENDCONTROL3 |
| 8 | `8259D680` | `8259D710` | `00000000` | SEPARATEALPHABLENDENABLE | bits1-1 dev+11452 RB_BLENDCONTROL0 RB_BLENDCONTROL1 RB_BLENDCONTROL2 RB_BLENDCONTROL3 |
| 9 | `8259D7C8` | `8259D868` | `FFFFFFFF` | BLENDFACTOR | RB_BLEND_GREEN RB_BLEND_RED RB_BLEND_BLUE RB_BLEND_ALPHA |
| 10 | `8259D3E8` | `8259D470` | `00000001` | SRCBLEND | dev+11448 RB_BLENDCONTROL0 RB_BLENDCONTROL1 RB_BLENDCONTROL2 RB_BLENDCONTROL3 |
| 11 | `8259D480` | `8259D508` | `00000000` | DESTBLEND | bits19-23 dev+11448 RB_BLENDCONTROL0 RB_BLENDCONTROL1 RB_BLENDCONTROL2 RB_BLENDCONTROL3 |
| 12 | `8259D350` | `8259D3D8` | `00000000` | BLENDOP | bits24-26 dev+11448 RB_BLENDCONTROL0 RB_BLENDCONTROL1 RB_BLENDCONTROL2 RB_BLENDCONTROL3 |
| 13 | `8259D590` | `8259D5F8` | `00000001` | SRCBLENDALPHA | bits11-15 dev+11448 RB_BLENDCONTROL0 RB_BLENDCONTROL1 RB_BLENDCONTROL2 RB_BLENDCONTROL3 |
| 14 | `8259D608` | `8259D670` | `00000000` | DESTBLENDALPHA | bits3-7 dev+11448 RB_BLENDCONTROL0 RB_BLENDCONTROL1 RB_BLENDCONTROL2 RB_BLENDCONTROL3 |
| 15 | `8259D518` | `8259D580` | `00000000` | BLENDOPALPHA | bits8-10 dev+11448 RB_BLENDCONTROL0 RB_BLENDCONTROL1 RB_BLENDCONTROL2 RB_BLENDCONTROL3 |
| 16 | `8259D268` | `8259D2A0` | `00000000` | ALPHATESTENABLE | bits28-28 RB_COLORCONTROL |
| 17 | `8259D720` | `8259D760` | `00000000` | ALPHAREF | RB_ALPHA_REF |
| 18 | `8259D790` | `8259D7B8` | `00000007` | ALPHAFUNC | bits29-31 RB_COLORCONTROL |
| 19 | `8259D9B8` | `8259DA00` | `00000000` | STENCILENABLE | dev+11976 bits31-31 RB_DEPTHCONTROL |
| 20 | `8259DA08` | `8259DA40` | `00000000` | TWOSIDEDSTENCILMODE | bits24-24 RB_DEPTHCONTROL |
| 21 | `8259DA88` | `8259DAC0` | `00000000` | STENCILFAIL | bits18-20 RB_DEPTHCONTROL |
| 22 | `8259DAD0` | `8259DB08` | `00000000` | STENCILZFAIL | bits12-14 RB_DEPTHCONTROL |
| 23 | `8259DB18` | `8259DB40` | `00000000` | STENCILPASS | bits15-17 RB_DEPTHCONTROL |
| 24 | `8259DA50` | `8259DA78` | `00000007` | STENCILFUNC | bits21-23 RB_DEPTHCONTROL |
| 25 | `8259DC50` | `8259DC70` | `00000000` | STENCILREF | RB_STENCILREFMASK+3 |
| 26 | `8259DC78` | `8259DC98` | `FFFFFFFF` | STENCILMASK | RB_STENCILREFMASK+2 |
| 27 | `8259DCA0` | `8259DCC0` | `FFFFFFFF` | STENCILWRITEMASK | RB_STENCILREFMASK+1 |
| 28 | `8259DB88` | `8259DBC0` | `00000000` | CCW_STENCILFAIL | bits6-8 RB_DEPTHCONTROL |
| 29 | `8259DBD0` | `8259DC08` | `00000000` | CCW_STENCILZFAIL | bits0-2 RB_DEPTHCONTROL |
| 30 | `8259DC18` | `8259DC40` | `00000000` | CCW_STENCILPASS | bits3-5 RB_DEPTHCONTROL |
| 31 | `8259DB50` | `8259DB78` | `00000007` | CCW_STENCILFUNC | bits9-11 RB_DEPTHCONTROL |
| 32 | `8259DCC8` | `8259DCE8` | `00000000` | CCW_STENCILREF | RB_STENCILREFMASK_BF+3 |
| 33 | `8259DCF0` | `8259DD10` | `FFFFFFFF` | CCW_STENCILMASK | RB_STENCILREFMASK_BF+2 |
| 34 | `8259DD18` | `8259DD38` | `FFFFFFFF` | CCW_STENCILWRITEMASK | RB_STENCILREFMASK_BF+1 |
| 35 | `8259DD40` | `8259DD70` | `00000000` | CLIPPLANEENABLE | PA_CL_CLIP_CNTL |
| 36 | `8259E088` | `8259E0D0` | `3F800000` | POINTSIZE | dev+11988 PA_SU_POINT_SIZE+2 PA_SU_POINT_SIZE |
| 37 | `8259E0E0` | `8259E120` | `3F800000` | POINTSIZE_MIN | dev+11992 PA_SU_POINT_MINMAX+2 |
| 38 | `8259E078` | `8259E080` | `00000000` | unidentified | dev+11980 |
| 39 | `8259E130` | `8259E170` | `42800000` | POINTSIZE_MAX | dev+11996 PA_SU_POINT_MINMAX |
| 40 | `8259DEF0` | `8259DF18` | `00000001` | MULTISAMPLEANTIALIAS | bits16-16 PA_SU_SC_MODE_CNTL |
| 41 | `8259DF28` | `8259DF50` | `FFFFFFFF` | MULTISAMPLEMASK | PA_SC_AA_MASK |
| 42 | `8259FF38` | `8259DD80` | `00000000` | SCISSORTESTENABLE | dev+11952 |
| 43 | `8259DD88` | `8259DE28` | `00000000` | SLOPESCALEDEPTHBIAS | PA_SU_POLY_OFFSET_FRONT_SCALE PA_SU_POLY_OFFSET_BACK_SCALE PA_SU_SC_MODE_CNTL |
| 44 | `8259DE48` | `8259DEE0` | `00000000` | DEPTHBIAS | PA_SU_POLY_OFFSET_FRONT_OFFSET PA_SU_POLY_OFFSET_BACK_OFFSET PA_SU_SC_MODE_CNTL |
| 45 | `8259DF58` | `8259DF98` | `0000000F` | COLORWRITEENABLE | dev+11956 bits28-31 RB_COLOR_MASK |
| 46 | `8259DFA0` | `8259DFE0` | `0000000F` | COLORWRITEENABLE1 | dev+11960 bits24-27 RB_COLOR_MASK |
| 47 | `8259DFE8` | `8259E028` | `0000000F` | COLORWRITEENABLE2 | dev+11964 bits20-23 RB_COLOR_MASK |
| 48 | `8259E030` | `8259E070` | `0000000F` | COLORWRITEENABLE3 | dev+11968 bits16-19 RB_COLOR_MASK |
| 49 | `8259E510` | `8259E528` | `00000000` | unidentified | dev+12124 dev+12128 dev+12132 dev+12136 |
| 50 | `8259EB70` | `8259EB90` | `00000001` | TESSELLATIONMODE | bits30-31 VGT_HOS_CNTL |
| 51 | `8259EB10` | `8259EB30` | `3F800000` | MINTESSELLATIONLEVEL | VGT_HOS_MIN_TESS_LEVEL |
| 52 | `8259EB40` | `8259EB60` | `3F800000` | MAXTESSELLATIONLEVEL | VGT_HOS_MAX_TESS_LEVEL |
| 53 | `8259E180` | `8259E470` | `00000000` | WRAP0 | SQ_WRAPPING_0 |
| 54 | `8259E1B0` | `8259E480` | `00000000` | WRAP1 | SQ_WRAPPING_0 |
| 55 | `8259E1E0` | `8259E490` | `00000000` | WRAP2 | SQ_WRAPPING_0 |
| 56 | `8259E210` | `8259E4A0` | `00000000` | WRAP3 | SQ_WRAPPING_0 |
| 57 | `8259E240` | `8259E4B0` | `00000000` | WRAP4 | SQ_WRAPPING_0 |
| 58 | `8259E270` | `8259E4C0` | `00000000` | WRAP5 | SQ_WRAPPING_0 |
| 59 | `8259E2A0` | `8259E4D0` | `00000000` | WRAP6 | SQ_WRAPPING_0 |
| 60 | `8259E2D0` | `8259E4E0` | `00000000` | WRAP7 | bits0-3 SQ_WRAPPING_0 |
| 61 | `8259E2F8` | `8259E4F0` | `00000000` | WRAP8 | SQ_WRAPPING_1 |
| 62 | `8259E328` | `8259E500` | `00000000` | WRAP9 | SQ_WRAPPING_1 |
| 63 | `8259E358` | `82704DD0` | `00000000` | WRAP10 | SQ_WRAPPING_1 |
| 64 | `8259E388` | `82704DD0` | `00000000` | WRAP11 | SQ_WRAPPING_1 |
| 65 | `8259E3B8` | `82704DD0` | `00000000` | WRAP12 | SQ_WRAPPING_1 |
| 66 | `8259E3E8` | `82704DD0` | `00000000` | WRAP13 | SQ_WRAPPING_1 |
| 67 | `8259E418` | `82704DD0` | `00000000` | WRAP14 | SQ_WRAPPING_1 |
| 68 | `8259E448` | `82704DD0` | `00000000` | WRAP15 | bits0-3 SQ_WRAPPING_1 |
| 69 | `8259E530` | `8259E580` | `00000001` | VIEWPORTENABLE | dev+12140 PA_CL_VTE_CNTL PA_CL_CLIP_CNTL |
| 70 | `8259E590` | `8259E6E8` | `00000000` | unidentified | call 825A3BA0 |
| 71 | `8259E6F0` | `8259E848` | `00000000` | unidentified | call 825A3BA0 |
| 72 | `8259E850` | `8259E9A8` | `00000000` | unidentified | call 825A3BA0 |
| 73 | `8259E9B0` | `8259EB08` | `00000000` | unidentified | call 825A3BA0 |
| 74 | `8259EBA0` | `8259EBC8` | `00000000` | HALFPIXELOFFSET | bits31-31 PA_SU_VTX_CNTL |
| 75 | `8259EBD8` | `8259EC00` | `00000000` | PRIMITIVERESETENABLE | bits10-10 PA_SU_SC_MODE_CNTL |
| 76 | `8259EC10` | `8259EC30` | `0000FFFF` | PRIMITIVERESETINDEX | VGT_MULTI_PRIM_IB_RESET_INDX |
| 77 | `8259EC38` | `8259EC60` | `00000000` | ALPHATOMASKENABLE | bits27-27 RB_COLORCONTROL |
| 78 | `8259EC70` | `8259EC98` | `00000087` | ALPHATOMASKOFFSETS | bits0-7 RB_COLORCONTROL |
| 79 | `8259ECB0` | `8259ECD8` | `40000000` | GUARDBAND_X | PA_CL_GB_HORZ_CLIP_ADJ |
| 80 | `8259ECE0` | `8259ED08` | `40000000` | GUARDBAND_Y | PA_CL_GB_VERT_CLIP_ADJ |
| 81 | `8259ED10` | `8259ED38` | `3F800000` | DISCARDBAND_X | PA_CL_GB_HORZ_DISC_ADJ |
| 82 | `8259ED40` | `8259ED68` | `3F800000` | DISCARDBAND_Y | PA_CL_GB_VERT_DISC_ADJ |
| 83 | `8259ED70` | `8259ED98` | `00000000` | HIZENABLE | bits28-28 RB_HIZCONTROL |
| 84 | `8259EDA8` | `8259EDD0` | `00000000` | HIZWRITEENABLE | bits29-29 RB_HIZCONTROL |
| 85 | `8259EDE0` | `8259EE08` | `00000000` | unidentified | bits26-26 RB_HIZCONTROL |
| 86 | `8259EE18` | `8259EE38` | `00000000` | unidentified | RB_HIZCONTROL+2 |
| 87 | `8259EE40` | `8259EE48` | `00000000` | unidentified | dev+13756 |
| 88 | `8259EE50` | `8259EE60` | `00000000` | unidentified | bits2-8 dev+11452 |

The sampler-state setters (`0x8285126C`, 20 entries) take `(pDevice, Sampler,
Value)` and patch the sampler's fetch constant at device+1152+24·s or the
per-sampler bytes at +12046/+12072/+12098. They are listed in
`amped3_d3d_names.toml` by table index only; individual names still need doing.

## Hook plan for a native Vulkan renderer

A native renderer here means keeping the game's D3D calls but sending them to
our own Vulkan code, instead of letting D3D build Xenos command packets for the
emulated GPU. The recompiler already gives us the hooks we need: every guest
function is a weak symbol, so defining `D3DDevice_DrawVertices` with
`REX_HOOK_RAW` (rex/hook.h) replaces it, and `__imp__D3DDevice_DrawVertices`
still calls the original. The names in `amped3_d3d_names.toml` make those
symbols readable. Build with `setup.ps1 -D3DNames` to apply them.

Seams, in the order a native backend would need them:

1. **Device and presentation:** `Direct3D_CreateDevice`, `D3DDevice_Swap`,
   `D3DDevice_Present`. Create the Vulkan device and swapchain on creation and
   present on Swap. The game calls Swap directly through its wrapper
   (`824A6D08`), alternating between two front-buffer textures.
2. **Draws:** `D3DDevice_DrawVertices`, `DrawIndexedVertices`,
   `DrawVerticesUP`, `DrawIndexedVerticesUP`, `Clear`/`ClearF`. On each, read
   the device object (table above) and build a Vulkan pipeline and draw. This
   one seam covers every inlined and table-driven state change.
3. **Render targets:** `CreateSurface`, `SetRenderTarget`,
   `SetDepthStencilSurface`, `Resolve`, `BeginTiling`/`EndTiling`. With real
   render targets there is no 10 MB EDRAM limit, so tiling collapses into one
   pass and Resolve becomes an image copy into the destination texture.
4. **Shaders:** `CreateVertexShader` and `CreatePixelShader` receive Xenos
   microcode, which still needs translating (the SDK's SPIR-V translator can
   be reused offline). `D3DXAssembleShader` (`825AF298`) is earlier in the
   pipeline: it sees the D3D9-style assembly text the game assembles at run
   time. Translating that text instead of microcode is a possible shortcut,
   but the 360 dialect has extensions (`vfetch`, `xps`) that would need
   handling.
5. **Resources:** most textures, vertex buffers and index buffers are *not*
   created through D3D. The game allocates physical memory and fills the
   headers itself. Their contents have to be read from guest memory when
   first bound (texture fetch constant at texture+16, vertex fetch from the
   stream table) and tracked for changes, the same job Xenia's texture and
   shared-memory caches do today. `D3DResource_Release` (`825A4A80`) is the
   destroy hook.
6. **Synchronisation:** `D3D_BlockOnFence`, `D3DResource_BlockUntilNotBusy`,
   queries. Map D3D fences onto Vulkan timeline semaphores.

What does not go away: textures stay in Xenos tiled layouts and formats,
vertex data in Xenos formats, and shaders in Xenos microcode (or 360 assembly).
A native renderer avoids emulating the command processor and EDRAM, but it
still needs a texture untiler, format conversion and a shader translator. Most
of that can be borrowed from the SDK's existing Xenos code.

## Not mapped yet

- Sampler-state names (20 table entries).
- Render states 38, 49, 70–73, 85–88 (writes listed, meaning unclear).
- Volume-texture and desc helpers (`82597620`, `82597AF0`, `82597B70`), low confidence.
- D3DX internals beyond the shader-build entry points.
- `8259B600` (creates a 9696-byte object, possibly a state block) and
  `825A5F08` (swap path behind a critical section; no direct callers).
- `8259FF38` (render state 42, scissor test) is not registered with the
  recompiler. If a run ever stops with "unregistered function 0x8259FF38",
  `setup.ps1 -FixCrashes` adds it.

## Reproducing

`tools/d3d-map/` has the scripts used. They read a decoded image of
`default.xex` (`image.bin`) and an llvm-objdump disassembly (`text.asm`), which
are not included because they are built from the game. `gen_map.py` holds the
function list and writes both `amped3_d3d_names.toml` and the tables above.
