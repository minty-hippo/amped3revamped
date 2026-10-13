// Native renderer preview: see live_renderer.h.
//
// Recording (game thread) is kept cheap: the GPU state is diffed against the
// previous draw in 32-byte chunks straight from guest memory and only the
// changed chunks are copied; vertex and index data is copied once per frame
// per range; textures are only named here (fetch constant + a sampled hash of
// their data) and read from guest memory by the worker when first needed.

#include "live_renderer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>

#include "live_present.h"
#include "perf_overlay.h"
#include "perf_trace.h"
#include "thread_qos.h"
#include "presenter_external.h"
#include "native/frame.h"
#include "native/frame_writer.h"
#include "native/glsl_compiler.h"
#include "native/renderer.h"
#include "native/xenos_shader.h"
#include "texture_ranges.h"

REXCVAR_DEFINE_BOOL(native_renderer, true, "GPU",
                    "Draw the game with the native Vulkan renderer (off: the emulated GPU's "
                    "image, as the console draws it)");
REXCVAR_DEFINE_BOOL(revamped_graphics, true, "GPU",
                    "Native renderer: Revamped Graphics, the switch in the game's own Options "
                    "menu. Off turns every visual enhancement off at once (motion blur, speed "
                    "blur, distance fog, god rays, foliage fade, map clouds); on, each one "
                    "follows its own setting");
REXCVAR_DEFINE_BOOL(native_keep_emulation, false, "GPU",
                    "Native renderer: keep the emulated GPU drawing every frame underneath "
                    "(slower; for comparing the two)");
REXCVAR_DEFINE_BOOL(native_skip_state_writes, true, "GPU",
                    "Native renderer: while the emulated GPU doesn't draw, Direct3D's state "
                    "flush before each draw skips writing the changed registers and shader "
                    "constants as GPU commands (only the vertex fetch and shader setup the "
                    "recording reads still run). Everything is written again when the "
                    "emulation draws.");
REXCVAR_DEFINE_BOOL(native_vertex_cache, true, "GPU",
                    "Native renderer: reuse vertex buffers the game hasn't written to since the "
                    "last frame (write-watched pages) instead of copying them every frame");
REXCVAR_DEFINE_BOOL(native_msaa, true, "GPU",
                    "Native renderer: use the game's multisampling (MSAA) as the Xbox 360 does "
                    "(off: one sample per pixel, like the emulated image)");
REXCVAR_DEFINE_INT32(native_render_scale, 100, "GPU",
                     "Native renderer: the 3D scene's resolution, in percent of the picture's "
                     "size on screen (the window, or its 16:9 part when letterboxed); the HUD, "
                     "menus and text are always drawn at full size")
    .range(25, 100);
REXCVAR_DEFINE_STRING(native_upscaler, "off", "GPU",
                      "Native renderer: temporal upscaler from the render scale to the "
                      "picture's size on screen: off, xess (Intel XeSS: XMX on Arc, DP4a "
                      "elsewhere) or fsr (AMD FSR 3.1). Needs libxess.dll / "
                      "amd_fidelityfx_vk.dll next to the exe (setup.ps1 fetches them)")
    .allowed({"off", "xess", "fsr"});
REXCVAR_DEFINE_DOUBLE(native_upscaler_sharpness, 0.3, "GPU",
                      "Native renderer: FSR's sharpening, 0 (off) to 1")
    .range(0.0, 1.0);
REXCVAR_DEFINE_BOOL(native_camera_motion_blur, true, "GPU",
                    "Native renderer: blur the scenery along the camera's motion (not the HUD; "
                    "the rider and the ground right around them stay sharp)");
REXCVAR_DEFINE_BOOL(native_object_motion_blur, true, "GPU",
                    "Native renderer: blur other riders and characters along their own motion, "
                    "with soft edges scaled by their speed; the player's "
                    "rider and board are exempt. With camera motion blur on too, everything else "
                    "blurs by its motion on screen");
REXCVAR_DEFINE_DOUBLE(native_camera_motion_blur_strength, 1.0, "GPU",
                      "Native renderer: motion blur length (camera and object blur). 1 = the "
                      "motion of 1/60 s (half a frame at 30 fps), the same at any frame rate")
    .range(0.0, 3.0);
REXCVAR_DEFINE_BOOL(native_speed_blur, true, "GPU",
                    "Native renderer: a radial blur at the edges of the screen that fades in as "
                    "the rider picks up speed and grows stronger the faster they go (the HUD "
                    "and the middle of the screen stay sharp)");
REXCVAR_DEFINE_DOUBLE(native_speed_blur_strength, 1.0, "GPU",
                      "Native renderer: speed blur strength (0..2)")
    .range(0.0, 2.0);
REXCVAR_DEFINE_BOOL(native_fog, true, "GPU",
                    "Native renderer: distance fog: haze that builds with distance and is "
                    "thicker low down, lit by the sky and the sun (or moon) everywhere, "
                    "brightest looking toward it");
REXCVAR_DEFINE_DOUBLE(native_fog_density, 1.0, "GPU",
                      "Native renderer: distance fog density (1: about half hazed 2 km away)")
    .range(0.0, 3.0);
REXCVAR_DEFINE_DOUBLE(native_fog_start, 300.0, "GPU",
                      "Native renderer: distance fog starts this far from the camera (metres)")
    .range(0.0, 1000.0);
REXCVAR_DEFINE_BOOL(native_smooth_foliage, true, "GPU",
                    "Native renderer, with multisampling: tree branches and other cut-out "
                    "textures get smooth anti-aliased edges, solid as on the console (the "
                    "GPU's own alpha to coverage makes them see-through and stippled)");
REXCVAR_DEFINE_BOOL(native_map_clouds, true, "GPU",
                    "Native renderer: at a cut to or from the trail map, cartoon clouds close "
                    "in over the old view and part from the middle out over the new one (just "
                    "part, loading into a level on the map); the distance fog fades with them");
REXCVAR_DEFINE_BOOL(native_sharp_fonts, true, "GPU",
                    "Native renderer: the game's fonts (menus, HUD, scores, button icons) "
                    "rebuilt at 4x their size from their own letters when first shown, so text "
                    "stays sharp at high resolutions instead of being a stretched 720p bitmap");
REXCVAR_DEFINE_BOOL(native_texture_pack, true, "GPU",
                    "Native renderer: replaces game textures with the PNGs in the textures "
                    "folder next to the exe (<name>_<hash>.png; see textures\\README.txt). "
                    "It comes with the new Amped 3 logo for the start-up and title screens");
REXCVAR_DEFINE_BOOL(native_upscaled_textures, true, "GPU",
                    "Native renderer: uses the game textures upscaled on this PC at install "
                    "(textures\\upscaled, made by the installer's optional texture upscaling "
                    "step) for every texture the texture pack and Sharp fonts don't replace");
REXCVAR_DEFINE_BOOL(native_foliage_fade, true, "GPU",
                    "Native renderer, with smooth foliage edges: tree branches close to the "
                    "camera fade out smoothly toward the middle of the screen, keeping your "
                    "rider and the way ahead in view; trees the game fades near the camera "
                    "turn evenly see-through instead of breaking into ragged holes");
REXCVAR_DEFINE_BOOL(native_god_rays, true, "GPU",
                    "Native renderer: light shafts from the sun (or moon) while it's in or near "
                    "view, broken up by clouds, trees and terrain in front of it");
REXCVAR_DEFINE_DOUBLE(native_god_rays_strength, 1.0, "GPU",
                      "Native renderer: god ray strength (0..2)")
    .range(0.0, 2.0);
REXCVAR_DEFINE_BOOL(native_motion_blur_debug, false, "GPU",
                    "Native renderer, diagnostics: show the motion blur's motion instead of the "
                    "picture (red = sideways, green = up/down, blue = object pixels, grey = "
                    "the player, exempt)");

