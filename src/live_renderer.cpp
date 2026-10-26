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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <deque>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/thread.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/gpu_plugin.h>
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
#include "guest_waits.h"
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
                    "follows its own setting. Sharp fonts and upscaled textures aren't part of "
                    "it: they're off until turned on themselves");
REXCVAR_DEFINE_BOOL(native_keep_emulation, false, "GPU",
                    "Native renderer: keep the emulated GPU drawing every frame underneath "
                    "(slower; for comparing the two)");
REXCVAR_DEFINE_BOOL(native_parallel_commands, true, "GPU",
                    "Native renderer: the frame's Vulkan commands are recorded on a thread of "
                    "their own while the renderer prepares the next draws (the driver's time "
                    "for each command moves off the renderer's thread).");
REXCVAR_DEFINE_BOOL(native_merge_clears, true, "GPU",
                    "Native renderer: a clear waits for the next pass that draws to its image "
                    "and becomes that pass's load op (or is done inside the pass already drawing "
                    "to it), instead of a render pass of its own. Fewer passes, and tile-based "
                    "GPUs (Adreno, Mali) don't write the whole image out and read it back.");
REXCVAR_DEFINE_BOOL(native_skip_draw_commands, true, "GPU",
                    "Native renderer: while the emulated GPU doesn't draw (and "
                    "native_skip_state_writes is on), DrawVertices and DrawIndexedVertices "
                    "only update Direct3D's state and don't write their draw packets.");
REXCVAR_DEFINE_BOOL(native_skip_state_writes, true, "GPU",
                    "Native renderer: while the emulated GPU doesn't draw, Direct3D's state "
                    "flush before each draw skips writing the changed registers and shader "
                    "constants as GPU commands (only the vertex fetch and shader setup the "
                    "recording reads still run). Everything is written again when the "
                    "emulation draws.");
REXCVAR_DEFINE_BOOL(native_deferred_naming, true, "GPU",
                    "Native renderer: name each draw's textures, vertex and index data on a "
                    "recorder thread instead of the game's drawing thread. The game sees a "
                    "GPU fence pass only once the recorder has read everything drawn before "
                    "it, as a real GPU would. Needs a restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(native_vertex_cache, true, "GPU",
                    "Native renderer: reuse vertex buffers the game hasn't written to since the "
                    "last frame (write-watched pages) instead of copying them every frame");
REXCVAR_DEFINE_BOOL(native_msaa, true, "GPU",
                    "Native renderer: use the game's multisampling (MSAA) as the Xbox 360 does "
                    "(off: one sample per pixel, same lighting and colors, rougher edges "
                    "unless an upscaler smooths them)");
REXCVAR_DEFINE_BOOL(native_upscaler_msaa, false, "GPU",
                    "Native renderer: the game's multisampling (MSAA) while FSR or XeSS is on "
                    "(off: the upscaler smooths the edges; same lighting and colors, faster). "
                    "Without an upscaler native_msaa decides");
REXCVAR_DEFINE_INT32(native_render_scale, 100, "GPU",
                     "Native renderer: the 3D scene's resolution, in percent of the picture's "
                     "size on screen (the window, or its 16:9 part when letterboxed); the HUD, "
                     "menus and text are always drawn at full size")
    .range(25, 100);
REXCVAR_DEFINE_STRING(native_upscaler, "off", "GPU",
                      "Native renderer: temporal upscaler from the render scale to the "
                      "picture's size on screen: off, xess (Intel XeSS: XMX on Arc, DP4a "
                      "elsewhere; Windows only) or fsr (AMD FSR 3.1). Needs libxess.dll / "
                      "amd_fidelityfx_vk.dll next to the exe (setup.ps1 fetches them), on Linux "
                      "libamd_fidelityfx_vk.so (setup.sh builds it)")
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
REXCVAR_DEFINE_BOOL(native_sharp_fonts, false, "GPU",
                    "Native renderer: the game's fonts (menus, HUD, scores, button icons) "
                    "rebuilt at 4x their size from their own letters when first shown, so text "
                    "stays sharp at high resolutions instead of being a stretched 720p bitmap. "
                    "Off unless turned on here (Revamped Graphics doesn't turn it on)");
REXCVAR_DEFINE_BOOL(native_texture_pack, true, "GPU",
                    "Native renderer: replaces game textures with the PNGs in the textures "
                    "folder next to the exe (<name>_<hash>.png; see textures\\README.txt). "
                    "It comes with the new Amped 3 logo for the start-up and title screens");
REXCVAR_DEFINE_BOOL(native_upscaled_textures, false, "GPU",
                    "Native renderer: uses the game textures upscaled on this PC at install "
                    "(textures\\upscaled, made by the installer's optional texture upscaling "
                    "step) for every texture the texture pack and Sharp fonts don't replace. "
                    "Off unless turned on here (Revamped Graphics doesn't turn it on)");
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
REXCVAR_DEFINE_INT32(native_shadow_resolution, 200, "GPU",
                     "Native renderer: the sun's shadow map, in percent of the game's "
                     "(1280 x 720), whatever the scene's resolution: higher gives sharper, more "
                     "detailed shadows from riders, rails and objects")
    .range(100, 400);
REXCVAR_DEFINE_INT32(native_shadow_distance, 0, "GPU",
                     "Native renderer: how far ahead the sun's shadows reach, in metres (0: the "
                     "game's, about 40 m). Only what the game draws into its shadow map casts "
                     "shadows; farther reach spreads the map thinner")
    .range(0, 300);
REXCVAR_DEFINE_BOOL(native_tree_shadows, true, "GPU",
                    "Native renderer: the trees and objects near the slope cast real sun "
                    "shadows (into the game's shadow map), dappled through the branches");
REXCVAR_DEFINE_BOOL(native_ambient_occlusion, true, "GPU",
                    "Native renderer: ambient occlusion: creases, corners and the snow around "
                    "trees, rails and jumps shaded where the open sky's light can't reach");
REXCVAR_DEFINE_DOUBLE(native_ambient_occlusion_strength, 1.0, "GPU",
                      "Native renderer: ambient occlusion strength (0..2)")
    .range(0.0, 2.0);
