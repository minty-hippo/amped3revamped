// The native renderer (step 4): draws a Frame (src/native/frame.h) with
// Vulkan, using the translated shaders (xenos_shader), the CPU texture decoder
// (xenos_texture) and the Xenos render state mapped to Vulkan.
//
// Used by the offline replayer (tools/replay) and, in the game, by the live
// renderer (live_renderer.cpp). A Renderer keeps its caches (shaders,
// pipelines, textures, render targets, resolved images) from frame to frame.
// Not thread-safe: one thread renders.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "frame.h"

namespace amped3::native {

// GLSL -> SPIR-V. `name` identifies the shader (for caches and logs).
// Returns false and sets *error on failure.
using ShaderCompiler = std::function<bool(bool fragment, const std::string& name,
                                          const std::string& glsl, std::vector<uint32_t>* spirv,
                                          std::string* error)>;

struct RendererOptions {
  int device_index = -1;  // -1: first discrete / integrated GPU
  ShaderCompiler compile;
  // Texture cache: decoded textures stay on the GPU up to this much memory
  // (MB; at most half the GPU's local memory), least recently used freed
  // first, and any unused for texture_keep_frames frames are freed.
  uint32_t texture_budget_mb = 1536;
  uint32_t texture_keep_frames = 36000;
  // Multisampled render targets as the guest sets them up (off: one sample,
  // the guest's sample masks ignored, as the SDK's emulation does).
  bool msaa = true;
  // Diagnostics: multiplies the polygon offset (constant and slope).
  float depth_bias_scale = 1.0f;
  // Diagnostics, comma-separated: "nobias" (no depth bias in pipelines),
  // "clamp" (depth clamp on), "list" (non-indexed triangle strips as lists).
  std::string diag;
  // Draw-range diagnostics ("show": colour writes on, depth/stencil tests off;
  // "nodepth": depth test off, "novtex", "vtexplain", "posdump": vertex
  // positions stored, see ReadDebugStores) apply to draws
  // diag_first..diag_last (1-based).
  uint32_t diag_first = 0, diag_last = 0;
  // Pick the physical device with this deviceUUID (VkPhysicalDeviceIDProperties),
  // so output images can be shared with the presenter's device.
  bool match_device_uuid = false;
  // Render returns once the frame is submitted, without waiting for the GPU
  // (two frames in flight; WaitFrame / ReadOutput per frame ticket). The game
  // uses this; the replayer waits.
  bool async = false;
  // Internal resolution: render targets and resolved images this many times
  // the guest's size in each direction (1 = native 720p), 0.25 .. 4, snapped
  // to multiples of 1/80 (16:9 sizes in multiples of 80 stay whole pixels).
  float resolution_scale = 1.0f;
  // The 3D scene is drawn at resolution_scale; the rest (composite, HUD,
  // menus) at output_scale (0: resolution_scale too). Temporal upscaler
  // (UpscalerKind: 0 none, 1 XeSS, 2 FSR): rebuilds the scene at
  // output_scale; without one the composite stretches it.
  int upscaler = 0;
  float output_scale = 0.0f;
  // Anisotropic filtering: -1 = as the game asks; 0 = off; 1..5 = up to 1x,
  // 2x, 4x, 8x, 16x on every mipmapped texture (the SDK's anisotropic_override).
  int anisotropy_override = -1;
  uint8_t device_uuid[16] = {};
  // Built pipelines kept on disk (a VkPipelineCache's data): loaded at start,
  // written back every minute while new ones are built and when the renderer
  // is destroyed, so a pipeline is compiled once rather than each session.
  // Empty: none.
  std::string pipeline_cache_path;
  // The game's cloud sprite (the pause menu's puffs), kept once the game has
  // used it so the map clouds can use it from the start of later sessions.
  // Empty: next to pipeline_cache_path (none if that's empty too).
  std::string cloud_sprite_path;
  // Scene draws whose pipeline isn't built yet, once a frame has spent a few
  // milliseconds building pipelines: built in the background, the draw left
  // out until then (a frame or two) instead of the frame stalling.
  bool async_pipelines = false;
  // Multisampled targets: alpha-tested and alpha-to-coverage surfaces
  // (foliage) get anti-aliased, dithered coverage from the shader instead of
  // the hardware's few coverage steps and a hard alpha test.
  bool smooth_alpha = true;
  // With smooth_alpha: cut-out scene surfaces (foliage) close to the camera
  // and near the middle of the screen fade out smoothly (dithered coverage),
  // keeping the rider and the way ahead in view.
  bool foliage_fade = true;
  // Texture replacements (texture_replace.h): PNGs from texture_pack_folder
  // named <name>_<hash>.png, and the game's fonts rebuilt at 4x.
  bool texture_pack = false;
  std::string texture_pack_folder;
  bool hd_fonts = false;
  // The upscaler's output (texture_pack_folder/upscaled), for textures
  // nothing else replaces.
  bool upscaled_textures = false;
};

// The picture to show: the front buffer with the guest's gamma ramp applied,
// in the presenters' guest output format (A2B10G10R10, red in the low bits).
struct OutputInfo {
  uint32_t width = 0, height = 0;
  // Shared output (DrawOutput with share = true): image `slot` of
  // kOutputSlots, whose memory another Vulkan device on the same GPU imports
  // (dedicated allocation of `memory_size` bytes, COLOR_ATTACHMENT |
  // TRANSFER_SRC usage, optimal tiling). `handle` is a Windows NT handle the
  // renderer keeps owning, or elsewhere a new file descriptor the importer
  // takes over; both change only with `generation`. The image is left in
  // VK_IMAGE_LAYOUT_GENERAL, released to VK_QUEUE_FAMILY_EXTERNAL, and finished
  // when DrawOutput returns.
  uint32_t slot = 0;
  uint64_t generation = 0;
  uint64_t handle = 0;
  uint64_t memory_size = 0;
};

// An image the renderer owns; valid until the next Render call.
struct RenderedImage {
  uint32_t width = 0, height = 0;
  std::string name;
  const void* handle = nullptr;  // opaque
  // FinalImage: the picture of the swapped front buffer (false: only the last
  // render target drawn, as a stand-in).
  bool front = false;
};

struct RenderStats {
  uint32_t draws = 0, draws_done = 0;
  std::map<std::string, uint32_t> skipped;  // reason -> count
  std::string details;                      // first few occurrences per reason
  std::vector<std::string> notices;         // worth a line in the game's log
  // CPU time (ms): vertex data and per-frame setup, recording the draws, and
  // waiting for the GPU to finish (0 when async).
  double prepass_ms = 0, record_ms = 0, gpu_wait_ms = 0;
  // Sampled frames (every 30th): the draws' CPU time by part (ms, all draws):
  // setup (shaders, resolve bookkeeping), render targets, textures, pipeline
  // lookup, uniforms, texture descriptor sets, indices, Vulkan commands,
  // after (motion blur copies).
  bool draw_parts_sampled = false;
  double draw_part_ms[9] = {};
  // Async: identifies the frame for WaitFrame / ReadOutput.
  uint64_t ticket = 0;
  // An output was requested and drawn (Render's `output`).
  bool output_drawn = false;
  // The upscaler in use ("XeSS 3.0.2", "FSR 3.1.4"), or why it isn't.
  std::string upscaler;
  // The guest's front buffer size (guest pixels; 0 = none this frame).
  uint32_t guest_output_width = 0, guest_output_height = 0;
  // Motion blur applied to the scene this frame (SetMotionBlur).
  uint32_t motion_blurred = 0;
  // Speed blur applied to the scene this frame (SetSpeedBlur).
  uint32_t speed_blurred = 0;
  // Distance fog applied to the scene this frame (SetFog).
  uint32_t fogged = 0;
  uint32_t god_rays = 0;  // god rays added this frame (SetGodRays)
  // The nearest character to the camera (world units; 1e9: none).
  float nearest_character = 1e9f;
  // Object motion blur: scene draws given motion vectors, and of those the
  // ones matched to a draw in the previous frame (the rest use the camera's).
  uint32_t velocity_draws = 0, velocity_matched = 0;
  // ... and of those, the player character's (exempt from object blur).
  uint32_t velocity_player = 0;
  // First-use work this frame (what makes a frame hitch): textures decoded
  // and uploaded, pipelines and shaders built, and the CPU time each took.
  uint32_t new_textures = 0, new_pipelines = 0, new_shaders = 0;
  // Pipelines sent to the background builders, and draws left out this frame
  // while theirs is being built (RendererOptions::async_pipelines).
  uint32_t deferred_pipelines = 0, draws_waiting = 0;
  double texture_ms = 0, pipeline_ms = 0, shader_ms = 0;
  double texture_mb = 0;
  // Textures dropped from the cache this frame, and the cache's size.
  uint32_t textures_evicted = 0, textures_cached = 0;
  // The vertex data arena started over this frame (it was full: waits for
  // the GPU), and its size.
  uint32_t arena_resets = 0;
  uint32_t texture_sets_written = 0;  // texture descriptor sets written this frame
  uint32_t texture_set_resets = 0;    // cached texture sets dropped (a view they use destroyed)
  double arena_mb = 0;
  double texture_cache_mb = 0;
  // Render passes begun, and clears: done as a pass of their own, folded
  // into the next pass that draws to the image (its load op), or done inside
  // the pass already drawing to it (SetMergeClears).
  uint32_t passes = 0, clear_passes = 0, clears_merged = 0, clears_in_pass = 0;
};

// The GPU time of a frame's parts (Renderer::GpuParts), in order: what ran
// up to each resolve (named by the render target it copies from), and the
// output passes.
struct GpuPart {
  std::string name;
  double ms = 0;
};

class Renderer {
 public:
  // Creates the Vulkan device. Throws std::runtime_error on failure.
  explicit Renderer(RendererOptions options);
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  const std::string& device_name() const;

