// amped3 - ReXGlue Recompiled Project: app hooks.

#include "amped3_app.h"
#include "audio_trace.h"

#include <cctype>
#include <chrono>
#include <fstream>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "crash_report.h"
#include "frame_capture.h"
#include "graphics_panel.h"
#include "guest_waits.h"
#include "live_renderer.h"
#include "options_graphics.h"
#include "options_menu.h"
#include "pad_bridge.h"
#include "perf_overlay.h"
#include "thread_qos.h"


// Direct3D 12 on Windows; Vulkan everywhere else (Linux: SteamOS, ARM handhelds).
#ifdef _WIN32
#define AMPED3_DEFAULT_GPU_BACKEND "d3d12"
#else
#define AMPED3_DEFAULT_GPU_BACKEND "vulkan"
#endif
REXCVAR_DEFINE_STRING(gpu_backend, AMPED3_DEFAULT_GPU_BACKEND, "GPU",
                      "Graphics API for the GPU plugin: d3d12 (Windows) or vulkan")
    .allowed({"d3d12", "vulkan"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// The SDK's Vulkan presenter prefers IMMEDIATE (no sync, tears) whenever the
// driver offers it. The game runs at a fixed 30/60 fps, so tearing buys
// nothing: keep it off unless asked for, which leaves MAILBOX/FIFO.
REXCVAR_DEFINE_BOOL(allow_screen_tearing, false, "GPU",
                    "Vulkan: present immediately without waiting for vertical blank (tears)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(emulated_gpu_lean, true, "GPU",
                    "When the native renderer is on at launch, start the Xbox 360 GPU "
                    "emulation lean: no shader cache preload and no resolution scaling (it "
                    "only follows the game's commands then). Switching to the emulation later "
                    "still works, but compiles shaders as it goes and draws at 1x until a "
                    "restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// The command processor keeps the guest's gamma ramp behind protected
// getters; a using-declaration in a derived class makes them nameable (the
// class is never instantiated).
struct GammaRampAccess : rex::graphics::CommandProcessor {
  using CommandProcessor::gamma_ramp_256_entry_table;
  using CommandProcessor::gamma_ramp_pwl_rgb;
  static constexpr auto kTable = &GammaRampAccess::gamma_ramp_256_entry_table;
  static constexpr auto kPwl = &GammaRampAccess::gamma_ramp_pwl_rgb;
};

// SDK patch 0011: the gamma ramp as of the last swap (false: an SDK without
// the patch, or no swap yet).
template <typename CP>
bool ReadSwapGammaRamp(CP* cp, uint32_t* table, uint32_t* pwl) {
  if constexpr (requires { cp->GetSwapGammaRamp(table, pwl); }) {
    return cp->GetSwapGammaRamp(table, pwl) != 0;
  } else {
    return false;
  }
}

// SDK patch 0009: the emulated GPU's drawing switch. Compiles against an SDK
// without the patch (built with -SkipSdk); the switch is then unavailable and
// the emulated GPU keeps drawing.
template <typename CP>
bool SetHostDrawingEnabled(CP* cp, bool on) {
  if constexpr (requires { cp->SetHostDrawingEnabled(on); }) {
    cp->SetHostDrawingEnabled(on);
    return true;
  } else {
    return false;
  }
}

}  // namespace

namespace {
constexpr const char* kMenuBind = "bind_options_menu";
constexpr const char* kHangBind = "bind_hang_report";
constexpr const char* kCaptureBind = "bind_frame_capture";
}  // namespace

Amped3App::~Amped3App() {
  StopControllerHotkeyWatcher();
}

void Amped3App::OnPreSetup(rex::RuntimeConfig& config) {
  // GPU emulation lives in the rexgpu-xenos plugin; the SDK loads none unless
  // asked. --gpu_plugin on the command line still overrides this.
  if (config.gpu_plugin.empty()) {
    config.gpu_plugin = "xenos";
  }
  if (!rex::cvar::SetFlagByName("vulkan_allow_present_mode_immediate",
                                REXCVAR_GET(allow_screen_tearing) ? "true" : "false")) {
    REXLOG_WARN("Could not set vulkan_allow_present_mode_immediate");
  }
  // The SDK would load the plugin with backend "any" (D3D12 first on Windows).
  // Load it here with the API chosen in Options -> Display/Graphics instead;
  // if that fails (no Vulkan driver, say) fall back to the SDK's choice.
  if (!config.graphics) {
    const std::string backend = REXCVAR_GET(gpu_backend);
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
    if (!config.graphics) {
      REXLOG_WARN("GPU backend '{}' unavailable; using the default", backend);
    }
  }
}

namespace {

// default.xex in `dir`, in any letter case (Linux file names are
// case-sensitive; the game's own file lookups already aren't).
bool HasDefaultXex(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return false;
  if (std::filesystem::is_regular_file(dir / "default.xex", ec)) return true;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    std::string name = entry.path().filename().string();
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name == "default.xex" && entry.is_regular_file(ec)) return true;
  }
  return false;
}

}  // namespace