REXCVAR_DEFINE_BOOL(native_ambient_occlusion_debug, false, "GPU",
                    "Native renderer, diagnostics: show the ambient occlusion alone (white = "
                    "open)");
REXCVAR_DEFINE_BOOL(native_reactive_debug, false, "GPU",
                    "Native renderer, diagnostics: with FSR, show the scene (stretched, not "
                    "upscaled, darkened) with FSR's reactive mask in red: where snow and "
                    "other particles tell FSR to trust the current frame");
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
constexpr uint32_t kGpuFencePointer = 10384;  // pointer to the fence value the GPU writes
constexpr uint32_t kCpuFence = 10396;         // the next fence Direct3D inserts
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

// What a draw's resources are named from: copied from the device on the game
// thread at the draw, named on the recorder thread (see "Deferred naming"),
// or right away when that is off. The results go into the draw at the end of
// the frame.
struct DrawInputs {
  uint32_t draw = 0;  // index in the frame's draws
  struct Texture {
    uint32_t slot;
    std::array<uint32_t, 6> words;
  };
  struct VertexFetch {
    uint32_t vf, w0, w1;
  };
  native::SmallVector<Texture, 4> textures;
  native::SmallVector<VertexFetch, 2> vertex_fetches;
  uint32_t ib_address = 0, ib_size = 0;  // indexed draws through an index buffer
  // Results.
  native::SmallVector<native::FrameTexture, 4> named_textures;
  native::SmallMap<uint32_t, std::string, 2> vmem;
  std::string index_data;
  // Emptied for reuse in place (assigning a fresh one moved ~500 bytes of
  // inline storage element by element).
  void Reset() {
    draw = 0;
    textures.clear();
    vertex_fetches.clear();
    ib_address = ib_size = 0;
    named_textures.clear();
    vmem.clear();
    index_data.clear();
  }
};

// DrawInputs in chunks: adding one never moves the others, so the recorder
// thread can work on one while the game thread adds more. No lock: only the
// game thread adds, and the recorder reads an entry only after the queue
// (g_queue_mutex) handed it over; the chunk table is fixed, never moved.
class DrawInputList {
 public:
  static constexpr size_t kChunk = 256;
  using Chunk = std::array<DrawInputs, kChunk>;
  // Game thread. Returns the entry and its index.
  DrawInputs& Add(size_t* index) {
    const size_t i = count_.load(std::memory_order_relaxed);
    auto& chunk = chunks_[std::min(i / kChunk, kMaxChunks - 1)];
    if (!chunk) chunk = std::make_unique<Chunk>();
    DrawInputs& d = (*chunk)[i % kChunk];
    if (i < reused_) d.Reset();  // from an earlier frame
    count_.store(i + 1, std::memory_order_release);
    *index = i;
    return d;
  }
  DrawInputs& At(size_t i) { return (*chunks_[i / kChunk])[i % kChunk]; }
  size_t size() const { return count_.load(std::memory_order_acquire); }
  // Chunks of an earlier frame to fill again (frame recycling: their memory
  // is already the process's). Before the frame is used.
  void Adopt(std::vector<std::unique_ptr<Chunk>> chunks) {
    size_t n = 0;
    for (auto& c : chunks) {
      if (n < kMaxChunks) chunks_[n++] = std::move(c);
    }
    reused_ = n * kChunk;
  }
  // When the frame is done with.
  std::vector<std::unique_ptr<Chunk>> Release() {
    std::vector<std::unique_ptr<Chunk>> out;
    for (auto& c : chunks_) {
      if (c) out.push_back(std::move(c));
    }
    count_ = 0;
    reused_ = 0;
    return out;
  }

 private:
  // Enough for kMaxEvents draws (Room() stops recording before more).
  static constexpr size_t kMaxChunks = kMaxEvents / kChunk + 2;
  std::array<std::unique_ptr<Chunk>, kMaxChunks> chunks_;
  std::atomic<size_t> count_{0};
  size_t reused_ = 0;  // the first reused_ entries come from an earlier frame
};

struct RecordedFrame {
  Frame frame;
  // blobs / versions / textures: written by the game thread (user-pointer
  // data) and by the recorder thread (named resources) while recording.
  std::mutex maps_mutex;
  DrawInputList inputs;
  // Front buffer fetch words to name at the end of the frame.
  std::array<uint32_t, 6> swap_fetch_words{};
  bool name_swap = false;
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
  ~RecordedFrame();
};

// Recycled frame memory: a recorded frame's big buffers (draws, events, state
// deltas, draw inputs: a few MB) go back to a pool when it is destroyed, and
// the next frame takes them. Fresh allocations of that size come straight
// from the kernel each frame, and the game thread then page-faulted on every
// page it wrote (thousands of faults a frame).
struct FrameBuffers {
  std::vector<native::Event> events;
  std::vector<native::Draw> draws;
  std::vector<uint8_t> deltas;
  std::vector<uint32_t> draw_delta_end;
  std::vector<std::unique_ptr<DrawInputList::Chunk>> chunks;
};
std::mutex g_frame_pool_mutex;
std::vector<FrameBuffers> g_frame_pool;  // at most kFramePool
constexpr size_t kFramePool = 3;

RecordedFrame::~RecordedFrame() {
  FrameBuffers b;
  b.events = std::move(frame.events);
  b.draws = std::move(frame.draws);
  b.deltas = std::move(deltas);
  b.draw_delta_end = std::move(draw_delta_end);
  b.chunks = inputs.Release();
  // Emptied here, outside the lock (the draws hold strings and state).
  b.events.clear();
  b.draws.clear();
  b.deltas.clear();
  b.draw_delta_end.clear();
  std::lock_guard lock(g_frame_pool_mutex);
  if (g_frame_pool.size() < kFramePool) g_frame_pool.push_back(std::move(b));
}

// A frame's buffers: recycled ones when there are any.
void TakeFrameBuffers(RecordedFrame& f) {
  FrameBuffers b;
  {
    std::lock_guard lock(g_frame_pool_mutex);
    if (g_frame_pool.empty()) return;
    b = std::move(g_frame_pool.back());
    g_frame_pool.pop_back();
  }
  f.frame.events = std::move(b.events);
  f.frame.draws = std::move(b.draws);
  f.deltas = std::move(b.deltas);
  f.draw_delta_end = std::move(b.draw_delta_end);
  f.inputs.Adopt(std::move(b.chunks));
}

