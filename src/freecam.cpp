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

#include <rex/cvar.h>
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
#include "night_lights.h"
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
  // Where to start (position, forward) once the entry is taken: the view
  // mode's pose, so switching doesn't jump back to the game's camera.
  bool have_target = false;
  double target[6] = {};
  // The last poses set (newest at hist_at - 1): for telling when a frame the
  // game drew from the noclip camera has reached the screen.
  double hist[16][3] = {};
  int hist_at = 0, hist_n = 0;
} g_noclip;
// To noclip: frames already on their way were drawn from the game's own
// camera (the rider's), so the view mode stays on, at the noclip camera,
// until the game's first noclip frame is drawn (no one-frame flash back).
std::atomic<bool> g_handover{false};
std::atomic<int64_t> g_handover_ns{0};
std::atomic<bool> g_noclip_on{false};
// Noclip, the game paused: its camera setup doesn't run, so the freecam is
// the view mode's around where the noclip camera last was (what the game
// drew last). The camera setup's last run, for telling.
std::atomic<int64_t> g_noclip_hook_ns{0};
std::atomic<bool> g_paused_view{false};
constexpr int kStatePaused = 15;  // the game state machine: its pause menu
// L3: faster, until the left stick lets go.
bool g_boost = false;

// The keyboard and mouse, from the overlay (UI thread), for OnPad.
std::mutex g_keys_mutex;
// What the light editor's D-pad changes (night_lights::Mode).
const char* const kDpadModes[] = {"turn, tilt", "width, height (windows, panels)", "reach, spread",
                                  "color, brightness"};

