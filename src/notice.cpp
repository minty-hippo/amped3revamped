// amped3 - a short notice at the top of the screen (notice.h).

#include "notice.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <memory>
#include <mutex>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "graphics_panel.h"

namespace amped3::notice {
namespace {

using Clock = std::chrono::steady_clock;
constexpr double kSeconds = 2.0;

rex::ui::ImGuiDrawer* g_drawer = nullptr;
std::function<void(std::function<void()>)> g_post_ui;
std::mutex g_mutex;
std::string g_text;
Clock::time_point g_shown;

class Overlay;
void CloseLater();

class Overlay final : public rex::ui::ImGuiDialog {
 public:
  explicit Overlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::string text;
    double age;
    {
      std::lock_guard lock(g_mutex);
      text = g_text;
      age = std::chrono::duration<double>(Clock::now() - g_shown).count();
    }
    if (age > kSeconds) {
      CloseLater();
      return;
    }
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (w < 64 || h < 64 || text.empty()) return;
    const float pic_h = std::min(h, w * 9.0f / 16.0f);
    const float px = std::max(14.0f, pic_h * 0.032f);
    ImFont* font = amped3::UiFont(px);
    // Fades out over its last half second.
    const float alpha = float(std::clamp((kSeconds - age) / 0.5, 0.0, 1.0));
    const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0, text.c_str());
    const float x = (w - size.x) * 0.5f, y = (h - pic_h) * 0.5f + pic_h * 0.06f;
    const float pad_x = px * 0.6f, pad_y = px * 0.3f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x - pad_x, y - pad_y), ImVec2(x + size.x + pad_x, y + size.y + pad_y),
                      IM_COL32(0, 0, 0, int(170 * alpha)), px * 0.25f);
    dl->AddText(font, px, ImVec2(x, y), IM_COL32(255, 255, 255, int(255 * alpha)), text.c_str());
  }
};

std::unique_ptr<Overlay> g_overlay;  // UI thread; only while a notice shows

// Removed after this frame (not from inside its own draw), unless a new
// notice came meanwhile.
void CloseLater() {
  if (!g_post_ui) return;
  g_post_ui([] {
    std::lock_guard lock(g_mutex);
    if (std::chrono::duration<double>(Clock::now() - g_shown).count() > kSeconds) {
      g_overlay.reset();
    }
  });
}

}  // namespace

void Install(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_ui) {
  g_drawer = drawer;
  g_post_ui = std::move(post_ui);
}

void Show(std::string text) {
  {
    std::lock_guard lock(g_mutex);
    g_text = std::move(text);
    g_shown = Clock::now();
  }
  if (!g_post_ui || !g_drawer) return;
  g_post_ui([] {
    if (!g_overlay) g_overlay = std::make_unique<Overlay>(g_drawer);
  });
}

}  // namespace amped3::notice
