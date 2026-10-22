// amped3 - the Graphics panel (see graphics_panel.h).

#include "graphics_panel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/ui/window.h>

#include "frame_limit.h"
#include "live_renderer.h"
#include "options_menu.h"
#include "pad_bridge.h"

namespace amped3 {

// kStops: a slider over a fixed list of values (`choices`), left to right.
enum class RowKind { kToggle, kSlider, kChoice, kFullscreen, kStops };

struct RowChoice {
  const char* value;
  const char* label;
};

struct GraphicsRow {
  const char* cvar;
  const char* label;
  RowKind kind = RowKind::kToggle;
  double min = 0, max = 0, step = 0;
  std::vector<RowChoice> choices = {};
  bool integer = false;  // an integer slider (written without decimals)
};

namespace {

namespace xi = rex::input;

using Row = GraphicsRow;
using Kind = RowKind;

struct Tab {
  const char* name;
  std::vector<Row> rows;
};

const std::vector<Tab>& Tabs() {
  static const std::vector<Tab> tabs = {
      // Revamped Graphics first; the rest are what it turns on and off.
      {"Effects",
       {
           {"revamped_graphics", "Revamped Graphics"},
           {"native_camera_motion_blur", "Camera Motion Blur"},
           {"native_object_motion_blur", "Object Motion Blur"},
           {"native_camera_motion_blur_strength", "Motion Blur Strength", Kind::kSlider, 0.0, 3.0, 0.1},
           {"native_speed_blur", "Speed Blur"},
           {"native_fog", "Distance Fog"},
           {"native_god_rays", "God Rays"},
           {"native_foliage_fade", "Branch Fade"},
           {"native_map_clouds", "Trail Map Clouds"},
       }},
      {"Display",
       {
           {"fullscreen", "Fullscreen", Kind::kFullscreen},
           {"frame_rate_limit", "Frame Rate", Kind::kStops, 0, 0, 0,
            {{"30", "30 fps"},
             {"40", "40 fps"},
             {"50", "50 fps"},
             {"60", "60 fps"},
             {"72", "72 fps"},
             {"90", "90 fps"},
             {"120", "120 fps"},
             {"144", "144 fps"},
             {"0", "Unlocked"}}},
           {"resolution", "Display Resolution", Kind::kChoice, 0, 0, 0,
            {{"", "Native"}, {"1080p", "1920 x 1080"}, {"1440p", "2560 x 1440"}, {"4k", "3840 x 2160"}}},
           {"native_render_scale", "Render Resolution", Kind::kSlider, 25, 100, 5, {}, true},
           {"native_upscaler", "Upscaler", Kind::kChoice, 0, 0, 0,
#ifdef _WIN32
            {{"off", "Off"}, {"xess", "Intel XeSS"}, {"fsr", "AMD FSR 3.1"}}},
#else
            // Intel ships XeSS for Windows only.
            {{"off", "Off"}, {"fsr", "AMD FSR 3.1"}}},
#endif
           {"native_upscaler_sharpness", "FSR Sharpness", Kind::kSlider, 0.0, 1.0, 0.05},
           {"native_upscaler_msaa", "Upscaler MSAA"},
           // Off by default and not part of Revamped Graphics.
           {"native_sharp_fonts", "Sharp Fonts"},
           {"native_upscaled_textures", "Upscaled Textures"},
       }},
  };
  return tabs;
}

// The rows this build has (a setting the SDK doesn't register is left out).
std::vector<const Row*> VisibleRows(int tab) {
  std::vector<const Row*> rows;
  for (const Row& r : Tabs()[tab].rows) {
    if (rex::cvar::GetFlagInfo(r.cvar)) rows.push_back(&r);
  }
  return rows;
}

bool GetBool(const char* cvar) { return rex::cvar::GetFlagByName(cvar) == "true"; }
double GetDouble(const char* cvar) { return std::atof(rex::cvar::GetFlagByName(cvar).c_str()); }
std::string GetString(const char* cvar) { return rex::cvar::GetFlagByName(cvar); }
bool NativeRendererOn() { return GetBool("native_renderer"); }

// The choice index of the setting's current value (0 if none matches).
int ChoiceIndex(const Row& r) {
  const std::string v = GetString(r.cvar);
  for (size_t i = 0; i < r.choices.size(); ++i) {
    if (v == r.choices[i].value) return int(i);
  }
  return 0;
}

// A stop slider's position: the stop matching the setting (auto counts as the
// rate it stands for), else the nearest one (a rate typed into amped3.toml).
int StopIndex(const Row& r) {
  std::string v = GetString(r.cvar);
  if (std::string_view(r.cvar) == "frame_rate_limit") v = frame_limit::Effective(v);
  for (size_t i = 0; i < r.choices.size(); ++i) {
    if (v == r.choices[i].value) return int(i);
  }
  const double want = std::atof(v.c_str());
  int best = 0;
  double best_d = DBL_MAX;
  for (size_t i = 0; i < r.choices.size(); ++i) {
    double c = std::atof(r.choices[i].value);
    if (c == 0.0) c = 1e9;  // unlocked sits past the highest rate
    const double d = std::abs(c - (want == 0.0 ? 1e9 : want));
    if (d < best_d) best_d = d, best = int(i);
  }
  return best;
}

// The game's menu colors: gold titles and selection, white text.
const ImU32 kGold = IM_COL32(255, 204, 0, 255);
const ImU32 kWhite = IM_COL32(255, 255, 255, 255);
const ImU32 kDim = IM_COL32(255, 255, 255, 110);
const ImU32 kPanel = IM_COL32(8, 14, 34, 225);
const ImU32 kEdge = IM_COL32(255, 204, 0, 170);
const ImU32 kSelect = IM_COL32(255, 204, 0, 48);
const ImU32 kTrack = IM_COL32(255, 255, 255, 60);

// Baked sizes (pixels) of the panel's font, smallest first.
constexpr float kFontSizes[] = {18.0f, 26.0f, 36.0f, 52.0f, 72.0f};
ImFont* g_fonts[sizeof(kFontSizes) / sizeof(kFontSizes[0])] = {};

// The baked font for text `px` high: the smallest at least that size (drawn
// slightly smaller, which stays sharp), else the largest.
ImFont* PanelFont(float px) {
  ImFont* best = nullptr;
  for (size_t i = 0; i < sizeof(kFontSizes) / sizeof(kFontSizes[0]); ++i) {
    if (!g_fonts[i]) continue;
    best = g_fonts[i];
    if (kFontSizes[i] >= px * 0.97f) break;
  }
  return best ? best : ImGui::GetFont();
}

}  // namespace