struct Keys {
  float move[3] = {};
  float look[2] = {};
  float zoom = 0.0f;
  float speed_scale = 1.0f;
  float mouse[2] = {};  // radians, added up until OnPad hands them on
  bool reset = false;
  bool leave = false;
  bool switch_mode = false;
  // Night lights (time of day): place, remove, the kind to place.
  // The light editor (keyboard): L on / off, Enter place, O on every copy,
  // J kind, Delete delete, , . turn, Page Up / Down pick, Backspace back,
  // Ctrl+Z undo.
  bool ed_toggle = false, ed_place = false, ed_every = false, ed_kind = false, ed_delete = false,
       ed_back = false, ed_undo = false, ed_color = false, ed_surface = false;  // M: the surface picker
  int ed_turn = 0, ed_pick = 0, ed_tilt = 0;
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
    if (replay::Active()) {
      title = "FREECAM: REPLAY (around what the replay drew)";
    } else
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
    const bool editor = night_lights::EditorOn();
    if (editor) {
      // The light editor's own controls (the camera's stay as they are).
      const bool picked = night_lights::HasSelection();
      title = "LIGHT EDITOR";
      items = {
          "Sticks / LT RT: fly",
          "A / Enter: place a " + night_lights::KindName(),
          "Y / O: on every copy of the object",
          picked ? "X / Delete: delete it" : "X / J: kind",
          std::string("D-pad / , . [ ]: ") + kDpadModes[night_lights::Mode()],
          "R3 / K: what the D-pad changes",
          "LB RB / Page Up Down: pick a placed light",
          "Start / Ctrl+Z: undo",
          "M: the surface under the cross (again: change it)",
          picked ? "B / Backspace: unpick" : "B / Backspace: done",
          "View / H: hide controls",
      };
    } else if (live::TimeOfDayHour() >= 0.0f && !replay::Active()) {
      items.push_back("D-pad up / L: light editor (night lights)");
    }
    if (!editor) {
      items.push_back("View / H: hide controls");
      items.push_back("B / tap L3+R3 / F6: leave");
    }
    help_bar::Draw(io, title, items, replay::Active() ? replay::BarRows() : 0);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (editor) {
      // A card at the top: what's being placed (or what's picked) and how
      // many lights the mountain has.
      const float px = std::max(14.0f, h * 0.024f);
      ImFont* font = amped3::UiFont(px);
      const std::string picked = night_lights::SelectionName();
      const std::string line1 = picked.empty() ? "Placing: " + night_lights::KindName()
                                               : "Picked: " + picked;
      const std::string line2 = (picked.empty() ? night_lights::PreviewDetails() + "   " : std::string()) +
                                std::to_string(night_lights::Count()) + " lights on this mountain";
      const float tw = std::max(font->CalcTextSizeA(px, FLT_MAX, 0, line1.c_str()).x,
                                font->CalcTextSizeA(px * 0.75f, FLT_MAX, 0, line2.c_str()).x);
      const ImVec2 a(w * 0.5f - tw * 0.5f - px, h * 0.06f), b(w * 0.5f + tw * 0.5f + px, h * 0.06f + px * 2.4f);
      dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 150), px * 0.35f);
      dl->AddRect(a, b, picked.empty() ? IM_COL32(90, 200, 255, 200) : IM_COL32(255, 210, 60, 220),
                  px * 0.35f, 0, 2.0f);
      dl->AddText(font, px, ImVec2(w * 0.5f - font->CalcTextSizeA(px, FLT_MAX, 0, line1.c_str()).x * 0.5f,
                                   a.y + px * 0.25f),
                  IM_COL32(255, 255, 255, 240), line1.c_str());
      dl->AddText(font, px * 0.75f,
                  ImVec2(w * 0.5f - font->CalcTextSizeA(px * 0.75f, FLT_MAX, 0, line2.c_str()).x * 0.5f,
                         a.y + px * 1.4f),
                  IM_COL32(200, 200, 200, 220), line2.c_str());
    }
    // A cross at the middle: where a light goes (in the editor, always).
    if (editor || (live::TimeOfDayHour() >= 0.0f && help_bar::Visible())) {
      const ImVec2 c(w * 0.5f, h * 0.5f);
      const float r = std::max(6.0f, h * 0.008f);
      for (int pass = 0; pass < 2; ++pass) {
        const ImU32 col = pass ? (editor ? IM_COL32(120, 210, 255, 240) : IM_COL32(255, 255, 255, 220))
                               : IM_COL32(0, 0, 0, 160);
        const float t = pass ? 1.5f : 3.5f;
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.35f, c.y), col, t);
        dl->AddLine(ImVec2(c.x + r * 0.35f, c.y), ImVec2(c.x + r, c.y), col, t);
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y - r * 0.35f), col, t);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.35f), ImVec2(c.x, c.y + r), col, t);
      }
    }
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
    auto hit = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, false); };
    Keys ed;
    ed.ed_toggle = g_keys.ed_toggle || hit(ImGuiKey_L);
    ed.ed_place = g_keys.ed_place || hit(ImGuiKey_Enter) || hit(ImGuiKey_KeypadEnter);
    ed.ed_every = g_keys.ed_every || hit(ImGuiKey_O);
    ed.ed_kind = g_keys.ed_kind || hit(ImGuiKey_J);
    ed.ed_delete = g_keys.ed_delete || hit(ImGuiKey_Delete);
    ed.ed_back = g_keys.ed_back || hit(ImGuiKey_Backspace);
    ed.ed_undo = g_keys.ed_undo || (io.KeyCtrl && hit(ImGuiKey_Z));
    ed.ed_turn = g_keys.ed_turn + int(hit(ImGuiKey_Period)) - int(hit(ImGuiKey_Comma));
    ed.ed_pick = g_keys.ed_pick + int(hit(ImGuiKey_PageDown)) - int(hit(ImGuiKey_PageUp));
    ed.ed_tilt = g_keys.ed_tilt + int(hit(ImGuiKey_RightBracket)) - int(hit(ImGuiKey_LeftBracket));
    ed.ed_color = g_keys.ed_color || hit(ImGuiKey_K);
    ed.ed_surface = g_keys.ed_surface || hit(ImGuiKey_M);
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
    g_keys.ed_toggle = ed.ed_toggle;
    g_keys.ed_place = ed.ed_place;
    g_keys.ed_every = ed.ed_every;
    g_keys.ed_kind = ed.ed_kind;
    g_keys.ed_delete = ed.ed_delete;
    g_keys.ed_back = ed.ed_back;
    g_keys.ed_undo = ed.ed_undo;
    g_keys.ed_turn = ed.ed_turn;
    g_keys.ed_pick = ed.ed_pick;
    g_keys.ed_tilt = ed.ed_tilt;
    g_keys.ed_color = ed.ed_color;
    g_keys.ed_surface = ed.ed_surface;
    g_keys.mouse[0] = mouse[0];
    g_keys.mouse[1] = mouse[1];
  }
};

std::unique_ptr<Overlay> g_overlay;  // UI thread, while on

