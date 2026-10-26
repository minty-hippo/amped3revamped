// amped3 - in-game options menu. See options_menu.h.

#include "options_menu.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/input/input.h>
#include <rex/ui/window.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "challenges.h"
#include "cutscenes.h"
#include "live_renderer.h"
#include "pad_bridge.h"

namespace amped3 {

namespace {

using rex::cvar::Lifecycle;
namespace xi = rex::input;

// ---------------------------------------------------------------------------
// Option tables
// ---------------------------------------------------------------------------

enum class Kind { kToggle, kChoice, kSliderInt, kSliderFloat };

struct Choice {
  std::string value;
  std::string label;
};

struct Option {
  const char* cvar;
  const char* label;
  const char* help;
  Kind kind;
  std::vector<Choice> choices;  // kChoice; empty = the cvar's allowed values
  double min = 0;               // sliders; min == max = the cvar's own range
  double max = 0;
  const char* format = nullptr;
  bool restart = false;  // needs a relaunch even though the SDK applies it live
  // Integer sliders: the text shown for a value (instead of `format`).
  std::string (*value_text)(int) = nullptr;
  // Shown only when this returns true (nullptr: always).
  bool (*visible)() = nullptr;
};

struct Section {
  const char* title;
  std::vector<Option> options;
  bool collapsible = false;  // a header that opens and closes (closed at first)
};

struct Tab {
  const char* name;
  std::vector<Section> sections;
  const char* footnote = nullptr;
  // Custom contents instead of sections; returns true to close the menu.
  bool (*draw)() = nullptr;
};

Option Toggle(const char* cvar, const char* label, const char* help, bool restart = false) {
  return Option{cvar, label, help, Kind::kToggle, {}, 0, 0, nullptr, restart};
}
Option Choices(const char* cvar, const char* label, const char* help, std::vector<Choice> choices,
               bool restart = false) {
  return Option{cvar, label, help, Kind::kChoice, std::move(choices), 0, 0, nullptr, restart};
}
Option SliderI(const char* cvar, const char* label, const char* help, int min, int max,
               const char* format) {
  return Option{cvar, label, help, Kind::kSliderInt, {}, double(min), double(max), format, false};
}
Option SliderF(const char* cvar, const char* label, const char* help, const char* format,
               double min = 0, double max = 0) {
  return Option{cvar, label, help, Kind::kSliderFloat, {}, min, max, format, false};
}

bool NativeRendererOn() { return rex::cvar::GetFlagByName("native_renderer") == "true"; }
bool NativeRendererOff() { return !NativeRendererOn(); }
bool NativeUpscalerOn() {
  return NativeRendererOn() && rex::cvar::GetFlagByName("native_upscaler") != "off";
}
bool NativeFsrOn() {
  return NativeRendererOn() && rex::cvar::GetFlagByName("native_upscaler") == "fsr";
}

// "75%  (1920 x 1080)": the native renderer's internal resolution at a step.
std::string RenderScaleText(int percent) {
  uint32_t w = 0, h = 0;
  amped3::live::RenderScaleFor(percent, &w, &h);
  return fmt::format("{}%  ({} x {})", percent, w, h);
}

Option NativeRenderScale() {
  Option o = SliderI("native_render_scale", "Render scale",
                     "The resolution the game's 3D scene is drawn at, as a percentage of the "
                     "picture on your screen (the window, or its 16:9 part with Keep 16:9 "
                     "aspect): 100% draws every pixel you see, lower is faster and softer. The "
                     "HUD, menus and text are always drawn at full resolution. With an Upscaler "
                     "chosen below, this is the resolution it starts from and it rebuilds the "
                     "full-size picture (67% is its Quality mode, 59% Balanced, 50% "
                     "Performance); without one the scene is stretched to fit. The resolution "
                     "it gives is shown on the slider and follows the window size. Applies at "
                     "once.",
                     25, 100, "%d%%");
  o.value_text = RenderScaleText;
  o.visible = NativeRendererOn;
  return o;
}

// Settings appear only if this build of the SDK registers the cvar, so the
// menu never offers something that does nothing (e.g. FSR on a build without
// FidelityFX).
const std::vector<Tab>& Tabs() {
  static const std::vector<Tab> tabs = {
      {"Display",
       {
           {"Screen",
            {
                Toggle("fullscreen", "Fullscreen",
                       "Borderless fullscreen on the current monitor."),
                Choices("resolution", "Output resolution",
                        "The resolution of the picture, the window size, and the video mode the "
                        "game is told it is running at. With the native renderer this applies at "
                        "once: the picture (HUD and menus, and the 3D scene at Render scale "
                        "100%) is made at this size, windowed or fullscreen, then scaled to the "
                        "screen; Native makes it at the window's own size. The game's video "
                        "mode changes at the next launch; Amped 3 was made for 720p, so if "
                        "menus or the HUD look misplaced, go back to Native.",
                        {{"", "Native (window size)"},
                         {"1080p", "1920 x 1080"},
                         {"1440p", "2560 x 1440"},
                         {"4k", "3840 x 2160"}}),
                Toggle("present_letterbox", "Keep 16:9 aspect",
                       "Black bars instead of stretching when the window or monitor is not 16:9."),
                Choices("frame_rate_limit", "Frame rate limit",
                        "Auto is 60 fps with Revamped Graphics and 30 without. 30 is the "
                        "console's own pacing, locked to your monitor (gameplay at 30, menus at "
                        "60). The others pace the game on the PC; gameplay runs at normal speed "
                        "at any frame rate. Unlocked runs as fast as your PC allows. Also a "
                        "slider in the game's Pause > Options > Graphics (Display tab).",
                        {{"auto", "Auto (60 with Revamped Graphics)"},
                         {"30", "30 fps (console)"},
                         {"40", "40 fps"},
                         {"50", "50 fps"},
                         {"60", "60 fps"},
                         {"72", "72 fps"},
                         {"90", "90 fps"},
                         {"120", "120 fps"},
                         {"144", "144 fps"},
                         {"0", "Unlocked"}}),
                Choices("vblank_source", "Frame pacing",
                        "Monitor: the game's 60 Hz clock follows your monitor's real refresh, so "
                        "every frame is shown for exactly one refresh (120 Hz monitors use every "
                        "second refresh). Fixed timer: the old free-running 60 Hz clock, which "
                        "drifts against the monitor and repeats or drops a frame every few "
                        "seconds. Monitors at other rates (75, 144 Hz) always use the timer.",
                        {{"display", "Monitor (smooth)"}, {"timer", "Fixed 60 Hz timer"}}),
            }},
           {"Image quality",
            {
                NativeRenderScale(),
                [] {
                  Option o = Choices(
                      "native_upscaler", "Upscaler",
                      "Rebuilds a full-resolution picture from the Render scale using several "
                      "frames, the game's depth and its motion: sharper than a filter at the same "
                      "cost. Intel XeSS uses the XMX cores on Intel Arc GPUs and runs on other "
                      "GPUs through DP4a (Windows only: Intel has no Linux XeSS library). AMD "
                      "FSR 3.1 runs on any GPU (FSR 4's machine-learning upscaler is Direct3D "
                      "12 only); on Linux setup.sh builds it from AMD's FidelityFX SDK sources. "
                      "The HUD is drawn afterwards at full resolution. Leave the Upscaling "
                      "filter on Bilinear. If the library is missing or the GPU can't run it, "
                      "the picture is simply stretched and the log says why. Native renderer "
                      "only.",
#ifdef _WIN32
                      {{"off", "Off"}, {"xess", "Intel XeSS"}, {"fsr", "AMD FSR 3.1"}});
#else
                      // Intel ships XeSS for Windows only.
                      {{"off", "Off"}, {"fsr", "AMD FSR 3.1"}});
#endif
                  o.visible = NativeRendererOn;
                  return o;
                }(),
                [] {
                  Option o = SliderF("native_upscaler_sharpness", "FSR sharpness",
                                     "Sharpening applied by AMD FSR 3.1 (0 = none).", "%.2f",
                                     0.0, 1.0);
                  o.visible = NativeFsrOn;
                  return o;
                }(),
                [] {
                  Option o = Toggle(
                      "native_upscaler_msaa", "MSAA with the upscaler",
                      "The game's multisample anti-aliasing while FSR or XeSS is on. Off by "
                      "default: the upscaler smooths the edges itself, and one sample per "
                      "pixel keeps the same lighting and colors while saving GPU time. "
                      "(Without an upscaler, Multisampling (MSAA) under Native renderer "
                      "decides.)");
                  o.visible = NativeUpscalerOn;
                  return o;
                }(),
                [] {
                  Option o = SliderI(
                      "resolution_scale", "Render scale",
                      "Emulated renderer: draws the game's 3D at a multiple of its resolution: "
                      "2x is 1440p from 720p, 3x is 4K. Costs GPU time and needs a restart; if "
                      "bloom, shadows or depth of field look shifted, go back to 1x. (The native "
                      "renderer has its own render scale, shown while it is on.)",
                      1, 3, "%dx");
                  o.visible = NativeRendererOff;
                  return o;
                }(),
                Choices("present_effect", "Upscaling / sharpening",
                        "Filter used to fit the image to your screen. Integer scaling shows the "
                        "picture at the largest whole multiple of its size that fits, with sharp "
                        "square pixels and black borders (use it with Render scale 1x: a 720p "
                        "picture is 1x on a 1080p screen, 2x at 1440p, 3x at 4K). CAS sharpens; "
                        "FSR upscales with sharpening and works best together with Render scale "
                        "1x on high resolution screens.",
                        {}),
                SliderF("present_cas_additional_sharpness", "CAS sharpness",
                        "Extra sharpening when the filter is AMD CAS.", "%.2f"),
                SliderF("present_fsr_sharpness_reduction", "FSR softness",
                        "Higher is softer. Only used when the filter is AMD FSR.", "%.2f"),
            }},
       }},
      {"Graphics",
       {
           {"Renderer",
            {
                Choices("gpu_backend", "Graphics API",
                        "Which API the Xbox 360 GPU emulation and the window use. Automatic "
                        "picks Vulkan while the native renderer draws (its picture then reaches "
                        "the window without a trip through the CPU) and, on Windows, "
                        "Direct3D 12 when the emulation draws. Falls back to the default if the "
                        "chosen API isn't available.",
#ifdef _WIN32
                        {{"auto", "Automatic"}, {"d3d12", "Direct3D 12"}, {"vulkan", "Vulkan"}}),
#else
                        {{"auto", "Automatic"}, {"vulkan", "Vulkan"}}),
#endif
                Choices("render_target_path_vulkan", "Vulkan render path",
                        "How Vulkan emulates the console's on-chip framebuffer. Render targets "
                        "(Automatic) is much faster. Shader interlock is more exact for blending "
                        "and format changes but costs a lot of GPU time; try it only if a "
                        "surface looks wrong on Vulkan but right on Direct3D 12.",
                        {{"", "Render targets (default)"},
                         {"fsi", "Shader interlock (accurate)"}}),
                Toggle("allow_screen_tearing", "Allow screen tearing (Vulkan)",
                       "Shows each frame the instant it's ready instead of at the next screen "
                       "refresh. Slightly lower input lag, but causes tear lines. Off is "
                       "recommended."),
                Toggle("gamma_render_target_as_unorm16", "High-precision gamma blending",
                       "Stores gamma-corrected surfaces at 16 bits per channel when using "
                       "Render targets. Turn off if lighting or fog looks too dark.",
                       /*restart=*/true),
            }},
           {"Textures",
            {
                Choices("anisotropic_override", "Anisotropic filtering",
                        "Keeps snow and slope textures sharp at grazing angles. 16x costs very "
                        "little on a modern GPU.",
                        {{"-1", "Game default"},
                         {"0", "Off"},
                         {"1", "1x"},
                         {"2", "2x"},
                         {"3", "4x"},
                         {"4", "8x"},
                         {"5", "16x"}}),
            }},
           {"Shaders and accuracy",
            {
                Toggle("async_shader_compilation", "Background shader compilation",
                       "Avoids hitches the first time an effect appears, at the cost of that "
                       "effect being missing for a moment. Turn off if you see flicker or hangs. "
                       "With the native renderer: scenery whose pipeline isn't built yet appears "
                       "a frame or two later instead of the frame stalling (built pipelines are "
                       "kept on disk, so this happens once)."),
                Choices("readback_resolve", "GPU readback",
                        "Lets the game read back images the GPU drew. Leave off unless parts of "
                        "the HUD, menus, photos or reflections are missing or wrong.",
                        {{"none", "Off (fastest)"},
                         {"fast", "Fast (one frame late)"},
                         {"some", "Balanced"},
                         {"full", "Full (most accurate)"}}),
            }},
           {"Native renderer",
            {
                Toggle("native_renderer", "Use the native renderer",
                       "Draws each frame with the port's own Vulkan renderer instead of the "
                       "Xbox 360 GPU emulation (on by default). Once its image is up, the "
                       "emulation stops drawing. Turn it off if anything looks wrong; problems "
                       "are written to the log. While it's on, the emulation starts without "
                       "its shader cache and resolution scaling (see Lean emulation below), so "
                       "switching it off fully applies after a restart."),
                Toggle("emulated_gpu_lean", "Lean emulation",
                       "With the native renderer on at launch, the Xbox 360 GPU emulation only "
                       "follows the game's commands, so it starts without loading its shader "
                       "cache and without resolution scaling: faster start-up and less memory "
                       "(most on handhelds, where graphics memory is system memory). Turn off "
                       "if you switch between the renderers often.",
                       /*restart=*/true),
                Toggle("revamped_graphics", "Revamped Graphics",
                       "All the visual enhancements below at once (motion blur, speed blur, "
                       "ambient occlusion, distance fog, god rays, branch fade and the trail "
                       "map clouds), each "
                       "as set here. Off draws the game as the console does. Also in the "
                       "game's own menu: Pause > Options > Graphics. Native renderer "
                       "only."),
                Toggle("native_keep_emulation", "Keep the emulation drawing",
                       "Lets the GPU emulation keep drawing every frame underneath the native "
                       "renderer, as in earlier builds. Slower; only useful for comparing the "
                       "two."),
                Toggle("native_msaa", "Multisampling (MSAA)",
                       "Draws with the game's own multisample anti-aliasing, as the Xbox 360 "
                       "does: smoother edges. Off draws one sample per pixel with the same "
                       "lighting and colors (the game's per-sample lighting and see-through "
                       "foliage are reproduced), a little faster. Applies without an "
                       "upscaler; with FSR or XeSS on, MSAA with the upscaler (Image "
                       "quality) decides."),
                Toggle("native_sharp_fonts", "Sharp fonts",
                       "The game's fonts (menus, HUD, scores, button icons) are 720p bitmaps. "
                       "This rebuilds each at 4x from its own letters when first shown, with "
                       "smooth, anti-aliased edges, so text stays sharp at 1080p, 1440p and 4K. "
                       "Off by default; Revamped Graphics doesn't turn it on. Native renderer "
                       "only."),
                Toggle("native_texture_pack", "Texture pack",
                       "Uses the PNGs in the textures folder next to the exe in place of the "
                       "game's textures (see textures\\README.txt to add your own). Comes with "
                       "the new Amped 3 logo for the start-up and title screens. Native "
                       "renderer only."),
                Toggle("native_upscaled_textures", "Upscaled textures",
                       "Uses the game's textures upscaled on this PC (the installer's optional "
                       "texture upscaling step, into textures\\upscaled) wherever the texture "
                       "pack and Sharp fonts don't replace one. Off by default; Revamped "
                       "Graphics doesn't turn it on. Native renderer only."),
                Toggle("native_smooth_foliage", "Smooth foliage edges",
                       "Tree branches, fences and other cut-out textures get smooth, "
                       "anti-aliased edges and stay solid as on the console, instead of the "
                       "see-through, stippled look a PC graphics card gives them. Needs "
                       "Multisampling (MSAA). Native renderer only."),
                Toggle("native_foliage_fade", "Fade branches near the camera",
                       "Tree branches between the camera and your rider fade out smoothly, "
                       "most toward the middle of the screen, so your rider and the way ahead "
                       "stay in view, and trees the game fades near the camera turn evenly "
                       "see-through instead of breaking into ragged holes. Needs Smooth "
                       "foliage edges. Native renderer only."),
                Toggle("native_map_clouds", "Trail map cloud transition",
                       "Choosing the trail map from the pause menu, the pause menu's cartoon "
                       "clouds close in, then disperse from the middle out over the map (loading "
                       "into a level, they just disperse); leaving it, they close in over the map "
                       "and part over the slope, the distance fog thinning out (or back in) as "
                       "they go. Uses the "
                       "game's own cloud art once the pause menu has shown it (kept for later "
                       "sessions); look-alikes until then. Native renderer only."),
                Toggle("native_fog", "Distance fog",
                       "Haze that builds with distance and settles in the valleys, so far "
                       "mountains fade into the sky. It is lit by the sky and the sun (or "
                       "moon) wherever you look, glowing in the sun's color most toward it, so "
                       "it follows each mountain's time of day. Nearby snow, your rider and "
                       "the HUD stay clear; it fades out on the trail map (with the cloud transition, as "
                       "the clouds part). Native renderer "
                       "only."),
                SliderF("native_fog_density", "Fog density",
                        "How thick the distance fog is (1: about half hazed 2 km away).", "%.2f",
                        0.0, 3.0),
                SliderF("native_fog_start", "Fog start distance",
                        "How far away the fog begins, in metres.", "%.0f m", 0.0, 1000.0),
                Choices("native_shadow_resolution", "Shadow resolution",
                        "The sun's shadow map (riders, rails, jumps and objects; the trees' and "
                        "mountains' shadows are painted into the slopes), drawn larger than the "
                        "game's 1280 x 720: sharper, more detailed shadow edges. Independent of "
                        "the render resolution. Native renderer only.",
                        {{"100", "1x (1280 x 720, the game's)"},
                         {"150", "1.5x (1920 x 1080)"},
                         {"200", "2x (2560 x 1440)"},
                         {"300", "3x (3840 x 2160)"},
                         {"400", "4x (5120 x 2880)"}}),
                Choices("native_shadow_distance", "Shadow distance",
                        "How far ahead the sun's shadows reach. The game's map covers about "
                        "40 m of the slope ahead; farther spreads it thinner (raise Shadow "
                        "resolution with it). Only what the game draws into its shadow map "
                        "casts shadows, so far away that's mostly riders, rails and objects. "
                        "Native renderer only.",
                        {{"0", "The game's (about 40 m)"},
                         {"75", "75 m"},
                         {"100", "100 m"},
                         {"150", "150 m"},
                         {"200", "200 m"},
                         {"300", "300 m"}}),
                Toggle("native_ambient_occlusion", "Ambient occlusion",
                       "Shading where the open sky's light can't reach: the snow around tree "
                       "trunks, rails, jumps and buildings, creases and corners, worked out "
                       "from the scene's depth each frame (within a few metres, none in the "
                       "far distance). Native renderer only."),
                SliderF("native_ambient_occlusion_strength", "Ambient occlusion strength",
                        "How dark the shading gets (0 = none).", "%.2f", 0.0, 2.0),
                Toggle("native_god_rays", "God rays",
                       "Shafts of light streaming from the sun (or moon) while it's in or "
                       "near view, broken up by the clouds, trees and peaks in front of it. "
                       "Native renderer only."),
                SliderF("native_god_rays_strength", "God ray strength",
                        "How bright the light shafts are (0 = none).", "%.2f", 0.0, 2.0),
                Toggle("native_vertex_cache", "Reuse unchanged vertex data",
                       "Copies the game's vertex buffers only when the game has written to "
                       "them, instead of every frame. Turn off if geometry ever looks stale "
                       "or out of place."),
            }},
           // Collapsed until opened: every blur effect in one place.
           {"Blur",
            {
                Toggle("native_camera_motion_blur", "Camera motion blur",
                       "Blurs the scenery as the camera turns and moves, the same amount at any "
                       "frame rate. The HUD stays sharp, and so do your rider and the snow right "
                       "around them. Native renderer only."),
                Toggle("native_object_motion_blur", "Object motion blur",
                       "Blurs other riders and characters along their own motion, by how fast "
                       "each one moves, with soft edges that trail past them. Your own rider "
                       "and board are never blurred. Together with camera motion blur, the "
                       "scenery blurs by how it moves on screen. "
                       "Costs some GPU time (the characters are drawn again). "
                       "Native renderer only."),
                SliderF("native_camera_motion_blur_strength", "Motion blur strength",
                        "How long the blur trails are, for camera and object blur. 1 = the "
                        "motion over 1/60 s (like a film camera at 30 fps); 0 = none.",
                        "%.2f", 0.0, 3.0),
                Toggle("native_speed_blur", "Speed blur",
                       "A radial blur at the edges of the screen that fades in once your rider "
                       "picks up speed and grows the faster you go, streaking toward where "
                       "you're heading. The middle of the screen, your rider and the HUD stay "
                       "sharp. Native renderer only."),
                SliderF("native_speed_blur_strength", "Speed blur strength",
                        "How strong the speed blur gets at top speed (0 = none).", "%.2f", 0.0,
                        2.0),
            },
            true},
       }},
      {"Audio",
       {
           {"Output",
            {
                Toggle("audio_mute", "Mute", "Silences all game audio."),
                Toggle("audio_force_stereo", "Stereo mix",
                       "Mixes the game's 5.1 sound down to left and right even when the output "
                       "says it has more speakers. Turn on if cutscene dialogue is missing: "
                       "some handheld and laptop speakers report 5.1 but only play the front "
                       "pair, which drops the center channel the dialogue is on."),
                SliderI("audio_maxqframes", "Audio buffer",
                        "Lower means less delay; raise it if you hear crackling.", 4, 64,
                        "%d frames"),
            }},
       }},
      {"Controls",
       {
           {"Controller",
            {
                Choices("input_backend", "Controller driver",
                        "SDL supports Xbox, PlayStation, Switch Pro and most other controllers, "
                        "with hot-plugging and the right button layout. XInput supports Xbox "
                        "controllers only.",
#ifdef _WIN32
                        {{"sdl", "SDL (all controllers)"}, {"xinput", "XInput (Xbox only)"}},
#else
                        {{"sdl", "SDL (all controllers)"}},
#endif
                        /*restart=*/true),
                Toggle("guide_button", "Send Guide button to game",
                       "Passes the Xbox / PS button through to the game. Usually left off so it "
                       "keeps opening Steam or Game Bar."),
            }},
           {"Keyboard and mouse",
            {
                Toggle("mnk_mode", "Keyboard as controller",
                       "Play with the keyboard. Keys are the keybind_* settings (F4 panel or "
                       "amped3.toml)."),
                Toggle("mnk_mouse", "Mouse controls the camera",
                       "Mouse acts as the right stick while keyboard mode is on."),
                SliderF("mnk_sensitivity", "Mouse sensitivity", "Right-stick speed from the mouse.",
                        "%.2f"),
            }},
       },
       "Open this menu: Back + Start (View + Menu) or F1.   Close: B or Esc.   Tabs: LB / RB."},
      {"Enhancements",
       {
           // "Game patches" is filled at draw time from cvars in the "Amped 3/Patches"
           // category (see README: Adding an enhancement).
           {"Game patches", {}},
           {"Frame rate tuning",
            {
                SliderF("sled_crash_score_compensation", "Sled crash score compensation",
                        "Above 30 fps the physics reports softer hits in sled crash mode. This "
                        "sets how much they are scaled back up (with \"Fix sled crash score "
                        "frame rate\" on): 0 = none, 1 = in proportion to the frame rate. "
                        "0.7 (default) matches 30 fps scoring. Raise it if high frame rates score too "
                        "little, lower it if too much.",
                        "%.2f", 0.0, 1.0),
            }},
           {"Experimental timing",
            {
                Choices("video_mode_refresh_rate", "Console refresh rate",
                        "Speeds up the virtual console's vertical-blank clock. A game that paces "
                        "itself by elapsed time can then render more frames; a game that counts "
                        "frames will play too fast. Go back to 60 Hz if gameplay speeds up.",
                        {{"60", "60 Hz (original)"}, {"120", "120 Hz"}, {"144", "144 Hz"}}),
            }},
           {"Performance",
            {
                Toggle("perf_trace", "Record performance trace",
                       "Records where each frame's time goes for a while (perf_trace_seconds, "
                       "20 s by default), then switches itself off and writes "
                       "logs/perf_<time>.csv, _threads.csv and _summary.txt. Close this menu and "
                       "play normally while it records. Also F11."),
                SliderI("perf_trace_seconds", "Trace length",
                        "How many seconds a performance trace records.", 5, 120, "%d s"),
            }},
       }},
      {"Cutscenes", {},
       "Plays the game's story cutscenes on demand, for testing. A scene interrupts the ride "
       "and returns to it when it ends or is skipped.",
       &cutscenes::DrawTab},
      {"Challenges", {},
       "Starts any challenge or story mission on the loaded mountain, including story missions "
       "already played.",
       &challenges::DrawTab},
  };
  return tabs;
}

