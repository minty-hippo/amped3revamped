// Native renderer preview in the running game (step 4, stage 3;
// docs/native-renderer-plan.md).
//
// With the setting `native_renderer` on, the Direct3D hooks (frame_capture.cpp)
// also record each frame's draws, clears, resolves and state here. A worker
// thread draws the recorded frame with the native renderer
// (src/native/renderer.cpp, its own Vulkan device), reads the image back and
// hands it to the window through the presenter (SDK patch 0008), in place of
// the emulated GPU's output. Once the native image is up, the emulated GPU
// stops drawing (step 5, SDK patch 0009): it still runs the command stream
// the game waits on (fences, interrupts), but draws, resolves and swaps do no
// host work. It draws again when the setting is switched off, if the native
// renderer fails, or with `native_keep_emulation`.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace rex::ui {
class Presenter;
}

namespace amped3::live {

struct DrawCall {
  enum class Kind { kVertices, kIndexed, kVerticesUP, kIndexedUP } kind = Kind::kVertices;
  uint32_t prim = 0;
  uint32_t start = 0;  // first vertex (non-indexed) or first index
  uint32_t count = 0;  // vertex or index count
  int32_t base_vertex = 0;
  // User-pointer draws (guest virtual addresses).
  uint32_t up_vertices = 0, up_vertices_size = 0;
  uint32_t up_indices = 0, up_indices_size = 0;
  bool up_index_32bit = false;
  // BeginVertices: the game writes the vertices after the call returns; they
  // are read at the next recorded event.
  bool vertices_follow = false;
};

// The guest's gamma ramp (the GPU's DC_LUT registers), as the emulated swap
// applies it: a 256-entry table (10 bits per channel, red at bit 20, green at
// 10, blue at 0) for 8-bit front buffers, a piecewise-linear ramp (128 x RGB,
// base in the low 16 bits, delta in the high 16) for 2_10_10_10 ones.
struct GammaRamp {
  uint32_t table[256] = {};
  uint32_t pwl[128][3] = {};
};

// `backend`: the GPU plugin's API ("d3d12" or "vulkan"), for presentation.
// `gamma`: fills in the current gamma ramp (false: not available).
// `host_drawing`: switches the emulated GPU's drawing on or off (SDK patch
// 0009; false: not available).
void Install(const std::filesystem::path& log_dir,
             std::function<rex::ui::Presenter*()> presenter, std::string backend,
             std::function<bool(GammaRamp*)> gamma = {},
             std::function<bool(bool)> host_drawing = {});
void Shutdown();

// The window's size in pixels (for the native render scale; 0 x 0 unknown).
void SetWindowSize(std::function<std::pair<uint32_t, uint32_t>()> size);
// The native renderer's internal resolution at `percent` render scale (of the
// picture's size on screen: the window, fitted to 16:9 when letterboxed), and
// the scale of the game's own resolution that gives it.
float RenderScaleFor(int percent, uint32_t* width = nullptr, uint32_t* height = nullptr);

// The setting is on (shader tracking must run).
bool Enabled();

// While the port's Graphics panel is open: the game's HUD and menus are left
// out of the picture, so the scene behind can be seen as settings change.
void SetHideInterface(bool hide);
// A frame is being recorded: the hooks below do work only then.
bool Recording();

// Whether Direct3D's per-draw state flush may skip writing GPU commands: the
// emulated GPU doesn't draw (the native renderer's image is up) and the
// setting native_skip_state_writes is on. Game thread, every draw.
bool SkipEmulatedStateWrites();

// Direct3D's ring buffer was set up (D3D_InitializeRingBuffer returned): with
// native_deferred_naming, starts the recorder thread and has the command
// processor hold each fence until the recorder has named the draws before it
// (see "Deferred naming" in live_renderer.cpp). Game thread, once.
void OnRingBufferInitialized(uint8_t* base, uint32_t dev);

// sub_825A3330, Direct3D inserting a fence: before it may store the fence
// value itself (deferred naming waits for the recorder first). Game thread.
void BeforeCpuFenceStore(uint8_t* base, uint32_t dev);

// Direct3D hooks, called after the call (state flushed), on the game's
// rendering thread.
void OnDraw(uint8_t* base, uint32_t dev, const DrawCall& call, const std::string& vs_ucode,
            const std::string& ps_ucode);
void OnClear(uint8_t* base, uint32_t dev, uint32_t flags, uint32_t color, double z,
             uint32_t stencil);
void OnResolve(uint8_t* base, uint32_t dev, uint32_t flags, uint32_t src_rect, uint32_t dest_tex,
               uint32_t dest_point, uint32_t clear_color, double z);
void OnBeginTiling(uint8_t* base, uint32_t count, uint32_t rects);
void OnEndTiling();
void OnSwap(uint8_t* base, uint32_t front_buffer);

// F10 with the preview on: the next recorded frame is also written to logs
// in the capture format (logs\frame_<time>_native.txt), for tools/replay.
void RequestDump();

// Microcode the GPU ran, by stem (ucode_vs_<hash>), big-endian as in memory.
// Called once per new stem.
void NoteMicrocode(const std::string& stem, const uint8_t* data, size_t size);

struct Status {
  bool on = false;          // setting on
  bool presenting = false;  // the window shows the native image
  bool emulation_drawing = true;  // the emulated GPU draws too
  uint64_t frames = 0;      // frames drawn
  double render_ms = 0;     // last frame: drawing + readback
  std::string message;      // last problem, if any
};
Status GetStatus();

}  // namespace amped3::live