  // What Render should also draw: the presentable output (see DrawOutput).
  struct OutputRequest {
    const uint8_t* lut = nullptr;  // 3 x 256, or nullptr
    bool share = false;            // into shared slot `slot`; else for ReadOutput
    uint32_t slot = 0;
  };
  // Draws the frame and, unless async, waits for the GPU. `max_draws` stops
  // early (replay debugging). With `output`, the output pass is recorded into
  // the same submission when the frame has a front-buffer picture
  // (stats.output_drawn, *info). Throws std::runtime_error on Vulkan errors.
  RenderStats Render(const Frame& frame, FrameResources& resources,
                     uint32_t max_draws = UINT32_MAX, const OutputRequest* output = nullptr,
                     OutputInfo* info = nullptr);
  // Motion blur for the following frames: each pixel is blurred along its
  // screen motion since the previous frame times `scale` (0 = off). `camera`:
  // the camera's motion (from the depth; the rider and nearby snow excluded).
  // `objects`: each object's own motion, from its vertex shader run with this
  // and last frame's constants (with `camera` too: the whole motion on screen).
  // Needs the game's deferred lighting pass in two consecutive frames.
  // `debug` shows the motion (red/green, x/y) and object pixels (blue) instead.
  void SetMotionBlur(float scale, bool camera = true, bool objects = false, bool debug = false);
  // Object blur on the scenery drawn before the lighting pass too (moving
  // parts of the level), not only the characters drawn after it. Costs a
  // few ms of CPU per frame: every scene draw is matched and drawn again.
  void SetSceneryObjectBlur(bool on);
  // Clears folded into the next render pass on the image (its load op) or
  // done inside the pass drawing to it, instead of a pass of their own (fewer
  // passes; tile-based GPUs such as Adreno avoid writing and reading back the
  // whole image). Applies from the next frame.
  void SetMergeClears(bool on);
  // The frame's Vulkan commands recorded on a thread of their own while the
  // renderer prepares the next draws (cmd_stream.h). From the next frame.
  void SetParallelCommands(bool on);
  // Upscaler inputs for the following frames: the time since the last frame
  // (ms) and FSR's sharpening (0..1).
  void SetUpscaleParams(float frame_ms, float sharpness);
  // Speed blur for the following frames: a radial blur toward the direction
  // of travel at the edges of the 3D scene, fading in with the camera's speed
  // (it follows the rider). `strength` scales it (0 = off); `dt` is the time
  // since the last frame (s). `force_amount` >= 0 fixes the speed's share
  // (0..1) instead (diagnostics).
  void SetSpeedBlur(float strength, float dt, float force_amount = -1.0f);
  // Distance fog for the following frames: height fog in the sky's color,
  // lit toward the sun, from `start` world units (metres) away; `density`
  // scales it (0 = off). Fades out without a rider near the camera.
  void SetFog(float density, float start);
  // God rays for the following frames: light shafts from the sky around the
  // sun (or moon) while it's in or near view, blocked by the scenery;
  // `strength` scales them (0 = off). Fade out without a rider nearby.
  void SetGodRays(float strength);
  // Foliage camera fade for the following frames (RendererOptions::foliage_fade).
  void SetFoliageFade(bool on);
  // Cartoon clouds that part when the view cuts to or from the trail map
  // (the fog fading with them).
  void SetMapClouds(bool on);
  // Whether the game has its trail map open (game state 7), each frame: the
  // map clouds play only for cuts to or from it (not, say, the camera cut of a
  // challenge restart). Never called (the replayer): any riding <-> overview
  // cut plays them.
  void SetTrailMapOpen(bool open);
  // Whether the game is playing a cutscene (game state 20), each frame: no map
  // clouds then (a scene's camera, far from the rider after a video, would
  // otherwise read as loading into the map).
  void SetInCutscene(bool scene);
  // Draw only the 3D scene (and its composite): the game's HUD and menus,
  // everything after the composite reads the finished scene, are left out.
  // For looking at the graphics while the port's Graphics panel is open.
  void SetHideInterface(bool hide);
  // The game state machine's state (0x828901E0 + 8), each frame: the map
  // clouds' reveal after a loading screen plays only on the trail map (7),
  // never over a video, a cutscene or a riding shot. -1: unknown (replayer).
  void SetGameState(int state);
  // Async: waits until the GPU has finished frame `ticket`; returns the CPU
  // time waited (ms).
  double WaitFrame(uint64_t ticket);
  // The GPU time frame `ticket` took, start to end (ms), after WaitFrame; -1
  // when the device has no timestamps or the frame is gone.
  double GpuFrameMs(uint64_t ticket);
  // The GPU time of the frame's parts (empty if not measured). After WaitFrame.
  std::vector<GpuPart> GpuParts(uint64_t ticket);
  // The packed output of frame `ticket` (non-shared output request), after
  // WaitFrame. False if that frame is no longer available.
  bool ReadOutput(uint64_t ticket, std::vector<uint8_t>* packed);