constexpr const char* kPatchCategory = "Amped 3/Patches";

const char* EffectLabel(std::string_view v) {
  if (v == "bilinear") return "Bilinear (no sharpening)";
  if (v == "integer") return "Integer scaling (sharp pixels)";
  if (v == "cas") return "AMD CAS (sharpen)";
  if (v == "fsr") return "AMD FSR 1 (upscale + sharpen)";
  if (v == "fsr2") return "AMD FSR 2";
  if (v == "fsr3") return "AMD FSR 3";
  return nullptr;
}

// ---------------------------------------------------------------------------
// cvar helpers
// ---------------------------------------------------------------------------

bool ParseNumber(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end && *end == '\0';
}

bool SameValue(const std::string& a, const std::string& b) {
  if (a == b) return true;
  double x = 0, y = 0;
  return ParseNumber(a, x) && ParseNumber(b, y) && std::fabs(x - y) < 1e-6;
}

// Settings the SDK marks restart-only because the GPU emulation reads them at
// start-up, but that the native renderer applies at once (it rebuilds its
// targets at the new size). While the native renderer is on, the menu
// doesn't ask for a restart for them; the emulation picks the value up at
// the next launch.
bool LiveWithNativeRenderer(std::string_view name) {
  // Output resolution: the window is resized at once (ResizeWindowTo) and the
  // native renderer follows it; the game's own video mode changes at the
  // next launch.
  return name == "resolution" && NativeRendererOn();
}