namespace amped3::live {
namespace {

using native::Draw;
using native::Frame;
using native::GpuState;

// --- Device layout (docs\d3d-map.md) --------------------------------------
constexpr uint32_t kFetchConsts = 1152;     // 32 x 24 bytes
constexpr uint32_t kStateEnd = 10272;       // fetch, float (512 x float4), bool/loop (40)
constexpr uint32_t kFloatConstsOffset = 768;  // within the state block
constexpr uint32_t kBoolLoopOffset = 768 + 8192;
constexpr uint32_t kRegShadow = 11456;      // register shadow blocks 0x2000..0x23FF
constexpr uint32_t kRegShadowEnd = 12000;
constexpr uint32_t kIndexBuffer = 12532;
constexpr uint32_t kTextures = 12704;       // 26 slots
constexpr uint32_t kTextureSlots = 26;
struct ShadowBlock {
  uint32_t offset;
  uint32_t first_register;
};
constexpr ShadowBlock kBlocks[] = {{11456, 0x2000}, {11532, 0x2100}, {11616, 0x2180},
                                   {11636, 0x2200}, {11684, 0x2280}, {11768, 0x2300},
                                   {11920, 0x2380}};

// The raw state image: [fetch | float consts | bool/loop] then the register
// shadow, big-endian as in guest memory.
constexpr uint32_t kStateBytes = kStateEnd - kFetchConsts;           // 9120
constexpr uint32_t kShadowBytes = kRegShadowEnd - kRegShadow;        // 544
constexpr uint32_t kRawBytes = kStateBytes + kShadowBytes;
constexpr uint32_t kChunk = 32;
constexpr uint32_t kChunks = kRawBytes / kChunk;
static_assert(kStateBytes % kChunk == 0 && kShadowBytes % kChunk == 0);

constexpr uint32_t kMaxEvents = 40000;

inline uint32_t U32(const uint8_t* p) { return rex::memory::load_and_swap<uint32_t>(p); }
inline uint32_t U32(uint8_t* base, uint32_t address) { return U32(base + address); }
inline float F32(uint8_t* base, uint32_t address) {
  const uint32_t bits = U32(base, address);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint64_t Fnv1a(const void* data, size_t size, uint64_t h = 1469598103934665603ull) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

// A fast 64-bit hash (8 bytes at a time) for texture contents.
uint64_t HashBytes(const uint8_t* p, size_t size, uint64_t h) {
  constexpr uint64_t kMul = 0x9E3779B97F4A7C15ull;
  size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    uint64_t v;
    std::memcpy(&v, p + i, 8);
    h = (h ^ v) * kMul;
    h ^= h >> 29;
  }
  for (; i < size; ++i) h = (h ^ p[i]) * kMul;
  return h ^ (h >> 32);
}

// 32 bytes equal (inlined; memcmp calls cost more than the compare).
inline bool Same32(const uint8_t* a, const uint8_t* b) {
  uint64_t x[4], y[4];
  std::memcpy(x, a, 32);
  std::memcpy(y, b, 32);
  return ((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) == 0;
}

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

std::atomic<uint64_t> g_blob_version{0};
uint64_t NewVersion() { return g_blob_version.fetch_add(1, std::memory_order_relaxed) + 1; }

// --- Recorded frame ---------------------------------------------------------------

struct RecordedFrame {
  Frame frame;
  // Vertex / index data. Shared: unchanged vertex buffers are the same
  // cached copy from frame to frame (see the vertex cache).
  std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> blobs;
  // Per blob: changes whenever its bytes may have changed (the renderer keeps
  // vertex data on the GPU while it stays the same).
  std::unordered_map<std::string, uint64_t> versions;
  std::unordered_map<std::string, std::array<uint32_t, 6>> textures;  // name -> fetch words
  // GPU state: per draw, the chunks that changed since the previous draw.
  std::vector<uint8_t> deltas;              // [u16 chunk][32 bytes] records
  std::vector<uint32_t> draw_delta_end;     // per draw: end offset in `deltas`
  std::string dump_name;                    // non-empty: also save as a capture (F10)
  bool states_built = false;                // BuildStates done (by the prep thread)
  double states_ms = 0;                     // its time
  std::chrono::steady_clock::time_point swap_time;  // when the game swapped it
};

// Microcode by stem (all the game has run so far).
std::mutex g_ucode_mutex;
std::unordered_map<std::string, std::vector<uint8_t>> g_ucode;

std::vector<uint8_t> FindMicrocode(const std::string& stem) {
  std::lock_guard lock(g_ucode_mutex);
  auto it = g_ucode.find(stem);
  return it == g_ucode.end() ? std::vector<uint8_t>{} : it->second;
}

// What a shader reads, from its translation (cached per microcode stem).
struct ShaderUse {
  bool known = false;                  // translated; else assume everything
  std::vector<uint32_t> vertex_fetch;  // VS: vertex fetch constants read
  std::vector<uint32_t> textures;      // texture fetch constants read
  // Float constant chunks (32 bytes = 2 constants) the shader reads: bit k of
  // chunks[k >> 6] for chunk k (0..127); all when it indexes them with a0/aL.
  bool all_constants = true;
  uint64_t chunks[2] = {};
};
std::unordered_map<std::string, ShaderUse> g_shader_use;

// The last shader looked up per stage: consecutive draws mostly share their
// shaders, so a string compare usually replaces the map lookup.
struct ShaderUseCache {
  std::string stem;
  const ShaderUse* use = nullptr;
};
ShaderUseCache g_use_cache[2];

const ShaderUse& UseOf(const std::string& stem, native::ShaderStage stage) {
  static const ShaderUse kUnknown;
  if (stem.empty()) return kUnknown;
  ShaderUseCache& cache = g_use_cache[stage == native::ShaderStage::kVertex ? 0 : 1];
  if (cache.use && cache.stem == stem) return *cache.use;
  auto it = g_shader_use.find(stem);
  if (it != g_shader_use.end()) {
    cache.stem = stem;
    cache.use = &it->second;
    return it->second;
  }
  const auto bytes = FindMicrocode(stem);
  if (bytes.size() < 12) return kUnknown;  // not seen yet; try again next time
  ShaderUse use;
  std::vector<uint32_t> ucode(bytes.size() / 4);
  for (size_t i = 0; i < ucode.size(); ++i) ucode[i] = U32(bytes.data() + i * 4);
  auto r = native::TranslateShader(stage, ucode.data(), ucode.size());
  if (r.ok) {
    use.known = true;
    for (uint32_t i = 0; i < 96; ++i) {
      if (r.info.vertex_fetch_constants[i >> 5] & (1u << (i & 31))) use.vertex_fetch.push_back(i);
    }
    for (const auto& t : r.info.textures) {
      if (std::find(use.textures.begin(), use.textures.end(), t.fetch_constant) ==
          use.textures.end()) {
        use.textures.push_back(t.fetch_constant);
      }
    }
    use.all_constants = r.info.dynamic_constants;
    for (uint32_t c = 0; c < 256; ++c) {
      if (r.info.float_constants[c >> 5] & (1u << (c & 31))) {
        use.chunks[(c / 2) >> 6] |= 1ull << ((c / 2) & 63);
      }
    }
  }
  const ShaderUse& stored = g_shader_use.emplace(stem, std::move(use)).first->second;
  cache.stem = stem;
  cache.use = &stored;
  return stored;
}

const std::vector<uint32_t>& VertexFetchSlots(const std::string& stem) {
  return UseOf(stem, native::ShaderStage::kVertex).vertex_fetch;
}

// --- Recorder state (game thread, under g_mutex) -------------------------------------

std::filesystem::path g_dir;
std::function<rex::ui::Presenter*()> g_presenter;
std::string g_backend;
std::function<bool(GammaRamp*)> g_gamma;
std::function<bool(bool)> g_host_drawing;
// Whether the emulated GPU draws (as last requested; SDK patch 0009 applies
// it at the next swap).
std::atomic<bool> g_host_drawing_on{true};

std::mutex g_mutex;
std::atomic<bool> g_recording{false};
std::atomic<bool> g_dump_requested{false};
std::unique_ptr<RecordedFrame> g_frame;
std::array<uint8_t, kRawBytes> g_prev_raw{};
std::unordered_map<uint64_t, std::string> g_tex_names;  // fetch words hash -> name (per frame)
// Vertex / index ranges already copied this frame: (kind, address, size) -> name.
std::unordered_map<uint64_t, std::string> g_blob_names;
// What each texture slot and vertex fetch constant was named at the previous
// draw this frame: draws in a row mostly keep their bindings, so comparing the
// raw fetch words usually replaces hashing and the map lookups.
struct SlotName {
  std::array<uint32_t, 6> words{};
  std::string name;
  uint64_t frame = 0;  // g_record_frame it was set in (names are per frame)
};
struct VertexFetchName {
  uint32_t w0 = 0, w1 = 0;
  std::string name;
  uint64_t frame = 0;
};
uint32_t g_events = 0;
struct PendingVertices {
  uint32_t address = 0, size = 0;
  std::string name;
};
PendingVertices g_pending;
// Sizes of the last recorded frame (to reserve the next one's vectors).
size_t g_last_draws = 0, g_last_events = 0, g_last_deltas = 0;
bool g_frame_overflow = false;

// --- Worker ---------------------------------------------------------------------------

std::mutex g_work_mutex;
std::condition_variable g_work_cv;
std::unique_ptr<RecordedFrame> g_work;  // latest finished frame, waiting for the worker
std::atomic<bool> g_stop{false};
std::thread g_worker;
// Prep thread: builds each frame's per-draw GPU states from the recorded
// deltas, then hands the frame to the worker, so that work overlaps the
// worker drawing the previous frame instead of adding to it.
std::mutex g_prep_mutex;
std::condition_variable g_prep_cv;
std::unique_ptr<RecordedFrame> g_prep;  // waiting for the prep thread
std::thread g_prep_thread;
std::atomic<bool> g_failed{false};
std::atomic<bool> g_hide_interface{false};  // SetHideInterface

std::mutex g_status_mutex;
Status g_status;

void SetMessage(const std::string& message) {
  std::lock_guard lock(g_status_mutex);
  g_status.message = message;
}

// --- Recording helpers --------------------------------------------------------------

void FlushPending() {
  PendingVertices p = std::move(g_pending);
  g_pending = {};
  if (p.name.empty() || !g_frame) return;
  auto* memory = GuestMemory();
  if (!memory || !p.size || p.size > (16u << 20)) return;
  const uint8_t* src = memory->TranslateVirtual<const uint8_t*>(p.address);
  g_frame->blobs[p.name] = std::make_shared<std::vector<uint8_t>>(src, src + p.size);
  g_frame->versions[p.name] = NewVersion();
}

uint32_t AddEvent(native::EventType type, uint32_t index) {
  FlushPending();
  g_frame->frame.events.push_back({type, index, g_events});
  return g_events++;
}

bool Room() {
  if (g_events < kMaxEvents) return true;
  if (!g_frame_overflow) {
    g_frame_overflow = true;
    SetMessage("frame too long to record");
  }
  return false;
}

// Blob buffers of drawn frames, reused by name: the same vertex and index
// buffers are copied every frame, and writing into memory already mapped
// avoids a fresh allocation (and its page faults) each time. Only buffers no
// one else holds (not the vertex cache's) come back.
std::mutex g_recycle_mutex;
std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> g_recycled;

void RecycleBlobs(RecordedFrame& f) {
  std::lock_guard lock(g_recycle_mutex);
  if (g_recycled.size() > 8192) g_recycled.clear();
  for (auto& [name, blob] : f.blobs) {
    if (blob && blob.use_count() == 1 && blob->capacity() >= 4096) {
      g_recycled[name] = std::move(blob);
    }
  }
  f.blobs.clear();
}

std::shared_ptr<std::vector<uint8_t>> NewBlob(const std::string& name, const uint8_t* src,
                                              uint32_t size) {
  std::shared_ptr<std::vector<uint8_t>> blob;
  if (size >= 4096) {
    std::lock_guard lock(g_recycle_mutex);
    auto it = g_recycled.find(name);
    if (it != g_recycled.end()) {
      blob = std::move(it->second);
      g_recycled.erase(it);
    }
  }
  if (!blob) blob = std::make_shared<std::vector<uint8_t>>();
  blob->assign(src, src + size);
  return blob;
}

void CopyGuest(const std::string& name, const uint8_t* src, uint32_t size) {
  auto& blob = g_frame->blobs[name];
  if (!blob) {
    blob = NewBlob(name, src, size);
    g_frame->versions[name] = NewVersion();
  }
}

// --- Vertex cache ---------------------------------------------------------------------
//
// Most vertex buffers don't change from frame to frame. A copied buffer's
// physical pages are write-watched (Memory::EnablePhysicalMemoryAccessCallbacks,
// the mechanism the emulated GPU uses for its copy of guest memory): the first
// write to a watched page bumps that page's write epoch, and the buffer is
// copied again only when one of its pages was written after its copy. Buffers
// written every frame (dynamic) are not watched (a page fault per page per
// frame would cost more than the copy) and are copied each frame. As a safety
// net every cached buffer is copied again every few seconds.

constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPhysicalPages = (512u << 20) >> kPageShift;
std::unique_ptr<std::atomic<uint32_t>[]> g_page_epoch;
std::atomic<uint32_t> g_write_epoch{1};
void* g_watch_handle = nullptr;
rex::memory::Memory* g_watch_memory = nullptr;

// Called with the memory system's lock held, on the writing thread: no locks.
std::pair<uint32_t, uint32_t> OnPhysicalWrite(void*, uint32_t start, uint32_t length, bool) {
  if (!length) return {start, length};
  const uint32_t epoch = g_write_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
  const uint32_t first = start >> kPageShift;
  const uint32_t last = std::min((start + length - 1) >> kPageShift, kPhysicalPages - 1);
  for (uint32_t p = first; p <= last; ++p) g_page_epoch[p].store(epoch, std::memory_order_release);
  // Only these pages may be unwatched: the cache can't hear about others.
  return {start, length};
}

struct CachedVertices {
  std::shared_ptr<std::vector<uint8_t>> data;
  uint64_t version = 0;       // NewVersion() at the last copy
  uint32_t epoch = 0;         // write epoch when copied
  uint64_t copied_frame = 0;  // recording frame number of the copy
  uint64_t used_frame = 0;
  uint32_t dirty_streak = 0;  // consecutive uses that found it rewritten
  bool dynamic = false;
  uint64_t dynamic_since = 0;
};
std::unordered_map<uint64_t, CachedVertices> g_vertex_cache;
uint64_t g_record_frame = 0;  // frames recorded (game thread)
struct VertexCacheStats {
  std::atomic<uint64_t> hits{0}, copies{0}, hit_bytes{0}, copy_bytes{0};
} g_vcache_stats;

bool VertexCacheEnabled() { return REXCVAR_GET(native_vertex_cache); }

bool PagesClean(uint32_t address, uint32_t size, uint32_t epoch) {
  const uint32_t first = address >> kPageShift;
  const uint32_t last = std::min((address + size - 1) >> kPageShift, kPhysicalPages - 1);
  for (uint32_t p = first; p <= last; ++p) {
    if (g_page_epoch[p].load(std::memory_order_acquire) > epoch) return false;
  }
  return true;
}

// The bytes of a vertex buffer (physical address) for this frame.
std::shared_ptr<std::vector<uint8_t>> CachedVertexData(rex::memory::Memory* memory,
                                                       const std::string& name, uint32_t address,
                                                       uint32_t size, uint64_t* version) {
  const uint8_t* src = memory->TranslatePhysical<const uint8_t*>(address);
  if (!VertexCacheEnabled() || address + uint64_t(size) > (512ull << 20)) {
    ++g_vcache_stats.copies;
    g_vcache_stats.copy_bytes += size;
    *version = NewVersion();
    return NewBlob(name, src, size);
  }
  if (!g_page_epoch) {
    g_page_epoch.reset(new std::atomic<uint32_t>[kPhysicalPages]);
    for (uint32_t p = 0; p < kPhysicalPages; ++p) g_page_epoch[p].store(0);
  }
  if (!g_watch_handle) {
    g_watch_memory = memory;
    g_watch_handle = memory->RegisterPhysicalMemoryInvalidationCallback(OnPhysicalWrite, nullptr);
  }
  const uint64_t key = (uint64_t(address) << 32) | size;
  CachedVertices& e = g_vertex_cache[key];
  e.used_frame = g_record_frame;
  // Staggered safety refresh (every ~4 s at 60 fps).
  const bool refresh = g_record_frame - e.copied_frame > 240 + (key % 61);
  if (e.data && !e.dynamic && !refresh && PagesClean(address, size, e.epoch)) {
    e.dirty_streak = 0;
    ++g_vcache_stats.hits;
    g_vcache_stats.hit_bytes += size;
    *version = e.version;
    return e.data;
  }
  if (e.data && !refresh && !e.dynamic) {
    // Rewritten since the last copy: after a few uses in a row it is treated
    // as dynamic (and gets another chance after 600 frames).
    if (++e.dirty_streak >= 3) {
      e.dynamic = true;
      e.dynamic_since = g_record_frame;
    }
  }
  if (e.dynamic && g_record_frame - e.dynamic_since > 600) {
    e.dynamic = false;
    e.dirty_streak = 0;
  }
  const uint32_t epoch = g_write_epoch.load(std::memory_order_acquire);
  if (!e.dynamic) memory->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
  // Reuse the cached vector when no recorded frame still holds it.
  if (e.data && e.data.use_count() == 1) {
    e.data->assign(src, src + size);
  } else {
    e.data = std::make_shared<std::vector<uint8_t>>(src, src + size);
  }
  e.epoch = epoch;
  e.version = NewVersion();
  *version = e.version;
  e.copied_frame = g_record_frame;
  ++g_vcache_stats.copies;
  g_vcache_stats.copy_bytes += size;
  return e.data;
}

// Forgets buffers not drawn for a while (once a second or so).
void TrimVertexCache() {
  if (g_record_frame % 64) return;
  for (auto it = g_vertex_cache.begin(); it != g_vertex_cache.end();) {
    if (g_record_frame - it->second.used_frame > 600) {
      it = g_vertex_cache.erase(it);
    } else {
      ++it;
    }
  }
}

// The six fetch constant words (host order) at a guest address.
std::array<uint32_t, 6> FetchWords(uint8_t* base, uint32_t address) {
  std::array<uint32_t, 6> w;
  for (int i = 0; i < 6; ++i) w[i] = U32(base, address + 4 * i);
  return w;
}

// A name for a texture: its fetch constant plus a hash of its data, so the
// renderer's cache notices when a texture's contents change.
//
// The hash is kept per memory range and recomputed (over all of it) only when
// a page of the range was written since: the ranges are write-watched like
// vertex buffers. Textures rewritten every frame (video) stop being watched
// and are hashed every frame. A texture that changed is also copied into the
// frame now, so the worker decodes these bytes and not later ones (the game
// decodes the next video frame into the same buffers while the worker works).
struct CachedTextureHash {
  uint64_t hash = 0;
  uint32_t epoch = 0;
  uint64_t used_frame = 0;
  uint32_t dirty_streak = 0;
  bool dynamic = false;
  uint64_t dynamic_since = 0;
};
std::unordered_map<uint64_t, CachedTextureHash> g_texture_hashes;  // (address, size)
constexpr size_t kTextureSnapshotBudget = 16u << 20;  // bytes copied a frame at most
size_t g_texture_snapshot_bytes = 0;                  // this frame

void EnsureWriteWatch(rex::memory::Memory* memory) {
  if (!g_page_epoch) {
    g_page_epoch.reset(new std::atomic<uint32_t>[kPhysicalPages]);
    for (uint32_t p = 0; p < kPhysicalPages; ++p) g_page_epoch[p].store(0);
  }
  if (!g_watch_handle) {
    g_watch_memory = memory;
    g_watch_handle = memory->RegisterPhysicalMemoryInvalidationCallback(OnPhysicalWrite, nullptr);
  }
}

// Hash of a texture range; *changed: its bytes differ from the last time
// (or it is new).
uint64_t TextureRangeHash(rex::memory::Memory* memory, uint32_t address, uint32_t size,
                          bool* changed) {
  *changed = false;
  if (!size) return 0;
  const uint8_t* p = memory->TranslatePhysical<const uint8_t*>(address);
  if (address + uint64_t(size) > (512ull << 20)) return HashBytes(p, size, 1469598103934665603ull);
  EnsureWriteWatch(memory);
  const uint64_t key = (uint64_t(address) << 32) | size;
  auto [it, inserted] = g_texture_hashes.try_emplace(key);
  CachedTextureHash& e = it->second;
  e.used_frame = g_record_frame;
  if (!inserted && !e.dynamic && PagesClean(address, size, e.epoch)) {
    e.dirty_streak = 0;
    return e.hash;
  }
  if (!inserted && !e.dynamic && ++e.dirty_streak >= 3) {
    e.dynamic = true;  // rewritten every frame: hash it every frame, unwatched
    e.dynamic_since = g_record_frame;
  }
  if (e.dynamic && g_record_frame - e.dynamic_since > 600) {
    e.dynamic = false;
    e.dirty_streak = 0;
  }
  const uint32_t epoch = g_write_epoch.load(std::memory_order_acquire);
  if (!e.dynamic) memory->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
  const uint64_t h = HashBytes(p, size, 1469598103934665603ull);
  *changed = inserted || h != e.hash;
  e.hash = h;
  e.epoch = epoch;
  return h;
}

// Forgets texture ranges not drawn for a while.
void TrimTextureHashes() {
  if (g_record_frame % 64) return;
  for (auto it = g_texture_hashes.begin(); it != g_texture_hashes.end();) {
    if (g_record_frame - it->second.used_frame > 600) {
      it = g_texture_hashes.erase(it);
    } else {
      ++it;
    }
  }
}

const std::string& TextureName(const std::array<uint32_t, 6>& words) {
  const uint64_t key = Fnv1a(words.data(), sizeof(words));
  auto it = g_tex_names.find(key);
  if (it != g_tex_names.end()) return it->second;
  uint64_t h = 1469598103934665603ull;
  auto* memory = GuestMemory();
  const TextureRanges ranges = GetTextureRanges(words.data());
  bool base_changed = false, mips_changed = false;
  if (memory && ranges.base_size) {
    h ^= TextureRangeHash(memory, ranges.base_address, ranges.base_size, &base_changed);
    if (ranges.mip_size) {
      h = (h * 0x100000001B3ull) ^
          TextureRangeHash(memory, ranges.mip_address, ranges.mip_size, &mips_changed);
    }
  }
  std::string name =
      native::ShortName('T', key, 6) + "_" + native::ShortName('T', h, 7).substr(1);
  g_frame->textures[name] = words;
  const std::string& stored = g_tex_names.emplace(key, name).first->second;
  // Snapshot what changed (within a budget; the rest is read by the worker).
  auto snapshot = [&](const char* suffix, uint32_t address, uint32_t size) {
    if (!size || g_texture_snapshot_bytes + size > kTextureSnapshotBudget) return;
    const uint8_t* p = memory->TranslatePhysical<const uint8_t*>(address);
    g_frame->blobs[name + suffix] = std::make_shared<std::vector<uint8_t>>(p, p + size);
    g_texture_snapshot_bytes += size;
  };
  if (base_changed) snapshot(".base", ranges.base_address, ranges.base_size);
  if (mips_changed) snapshot(".mips", ranges.mip_address, ranges.mip_size);
  return stored;
}

// Changed state chunks since the previous draw, straight from guest memory.
// Chunks of the state block (32 bytes each): fetch constants 0..23, vertex
// shader float constants 24..151, pixel shader ones 152..279, bool/loop
// 280..284; then the register shadow.
constexpr uint32_t kVsConstChunk = kFloatConstsOffset / kChunk;     // 24
constexpr uint32_t kPsConstChunk = kVsConstChunk + 4096 / kChunk;   // 152
constexpr uint32_t kBoolLoopChunk = kPsConstChunk + 4096 / kChunk;  // 280

// Changed state chunks since the previous draw, straight from guest memory.
// Float constants are compared only where this draw's shaders read them (most
// of the 8 KB is unused by any one shader); a constant skipped here is
// recorded when a later draw's shader reads it, so every draw's shaders see
// current values.
void RecordStateDelta(uint8_t* base, uint32_t dev, const ShaderUse& vs, const ShaderUse& ps) {
  auto& deltas = g_frame->deltas;
  auto record = [&](uint32_t chunk, const uint8_t* src) {
    uint8_t* prev = g_prev_raw.data() + chunk * kChunk;
    if (Same32(prev, src)) return;
    std::memcpy(prev, src, kChunk);
    const uint16_t c16 = uint16_t(chunk);
    const size_t at = deltas.size();
    deltas.resize(at + 2 + kChunk);
    std::memcpy(deltas.data() + at, &c16, 2);
    std::memcpy(deltas.data() + at + 2, src, kChunk);
  };
  const uint8_t* state = base + dev + kFetchConsts;
  for (uint32_t c = 0; c < kVsConstChunk; ++c) record(c, state + c * kChunk);
  auto constants = [&](uint32_t first, const ShaderUse& use) {
    if (!use.known || use.all_constants) {
      for (uint32_t k = 0; k < 128; ++k) record(first + k, state + (first + k) * kChunk);
      return;
    }
    for (uint32_t w = 0; w < 2; ++w) {
      for (uint64_t bits = use.chunks[w]; bits; bits &= bits - 1) {
        const uint32_t k = w * 64 + uint32_t(std::countr_zero(bits));
        record(first + k, state + (first + k) * kChunk);
      }
    }
  };
  constants(kVsConstChunk, vs);
  constants(kPsConstChunk, ps);
  for (uint32_t c = kBoolLoopChunk; c < kStateBytes / kChunk; ++c) record(c, state + c * kChunk);
  const uint8_t* shadow = base + dev + kRegShadow;
  for (uint32_t off = 0; off < kShadowBytes; off += kChunk) {
    record((kStateBytes + off) / kChunk, shadow + off);
  }
  g_frame->draw_delta_end.push_back(uint32_t(deltas.size()));
}

void StartFrame() {
  ++g_record_frame;
  TrimVertexCache();
  TrimTextureHashes();
  g_texture_snapshot_bytes = 0;
  g_frame = std::make_unique<RecordedFrame>();
  g_frame->frame.draws.reserve(g_last_draws + g_last_draws / 4 + 64);
  g_frame->frame.events.reserve(g_last_events + g_last_events / 4 + 64);
  g_frame->deltas.reserve(g_last_deltas + g_last_deltas / 4 + 4096);
  g_frame->draw_delta_end.reserve(g_last_draws + g_last_draws / 4 + 64);
  if (g_dump_requested.exchange(false)) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    g_frame->dump_name = std::string("frame_") + buf + "_native";
  }
  g_prev_raw.fill(0);
  g_tex_names.clear();
  g_blob_names.clear();
  g_events = 0;
  g_pending = {};
  g_frame_overflow = false;
  g_recording = true;
}

// --- Worker -----------------------------------------------------------------------------

// Resource reads for the renderer: recorded blobs, microcode, and textures
// straight from guest memory.
class LiveResources : public native::FrameResources {
 public:
  explicit LiveResources(RecordedFrame& f) : f_(f) {}
  const std::vector<uint8_t>* Peek(const std::string& name) override {
    auto it = f_.blobs.find(name);
    return it != f_.blobs.end() && it->second ? it->second.get() : nullptr;
  }
  uint64_t Version(const std::string& name) override {
    auto it = f_.versions.find(name);
    return it != f_.versions.end() ? it->second : 0;
  }
  std::vector<uint8_t> Read(const std::string& name) override {
    auto it = f_.blobs.find(name);
    if (it != f_.blobs.end()) return it->second ? *it->second : std::vector<uint8_t>{};
    if (name.rfind("ucode_", 0) == 0) return FindMicrocode(name);
    if (native::IsTextureResource(name)) {
      const size_t dot = name.rfind('.');
      if (dot == std::string::npos) return {};
      auto t = f_.textures.find(name.substr(0, dot));
      if (t == f_.textures.end()) return {};
      const TextureRanges ranges = GetTextureRanges(t->second.data());
      const bool mips = name.compare(dot, std::string::npos, ".mips") == 0;
      const uint32_t address = mips ? ranges.mip_address : ranges.base_address;
      const uint32_t size = mips ? ranges.mip_size : ranges.base_size;
      auto* memory = GuestMemory();
      if (!memory || !size) return {};
      const uint8_t* p = memory->TranslatePhysical<const uint8_t*>(address);
      return std::vector<uint8_t>(p, p + size);
    }
    return {};
  }

