// amped3 - the Graphics panel, opened from the game's own Options menu
// (Pause > Options > Graphics; see options_graphics.cpp).
//
// Drawn by the port at the left of the screen in the look of the game's
// menus, with the game's own HUD and menus hidden meanwhile (native renderer)
// so the scene shows each change as it's made:
// two tabs, switched with LB / RB:
//   Effects: Revamped Graphics (every enhancement at once), a switch for each
//            enhancement, and the motion blur strength.
//   Display: fullscreen, display (output) resolution, render resolution,
//            upscaler and FSR sharpness.
// Controller: up / down choose, left / right / A change, B goes back.
// Keyboard: arrows, Enter, Escape, Q / E (or Tab) for tabs.
// Changes apply at once and are saved to amped3.toml when it closes.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

struct ImFontAtlas;
namespace rex::ui {
class Window;
}

namespace amped3 {

// The panel's fonts: a TrueType font baked at a few sizes, so its text is
// drawn near 1:1 at any display resolution. From the app's OnConfigureFonts.
void LoadPanelFonts(ImFontAtlas* atlas);

struct GraphicsRow;  // a setting on the panel (graphics_panel.cpp)

class GraphicsPanel final : public rex::ui::ImGuiDialog {
 public:
  GraphicsPanel(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                std::filesystem::path config_path, std::function<void()> on_closed);
  ~GraphicsPanel() override;

  void RequestClose() { Close(); }

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  void HandleInput(ImGuiIO& io);
  void Change(const GraphicsRow& row, int direction);
  std::string Hint(const GraphicsRow& row) const;

  rex::ui::Window* window_;
  std::filesystem::path config_path_;
  std::function<void()> on_closed_;
  int tab_ = 0;
  int cursor_[2] = {0, 0};  // per tab
  bool dirty_ = false;
  bool first_frame_ = true;
  uint16_t prev_buttons_ = 0;
  int held_direction_ = 0;  // -1 up, 1 down, -2 left, 2 right
  double repeat_at_ = 0.0;
};

}  // namespace amped3
