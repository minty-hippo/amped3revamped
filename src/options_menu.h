// amped3 - in-game options menu.
//
// A controller-first ImGui dialog over the SDK's cvars: display, graphics,
// audio, controls and enhancement settings. Changes apply immediately where
// the SDK supports it, are saved to amped3.toml next to the executable, and
// settings that only take effect on the next launch are flagged in the UI.
//
// Open/close: Back+Start (View+Menu) on any controller, or F1 (rebindable as
// bind_options_menu in amped3.toml). B / Escape closes. LB/RB switch tabs.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class Window;
}

namespace amped3 {

// After the Output resolution ("resolution") changes: with the native
// renderer, resizes the window to it at once (windowed only). UI thread.
void ApplyOutputResolution(rex::ui::Window* window, const std::string& value);

class OptionsMenu final : public rex::ui::ImGuiDialog {
 public:
  OptionsMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
              std::filesystem::path config_path, std::function<void()> on_closed);
  ~OptionsMenu() override;

  // Closes on the next drawn frame (the dialog then deletes itself).
  void RequestClose() { Close(); }

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  void FeedGamepad(ImGuiIO& io);
  void ReleaseGamepad(ImGuiIO& io);
  void Save();

  rex::ui::Window* window_;
  std::filesystem::path config_path_;
  std::function<void()> on_closed_;

  uint16_t prev_buttons_ = 0;
  bool first_frame_ = true;
  bool popup_was_open_ = false;
  int current_tab_ = 0;
  int requested_tab_ = -1;
  bool dirty_ = false;
  std::string help_text_;
  int prev_config_flags_ = 0;
  int prev_backend_flags_ = 0;
};

}  // namespace amped3
