// amped3 - loading into a mountain: progress and "Press X to Continue"
// (load_screen.h).

#include "load_screen.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/input/input.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "graphics_panel.h"
#include "pad_bridge.h"

namespace amped3::load_screen {
namespace {

Host g_host;
std::mutex g_mutex;
uint32_t g_done = 0, g_total = 0;
bool g_waiting = false;
bool g_continue = false;   // pressed (taken by TakeContinue)
bool g_armed = false;      // X seen up since the wait began (a held X doesn't count)
bool g_blocked = false;    // the game's input blocked for the wait

class Overlay final : public rex::ui::ImGuiDialog {
 public:
  explicit Overlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    uint32_t done, total;
    bool waiting;
    {
      std::lock_guard lock(g_mutex);
      done = g_done;
      total = g_total;
      waiting = g_waiting;
    }
    const bool compiling = total > 0 && done < total;
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (w < 64 || h < 64 || (!compiling && !waiting)) return;
    // The game's picture (16:9, letterboxed): the bar near its bottom, under
    // the loading screen's slides.
    const float pic_h = std::min(h, w * 9.0f / 16.0f), pic_w = pic_h * 16.0f / 9.0f;
    const float left = (w - pic_w) * 0.5f, top = (h - pic_h) * 0.5f;
    const float px = std::max(14.0f, pic_h * 0.028f);
    ImFont* font = amped3::UiFont(px);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float bar_w = pic_w * 0.42f, bar_h = std::max(6.0f, pic_h * 0.012f);
    const float bx = left + (pic_w - bar_w) * 0.5f, by = top + pic_h * 0.905f;
    if (compiling) {
      const float t = float(done) / float(std::max(total, 1u));
      char label[96];
      std::snprintf(label, sizeof(label), "Preparing shaders  %u / %u", done, total);
      const ImVec2 ts = font->CalcTextSizeA(px * 0.8f, FLT_MAX, 0, label);
      const float pad = px * 0.45f;
      dl->AddRectFilled(ImVec2(bx - pad, by - ts.y - pad * 1.6f), ImVec2(bx + bar_w + pad, by + bar_h + pad),
                        IM_COL32(0, 0, 0, 150), px * 0.3f);
      dl->AddText(font, px * 0.8f, ImVec2(bx + (bar_w - ts.x) * 0.5f, by - ts.y - pad * 0.8f),
                  IM_COL32(235, 240, 250, 255), label);
      dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bar_w, by + bar_h), IM_COL32(255, 255, 255, 50),
                        bar_h * 0.5f);
      if (t > 0.0f) {
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bar_w * t, by + bar_h), IM_COL32(120, 190, 255, 235),
                          bar_h * 0.5f);
      }
    } else if (waiting) {
      // "Press (X) to Continue": the Xbox X button, blue.
      const char* before = "Press";
      const char* after = "to Continue";
      const float tp = px * 1.05f;
      const ImVec2 s1 = font->CalcTextSizeA(tp, FLT_MAX, 0, before);
      const ImVec2 s2 = font->CalcTextSizeA(tp, FLT_MAX, 0, after);
      const float r = tp * 0.55f, gap = tp * 0.35f;
      const float total_w = s1.x + gap + 2 * r + gap + s2.x;
      const float x0 = left + (pic_w - total_w) * 0.5f, y0 = by - tp * 0.5f;
      // A gentle pulse.
      const float pulse = 0.75f + 0.25f * std::sin(float(ImGui::GetTime()) * 3.0f);
      const int a = int(255 * pulse);
      const float pad = tp * 0.5f;
      dl->AddRectFilled(ImVec2(x0 - pad, y0 - pad * 0.6f), ImVec2(x0 + total_w + pad, y0 + tp + pad * 0.6f),
                        IM_COL32(0, 0, 0, 150), tp * 0.3f);
      dl->AddText(font, tp, ImVec2(x0, y0), IM_COL32(255, 255, 255, a), before);
      const ImVec2 c(x0 + s1.x + gap + r, y0 + tp * 0.5f);
      dl->AddCircleFilled(c, r, IM_COL32(40, 110, 220, a), 32);
      const ImVec2 xs = font->CalcTextSizeA(tp * 0.8f, FLT_MAX, 0, "X");
      dl->AddText(font, tp * 0.8f, ImVec2(c.x - xs.x * 0.5f, c.y - xs.y * 0.5f), IM_COL32(255, 255, 255, a), "X");
      dl->AddText(font, tp, ImVec2(c.x + r + gap, y0), IM_COL32(255, 255, 255, a), after);
      // The press: X on a pad (after it's been seen up), Enter, Space, or a click.
      const auto pad_now = amped3::pad::Latest();
      const bool x_down = (pad_now.buttons & (rex::input::X_INPUT_GAMEPAD_X)) != 0;
      bool pressed = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Left);
      std::lock_guard lock(g_mutex);
      if (!x_down) g_armed = true;
      if (x_down && g_armed) pressed = true;
      if (pressed) g_continue = true;
    }
  }
};

std::unique_ptr<Overlay> g_overlay;  // UI thread

}  // namespace

void Install(Host host) { g_host = std::move(host); }

void Update(uint32_t done, uint32_t total, bool waiting) {
  bool show, was_waiting, block_change = false, block = false;
  {
    std::lock_guard lock(g_mutex);
    was_waiting = g_waiting;
    g_done = done;
    g_total = total;
    g_waiting = waiting;
    if (waiting && !was_waiting) {
      g_continue = false;
      g_armed = false;
    }
    show = waiting || (total > 0 && done < total);
    if (waiting != g_blocked) {
      g_blocked = waiting;
      block_change = true;
      block = waiting;
    }
  }
  if (block_change && g_host.block_game_input) g_host.block_game_input(block);
  if (!g_host.post_ui || !g_host.drawer) return;
  static bool shown = false;  // (worker thread: what was asked of the UI thread)
  if (show == shown) return;
  shown = show;
  g_host.post_ui([show] {
    if (show && !g_overlay) g_overlay = std::make_unique<Overlay>(g_host.drawer);
    if (!show) g_overlay.reset();
  });
}

bool TakeContinue() {
  std::lock_guard lock(g_mutex);
  const bool c = g_continue;
  g_continue = false;
  return c;
}

bool Waiting() {
  std::lock_guard lock(g_mutex);
  return g_waiting;
}

}  // namespace amped3::load_screen