 private:
  RecordedFrame& f_;
};

// Rebuilds each draw's full GPU state from the recorded chunk deltas: the
// changed chunks are decoded into a running state, which is copied for each
// draw where something changed (draws in between share it).
void BuildStates(RecordedFrame& f) {
  // Register for each 32-bit word of the shadow region (0: none).
  static const std::array<uint16_t, kShadowBytes / 4> kShadowRegs = [] {
    std::array<uint16_t, kShadowBytes / 4> t{};
    for (uint32_t w = 0; w < kShadowBytes / 4; ++w) {
      const uint32_t guest = kRegShadow + w * 4;
      const ShadowBlock* block = &kBlocks[0];
      for (const auto& b : kBlocks) {
        if (guest >= b.offset) block = &b;
      }
      const uint32_t reg = block->first_register + (guest - block->offset) / 4;
      t[w] = (reg >= 0x2000 && reg < 0x2400) ? uint16_t(reg) : 0;
    }
    return t;
  }();
  auto cur = std::make_unique<GpuState>();
  std::shared_ptr<GpuState> current;
  uint32_t at = 0;
  for (size_t i = 0; i < f.frame.draws.size(); ++i) {
    const uint32_t end = i < f.draw_delta_end.size() ? f.draw_delta_end[i] : at;
    const bool changed = end > at || !current;
    for (; at + 2 + kChunk <= end; at += 2 + kChunk) {
      uint16_t chunk;
      std::memcpy(&chunk, f.deltas.data() + at, 2);
      if (chunk >= kChunks) continue;
      const uint8_t* src = f.deltas.data() + at + 2;
      for (uint32_t w = 0; w < kChunk / 4; ++w) {
        const uint32_t off = chunk * kChunk + w * 4;
        const uint32_t v = U32(src + w * 4);
        if (off < kStateBytes) {
          const uint32_t k = off / 4;
          if (k < 192) {
            cur->fetch.set(k, v);
          } else if (k < 192 + 1024) {
            cur->vs_consts.set(k - 192, v);
          } else if (k < 192 + 2048) {
            cur->ps_consts.set(k - 192 - 1024, v);
          } else if (k < 192 + 2048 + 40) {
            cur->bool_loop.set(k - 192 - 2048, v);
          }
        } else {
          const uint16_t reg = kShadowRegs[(off - kStateBytes) / 4];
          if (reg) cur->regs.set(reg - 0x2000, v);
        }
      }
    }
    at = end;
    if (changed) current = std::make_shared<GpuState>(*cur);
    f.frame.draws[i].state = current;
  }
}

// Replaces a frame the next stage hasn't started, unless that one is to be
// saved (F10).
void HandOver(std::unique_ptr<RecordedFrame> f, std::mutex& mutex, std::condition_variable& cv,
              std::unique_ptr<RecordedFrame>& slot) {
  {
    std::lock_guard lock(mutex);
    if (!slot || slot->dump_name.empty()) slot = std::move(f);
  }
  cv.notify_one();
}

void PrepMain() {
  qos::PreferPerformanceThread(L"amped3 native renderer prep", true);
  while (true) {
    std::unique_ptr<RecordedFrame> f;
    {
      std::unique_lock lock(g_prep_mutex);
      g_prep_cv.wait(lock, [] { return g_stop.load() || g_prep != nullptr; });
      if (g_stop) break;
      f = std::move(g_prep);
    }
    const auto t0 = std::chrono::steady_clock::now();
    BuildStates(*f);
    f->states_built = true;
    f->states_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    HandOver(std::move(f), g_work_mutex, g_work_cv, g_work);
  }
}

std::vector<uint32_t> ReadSpirv(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.size() < 20 || bytes.size() % 4) return {};
  std::vector<uint32_t> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  return words;
}

// glslang with a disk cache (logs\native_shaders\<hash of the GLSL>.spv), so
// shaders compile once per translation.
bool CompileCached(bool fragment, const std::string& name, const std::string& glsl,
                   std::vector<uint32_t>* spirv, std::string* error) {
  const std::filesystem::path dir = g_dir / "native_shaders";
  const auto path = dir / fmt::format("{:016X}.spv", Fnv1a(glsl.data(), glsl.size()));
  *spirv = ReadSpirv(path);
  if (!spirv->empty()) return true;
  if (!native::CompileGlsl(fragment, glsl, spirv, error)) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / (name + (fragment ? ".frag" : ".vert")), std::ios::binary) << glsl;
    std::ofstream(dir / (name + ".log"), std::ios::binary) << *error;
    REXLOG_WARN("Native renderer: {} failed to compile; GLSL and log in {}", name, dir.string());
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(spirv->data()), std::streamsize(spirv->size() * 4));
  return true;
}

