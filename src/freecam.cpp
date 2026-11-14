// amped3 - freecam (freecam.h).

#include "freecam.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <memory>
#include <cstring>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "graphics_panel.h"
#include "help_bar.h"
#include "replay.h"
#include "live_renderer.h"
#include "notice.h"

namespace amped3::freecam {
namespace {

using Clock = std::chrono::steady_clock;

Host g_host;
std::atomic<bool> g_active{false};

// Two ways to fly. Noclip (the default): the game's own camera is moved (the
// hooks below, in the game's camera setup), so the game draws, culls and
// lights for the freecam: the whole world is there. View: the renderer
// reprojects what the game drew from its own camera (src/native/renderer.cpp
// SetFreecam): the game's camera stays put (it keeps following the rider),
// so only what it sees is there; it zooms.
enum class Mode { kNoclip, kView };
std::atomic<Mode> g_mode{Mode::kNoclip};

// Noclip: the input (from OnPad) and the camera's pose (the game thread, in
// the hooks). Axes as the game's view matrix has them: right, up, back.
struct Noclip {
  std::mutex mutex;
  float move[3] = {};
  float look[2] = {};
  float look_delta[2] = {};
  float speed = 8.0f;
  bool reset = false;
  bool have_entry = false;
  double right0[3] = {}, up0[3] = {}, back0[3] = {};  // at entry
  double yaw_sign = 1.0, pitch_sign = 1.0;
  double p[3] = {}, yaw = 0.0, pitch = 0.0;
  double right[3] = {}, up[3] = {}, back[3] = {};  // now
  Clock::time_point last{};
  bool posed = false;  // this frame's pose set (the second hook uses it)
} g_noclip;
std::atomic<bool> g_noclip_on{false};
// Noclip, the game paused: its camera setup doesn't run, so the freecam is
// the view mode's around where the noclip camera last was (what the game
// drew last). The camera setup's last run, for telling.
std::atomic<int64_t> g_noclip_hook_ns{0};
std::atomic<bool> g_paused_view{false};
// L3: faster, until the left stick lets go.
bool g_boost = false;

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
  bool switch_mode = false;
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
    const bool noclip = g_mode.load() == Mode::kNoclip;
    const bool paused = noclip && g_paused_view.load();
    std::string title;
    if (!noclip && !live::FreecamShowing()) {
      title = "FREECAM: waiting for a 3D scene";
    } else if (paused) {
      title = "FREECAM: NOCLIP - game paused: looking around the last spot";
    } else {
      title = noclip ? "FREECAM: NOCLIP" : "FREECAM: VIEW (around what the game drew)";
    }
    std::vector<std::string> items = {
        "Left stick / W A S D: move",
        "L3 / Shift: faster (until the stick lets go)",
        "X / Alt: slower",
        "Right stick / arrows / right mouse: look",
        "LT RT / Q E: down, up",
    };
    if (!noclip || paused) items.push_back("LB RB / Z C: zoom");
    items.push_back("Y / R: back to the game's camera");
    items.push_back(noclip ? "D-pad down / N: view mode" : "D-pad down / N: noclip mode");
    items.push_back("View / H: hide controls");
    items.push_back("B / L3+R3 / F6: leave");
    help_bar::Draw(io, title, items, replay::Active() ? replay::BarRows() : 0);
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
    const bool switch_mode = g_keys.switch_mode || ImGui::IsKeyPressed(ImGuiKey_N, false);
    if (ImGui::IsKeyPressed(ImGuiKey_H, false)) help_bar::Toggle();
    float mouse[2] = {g_keys.mouse[0], g_keys.mouse[1]};
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
      mouse[0] += io.MouseDelta.x * 0.003f;
      mouse[1] -= io.MouseDelta.y * 0.003f;
    }
    g_keys = k;
    g_keys.reset = reset;
    g_keys.leave = leave;
    g_keys.switch_mode = switch_mode;
    g_keys.mouse[0] = mouse[0];
    g_keys.mouse[1] = mouse[1];
  }
};

