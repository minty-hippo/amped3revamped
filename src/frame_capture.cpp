// amped3 - frame capture for the native-renderer work. See frame_capture.h
// and docs\frame-capture.md.
//
// Every hook below replaces a recompiled Direct3D function (the generated
// code defines each guest function as a weak alias of __imp__<name>, so a
// strong definition here takes over) and then calls the original. Device
// offsets come from docs\d3d-map.md.

#include "frame_capture.h"
#include "frame_limit.h"
#include "perf_trace.h"
#include "native/xenos_shader.h"
#include "live_renderer.h"
#include "perf_overlay.h"
#include "texture_ranges.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(log_frame_rate, true, "Amped 3/Debug",
                    "Every 10 seconds, write the game's frame rate and slowest frame to the log.");

REXCVAR_DEFINE_BOOL(capture_resources, true, "Amped 3/Debug",
                    "Frame capture (F10) also saves the textures, vertex buffers, index "
                    "buffers and shader constants each draw reads, to logs\\frame_<time>\\.");

REXCVAR_DEFINE_BOOL(capture_shaders, true, "Amped 3/Debug",
                    "Save every shader the game creates to logs\\shaders (for the native "
                    "renderer work), and the microcode of each shader as drawn. Each shader is "
                    "written once.");

// Originals of the hooked functions (named by amped3_d3d_names.toml).
REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_EXTERN(__imp__D3DDevice_DrawVerticesUP);
REX_EXTERN(__imp__D3DDevice_DrawIndexedVerticesUP);
REX_EXTERN(__imp__D3D_PatchVertexShaderFetches);
REX_EXTERN(__imp__D3D_DrawVerticesUP_Internal);
REX_EXTERN(__imp__D3D_CommitShaders);
REX_EXTERN(__imp__D3DDevice_ClearF);
REX_EXTERN(__imp__D3DDevice_Resolve);
REX_EXTERN(__imp__D3DDevice_BeginTiling);
REX_EXTERN(__imp__D3DDevice_EndTiling);
REX_EXTERN(__imp__D3DDevice_SetRenderTarget);
REX_EXTERN(__imp__D3DDevice_SetDepthStencilSurface);
REX_EXTERN(__imp__D3DDevice_Swap);
REX_EXTERN(__imp__D3DDevice_CreateVertexShader);
REX_EXTERN(__imp__D3DDevice_CreatePixelShader);
REX_EXTERN(__imp__D3DXAssembleShader);
REX_EXTERN(__imp__D3D_FlushDirtyState);
REX_EXTERN(D3D_CommitVertexFetch);
REX_EXTERN(D3D_CommitShaders);
REX_EXTERN(__imp__D3D_BlockOnFence);
REX_EXTERN(__imp__D3DDevice_BlockUntilIdle);
REX_EXTERN(__imp__D3DResource_BlockUntilNotBusy);
REX_EXTERN(__imp__D3D_MakeSpace);