// Writes a recorded frame in the F10 capture format, so tools/replay can draw
// exactly what the preview drew: logs\<name>.txt plus logs\<name>\.
void DumpFrame(RecordedFrame& f, LiveResources& resources) {
  std::string error;
  const auto txt = g_dir / (f.dump_name + ".txt");
  if (native::WriteFrameCapture(f.frame, resources, txt, g_dir / f.dump_name, &error)) {
    REXLOG_INFO("Native renderer: recorded frame written to {}", txt.string());
  } else {
    REXLOG_WARN("Native renderer: could not save the recorded frame: {}", error);
  }
}

// The front buffer's format, from the resolve that filled it (0: unknown).
uint32_t FrontBufferFormat(const Frame& frame) {
  for (const auto& r : frame.resolves) {
    if (r.has_dest && r.dest_object == frame.swap_front_buffer) return r.dest_raw[1] & 63;
  }
  return frame.has_swap_fetch ? frame.swap_fetch[1] & 63 : 0;
}

// The guest's gamma ramp as a lookup table (red, green, blue x 256), as the
// emulated swap applies it (SDK vulkan/command_processor.cpp, the
// apply-gamma shaders): the 256-entry table for 8-bit front buffers, the PWL
// ramp for 2_10_10_10 (indexed with 8-bit precision). false: no ramp yet.
bool GammaLut(uint32_t front_format, uint8_t lut[768]) {
  static GammaRamp ramp;
  if (!g_gamma || !g_gamma(&ramp)) return false;
  const bool pwl = front_format == 7 || front_format == 50;  // k_2_10_10_10(_AS_16_16_16_16)
  bool any = false;
  for (uint32_t c = 0; c < 3; ++c) {
    for (uint32_t v = 0; v < 256; ++v) {
      float out;
      if (pwl) {
        const uint32_t v10 = (v * 1023 + 127) / 255;
        const uint32_t e = ramp.pwl[v10 >> 3][c];
        out = (float(e & 0xFFFF) + float((v10 & 7) * (e >> 16)) * 0.125f) * 1.52737048e-05f;
      } else {
        const uint32_t e = ramp.table[v] >> (20 - 10 * c);  // red, green, blue
        out = float(e & 1023) / 1023.0f;
      }
      out = std::clamp(out, 0.0f, 1.0f);
      lut[c * 256 + v] = uint8_t(out * 255.0f + 0.5f);
      any = any || lut[c * 256 + v] != 0;
    }
  }
  if (!any) return false;  // all zero: the ramp isn't set up yet
  // Diagnostics: log each new ramp (a few points per channel; whether it is
  // monotonic), at most every 2 s and 200 times a run.
  static uint8_t last[768] = {};
  static auto last_log = std::chrono::steady_clock::time_point{};
  static int logged = 0;
  const auto now = std::chrono::steady_clock::now();
  if (std::memcmp(last, lut, 768) != 0 && logged < 200 && now - last_log > std::chrono::seconds(2)) {
    std::memcpy(last, lut, 768);
    last_log = now;
    ++logged;
    std::string text;
    for (uint32_t c = 0; c < 3; ++c) {
      bool up = true, down = true;
      for (uint32_t v = 1; v < 256; ++v) {
        up = up && lut[c * 256 + v] >= lut[c * 256 + v - 1];
        down = down && lut[c * 256 + v] <= lut[c * 256 + v - 1];
      }
      text += fmt::format(" {}:{},{},{},{},{}{}", "RGB"[c], lut[c * 256], lut[c * 256 + 64],
                          lut[c * 256 + 128], lut[c * 256 + 192], lut[c * 256 + 255],
                          up ? "" : down ? " (decreasing)" : " (NOT MONOTONIC)");
    }
    REXLOG_INFO("Native renderer: gamma ramp ({}){}", pwl ? "PWL" : "table", text);
  }
  return true;
}

