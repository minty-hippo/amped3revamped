# Generates docs tables + amped3_d3d_names.toml from one list, so they can't disagree.
from idx import *
H,M,L='high','medium','low'
F=[
# (addr, name, signature/args, confidence, evidence, hook note)
# --- Device lifecycle
(0x825A43D8,'Direct3D_CreateDevice','(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice)',H,'Allocates the 0x5080-byte device (128-byte aligned), stores BehaviorFlags at +0x5050, calls device init; returns E_OUTOFMEMORY 0x8007000E. Only caller is the game renderer init 824A6FC0.','Device creation seam: create the native device/swapchain here.'),
(0x825AAED8,'D3D_InitializeDevice','(pDevice, pPresentationParameters)',H,'Only caller of VdInitializeEDRAM / VdIsHSIOTrainingSucceeded; calls ring-buffer and engine init.',''),
(0x825AA940,'D3D_InitializeEngines','(pDevice)',H,'Calls VdInitializeEngines and VdSetGraphicsInterruptCallback.',''),
(0x825A2980,'D3D_GraphicsInterruptCallback','(source, pDevice)',H,'Address the runtime log reports for SetInterruptCallback(825A2980, ...).',''),
(0x825A3EC0,'D3D_InitializeRingBuffer','(pDevice)',H,'Only caller of VdInitializeRingBuffer / VdEnableRingBufferRPtrWriteBack; emits ME_INIT.',''),
(0x825AB0F0,'D3D_ShutdownDevice','(pDevice)',H,'Calls VdShutdownEngines and clears the interrupt callback.',''),
(0x825A4330,'D3D_DestroyDevice','(pDevice)',M,'Shutdown + free of the device block; called on CreateDevice failure and by game teardown.',''),
(0x825AAA00,'D3D_SetDefaultStates','(pDevice)',M,'Walks the render-state table at 0x82850DE0 and calls every setter with its default.',''),
(0x825931C0,'Direct3D_GetDeviceCaps','(Adapter, DeviceType, pCaps)',M,'Copies a fixed 1104-byte block (D3DCAPS9 size) from 0x820007A0.',''),
# --- Command buffer / sync
(0x825A3BA0,'D3D_MakeSpace','(pDevice)',H,'Called (52 sites) whenever device+0 (write pointer) reaches device+8 (limit); kicks the segment and waits on fences.','Not needed by a native renderer; command buffer is bypassed.'),
(0x825A3818,'D3D_ReservePredicated','(pTilingBuffer)',M,'Reserve in the secondary buffer at device+13520 used while predicated tiling is active.',''),
(0x825A3550,'D3D_BlockOnFence','(pDevice, Fence, ...)',H,'Spins until *(device+10384) (GPU-written fence) passes Fence.','Native fences replace this.'),
(0x825A3DF0,'D3DDevice_BlockUntilIdle','(pDevice)',M,'Inserts a fence (type-0 write to reg 1480) and blocks on it.',''),
(0x825A37B8,'D3DResource_BlockUntilNotBusy','(pResource)',M,'Blocks on the resource fence at +4 using the global device.',''),
(0x825AB500,'D3D_WaitForPresentInterval','(pDevice)',L,'WAIT_REG_MEM on a vblank counter; called from Present and two game sites.',''),
(0x825A9268,'D3D_FlushDirtyState','(pDevice)',H,'Called at the start of every draw. Walks the 64-bit dirty masks at device+16..+48 and writes each dirty register block from its device shadow.','Key seam: at this point the device shadows hold the complete pipeline state for the draw.'),
(0x825A8C98,'D3D_CommitShaders','(pDevice)',H,'Reads current VS (device+12948), PS (+12944) and vertex declaration (+11408); emits IM_LOAD / INVALIDATE_STATE.',''),
(0x825A8130,'D3D_CommitVertexFetch','(pDevice)',M,'Builds vertex fetch constants from streams (+12556) and declaration (+11408).',''),
(0x825A7E58,'D3D_WriteRegisterBlock','(pDevice, Mask, FirstRegister, pShadow)',M,'Type-0 packet writer used by the flush for each register block.',''),
(0x825A8008,'D3D_WriteAluConstants','(pDevice, Mask, FirstRegister, pShadow)',M,'Writes float constants 0x4000/0x4400 from device+1920 / +6016.',''),
(0x825A7F18,'D3D_WriteFetchConstants','(pDevice, Mask, FirstRegister, pShadow)',M,'Writes fetch constants 0x4800 from device+1152.',''),
# --- Present
(0x825ABC50,'D3DDevice_Present','(pDevice)',H,'Saves viewport/scissor, sets back buffer (device+13948), resolves it into the front buffer (device+13944), restores state, calls Swap.','Present seam #1.'),
(0x825AB670,'D3DDevice_Swap','(pDevice, pFrontBuffer, pSwapParameters)',H,'Only caller of VdSwap; stores the new front buffer at device+13960.','Present seam #2. The game calls this directly (via 824A6D08), not Present.'),
# --- Draws / clears
(0x825A1278,'D3DDevice_DrawVertices','(pDevice, PrimitiveType, StartVertex, VertexCount)',H,'StartVertex goes to VGT_INDX_OFFSET (device+11540); emits DRAW_INDX header 0xC0012201 (auto-index).','Draw seam.'),
(0x825A14B8,'D3DDevice_DrawIndexedVertices','(pDevice, PrimitiveType, BaseVertexIndex, StartIndex, IndexCount)',H,'Like DrawVertices but reads the index buffer (device+12532); emits 0xC003xxxx DRAW_INDX with index base/size.','Draw seam.'),
(0x825A0E00,'D3DDevice_DrawVerticesUP','(pDevice, PrimitiveType, VertexCount, pVertexData, Stride)',M,'Copies user vertices into the command buffer, then calls 825A0B60.','Draw seam (user pointer).'),
(0x825A0B60,'D3D_DrawVerticesUP_Internal','(pDevice, PrimitiveType, VertexCount, Stride)',M,'Shared tail of the UP draws.',''),
(0x825A1198,'D3DDevice_DrawIndexedVerticesUP','(pDevice, PrimitiveType, MinIndex, NumVertices, IndexCount, pIndexData, IndexFormat, pVertexData, Stride)',M,'UP indexed variant; calls 825A0E88.','Draw seam (user pointer).'),
(0x825A0E88,'D3D_DrawIndexedVerticesUP_Internal','',M,'Shared tail of the indexed UP draw.',''),
(0x825A25F0,'D3DDevice_Clear','(pDevice, Count, pRects, Flags, Color, Z, Stencil)',H,'Converts the ARGB colour to a float4 and calls ClearF once per rect.','Clear seam.'),
(0x825A1F30,'D3DDevice_ClearF','(pDevice, Flags, pRect, pColor, Z, Stencil)',H,'Draws a clear quad with immediately-loaded shaders (IM_LOAD_IMMEDIATE).','Clear seam.'),
# --- Resolve / tiling
(0x825A6CA8,'D3DDevice_Resolve','(pDevice, Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel, DestSliceOrFace, pClearColor, ClearZ, ClearStencil, pParameters)',H,'Largest D3D function; programs RB_COPY_* and draws the copy; called by Present, EndTiling and 4 game sites.','Resolve seam: becomes a native RT-to-texture copy.'),
(0x825A0528,'D3DDevice_BeginTiling','(pDevice, Flags, Count, pTileRects, pClearColor, ClearZ, ClearStencil)',M,'Stores tile count/rects at device+12972.., starts predicated tiling (EVENT_WRITE), clears.','Predicated tiling only exists because of 10 MB EDRAM; a native renderer can turn these into a single pass.'),
(0x825A0850,'D3DDevice_EndTiling','(pDevice, Flags, pResolveRects, pDestTexture, pClearColor, ClearZ, ClearStencil, pParameters)',M,'Ends tiling and resolves each tile (calls Resolve).',''),
(0x825A0458,'D3D_SetTilePredication','(pDevice, ...)',L,'SET_BIN_MASK packet; used by EndTiling and two game sites.',''),
# --- State setters that are real functions
(0x82597BD8,'D3DDevice_SetTexture','(pDevice, Sampler, pTexture)',H,'Stores pTexture at device+12704+4*s, copies the texture fetch constant (texture+16, 24 bytes) into device+1152+24*s merged with sampler state, sets the fetch dirty bit.','State seam (or read device+12704 at draw time).'),
(0x8259F9C8,'D3DDevice_SetStreamSource','(pDevice, StreamNumber, pVertexBuffer, OffsetInBytes, Stride)',H,'Stream table at device+12556+8*n {Offset, pVB}; Stride/4 as a byte at device+12688+n; sets dirty bit 0x400 at +48.',''),
(0x8259FA68,'D3DDevice_SetIndices','(pDevice, pIndexBuffer)',H,'Stores at device+12532 with bind-count/fence bookkeeping.',''),
(0x82599D08,'D3DDevice_SetVertexShader','(pDevice, pVertexShader)',M,'Stores at device+20440, then 82599BB0 binds the declaration-patched copy at device+12948.',''),
(0x82599BB0,'D3D_BindVertexShaderForDeclaration','(pDevice, pVertexShader)',M,'Writes device+12948 (the shader actually loaded by CommitShaders).',''),
(0x82599998,'D3DDevice_SetPixelShader','(pDevice, pPixelShader)',H,'Stores at device+12944, AddRef/Release, sets dirty bits 0x120 at +48.',''),
(0x8259AF88,'D3DDevice_SetVertexDeclaration','(pDevice, pDecl)',H,'Stores at device+11408, sets dirty bits 0x480.',''),
(0x8259B698,'D3DDevice_SetFVF','(pDevice, FVF)',M,'Stores the FVF at device+12528, builds a declaration (82597D60), then SetVertexDeclaration.',''),
(0x8259FF48,'D3DDevice_SetRenderTarget','(pDevice, RenderTargetIndex, pSurface)',H,'Render targets at device+12536+4*i, bind-count bookkeeping, updates surface/viewport state (8259FE70).','RT seam.'),
(0x825A02E0,'D3DDevice_SetDepthStencilSurface','(pDevice, pSurface)',H,'Stores at device+12552.','RT seam.'),
(0x8259FAE8,'D3DDevice_GetRenderTarget','(pDevice, Index)',M,'Returns device+12536+4*i with AddRef.',''),
(0x8259FB30,'D3DDevice_GetDepthStencilSurface','(pDevice)',M,'Returns device+12552 with AddRef.',''),
(0x8259F7D8,'D3DDevice_SetViewport','(pDevice, pViewport)',H,'Copies a 6-dword D3DVIEWPORT9 to device+12808; Present saves/restores the same block.',''),
(0x8259F710,'D3DDevice_SetScissorRect','(pDevice, pRect)',M,'Writes device+12832..12844; called right after SetViewport in Present.',''),
(0x8259FC90,'D3DDevice_SetClipPlane','(pDevice, Index, pPlane)',M,'Copies 4 floats to device+12848+16*i.',''),
(0x8259FB70,'D3DDevice_SetGammaRamp','(pDevice, Flags, pRamp)',M,'Copies a 1536-byte ramp and compares it with device+14044.',''),
(0x8259FE70,'D3D_UpdateSurfaceState','(pDevice)',L,'Shared by SetRenderTarget / SetDepthStencilSurface.',''),
# --- Resources
(0x825976C8,'D3DDevice_CreateTexture','(Width, Height, Depth, Levels, Usage, Format, Pool, ResourceType)',M,'Allocates a 40-byte texture header, fills it via 825971C8, allocates physical memory for base and mips. Format 0x18280186 (A8R8G8B8) and type 3 at game sites.','Resource seam (only textures the game asks D3D to create; most game textures are loaded into game-allocated memory).'),
(0x825971C8,'D3D_SetTextureHeader','(ResourceType, Width, Height, ...)',M,'Fills the texture fetch constant and returns base/mip sizes (XGSetTextureHeader-style).',''),
(0x825977E8,'D3DDevice_CreateSurface','(Width, Height, Format, MultiSample, pParameters)',M,'64-byte surface; allocates EDRAM tiles (825A4DD0, 2048-tile limit). Called with A8R8G8B8 and D24S8.','Render-target / depth creation seam.'),
(0x825974B0,'D3DTexture_GetSurfaceLevel','(pTexture, Level)',M,'64-byte surface object that AddRefs its parent.',''),
(0x82597570,'D3DCubeTexture_GetCubeMapSurface','(pCubeTexture, Face, Level)',M,'Three-argument variant of the above.',''),
(0x82597620,'D3DVolumeTexture_GetVolumeLevel','(pVolumeTexture, Level)',L,'Uses the volume size helper 82596430.',''),
(0x82597AF0,'D3DBaseTexture_GetLevelDesc','(pTexture, Level, pDesc)',L,'Writes a desc with Type=1.',''),
(0x82597B70,'D3DSurface_GetDesc','(pSurface, pDesc)',L,'Writes a desc with Type=2.',''),
(0x825A4500,'D3DResource_AddRef','(pResource)',H,'Increments the reference count in the low 8 bits of Common (+0); recurses to the parent at +44.',''),
(0x825A4A80,'D3DResource_Release','(pResource)',H,'Decrements; on zero calls 825A4618 to destroy. 44 call sites.','Resource destroy seam.'),
(0x825A4618,'D3D_DestroyResource','(pResource)',M,'Waits on the resource fence, frees memory with D3D attributes.',''),
(0x825A4B00,'D3DResource_GetDevice','(pResource, ppDevice)',M,'Returns the global device with AddRef.',''),
(0x825A4DD0,'D3D_AllocateEdram','(TileCount, pBase)',L,'EDRAM tile allocator used by CreateSurface.',''),
(0x825A4D80,'D3D_FreeEdram','(Base, TileCount)',L,'',''),
# --- Shaders
(0x8259AE90,'D3DDevice_CreateVertexShader','(pFunction)',M,'Allocates header + physical microcode (attrs 0xA5800000); the game calls it right after an ID3DXBuffer GetBufferPointer. The matching release frees +40 with 0xA1800000.','Shader seam: receives Xenos microcode.'),
(0x8259AD80,'D3DDevice_CreatePixelShader','(pFunction)',M,'Same pattern, physical attrs 0xB5800000; release frees +12 with 0xB1800000.','Shader seam: receives Xenos microcode.'),
(0x82599890,'D3DVertexShader_Release','(pShader)',M,'Refcount at +4; frees microcode at +40.',''),
(0x825997D8,'D3DPixelShader_Release','(pShader)',M,'Refcount at +4; frees microcode at +12.',''),
(0x82599D70,'D3DDevice_CreateVertexDeclaration','(pVertexElements)',M,'Counts 12-byte elements up to Stream 0xFF (D3DDECL_END).',''),
(0x82599E30,'D3DVertexDeclaration_Release','(pDecl)',M,'',''),
(0x825AF298,'D3DXAssembleShader','(pSrcData, SrcDataLen, pDefines, pInclude, Flags, ppShader, ppErrorMsgs)',M,'7-argument wrapper that builds a memory-source object and calls the assembler (825AF1D0). Shader-assembler strings live in .rdata.','Shader-source seam: sees the D3D9-style assembly text before it becomes microcode.'),
(0x825AF9A8,'D3DX_ShaderBuildB','(...)',L,'Second D3DX shader entry with the same shape; 3 game sites.',''),
# --- Queries
(0x82593698,'D3DDevice_CreateQuery','(pDevice, Type, ppQuery)',M,'148-byte object; switches on Type 8/9/10.',''),
(0x82593778,'D3DQuery_Release','(pQuery)',M,'',''),
(0x825938C0,'D3DQuery_Issue','(pQuery, IssueFlags)',M,'EVENT_WRITE / EVENT_WRITE_ZPD.',''),
(0x82593A48,'D3DQuery_GetData','(pQuery, pData, Size, Flags)',M,'',''),
# --- Game side
(0x824A6FC0,'Game_CreateRenderDevice','(...)',H,'Fills D3DPRESENT_PARAMETERS and calls Direct3D_CreateDevice; wraps the device in a 364-byte object (vtable 0x8207A5BC, device at +4).',''),
(0x824A6D08,'Game_RenderDevice_Swap','(this)',H,'Vtable slot +40: Swap(device, frontBuffer[+196]); then swaps +196/+200 (double-buffered front buffers).',''),
]
RS_NAMES={2:'ZENABLE',3:'ZFUNC',4:'ZWRITEENABLE',5:'FILLMODE',6:'CULLMODE',7:'ALPHABLENDENABLE',8:'SEPARATEALPHABLENDENABLE',9:'BLENDFACTOR',10:'SRCBLEND',11:'DESTBLEND',12:'BLENDOP',13:'SRCBLENDALPHA',14:'DESTBLENDALPHA',15:'BLENDOPALPHA',16:'ALPHATESTENABLE',17:'ALPHAREF',18:'ALPHAFUNC',19:'STENCILENABLE',20:'TWOSIDEDSTENCILMODE',21:'STENCILFAIL',22:'STENCILZFAIL',23:'STENCILPASS',24:'STENCILFUNC',25:'STENCILREF',26:'STENCILMASK',27:'STENCILWRITEMASK',28:'CCW_STENCILFAIL',29:'CCW_STENCILZFAIL',30:'CCW_STENCILPASS',31:'CCW_STENCILFUNC',32:'CCW_STENCILREF',33:'CCW_STENCILMASK',34:'CCW_STENCILWRITEMASK',35:'CLIPPLANEENABLE',36:'POINTSIZE',37:'POINTSIZE_MIN',39:'POINTSIZE_MAX',42:'SCISSORTESTENABLE',40:'MULTISAMPLEANTIALIAS',41:'MULTISAMPLEMASK',43:'SLOPESCALEDEPTHBIAS',44:'DEPTHBIAS',45:'COLORWRITEENABLE',46:'COLORWRITEENABLE1',47:'COLORWRITEENABLE2',48:'COLORWRITEENABLE3',50:'TESSELLATIONMODE',51:'MINTESSELLATIONLEVEL',52:'MAXTESSELLATIONLEVEL',69:'VIEWPORTENABLE',74:'HALFPIXELOFFSET',75:'PRIMITIVERESETENABLE',76:'PRIMITIVERESETINDEX',77:'ALPHATOMASKENABLE',78:'ALPHATOMASKOFFSETS',79:'GUARDBAND_X',80:'GUARDBAND_Y',81:'DISCARDBAND_X',82:'DISCARDBAND_Y',83:'HIZENABLE',84:'HIZWRITEENABLE'}
for i in range(16): RS_NAMES[53+i]=f'WRAP{i}'
import rs
def rs_rows():
    rows=[]; T=0x82850E40
    for i in range(89):
        g,s,d=[u32(T+12*i+4*k) for k in range(3)]
        if s==0x82260980: continue
        rows.append((i,s,g,d,RS_NAMES.get(i,'?'),rs.describe(s)))
    return rows