// Resizes the window's client area to an Output resolution preset, as the
// SDK sizes it at start-up (logical pixels), kept within the
// monitor's work area at 16:9. Windowed only (fullscreen is the monitor's
// size). UI thread.
void ResizeWindowTo(rex::ui::Window* window, const std::string& value) {
#ifdef _WIN32
  // Native: the picture follows the window, whatever its size.
  if (!window || window->IsFullscreen() || value.empty()) return;
  HWND hwnd = static_cast<HWND>(window->GetNativeWindowHandle());
  if (!hwnd) return;
  int32_t w = 0, h = 0;
  if (!rex::graphics::video_mode_util::TryParseResolutionPreset(value, w, h) || w <= 0 || h <= 0) {
    return;
  }
  uint32_t cw = window->SizeToPhysical(uint32_t(w)), ch = window->SizeToPhysical(uint32_t(h));
  // The frame (borders, title bar) as it is now.
  RECT outer, client;
  GetWindowRect(hwnd, &outer);
  GetClientRect(hwnd, &client);
  const int frame_w = (outer.right - outer.left) - (client.right - client.left);
  const int frame_h = (outer.bottom - outer.top) - (client.bottom - client.top);
  MONITORINFO mi{sizeof(mi)};
  RECT work{0, 0, 1 << 16, 1 << 16};
  if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) work = mi.rcWork;
  const int max_w = (work.right - work.left) - frame_w;
  const int max_h = (work.bottom - work.top) - frame_h;
  if (max_w > 0 && max_h > 0 && (int(cw) > max_w || int(ch) > max_h)) {
    // Bigger than the screen: the largest that fits, same shape.
    const double f = std::min(double(max_w) / cw, double(max_h) / ch);
    cw = uint32_t(cw * f);
    ch = uint32_t(ch * f);
  }
  const int ww = int(cw) + frame_w, wh = int(ch) + frame_h;
  // Same top-left unless that would put part of it off the work area.
  int x =
      std::clamp(int(outer.left), int(work.left), std::max(int(work.left), int(work.right) - ww));
  int y =
      std::clamp(int(outer.top), int(work.top), std::max(int(work.top), int(work.bottom) - wh));
  SetWindowPos(hwnd, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
#else
  (void)window;
  (void)value;
#endif
}