// Per-frame timings, averaged and logged every few seconds.
struct Timings {
  double record = 0;  // game thread: the recording hooks
  double states = 0, prepass = 0, draw = 0, gpu = 0, output = 0, present = 0, total = 0;
  uint32_t frames = 0, recorded = 0;
  // Hitches: worker frames over kSlowFrameMs, and the worst one's cause.
  uint32_t slow = 0;
  double worst = 0;
  std::string worst_detail;
  // Sampled frames: the worker's draw recording by part (RenderStats::draw_part_ms).
  double parts[9] = {};
  uint32_t part_frames = 0;
};
constexpr double kSlowFrameMs = 20.0;
std::mutex g_timing_mutex;
Timings g_timings;
std::atomic<uint64_t> g_record_ns{0};  // this frame so far (game thread)
// Game-thread draw recording by part (ns this frame): textures, vertex data,
// indices / user data, state.
std::array<uint64_t, 5> g_draw_ns{};
std::array<double, 5> g_draw_ms_total{};

std::chrono::steady_clock::time_point g_timing_start{};

// Times one recording hook (game thread).
struct RecordTimer {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  ~RecordTimer() {
    g_record_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count());
  }
};

void LogTimings(bool shared) {
  std::lock_guard lock(g_timing_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (g_timing_start == std::chrono::steady_clock::time_point{}) g_timing_start = now;
  const double secs = std::chrono::duration<double>(now - g_timing_start).count();
  if (secs < 5.0 || !g_timings.frames) return;
  const Timings& t = g_timings;
  const double n = t.frames;
  REXLOG_INFO(
      "Native renderer: {:.1f} frames/s shown ({} recorded); per frame: game-thread recording "
      "{:.2f} ms (draws: textures {:.2f}, vertex data {:.2f}, indices {:.2f}, state {:.2f}, store {:.2f}); worker {:.2f} ms = states {:.2f} + vertex data {:.2f} + draws {:.2f}, then "
      "waiting for the GPU {:.2f} + present {:.2f} ({})",
      n / secs, t.recorded, t.recorded ? t.record / t.recorded : 0.0,
      t.recorded ? g_draw_ms_total[0] / t.recorded : 0.0,
      t.recorded ? g_draw_ms_total[1] / t.recorded : 0.0,
      t.recorded ? g_draw_ms_total[2] / t.recorded : 0.0,
      t.recorded ? g_draw_ms_total[3] / t.recorded : 0.0,
      t.recorded ? g_draw_ms_total[4] / t.recorded : 0.0, t.total / n, t.states / n,
      t.prepass / n, t.draw / n, t.gpu / n, t.present / n,
      shared ? "shared image" : "through the CPU");
  const uint64_t hit = g_vcache_stats.hit_bytes.exchange(0);
  const uint64_t copied = g_vcache_stats.copy_bytes.exchange(0);
  if (hit + copied) {
    REXLOG_INFO("Native renderer: vertex data {:.1f} MB/frame, {:.0f}% reused from the cache",
                double(hit + copied) / std::max<uint32_t>(t.recorded, 1) / (1 << 20),
                100.0 * double(hit) / double(hit + copied));
  }
  if (t.slow) {
    REXLOG_INFO("Native renderer: {} frame(s) over {:.0f} ms; the slowest {:.1f} ms: {}", t.slow,
                kSlowFrameMs, t.worst, t.worst_detail);
  }
  if (t.part_frames) {
    const double k = 1.0 / t.part_frames;
    REXLOG_INFO(
        "Native renderer: draws by part (ms per frame, {} sampled): setup {:.2f}, targets {:.2f}, "
        "textures {:.2f}, pipeline {:.2f}, uniforms {:.2f}, texture sets {:.2f}, indices {:.2f}, "
        "Vulkan commands {:.2f}, after {:.2f}",
        t.part_frames, t.parts[0] * k, t.parts[1] * k, t.parts[2] * k, t.parts[3] * k,
        t.parts[4] * k, t.parts[5] * k, t.parts[6] * k, t.parts[7] * k, t.parts[8] * k);
  }
  g_timings = {};
  g_draw_ms_total = {};
  g_timing_start = now;
}

std::function<std::pair<uint32_t, uint32_t>()> g_window_size;
// The game's front buffer size (guest pixels), from the last frame drawn.
std::atomic<uint32_t> g_guest_w{1280}, g_guest_h{720};

// An integer setting by name (default if unset or not a number).
int IntSetting(const char* name, int fallback) {
  const std::string v = rex::cvar::GetFlagByName(name);
  if (v.empty()) return fallback;
  try {
    return std::stoi(v);
  } catch (...) {
    return fallback;
  }
}

}  // namespace

void SetWindowSize(std::function<std::pair<uint32_t, uint32_t>()> size) {
  g_window_size = std::move(size);
}

float RenderScaleFor(int percent, uint32_t* width, uint32_t* height) {
  const uint32_t gw = std::max(g_guest_w.load(), 1u), gh = std::max(g_guest_h.load(), 1u);
  auto [ww, wh] = g_window_size ? g_window_size() : std::pair<uint32_t, uint32_t>{0, 0};
  // Display > Output resolution: the picture is made at that size (then
  // scaled to the window or screen); Native (no preset): the window's size.
  {
    int32_t pw = 0, ph = 0;
    const std::string preset = rex::cvar::GetFlagByName("resolution");
    if (!preset.empty() &&
        rex::graphics::video_mode_util::TryParseResolutionPreset(preset, pw, ph) && pw > 0 &&
        ph > 0) {
      ww = uint32_t(pw);
      wh = uint32_t(ph);
    }
  }
  if (!ww || !wh) {
    ww = gw;
    wh = gh;
  }
  // 100%: the picture's pixels on screen. Letterboxed, the 16:9 part of the
  // window; stretched, enough to cover the window in both directions.
  const float sx = float(ww) / float(gw), sy = float(wh) / float(gh);
  const bool letterbox = rex::cvar::GetFlagByName("present_letterbox") != "false";
  float s = (letterbox ? std::min(sx, sy) : std::max(sx, sy)) * float(std::clamp(percent, 25, 100)) /
            100.0f;
  // As the renderer snaps it: whole pixels for 16:9 sizes in multiples of 80.
  s = std::clamp(std::round(s * 80.0f) / 80.0f, 0.25f, 4.0f);
  if (width) *width = uint32_t(std::lround(double(gw) * s));
  if (height) *height = uint32_t(std::lround(double(gh) * s));
  return s;
}