// Microcode by stem (all the game has run so far).
std::mutex g_ucode_mutex;
std::unordered_map<std::string, std::vector<uint8_t>> g_ucode;

// Short names for microcode in recorded draws: the stems (ucode_vs_<16 hex>)
// are longer than a string's inline buffer, so every draw's copy of them
// allocated. The recorder stores V<11> / P<11> instead (a hash of the stem;
// fits inline) and the resources look the stem up again when the renderer
// first reads a shader. Frame dumps (F10) keep the aliases; their microcode
// is saved under the same names.
std::mutex g_alias_mutex;
std::unordered_map<std::string, std::string> g_alias_to_stem;

bool IsMicrocodeAlias(const std::string& name) {
  return name.size() == 12 && (name[0] == 'V' || name[0] == 'P');
}

std::string StemOfAlias(const std::string& alias) {
  std::lock_guard lock(g_alias_mutex);
  auto it = g_alias_to_stem.find(alias);
  return it != g_alias_to_stem.end() ? it->second : std::string();
}

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
  auto blob = std::make_shared<std::vector<uint8_t>>(src, src + p.size);
  std::lock_guard lock(g_frame->maps_mutex);
  g_frame->blobs[p.name] = std::move(blob);
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

void CopyGuestInto(RecordedFrame& f, const std::string& name, const uint8_t* src, uint32_t size) {
  std::lock_guard lock(f.maps_mutex);
  auto& blob = f.blobs[name];
  if (!blob) {
    blob = NewBlob(name, src, size);
    f.versions[name] = NewVersion();
  }
}