ImFont* UiFont(float px) { return PanelFont(px); }

void LoadPanelFonts(ImFontAtlas* atlas) {
  // A clean, condensed sans like the game's menu text; fallbacks in order.
  // A clean, bold sans: Windows' own fonts there; on Linux the common
  // distribution fonts (SteamOS, Fedora, Debian and others), first found wins.
  std::vector<std::string> files;
#ifdef _WIN32
  const char* windir = std::getenv("WINDIR");
  const std::filesystem::path fonts =
      std::filesystem::path(windir && *windir ? windir : "C:\\Windows") / "Fonts";
  for (const char* name : {"bahnschrift.ttf", "segoeuib.ttf", "arialbd.ttf"}) {
    files.push_back((fonts / name).string());
  }
#else
  for (const char* path : {
           "/usr/share/fonts/noto/NotoSans-Bold.ttf",
           "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
           "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
           "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
           "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
           "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
           "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
           "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
       }) {
    files.push_back(path);
  }
#endif
  static const ImWchar kRanges[] = {0x0020, 0x00FF, 0};
  for (const std::string& file : files) {
    if (!std::filesystem::exists(file)) continue;
    for (size_t i = 0; i < sizeof(kFontSizes) / sizeof(kFontSizes[0]); ++i) {
      ImFontConfig config;
      config.OversampleH = 2;
      config.OversampleV = 1;
      config.PixelSnapH = false;
      g_fonts[i] = atlas->AddFontFromFileTTF(file.c_str(), kFontSizes[i], &config, kRanges);
    }
    return;
  }
}