namespace {

void WorkerMain() {
  // The frame's critical path: keep it off throttled efficiency cores.
  qos::PreferPerformanceThread(L"amped3 native renderer", true);
  // At start-up the window shows black until the native renderer's first
  // picture (its set-up takes about half a second), instead of the emulated
  // GPU's frames meanwhile: the start-up screen then appears once, as the
  // native renderer draws it (with the texture pack's logo), rather than the
  // emulated one being replaced a moment later.
  static bool started_black = false;
  if (!started_black && !g_failed) {
    started_black = true;
    if (rex::ui::Presenter* presenter = g_presenter ? g_presenter() : nullptr) {
      if (PresenterHasExternalOutput()) {
        constexpr uint32_t kW = 1280, kH = 720;
        const std::vector<uint32_t> black(size_t(kW) * kH, 0xC0000000u);  // A2B10G10R10
        std::string error;
        SetGuestOutputExternal(presenter, true);
        if (!PresentPacked(presenter, g_backend, kW, kH,
                           reinterpret_cast<const uint8_t*>(black.data()), &error)) {
          SetGuestOutputExternal(presenter, false);
        }
      }
    }
  }
  using Clock = std::chrono::steady_clock;
  auto since = [](Clock::time_point a) {
    return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
  };
  std::unique_ptr<native::Renderer> renderer;
  bool renderer_msaa = true;
  std::chrono::steady_clock::time_point last_swap_time;  // of the last frame drawn
  float renderer_scale = 1.0f;
  // A new scale (render scale setting, window size) waits until it has held
  // for a moment: resizing the window doesn't rebuild the targets each frame.
  float scale_candidate = 0.0f, out_scale_candidate = 0.0f;
  Clock::time_point scale_since{};
  float renderer_out_scale = 0.0f;  // the upscaler's output (0: none)
  int renderer_upscaler = 0;
  bool renderer_async = false;
  bool renderer_smooth = true;
  std::string upscaler_reported;
  int renderer_aniso = -1;
  bool renderer_fonts = false, renderer_pack = false, renderer_upscaled = false;
  std::set<std::string> reported;
  std::vector<uint8_t> packed;
  bool presenting = false;
  bool shared = false;       // presenting through images shared with the presenter's device
  uint32_t shared_slot = 0;  // next shared slot to draw into
  // The renderer runs async: frame N is submitted, then frame N-1 (finished
  // on the GPU while N was being recorded) is shown. A frame left waiting is
  // shown once no new one arrives for a few milliseconds.
  struct Pending {
    bool valid = false;
    uint64_t ticket = 0;
    native::OutputInfo info;
  } pending;

  double last_gpu_wait = 0;  // the last WaitFrame (ms)
  auto present_pending = [&]() {
    if (!pending.valid || !renderer) return;
    pending.valid = false;
    const double gpu_ms = renderer->WaitFrame(pending.ticket);
    last_gpu_wait = gpu_ms;
    perf::ReportGpuFrame(renderer->GpuFrameMs(pending.ticket));
    const auto t_present = Clock::now();
    rex::ui::Presenter* presenter = g_presenter ? g_presenter() : nullptr;
    if (!presenter || !REXCVAR_GET(native_renderer) || g_stop) return;
    std::string error;
    bool ok;
    if (!IsGuestOutputExternal(presenter)) SetGuestOutputExternal(presenter, true);
    if (shared) {
      ok = PresentShared(presenter, pending.info, &error);
      if (!ok) {
        REXLOG_WARN("Native renderer: shared image failed ({}); presenting through the CPU from "
                    "the next frame",
                    error);
        shared = false;
        return;
      }
    } else {
      ok = renderer->ReadOutput(pending.ticket, &packed) &&
           PresentPacked(presenter, g_backend, pending.info.width, pending.info.height,
                         packed.data(), &error);
    }
    if (ok) {
      perf::ReportPresented();
      if (!presenting) {
        presenting = true;
        SetMessage("");
        REXLOG_INFO("Native renderer: showing its own image ({}x{})", pending.info.width,
                    pending.info.height);
      }
    } else {
      SetMessage("present failed: " + error);
      SetGuestOutputExternal(presenter, false);
      presenting = false;
    }
    {
      std::lock_guard lock(g_timing_mutex);
      g_timings.gpu += gpu_ms;
      g_timings.present += since(t_present);
    }
    std::lock_guard lock(g_status_mutex);
    g_status.presenting = presenting;
  };

  while (true) {
    std::unique_ptr<RecordedFrame> f;
    // Hands the frame's blob buffers back for reuse, however this iteration ends.
    struct Recycle {
      std::unique_ptr<RecordedFrame>& f;
      ~Recycle() {
        if (f) RecycleBlobs(*f);
      }
    } recycle{f};
    {
      std::unique_lock lock(g_work_mutex);
      auto ready = [] { return g_stop.load() || g_work != nullptr; };
      if (pending.valid && !g_work_cv.wait_for(lock, std::chrono::milliseconds(3), ready)) {
        lock.unlock();
        try {
          present_pending();
        } catch (const std::exception& e) {
          REXLOG_ERROR("Native renderer stopped: {}", e.what());
          g_failed = true;
          renderer.reset();
        }
        continue;
      }
      g_work_cv.wait(lock, ready);
      if (g_stop) break;
      f = std::move(g_work);
    }
    if (!REXCVAR_GET(native_renderer)) {
      pending.valid = false;
      continue;
    }
    const auto t0 = Clock::now();
    last_gpu_wait = 0;
    try {
      // The MSAA setting decides how render targets are made: start over.
      const bool msaa = REXCVAR_GET(native_msaa);
      // The SDK's settings (Display > Image quality), read by name: they live
      // in the GPU plugin.
      float scale = RenderScaleFor(REXCVAR_GET(native_render_scale));
      // The render scale is the 3D scene's; the HUD, menus and text are
      // drawn at the picture's full size on screen (100%), where an upscaler
      // also rebuilds the scene.
      const std::string upscaler_name = REXCVAR_GET(native_upscaler);
      // native::UpscalerKind: 1 = XeSS, 2 = FSR.
      const int upscaler = upscaler_name == "xess" ? 1 : upscaler_name == "fsr" ? 2 : 0;
      float out_scale = RenderScaleFor(100);
      if (renderer && (scale != renderer_scale || out_scale != renderer_out_scale)) {
        if (scale != scale_candidate || out_scale != out_scale_candidate) {
          scale_candidate = scale;
          out_scale_candidate = out_scale;
          scale_since = Clock::now();
        }
        if (Clock::now() - scale_since < std::chrono::milliseconds(400)) {
          scale = renderer_scale;
          out_scale = renderer_out_scale;
        }
      }
      const int aniso = std::clamp(IntSetting("anisotropic_override", -1), -1, 5);
      // Display > Background shader compilation: also the native renderer's
      // background pipeline builds.
      const bool async_pipelines =
          rex::cvar::GetFlagByName("async_shader_compilation") != "false";
      const bool smooth_foliage = REXCVAR_GET(native_smooth_foliage);
      const bool sharp_fonts = REXCVAR_GET(native_sharp_fonts);
      const bool texture_pack = REXCVAR_GET(native_texture_pack);
      const bool upscaled_textures = REXCVAR_GET(native_upscaled_textures);
      if (renderer && (msaa != renderer_msaa || scale != renderer_scale ||
                       out_scale != renderer_out_scale || upscaler != renderer_upscaler ||
                       aniso != renderer_aniso || async_pipelines != renderer_async ||
                       smooth_foliage != renderer_smooth || sharp_fonts != renderer_fonts ||
                       texture_pack != renderer_pack ||
                       upscaled_textures != renderer_upscaled)) {
        pending.valid = false;
        renderer.reset();
      }
      if (!renderer) {
        pending.valid = false;
        if (!PresenterHasExternalOutput()) {
          throw std::runtime_error(
              "the SDK is missing patch 0008 (external guest output); run setup.ps1 without "
              "-SkipSdk to rebuild it");
        }
        if (!native::GlslCompilerAvailable()) {
          throw std::runtime_error(
              "this build has no GLSL compiler; rebuild with setup.ps1 (it builds in the SDK's "
              "glslang)");
        }
        native::RendererOptions options;
        options.compile = CompileCached;
        options.msaa = msaa;
        options.resolution_scale = scale;
        // Pipelines built in earlier sessions (and by earlier renderers this
        // session: settings changes) come from here instead of the driver's
        // compiler.
        options.pipeline_cache_path = (g_dir / "native_pipeline_cache.bin").string();
        options.upscaler = upscaler;
        options.output_scale = out_scale;
        options.async_pipelines = async_pipelines;
        renderer_async = async_pipelines;
        options.smooth_alpha = smooth_foliage;
        renderer_smooth = smooth_foliage;
        options.anisotropy_override = aniso;
        options.hd_fonts = sharp_fonts;
        options.texture_pack = texture_pack;
        options.texture_pack_folder = (g_dir.parent_path() / "textures").string();
        renderer_fonts = sharp_fonts;
        renderer_pack = texture_pack;
        options.upscaled_textures = upscaled_textures;
        renderer_upscaled = upscaled_textures;
        renderer_scale = scale;
        renderer_out_scale = out_scale;
        renderer_upscaler = upscaler;
        upscaler_reported.clear();
        renderer_aniso = aniso;
        options.async = true;
        // Draw on the presenter's GPU, so the picture can stay on it.
        rex::ui::Presenter* presenter = g_presenter ? g_presenter() : nullptr;
        options.match_device_uuid = presenter && PresenterDeviceUuid(presenter, options.device_uuid);
        renderer_msaa = msaa;
        renderer = std::make_unique<native::Renderer>(options);
        shared = renderer->CanShareOutput() && presenter && CanPresentShared(presenter);
        {
          uint32_t sw = 0, sh = 0, ow = 0, oh = 0;
          RenderScaleFor(REXCVAR_GET(native_render_scale), &sw, &sh);
          RenderScaleFor(100, &ow, &oh);
          REXLOG_INFO("Native renderer: 3D scene at {}x{}, picture (HUD, menus) at {}x{}", sw, sh,
                      ow, oh);
        }
        perf::SetGpuName(renderer->device_name());
        REXLOG_INFO("Native renderer: drawing on {}; picture to the window {}",
                    renderer->device_name(),
                    shared ? "through a shared image (stays on the GPU)"
                           : "through the CPU (shared images need the Vulkan GPU backend and "
                             "SDK patch 0010)");
      }
      double states_ms = since(t0);  // settings and renderer checks
      if (f->states_built) {
        states_ms += f->states_ms;  // on the prep thread, overlapping the previous frame
      } else {
        BuildStates(*f);
        states_ms = since(t0);
      }
      LiveResources resources(*f);
      if (!f->dump_name.empty()) DumpFrame(*f, resources);
      // The output (front buffer through the gamma ramp) is drawn in the
      // frame's own submission.
      uint8_t lut[768];
      const bool has_lut = GammaLut(FrontBufferFormat(f->frame), lut);
      native::Renderer::OutputRequest request;
      request.lut = has_lut ? lut : nullptr;
      request.share = shared;
      if (shared) {
        request.slot = shared_slot;
        shared_slot = (shared_slot + 1) % native::Renderer::kOutputSlots;
        WaitSharedSlot(request.slot);  // the presenter has finished copying from it
      }
      native::OutputInfo info;
      // Motion blur: the motion since the last frame drawn, scaled to a
      // fixed exposure (1/60 s x strength) so it looks the same at any rate.
      {
        float blur = 0.0f;
        const double dt = std::chrono::duration<double>(f->swap_time - last_swap_time).count();
        // Revamped Graphics (the game's Options menu) off: none of the
        // enhancements, each keeps its own setting for when it's back on.
        const bool revamped = REXCVAR_GET(revamped_graphics);
        const bool camera = revamped && REXCVAR_GET(native_camera_motion_blur);
        const bool objects = revamped && REXCVAR_GET(native_object_motion_blur);
        if ((camera || objects) && dt > 0.001 && dt < 0.25) {
          blur = float(std::min(REXCVAR_GET(native_camera_motion_blur_strength) / 60.0 / dt, 4.0));
        }
        renderer->SetMotionBlur(blur, camera, objects, REXCVAR_GET(native_motion_blur_debug));
        renderer->SetUpscaleParams(dt > 0.0 && dt < 0.25 ? float(dt * 1000.0) : 16.7f,
                                   float(REXCVAR_GET(native_upscaler_sharpness)));
        renderer->SetFog(revamped && REXCVAR_GET(native_fog) ? float(REXCVAR_GET(native_fog_density)) : 0.0f,
                         float(REXCVAR_GET(native_fog_start)));
        renderer->SetFoliageFade(revamped && REXCVAR_GET(native_foliage_fade));
        renderer->SetMapClouds(revamped && REXCVAR_GET(native_map_clouds));
        renderer->SetHideInterface(g_hide_interface.load());
        // The game state machine (0x828901E0) + 8: 7 = the trail map.
        if (auto* memory = GuestMemory()) {
          const uint32_t game_state = U32(memory->virtual_membase() + 0x828901E8u);
          renderer->SetTrailMapOpen(game_state == 7);
          renderer->SetInCutscene(game_state == 20);  // a story scene
          renderer->SetGameState(int(game_state));
        }
        renderer->SetGodRays(revamped && REXCVAR_GET(native_god_rays)
                                 ? float(REXCVAR_GET(native_god_rays_strength))
                                 : 0.0f);
        renderer->SetSpeedBlur(
            revamped && REXCVAR_GET(native_speed_blur) ? float(REXCVAR_GET(native_speed_blur_strength)) : 0.0f,
            dt > 0.0 && dt < 0.25 ? float(dt) : 1.0f / 60.0f);
        last_swap_time = f->swap_time;
      }
      const auto stats = renderer->Render(f->frame, resources, UINT32_MAX, &request, &info);
      if (renderer_upscaler && !stats.upscaler.empty() && stats.upscaler != upscaler_reported) {
        upscaler_reported = stats.upscaler;
        REXLOG_INFO("Native renderer: upscaler: {}", stats.upscaler);
      }
      for (const auto& line : stats.notices) REXLOG_INFO("Native renderer: {}", line);
      if (stats.guest_output_width && stats.guest_output_height) {
        g_guest_w = stats.guest_output_width;
        g_guest_h = stats.guest_output_height;
      }
      for (const auto& [why, n] : stats.skipped) {
        if (reported.insert(why).second) {
          REXLOG_WARN("Native renderer: {} ({} in the first frame showing it)", why, n);
        }
      }
      // Show the previous frame (its GPU work ran while this one was recorded).
      present_pending();
      if (stats.output_drawn) {
        pending.valid = true;
        pending.ticket = stats.ticket;
        pending.info = info;
      } else {
        // No front-buffer picture (what a resolve put there, this frame or an
        // earlier one, or what the CPU wrote): the emulated image is shown
        // while the emulated GPU still draws; otherwise the previous native
        // image stays up.
        if (g_host_drawing_on.load()) {
          if (rex::ui::Presenter* presenter = g_presenter ? g_presenter() : nullptr) {
            if (IsGuestOutputExternal(presenter)) SetGuestOutputExternal(presenter, false);
          }
          presenting = false;
          SetMessage("this frame has no front-buffer picture: showing the emulated image");
        } else {
          SetMessage("this frame has no front-buffer picture: kept the previous one");
        }
      }
      {
        std::lock_guard lock(g_timing_mutex);
        g_timings.states += states_ms;
        g_timings.prepass += stats.prepass_ms;
        g_timings.draw += stats.record_ms;
        if (stats.draw_parts_sampled) {
          for (int k = 0; k < 9; ++k) g_timings.parts[k] += stats.draw_part_ms[k];
          ++g_timings.part_frames;
        }
        perf::ReportRenderFrame(states_ms + stats.prepass_ms + stats.record_ms);
        const double frame_ms = since(t0);
        g_timings.total += frame_ms;
        ++g_timings.frames;
        if (frame_ms > kSlowFrameMs) {
          ++g_timings.slow;
          if (frame_ms > g_timings.worst) {
            g_timings.worst = frame_ms;
            g_timings.worst_detail = fmt::format(
                "states {:.1f} + vertex data {:.1f} + draws {:.1f} ms, waiting for the GPU "
                "{:.1f} ms; first use of {} textures ({:.1f} MB, {:.1f} ms), {} pipelines "
                "({:.1f} ms; {} more to the background), {} shaders ({:.1f} ms); {} textures cached "
                "({:.0f} MB), {} freed; vertex arena {:.0f} MB{}",
                states_ms, stats.prepass_ms, stats.record_ms, last_gpu_wait, stats.new_textures,
                stats.texture_mb, stats.texture_ms, stats.new_pipelines, stats.pipeline_ms,
                stats.deferred_pipelines, stats.new_shaders, stats.shader_ms, stats.textures_cached,
                stats.texture_cache_mb, stats.textures_evicted, stats.arena_mb,
                stats.arena_resets ? " (started over: it was full)" : "");
          }
        }
      }
      LogTimings(shared);
      const double ms = since(t0);
      std::lock_guard lock(g_status_mutex);
      ++g_status.frames;
      g_status.render_ms = ms;
      g_status.presenting = presenting;
    } catch (const std::exception& e) {
      REXLOG_ERROR("Native renderer stopped: {}", e.what());
      SetMessage(std::string("stopped: ") + e.what());
      g_failed = true;
      if (rex::ui::Presenter* presenter = g_presenter ? g_presenter() : nullptr) {
        SetGuestOutputExternal(presenter, false);
      }
      pending.valid = false;
      renderer.reset();
      presenting = false;
      std::lock_guard lock(g_status_mutex);
      g_status.presenting = false;
    }
  }
  pending.valid = false;
  renderer.reset();
}

// Asks the emulated GPU to draw or not (SDK patch 0009). Without the patch
// it always draws.
void SetHostDrawing(bool on) {
  if (g_host_drawing_on.load() == on) return;
  if (!g_host_drawing || !g_host_drawing(on)) {
    g_host_drawing_on = true;
    return;
  }
  g_host_drawing_on = on;
  REXLOG_INFO("Native renderer: emulated GPU drawing {}", on ? "on" : "off");
}

void EnsureWorker() {
  if (g_worker.joinable()) return;
  g_stop = false;
  g_worker = std::thread(WorkerMain);
  g_prep_thread = std::thread(PrepMain);
}

}  // namespace