bool NeedsRestart(const Option& o, const rex::cvar::FlagEntry& info) {
  if (LiveWithNativeRenderer(o.cvar)) return o.restart;
  return o.restart || info.lifecycle != Lifecycle::kHotReload;
}

// Names changed this session that need a relaunch but that the SDK treats as
// live (and so does not report through GetPendingRestartFlags).
bool g_emulated_gpu_lean = false;

std::set<std::string>& ForcedRestarts() {
  static std::set<std::string> names;
  return names;
}

std::string LabelFor(const std::string& cvar) {
  for (const auto& tab : Tabs())
    for (const auto& sec : tab.sections)
      for (const auto& o : sec.options)
        if (cvar == o.cvar) return o.label;
  return cvar;
}

std::string PrettyName(std::string_view name) {
  std::string out(name);
  std::replace(out.begin(), out.end(), '_', ' ');
  if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}

}  // namespace

void ApplyOutputResolution(rex::ui::Window* window, const std::string& value) {
  if (NativeRendererOn()) ResizeWindowTo(window, value);
}

// ---------------------------------------------------------------------------
// OptionsMenu
// ---------------------------------------------------------------------------

OptionsMenu::OptionsMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                         std::filesystem::path config_path, std::function<void()> on_closed)
    : ImGuiDialog(drawer),
      window_(window),
      config_path_(std::move(config_path)),
      on_closed_(std::move(on_closed)) {
  auto& io = GetIO();
  prev_config_flags_ = io.ConfigFlags;
  prev_backend_flags_ = io.BackendFlags;
}