namespace amped3::capture {
namespace {

// --- Device layout (docs\d3d-map.md) -------------------------------------
constexpr uint32_t kDirtyMasks = 16;     // 5 x u64
constexpr uint32_t kFetchConsts = 1152;  // 32 x 24 bytes
constexpr uint32_t kFlags = 10432;       // 2 bytes
constexpr uint32_t kRegShadow = 11456;   // register shadow blocks 0x2000..0x23FF
constexpr uint32_t kRegShadowEnd = 12000;
constexpr uint32_t kVertexDecl = 11408;
constexpr uint32_t kIndexBuffer = 12532;
constexpr uint32_t kRenderTargets = 12536;  // 4
constexpr uint32_t kDepthStencil = 12552;
constexpr uint32_t kStreams = 12556;        // 16 x {offset, vb}
constexpr uint32_t kStreamStrides = 12688;  // 16 bytes, stride / 4
constexpr uint32_t kTextures = 12704;       // 26 slots
constexpr uint32_t kTextureSlots = 26;
constexpr uint32_t kViewport = 12808;
constexpr uint32_t kScissor = 12832;
constexpr uint32_t kPixelShader = 12944;
constexpr uint32_t kVertexShaderLoaded = 12948;
constexpr uint32_t kVertexShaderSet = 20440;

struct ShadowBlock {
  uint32_t offset;
  uint32_t first_register;
};
constexpr ShadowBlock kBlocks[] = {{11456, 0x2000}, {11532, 0x2100}, {11616, 0x2180},
                                   {11636, 0x2200}, {11684, 0x2280}, {11768, 0x2300},
                                   {11920, 0x2380}};

std::string RegisterName(uint32_t offset) {
  const ShadowBlock* block = nullptr;
  for (const auto& b : kBlocks) {
    if (offset >= b.offset) block = &b;
  }
  if (!block) return fmt::format("d+{}", offset);
  return fmt::format("r{:04X}", block->first_register + (offset - block->offset) / 4);
}

// --- State --------------------------------------------------------------------
std::filesystem::path g_dir;
enum State : int { kIdle = 0, kArmed = 1, kRecording = 2 };
std::atomic<int> g_state{kIdle};
std::recursive_mutex g_mutex;  // capture buffer; recursive: EndTiling calls Resolve
std::string g_out;
uint32_t g_events = 0;
uint32_t g_draws = 0;
std::vector<uint32_t> g_prev_regs;
std::vector<uint32_t> g_prev_consts;  // float + bool/loop constants, previous draw

// Resource data saved with a capture (capture_resources).
std::string g_stamp;
std::filesystem::path g_res_dir;
std::string g_res_index;
std::unordered_set<uint64_t> g_res_done;
uint64_t g_res_bytes = 0;
bool g_res_full_logged = false;
constexpr uint64_t kMaxResourceBytes = 768ull << 20;
std::unordered_set<uint32_t> g_objects_dumped;
constexpr uint32_t kMaxEvents = 20000;

std::mutex g_shader_mutex;
std::unordered_map<uint32_t, std::string> g_shader_names;  // guest object -> file stem
std::atomic<uint32_t> g_frame_counter{0};

// --- Frame rate (measured at Swap) ---------------------------------------------
using Clock = std::chrono::steady_clock;
std::mutex g_rate_mutex;
FrameRate g_rate;
Clock::time_point g_window_start{}, g_last_swap{}, g_log_start{};
uint32_t g_window_frames = 0;
uint32_t g_log_frames = 0;
double g_log_worst_ms = 0;

void OnSwapTiming() {
  const auto now = Clock::now();
  std::lock_guard lock(g_rate_mutex);
  ++g_rate.frame_count;
  if (g_last_swap != Clock::time_point{}) {
    const double ms = std::chrono::duration<double, std::milli>(now - g_last_swap).count();
    g_log_worst_ms = std::max(g_log_worst_ms, ms);
  }
  g_last_swap = now;
  if (g_window_start == Clock::time_point{}) {
    g_window_start = g_log_start = now;
    return;
  }
  ++g_window_frames;
  ++g_log_frames;
  const double window = std::chrono::duration<double>(now - g_window_start).count();
  if (window >= 0.5) {
    g_rate.fps = g_window_frames / window;
    g_rate.frame_time_ms = 1000.0 * window / g_window_frames;
    g_window_frames = 0;
    g_window_start = now;
  }
  const double log_window = std::chrono::duration<double>(now - g_log_start).count();
  if (log_window >= 10.0) {
    if (REXCVAR_GET(log_frame_rate)) {
      REXLOG_INFO("Frame rate: {:.1f} fps average over {:.0f} s, slowest frame {:.1f} ms",
                  g_log_frames / log_window, log_window, g_log_worst_ms);
    }
    g_log_frames = 0;
    g_log_worst_ms = 0;
    g_log_start = now;
  }
}

inline uint32_t U32(uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}
inline uint8_t U8(uint8_t* base, uint32_t address) { return base[address]; }
inline uint64_t U64(uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint64_t>(base + address);
}
inline float F32(uint8_t* base, uint32_t address) {
  uint32_t bits = U32(base, address);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

bool Recording() { return g_state.load(std::memory_order_relaxed) == kRecording; }

// The microcode the GPU runs is followed for shader dumps and for the native
// renderer preview.
bool TrackShaders() { return REXCVAR_GET(capture_shaders) || amped3::live::Enabled(); }

std::string Hex(uint8_t* base, uint32_t address, uint32_t dwords) {
  std::string s;
  for (uint32_t i = 0; i < dwords; ++i) {
    if (i) s += ' ';
    s += fmt::format("{:08X}", U32(base, address + 4 * i));
  }
  return s;
}

uint64_t Fnv1a(const uint8_t* data, size_t size) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    h ^= data[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

std::string ShaderName(uint32_t object) {
  if (!object) return "none";
  std::lock_guard lock(g_shader_mutex);
  auto it = g_shader_names.find(object);
  return it != g_shader_names.end() ? fmt::format("{:08X}({})", object, it->second)
                                    : fmt::format("{:08X}", object);
}

// Raw words of a guest object, once per frame (surfaces, buffers, decls).
void DumpObjectOnce(uint8_t* base, const char* what, uint32_t object, uint32_t dwords) {
  if (!object || !g_objects_dumped.insert(object).second) return;
  g_out += fmt::format("  object {} {:08X}: {}\n", what, object, Hex(base, object, dwords));
}

// Xenos texture fetch constant (6 dwords): the fields the native renderer
// needs first. Raw words are logged too, so nothing depends on this decode.
std::string DescribeTextureFetch(uint8_t* base, uint32_t address) {
  uint32_t d[6];
  for (int i = 0; i < 6; ++i) d[i] = U32(base, address + 4 * i);
  const uint32_t type = d[0] & 3;
  const uint32_t pitch = ((d[0] >> 22) & 0x1FF) * 32;
  const bool tiled = (d[0] >> 31) & 1;
  const uint32_t format = d[1] & 0x3F;
  const uint32_t endian = (d[1] >> 6) & 3;
  const uint32_t base_address = d[1] & 0xFFFFF000;
  const uint32_t width = (d[2] & 0x1FFF) + 1;
  const uint32_t height = ((d[2] >> 13) & 0x1FFF) + 1;
  const uint32_t dimension = (d[5] >> 9) & 3;
  const uint32_t mip_address = d[5] & 0xFFFFF000;
  return fmt::format(
      "type={} dim={} fmt={} endian={} {}x{} pitch={} tiled={} base={:08X} mips={:08X} "
      "raw=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
      type, dimension, format, endian, width, height, pitch, tiled ? 1 : 0, base_address,
      mip_address, d[0], d[1], d[2], d[3], d[4], d[5]);
}

// --- Resource data (capture_resources) ---------------------------------------
rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

bool SaveResource(const std::string& name, const uint8_t* data, size_t size) {
  if (!data || !size) return false;
  if (g_res_bytes + size > kMaxResourceBytes) {
    if (!g_res_full_logged) {
      g_res_full_logged = true;
      g_res_index += "# size limit reached; later resources were not saved\n";
    }
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(g_res_dir, ec);
  std::ofstream f(g_res_dir / name, std::ios::binary);
  f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  g_res_bytes += size;
  return true;
}

// Saves the texture's base level and mip chain exactly as they are in guest
// memory (tiled, big-endian), keyed by the fetch constant. Returns the name.
std::string SaveTexture(uint8_t* base, uint32_t fetch_address) {
  uint32_t d[6];
  for (int i = 0; i < 6; ++i) d[i] = U32(base, fetch_address + 4 * i);
  const uint64_t key = Fnv1a(reinterpret_cast<const uint8_t*>(d), sizeof(d));
  const std::string name = fmt::format("tex_{:016X}", key);
  if (!REXCVAR_GET(capture_resources) || !g_res_done.insert(key).second) return name;
  auto* memory = GuestMemory();
  if (!memory) return name;
  uint32_t base_page = 0, base_size = 0, mip_page = 0, mip_size = 0;
  uint32_t width = 0, height = 0, depth = 0, mip_min = 0, mip_max = 0;
  {
    const amped3::TextureRanges ranges = amped3::GetTextureRanges(d);
    base_page = ranges.base_address >> 12;
    base_size = ranges.base_size;
    mip_page = ranges.mip_address >> 12;
    mip_size = ranges.mip_size;
    width = ranges.width;
    height = ranges.height;
    depth = ranges.depth;
    mip_min = ranges.mip_min;
    mip_max = ranges.mip_max;
  }
  bool base_saved = false, mips_saved = false;
  if (base_page && base_size) {
    base_saved = SaveResource(name + ".base.bin",
                              memory->TranslatePhysical<const uint8_t*>(base_page << 12), base_size);
  }
  if (mip_page && mip_size) {
    mips_saved = SaveResource(name + ".mips.bin",
                              memory->TranslatePhysical<const uint8_t*>(mip_page << 12), mip_size);
  }
  g_res_index += fmt::format(
      "{} fetch=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}] size={}x{}x{} base={:08X}:{}{} "
      "mips={:08X}:{}{} levels={}-{} (sizes are upper bounds)\n",
      name, d[0], d[1], d[2], d[3], d[4], d[5], width, height, depth, base_page << 12, base_size, base_saved ? "" : "(not saved)",
      mip_page << 12, mip_size, mips_saved ? "" : "(not saved)", mip_min, mip_max);
  return name;
}

// Vertex buffer: +12/+16 hold the Xenos vertex fetch constant (address | type,
// size in words << 2 | endian). Index buffer: +12 address, +16 size in bytes.
std::string SaveBuffer(uint8_t* base, const char* kind, uint32_t object) {
  const uint32_t w0 = U32(base, object + 12);
  const uint32_t w1 = U32(base, object + 16);
  uint32_t address, size;
  if (kind[0] == 'v') {
    address = w0 & ~3u;
    size = (w1 >> 2) * 4;
  } else {
    address = w0;
    size = w1;
  }
  const std::string name = fmt::format("{}_{:08X}_{}", kind, address, size);
  const uint64_t key = (uint64_t(address) << 32) | size;
  if (!REXCVAR_GET(capture_resources) || !address || !size || size > (64u << 20) ||
      !g_res_done.insert(key ^ (kind[0] == 'v' ? 0x5600000000000000ull : 0x4900000000000000ull))
           .second) {
    return name;
  }
  auto* memory = GuestMemory();
  if (!memory) return name;
  const bool saved =
      SaveResource(name + ".bin", memory->TranslateVirtual<const uint8_t*>(address), size);
  g_res_index += fmt::format("{} object={:08X} address={:08X} size={} words=[{:08X} {:08X}]{}\n",
                             name, object, address, size, w0, w1, saved ? "" : " (not saved)");
  return name;
}

// User-pointer draw data (DrawVerticesUP and friends), saved per draw.
std::string SaveUserData(const char* kind, uint32_t address, uint32_t size) {
  const std::string name = fmt::format("{}_{:08X}_{}_{}", kind, address, size, g_events);
  if (!REXCVAR_GET(capture_resources) || !address || !size || size > (16u << 20)) return name;
  auto* memory = GuestMemory();
  if (!memory) return name;
  SaveResource(name + ".bin", memory->TranslateVirtual<const uint8_t*>(address), size);
  return name;
}

// Float constants (VS then PS, 256 x float4 each) and bool/loop constants:
// the ones that changed since the previous draw.
void DumpConstants(uint8_t* base, uint32_t dev) {
  constexpr uint32_t kFloatStart = 1920, kFloatCount = 512;    // float4 registers
  constexpr uint32_t kBoolLoopStart = 10112, kBoolLoopDwords = 40;
  std::vector<uint32_t> now(kFloatCount * 4 + kBoolLoopDwords);
  for (uint32_t i = 0; i < kFloatCount * 4; ++i) now[i] = U32(base, dev + kFloatStart + 4 * i);
  for (uint32_t i = 0; i < kBoolLoopDwords; ++i) {
    now[kFloatCount * 4 + i] = U32(base, dev + kBoolLoopStart + 4 * i);
  }
  std::string vs, ps, bl;
  for (uint32_t r = 0; r < kFloatCount; ++r) {
    bool changed = g_prev_consts.empty();
    for (uint32_t c = 0; c < 4 && !changed; ++c) changed = g_prev_consts[r * 4 + c] != now[r * 4 + c];
    if (!changed) continue;
    std::string& out = r < 256 ? vs : ps;
    out += fmt::format(" c{}=[{:08X} {:08X} {:08X} {:08X}]", r % 256, now[r * 4], now[r * 4 + 1],
                       now[r * 4 + 2], now[r * 4 + 3]);
  }
  for (uint32_t i = 0; i < kBoolLoopDwords; ++i) {
    const uint32_t v = now[kFloatCount * 4 + i];
    if (g_prev_consts.empty() || g_prev_consts[kFloatCount * 4 + i] != v) {
      bl += fmt::format(" {}{}={:08X}", i < 8 ? "b" : "l", i < 8 ? i : i - 8, v);
    }
  }
  if (!vs.empty()) g_out += "  vs_consts" + vs + "\n";
  if (!ps.empty()) g_out += "  ps_consts" + ps + "\n";
  if (!bl.empty()) g_out += "  bool_loop" + bl + "\n";
  g_prev_consts = std::move(now);
}

// Everything a draw depends on, read from the device before D3D flushes it.
void DumpDrawState(uint8_t* base, uint32_t dev) {
  std::string& o = g_out;
  o += fmt::format("  dirty={:016X} {:016X} {:016X} {:016X} {:016X} flags={:02X}{:02X}\n",
                   U64(base, dev + kDirtyMasks), U64(base, dev + kDirtyMasks + 8),
                   U64(base, dev + kDirtyMasks + 16), U64(base, dev + kDirtyMasks + 24),
                   U64(base, dev + kDirtyMasks + 32), U8(base, dev + kFlags),
                   U8(base, dev + kFlags + 1));
  o += fmt::format("  vs={} vs_set={} ps={} decl={:08X}\n",
                   ShaderName(U32(base, dev + kVertexShaderLoaded)),
                   ShaderName(U32(base, dev + kVertexShaderSet)),
                   ShaderName(U32(base, dev + kPixelShader)), U32(base, dev + kVertexDecl));
  DumpObjectOnce(base, "decl", U32(base, dev + kVertexDecl), 32);

  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t rt = U32(base, dev + kRenderTargets + 4 * i);
    if (rt) {
      o += fmt::format("  rt{}={:08X}\n", i, rt);
      DumpObjectOnce(base, "surface", rt, 16);
    }
  }
  const uint32_t ds = U32(base, dev + kDepthStencil);
  if (ds) {
    o += fmt::format("  ds={:08X}\n", ds);
    DumpObjectOnce(base, "surface", ds, 16);
  }

  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t vb = U32(base, dev + kStreams + 8 * i + 4);
    if (!vb) continue;
    o += fmt::format("  stream{}: vb={:08X} offset={} stride={}\n", i, vb,
                     U32(base, dev + kStreams + 8 * i), U8(base, dev + kStreamStrides + i) * 4);
    DumpObjectOnce(base, "vb", vb, 8);
    if (REXCVAR_GET(capture_resources)) o += "    data=" + SaveBuffer(base, "vb", vb) + "\n";
  }
  const uint32_t ib = U32(base, dev + kIndexBuffer);
  if (ib) {
    o += fmt::format("  ib={:08X}\n", ib);
    DumpObjectOnce(base, "ib", ib, 8);
    if (REXCVAR_GET(capture_resources)) o += "    data=" + SaveBuffer(base, "ib", ib) + "\n";
  }

  for (uint32_t s = 0; s < kTextureSlots; ++s) {
    const uint32_t tex = U32(base, dev + kTextures + 4 * s);
    // No texture object, but a texture the game wrote straight into the fetch
    // constant (the GPU reads it either way): recorded as tex<n>=00000000.
    const uint32_t w0 = U32(base, dev + kFetchConsts + 24 * s);
    const uint32_t w1 = U32(base, dev + kFetchConsts + 24 * s + 4);
    const bool raw = (w0 & 3) == 2 && (w1 >> 12) != 0;
    if (!tex && !raw) continue;
    o += fmt::format("  tex{}={:08X} {}\n", s, tex,
                     DescribeTextureFetch(base, dev + kFetchConsts + 24 * s));
    if (REXCVAR_GET(capture_resources)) {
      o += "    data=" + SaveTexture(base, dev + kFetchConsts + 24 * s) + "\n";
    }
  }

  o += fmt::format("  viewport={} {} {} {} {:.3f} {:.3f} scissor={} {} {} {}\n",
                   U32(base, dev + kViewport), U32(base, dev + kViewport + 4),
                   U32(base, dev + kViewport + 8), U32(base, dev + kViewport + 12),
                   F32(base, dev + kViewport + 16), F32(base, dev + kViewport + 20),
                   U32(base, dev + kScissor), U32(base, dev + kScissor + 4),
                   U32(base, dev + kScissor + 8), U32(base, dev + kScissor + 12));

  // Register shadows: everything on the first draw, then only what changed.
  const uint32_t count = (kRegShadowEnd - kRegShadow) / 4;
  std::vector<uint32_t> regs(count);
  for (uint32_t i = 0; i < count; ++i) regs[i] = U32(base, dev + kRegShadow + 4 * i);
  std::string changed;
  for (uint32_t i = 0; i < count; ++i) {
    if (g_prev_regs.empty() || g_prev_regs[i] != regs[i]) {
      changed += fmt::format(" {}={:08X}", RegisterName(kRegShadow + 4 * i), regs[i]);
    }
  }
  if (!changed.empty()) o += "  regs" + changed + "\n";
  g_prev_regs = std::move(regs);
  DumpConstants(base, dev);
}

// Render target registers at a clear or resolve: RB_SURFACE_INFO,
// RB_COLOR_INFO, RB_DEPTH_INFO, RB_COLOR1..3_INFO.
std::string TargetsLine(uint8_t* base, uint32_t dev) {
  std::string s = "  targets";
  for (uint32_t i = 0; i < 6; ++i) {
    s += fmt::format(" r{:04X}={:08X}", 0x2000 + i, U32(base, dev + kRegShadow + 4 * i));
  }
  return s + "\n";
}

// BeginVertices draws (D3D_DrawVerticesUP_Internal called by the game): the
// game writes the vertices into the command buffer after the call returns, so
// they are saved when the next event is recorded.
struct PendingVertices {
  uint32_t address = 0, size = 0;
  std::string name;
};
PendingVertices g_pending_vertices;

void FlushPendingVertices() {
  PendingVertices p = std::move(g_pending_vertices);
  g_pending_vertices = {};
  if (p.name.empty() || !REXCVAR_GET(capture_resources)) return;
  auto* memory = GuestMemory();
  if (!memory) return;
  SaveResource(p.name + ".bin", memory->TranslateVirtual<const uint8_t*>(p.address), p.size);
}

void Event(const std::string& line) {
  FlushPendingVertices();
  g_out += fmt::format("[{}] {}\n", g_events++, line);
}

bool Room() { return g_events < kMaxEvents; }

std::string Stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

void WriteFrame() {
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  const auto path = g_dir / fmt::format("frame_{}.txt", g_stamp);
  std::ofstream f(path, std::ios::binary);
  f << g_out;
  if (!g_res_index.empty()) {
    std::filesystem::create_directories(g_res_dir, ec);
    std::ofstream index(g_res_dir / "resources.txt", std::ios::binary);
    index << "# Resource data for frame_" << g_stamp << ".txt. Raw guest memory: textures are\n"
          << "# tiled and big-endian as the GPU reads them; buffers are as the game wrote them.\n"
          << g_res_index;
    REXLOG_INFO("Frame capture: {:.1f} MB of resource data written to {}",
                g_res_bytes / 1048576.0, g_res_dir.string());
  }
  REXLOG_INFO("Frame capture: {} draws, {} events written to {}", g_draws, g_events,
              path.string());
}

// --- Shader dumps --------------------------------------------------------------
void SaveFile(const std::filesystem::path& path, const void* data, size_t size) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) return;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::binary);
  f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void AppendIndex(const std::string& line) {
  std::error_code ec;
  std::filesystem::create_directories(g_dir / "shaders", ec);
  std::ofstream f(g_dir / "shaders" / "index.txt", std::ios::app);
  f << line << "\n";
}

// D3DDevice_CreateVertexShader / CreatePixelShader take the compiled shader
// blob: +4 = offset of the microcode, +8 = microcode size in bytes.
std::string DumpShaderBlob(uint8_t* base, const char* kind, uint32_t blob) {
  if (!blob) return {};
  const uint32_t header = U32(base, blob + 4);
  const uint32_t ucode = U32(base, blob + 8);
  const uint32_t total = header + ucode;
  if (header > 0x10000 || ucode > 0x100000 || total == 0) {
    REXLOG_WARN("capture_shaders: implausible {} blob at {:08X} (header {}, ucode {})", kind, blob,
                header, ucode);
    return {};
  }
  const uint64_t hash = Fnv1a(base + blob, total);
  const std::string stem = fmt::format("{}_{:016X}", kind, hash);
  SaveFile(g_dir / "shaders" / (stem + ".bin"), base + blob, total);
  return stem;
}

// Microcode as the GPU runs it.
// - Vertex shaders: the blobs from CreateVertexShader are templates whose
//   vertex fetches D3D_PatchVertexShaderFetches (825A8250) fills in for the
//   current declaration. D3D_CommitShaders either patches the shader's own
//   microcode (+40) or, while the GPU may still be reading that, copies it into
//   the command buffer and patches the copy (825A8830, IM_LOAD_IMMEDIATE). The
//   copy is the only place the patched code exists, so it is saved from the
//   patch function hook below.
// - Pixel shaders are loaded unchanged from +12 (size +60), saved at draw time.
constexpr uint32_t kVertexShaderUcode = 40, kVertexShaderUcodeSize = 600;
constexpr uint32_t kPixelShaderUcode = 12, kPixelShaderUcodeSize = 60;
std::unordered_map<uint64_t, std::string> g_bound_seen;  // object x checksum -> stem
uint32_t g_last_ps = 0;

// Saves the microcode (once) and returns its file stem, ucode_<kind>_<hash>.
std::string SaveDrawnMicrocode(uint8_t* base, const char* kind, uint32_t object, uint32_t address,
                               uint32_t bytes, uint32_t decl) {
  if (!object || !address || !bytes || bytes > 0x10000 || (bytes & 3)) return {};
  // Cheap checksum first; the full hash only for new microcode.
  uint64_t sum = 0;
  for (uint32_t i = 0; i < bytes; i += 4) {
    sum = sum * 31 + U32(base, address + i);
  }
  const uint64_t key = sum ^ (uint64_t(object) << 32) ^ bytes ^ (kind[0] == 'v' ? 1ull << 63 : 0);
  {
    std::lock_guard lock(g_shader_mutex);
    auto it = g_bound_seen.find(key);
    if (it != g_bound_seen.end()) return it->second;
  }
  std::vector<uint32_t> ucode(bytes / 4);
  for (uint32_t i = 0; i < bytes / 4; ++i) ucode[i] = U32(base, address + i * 4);
  const uint64_t hash = amped3::native::HashMicrocode(ucode.data(), ucode.size());
  const std::string stem = fmt::format("ucode_{}_{:016X}", kind, hash);
  std::string from;
  {
    std::lock_guard lock(g_shader_mutex);
    g_bound_seen[key] = stem;
    auto it = g_shader_names.find(object);
    if (it != g_shader_names.end()) from = it->second;
  }
  amped3::live::NoteMicrocode(stem, base + address, bytes);
  if (REXCVAR_GET(capture_shaders)) {
    SaveFile(g_dir / "shaders" / (stem + ".bin"), base + address, bytes);
    AppendIndex(fmt::format("{} object={:08X} decl={:08X} blob={}", stem, object, decl,
                            from.empty() ? "?" : from));
  }
  return stem;
}

// The microcode the GPU currently has loaded (D3D_CommitShaders tracking).
std::string g_cur_vs_ucode, g_cur_ps_ucode;
bool g_patched_in_commit = false;
uint32_t g_commit_vs = 0, g_commit_decl = 0, g_commit_ps = 0;

void NoteDrawnPixelShader(uint8_t* base, uint32_t dev) {
  if (!TrackShaders() || !dev) return;
  const uint32_t ps = U32(base, dev + kPixelShader);
  if (!ps || ps == g_last_ps) return;
  g_last_ps = ps;
  g_cur_ps_ucode = SaveDrawnMicrocode(base, "ps", ps, U32(base, ps + kPixelShaderUcode),
                                      U32(base, ps + kPixelShaderUcodeSize),
                                      U32(base, dev + kVertexDecl));
}

// Vertex fetch constants each vertex shader reads, from its translation.
std::unordered_map<std::string, std::vector<uint32_t>> g_vs_fetch_slots;

const std::vector<uint32_t>& VertexFetchSlots(const std::string& stem) {
  static const std::vector<uint32_t> kNone;
  if (stem.empty()) return kNone;
  auto it = g_vs_fetch_slots.find(stem);
  if (it != g_vs_fetch_slots.end()) return it->second;
  std::vector<uint32_t> slots;
  std::ifstream f(g_dir / "shaders" / (stem + ".bin"), std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<uint32_t> ucode(bytes.size() / 4);
  for (size_t i = 0; i < ucode.size(); ++i) {
    ucode[i] = (uint32_t(bytes[i * 4]) << 24) | (uint32_t(bytes[i * 4 + 1]) << 16) |
               (uint32_t(bytes[i * 4 + 2]) << 8) | bytes[i * 4 + 3];
  }
  if (!ucode.empty()) {
    auto r = amped3::native::TranslateShader(amped3::native::ShaderStage::kVertex, ucode.data(),
                                             ucode.size());
    if (r.ok) {
      for (uint32_t i = 0; i < 96; ++i) {
        if (r.info.vertex_fetch_constants[i >> 5] & (1u << (i & 31))) slots.push_back(i);
      }
    }
  }
  return g_vs_fetch_slots.emplace(stem, std::move(slots)).first->second;
}

// What the GPU sees for a draw, read after Direct3D has flushed its state:
// the loaded microcode, the fetch constants (D3D fills the vertex ones from
// the streams during the draw) and the vertex memory they point at
// (physical addresses).
std::vector<uint32_t> g_prev_fetch;
void DumpGpuView(uint8_t* base, uint32_t dev) {
  std::string& o = g_out;
  o += fmt::format("  ucode vs={} ps={}\n", g_cur_vs_ucode.empty() ? "?" : g_cur_vs_ucode,
                   g_cur_ps_ucode.empty() ? "?" : g_cur_ps_ucode);
  constexpr uint32_t kFetchDwords = 192;
  std::vector<uint32_t> fetch(kFetchDwords);
  for (uint32_t i = 0; i < kFetchDwords; ++i) fetch[i] = U32(base, dev + kFetchConsts + 4 * i);
  std::string changed;
  for (uint32_t i = 0; i < kFetchDwords; ++i) {
    if (g_prev_fetch.empty() || g_prev_fetch[i] != fetch[i]) {
      changed += fmt::format(" f{}={:08X}", i, fetch[i]);
    }
  }
  if (!changed.empty()) o += "  fetch" + changed + "\n";
  g_prev_fetch = fetch;
  if (!REXCVAR_GET(capture_resources)) return;
  auto* memory = GuestMemory();
  if (!memory) return;
  std::string vmem;
  for (uint32_t vf : VertexFetchSlots(g_cur_vs_ucode)) {
    const uint32_t w0 = fetch[vf * 2], w1 = fetch[vf * 2 + 1];
    if ((w0 & 3) != 3) continue;  // not a vertex fetch constant
    const uint32_t address = (w0 & ~3u) & 0x1FFFFFFF;
    const uint32_t size = ((w1 >> 2) & 0xFFFFFF) * 4;
    const std::string name = fmt::format("mem_{:08X}_{}", address, size);
    vmem += fmt::format(" vf{}={}", vf, name);
    const uint64_t key = ((uint64_t(address) << 32) | size) ^ 0x4D00000000000000ull;
    if (!address || !size || size > (64u << 20) || !g_res_done.insert(key).second) continue;
    SaveResource(name + ".bin", memory->TranslatePhysical<const uint8_t*>(address), size);
  }
  if (!vmem.empty()) o += "  vmem" + vmem + "\n";
}

}  // namespace

void Install(const std::filesystem::path& log_dir) { g_dir = log_dir; }

FrameRate GetFrameRate() {
  std::lock_guard lock(g_rate_mutex);
  return g_rate;
}

void RequestFrame() {
  int expected = kIdle;
  if (g_state.compare_exchange_strong(expected, kArmed)) {
    REXLOG_INFO("Frame capture: armed, recording the next frame");
  }
  amped3::live::RequestDump();
}

}  // namespace amped3::capture