void Install(const std::filesystem::path& log_dir, std::function<rex::ui::Presenter*()> presenter,
             std::string backend, std::function<bool(GammaRamp*)> gamma,
             std::function<bool(bool)> host_drawing) {
  g_dir = log_dir;
  g_host_drawing = std::move(host_drawing);
  g_presenter = std::move(presenter);
  g_backend = std::move(backend);
  g_gamma = std::move(gamma);
}

void Shutdown() {
  g_recording = false;
  {
    std::lock_guard lock(g_work_mutex);
    g_stop = true;
  }
  {
    std::lock_guard lock(g_prep_mutex);
  }
  g_prep_cv.notify_all();
  g_work_cv.notify_all();
  if (g_prep_thread.joinable()) g_prep_thread.join();
  if (g_worker.joinable()) g_worker.join();
  SetHostDrawing(true);
  if (rex::ui::Presenter* presenter = g_presenter ? g_presenter() : nullptr) {
    SetGuestOutputExternal(presenter, false);
    ReleasePresentResources();
  }
}

bool Enabled() { return REXCVAR_GET(native_renderer) && !g_failed.load(); }

void SetHideInterface(bool hide) { g_hide_interface = hide; }

bool Recording() { return g_recording.load(std::memory_order_relaxed); }

bool SkipEmulatedStateWrites() {
  return REXCVAR_GET(native_skip_state_writes) && !g_host_drawing_on.load(std::memory_order_relaxed);
}

void RequestDump() {
  if (REXCVAR_GET(native_renderer)) g_dump_requested = true;
}

void NoteMicrocode(const std::string& stem, const uint8_t* data, size_t size) {
  std::lock_guard lock(g_ucode_mutex);
  auto& v = g_ucode[stem];
  if (v.empty()) v.assign(data, data + size);
}

// A fetch constant holding a texture (type 2) with a base address: set by the
// game directly rather than through a Direct3D texture object.
bool RawTextureFetch(const std::array<uint32_t, 6>& words) {
  return (words[0] & 3) == 2 && (words[1] >> 12) != 0;
}

std::array<SlotName, kTextureSlots> g_slot_names;
std::array<VertexFetchName, 96> g_vf_names;

