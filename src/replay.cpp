// amped3 - instant replay (replay.h).

#include "replay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "freecam.h"
#include "graphics_panel.h"
#include "help_bar.h"
#include "live_renderer.h"
#include "notice.h"

namespace amped3::replay {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kStateRiding = 13;
constexpr float kSpeeds[] = {0.125f, 0.25f, 0.5f, 1.0f, 2.0f};

Host g_host;
std::atomic<bool> g_active{false};
std::atomic<bool> g_we_paused{false};  // Start pressed for the game: pressed again after
std::atomic<bool> g_show_help{true};
std::atomic<int> g_bar_rows{0};
std::atomic<int64_t> g_started_ns{0};

// From the overlay (UI thread): the keyboard, for OnPad.
std::mutex g_keys_mutex;
struct Keys {
  bool toggle = false, restart = false, leave = false;
  int speed_step = 0;
  float scrub = 0.0f;  // -1..1
} g_keys;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

int SpeedIndex(float speed) {
  int best = 3;
  for (int i = 0; i < 5; ++i) {
    if (std::fabs(kSpeeds[i] - speed) < std::fabs(kSpeeds[best] - speed)) best = i;
  }
  return best;
}

std::string Time(double s) {
  char b[16];
  std::snprintf(b, sizeof(b), "%d:%04.1f", int(s) / 60, std::fmod(s, 60.0));
  return b;
}

class Overlay final : public rex::ui::ImGuiDialog {
 public:
  explicit Overlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!g_active.load()) return;
    if (!freecam::Active()) ReadKeys();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (w < 64 || h < 64) return;
    const float pic_w = std::min(w, h * 16.0f / 9.0f), pic_h = std::min(h, w * 9.0f / 16.0f);
    const float px = std::max(13.0f, pic_h * 0.024f);
    ImFont* font = amped3::UiFont(px);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const live::ReplayStatus st = live::ReplayGetStatus();
    // The bar: REPLAY, play / pause, time, speed, progress.
    const float x0 = (w - pic_w) * 0.5f + pic_w * 0.06f, x1 = (w + pic_w) * 0.5f - pic_w * 0.06f;
    const float y = (h - pic_h) * 0.5f + pic_h * 0.08f;  // the top (the controls go along the bottom)
    char speed[16];
    std::snprintf(speed, sizeof(speed), "x%g", double(st.speed));
    const std::string label = std::string("REPLAY   ") + (st.paused ? "||" : ">") + "   " +
                              Time(st.position) + " / " + Time(st.length) + "   " + speed;
    dl->AddRectFilled(ImVec2(x0 - px * 0.6f, y - px * 1.9f), ImVec2(x1 + px * 0.6f, y + px * 0.9f),
                      IM_COL32(0, 0, 0, 140), px * 0.3f);
    dl->AddText(font, px, ImVec2(x0, y - px * 1.6f), IM_COL32(255, 255, 255, 240), label.c_str());
    const float frac = st.length > 0 ? float(std::clamp(st.position / st.length, 0.0, 1.0)) : 0.0f;
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + px * 0.35f), IM_COL32(255, 255, 255, 60));
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * frac, y + px * 0.35f),
                      IM_COL32(250, 210, 40, 230));
    // The controls, along the bottom (above the progress bar).
    const std::vector<std::string> items = {
        "A / Space: play, pause",
        "LB RB / [ ]: slower, faster",
        "LT RT / Left Right: back, forward",
        "Y / Home: from the start",
        "Tap L3+R3 / F6: freecam",
        "View / H: hide controls",
        "B / hold L3+R3 / Esc / F7: leave",
    };
    if (freecam::Active()) {
      g_bar_rows = 0;  // the freecam's bar has the bottom
      return;
    }
    g_bar_rows = help_bar::Draw(io, "INSTANT REPLAY", items, 0);
  }

 private:
  static void ReadKeys() {
    std::lock_guard lock(g_keys_mutex);
    g_keys.toggle |= ImGui::IsKeyPressed(ImGuiKey_Space, false);
    g_keys.restart |= ImGui::IsKeyPressed(ImGuiKey_Home, false);
    if (ImGui::IsKeyPressed(ImGuiKey_H, false)) help_bar::Toggle();
    g_keys.leave |= ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) --g_keys.speed_step;
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) ++g_keys.speed_step;
    g_keys.scrub = float(ImGui::IsKeyDown(ImGuiKey_RightArrow)) -
                   float(ImGui::IsKeyDown(ImGuiKey_LeftArrow));
  }
};