// Where the game files are, first match wins:
//   game/ next to the exe (the installed layout), or default.xex beside the exe
//   game/ in the current folder (launchers that cd first)
//   the folder named in game.path next to the exe (setup.sh writes it into
//     the build folder so the build can be run in place)
//   any single folder under game/ holding default.xex (an extra level from
//     unpacking an archive)
// Every place looked at is kept for the log.
std::filesystem::path Amped3App::FindGameDir(const std::filesystem::path& exe_dir,
                                             std::vector<std::string>* searched) {
  std::vector<std::filesystem::path> candidates = {exe_dir / "game", exe_dir};
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec && cwd != exe_dir) candidates.push_back(cwd / "game");
  if (std::ifstream file(exe_dir / "game.path"); file) {
    std::string line;
    std::getline(file, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) candidates.push_back(std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(line.data()), line.size())));
  }
  for (const auto& dir : candidates) {
    searched->push_back(dir.string());
    if (HasDefaultXex(dir)) return dir;
  }
  for (const auto& entry : std::filesystem::directory_iterator(exe_dir / "game", ec)) {
    if (!entry.is_directory(ec)) continue;
    searched->push_back(entry.path().string());
    if (HasDefaultXex(entry.path())) return entry.path();
  }
  return {};
}

void Amped3App::OnConfigurePaths(rex::PathConfig& paths) {
  // Everything lives next to the exe, so the install folder is portable:
  //   game\    the game files (unless --game_data_root says otherwise)
  //   save\    saves and profile (unless --user_data_root)
  //   cache\   the emulated GPU's shader storage (unless --cache_root)
  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  if (paths.game_data_root.empty()) {
    paths.game_data_root = FindGameDir(exe_dir, &game_search_);
  }
  if (REXCVAR_GET(user_data_root).empty()) {
    const auto old_dir = paths.user_data_root;  // the SDK's default: Documents\amped3
    paths.user_data_root = exe_dir / "save";
    MoveOldSaves(old_dir, paths.user_data_root);
  }
  if (REXCVAR_GET(cache_root).empty()) paths.cache_root = exe_dir / "cache";
  game_dir_ = paths.game_data_root;
  config_path_ = paths.config_path;
}

// Saves from builds that kept them in Documents\amped3: copied (not moved;
// the old folder stays as a backup) into save\ the first time, if save\
// doesn't exist yet. Reported once logging is up.
void Amped3App::MoveOldSaves(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code ec;
  if (from.empty() || from == to || std::filesystem::exists(to, ec) ||
      !std::filesystem::is_directory(from, ec)) {
    return;
  }
  std::filesystem::create_directories(to, ec);
  for (const auto& entry : std::filesystem::directory_iterator(from, ec)) {
    if (entry.path().filename() == "cache") continue;  // shader storage, rebuilt
    std::filesystem::copy(entry.path(), to / entry.path().filename(),
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::skip_existing,
                          ec);
    if (ec) {
      saves_note_ = "Couldn't copy all old saves from " + from.string() + ": " + ec.message();
      return;
    }
  }
  saves_note_ = "Copied saves from " + from.string() + " to " + to.string() +
                " (the old folder is left as a backup)";
}

void Amped3App::OnPostInitLogging() {
  // Same folder the SDK writes amped3_NNN.log to.
  amped3::crash::Install(rex::filesystem::GetExecutableFolder() / "logs");
  amped3::capture::Install(rex::filesystem::GetExecutableFolder() / "logs");

  // Paths are resolved before the log file opens, so report them here.
  if (game_dir_.empty()) {
    REXLOG_ERROR(
        "Amped 3 game files not found. Put the extracted game in a 'game' folder next to "
        "{}, or run with --game_data_root=<folder containing default.xex>.",
        rex::filesystem::GetExecutablePath().filename().string());
    REXLOG_ERROR("  Executable: {}", rex::filesystem::GetExecutablePath().string());
    for (const auto& dir : game_search_) REXLOG_ERROR("  Looked in: {}", dir);
  } else {
    REXLOG_INFO("Amped 3 game files: {}", game_dir_.string());
  }
  if (!saves_note_.empty()) REXLOG_INFO("{}", saves_note_);

  // The config is loaded and the GPU isn't set up yet: with the native
  // renderer drawing, the emulated GPU only processes the game's commands,
  // so skip what only its drawing uses (SDK patch 0017).
  lean_gpu_ = REXCVAR_GET(emulated_gpu_lean) &&
              rex::cvar::GetFlagByName("native_renderer") == "true";
  amped3::SetEmulatedGpuLean(lean_gpu_);
  if (lean_gpu_) {
    rex::system::SetDrawResolutionScaleCap(1);
    REXLOG_INFO(
        "Emulated GPU: lean (native renderer on): no shader cache preload, 1x resolution");
  }
}