// The renderer's freecam placed at the noclip camera's pose (no jump back to
// the game's camera when the view mode takes over).
void ViewFromNoclip(native::Renderer::FreecamInput& in) {
  std::lock_guard lock(g_noclip.mutex);
  if (!g_noclip.have_entry) return;
  in.set_pose = true;
  for (int i = 0; i < 3; ++i) {
    in.pose[i] = float(g_noclip.p[i]);
    in.pose[3 + i] = float(-g_noclip.back[i]);
  }
}

// The noclip camera to start at the renderer freecam's pose.
void NoclipFromView() {
  float pose[6];
  const bool have = live::FreecamPose(pose);
  std::lock_guard lock(g_noclip.mutex);
  g_noclip.have_entry = false;
  g_noclip.posed = false;
  g_noclip.have_target = have;
  for (int i = 0; i < 6; ++i) g_noclip.target[i] = pose[i];
}

// A frame drawn from the noclip camera has been shown (the renderer's last
// frame's camera is one the noclip hook set).
bool NoclipDrawn() {
  float cam[9];
  if (!live::GameCamera(cam)) return false;
  std::lock_guard lock(g_noclip.mutex);
  for (int k = 0; k < g_noclip.hist_n; ++k) {
    const double* h = g_noclip.hist[k];
    const double dx = cam[0] - h[0], dy = cam[1] - h[1], dz = cam[2] - h[2];
    if (dx * dx + dy * dy + dz * dz < 0.02 * 0.02) return true;
  }
  return false;
}

void StartHandover() {
  {
    std::lock_guard lock(g_noclip.mutex);
    g_noclip.hist_n = g_noclip.hist_at = 0;
  }
  g_handover_ns = NowNs();
  g_handover = true;
}