std::unique_ptr<Overlay> g_overlay;  // UI thread, while on

void Leave() {
  if (!g_active.exchange(false)) return;
  live::ReplayStop();
  if (g_we_paused.exchange(false)) {
    pad::PressForGame(rex::input::X_INPUT_GAMEPAD_START, 120);  // back to riding
  }
  if (g_host.block_game_input && !freecam::Active()) g_host.block_game_input(false);
  if (g_host.post_ui) g_host.post_ui([] { g_overlay.reset(); });
}

void Enter() {
  if (!live::Enabled()) {
    notice::Show("Instant replay needs the native renderer");
    return;
  }
  if (!live::ReplayAvailable()) {
    notice::Show("Nothing to replay yet: it keeps the last seconds of riding");
    return;
  }
  // The game paused for it: by Start while riding; already paused (a still
  // scene), as it is. Anywhere else Start does other things (skips a scene).
  const bool still = live::SceneStill();
  if (!still && live::GameState() != kStateRiding) {
    notice::Show("Instant replay: pause the game first");
    return;
  }
  if (!live::ReplayStart()) return;
  g_active = true;
  if (!still) {
    pad::PressForGame(rex::input::X_INPUT_GAMEPAD_START, 120);
    g_we_paused = true;
  }
  if (g_host.block_game_input) g_host.block_game_input(true);
  g_started_ns = NowNs();
  g_show_help = false;
  {
    std::lock_guard lock(g_keys_mutex);
    g_keys = Keys{};
  }
  if (g_host.post_ui && g_host.drawer) {
    g_host.post_ui([] {
      if (!g_overlay && g_active.load()) g_overlay = std::make_unique<Overlay>(g_host.drawer);
    });
  }
}

}  // namespace

void Install(Host host) { g_host = std::move(host); }

void Toggle() {
  if (g_active.load()) {
    Leave();
  } else {
    Enter();
  }
}

bool Active() { return g_active.load(); }

int BarRows() { return g_bar_rows.load(); }

void OnPad(const pad::Snapshot& pad) {
  static uint16_t last_buttons = 0;
  static Clock::time_point last_time = Clock::now();
  const auto now = Clock::now();
  const double dt = std::min(0.1, std::chrono::duration<double>(now - last_time).count());
  last_time = now;
  const uint16_t pressed = uint16_t(pad.buttons & ~last_buttons);
  last_buttons = pad.buttons;
  if (!g_active.load()) return;
  // The replay ran out (the kept frames gone: a new run): leave.
  if (!live::ReplayPlaying()) {
    Leave();
    return;
  }
  Keys keys;
  if (!freecam::Active()) {
    std::lock_guard lock(g_keys_mutex);
    keys = g_keys;
    g_keys.toggle = g_keys.restart = g_keys.leave = false;
    g_keys.speed_step = 0;
  } else {
    return;  // the freecam has the controller
  }
  using namespace rex::input;
  if ((pressed & X_INPUT_GAMEPAD_B) || keys.leave) {
    Leave();
    return;
  }
  if (pressed & X_INPUT_GAMEPAD_BACK) help_bar::Toggle();
  const live::ReplayStatus st = live::ReplayGetStatus();
  int step = keys.speed_step;
  if (pressed & X_INPUT_GAMEPAD_LEFT_SHOULDER) --step;
  if (pressed & X_INPUT_GAMEPAD_RIGHT_SHOULDER) ++step;
  if (step) live::ReplayControl(kSpeeds[std::clamp(SpeedIndex(st.speed) + step, 0, 4)]);
  if ((pressed & X_INPUT_GAMEPAD_Y) || keys.restart) live::ReplayControl(0.0f, 0, 0.0);
  if ((pressed & X_INPUT_GAMEPAD_A) || keys.toggle) live::ReplayControl(0.0f, 2);
  // Scrubbing: held triggers / arrows move the position (paused meanwhile).
  const float scrub = std::clamp(float(pad.right_trigger) / 255.0f -
                                     float(pad.left_trigger) / 255.0f + keys.scrub,
                                 -1.0f, 1.0f);
  if (std::fabs(scrub) > 0.15f) {
    const double pos = std::clamp(st.position + double(scrub) * 3.0 * dt, 0.0, st.length);
    live::ReplayControl(0.0f, 1, pos);
  }
}

}  // namespace amped3::replay