std::unique_ptr<Overlay> g_overlay;  // UI thread, while on

// What the renderer gets: in noclip only the HUD left out.
void SendRenderer(native::Renderer::FreecamInput in) {
  if (g_mode.load() == Mode::kNoclip && !g_paused_view.load()) {
    native::Renderer::FreecamInput hud;
    hud.hide_hud = true;
    live::SetFreecam(hud);
  } else {
    live::SetFreecam(in);
  }
}

void SetOn(bool on) {
  if (on == g_active.load()) return;
  if (on && !live::Enabled()) {
    notice::Show("Freecam needs the native renderer");
    return;
  }
  g_active = on;
  REXLOG_INFO("Freecam {} ({})", on ? "on" : "off",
              g_mode.load() == Mode::kNoclip ? "noclip" : "view");
  if (g_host.block_game_input) g_host.block_game_input(on);
  {
    std::lock_guard lock(g_noclip.mutex);
    g_noclip.have_entry = false;  // from the game's camera each time
    g_noclip.reset = false;
  }
  g_noclip_on = on && g_mode.load() == Mode::kNoclip;
  g_paused_view = false;
  g_boost = false;
  g_noclip_hook_ns = NowNs();
  native::Renderer::FreecamInput in;
  in.active = on;
  in.reset = on;  // from the game's camera each time
  if (on) {
    SendRenderer(in);
  } else {
    live::SetFreecam(in);
  }
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
    g_keys.reset = g_keys.leave = g_keys.switch_mode = false;
  }
  if ((pressed & X_INPUT_GAMEPAD_B) || keys.leave) {
    SetOn(false);
    return;
  }
  if (pressed & X_INPUT_GAMEPAD_BACK) help_bar::Toggle();
  // Noclip with the game paused (its camera setup idle for a while): the
  // view mode around the last spot, until it runs again.
  if (g_mode.load() == Mode::kNoclip) {
    const int64_t idle = NowNs() - g_noclip_hook_ns.load();
    const bool was = g_paused_view.load();
    const bool now_paused = was ? idle > 60'000'000 : idle > 250'000'000;
    if (now_paused != was) {
      g_paused_view = now_paused;
      native::Renderer::FreecamInput fresh;
      fresh.active = now_paused;
      fresh.reset = true;  // from the last noclip view (what the game drew)
      live::SetFreecam(native::Renderer::FreecamInput{});
      if (now_paused) live::SetFreecam(fresh);
    }
  } else {
    g_paused_view = false;
  }
  if ((pressed & X_INPUT_GAMEPAD_DPAD_DOWN) || keys.switch_mode) {
    const Mode m = g_mode.load() == Mode::kNoclip ? Mode::kView : Mode::kNoclip;
    g_mode = m;
    {
      std::lock_guard lock(g_noclip.mutex);
      g_noclip.have_entry = false;
    }
    g_noclip_on = m == Mode::kNoclip;
    native::Renderer::FreecamInput fresh;
    fresh.active = true;
    fresh.reset = true;
    live::SetFreecam(native::Renderer::FreecamInput{});  // the renderer's starts over
    SendRenderer(fresh);
    notice::Show(m == Mode::kNoclip ? "Freecam: noclip (the game draws from it)"
                                    : "Freecam: view (around what the game drew)");
    return;
  }
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
  // L3 (alone: with R3 it leaves): faster until the left stick lets go.
  const bool stick_moving = Stick(pad.lx) != 0.0f || Stick(pad.ly) != 0.0f;
  if ((pressed & X_INPUT_GAMEPAD_LEFT_THUMB) && !(pad.buttons & X_INPUT_GAMEPAD_RIGHT_THUMB)) {
    g_boost = !g_boost;
  }
  if (!stick_moving && !(pressed & X_INPUT_GAMEPAD_LEFT_THUMB)) g_boost = false;
  float scale = keys.speed_scale;
  if (g_boost) scale *= 4.0f;
  if (pad.buttons & X_INPUT_GAMEPAD_X) scale *= 0.25f;
  in.speed = 8.0f * scale;
  if (g_mode.load() == Mode::kNoclip) {
    std::lock_guard lock(g_noclip.mutex);
    std::memcpy(g_noclip.move, in.move, sizeof(in.move));
    std::memcpy(g_noclip.look, in.look, sizeof(in.look));
    g_noclip.look_delta[0] += in.look_delta[0];
    g_noclip.look_delta[1] += in.look_delta[1];
    g_noclip.speed = in.speed;
    g_noclip.reset |= in.reset;
  }
  SendRenderer(in);
}


