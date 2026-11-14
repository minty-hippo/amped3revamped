// amped3 - freecam (freecam.h).

#include "freecam.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>

#include <imgui.h>

#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "graphics_panel.h"
#include "live_renderer.h"
#include "notice.h"

namespace amped3::freecam {
namespace {

using Clock = std::chrono::steady_clock;

Host g_host;
std::atomic<bool> g_active{false};

// The keyboard and mouse, from the overlay (UI thread), for OnPad.
std::mutex g_keys_mutex;
struct Keys {
  float move[3] = {};
  float look[2] = {};
  float zoom = 0.0f;
  float speed_scale = 1.0f;
  float mouse[2] = {};  // radians, added up until OnPad hands them on
  bool reset = false;
  bool leave = false;
} g_keys;

std::atomic<bool> g_show_help{true};
std::atomic<int64_t> g_started_ns{0};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

float Stick(int16_t v) {
  // Dead zone, then a curve: fine control near the middle.
  const float f = std::clamp(float(v) / 32767.0f, -1.0f, 1.0f);
  const float a = std::fabs(f);
  if (a < 0.24f) return 0.0f;
  const float t = (a - 0.24f) / 0.76f;
  return std::copysign(t * t, f);
}

class Overlay final : public rex::ui::ImGuiDialog {
 public:
  explicit Overlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!g_active.load()) return;
    ReadKeys(io);
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (w < 64 || h < 64) return;
    const float pic_h = std::min(h, w * 9.0f / 16.0f);
    const float px = std::max(13.0f, pic_h * 0.022f);
    ImFont* font = amped3::UiFont(px);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float x0 = (w - std::min(w, h * 16.0f / 9.0f)) * 0.5f + px, y0 = (h - pic_h) * 0.5f + px;
    const bool showing = live::FreecamShowing();
    const double age = double(NowNs() - g_started_ns.load()) * 1e-9;
    std::string text;
    if (!showing) {
      text = "Freecam: waiting for a 3D scene";
    } else if (g_show_help.load() || age < 6.0) {
      text =
          "FREECAM\n"
          "Left stick / W A S D: move     Right stick / arrows / right mouse: look\n"
          "LT RT / Q E: down, up     LB RB / Z C: zoom     A / Shift: faster     X / Alt: slower\n"
          "Y / R: back to the game's camera     View: hide this     B / L3+R3 / F6: leave";
    }
    if (text.empty()) return;
    const float alpha = showing && !g_show_help.load() ? float(std::clamp(6.0 - age, 0.0, 1.0)) : 1.0f;
    const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0, text.c_str());
    const float pad = px * 0.5f;
    dl->AddRectFilled(ImVec2(x0 - pad, y0 - pad), ImVec2(x0 + size.x + pad, y0 + size.y + pad),
                      IM_COL32(0, 0, 0, int(150 * alpha)), px * 0.3f);
    dl->AddText(font, px, ImVec2(x0, y0), IM_COL32(255, 255, 255, int(235 * alpha)), text.c_str());
  }

 private:
  static void ReadKeys(ImGuiIO& io) {
    auto down = [](ImGuiKey k) { return ImGui::IsKeyDown(k); };
    Keys k;
    k.move[0] = float(down(ImGuiKey_D)) - float(down(ImGuiKey_A));
    k.move[1] = float(down(ImGuiKey_E)) - float(down(ImGuiKey_Q));
    k.move[2] = float(down(ImGuiKey_W)) - float(down(ImGuiKey_S));
    k.look[0] = float(down(ImGuiKey_RightArrow)) - float(down(ImGuiKey_LeftArrow));
    k.look[1] = float(down(ImGuiKey_UpArrow)) - float(down(ImGuiKey_DownArrow));
    k.zoom = float(down(ImGuiKey_C)) - float(down(ImGuiKey_Z));
    k.speed_scale = io.KeyShift ? 4.0f : io.KeyAlt ? 0.25f : 1.0f;
    std::lock_guard lock(g_keys_mutex);
    const bool reset = g_keys.reset || ImGui::IsKeyPressed(ImGuiKey_R, false);
    const bool leave = g_keys.leave || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    float mouse[2] = {g_keys.mouse[0], g_keys.mouse[1]};
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
      mouse[0] += io.MouseDelta.x * 0.003f;
      mouse[1] -= io.MouseDelta.y * 0.003f;
    }
    g_keys = k;
    g_keys.reset = reset;
    g_keys.leave = leave;
    g_keys.mouse[0] = mouse[0];
    g_keys.mouse[1] = mouse[1];
  }
};