// =============================================================================
// Hooks
// =============================================================================
using namespace amped3::capture;
namespace cap = amped3::capture;

// The event line is written before the draw; the state after it, once
// Direct3D has flushed (vertex fetch constants, index offset, shaders).
#define CAPTURE_DRAW(label_expr)                       \
  bool captured_draw = false;                          \
  if (cap::Recording()) {                              \
    std::lock_guard lock(cap::g_mutex);                \
    if (cap::Room()) {                                 \
      ++cap::g_draws;                                  \
      cap::Event(label_expr);                          \
      captured_draw = true;                            \
    }                                                  \
  }

#define CAPTURE_DRAW_STATE(dev)                        \
  cap::NoteDrawnPixelShader(base, dev);                \
  if (captured_draw && cap::Recording()) {             \
    std::lock_guard lock(cap::g_mutex);                \
    cap::DumpDrawState(base, dev);                     \
    cap::DumpGpuView(base, dev);                       \
  }

// The native renderer preview records the same draw (live_renderer.cpp).
#define LIVE_DRAW(dev, call)                                                    \
  if (amped3::live::Recording()) {                                              \
    amped3::trace::Scope live_scope(amped3::trace::Zone::kRecord);              \
    amped3::live::OnDraw(base, dev, call, cap::g_cur_vs_ucode, cap::g_cur_ps_ucode); \
  }