bool Amped3App::OnWantShaderStorage() { return !lean_gpu_; }

void Amped3App::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  // F3: the performance overlay, in place of the SDK's debug overlay.
  amped3::perf::Install(drawer);
  // Keyboard shortcut; shows up (and can be rebound) in the F4 settings panel.
  rex::ui::RegisterBind(kMenuBind, "F1", "Open or close the Amped 3 Revamped options menu",
                        [this] { ToggleOptionsMenu(); });
  // For a game that's stuck without crashing: every thread's stack, so the
  // stuck game thread and what it waits on show up.
  rex::ui::RegisterBind(kHangBind, "F9", "Write a hang report (all thread stacks)",
                        [] { amped3::crash::WriteHangReport(); });
  // Records the next whole frame's draws and device state (docs/frame-capture.md).
  rex::ui::RegisterBind(kCaptureBind, "F10", "Capture one frame of Direct3D calls to logs",
                        [] { amped3::capture::RequestFrame(); });
}

void Amped3App::OnConfigureFonts(ImFontAtlas* atlas) {
  // The Graphics panel's text, baked at a few sizes so it's sharp at any
  // resolution (the SDK's own font stays the default for everything else).
  amped3::LoadPanelFonts(atlas);
}

void Amped3App::OnPostSetup() {
  // The window says what this is (the SDK titles it with the exe's name).
  app_context().CallInUIThread([this] {
    if (auto* w = window()) w->SetTitle("Amped 3 Revamped");
  });
  // Hybrid CPUs: the game's threads stay on full-speed cores.
  amped3::qos::PreferPerformanceProcess();
  if (auto* memory = runtime()->memory()) {
    amped3::crash::SetGuestMemory(memory->virtual_membase(), memory->physical_membase());
  }
  // After the SDK's own fault handlers (MMIO), so only faults they don't
  // claim are reported (Linux; nothing to do on Windows).
  amped3::crash::InstallFaultHandler();
  amped3::crash::SetHangReportExtra(
      [this] { return amped3::diag::DescribeGuestWaits(runtime()->kernel_state()); });

  // Route game input through the pad bridge so the game sees an idle
  // controller while the options menu is open, then watch for Back+Start.
  auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
  auto* drawer = imgui_drawer();
  amped3::pad::Install(input, &game_input_blocked_, [drawer]() {
    // Same rule the SDK applies for its own overlays (F3 / F4 / `).
    return drawer && drawer->HasDialogs() && drawer->GetIO().WantCaptureMouse;
  });
  if (input) {
    StartControllerHotkeyWatcher();
  }

  // Pause > Options > Graphics in the game's own menu (patched in memory as it
  // loads: gui_patch.cpp) opens the port's Graphics panel. Called on the game
  // thread; the game stops seeing the controller at once.
  amped3::options_graphics::SetOpenPanel([this] {
    game_input_blocked_ = true;
    app_context().CallInUIThread([this] { OpenGraphicsPanel(); });
  });

  // Music / XMA stream diagnostics (setting trace_audio).
  amped3::audio_trace::Start(runtime());

  // Native renderer: its render scale is a percentage of the window's size.
  amped3::live::SetWindowSize([this]() -> std::pair<uint32_t, uint32_t> {
    auto* w = window();
    return w ? std::pair<uint32_t, uint32_t>{w->GetActualPhysicalWidth(),
                                             w->GetActualPhysicalHeight()}
             : std::pair<uint32_t, uint32_t>{0, 0};
  });

  // Native renderer preview (setting native_renderer): presents through the
  // GPU plugin's presenter.
  amped3::live::Install(
      rex::filesystem::GetExecutableFolder() / "logs",
      [this]() -> rex::ui::Presenter* {
        auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
        return graphics ? graphics->presenter() : nullptr;
      },
      REXCVAR_GET(gpu_backend),
      [this](amped3::live::GammaRamp* ramp) {
        auto* graphics =
            static_cast<rex::graphics::GraphicsSystem*>(runtime() ? runtime()->graphics_system()
                                                                   : nullptr);
        auto* cp = graphics ? graphics->command_processor() : nullptr;
        if (!cp) return false;
        // The ramp as of the last swap (SDK patch 0011), copied on the GPU
        // thread; reading the live registers from here races with the game
        // rewriting them.
        if (ReadSwapGammaRamp(cp, ramp->table, &ramp->pwl[0][0])) return true;
        const auto* table = (cp->*GammaRampAccess::kTable)();
        const auto* pwl = (cp->*GammaRampAccess::kPwl)();
        for (int i = 0; i < 256; ++i) ramp->table[i] = table[i].value;
        for (int i = 0; i < 128; ++i) {
          for (int c = 0; c < 3; ++c) ramp->pwl[i][c] = pwl[i * 3 + c].value;
        }
        return true;
      },
      [this](bool on) {
        auto* graphics =
            static_cast<rex::graphics::GraphicsSystem*>(runtime() ? runtime()->graphics_system()
                                                                   : nullptr);
        auto* cp = graphics ? graphics->command_processor() : nullptr;
        return cp && SetHostDrawingEnabled(cp, on);
      });

  // Frame rate for the F3 overlay, measured at D3DDevice_Swap.
  SetGuestFrameStats([] {
    const auto rate = amped3::capture::GetFrameRate();
    rex::ui::FrameStats stats;
    stats.fps = rate.fps;
    stats.frame_time_ms = rate.frame_time_ms;
    stats.frame_count = rate.frame_count;
    return stats;
  });
}