void CopyGuest(const std::string& name, const uint8_t* src, uint32_t size) {
  CopyGuestInto(*g_frame, name, src, size);
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

std::atomic<uint64_t> g_watch_calls{0}, g_watch_pages{0};  // diagnostics (stall watch)

// Called with the memory system's lock held, on the writing thread: no locks.
std::pair<uint32_t, uint32_t> OnPhysicalWrite(void*, uint32_t start, uint32_t length, bool) {
  g_watch_calls.fetch_add(1, std::memory_order_relaxed);
  g_watch_pages.fetch_add((length + 4095) >> 12, std::memory_order_relaxed);
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
  uint32_t same_streak = 0;   // dynamic: consecutive copies that found it unchanged
  uint32_t same_needed = 1;   // ... before it is watched (1: a range seen for the first time)
};
std::unordered_map<uint64_t, CachedVertices> g_vertex_cache;

// A range treated as dynamic (rewritten every frame) is watched again only
// once it stopped changing for this many uses in a row. (It used to be
// watched again after 600 frames whatever it did: a video's buffers then
// cost a page fault per page for a few frames every 20 s, and a fault costs
// about 1 ms on an ARM handheld: ~0.9 s video freezes.)
constexpr uint32_t kStaticAgain = 30;
// A range seen for the first time isn't watched yet either: only when its
// next use finds it unchanged. A video's buffers, rewritten every frame, are
// then never protected (their first two frames used to fault on every page:
// a ~0.9 s freeze and an audio pop as an in-game video starts).
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
  if (!e.data) {  // first use: copied, not watched yet (kStaticAgain)
    e.dynamic = true;
    e.same_streak = 0;
    e.same_needed = 1;
  }
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
    // Rewritten since the last copy: after two uses in a row it is treated
    // as dynamic (copied every use, unwatched).
    if (++e.dirty_streak >= 2) {
      e.dynamic = true;
      e.same_streak = 0;
      e.same_needed = kStaticAgain;
    }
  }
  // Dynamic and unchanged for a while: watched again.
  if (e.dynamic && e.data && e.data->size() == size &&
      std::memcmp(e.data->data(), src, size) == 0) {
    if (++e.same_streak >= e.same_needed) {
      e.dynamic = false;
      e.dirty_streak = 0;
    }
  } else {
    e.same_streak = 0;
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
  uint32_t same_streak = 0;  // dynamic: consecutive hashes that found it unchanged
  uint32_t same_needed = 1;  // ... before it is watched (1: a range seen for the first time)
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
  if (inserted) e.dynamic = true;  // first use: hashed, not watched yet (kStaticAgain)
  if (!inserted && !e.dynamic && PagesClean(address, size, e.epoch)) {
    e.dirty_streak = 0;
    return e.hash;
  }
  if (!inserted && !e.dynamic && ++e.dirty_streak >= 2) {
    e.dynamic = true;  // rewritten every frame: hash it every frame, unwatched
    e.same_streak = 0;
    e.same_needed = kStaticAgain;
  }
  // Watched (static) ranges: the watch goes on before the bytes are read, so
  // a write after the read is never missed.
  uint32_t epoch = g_write_epoch.load(std::memory_order_acquire);
  if (!e.dynamic) memory->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
  uint64_t h = HashBytes(p, size, 1469598103934665603ull);
  // Dynamic and unchanged for a while: watched again (kStaticAgain).
  if (e.dynamic && !inserted && h == e.hash) {
    if (++e.same_streak >= e.same_needed) {
      e.dynamic = false;
      e.dirty_streak = 0;
      epoch = g_write_epoch.load(std::memory_order_acquire);
      memory->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
      h = HashBytes(p, size, 1469598103934665603ull);
    }
  } else {
    e.same_streak = 0;
  }
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

const std::string& TextureName(RecordedFrame& f, const std::array<uint32_t, 6>& words) {
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
  {
    std::lock_guard lock(f.maps_mutex);
    f.textures[name] = words;
  }
  const std::string& stored = g_tex_names.emplace(key, name).first->second;
  // Snapshot what changed (within a budget; the rest is read by the worker).
  auto snapshot = [&](const char* suffix, uint32_t address, uint32_t size) {
    if (!size || g_texture_snapshot_bytes + size > kTextureSnapshotBudget) return;
    const uint8_t* p = memory->TranslatePhysical<const uint8_t*>(address);
    auto blob = std::make_shared<std::vector<uint8_t>>(p, p + size);
    std::lock_guard lock(f.maps_mutex);
    f.blobs[name + suffix] = std::move(blob);
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

// The recorder side's per-frame reset: the names are per frame (a texture's
// name carries its contents' hash). On the recorder thread with deferred
// naming, else on the game thread at the frame's start.
void BeginNaming() {
  ++g_record_frame;
  TrimVertexCache();
  TrimTextureHashes();
  g_texture_snapshot_bytes = 0;
  g_tex_names.clear();
  g_blob_names.clear();
}

bool DeferredNaming();
void StopRecorder();

void StartFrame() {
  if (!DeferredNaming()) BeginNaming();
  g_frame = std::make_unique<RecordedFrame>();
  TakeFrameBuffers(*g_frame);
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
    if (IsMicrocodeAlias(name)) return FindMicrocode(StemOfAlias(name));
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
  // Render passes and clears (RenderStats).
  uint64_t passes = 0, clear_passes = 0, clears_merged = 0, clears_in_pass = 0;
  // GPU time by part (Renderer::GpuParts), summed per name; frames measured.
  std::map<std::string, double> gpu_parts;
  uint32_t gpu_part_frames = 0;
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
  {
    // Write-watch faults (a game thread writing a page the renderer watches:
    // a signal and a protection change, far dearer than the write).
    static uint64_t last_calls = 0, last_pages = 0;
    const uint64_t calls = g_watch_calls.load(), pages = g_watch_pages.load();
    REXLOG_INFO("Native renderer: write-watch faults {:.1f} per frame ({:.1f} pages)",
                double(calls - last_calls) / n, double(pages - last_pages) / n);
    last_calls = calls;
    last_pages = pages;
  }
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
  REXLOG_INFO(
      "Native renderer: per frame {:.1f} render passes; clears: {:.1f} as passes of their own, "
      "{:.1f} folded into a pass's load op, {:.1f} inside a pass",
      double(t.passes) / n, double(t.clear_passes) / n, double(t.clears_merged) / n,
      double(t.clears_in_pass) / n);
  if (t.gpu_part_frames) {
    // The biggest parts first.
    std::vector<std::pair<double, std::string>> parts;
    double sum = 0;
    for (const auto& [name, ms] : t.gpu_parts) {
      parts.push_back({ms / t.gpu_part_frames, name});
      sum += ms / t.gpu_part_frames;
    }
    std::sort(parts.rbegin(), parts.rend());
    std::string text;
    for (size_t i = 0; i < parts.size() && i < 10; ++i) {
      text += fmt::format("{}{} {:.2f}", i ? ", " : "", parts[i].second, parts[i].first);
    }
    REXLOG_INFO("Native renderer: GPU by part ({:.2f} ms per frame, {} parts): {}", sum,
                parts.size(), text);
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
    {
      const auto parts = renderer->GpuParts(pending.ticket);
      if (!parts.empty()) {
        std::lock_guard lock(g_timing_mutex);
        for (const auto& part : parts) g_timings.gpu_parts[part.name] += part.ms;
        ++g_timings.gpu_part_frames;
      }
    }
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
      // The SDK's settings (Display > Image quality), read by name: they live
      // in the GPU plugin.
      float scale = RenderScaleFor(REXCVAR_GET(native_render_scale));
      // The render scale is the 3D scene's; the HUD, menus and text are
      // drawn at the picture's full size on screen (100%), where an upscaler
      // also rebuilds the scene.
      const std::string upscaler_name = REXCVAR_GET(native_upscaler);
      // native::UpscalerKind: 1 = XeSS, 2 = FSR.
      const int upscaler = upscaler_name == "xess" ? 1 : upscaler_name == "fsr" ? 2 : 0;
      // The MSAA setting decides how render targets are made: start over.
      // With an upscaler its own setting (off by default: the upscaler
      // smooths the edges).
      const bool msaa = upscaler ? REXCVAR_GET(native_upscaler_msaa) : REXCVAR_GET(native_msaa);
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
      // Sharp fonts and upscaled textures: only when turned on themselves
      // (off by default, not part of Revamped Graphics).
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
                                   float(REXCVAR_GET(native_upscaler_sharpness)),
                                   REXCVAR_GET(native_reactive_debug));
        renderer->SetFog(revamped && REXCVAR_GET(native_fog) ? float(REXCVAR_GET(native_fog_density)) : 0.0f,
                         float(REXCVAR_GET(native_fog_start)));
        renderer->SetFoliageFade(revamped && REXCVAR_GET(native_foliage_fade));
        renderer->SetMergeClears(REXCVAR_GET(native_merge_clears));
        renderer->SetParallelCommands(REXCVAR_GET(native_parallel_commands));
        renderer->SetMapClouds(revamped && REXCVAR_GET(native_map_clouds));
        renderer->SetHideInterface(g_hide_interface.load());
        // The game state machine (0x828901E0) + 8: 7 = the trail map.
        if (auto* memory = GuestMemory()) {
          const uint32_t game_state = U32(memory->virtual_membase() + 0x828901E8u);
          renderer->SetTrailMapOpen(game_state == 7);
          renderer->SetInCutscene(game_state == 20);  // a story scene
          renderer->SetGameState(int(game_state));
        }
        // Without Revamped Graphics: the game's shadow map (1x, its reach).
        renderer->SetShadows(
            revamped ? float(REXCVAR_GET(native_shadow_resolution)) / 100.0f : 1.0f,
            revamped ? float(REXCVAR_GET(native_shadow_distance)) : 0.0f);
        renderer->SetTreeShadows(revamped && REXCVAR_GET(native_tree_shadows) ? 1.0f : 0.0f);
        renderer->SetAmbientOcclusion(
            revamped && REXCVAR_GET(native_ambient_occlusion)
                ? float(REXCVAR_GET(native_ambient_occlusion_strength))
                : 0.0f,
            REXCVAR_GET(native_ambient_occlusion_debug));
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
        g_timings.passes += stats.passes;
        g_timings.clear_passes += stats.clear_passes;
        g_timings.clears_merged += stats.clears_merged;
        g_timings.clears_in_pass += stats.clears_in_pass;
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
  StopRecorder();
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

bool SkipEmulatedDrawCommands() {
  return REXCVAR_GET(native_skip_draw_commands) && SkipEmulatedStateWrites();
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

// The alias of a microcode stem (empty stays empty); the last one per stage is
// remembered, as consecutive draws mostly share their shaders.
const std::string& MicrocodeAlias(const std::string& stem, char tag) {
  struct Last {
    std::string stem, alias;
  };
  static Last last[2];
  static const std::string kEmpty;
  if (stem.empty()) return kEmpty;
  Last& l = last[tag == 'V' ? 0 : 1];
  if (!l.alias.empty() && l.stem == stem) return l.alias;
  l.stem = stem;
  l.alias = native::ShortName(tag, Fnv1a(stem.data(), stem.size()), 11);
  std::lock_guard lock(g_alias_mutex);
  g_alias_to_stem.emplace(l.alias, stem);
  return l.alias;
}

std::array<SlotName, kTextureSlots> g_slot_names;
std::array<VertexFetchName, 96> g_vf_names;

namespace {

// Names a draw's textures, vertex memory and index buffer from its inputs
// (recorder side: the name caches, the vertex cache and the texture hashes
// are only used here).
void NameDraw(RecordedFrame& f, DrawInputs& in) {
  auto* memory = GuestMemory();
  for (const auto& t : in.textures) {
    native::FrameTexture ft;
    ft.slot = t.slot;
    std::copy(t.words.begin(), t.words.end(), ft.raw.begin());
    SlotName& last = g_slot_names[t.slot];
    if (last.frame != g_record_frame || last.words != t.words) {
      last.words = t.words;
      last.name = TextureName(f, t.words);
      last.frame = g_record_frame;
    }
    ft.data = last.name;
    in.named_textures.push_back(std::move(ft));
  }
  if (!memory) return;
  for (const auto& v : in.vertex_fetches) {
    VertexFetchName& last = g_vf_names[v.vf % 96];
    if (last.frame == g_record_frame && last.w0 == v.w0 && last.w1 == v.w1) {
      in.vmem[v.vf] = last.name;
      continue;
    }
    const uint32_t address = (v.w0 & ~3u) & 0x1FFFFFFF;
    const uint32_t size = ((v.w1 >> 2) & 0xFFFFFF) * 4;
    if (!address || !size || size > (64u << 20)) continue;
    const uint64_t key = (uint64_t(address) << 32) | size;
    auto it = g_blob_names.find(key);
    if (it == g_blob_names.end()) {
      std::string name = native::ShortName('M', (uint64_t(size) << 29) | address, 10);
      bool have;
      {
        std::lock_guard lock(f.maps_mutex);
        have = f.blobs[name] != nullptr;
      }
      if (!have) {
        uint64_t version = 0;
        auto blob = CachedVertexData(memory, name, address, size, &version);
        std::lock_guard lock(f.maps_mutex);
        f.blobs[name] = std::move(blob);
        f.versions[name] = version;
      }
      it = g_blob_names.emplace(key, std::move(name)).first;
    }
    in.vmem[v.vf] = it->second;
    last.w0 = v.w0;
    last.w1 = v.w1;
    last.name = it->second;
    last.frame = g_record_frame;
  }
  const uint32_t address = in.ib_address, size = in.ib_size;
  if (address && size && size <= (64u << 20)) {
    const uint64_t key = (uint64_t(address) << 32) | size | (1ull << 31);  // not physical
    auto it = g_blob_names.find(key);
    if (it == g_blob_names.end()) {
      std::string name = native::ShortName('I', (uint64_t(size) << 32) | address, 10);
      // Index buffers are mostly static too: through the vertex cache when
      // they are in physical memory (as Direct3D allocates them).
      const uint32_t physical = memory->GetPhysicalAddress(address);
      if (physical != UINT32_MAX &&
          memory->GetPhysicalAddress(address + size - 1) == physical + size - 1) {
        bool have;
        {
          std::lock_guard lock(f.maps_mutex);
          have = f.blobs[name] != nullptr;
        }
        if (!have) {
          uint64_t version = 0;
          auto blob = CachedVertexData(memory, name, physical, size, &version);
          std::lock_guard lock(f.maps_mutex);
          f.blobs[name] = std::move(blob);
          f.versions[name] = version;
        }
      } else {
        CopyGuestInto(f, name, memory->TranslateVirtual<const uint8_t*>(address), size);
      }
      it = g_blob_names.emplace(key, std::move(name)).first;
    }
    in.index_data = it->second;
  }
}

void ApplyNames(Draw& d, DrawInputs& in) {
  for (auto& t : in.named_textures) d.textures.push_back(std::move(t));
  for (const auto& [vf, name] : in.vmem) d.vmem[vf] = name;
  if (!in.index_data.empty()) d.index_data = std::move(in.index_data);
}

// --- Deferred naming --------------------------------------------------------------
//
// With native_deferred_naming, the game's drawing thread only copies each
// draw's fetch constants and index buffer address (DrawInputs) and queues
// them; the recorder thread names them (texture hashes, the vertex cache,
// index copies) and, at the end of each frame, puts the names into the draws
// and hands the frame on.
//
// The recorder reads guest memory after the draw, so the game must not have
// rewritten it by then. On the console the GPU reads it later too, and the game
// waits for a GPU fence before rewriting memory a draw used. Direct3D inserts
// a fence (value: the device's CPU fence, +2 each time) with an EVENT_WRITE_SHD
// packet that makes the GPU store it to the fence word (device+10384 points to
// it), and in some phases also stores it from the CPU right away. So a fence
// is let through only once the recorder has named every draw made before it:
// the command processor waits for that before storing a fence (SDK patch
// 0019), and so does the game thread before Direct3D's own CPU store
// (sub_825A3330). Each draw is queued with the CPU fence current when it was
// drawn (the fence that covers it).

struct QueuedJob {
  RecordedFrame* frame = nullptr;  // the draw's frame (or the frame ending)
  size_t input = 0;                // index in frame->inputs
  uint32_t stamp = 0;              // CPU fence when it was drawn (covered by that fence)
  std::unique_ptr<RecordedFrame> end;  // set: end of this frame
  bool hand_over = false;
};

// The game thread queues draws without waking the recorder for each one (a
// wake is a system call: about 3 us a draw on the handheld, 5 ms a frame,
// on the thread that limits the frame rate). The recorder takes everything
// queued at once; it's woken when it sleeps and kWakeBatch draws are waiting,
// at the end of a frame, and when a fence waits for the names.
constexpr size_t kWakeBatch = 32;
// And it looks for queued draws on its own this often while it sleeps, so a
// draw is never left waiting long for its name (frames of a few draws: menus,
// videos).
constexpr auto kRecorderPoll = std::chrono::milliseconds(2);

// Diagnostics (Armada video stalls): rare long waits for names, and long
// naming jobs, go to the log.
void LogSlow(const char* what, double ms, size_t queued) {
  static std::atomic<int> logged{0};
  if (ms < 50.0 || logged.fetch_add(1) >= 40) return;
  REXLOG_WARN("Native renderer: {} took {:.1f} ms ({} draws queued)", what, ms, queued);
}

std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::condition_variable g_drained_cv;  // draws were named
std::vector<QueuedJob> g_queue;
bool g_queue_has_stamp = false;  // g_queue holds a draw (its oldest stamp below)
uint32_t g_queue_oldest_stamp = 0;
bool g_naming_busy = false;      // the recorder holds draws it took off the queue
uint32_t g_naming_stamp = 0;     // the oldest of those not named yet
bool g_recorder_sleeping = false;
std::atomic<int> g_name_waiters{0};  // threads in WaitNamedThrough
std::thread g_recorder;
std::atomic<bool> g_recorder_stop{false};
bool g_deferred = false;  // decided when the device starts (restart to change)
std::atomic<uint32_t> g_fence_physical{0};  // the fence word, physical address
std::atomic<uint32_t> g_fence_physical_alt{0};

bool DeferredNaming() { return g_deferred; }

// Fence a is at or after b (32-bit wrap-around, as Direct3D compares them).
bool FenceAtOrAfter(uint32_t a, uint32_t b) { return int32_t(a - b) >= 0; }

// Waits until every draw covered by `fence` (stamp at or before it) is named.
void WaitNamedThrough(uint32_t fence) {
  std::unique_lock lock(g_queue_mutex);
  const auto pending = [fence] {
    if (g_naming_busy && FenceAtOrAfter(fence, g_naming_stamp)) return true;
    return g_queue_has_stamp && FenceAtOrAfter(fence, g_queue_oldest_stamp);
  };
  if (!pending()) return;
  g_name_waiters.fetch_add(1);
  const auto wait_start = std::chrono::steady_clock::now();
  if (g_recorder_sleeping) {  // draws it hasn't been woken for
    g_recorder_sleeping = false;
    g_queue_cv.notify_one();
  }
  // A safety net: never hold the GPU or the game for long if something is off.
  if (!g_drained_cv.wait_for(lock, std::chrono::seconds(2), [&] {
        return !pending() || g_recorder_stop.load();
      })) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("Native renderer: fence {:08X} waited 2 s for the recorder; going on", fence);
    }
  }
  g_name_waiters.fetch_sub(1);
  LogSlow("a fence waiting for draw names",
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start)
              .count(),
          g_queue.size());
}

void OnGpuEventWrite(uint32_t physical_address, uint32_t value) {
  if (physical_address == g_fence_physical.load(std::memory_order_relaxed) ||
      physical_address == g_fence_physical_alt.load(std::memory_order_relaxed)) {
    WaitNamedThrough(value);
  }
}

void EnqueueDraw(RecordedFrame* frame, size_t input, uint32_t stamp) {
  bool wake;
  {
    std::lock_guard lock(g_queue_mutex);
    if (!g_queue_has_stamp) {
      g_queue_has_stamp = true;
      g_queue_oldest_stamp = stamp;
    }
    QueuedJob& j = g_queue.emplace_back();
    j.frame = frame;
    j.input = input;
    j.stamp = stamp;
    wake = g_recorder_sleeping && g_queue.size() >= kWakeBatch;
    if (wake) g_recorder_sleeping = false;  // one wake is enough
  }
  if (wake) g_queue_cv.notify_one();
}

void EnqueueEndOfFrame(std::unique_ptr<RecordedFrame> frame, bool hand_over) {
  {
    std::lock_guard lock(g_queue_mutex);
    QueuedJob& j = g_queue.emplace_back();
    j.frame = frame.get();
    j.end = std::move(frame);
    j.hand_over = hand_over;
    g_recorder_sleeping = false;
  }
  g_queue_cv.notify_one();
}

// The stamp of the first draw at or after `from` in `batch` (none: false).
bool FirstStamp(const std::vector<QueuedJob>& batch, size_t from, uint32_t* stamp) {
  for (size_t i = from; i < batch.size(); ++i) {
    if (!batch[i].end) {
      *stamp = batch[i].stamp;
      return true;
    }
  }
  return false;
}

void RecorderMain() {
  qos::PreferPerformanceThread(L"amped3 native recorder", true);
  rex::thread::set_current_thread_name("amped3 recorder");
  RecordedFrame* naming = nullptr;  // the frame BeginNaming was last called for
  std::vector<QueuedJob> batch;
  while (!g_recorder_stop.load()) {
    {
      std::unique_lock lock(g_queue_mutex);
      if (g_queue.empty()) {
        g_recorder_sleeping = true;
        g_queue_cv.wait_for(lock, kRecorderPoll, [] {
          return g_recorder_stop.load() || (!g_recorder_sleeping && !g_queue.empty());
        });
        g_recorder_sleeping = false;
      }
      if (g_queue.empty()) continue;
      batch.swap(g_queue);
      g_queue_has_stamp = false;
      g_naming_busy = FirstStamp(batch, 0, &g_naming_stamp);
    }
    for (size_t b = 0; b < batch.size(); ++b) {
      QueuedJob& job = batch[b];
      const bool end_job = job.end != nullptr;
      const auto t0 = std::chrono::steady_clock::now();
      if (job.frame != naming) {
        naming = job.frame;
        BeginNaming();
      }
      if (!job.end) {
        NameDraw(*job.frame, job.frame->inputs.At(job.input));
      } else {
        RecordedFrame& f = *job.end;
        const size_t n = f.inputs.size();
        for (size_t i = 0; i < n; ++i) {
          DrawInputs& in = f.inputs.At(i);
          if (in.draw < f.frame.draws.size()) ApplyNames(f.frame.draws[in.draw], in);
        }
        if (f.name_swap) f.frame.swap_data = TextureName(f, f.swap_fetch_words);
        naming = nullptr;
        if (job.hand_over) HandOver(std::move(job.end), g_prep_mutex, g_prep_cv, g_prep);
        job.end.reset();
      }
      const int64_t job_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
      trace::Add(trace::Zone::kRecNaming, job_ns);
      LogSlow(end_job ? "naming a frame's end" : "naming a draw", double(job_ns) * 1e-6,
              batch.size() - b - 1);
      // A fence waiting for these names: let it see the progress now.
      if (g_name_waiters.load() > 0) {
        {
          std::lock_guard lock(g_queue_mutex);
          g_naming_busy = FirstStamp(batch, b + 1, &g_naming_stamp);
        }
        g_drained_cv.notify_all();
      }
    }
    batch.clear();
    {
      std::lock_guard lock(g_queue_mutex);
      g_naming_busy = false;
    }
    if (g_name_waiters.load() > 0) g_drained_cv.notify_all();
  }
}

void StopRecorder() {
  if (g_deferred) rex::system::SetGpuEventWriteHook(nullptr);
  g_recorder_stop = true;
  g_queue_cv.notify_all();
  g_drained_cv.notify_all();
  if (g_recorder.joinable()) g_recorder.join();
}

}  // namespace

void OnDraw(uint8_t* base, uint32_t dev, const DrawCall& call, const std::string& vs_ucode,
            const std::string& ps_ucode) {
  if (!Recording()) return;
  struct AllocCount {  // F11 trace: heap allocations while recording a draw
    uint64_t start = trace::ThreadAllocations();
    ~AllocCount() { trace::AddRecordAllocations(trace::ThreadAllocations() - start); }
  } alloc_count;
  RecordTimer timer;
  std::lock_guard lock(g_mutex);
  if (!g_frame || !Room()) return;
  // Where a draw's recording time goes, timed on one draw in 8 (and counted
  // 8 times): a clock read costs about as much as some of the parts, and
  // timing every draw cost the game thread several of them per draw.
  const bool timed = (g_events & 7) == 0;
  const auto setup_t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  auto lap = [timed, t = setup_t0](int part) mutable {
    if (!timed) return;
    const auto now = std::chrono::steady_clock::now();
    const int64_t ns = 8 * std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count();
    g_draw_ns[part] += uint64_t(ns);
    // F11 trace: textures, vertex memory, indices, state (part 4, storing, is small).
    static constexpr trace::Zone kZones[] = {trace::Zone::kRecTextures,
                                             trace::Zone::kRecVertices,
                                             trace::Zone::kRecIndices, trace::Zone::kRecState};
    if (part < 4) trace::Add(kZones[part], ns);
    t = now;
  };
  const uint32_t number = AddEvent(native::EventType::kDraw, uint32_t(g_frame->frame.draws.size()));
  // Built in place: a Draw keeps its first textures and streams inline, so
  // moving one in afterwards copied a few hundred bytes per draw.
  Draw& d = g_frame->frame.draws.emplace_back();
  d.kind = Draw::Kind(int(call.kind));
  d.prim = call.prim;
  d.start = call.start;
  d.count = call.count;
  d.base_vertex = call.base_vertex;
  d.vs = MicrocodeAlias(vs_ucode, 'V');
  d.ps = MicrocodeAlias(ps_ucode, 'P');
  auto* memory = GuestMemory();

  // What the draw's resources are named from, copied from the device now.
  // Textures: the fetch constants the shaders read (all bound ones if a
  // shader couldn't be translated).
  const ShaderUse& vs_use = UseOf(vs_ucode, native::ShaderStage::kVertex);
  const ShaderUse& ps_use = UseOf(ps_ucode, native::ShaderStage::kPixel);
  const bool deferred = DeferredNaming();
  DrawInputs local;
  size_t input_index = 0;
  DrawInputs& in = deferred ? g_frame->inputs.Add(&input_index) : local;
  in.draw = uint32_t(g_frame->frame.draws.size() - 1);
  uint32_t wanted = 0;
  if (vs_use.known && ps_use.known) {
    for (uint32_t t : vs_use.textures) wanted |= 1u << (t & 31);
    for (uint32_t t : ps_use.textures) wanted |= 1u << (t & 31);
  } else {
    wanted = ~0u;
  }
  // F11 trace: the setup above (part of rec_textures).
  if (timed) {
    trace::Add(trace::Zone::kRecSetup, 8 * std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - setup_t0)
                                               .count());
  }
  for (uint32_t s = 0; s < kTextureSlots; ++s) {
    if (!(wanted & (1u << s))) continue;
    const auto words = FetchWords(base, dev + kFetchConsts + 24 * s);
    // A texture bound through Direct3D, or one the game wrote straight into
    // the fetch constant with no texture object in the slot (the animated
    // page in story scene 2.1.1 does this): the GPU reads the fetch constant
    // either way.
    if (!U32(base, dev + kTextures + 4 * s) && !RawTextureFetch(words)) continue;
    in.textures.push_back({s, words});
  }

  lap(0);  // textures (and the draw's setup)

  // Vertex memory the vertex shader fetches.
  for (uint32_t vf : VertexFetchSlots(vs_ucode)) {
    const uint32_t w0 = U32(base, dev + kFetchConsts + vf * 8);
    const uint32_t w1 = U32(base, dev + kFetchConsts + vf * 8 + 4);
    if ((w0 & 3) == 3) in.vertex_fetches.push_back({vf, w0, w1});
  }

  lap(1);

  // Indices and user-pointer data (user-pointer data is copied now: the game
  // reuses that memory right away).
  if (call.kind == DrawCall::Kind::kIndexed) {
    const uint32_t ib = U32(base, dev + kIndexBuffer);
    if (ib) {
      in.ib_address = U32(base, ib + 12);
      in.ib_size = U32(base, ib + 16);
      d.index_32bit = (U32(base, ib) & 0x80000000u) != 0;
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
  if (deferred) {
    EnqueueDraw(g_frame.get(), input_index, U32(base, dev + kCpuFence));
  } else {
    NameDraw(*g_frame, in);
    ApplyNames(d, in);
  }

  lap(2);
  RecordStateDelta(base, dev, vs_use, ps_use);
  lap(3);
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

// --- Stall watchdog -----------------------------------------------------------
//
// Diagnostics, kept for future freezes (it found the write-watch faults that
// froze in-game videos on Armada): when no frame has been swapped for 250 ms
// (but under 5 s: loading), logs where every game thread is (link register
// and r3 as of its last kernel call) once, then where the stall's CPU time
// went, for the first 12 stalls. Quiet otherwise.
std::atomic<int64_t> g_last_swap_ns{0};
std::once_flag g_watchdog_once;

int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Per thread CPU time (Linux: /proc/self/task/*/stat), user and system
// separately, in clock ticks: what a stall's time went to.
struct ThreadTimes {
  std::string name;
  uint64_t user = 0, system = 0;
};
std::map<int, ThreadTimes> ReadThreadTimes() {
  std::map<int, ThreadTimes> out;
#if defined(__linux__)
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
    std::ifstream f(entry.path() / "stat");
    std::string line;
    if (!std::getline(f, line)) continue;
    const size_t open = line.find('('), close = line.rfind(')');
    if (open == std::string::npos || close == std::string::npos) continue;
    ThreadTimes t;
    t.name = line.substr(open + 1, close - open - 1);
    std::istringstream rest(line.substr(close + 2));
    std::string field;
    // Fields after the name: state(3) ... utime(14) stime(15).
    for (int i = 3; i <= 15 && rest >> field; ++i) {
      if (i == 14) t.user = std::strtoull(field.c_str(), nullptr, 10);
      if (i == 15) t.system = std::strtoull(field.c_str(), nullptr, 10);
    }
    out[std::atoi(entry.path().filename().c_str())] = t;
  }
#endif
  return out;
}

std::string ThreadTimeDeltas(const std::map<int, ThreadTimes>& before,
                             const std::map<int, ThreadTimes>& after) {
  std::vector<std::pair<uint64_t, std::string>> rows;
  for (const auto& [tid, a] : after) {
    auto b = before.find(tid);
    const uint64_t u = a.user - (b != before.end() ? b->second.user : 0);
    const uint64_t k = a.system - (b != before.end() ? b->second.system : 0);
    if (u + k == 0) continue;
    rows.push_back({u + k, fmt::format("{} {} user {} ms / system {} ms", a.name, tid, u * 10, k * 10)});
  }
  std::sort(rows.rbegin(), rows.rend());
  std::string out;
  for (size_t i = 0; i < rows.size() && i < 6; ++i) out += (i ? "; " : "") + rows[i].second;
  return out;
}

void WatchdogMain() {
  rex::thread::set_current_thread_name("amped3 stall watch");
  int stalls = 0, samples = 0;
  int64_t stall_swap = -1;
  std::map<int, ThreadTimes> baseline;
  uint64_t base_calls = 0, base_pages = 0;
  int tick = 0;
  bool in_stall = false;
  auto report = [&](const char* when) {
    REXLOG_WARN("Stall watch: {}: CPU {}; write-watch faults {} ({} pages)", when,
                ThreadTimeDeltas(baseline, ReadThreadTimes()), g_watch_calls.load() - base_calls,
                g_watch_pages.load() - base_pages);
  };
  while (stalls < 12) {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const int64_t last = g_last_swap_ns.load();
    if (!last) continue;
    const double ms = double(SteadyNs() - last) * 1e-6;
    if (ms < 250.0 || ms > 5000.0) {  // (longer: loading, a menu; not this)
      if (in_stall) {
        in_stall = false;
        report("the stall's whole time");
      }
      // A fresh baseline about every 0.25 s, for the next stall.
      if (++tick % 4 == 0) {
        baseline = ReadThreadTimes();
        base_calls = g_watch_calls.load();
        base_pages = g_watch_pages.load();
      }
      continue;
    }
    if (last != stall_swap) {
      if (in_stall) report("the stall's whole time");
      stall_swap = last;
      samples = 0;
      ++stalls;
      in_stall = true;
    }
    if (samples++ >= 1) continue;  // one snapshot per stall
    REXLOG_WARN("Stall watch: no frame for {:.0f} ms; {}", ms,
                amped3::diag::GuestThreadSnapshot(rex::system::kernel_state()));
  }
}

void OnSwap(uint8_t* base, uint32_t front_buffer) {
  g_last_swap_ns.store(SteadyNs());
  std::call_once(g_watchdog_once, [] { std::thread(WatchdogMain).detach(); });
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
      if (DeferredNaming()) {
        g_frame->name_swap = true;
        std::copy(frame.swap_fetch.begin(), frame.swap_fetch.end(),
                  g_frame->swap_fetch_words.begin());
      } else {
        frame.swap_data = TextureName(*g_frame, frame.swap_fetch);
      }
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
    if (DeferredNaming()) {
      // The recorder thread names what is still queued, then hands the frame
      // on (or drops it).
      EnqueueEndOfFrame(std::move(g_frame), enabled && !g_frame_overflow);
    } else if (enabled && !g_frame_overflow) {
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

void OnRingBufferInitialized(uint8_t* base, uint32_t dev) {
  if (!REXCVAR_GET(native_deferred_naming) || g_deferred) return;
  auto* memory = GuestMemory();
  const uint32_t fence = U32(base, dev + kGpuFencePointer);
  if (!memory || !fence) return;
  // The address the GPU stores the fence to: the physical address of the
  // fence word (Direct3D computes it from the virtual one; both forms here).
  g_fence_physical = memory->GetPhysicalAddress(fence);
  g_fence_physical_alt = fence & 0x1FFFFFFF;
  rex::system::SetGpuEventWriteHook(&OnGpuEventWrite);
  g_deferred = true;
  g_recorder_stop = false;
  g_recorder = std::thread(RecorderMain);
  REXLOG_INFO("Native renderer: deferred naming on (GPU fence word {:08X})", fence);
}

void BeforeCpuFenceStore(uint8_t* base, uint32_t dev) {
  if (!g_deferred) return;
  // sub_825A3330 stores the fence itself when the device's 20416 word is 0
  // and flag 0x04 at +10433 is set: only then wait here.
  if (U32(base, dev + 20416) != 0 || !(base[dev + 10433] & 0x04)) return;
  WaitNamedThrough(U32(base, dev + kCpuFence));
}

}  // namespace amped3::live