void OnDraw(uint8_t* base, uint32_t dev, const DrawCall& call, const std::string& vs_ucode,
            const std::string& ps_ucode) {
  if (!Recording()) return;
  RecordTimer timer;
  std::lock_guard lock(g_mutex);
  if (!g_frame || !Room()) return;
  auto lap = [t = std::chrono::steady_clock::now()](int part) mutable {
    const auto now = std::chrono::steady_clock::now();
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count();
    g_draw_ns[part] += uint64_t(ns);
    // F11 trace: textures, vertex memory, indices, state (part 4, storing, is small).
    static constexpr trace::Zone kZones[] = {trace::Zone::kRecTextures,
                                             trace::Zone::kRecVertices,
                                             trace::Zone::kRecIndices, trace::Zone::kRecState};
    if (part < 4) trace::Add(kZones[part], ns);
    t = now;
  };
  Draw d;
  d.kind = Draw::Kind(int(call.kind));
  d.prim = call.prim;
  d.start = call.start;
  d.count = call.count;
  d.base_vertex = call.base_vertex;
  d.vs = vs_ucode;
  d.ps = ps_ucode;
  const uint32_t number = AddEvent(native::EventType::kDraw, uint32_t(g_frame->frame.draws.size()));
  auto* memory = GuestMemory();

  // Textures: the fetch constants the shaders read (all bound ones if a
  // shader couldn't be translated).
  const ShaderUse& vs_use = UseOf(vs_ucode, native::ShaderStage::kVertex);
  const ShaderUse& ps_use = UseOf(ps_ucode, native::ShaderStage::kPixel);
  uint32_t wanted = 0;
  if (vs_use.known && ps_use.known) {
    for (uint32_t t : vs_use.textures) wanted |= 1u << (t & 31);
    for (uint32_t t : ps_use.textures) wanted |= 1u << (t & 31);
  } else {
    wanted = ~0u;
  }
  for (uint32_t s = 0; s < kTextureSlots; ++s) {
    if (!(wanted & (1u << s))) continue;
    const auto words = FetchWords(base, dev + kFetchConsts + 24 * s);
    // A texture bound through Direct3D, or one the game wrote straight into
    // the fetch constant with no texture object in the slot (the animated
    // page in story scene 2.1.1 does this): the GPU reads the fetch constant
    // either way.
    if (!U32(base, dev + kTextures + 4 * s) && !RawTextureFetch(words)) continue;
    native::FrameTexture t;
    t.slot = s;
    std::copy(words.begin(), words.end(), t.raw.begin());
    SlotName& last = g_slot_names[s];
    if (last.frame != g_record_frame || last.words != words) {
      last.words = words;
      last.name = TextureName(words);
      last.frame = g_record_frame;
    }
    t.data = last.name;
    d.textures.push_back(std::move(t));
  }

  lap(0);  // textures (and the draw's setup)

  // Vertex memory the vertex shader fetches.
  if (memory) {
    for (uint32_t vf : VertexFetchSlots(vs_ucode)) {
      const uint32_t w0 = U32(base, dev + kFetchConsts + vf * 8);
      const uint32_t w1 = U32(base, dev + kFetchConsts + vf * 8 + 4);
      if ((w0 & 3) != 3) continue;
      VertexFetchName& last = g_vf_names[vf % 96];
      if (last.frame == g_record_frame && last.w0 == w0 && last.w1 == w1) {
        d.vmem[vf] = last.name;
        continue;
      }
      const uint32_t address = (w0 & ~3u) & 0x1FFFFFFF;
      const uint32_t size = ((w1 >> 2) & 0xFFFFFF) * 4;
      if (!address || !size || size > (64u << 20)) continue;
      const uint64_t key = (uint64_t(address) << 32) | size;
      auto it = g_blob_names.find(key);
      if (it == g_blob_names.end()) {
        std::string name = native::ShortName('M', (uint64_t(size) << 29) | address, 10);
        auto& blob = g_frame->blobs[name];
        if (!blob) blob = CachedVertexData(memory, name, address, size, &g_frame->versions[name]);
        it = g_blob_names.emplace(key, std::move(name)).first;
      }
      d.vmem[vf] = it->second;
      last.w0 = w0;
      last.w1 = w1;
      last.name = it->second;
      last.frame = g_record_frame;
    }
  }

  lap(1);

  // Indices and user-pointer data.
  if (call.kind == DrawCall::Kind::kIndexed && memory) {
    const uint32_t ib = U32(base, dev + kIndexBuffer);
    if (ib) {
      const uint32_t address = U32(base, ib + 12), size = U32(base, ib + 16);
      d.index_32bit = (U32(base, ib) & 0x80000000u) != 0;
      if (address && size && size <= (64u << 20)) {
        const uint64_t key = (uint64_t(address) << 32) | size | (1ull << 31);  // not physical
        auto it = g_blob_names.find(key);
        if (it == g_blob_names.end()) {
          std::string name = native::ShortName('I', (uint64_t(size) << 32) | address, 10);
          // Index buffers are mostly static too: through the vertex cache
          // when they are in physical memory (as Direct3D allocates them).
          const uint32_t physical = memory->GetPhysicalAddress(address);
          if (physical != UINT32_MAX && memory->GetPhysicalAddress(address + size - 1) ==
                                            physical + size - 1) {
            auto& blob = g_frame->blobs[name];
            if (!blob) {
              blob = CachedVertexData(memory, name, physical, size, &g_frame->versions[name]);
            }
          } else {
            CopyGuest(name, memory->TranslateVirtual<const uint8_t*>(address), size);
          }
          it = g_blob_names.emplace(key, std::move(name)).first;
        }
        d.index_data = it->second;
      }
    }
  }
  if (memory && call.up_indices && call.up_indices_size) {
    d.index_data = fmt::format("upi_{}", number);
    d.index_32bit = call.up_index_32bit;
    CopyGuest(d.index_data, memory->TranslateVirtual<const uint8_t*>(call.up_indices),
              call.up_indices_size);
  }
  if (call.up_vertices && call.up_vertices_size) {
    d.up_vertices = fmt::format("upv_{}", number);
    if (call.vertices_follow) {
      g_pending = {call.up_vertices, call.up_vertices_size, d.up_vertices};
    } else if (memory) {
      CopyGuest(d.up_vertices, memory->TranslateVirtual<const uint8_t*>(call.up_vertices),
                call.up_vertices_size);
    }
  }

  lap(2);
  RecordStateDelta(base, dev, vs_use, ps_use);
  lap(3);
  g_frame->frame.draws.push_back(std::move(d));
  lap(4);
}

void OnClear(uint8_t* base, uint32_t dev, uint32_t flags, uint32_t color, double z,
             uint32_t stencil) {
  if (!Recording()) return;
  RecordTimer timer;
  std::lock_guard lock(g_mutex);
  if (!g_frame || !Room()) return;
  native::Clear c;
  c.flags = flags;
  if (color) {
    for (int i = 0; i < 4; ++i) c.color[i] = F32(base, color + 4 * i);
  }
  c.z = float(z);
  c.stencil = stencil;
  for (uint32_t i = 0; i < 6; ++i) c.targets[i] = U32(base, dev + kRegShadow + 4 * i);
  c.has_targets = true;
  AddEvent(native::EventType::kClear, uint32_t(g_frame->frame.clears.size()));
  g_frame->frame.clears.push_back(c);
}

void OnResolve(uint8_t* base, uint32_t dev, uint32_t flags, uint32_t src_rect, uint32_t dest_tex,
               uint32_t dest_point, uint32_t clear_color, double z) {
  if (!Recording()) return;
  RecordTimer timer;
  std::lock_guard lock(g_mutex);
  if (!g_frame || !Room()) return;
  native::Resolve r;
  r.flags = flags;
  if (src_rect) {
    r.full = false;
    for (int i = 0; i < 4; ++i) r.rect[i] = U32(base, src_rect + 4 * i);
  }
  r.dest_object = dest_tex;
  if (dest_tex) {
    const auto words = FetchWords(base, dest_tex + 16);
    std::copy(words.begin(), words.end(), r.dest_raw.begin());
    r.has_dest = true;
  }
  if (dest_point) {
    r.dest_point[0] = U32(base, dest_point);
    r.dest_point[1] = U32(base, dest_point + 4);
    r.has_dest_point = true;
  }
  if (clear_color) {
    for (int i = 0; i < 4; ++i) r.clear_rgba[i] = F32(base, clear_color + 4 * i);
  }
  r.clear_z = float(z);
  for (uint32_t i = 0; i < 6; ++i) r.targets[i] = U32(base, dev + kRegShadow + 4 * i);
  r.has_targets = true;
  AddEvent(native::EventType::kResolve, uint32_t(g_frame->frame.resolves.size()));
  g_frame->frame.resolves.push_back(r);
}

void OnBeginTiling(uint8_t* base, uint32_t count, uint32_t rects) {
  if (!Recording()) return;
  RecordTimer timer;
  std::lock_guard lock(g_mutex);
  if (!g_frame || !Room()) return;
  native::Tiling t;
  for (uint32_t i = 0; i < count && i < 8 && rects; ++i) {
    t.width = std::max(t.width, U32(base, rects + 16 * i + 8));
    t.height = std::max(t.height, U32(base, rects + 16 * i + 12));
  }
  AddEvent(native::EventType::kBeginTiling, uint32_t(g_frame->frame.tilings.size()));
  g_frame->frame.tilings.push_back(t);
}

void OnEndTiling() {
  if (!Recording()) return;
  std::lock_guard lock(g_mutex);
  if (!g_frame || !Room()) return;
  AddEvent(native::EventType::kEndTiling, 0);
}

void OnSwap(uint8_t* base, uint32_t front_buffer) {
  // Switching the setting off and on again retries after a failure.
  if (!REXCVAR_GET(native_renderer)) g_failed = false;
  const bool enabled = Enabled();
  std::lock_guard lock(g_mutex);
  if (g_recording && g_frame) {
    RecordTimer timer;
    AddEvent(native::EventType::kSwap, 0);
    auto& frame = g_frame->frame;
    frame.swap_front_buffer = front_buffer;
    if (front_buffer) {
      // The texture object: 16 bytes of header, then its fetch constant.
      std::vector<uint32_t> words(10);
      for (uint32_t i = 0; i < 10; ++i) words[i] = U32(base, front_buffer + 4 * i);
      std::copy(words.begin() + 4, words.end(), frame.swap_fetch.begin());
      frame.has_swap_fetch = true;
      frame.swap_data = TextureName(frame.swap_fetch);
      frame.objects[front_buffer] = std::move(words);
    }
    g_frame->swap_time = std::chrono::steady_clock::now();
    g_last_draws = frame.draws.size();
    g_last_events = frame.events.size();
    g_last_deltas = g_frame->deltas.size();
    g_recording = false;
    {
      std::lock_guard timing_lock(g_timing_mutex);
      g_timings.record += double(g_record_ns.exchange(0)) * 1e-6;
      ++g_timings.recorded;
      for (int k = 0; k < 5; ++k) {
        g_draw_ms_total[k] += double(g_draw_ns[k]) * 1e-6;
        g_draw_ns[k] = 0;
      }
    }
    if (enabled && g_frame_overflow && g_host_drawing_on.load()) {
      // Too long to record: show the emulated image rather than a stale one
      // (with the emulated GPU off, the previous native image stays up).
      if (rex::ui::Presenter* presenter = g_presenter ? g_presenter() : nullptr) {
        if (IsGuestOutputExternal(presenter)) SetGuestOutputExternal(presenter, false);
      }
    }
    if (enabled && !g_frame_overflow) {
      // To the prep thread (per-draw states), then the worker.
      HandOver(std::move(g_frame), g_prep_mutex, g_prep_cv, g_prep);
    }
    g_frame.reset();
  }
  bool presenting;
  {
    std::lock_guard status_lock(g_status_mutex);
    g_status.on = enabled;
    if (!enabled) g_status.presenting = false;
    presenting = g_status.presenting;
  }
  // Step 5: once the native image is up, the emulated GPU stops drawing (it
  // still runs the command stream the game waits on). It draws again if the
  // native renderer is switched off or fails.
  SetHostDrawing(!(enabled && presenting) || REXCVAR_GET(native_keep_emulation));
  if (enabled) {
    EnsureWorker();
    StartFrame();
  } else if (rex::ui::Presenter* presenter = g_presenter ? g_presenter() : nullptr) {
    if (IsGuestOutputExternal(presenter)) SetGuestOutputExternal(presenter, false);
  }
}

Status GetStatus() {
  std::lock_guard lock(g_status_mutex);
  Status s = g_status;
  s.on = REXCVAR_GET(native_renderer);
  s.emulation_drawing = g_host_drawing_on.load();
  return s;
}

}  // namespace amped3::live
