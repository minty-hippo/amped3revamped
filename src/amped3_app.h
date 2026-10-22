// amped3 - ReXGlue Recompiled Project
//
// App class for the Amped 3 PC port.

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <thread>

#include <rex/rex_app.h>

namespace amped3 {
class OptionsMenu;
class GraphicsPanel;
}

namespace amped3 {
// One game at a time: false when another Amped 3 Revamped is already running
// (a second launch from the launcher or a shortcut played its menu music over
// the first one's game). The lock is held until the process exits.
bool AcquireSingleInstance();
}  // namespace amped3

class Amped3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  ~Amped3App() override;

  // Defined in main.cpp, the one file that includes the generated init header
  // (which provides PPCImageConfig).
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPostInitLogging() override;
  bool OnWantShaderStorage() override;
  void OnPostSetup() override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnShutdown() override;

 private:
  // Opens the menu if closed, closes it if open. UI thread only.
  void ToggleOptionsMenu();
  // The Graphics panel (Pause > Options > Graphics in the game's menu). UI
  // thread only.
  void OpenGraphicsPanel();
  void StartControllerHotkeyWatcher();
  void StopControllerHotkeyWatcher();

  std::filesystem::path config_path_;
  std::filesystem::path game_dir_;
  bool lean_gpu_ = false;  // emulated GPU started lean (native renderer on at launch)
  std::vector<std::string> game_search_;  // where FindGameDir looked, for the log
  static std::filesystem::path FindGameDir(const std::filesystem::path& exe_dir,
                                           std::vector<std::string>* searched);
  std::string saves_note_;  // what MoveOldSaves did, for the log
  void MoveOldSaves(const std::filesystem::path& from, const std::filesystem::path& to);

  // Owned by the ImGui drawer once created (ImGuiDialog deletes itself on
  // close); this is a non-owning handle cleared from the menu's close callback.
  amped3::OptionsMenu* options_menu_ = nullptr;
  amped3::GraphicsPanel* graphics_panel_ = nullptr;
  // The game sees an idle controller while the port's menus are open, and
  // until every button is let go after they close (so the B that closes one
  // doesn't also go back in the game's menu).
  std::atomic<bool> options_menu_open_{false};
  std::atomic<bool> graphics_panel_open_{false};
  std::atomic<bool> game_input_blocked_{false};
  std::atomic<bool> unblock_when_released_{false};

  std::thread hotkey_thread_;
  std::atomic<bool> hotkey_thread_stop_{false};
};