// --- Noclip: the game's camera setup (sub_824B4660, once a frame) ----------
//
// It reads the camera's view matrix into r31+512 (0x8243B878), its
// projection (0x8243B940), its position into r29 r28 r26 and r31+160
// (0x821777C8) and its forward vector into r4 r25 r3 (0x82216560), then copies
// them into the render views (0x828A7850 + 352 n), multiplies view by
// projection and hands the camera on (0x82467C00 -> 0x828A76E0). The hooks set
// the view (0x824B4740, after its copy) and the position and forward
// (0x824B47BC, after the forward's loads) to the freecam's, before any of that.

namespace {

rex::memory::Memory* NoclipMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

double LoadF(const uint8_t* p) {
  const uint32_t b = rex::memory::load_and_swap<uint32_t>(p);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

void StoreF(uint8_t* p, double v) {
  const float f = float(v);
  uint32_t b;
  std::memcpy(&b, &f, 4);
  rex::memory::store_and_swap<uint32_t>(p, b);
}

uint32_t Bits(double v) {
  const float f = float(v);
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

void Rotate(const double axis[3], double a, const double v[3], double out[3]) {
  // Rodrigues.
  const double c = std::cos(a), s = std::sin(a);
  const double d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
  const double x[3] = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2],
                       axis[0] * v[1] - axis[1] * v[0]};
  for (int i = 0; i < 3; ++i) out[i] = v[i] * c + x[i] * s + axis[i] * d * (1.0 - c);
}

}  // namespace
}  // namespace amped3::freecam