OptionsMenu::~OptionsMenu() = default;

void OptionsMenu::FeedGamepad(ImGuiIO& io) {
  const pad::Snapshot s = pad::Latest();  // published by the app's watcher thread
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

  constexpr int kDeadzone = 14000;
  const uint16_t b = s.buttons;
  const bool up = (b & xi::X_INPUT_GAMEPAD_DPAD_UP) || s.ly > kDeadzone;
  const bool down = (b & xi::X_INPUT_GAMEPAD_DPAD_DOWN) || s.ly < -kDeadzone;
  const bool left = (b & xi::X_INPUT_GAMEPAD_DPAD_LEFT) || s.lx < -kDeadzone;
  const bool right = (b & xi::X_INPUT_GAMEPAD_DPAD_RIGHT) || s.lx > kDeadzone;

  // Only the keys the menu needs: A activates, B cancels, d-pad/left stick
  // move. X/Y/Start/Back are left out so ImGui's windowing mode never opens.
  io.AddKeyEvent(ImGuiKey_GamepadDpadUp, up);
  io.AddKeyEvent(ImGuiKey_GamepadDpadDown, down);
  io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, left);
  io.AddKeyEvent(ImGuiKey_GamepadDpadRight, right);
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (b & xi::X_INPUT_GAMEPAD_A) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (b & xi::X_INPUT_GAMEPAD_B) != 0);

  const uint16_t pressed = first_frame_ ? 0 : static_cast<uint16_t>(b & ~prev_buttons_);
  prev_buttons_ = b;

  const int tab_count = static_cast<int>(Tabs().size());
  if (pressed & xi::X_INPUT_GAMEPAD_LEFT_SHOULDER) {
    requested_tab_ = (current_tab_ + tab_count - 1) % tab_count;
  }
  if (pressed & xi::X_INPUT_GAMEPAD_RIGHT_SHOULDER) {
    requested_tab_ = (current_tab_ + 1) % tab_count;
  }
  // B closes the menu unless it is backing out of an open dropdown or an
  // active slider (ImGui handles those itself). Start alone also closes.
  const bool busy = popup_was_open_ || ImGui::IsAnyItemActive();
  if ((pressed & xi::X_INPUT_GAMEPAD_B) && !busy) {
    Close();
  }
  if ((pressed & xi::X_INPUT_GAMEPAD_START) && !(b & xi::X_INPUT_GAMEPAD_BACK)) {
    Close();
  }
}