GraphicsPanel::GraphicsPanel(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                             std::filesystem::path config_path, std::function<void()> on_closed)
    : ImGuiDialog(drawer),
      window_(window),
      config_path_(std::move(config_path)),
      on_closed_(std::move(on_closed)) {
  live::SetHideInterface(true);
}

GraphicsPanel::~GraphicsPanel() = default;

void GraphicsPanel::Change(const Row& r, int direction) {
  switch (r.kind) {
    case Kind::kSlider: {
      double v = GetDouble(r.cvar) + direction * r.step;
      v = std::clamp(std::round(v / r.step) * r.step, r.min, r.max);
      rex::cvar::SetFlagByName(r.cvar, r.integer ? fmt::format("{}", int(std::lround(v)))
                                                 : fmt::format("{:.2f}", v));
      break;
    }
    case Kind::kChoice: {
      const int n = int(r.choices.size());
      const int i = (ChoiceIndex(r) + direction + n) % n;
      rex::cvar::SetFlagByName(r.cvar, r.choices[i].value);
      if (std::string_view(r.cvar) == "resolution") {
        ApplyOutputResolution(window_, r.choices[i].value);
      }
      break;
    }
    case Kind::kStops: {
      const int n = int(r.choices.size());
      const int i = std::clamp(StopIndex(r) + direction, 0, n - 1);
      rex::cvar::SetFlagByName(r.cvar, r.choices[i].value);
      break;
    }
    case Kind::kFullscreen: {
      const bool v = !(window_ ? window_->IsFullscreen() : GetBool(r.cvar));
      rex::cvar::SetFlagByName(r.cvar, v ? "true" : "false");
      if (window_) window_->SetFullscreen(v);
      break;
    }
    case Kind::kToggle:
      rex::cvar::SetFlagByName(r.cvar, GetBool(r.cvar) ? "false" : "true");
      break;
  }
  dirty_ = true;
}

