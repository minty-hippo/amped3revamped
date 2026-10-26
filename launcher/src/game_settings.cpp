// amped3-launcher - the game's graphics settings (game_settings.h).

#include "game_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace launcher {

namespace {
using Kind = GameSetting::Kind;

std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r");
  size_t b = s.find_last_not_of(" \t\r");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// "name = value" (a comment after the value is dropped); false for others.
bool ParseLine(const std::string& line, std::string* key, std::string* value) {
  const std::string t = Trim(line);
  if (t.empty() || t[0] == '#' || t[0] == '[') return false;
  const size_t eq = t.find('=');
  if (eq == std::string::npos) return false;
  *key = Trim(t.substr(0, eq));
  std::string v = Trim(t.substr(eq + 1));
  if (!v.empty() && v[0] == '"') {
    const size_t end = v.find('"', 1);
    v = end == std::string::npos ? v.substr(1) : v.substr(1, end - 1);
  } else {
    const size_t hash = v.find('#');
    if (hash != std::string::npos) v = Trim(v.substr(0, hash));
  }
  *value = v;
  return !key->empty();
}

bool IsString(const GameSetting& s) {
  // Choices of words are strings in the file; numbers and true/false aren't.
  if (s.kind != Kind::kChoice) return false;
  for (const auto& c : s.choices) {
    const std::string v = c.first;
    if (v.empty() || v.find_first_not_of("0123456789.-") != std::string::npos) return true;
  }
  return false;
}
}  // namespace

const std::vector<GameSetting>& DisplaySettings() {
  static const std::vector<GameSetting> settings = {
      {"fullscreen", "Fullscreen", Kind::kToggle, "true"},
      {"frame_rate_limit", "Frame rate", Kind::kChoice, "auto",
       {{"auto", "Auto"}, {"30", "30 fps"}, {"40", "40 fps"}, {"50", "50 fps"}, {"60", "60 fps"},
        {"72", "72 fps"}, {"90", "90 fps"}, {"120", "120 fps"}, {"144", "144 fps"},
        {"0", "Unlocked"}}},
      {"resolution", "Display resolution", Kind::kChoice, "",
       {{"", "Native"}, {"1080p", "1920 x 1080"}, {"1440p", "2560 x 1440"}, {"4k", "3840 x 2160"}}},
      {"native_render_scale", "Render resolution", Kind::kSlider, "100", {}, 25, 100, 5, true},
      {"native_upscaler", "Upscaler", Kind::kChoice, "off",
#if defined(_WIN32)
       {{"off", "Off"}, {"xess", "Intel XeSS"}, {"fsr", "AMD FSR 3.1"}}},
#else
       // Intel ships XeSS for Windows only.
       {{"off", "Off"}, {"fsr", "AMD FSR 3.1"}}},
#endif
      {"native_upscaler_sharpness", "FSR sharpness", Kind::kSlider, "0.3", {}, 0.0, 1.0, 0.05},
      {"native_upscaler_msaa", "MSAA with the upscaler", Kind::kToggle, "false"},
      {"native_sharp_fonts", "Sharp fonts", Kind::kToggle, "false"},
      {"native_upscaled_textures", "Upscaled textures", Kind::kToggle, "false"},
  };
  return settings;
}