if __name__=='__main__':
    import sys
    # TOML
    out=['# Names for the Direct3D functions identified in default.xex (see docs/d3d-map.md).',
         '# Opt-in: add "amped3_d3d_names.toml" to [entrypoint] includes in the manifest to',
         '# give these functions readable names in generated code, crash reports and hooks.',
         '# Naming only; it does not change behaviour. Needs a codegen re-run (full rebuild).','',
         '[functions]']
    for a,n,sig,c,ev,hk in sorted(F):
        out.append(f'"0x{a:08X}" = {{ name = "{n}" }}  # {c}')
    for i,s,g,d,nm,desc in rs_rows():
        tag=f'D3DRS_{nm}' if nm!='?' else f'RS{i}'
        out.append(f'"0x{s:08X}" = {{ name = "D3DDevice_SetRenderState_{tag}" }}  # table #{i}')
    open(sys.argv[1],'w').write('\n'.join(out)+'\n')
    # markdown fragments
    cats=[]
    md=[]
    for a,n,sig,c,ev,hk in F:
        md.append(f'| `{a:08X}` | `{n}` | `{sig}` | {c} | {ev} | {hk} |')
    open(sys.argv[2],'w').write('\n'.join(md)+'\n')
    rsmd=[]
    for i,s,g,d,nm,desc in rs_rows():
        rsmd.append(f'| {i} | `{s:08X}` | `{g:08X}` | `{d:08X}` | {nm if nm!="?" else "unidentified"} | {desc} |')
    open(sys.argv[3],'w').write('\n'.join(rsmd)+'\n')
    print(len(F),'functions', len(rs_rows()),'render states')