void GraphicsPanel::HandleInput(ImGuiIO& io) {
  const pad::Snapshot s = pad::Latest();
  constexpr int kDeadzone = 14000;
  const uint16_t b = s.buttons;
  const uint16_t pressed = first_frame_ ? 0 : static_cast<uint16_t>(b & ~prev_buttons_);
  // Don't take the A that opened the panel, or anything held then, as input.
  prev_buttons_ = first_frame_ ? 0xFFFF : b;
  if (first_frame_) return;

  // LB / RB (or Q / E, Tab) switch tabs.
  const int tab_count = int(Tabs().size());
  int tab_step = 0;
  if ((pressed & xi::X_INPUT_GAMEPAD_LEFT_SHOULDER) || ImGui::IsKeyPressed(ImGuiKey_Q, false)) tab_step = -1;
  if ((pressed & xi::X_INPUT_GAMEPAD_RIGHT_SHOULDER) || ImGui::IsKeyPressed(ImGuiKey_E, false) ||
      ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
    tab_step = 1;
  }
  if (tab_step) tab_ = (tab_ + tab_step + tab_count) % tab_count;

  const std::vector<const Row*> rows = VisibleRows(tab_);
  const int count = int(rows.size());
  int& cursor = cursor_[tab_];
  if (count == 0) cursor = 0;
  else cursor = std::clamp(cursor, 0, count - 1);

  int direction = 0;
  if ((b & xi::X_INPUT_GAMEPAD_DPAD_UP) || s.ly > kDeadzone) direction = -1;
  else if ((b & xi::X_INPUT_GAMEPAD_DPAD_DOWN) || s.ly < -kDeadzone) direction = 1;
  else if ((b & xi::X_INPUT_GAMEPAD_DPAD_LEFT) || s.lx < -kDeadzone) direction = -2;
  else if ((b & xi::X_INPUT_GAMEPAD_DPAD_RIGHT) || s.lx > kDeadzone) direction = 2;
  if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) direction = -1;
  else if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) direction = 1;
  else if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) direction = -2;
  else if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) direction = 2;

  // Held directions repeat: once at once, then after 0.35 s every 0.09 s.
  // Display Resolution, Fullscreen and Upscaler don't repeat (each change
  // rebuilds the picture).
  const double now = ImGui::GetTime();
  bool fire = false;
  if (direction != held_direction_) {
    held_direction_ = direction;
    fire = direction != 0;
    repeat_at_ = now + 0.35;
  } else if (direction != 0 && now >= repeat_at_) {
    const bool sideways = direction == -2 || direction == 2;
    const bool repeats = !sideways || (count && rows[cursor]->kind != Kind::kChoice &&
                                       rows[cursor]->kind != Kind::kFullscreen);
    fire = repeats;
    repeat_at_ = now + 0.09;
  }
  if (fire && count) {
    if (direction == -1) cursor = (cursor + count - 1) % count;
    if (direction == 1) cursor = (cursor + 1) % count;
    if (direction == -2 || direction == 2) Change(*rows[cursor], direction > 0 ? 1 : -1);
  }
  if (count && ((pressed & xi::X_INPUT_GAMEPAD_A) || ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
    if (rows[cursor]->kind != Kind::kSlider && rows[cursor]->kind != Kind::kStops) {
      Change(*rows[cursor], 1);
    }
  }
  if ((pressed & (xi::X_INPUT_GAMEPAD_B | xi::X_INPUT_GAMEPAD_START)) ||
      ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    Close();
  }
  (void)io;
}

// A line about the selected setting, under the rows.
std::string GraphicsPanel::Hint(const Row& r) const {
  const std::string_view name = r.cvar;
  const bool native = NativeRendererOn();
  if (name == "fullscreen") return "Borderless fullscreen on this monitor.";
  if (name == "resolution") {
    return native ? "The size the picture is made at. Native follows the window."
                  : "The size the picture is made at. Applies at the next launch.";
  }
  if (!native && (name == "native_render_scale" || name == "native_upscaler" ||
                  name == "native_upscaler_sharpness" || name == "native_upscaler_msaa")) {
    return "Native renderer only (Options F1 > Native renderer).";
  }
  if (name == "native_render_scale") {
    return GetString("native_upscaler") == "off"
               ? "The 3D scene's resolution; the HUD stays sharp. Lower is faster."
               : "The upscaler's input: 67% Quality, 59% Balanced, 50% Performance.";
  }
  if (name == "native_upscaler") {
    return "Rebuilds full resolution from the Render Resolution.";
  }
  if (name == "native_upscaler_sharpness") {
    return GetString("native_upscaler") == "fsr" ? "Sharpening applied by AMD FSR 3.1."
                                                 : "Used with AMD FSR 3.1 only.";
  }
  if (name == "native_upscaler_msaa") {
    return GetString("native_upscaler") != "off"
               ? "MSAA under FSR / XeSS. Off: the upscaler smooths edges (faster)."
               : "Used with FSR or XeSS only.";
  }
  if (name == "native_upscaled_textures") {
    return native ? "Textures upscaled on this PC at install (textures\\upscaled)."
                  : "Native renderer only (Options F1 > Native renderer).";
  }
  if (name == "native_sharp_fonts") {
    return native ? "The game's text and button icons rebuilt at 4x, sharp at any resolution."
                  : "Native renderer only (Options F1 > Native renderer).";
  }
  if (name == "revamped_graphics") return "Every enhancement at once.";
  if (name == "frame_rate_limit") {
    return "30 is the console's own pacing; Revamped Graphics starts at 60. The game plays "
           "at normal speed at any rate.";
  }
  return {};
}