const std::vector<GameSetting>& EffectSettings() {
  static const std::vector<GameSetting> settings = {
      {"revamped_graphics", "Revamped Graphics", Kind::kToggle, "true"},
      {"native_camera_motion_blur", "Camera motion blur", Kind::kToggle, "true", {}, 0, 0, 0, false, true},
      {"native_object_motion_blur", "Object motion blur", Kind::kToggle, "true", {}, 0, 0, 0, false, true},
      {"native_camera_motion_blur_strength", "Motion blur strength", Kind::kSlider, "1.0", {}, 0.0,
       3.0, 0.1, false, true},
      {"native_speed_blur", "Speed blur", Kind::kToggle, "true", {}, 0, 0, 0, false, true},
      {"native_fog", "Distance fog", Kind::kToggle, "true", {}, 0, 0, 0, false, true},
      {"native_ambient_occlusion", "Ambient occlusion", Kind::kToggle, "true", {}, 0, 0, 0, false, true},
      {"native_god_rays", "God rays", Kind::kToggle, "true", {}, 0, 0, 0, false, true},
      {"native_shadow_resolution", "Shadow resolution", Kind::kChoice, "200",
       {{"100", "1x (the game's)"}, {"150", "1.5x"}, {"200", "2x"}, {"300", "3x"}, {"400", "4x"}},
       0, 0, 0, false, true},
      {"native_shadow_distance", "Shadow distance", Kind::kChoice, "0",
       {{"0", "The game's (40 m)"}, {"75", "75 m"}, {"100", "100 m"}, {"150", "150 m"},
        {"200", "200 m"}, {"300", "300 m"}},
       0, 0, 0, false, true},
      {"native_foliage_fade", "Branch fade", Kind::kToggle, "true", {}, 0, 0, 0, false, true},
      {"native_map_clouds", "Trail map clouds", Kind::kToggle, "true", {}, 0, 0, 0, false, true},
  };
  return settings;
}

void GameConfig::Load(const std::filesystem::path& file) {
  file_ = file;
  lines_.clear();
  values_.clear();
  std::ifstream f(file);
  std::string line, key, value;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines_.push_back(line);
    if (ParseLine(line, &key, &value)) values_[key] = value;
  }
}

std::string GameConfig::Get(const GameSetting& s) const {
  auto it = values_.find(s.key);
  return it == values_.end() ? std::string(s.fallback) : it->second;
}

bool GameConfig::Set(const GameSetting& s, const std::string& value) {
  values_[s.key] = value;
  const std::string written =
      std::string(s.key) + " = " + (IsString(s) ? "\"" + value + "\"" : value);
  bool found = false;
  std::string key, v;
  for (auto& line : lines_) {
    if (ParseLine(line, &key, &v) && key == s.key) {
      line = written;
      found = true;
    }
  }
  if (!found) lines_.push_back(written);
  return Save();
}

bool GameConfig::Save() {
  std::ofstream f(file_, std::ios::trunc);
  if (!f) return false;
  for (const auto& line : lines_) f << line << "\n";
  return bool(f);
}

std::string GameConfig::Text(const GameSetting& s) const {
  const std::string v = Get(s);
  switch (s.kind) {
    case Kind::kToggle:
      return v == "true" ? "On" : "Off";
    case Kind::kChoice:
      for (const auto& c : s.choices) {
        if (v == c.first) return c.second;
      }
      return v;
    case Kind::kSlider: {
      char buf[32];
      const double d = std::atof(v.c_str());
      if (s.integer) std::snprintf(buf, sizeof(buf), "%d%%", int(std::lround(d)));
      else std::snprintf(buf, sizeof(buf), s.step < 0.1 ? "%.2f" : "%.1f", d);
      return buf;
    }
  }
  return v;
}

bool GameConfig::Step(const GameSetting& s, int step) {
  const std::string v = Get(s);
  switch (s.kind) {
    case Kind::kToggle:
      return Set(s, v == "true" ? "false" : "true");
    case Kind::kChoice: {
      const int n = int(s.choices.size());
      int i = 0;
      for (int k = 0; k < n; ++k) {
        if (v == s.choices[size_t(k)].first) i = k;
      }
      i = (i + step % n + n) % n;
      return Set(s, s.choices[size_t(i)].first);
    }
    case Kind::kSlider: {
      double d = std::atof(v.c_str()) + step * s.step;
      d = std::clamp(d, s.min, s.max);
      d = std::round(d / s.step) * s.step;
      char buf[32];
      if (s.integer) std::snprintf(buf, sizeof(buf), "%d", int(std::lround(d)));
      else std::snprintf(buf, sizeof(buf), "%.2f", d);
      return Set(s, buf);
    }
  }
  return false;
}

}  // namespace launcher