void OptionsMenu::ReleaseGamepad(ImGuiIO& io) {
  for (ImGuiKey key : {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft,
                       ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown,
                       ImGuiKey_GamepadFaceRight}) {
    io.AddKeyEvent(key, false);
  }
  io.ConfigFlags = prev_config_flags_;
  io.BackendFlags = prev_backend_flags_;
}

void OptionsMenu::Save() {
  if (dirty_) {
    rex::cvar::SaveConfig(config_path_);
    dirty_ = false;
  }
}

void OptionsMenu::OnClose() {
  ReleaseGamepad(GetIO());
  Save();
  if (on_closed_) {
    on_closed_();
  }
}

void OptionsMenu::OnDraw(ImGuiIO& io) {
  FeedGamepad(io);
  if (!popup_was_open_ && !ImGui::IsAnyItemActive() &&
      ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    Close();
  }

  const ImVec2 display = io.DisplaySize;
  const ImVec2 size(std::min(display.x * 0.9f, std::max(720.0f, display.x * 0.5f)),
                    std::min(display.y * 0.9f, std::max(480.0f, display.y * 0.6f)));
  ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.94f);
  if (first_frame_) {
    ImGui::SetNextWindowFocus();
  }
  first_frame_ = false;

  const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
  if (!ImGui::Begin("Amped 3 Revamped  -  Options###amped3_options", nullptr, window_flags)) {
    ImGui::End();
    popup_was_open_ = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
    return;
  }

  std::string hovered_help;
  const float line = ImGui::GetTextLineHeightWithSpacing();
  const float footer_h = line * 4.5f + ImGui::GetFrameHeightWithSpacing();

  auto draw_option = [&](const Option& o) {
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(o.cvar);
    if (!info) {
      return;  // not in this SDK build
    }
    if (o.visible && !o.visible()) return;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(o.label);
    if (NeedsRestart(o, *info)) {
      ImGui::SameLine();
      ImGui::TextDisabled("(restart)");
    }
    ImGui::TableNextColumn();
    ImGui::PushID(o.cvar);
    ImGui::SetNextItemWidth(-FLT_MIN);

    bool changed = false;
    bool focused = false;
    std::string new_value;

    switch (o.kind) {
      case Kind::kToggle: {
        const bool is_fullscreen = std::string_view(o.cvar) == "fullscreen";
        bool v = is_fullscreen && window_ ? window_->IsFullscreen()
                                          : rex::cvar::Query<bool>(o.cvar);
        if (ImGui::Checkbox("##v", &v)) {
          changed = true;
          new_value = v ? "true" : "false";
          if (is_fullscreen && window_) {
            window_->SetFullscreen(v);
          }
        }
        focused = ImGui::IsItemFocused() || ImGui::IsItemHovered();
        ImGui::SameLine();
        ImGui::TextUnformatted(v ? "On" : "Off");
        break;
      }
      case Kind::kChoice: {
        std::vector<Choice> choices = o.choices;
        if (choices.empty()) {
          for (const auto& allowed : info->constraints.allowed_values) {
            const char* label = EffectLabel(allowed);
            choices.push_back({allowed, label ? label : allowed});
          }
        }
        const std::string current = rex::cvar::GetFlagByName(o.cvar);
        std::string preview = current.empty() ? std::string("Default") : current;
        for (const auto& c : choices) {
          if (SameValue(c.value, current)) {
            preview = c.label;
            break;
          }
        }
        const bool open = ImGui::BeginCombo("##v", preview.c_str());
        focused = ImGui::IsItemFocused() || ImGui::IsItemHovered();
        if (open) {
          for (const auto& c : choices) {
            const bool selected = SameValue(c.value, current);
            if (ImGui::Selectable(c.label.c_str(), selected)) {
              changed = !selected;
              new_value = c.value;
            }
            if (selected) {
              ImGui::SetItemDefaultFocus();
            }
          }
          ImGui::EndCombo();
        }
        break;
      }
      case Kind::kSliderInt: {
        int v = rex::cvar::Query<int32_t>(o.cvar);
        int lo = static_cast<int>(o.min), hi = static_cast<int>(o.max);
        if (lo == hi && info->constraints.HasRangeConstraint()) {
          lo = static_cast<int>(info->constraints.min.value_or(0));
          hi = static_cast<int>(info->constraints.max.value_or(lo + 1));
        }
        std::string text;
        if (o.value_text) {
          // The value's own text (ImGui prints the format with the value; a
          // literal % has to be doubled).
          for (char c : o.value_text(v)) {
            text += c;
            if (c == '%') text += '%';
          }
        }
        if (ImGui::SliderInt("##v", &v, lo, hi,
                             o.value_text ? text.c_str() : o.format ? o.format : "%d",
                             ImGuiSliderFlags_AlwaysClamp)) {
          changed = true;
          new_value = fmt::format("{}", v);
        }
        focused = ImGui::IsItemFocused() || ImGui::IsItemHovered();
        break;
      }
      case Kind::kSliderFloat: {
        float v = static_cast<float>(rex::cvar::Query<double>(o.cvar));
        double lo = o.min, hi = o.max;
        if (lo == hi) {
          lo = info->constraints.min.value_or(0.0);
          hi = info->constraints.max.value_or(1.0);
        }
        if (ImGui::SliderFloat("##v", &v, static_cast<float>(lo), static_cast<float>(hi),
                               o.format ? o.format : "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
          changed = true;
          new_value = fmt::format("{:.3f}", v);
        }
        focused = ImGui::IsItemFocused() || ImGui::IsItemHovered();
        break;
      }
    }
    ImGui::PopID();

    if (focused) {
      hovered_help = o.help ? o.help : info->description;
    }
    if (changed && rex::cvar::SetFlagByName(o.cvar, new_value)) {
      dirty_ = true;
      if (std::string_view(o.cvar) == "resolution" && NativeRendererOn()) {
        ResizeWindowTo(window_, new_value);
      }
      if (o.restart) {
        ForcedRestarts().insert(o.cvar);
      }
    }
  };

  auto draw_patches = [&]() {
    const auto names = rex::cvar::ListFlagsByCategory(kPatchCategory);
    if (names.empty()) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("None yet. Patches for this game are added as they are developed.");
      return;
    }
    for (const auto& name : names) {
      const auto* info = rex::cvar::GetFlagInfo(name);
      if (!info || info->type != rex::cvar::FlagType::Boolean) {
        continue;
      }
      // Patch cvars outlive this frame (they are registered statically), so
      // their name strings are stable for Option's const char* fields.
      Option o = Toggle(info->name.c_str(), nullptr, info->description.c_str());
      const std::string label = PrettyName(info->name);
      o.label = label.c_str();
      draw_option(o);
    }
  };

  const auto& tabs = Tabs();
  bool close_after_draw = false;
  if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTooltip)) {
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
      const Tab& tab = tabs[i];
      const ImGuiTabItemFlags flags = (requested_tab_ == i) ? ImGuiTabItemFlags_SetSelected : 0;
      if (!ImGui::BeginTabItem(tab.name, nullptr, flags)) {
        continue;
      }
      current_tab_ = i;
      ImGui::BeginChild("##body", ImVec2(0, -footer_h), ImGuiChildFlags_NavFlattened);
      if (tab.draw && tab.draw()) {
        close_after_draw = true;
      }
      for (const Section& section : tab.sections) {
        ImGui::Spacing();
        if (section.collapsible) {
          if (!ImGui::CollapsingHeader(section.title)) continue;
        } else {
          ImGui::SeparatorText(section.title);
        }
        if (ImGui::BeginTable(section.title, 2, ImGuiTableFlags_SizingStretchProp)) {
          ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
          ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
          if (std::string_view(section.title) == "Game patches") {
            draw_patches();
          }
          for (const Option& o : section.options) {
            draw_option(o);
          }
          ImGui::EndTable();
        }
      }
      if (tab.footnote) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("%s", tab.footnote);
      }
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  requested_tab_ = -1;

  // Footer: help for the focused setting, pending restarts, actions.
  ImGui::Separator();
  if (!hovered_help.empty()) {
    help_text_ = hovered_help;
  }
  ImGui::BeginChild("##help", ImVec2(0, line * 2.6f));
  ImGui::TextWrapped("%s", help_text_.c_str());
  ImGui::EndChild();

  std::set<std::string> restart_names(ForcedRestarts().begin(), ForcedRestarts().end());
  // The emulation was started lean for the native renderer: it draws now, but
  // at 1x and compiling shaders as it goes until the next launch.
  if (EmulatedGpuLean() && !NativeRendererOn()) restart_names.insert("native_renderer");
  for (const auto& name : rex::cvar::GetPendingRestartFlags()) {
    if (!LiveWithNativeRenderer(name)) restart_names.insert(name);
  }
  if (!restart_names.empty()) {
    std::string list;
    for (const auto& name : restart_names) {
      if (!list.empty()) list += ", ";
      list += LabelFor(name);
    }
    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "Restart Amped 3 Revamped to apply: %s",
                       list.c_str());
  } else {
    ImGui::TextDisabled("Changes are saved to %s.", config_path_.filename().string().c_str());
  }

  if (ImGui::Button("Reset this tab")) {
    if (current_tab_ >= 0 && current_tab_ < static_cast<int>(tabs.size())) {
      for (const auto& section : tabs[current_tab_].sections) {
        for (const auto& o : section.options) {
          if (rex::cvar::GetFlagInfo(o.cvar)) {
            rex::cvar::ResetToDefault(o.cvar);
          }
        }
        if (std::string_view(section.title) == "Game patches") {
          for (const auto& name : rex::cvar::ListFlagsByCategory(kPatchCategory)) {
            rex::cvar::ResetToDefault(name);
          }
        }
      }
      dirty_ = true;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Close") || close_after_draw) {
    Close();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("   F4: every setting (advanced)");

  ImGui::End();

  popup_was_open_ = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
  if (dirty_ && !ImGui::IsAnyItemActive()) {
    Save();  // write once a slider is released, not every frame of a drag
  }
}

}  // namespace amped3

namespace amped3 {
void SetEmulatedGpuLean(bool lean) { g_emulated_gpu_lean = lean; }
bool EmulatedGpuLean() { return g_emulated_gpu_lean; }
}  // namespace amped3