  // The last frame's front buffer (Swap): what a resolve put there, this frame
  // or an earlier one, else its texture decoded from memory (a front buffer
  // the CPU filled); failing those, the last render target drawn to (front =
  // false). Empty name if none.
  RenderedImage FinalImage() const;
  // Every render target and resolved image (replay --dump).
  std::vector<RenderedImage> AllImages() const;

  static constexpr uint32_t kOutputSlots = 3;
  // Whether output images can be exported to another Vulkan device.
  bool CanShareOutput() const;
  // Draws `src` (normally FinalImage()) through `lut` (3 x 256 bytes: red,
  // green, blue; nullptr: none) into an output image. share: into shared slot
  // `slot` (the caller makes sure the importer has finished reading it), and
  // fills *info; else reads the packed pixels back into *packed (width *
  // height * 4 bytes, top row first). Waits for the GPU.
  bool DrawOutput(const RenderedImage& src, const uint8_t* lut, bool share, uint32_t slot,
                  OutputInfo* info, std::vector<uint8_t>* packed);

  // RGBA8, top row first. Depth images come out as grey levels.
  bool ReadRGBA(const RenderedImage& image, std::vector<uint8_t>* rgba);
  // Depth images only: the stencil as a picture (red = bit 0, green = bit 1,
  // blue = higher bits).
  bool ReadStencil(const RenderedImage& image, std::vector<uint8_t>* rgba);
  // Depth images only: the depth values as floats (0..1).
  bool ReadDepth(const RenderedImage& image, std::vector<float>* depth);
  // Diagnostics ("posdump" with diag_first..diag_last): per vertex index
  // (first 4096), 4 x vec4: position before and after the viewport transform,
  // (vertex index, guest index, fetch dwords 190/191 as float bits), r10.
  // Unwritten entries are NaN.
  bool ReadDebugStores(std::vector<float>* values);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace amped3::native