// What the renderer gets: in noclip only the HUD left out.
void SendRenderer(native::Renderer::FreecamInput in) {
  if (g_mode.load() == Mode::kNoclip && !g_paused_view.load()) {
    if (g_handover.load()) {
      if (NoclipDrawn() || NowNs() - g_handover_ns.load() > 1'000'000'000) {
        g_handover = false;
      } else {
        // Held at the noclip camera (it moves there; the renderer doesn't).
        native::Renderer::FreecamInput hold;
        hold.active = true;
        hold.speed = 0.0f;
        ViewFromNoclip(hold);
        live::SetFreecam(hold);
        return;
      }
    }
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
  // The rider back in control: the light editor goes off with the freecam
  // (B, F6, L3+R3, or anything else that ends it).
  if (!on && night_lights::EditorOn()) night_lights::SetEditor(false);
  REXLOG_INFO("Freecam {} ({})", on ? "on" : "off",
              g_mode.load() == Mode::kNoclip ? "noclip" : "view");
  if (g_host.block_game_input) g_host.block_game_input(on);
  {
    std::lock_guard lock(g_noclip.mutex);
    g_noclip.have_entry = false;  // from the game's camera each time
    g_noclip.reset = false;
    g_noclip.have_target = false;
    g_noclip.posed = false;
  }
  // In a replay (the game paused), the view mode around the replay.
  g_paused_view = on && g_mode.load() == Mode::kNoclip && replay::Active();
  g_noclip_on = on && g_mode.load() == Mode::kNoclip && !g_paused_view.load();
  g_handover = false;
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

// The surface picker (light editor, M): the first press looks up the
// texture under the cross (the sun's sheen's surface for it: guessed from
// its colors, or set); pressed again while that's shown, the next surface,
// kept in native_materials. Called every tick: the look-up's answer too.
constexpr const char* kSurfaceNames[] = {"none", "snow", "metal", "wood", "rock", "paint", "cloth"};
std::string SurfaceKey(bool pressed) {
  static bool looking = false;
  static uint64_t hash = 0;
  static int surface = 0;  // as now (set, or the guess)
  static bool set = false;
  static int64_t shown_ns = 0;
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  auto hex = [](uint64_t h) {
    char b[24];
    std::snprintf(b, sizeof(b), "%016llX", (unsigned long long)h);
    return std::string(b);
  };
  // The list as it stands: HASH=surface entries.
  auto entries = []() {
    std::vector<std::pair<std::string, std::string>> out;
    const std::string s = rex::cvar::GetFlagByName("native_materials");
    size_t at = 0;
    while (at < s.size()) {
      size_t end = s.find_first_of(", ;", at);
      if (end == std::string::npos) end = s.size();
      const std::string tok = s.substr(at, end - at);
      at = end + 1;
      const size_t eq = tok.find('=');
      if (eq != std::string::npos) out.emplace_back(tok.substr(0, eq), tok.substr(eq + 1));
    }
    return out;
  };
  if (looking) {
    uint64_t h = 0;
    int guess = 0;
    const int r = live::SurfacePickResult(&h, &guess);
    if (r == -1) return {};
    looking = false;
    if (r != 1) return "Nothing to sort there (the sky, or a rider)";
    hash = h;
    surface = guess;
    set = false;
    for (const auto& [k, v] : entries()) {
      if (k.size() >= 16 && std::strtoull(k.substr(k.size() - 16).c_str(), nullptr, 16) == h) {
        for (int i = 0; i < 7; ++i) {
          if (v == kSurfaceNames[i]) {
            surface = i;
            set = true;
          }
        }
      }
    }
    shown_ns = now;
    return "Texture " + hex(hash) + ": " + kSurfaceNames[surface] + (set ? " (set)" : " (guessed)") +
           "  -  M again to change";
  }
  if (!pressed) return {};
  // Again, while it's shown: the next surface, kept.
  if (hash && now - shown_ns < 20'000'000'000) {
    surface = (surface + 1) % 7;
    set = true;
    shown_ns = now;
    auto list = entries();
    const std::string key = hex(hash);
    bool found = false;
    for (auto& [k, v] : list) {
      if (k.size() >= 16 && k.substr(k.size() - 16) == key) {
        v = kSurfaceNames[surface];
        found = true;
      }
    }
    if (!found) list.emplace_back(key, kSurfaceNames[surface]);
    std::string text;
    for (const auto& [k, v] : list) text += (text.empty() ? "" : ", ") + k + "=" + v;
    rex::cvar::SetFlagByName("native_materials", text);
    if (g_host.save_settings) g_host.save_settings();
    return "Texture " + key + ": " + kSurfaceNames[surface] + " (set)  -  M again to change";
  }
  live::PickSurface();
  looking = true;
  hash = 0;
  return "Looking at the surface under the cross...";
}

}  // namespace

void Install(Host host) { g_host = std::move(host); }

void Toggle() { SetOn(!g_active.load()); }

bool Active() { return g_active.load(); }

void OnPad(const pad::Snapshot& pad) {
  static uint16_t last_buttons = 0;
  uint16_t pressed = uint16_t(pad.buttons & ~last_buttons);
  last_buttons = pad.buttons;
  if (!g_active.load()) {
    if (night_lights::EditorOn()) night_lights::SetEditor(false);  // never without the freecam
    return;
  }
  using namespace rex::input;
  Keys keys;
  {
    std::lock_guard lock(g_keys_mutex);
    keys = g_keys;
    g_keys.mouse[0] = g_keys.mouse[1] = 0.0f;
    g_keys.reset = g_keys.leave = g_keys.switch_mode = false;
    g_keys.ed_toggle = g_keys.ed_place = g_keys.ed_every = g_keys.ed_kind = g_keys.ed_delete =
        g_keys.ed_back = g_keys.ed_undo = g_keys.ed_color = g_keys.ed_surface = false;
    g_keys.ed_turn = g_keys.ed_pick = g_keys.ed_tilt = 0;
  }
  // The light editor (time of day on; not in a replay: the replay's frames
  // aren't where the camera is): D-pad up (L) on and off. While on, it has
  // A, B, X, Y, the bumpers and the D-pad.
  const bool can_edit = live::TimeOfDayHour() >= 0.0f && !replay::Active();
  if (!can_edit && night_lights::EditorOn()) night_lights::SetEditor(false);
  // (On: D-pad up, which then tilts; off: B, or L.)
  if (can_edit && (((pressed & X_INPUT_GAMEPAD_DPAD_UP) && !night_lights::EditorOn()) || keys.ed_toggle)) {
    const bool on = !night_lights::EditorOn();
    night_lights::SetEditor(on);
    notice::Show(on ? "Light editor: aim the cross where a light goes" : "Light editor off");
    pressed &= uint16_t(~X_INPUT_GAMEPAD_DPAD_UP);
  }
  uint16_t buttons = pad.buttons;
  if (night_lights::EditorOn()) {
    constexpr uint16_t kEditor = X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_X |
                                 X_INPUT_GAMEPAD_Y | X_INPUT_GAMEPAD_LEFT_SHOULDER |
                                 X_INPUT_GAMEPAD_RIGHT_SHOULDER | X_INPUT_GAMEPAD_DPAD_UP |
                                 X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_DPAD_LEFT |
                                 X_INPUT_GAMEPAD_DPAD_RIGHT | X_INPUT_GAMEPAD_START |
                                 X_INPUT_GAMEPAD_RIGHT_THUMB;
    const bool picked = night_lights::HasSelection();
    std::string say;
    if ((pressed & X_INPUT_GAMEPAD_A) || keys.ed_place) say = night_lights::Place();
    if ((pressed & X_INPUT_GAMEPAD_Y) || keys.ed_every) say = night_lights::PlaceOnEveryCopy();
    if (pressed & X_INPUT_GAMEPAD_X) {
      say = picked ? night_lights::DeleteSelected() : night_lights::NextKind(1);
    }
    if (keys.ed_kind) say = night_lights::NextKind(1);
    if (keys.ed_delete) say = night_lights::DeleteSelected();
    // The D-pad: left / right and up / down, for what it changes now.
    const int lr = keys.ed_turn + ((pressed & X_INPUT_GAMEPAD_DPAD_RIGHT) ? 1 : 0) -
                   ((pressed & X_INPUT_GAMEPAD_DPAD_LEFT) ? 1 : 0);
    const int ud = keys.ed_tilt + ((pressed & X_INPUT_GAMEPAD_DPAD_UP) ? 1 : 0) -
                   ((pressed & X_INPUT_GAMEPAD_DPAD_DOWN) ? 1 : 0);
    switch (night_lights::Mode()) {
      case 0:
        if (lr) say = night_lights::Rotate(15.0f * float(lr));
        if (ud) say = night_lights::TiltBy(10.0f * float(ud));
        break;
      case 1:
        if (lr || ud) say = night_lights::Resize(lr, ud);
        break;
      case 2:
        if (lr) say = night_lights::Reach(lr);
        if (ud) say = night_lights::Spread(ud);
        break;
      default:
        if (lr) say = night_lights::NextColor(lr);
        if (ud) say = night_lights::Brightness(ud);
    }
    // (R3 alone: with L3 it's the freecam's own.)
    if (((pressed & X_INPUT_GAMEPAD_RIGHT_THUMB) && !(pad.buttons & X_INPUT_GAMEPAD_LEFT_THUMB)) ||
        keys.ed_color) {
      say = night_lights::NextMode(1);
    }
    const int pick = keys.ed_pick + ((pressed & X_INPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 0) -
                     ((pressed & X_INPUT_GAMEPAD_LEFT_SHOULDER) ? 1 : 0);
    if (pick) say = night_lights::Select(pick);
    if ((pressed & X_INPUT_GAMEPAD_START) || keys.ed_undo) say = night_lights::Undo();
    {
      std::string s = SurfaceKey(keys.ed_surface);
      if (!s.empty()) say = s;
    }
    if ((pressed & X_INPUT_GAMEPAD_B) || keys.ed_back) {
      if (picked) {
        say = night_lights::Deselect();
      } else {
        night_lights::SetEditor(false);
        say = "Light editor off";
      }
    }
    if (!say.empty()) notice::Show(say);
    pressed &= uint16_t(~kEditor);
    buttons &= uint16_t(~kEditor);
  }
  if ((pressed & X_INPUT_GAMEPAD_B) || keys.leave) {
    SetOn(false);  // (the editor with it)
    return;
  }
  if (pressed & X_INPUT_GAMEPAD_BACK) help_bar::Toggle();
  // Noclip with the game paused (its camera setup idle for a while): the
  // view mode around the last spot, until it runs again.
  // In a replay or the game's pause menu, too (the camera the game moves
  // isn't the one drawn then).
  if (g_mode.load() == Mode::kNoclip) {
    const int64_t idle = NowNs() - g_noclip_hook_ns.load();
    const bool was = g_paused_view.load();
    const bool held = replay::Active() || live::GameState() == kStatePaused;
    const bool now_paused = held || (was ? idle > 60'000'000 : idle > 250'000'000);
    if (now_paused != was) {
      g_paused_view = now_paused;
      if (now_paused) {
        native::Renderer::FreecamInput fresh;
        fresh.active = true;
        fresh.reset = true;
        // At the noclip camera (not in a replay: there, around what it drew).
        if (!replay::Active()) ViewFromNoclip(fresh);
        live::SetFreecam(native::Renderer::FreecamInput{});
        live::SetFreecam(fresh);
      } else {
        NoclipFromView();  // on from where the view mode got to
        g_noclip_hook_ns = NowNs();
        StartHandover();  // the view mode until a noclip frame is shown
      }
      g_noclip_on = !now_paused;
    }
  } else {
    g_paused_view = false;
  }
  if ((pressed & X_INPUT_GAMEPAD_DPAD_DOWN) || keys.switch_mode) {
    const Mode m = g_mode.load() == Mode::kNoclip ? Mode::kView : Mode::kNoclip;
    const bool was_view = g_mode.load() == Mode::kView || g_paused_view.load();
    native::Renderer::FreecamInput fresh;
    fresh.active = true;
    fresh.reset = true;
    // The pose carries over: no jump back to the game's camera.
    if (m == Mode::kView && !was_view) ViewFromNoclip(fresh);
    if (m == Mode::kView && was_view) {
      float pose[6];
      if (live::FreecamPose(pose)) {
        fresh.set_pose = true;
        std::memcpy(fresh.pose, pose, sizeof(pose));
      }
    }
    if (m == Mode::kNoclip) NoclipFromView();
    g_mode = m;
    g_paused_view = false;
    g_noclip_hook_ns = NowNs();
    g_noclip_on = m == Mode::kNoclip;
    if (m == Mode::kNoclip) {
      StartHandover();  // the view mode until a noclip frame is shown
    } else {
      g_handover = false;
      live::SetFreecam(native::Renderer::FreecamInput{});  // the renderer's starts over
    }
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
  in.zoom = std::clamp(float(bool(buttons & X_INPUT_GAMEPAD_RIGHT_SHOULDER)) -
                           float(bool(buttons & X_INPUT_GAMEPAD_LEFT_SHOULDER)) + keys.zoom,
                       -1.0f, 1.0f);
  // L3 (alone: with R3 it leaves): faster until the left stick lets go.
  const bool stick_moving = Stick(pad.lx) != 0.0f || Stick(pad.ly) != 0.0f;
  if ((pressed & X_INPUT_GAMEPAD_LEFT_THUMB) && !(pad.buttons & X_INPUT_GAMEPAD_RIGHT_THUMB)) {
    g_boost = !g_boost;
  }
  if (!stick_moving && !(pressed & X_INPUT_GAMEPAD_LEFT_THUMB)) g_boost = false;
  float scale = keys.speed_scale;
  if (g_boost) scale *= 4.0f;
  if (buttons & X_INPUT_GAMEPAD_X) scale *= 0.25f;
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
  g_noclip_hook_ns = NowNs();  // (also while the view mode stands in: how it tells)
  if (!g_noclip_on.load(std::memory_order_relaxed)) {
    g_noclip.posed = false;
    return;
  }
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
    if (n.have_target && !n.reset) {
      // Turned and tilted from the entry to the target's forward (no roll).
      double f[3] = {n.target[3], n.target[4], n.target[5]};
      const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
      if (fl > 1e-6) {
        for (double& c : f) c /= fl;
        const double he_l = std::hypot(fwd[0], fwd[2]), ht_l = std::hypot(f[0], f[2]);
        if (he_l > 1e-6 && ht_l > 1e-6) {
          const double hx = fwd[0] / he_l, hz = fwd[2] / he_l;
          const double tx = f[0] / ht_l, tz = f[2] / ht_l;
          n.yaw = std::atan2(hz * tx - hx * tz, hx * tx + hz * tz) * n.yaw_sign;
        }
        n.pitch = std::asin(std::clamp(f[1], -1.0, 1.0)) -
                  std::asin(std::clamp(fwd[1], -1.0, 1.0));
        for (int i = 0; i < 3; ++i) n.p[i] = n.target[i];
      }
    }
    n.have_target = false;
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
  for (int i = 0; i < 3; ++i) n.hist[n.hist_at][i] = n.p[i];
  n.hist_at = (n.hist_at + 1) % 16;
  n.hist_n = std::min(n.hist_n + 1, 16);
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