namespace {
amped3::live::DrawCall LiveCall(amped3::live::DrawCall::Kind kind, uint32_t prim, uint32_t start,
                                uint32_t count, int32_t base_vertex = 0) {
  amped3::live::DrawCall c;
  c.kind = kind;
  c.prim = prim;
  c.start = start;
  c.count = count;
  c.base_vertex = base_vertex;
  return c;
}
using LiveKind = amped3::live::DrawCall::Kind;
}  // namespace

extern "C" REX_FUNC(D3DDevice_DrawVertices) {
  CAPTURE_DRAW(fmt::format("DrawVertices prim={} start={} count={}", ctx.r4.u32, ctx.r5.u32,
                           ctx.r6.u32));
  const uint32_t dev = ctx.r3.u32;
  const auto live_call = LiveCall(LiveKind::kVertices, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  {
    amped3::trace::Scope d3d_scope(amped3::trace::Zone::kD3D);
    __imp__D3DDevice_DrawVertices(ctx, base);
  }
  CAPTURE_DRAW_STATE(dev);
  LIVE_DRAW(dev, live_call);
}

extern "C" REX_FUNC(D3DDevice_DrawIndexedVertices) {
  CAPTURE_DRAW(fmt::format("DrawIndexedVertices prim={} base_vertex={} start_index={} count={}",
                           ctx.r4.u32, ctx.r5.s32, ctx.r6.u32, ctx.r7.u32));
  const uint32_t dev = ctx.r3.u32;
  const auto live_call =
      LiveCall(LiveKind::kIndexed, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, ctx.r5.s32);
  {
    amped3::trace::Scope d3d_scope(amped3::trace::Zone::kD3D);
    __imp__D3DDevice_DrawIndexedVertices(ctx, base);
  }
  CAPTURE_DRAW_STATE(dev);
  LIVE_DRAW(dev, live_call);
}

// D3DDevice_DrawVerticesUP goes through D3D_DrawVerticesUP_Internal too; only
// direct calls from the game (BeginVertices style) are recorded there.
thread_local bool t_in_draw_up = false;

// D3D_DrawVerticesUP_Internal(pDevice, PrimitiveType, VertexCount, Stride):
// flushes state, reserves command buffer space and returns where the caller
// writes the vertices (D3DDevice_BeginVertices).
extern "C" REX_FUNC(D3D_DrawVerticesUP_Internal) {
  if (t_in_draw_up) {
    __imp__D3D_DrawVerticesUP_Internal(ctx, base);
    return;
  }
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  CAPTURE_DRAW(fmt::format("BeginVertices prim={} count={} stride={}", prim, count, stride));
  {
    amped3::trace::Scope d3d_scope(amped3::trace::Zone::kD3D);
    __imp__D3D_DrawVerticesUP_Internal(ctx, base);
  }
  const uint32_t data = ctx.r3.u32;
  if (captured_draw && cap::Recording()) {
    std::lock_guard lock(cap::g_mutex);
    const std::string name = fmt::format("upv_{:08X}_{}_{}", data, count * stride, cap::g_events);
    cap::g_out += "  up_vertices=" + name + "\n";
    cap::g_pending_vertices = {data, count * stride, name};
  }
  CAPTURE_DRAW_STATE(dev);
  auto live_call = LiveCall(LiveKind::kVerticesUP, prim, 0, count);
  live_call.up_vertices = data;
  live_call.up_vertices_size = count * stride;
  live_call.vertices_follow = true;
  LIVE_DRAW(dev, live_call);
}

extern "C" REX_FUNC(D3DDevice_DrawVerticesUP) {
  struct Guard {
    Guard() { t_in_draw_up = true; }
    ~Guard() { t_in_draw_up = false; }
  };
  CAPTURE_DRAW(fmt::format("DrawVerticesUP prim={} count={} data={:08X} stride={}", ctx.r4.u32,
                           ctx.r5.u32, ctx.r6.u32, ctx.r7.u32));
  if (cap::Recording() && REXCVAR_GET(capture_resources)) {
    std::lock_guard lock(cap::g_mutex);
    cap::g_out += "  up_vertices=" +
                  cap::SaveUserData("upv", ctx.r6.u32, ctx.r5.u32 * ctx.r7.u32) + "\n";
  }
  const uint32_t dev = ctx.r3.u32;
  auto live_call = LiveCall(LiveKind::kVerticesUP, ctx.r4.u32, 0, ctx.r5.u32);
  live_call.up_vertices = ctx.r6.u32;
  live_call.up_vertices_size = ctx.r5.u32 * ctx.r7.u32;
  {
    Guard guard;
    amped3::trace::Scope d3d_scope(amped3::trace::Zone::kD3D);
    __imp__D3DDevice_DrawVerticesUP(ctx, base);
  }
  CAPTURE_DRAW_STATE(dev);
  LIVE_DRAW(dev, live_call);
}

extern "C" REX_FUNC(D3DDevice_DrawIndexedVerticesUP) {
  // 9th argument (vertex stride) is on the stack; logged raw.
  CAPTURE_DRAW(fmt::format(
      "DrawIndexedVerticesUP prim={} min_index={} num_vertices={} index_count={} "
      "indices={:08X} index_format={} vertices={:08X} stack9={:08X}",
      ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
      cap::U32(base, ctx.r1.u32 + 84)));
  if (cap::Recording() && REXCVAR_GET(capture_resources)) {
    std::lock_guard lock(cap::g_mutex);
    const uint32_t index_bytes = ctx.r9.u32 == 0 ? 2 : 4;  // 0 seen with 16-bit data
    const uint32_t stride = cap::U32(base, ctx.r1.u32 + 84);
    cap::g_out += "  up_indices=" +
                  cap::SaveUserData("upi", ctx.r8.u32, ctx.r7.u32 * index_bytes) + "\n";
    if (stride && stride <= 256) {
      cap::g_out += "  up_vertices=" +
                    cap::SaveUserData("upv", ctx.r10.u32, (ctx.r5.u32 + ctx.r6.u32) * stride) +
                    "\n";
    }
  }
  const uint32_t dev = ctx.r3.u32;
  auto live_call = LiveCall(LiveKind::kIndexedUP, ctx.r4.u32, 0, ctx.r7.u32);
  {
    const uint32_t index_bytes = ctx.r9.u32 == 0 ? 2 : 4;
    const uint32_t stride = cap::U32(base, ctx.r1.u32 + 84);
    live_call.up_indices = ctx.r8.u32;
    live_call.up_indices_size = ctx.r7.u32 * index_bytes;
    live_call.up_index_32bit = index_bytes == 4;
    if (stride && stride <= 256) {
      live_call.up_vertices = ctx.r10.u32;
      live_call.up_vertices_size = (ctx.r5.u32 + ctx.r6.u32) * stride;
    }
  }
  {
    amped3::trace::Scope d3d_scope(amped3::trace::Zone::kD3D);
    __imp__D3DDevice_DrawIndexedVerticesUP(ctx, base);
  }
  CAPTURE_DRAW_STATE(dev);
  LIVE_DRAW(dev, live_call);
}

extern "C" REX_FUNC(D3DDevice_ClearF) {
  if (cap::Recording()) {
    std::lock_guard lock(cap::g_mutex);
    if (cap::Room()) {
      std::string color = "none";
      if (ctx.r6.u32) {
        color = fmt::format("{:.3f} {:.3f} {:.3f} {:.3f}", cap::F32(base, ctx.r6.u32),
                            cap::F32(base, ctx.r6.u32 + 4), cap::F32(base, ctx.r6.u32 + 8),
                            cap::F32(base, ctx.r6.u32 + 12));
      }
      std::string rect = ctx.r5.u32 ? cap::Hex(base, ctx.r5.u32, 4) : "full";
      cap::Event(fmt::format("ClearF flags={:08X} rect={} color={} z={:.4f} stencil={}",
                             ctx.r4.u32, rect, color, ctx.f1.f64, ctx.r8.u32));
      cap::g_out += cap::TargetsLine(base, ctx.r3.u32);
    }
  }
  {
    amped3::trace::Scope rec_scope(amped3::trace::Zone::kRecord);
    amped3::live::OnClear(base, ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, ctx.f1.f64, ctx.r8.u32);
  }
  {
    amped3::trace::Scope d3d_scope(amped3::trace::Zone::kD3D);
    __imp__D3DDevice_ClearF(ctx, base);
  }
}

extern "C" REX_FUNC(D3DDevice_Resolve) {
  if (cap::Recording()) {
    std::lock_guard lock(cap::g_mutex);
    if (cap::Room()) {
      std::string src = ctx.r5.u32 ? cap::Hex(base, ctx.r5.u32, 4) : "full";
      cap::Event(fmt::format(
          "Resolve flags={:08X} src_rect={} dest_tex={:08X} dest_point={:08X} level={} "
          "slice={} clear_color={:08X} z={:.4f}",
          ctx.r4.u32, src, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
          ctx.f1.f64));
      cap::DumpObjectOnce(base, "texture", ctx.r6.u32, 10);
      if (ctx.r6.u32) {
        cap::g_out += "  dest " + cap::DescribeTextureFetch(base, ctx.r6.u32 + 16) + "\n";
      }
      cap::g_out += cap::TargetsLine(base, ctx.r3.u32);
      if (ctx.r7.u32) {
        cap::g_out += fmt::format("  dest_point={} {}\n", cap::U32(base, ctx.r7.u32),
                                  cap::U32(base, ctx.r7.u32 + 4));
      }
      if (ctx.r10.u32) {
        cap::g_out += fmt::format("  clear_rgba={:.3f} {:.3f} {:.3f} {:.3f}\n",
                                  cap::F32(base, ctx.r10.u32), cap::F32(base, ctx.r10.u32 + 4),
                                  cap::F32(base, ctx.r10.u32 + 8), cap::F32(base, ctx.r10.u32 + 12));
      }
    }
  }
  {
    amped3::trace::Scope rec_scope(amped3::trace::Zone::kRecord);
    amped3::live::OnResolve(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                            ctx.r10.u32, ctx.f1.f64);
  }
  {
    amped3::trace::Scope d3d_scope(amped3::trace::Zone::kD3D);
    __imp__D3DDevice_Resolve(ctx, base);
  }
}

extern "C" REX_FUNC(D3DDevice_BeginTiling) {
  if (cap::Recording()) {
    std::lock_guard lock(cap::g_mutex);
    if (cap::Room()) {
      std::string rects;
      for (uint32_t i = 0; i < ctx.r5.u32 && i < 8 && ctx.r6.u32; ++i) {
        rects += " [" + cap::Hex(base, ctx.r6.u32 + 16 * i, 4) + "]";
      }
      cap::Event(fmt::format("BeginTiling flags={:08X} tiles={}{} clear_color={:08X} z={:.4f}",
                             ctx.r4.u32, ctx.r5.u32, rects, ctx.r7.u32, ctx.f1.f64));
    }
  }
  {
    amped3::trace::Scope rec_scope(amped3::trace::Zone::kRecord);
    amped3::live::OnBeginTiling(base, ctx.r5.u32, ctx.r6.u32);
  }
  {
    amped3::trace::Scope d3d_scope(amped3::trace::Zone::kD3D);
    __imp__D3DDevice_BeginTiling(ctx, base);
  }
}

extern "C" REX_FUNC(D3DDevice_EndTiling) {
  if (cap::Recording()) {
    std::lock_guard lock(cap::g_mutex);
    if (cap::Room()) {
      cap::Event(fmt::format("EndTiling flags={:08X} resolve_rects={:08X} dest_tex={:08X}",
                             ctx.r4.u32, ctx.r5.u32, ctx.r6.u32));
    }
  }
  {
    amped3::trace::Scope rec_scope(amped3::trace::Zone::kRecord);
    amped3::live::OnEndTiling();
  }
  {
    amped3::trace::Scope d3d_scope(amped3::trace::Zone::kD3D);
    __imp__D3DDevice_EndTiling(ctx, base);
  }
}

extern "C" REX_FUNC(D3DDevice_SetRenderTarget) {
  if (cap::Recording()) {
    std::lock_guard lock(cap::g_mutex);
    if (cap::Room()) {
      cap::Event(fmt::format("SetRenderTarget index={} surface={:08X}", ctx.r4.u32, ctx.r5.u32));
      cap::DumpObjectOnce(base, "surface", ctx.r5.u32, 16);
    }
  }
  __imp__D3DDevice_SetRenderTarget(ctx, base);
}

extern "C" REX_FUNC(D3DDevice_SetDepthStencilSurface) {
  if (cap::Recording()) {
    std::lock_guard lock(cap::g_mutex);
    if (cap::Room()) {
      cap::Event(fmt::format("SetDepthStencilSurface surface={:08X}", ctx.r4.u32));
      cap::DumpObjectOnce(base, "surface", ctx.r4.u32, 16);
    }
  }
  __imp__D3DDevice_SetDepthStencilSurface(ctx, base);
}

extern "C" REX_FUNC(D3DDevice_Swap) {
  amped3::trace::OnSwap();        // F11 performance trace: closes the frame's row
  amped3::perf::OnSwapBegin();    // F3 overlay: frame and game-thread CPU time
  {
    amped3::trace::Scope limit_scope(amped3::trace::Zone::kLimiter);
    amped3::frame_limit::OnSwap();  // frame rate cap (frame_limit.cpp)
  }
  amped3::perf::OnSwapEnd();
  const uint32_t frame = cap::g_frame_counter.fetch_add(1);
  cap::OnSwapTiming();
  {
    amped3::trace::Scope rec_scope(amped3::trace::Zone::kRecord);
    amped3::live::OnSwap(base, ctx.r4.u32);
  }
  const int state = cap::g_state.load();
  if (state == cap::kRecording) {
    std::lock_guard lock(cap::g_mutex);
    cap::Event(fmt::format("Swap front_buffer={:08X} params={:08X}", ctx.r4.u32, ctx.r5.u32));
    cap::DumpObjectOnce(base, "texture", ctx.r4.u32, 10);
    if (ctx.r4.u32) {
      cap::g_out += "  front " + cap::DescribeTextureFetch(base, ctx.r4.u32 + 16) + "\n";
    }
    cap::WriteFrame();
    cap::g_state = cap::kIdle;
  } else if (state == cap::kArmed) {
    std::lock_guard lock(cap::g_mutex);
    cap::g_out = fmt::format(
        "# Amped 3 frame capture, frame {} (counted from start-up), device {:08X}\n"
        "# Layout: docs/d3d-map.md. Format: docs/frame-capture.md.\n",
        frame + 1, ctx.r3.u32);
    cap::g_events = 0;
    cap::g_draws = 0;
    cap::g_prev_regs.clear();
    cap::g_prev_consts.clear();
    cap::g_prev_fetch.clear();
    cap::g_objects_dumped.clear();
    cap::g_stamp = cap::Stamp();
    cap::g_res_dir = cap::g_dir / fmt::format("frame_{}", cap::g_stamp);
    cap::g_res_index.clear();
    cap::g_res_done.clear();
    cap::g_res_bytes = 0;
    cap::g_res_full_logged = false;
    cap::g_state = cap::kRecording;
  }
  amped3::trace::Scope swap_scope(amped3::trace::Zone::kSwap);
  __imp__D3DDevice_Swap(ctx, base);
}

// Timing only (F11 performance trace): where the game thread waits on the GPU
// and what the command stream costs. See docs/d3d-map.md for these functions.
// D3D_FlushDirtyState(pDevice), at the start of every draw: walks the dirty
// masks at device+16..+48 (64-bit, big-endian) and writes each dirty register
// block, the shader constants and the fetch constants as GPU commands (see
// docs/d3d-map.md and docs/experimental-native-gpu.md). While the emulated GPU
// doesn't draw, nothing reads those commands: only the two calls that change
// CPU-side state the recording reads are kept (the vertex fetch constants built
// from the streams, and the shader setup that patches the vertex shader for
// the declaration), plus one shadow store, in the original order; the masks
// are cleared as the original does. When the emulation draws again, every mask
// is set once so the original writes the whole state.
namespace {
inline void Store64(uint8_t* base, uint32_t address, uint64_t v) {
  rex::memory::store_and_swap<uint64_t>(base + address, v);
}

void FlushDirtyStateNoCommands(PPCContext& ctx, uint8_t* base, uint32_t dev) {
  if (cap::U64(base, dev + 48)) {
    if (cap::U64(base, dev + 48) & 0x400) {  // streams / declaration
      ctx.r3.u64 = dev;
      D3D_CommitVertexFetch(ctx, base);
    }
    if (cap::U64(base, dev + 48) & 0x1E0) {  // shaders
      ctx.r3.u64 = dev;
      D3D_CommitShaders(ctx, base);
    }
    Store64(base, dev + 48, 0);
  }
  if (cap::U64(base, dev + 16)) Store64(base, dev + 16, 0);  // vertex shader constants
  if (cap::U64(base, dev + 24)) Store64(base, dev + 24, 0);  // pixel shader constants
  if (const uint64_t fetch = cap::U64(base, dev + 32)) {     // fetch constants, misc
    if (fetch & 0x80000000u) Store64(base, dev + 11440, 0xFFFFFFFFFF000000ull);
    Store64(base, dev + 32, 0);
  }
  if (cap::U64(base, dev + 40)) Store64(base, dev + 40, 0);
}
}  // namespace

extern "C" REX_FUNC(D3D_FlushDirtyState) {
  amped3::trace::Scope scope(amped3::trace::Zone::kFlushState);
  amped3::trace::NoteDrawThread();
  static bool skipping = false;  // game thread only
  const uint32_t dev = ctx.r3.u32;
  if (amped3::live::SkipEmulatedStateWrites()) {
    skipping = true;
    FlushDirtyStateNoCommands(ctx, base, dev);
    return;
  }
  if (skipping) {
    skipping = false;
    for (uint32_t off = 16; off <= 48; off += 8) Store64(base, dev + off, ~0ull);
  }
  __imp__D3D_FlushDirtyState(ctx, base);
}

extern "C" REX_FUNC(D3D_BlockOnFence) {
  amped3::trace::Scope scope(amped3::trace::Zone::kFenceWait);
  __imp__D3D_BlockOnFence(ctx, base);
}

extern "C" REX_FUNC(D3DDevice_BlockUntilIdle) {
  amped3::trace::Scope scope(amped3::trace::Zone::kFenceWait);
  __imp__D3DDevice_BlockUntilIdle(ctx, base);
}

extern "C" REX_FUNC(D3DResource_BlockUntilNotBusy) {
  amped3::trace::Scope scope(amped3::trace::Zone::kFenceWait);
  __imp__D3DResource_BlockUntilNotBusy(ctx, base);
}

extern "C" REX_FUNC(D3D_MakeSpace) {
  amped3::trace::Scope scope(amped3::trace::Zone::kMakeSpace);
  __imp__D3D_MakeSpace(ctx, base);
}

// D3D_PatchVertexShaderFetches(pDevice, pVertexShader, pMicrocode, pDecl):
// fills in the vertex fetches of the microcode at pMicrocode (the shader's own
// or a command-buffer copy) for the declaration.
extern "C" REX_FUNC(D3D_PatchVertexShaderFetches) {
  const uint32_t vs = ctx.r4.u32, ucode = ctx.r5.u32, decl = ctx.r6.u32;
  __imp__D3D_PatchVertexShaderFetches(ctx, base);
  if (cap::TrackShaders() && vs) {
    cap::g_cur_vs_ucode = cap::SaveDrawnMicrocode(
        base, "vs", vs, ucode, cap::U32(base, vs + cap::kVertexShaderUcodeSize), decl);
    cap::g_patched_in_commit = true;
  }
}

// D3D_CommitShaders(pDevice): loads the current shaders. When it doesn't
// patch (the shader's own microcode already matches the declaration), the
// loaded vertex shader is the one at +40.
extern "C" REX_FUNC(D3D_CommitShaders) {
  const uint32_t dev = ctx.r3.u32;
  cap::g_patched_in_commit = false;
  __imp__D3D_CommitShaders(ctx, base);
  if (!cap::TrackShaders() || !dev) return;
  const uint32_t vs = cap::U32(base, dev + cap::kVertexShaderLoaded);
  const uint32_t decl = cap::U32(base, dev + cap::kVertexDecl);
  const uint32_t ps = cap::U32(base, dev + cap::kPixelShader);
  if (!cap::g_patched_in_commit && vs && (vs != cap::g_commit_vs || decl != cap::g_commit_decl)) {
    cap::g_cur_vs_ucode = cap::SaveDrawnMicrocode(
        base, "vs", vs, cap::U32(base, vs + cap::kVertexShaderUcode),
        cap::U32(base, vs + cap::kVertexShaderUcodeSize), decl);
  }
  cap::g_commit_vs = vs;
  cap::g_commit_decl = decl;
  if (ps && ps != cap::g_commit_ps) {
    cap::g_cur_ps_ucode = cap::SaveDrawnMicrocode(
        base, "ps", ps, cap::U32(base, ps + cap::kPixelShaderUcode),
        cap::U32(base, ps + cap::kPixelShaderUcodeSize), decl);
  }
  cap::g_commit_ps = ps;
}

// --- Shader creation -----------------------------------------------------------------

extern "C" REX_FUNC(D3DDevice_CreateVertexShader) {
  const uint32_t blob = ctx.r3.u32;
  const std::string stem = REXCVAR_GET(capture_shaders) ? cap::DumpShaderBlob(base, "vs", blob) : "";
  __imp__D3DDevice_CreateVertexShader(ctx, base);
  if (!stem.empty()) {
    {
      std::lock_guard lock(cap::g_shader_mutex);
      cap::g_shader_names[ctx.r3.u32] = stem;
    }
    cap::AppendIndex(fmt::format("{} object={:08X} blob={:08X}", stem, ctx.r3.u32, blob));
  }
}

extern "C" REX_FUNC(D3DDevice_CreatePixelShader) {
  const uint32_t blob = ctx.r3.u32;
  const std::string stem = REXCVAR_GET(capture_shaders) ? cap::DumpShaderBlob(base, "ps", blob) : "";
  __imp__D3DDevice_CreatePixelShader(ctx, base);
  if (!stem.empty()) {
    {
      std::lock_guard lock(cap::g_shader_mutex);
      cap::g_shader_names[ctx.r3.u32] = stem;
    }
    cap::AppendIndex(fmt::format("{} object={:08X} blob={:08X}", stem, ctx.r3.u32, blob));
  }
}

// D3DXAssembleShader(pSrcData, SrcDataLen, pDefines, pInclude, Flags, ppShader,
// ppErrorMsgs): the assembly text the game builds at run time.
extern "C" REX_FUNC(D3DXAssembleShader) {
  if (REXCVAR_GET(capture_shaders) && ctx.r3.u32 && ctx.r4.u32 && ctx.r4.u32 < 0x100000) {
    const uint64_t hash = cap::Fnv1a(base + ctx.r3.u32, ctx.r4.u32);
    const std::string stem = fmt::format("asm_{:016X}", hash);
    cap::SaveFile(cap::g_dir / "shaders" / (stem + ".txt"), base + ctx.r3.u32, ctx.r4.u32);
    cap::AppendIndex(fmt::format("{} length={} flags={:08X}", stem, ctx.r4.u32, ctx.r7.u32));
  }
  __imp__D3DXAssembleShader(ctx, base);
}