void Amped3App::OnShutdown() {
  amped3::crash::MarkCleanShutdown();
  amped3::perf::Shutdown();
  amped3::live::Shutdown();
  amped3::audio_trace::Stop();
  StopControllerHotkeyWatcher();
  amped3::pad::Install(nullptr, nullptr, nullptr);
  rex::ui::UnregisterBind(kMenuBind);
  rex::ui::UnregisterBind(kHangBind);
  rex::ui::UnregisterBind(kCaptureBind);
}

void Amped3App::ToggleOptionsMenu() {
  if (options_menu_) {
    options_menu_->RequestClose();
    return;
  }
  if (!imgui_drawer()) {
    return;
  }
  if (graphics_panel_) {
    return;
  }
  options_menu_open_ = true;
  game_input_blocked_ = true;
  // The dialog registers itself with the drawer and deletes itself when it
  // closes; the callback clears our handle at that point.
  options_menu_ = new amped3::OptionsMenu(imgui_drawer(), window(), config_path_, [this] {
    options_menu_ = nullptr;
    options_menu_open_ = false;
    unblock_when_released_ = true;
  });
}

void Amped3App::OpenGraphicsPanel() {
  if (graphics_panel_ || options_menu_ || !imgui_drawer()) {
    if (!graphics_panel_ && !options_menu_) unblock_when_released_ = true;
    return;
  }
  graphics_panel_open_ = true;
  game_input_blocked_ = true;
  graphics_panel_ = new amped3::GraphicsPanel(imgui_drawer(), window(), config_path_, [this] {
    graphics_panel_ = nullptr;
    graphics_panel_open_ = false;
    unblock_when_released_ = true;
  });
}

void Amped3App::StartControllerHotkeyWatcher() {
  if (hotkey_thread_.joinable()) {
    return;
  }
  hotkey_thread_stop_ = false;
  hotkey_thread_ = std::thread([this] {
    using clock = std::chrono::steady_clock;
    constexpr auto kHold = std::chrono::milliseconds(150);
    constexpr uint16_t kCombo = rex::input::X_INPUT_GAMEPAD_BACK | rex::input::X_INPUT_GAMEPAD_START;
    clock::time_point held_since{};
    bool latched = false;
    while (!hotkey_thread_stop_.load()) {
      const auto pad = amped3::pad::Read();
      amped3::pad::Publish(pad);  // the port's menus read this on the UI thread
      // A menu just closed: give the game the controller back once every
      // button is up.
      if (unblock_when_released_ && pad.buttons == 0 && !options_menu_open_ &&
          !graphics_panel_open_) {
        unblock_when_released_ = false;
        game_input_blocked_ = false;
      }
      if ((pad.buttons & kCombo) == kCombo) {
        const auto now = clock::now();
        if (held_since == clock::time_point{}) {
          held_since = now;
        }
        if (!latched && now - held_since >= kHold) {
          latched = true;
          app_context().CallInUIThread([this] { ToggleOptionsMenu(); });
        }
      } else {
        held_since = {};
        latched = false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
  });
}

void Amped3App::StopControllerHotkeyWatcher() {
  hotkey_thread_stop_ = true;
  if (hotkey_thread_.joinable()) {
    hotkey_thread_.join();
  }
}