// After the view matrix is copied to r31+512.
void Amped3_NoclipView(PPCRegister& r31) {
  using namespace amped3::freecam;
  if (!g_noclip_on.load(std::memory_order_relaxed)) {
    g_noclip.posed = false;
    return;
  }
  g_noclip_hook_ns = NowNs();
  auto* memory = NoclipMemory();
  if (!memory) return;
  uint8_t* v = memory->TranslateVirtual(r31.u32 + 512);
  std::lock_guard lock(g_noclip.mutex);
  Noclip& n = g_noclip;
  const auto now = Clock::now();
  double dt = n.last == Clock::time_point{} ? 0.0 : std::chrono::duration<double>(now - n.last).count();
  n.last = now;
  dt = std::clamp(dt, 0.0, 0.1);
  if (!n.have_entry || n.reset) {
    // The game's camera: axes from the view matrix's columns (row vectors:
    // world -> view is p M), the position from its translation row.
    double m[16];
    for (int i = 0; i < 16; ++i) m[i] = LoadF(v + i * 4);
    for (int i = 0; i < 3; ++i) {
      n.right0[i] = m[i * 4 + 0];
      n.up0[i] = m[i * 4 + 1];
      n.back0[i] = m[i * 4 + 2];
    }
    for (int i = 0; i < 3; ++i) {
      n.p[i] = -(m[12] * n.right0[i] + m[13] * n.up0[i] + m[14] * n.back0[i]);
    }
    // Which way a positive angle turns: right, and up.
    const double world_up[3] = {0, 1, 0};
    const double fwd[3] = {-n.back0[0], -n.back0[1], -n.back0[2]};
    double t[3];
    Rotate(world_up, 0.1, fwd, t);
    n.yaw_sign = t[0] * n.right0[0] + t[1] * n.right0[1] + t[2] * n.right0[2] > 0 ? 1.0 : -1.0;
    Rotate(n.right0, 0.1, fwd, t);
    n.pitch_sign = t[1] > fwd[1] ? 1.0 : -1.0;
    n.yaw = n.pitch = 0.0;
    n.have_entry = true;
    n.reset = false;
  }
  // Moved.
  const double kTurn = 1.8;
  n.yaw += double(n.look[0]) * kTurn * dt + n.look_delta[0];
  n.pitch += double(n.look[1]) * kTurn * dt + n.look_delta[1];
  n.look_delta[0] = n.look_delta[1] = 0.0f;
  const double e0 = std::asin(std::clamp(-n.back0[1], -1.0, 1.0));
  n.pitch = std::clamp(n.pitch, -1.45 - e0, 1.45 - e0);
  const double world_up[3] = {0, 1, 0};
  double r1[3], u1[3], b1[3];
  Rotate(n.right0, n.pitch_sign * n.pitch, n.right0, r1);
  Rotate(n.right0, n.pitch_sign * n.pitch, n.up0, u1);
  Rotate(n.right0, n.pitch_sign * n.pitch, n.back0, b1);
  Rotate(world_up, n.yaw_sign * n.yaw, r1, n.right);
  Rotate(world_up, n.yaw_sign * n.yaw, u1, n.up);
  Rotate(world_up, n.yaw_sign * n.yaw, b1, n.back);
  const double step = double(n.speed) * dt;
  for (int i = 0; i < 3; ++i) {
    n.p[i] += (n.right[i] * n.move[0] + world_up[i] * n.move[1] - n.back[i] * n.move[2]) * step;
  }
  // The view matrix: the axes as columns, the translation row -p . axis.
  for (int i = 0; i < 3; ++i) {
    StoreF(v + (i * 4 + 0) * 4, n.right[i]);
    StoreF(v + (i * 4 + 1) * 4, n.up[i]);
    StoreF(v + (i * 4 + 2) * 4, n.back[i]);
    StoreF(v + (i * 4 + 3) * 4, 0.0);
  }
  auto dot = [&](const double a[3]) { return a[0] * n.p[0] + a[1] * n.p[1] + a[2] * n.p[2]; };
  StoreF(v + 12 * 4, -dot(n.right));
  StoreF(v + 13 * 4, -dot(n.up));
  StoreF(v + 14 * 4, -dot(n.back));
  StoreF(v + 15 * 4, 1.0);
  n.posed = true;
}

// After the forward vector's loads: the position (registers and r31+160) and
// the forward vector, the freecam's.
void Amped3_NoclipPose(PPCRegister& r31, PPCRegister& r29, PPCRegister& r28, PPCRegister& r26,
                       PPCRegister& r4, PPCRegister& r25, PPCRegister& r3) {
  using namespace amped3::freecam;
  if (!g_noclip_on.load(std::memory_order_relaxed)) return;
  auto* memory = NoclipMemory();
  if (!memory) return;
  std::lock_guard lock(g_noclip.mutex);
  const Noclip& n = g_noclip;
  if (!n.posed) return;
  r29.u64 = Bits(n.p[0]);
  r28.u64 = Bits(n.p[1]);
  r26.u64 = Bits(n.p[2]);
  uint8_t* pos = memory->TranslateVirtual(r31.u32 + 160);
  for (int i = 0; i < 3; ++i) StoreF(pos + i * 4, n.p[i]);
  r4.u64 = Bits(-n.back[0]);
  r25.u64 = Bits(-n.back[1]);
  r3.u64 = Bits(-n.back[2]);
}