std::unique_ptr<Overlay> g_overlay;  // UI thread, while on

void SetOn(bool on) {
  if (on == g_active.load()) return;
  if (on && !live::Enabled()) {
    notice::Show("Freecam needs the native renderer");
    return;
  }
  g_active = on;
  REXLOG_INFO("Freecam {}", on ? "on" : "off");
  if (g_host.block_game_input) g_host.block_game_input(on);
  native::Renderer::FreecamInput in;
  in.active = on;
  in.reset = on;  // from the game's camera each time
  live::SetFreecam(in);
  if (on) {
    g_started_ns = NowNs();
    g_show_help = false;
    {
      std::lock_guard lock(g_keys_mutex);
      g_keys = Keys{};
    }
  }
  if (g_host.post_ui && g_host.drawer) {
    g_host.post_ui([on] {
      if (on && !g_overlay) g_overlay = std::make_unique<Overlay>(g_host.drawer);
      if (!on) g_overlay.reset();
    });
  }
}

}  // namespace

void Install(Host host) { g_host = std::move(host); }

void Toggle() { SetOn(!g_active.load()); }

bool Active() { return g_active.load(); }

void OnPad(const pad::Snapshot& pad) {
  static uint16_t last_buttons = 0;
  const uint16_t pressed = uint16_t(pad.buttons & ~last_buttons);
  last_buttons = pad.buttons;
  if (!g_active.load()) return;
  using namespace rex::input;
  Keys keys;
  {
    std::lock_guard lock(g_keys_mutex);
    keys = g_keys;
    g_keys.mouse[0] = g_keys.mouse[1] = 0.0f;
    g_keys.reset = g_keys.leave = false;
  }
  if ((pressed & X_INPUT_GAMEPAD_B) || keys.leave) {
    SetOn(false);
    return;
  }
  if (pressed & X_INPUT_GAMEPAD_BACK) g_show_help = !g_show_help.load();
  native::Renderer::FreecamInput in;
  in.active = true;
  in.reset = keys.reset || (pressed & X_INPUT_GAMEPAD_Y);
  in.move[0] = std::clamp(Stick(pad.lx) + keys.move[0], -1.0f, 1.0f);
  in.move[2] = std::clamp(Stick(pad.ly) + keys.move[2], -1.0f, 1.0f);
  in.move[1] = std::clamp(float(pad.right_trigger) / 255.0f - float(pad.left_trigger) / 255.0f +
                              keys.move[1],
                          -1.0f, 1.0f);
  in.look[0] = std::clamp(Stick(pad.rx) + keys.look[0], -1.0f, 1.0f);
  in.look[1] = std::clamp(Stick(pad.ry) + keys.look[1], -1.0f, 1.0f);
  in.look_delta[0] = keys.mouse[0];
  in.look_delta[1] = keys.mouse[1];
  in.zoom = std::clamp(float(bool(pad.buttons & X_INPUT_GAMEPAD_RIGHT_SHOULDER)) -
                           float(bool(pad.buttons & X_INPUT_GAMEPAD_LEFT_SHOULDER)) + keys.zoom,
                       -1.0f, 1.0f);
  float scale = keys.speed_scale;
  if (pad.buttons & X_INPUT_GAMEPAD_A) scale *= 4.0f;
  if (pad.buttons & X_INPUT_GAMEPAD_X) scale *= 0.25f;
  in.speed = 8.0f * scale;
  live::SetFreecam(in);
}

}  // namespace amped3::freecam