void GraphicsPanel::OnDraw(ImGuiIO& io) {
  HandleInput(io);
  const bool first = first_frame_;
  first_frame_ = false;

  const ImVec2 display = io.DisplaySize;
  // At the left of the screen so the scene stays in view; sized from the
  // display's height (720p units).
  const float unit = std::max(display.y / 720.0f, 0.5f);
  const float text_px = 17.0f * unit;
  const float title_px = 27.0f * unit;
  const float tab_px = 15.0f * unit;
  const float hint_px = text_px * 0.78f;
  ImFont* f = PanelFont(text_px);
  ImFont* title_font = PanelFont(title_px);
  ImFont* tab_font = PanelFont(tab_px);
  ImFont* hint_font = PanelFont(hint_px);
  const float row_h = text_px * 1.6f;
  const float pad = 16.0f * unit;
  // As tall as the longest tab, so switching doesn't move the panel.
  size_t max_rows = 0;
  for (const Tab& t : Tabs()) max_rows = std::max(max_rows, t.rows.size());
  const ImVec2 size(std::min(display.x * 0.45f, 380.0f * unit),
                    pad * 2 + title_px * 1.25f + tab_px * 2.4f + row_h * float(max_rows) +
                        hint_px * 3.0f + text_px * 1.6f);
  const ImVec2 at(std::round(display.x * 0.03f), std::round(display.y * 0.5f - size.y * 0.5f));

  ImGui::SetNextWindowPos(at, ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.0f);
  if (first) ImGui::SetNextWindowFocus();
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoBackground;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  if (!ImGui::Begin("###amped3_graphics_panel", nullptr, flags)) {
    ImGui::End();
    ImGui::PopStyleVar();
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 end(at.x + size.x, at.y + size.y);
  dl->AddRectFilled(at, end, kPanel, 10.0f * unit);
  dl->AddRect(at, end, kEdge, 10.0f * unit, 0, 1.5f * unit);

  // Title.
  float y = at.y + pad;
  dl->AddText(title_font, title_px, ImVec2(at.x + pad, y), kGold, "GRAPHICS");
  y += title_px * 1.25f;

  // Tabs: LB  Effects  Display  RB, the current one gold and underlined.
  {
    const float ty = y + tab_px * 0.35f;
    auto text_w = [&](const char* t) { return tab_font->CalcTextSizeA(tab_px, FLT_MAX, 0.0f, t).x; };
    float x = at.x + pad;
    dl->AddText(tab_font, tab_px, ImVec2(x, ty), kDim, "LB");
    x += text_w("LB") + 12.0f * unit;
    const std::vector<Tab>& tabs = Tabs();
    for (int i = 0; i < int(tabs.size()); ++i) {
      std::string upper = tabs[i].name;
      for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
      const float w = text_w(upper.c_str());
      const bool current = i == tab_;
      dl->AddText(tab_font, tab_px, ImVec2(x, ty), current ? kGold : kDim, upper.c_str());
      if (current) {
        dl->AddRectFilled(ImVec2(x, ty + tab_px * 1.15f), ImVec2(x + w, ty + tab_px * 1.15f + 2.0f * unit),
                          kGold);
      }
      x += w + 16.0f * unit;
    }
    dl->AddText(tab_font, tab_px, ImVec2(x - 4.0f * unit, ty), kDim, "RB");
    y += tab_px * 2.0f;
    dl->AddLine(ImVec2(at.x + pad, y), ImVec2(end.x - pad, y), kEdge, 1.5f * unit);
    y += tab_px * 0.4f;
  }

  const std::vector<const Row*> rows = VisibleRows(tab_);
  const int cursor = rows.empty() ? -1 : std::clamp(cursor_[tab_], 0, int(rows.size()) - 1);
  const bool revamped = GetBool("revamped_graphics");
  const bool native = NativeRendererOn();
  const std::string upscaler = GetString("native_upscaler");
  const float value_right = end.x - pad;
  const float value_w = 70.0f * unit;
  const float arrow_s = 4.5f * unit;
  for (int i = 0; i < int(rows.size()); ++i) {
    const Row& r = *rows[i];
    const std::string_view name = r.cvar;
    const bool selected = i == cursor;
    const ImVec2 row0(at.x + pad * 0.5f, y);
    const ImVec2 row1(end.x - pad * 0.5f, y + row_h);
    if (selected) dl->AddRectFilled(row0, row1, kSelect, 4.0f * unit);
    // Dimmed while they do nothing: the enhancements while Revamped Graphics
    // is off (they keep their settings for when it's back on), the native
    // renderer's settings while it's off, FSR sharpness without FSR.
    bool dim = false;
    if (tab_ == 0) dim = name != "revamped_graphics" && !revamped;
    if (name == "native_render_scale" || name == "native_upscaler") dim = !native;
    if (name == "native_upscaler_sharpness") dim = !native || upscaler != "fsr";
    if (name == "native_upscaler_msaa") dim = !native || upscaler == "off";
    if (name == "native_sharp_fonts" || name == "native_upscaled_textures") dim = !native;
    const ImU32 color = selected ? kGold : (dim ? kDim : kWhite);
    const float ty = y + (row_h - text_px) * 0.5f;
    const float cy = y + row_h * 0.5f;
    const float indent = (tab_ == 0 && i > 0) || name == "native_upscaler_sharpness" ||
                                 name == "native_upscaler_msaa"
                             ? pad * 0.8f
                             : 0.0f;
    dl->AddText(f, text_px, ImVec2(at.x + pad + indent, ty), color, r.label);
    const float label_end =
        at.x + pad + indent + f->CalcTextSizeA(text_px, FLT_MAX, 0.0f, r.label).x;

    std::string value;
    bool arrows = false;
    // A slider's bar: its room is sized for the widest value text, so the bar
    // doesn't move as the value changes; the bar fills what's left between the
    // label and the value (up to its usual length), and is left out if there's
    // no room for it.
    auto draw_bar = [&](float t, const char* widest, const std::string& text) {
      const float room = std::max(f->CalcTextSizeA(text_px, FLT_MAX, 0.0f, widest).x,
                                  f->CalcTextSizeA(text_px, FLT_MAX, 0.0f, text.c_str()).x);
      const float bar1 = value_right - room - 12.0f * unit;
      const float bar0 = std::max(bar1 - value_w, label_end + 14.0f * unit);
      if (bar1 - bar0 >= 24.0f * unit) {
        dl->AddRectFilled(ImVec2(bar0, cy - 2 * unit), ImVec2(bar1, cy + 2 * unit), kTrack, 2 * unit);
        dl->AddRectFilled(ImVec2(bar0, cy - 2 * unit), ImVec2(bar0 + (bar1 - bar0) * t, cy + 2 * unit),
                          color, 2 * unit);
        dl->AddCircleFilled(ImVec2(bar0 + (bar1 - bar0) * t, cy), 5.0f * unit, color);
      }
    };
    switch (r.kind) {
      case Kind::kSlider: {
        const double v = GetDouble(r.cvar);
        const float t = float(std::clamp((v - r.min) / (r.max - r.min), 0.0, 1.0));
        // The value's room is sized for its widest text, so the bar doesn't
        // move as it changes; the bar fills what's left between the label
        // and the value (up to its usual length), and is left out if there's
        // no room for it.
        const char* widest = "0.0";
        if (name == "native_render_scale") {
          // "75%  1440 x 810": the percentage and the pixels it gives.
          const int percent = int(std::lround(v));
          uint32_t w = 0, h = 0;
          live::RenderScaleFor(percent, &w, &h);
          value = w && h ? fmt::format("{}%  {} x {}", percent, w, h) : fmt::format("{}%", percent);
          widest = "100%  0000 x 0000";
        } else if (r.step < 0.1) {
          value = fmt::format("{:.2f}", v);
          widest = "0.00";
        } else {
          value = fmt::format("{:.1f}", v);
        }
        draw_bar(t, widest, value);
        break;
      }
      case Kind::kStops: {
        const int index = StopIndex(r);
        const int n = int(r.choices.size());
        value = r.choices[index].label;
        const char* widest = r.choices[0].label;
        for (const RowChoice& c : r.choices) {
          if (std::strlen(c.label) > std::strlen(widest)) widest = c.label;
        }
        draw_bar(n > 1 ? float(index) / float(n - 1) : 0.0f, widest, value);
        break;
      }
      case Kind::kChoice:
        value = r.choices[ChoiceIndex(r)].label;
        arrows = true;
        break;
      case Kind::kFullscreen:
        value = (window_ ? window_->IsFullscreen() : GetBool(r.cvar)) ? "On" : "Off";
        arrows = true;
        break;
      case Kind::kToggle:
        value = GetBool(r.cvar) ? "On" : "Off";
        arrows = true;
        break;
    }
    // The value, made smaller if it would reach the label (narrow windows).
    const float arrow_room = arrows ? arrow_s * 2.2f + 8.0f * unit + arrow_s * 1.6f : 0.0f;
    const float space = value_right - label_end - 12.0f * unit - arrow_room;
    float vpx = text_px;
    ImVec2 vs = f->CalcTextSizeA(vpx, FLT_MAX, 0.0f, value.c_str());
    if (vs.x > space && vs.x > 0.0f) {
      vpx = text_px * std::max(space / vs.x, 0.65f);
      vs = f->CalcTextSizeA(vpx, FLT_MAX, 0.0f, value.c_str());
    }
    // Choices and switches: the game's arrows either side of the value.
    const float vx = value_right - vs.x - (arrows ? arrow_s * 2.2f : 0.0f);
    dl->AddText(f, vpx, ImVec2(vx, y + (row_h - vs.y) * 0.5f), color, value.c_str());
    if (selected && arrows) {
      const float lx = vx - 8.0f * unit;
      dl->AddTriangleFilled(ImVec2(lx - arrow_s, cy), ImVec2(lx + arrow_s * 0.6f, cy - arrow_s),
                            ImVec2(lx + arrow_s * 0.6f, cy + arrow_s), kGold);
      const float rx = value_right - arrow_s * 0.6f;
      dl->AddTriangleFilled(ImVec2(rx + arrow_s * 0.6f, cy), ImVec2(rx - arrow_s, cy - arrow_s),
                            ImVec2(rx - arrow_s, cy + arrow_s), kGold);
    }
    y += row_h;
  }

  // What the selected setting does, above the legend.
  if (cursor >= 0) {
    const std::string hint = Hint(*rows[cursor]);
    if (!hint.empty()) {
      const float hy = end.y - pad - text_px * 1.6f - hint_px * 2.6f;
      dl->AddText(hint_font, hint_px, ImVec2(at.x + pad, hy), kDim, hint.c_str(), nullptr,
                  size.x - pad * 2);
    }
  }

  // The button legend, as at the foot of the game's menus.
  const char* legend = "(LB/RB) Tab     (A) Change     (B) Back";
  const float lpx = text_px * 0.8f;
  ImFont* lf = PanelFont(lpx);
  const ImVec2 ls = lf->CalcTextSizeA(lpx, FLT_MAX, 0.0f, legend);
  dl->AddText(lf, lpx, ImVec2(at.x + (size.x - ls.x) * 0.5f, end.y - pad - ls.y), kDim, legend);

  ImGui::End();
  ImGui::PopStyleVar();
}

void GraphicsPanel::OnClose() {
  live::SetHideInterface(false);
  if (dirty_) rex::cvar::SaveConfig(config_path_);
  if (on_closed_) on_closed_();
}

}  // namespace amped3
