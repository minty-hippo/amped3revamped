// amped3-launcher - the game's graphics settings, read from and written to
// amped3.toml in the install folder (the same file the in-game Graphics panel
// and options menu use: one "name = value" line per setting).

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace launcher {

struct GameSetting {
  enum class Kind { kToggle, kChoice, kSlider };
  const char* key;    // the game's setting (cvar) name
  const char* label;
  Kind kind = Kind::kToggle;
  const char* fallback = "false";  // the game's default
  std::vector<std::pair<const char*, const char*>> choices = {};  // value, label
  double min = 0, max = 0, step = 0;
  bool integer = false;
  bool effect = false;  // does nothing while Revamped Graphics is off
};

// As in the in-game Graphics panel: its Display tab, and its Effects tab.
const std::vector<GameSetting>& DisplaySettings();
const std::vector<GameSetting>& EffectSettings();

class GameConfig {
 public:
  void Load(const std::filesystem::path& file);
  // The value (unquoted), or the setting's default when the file has none.
  std::string Get(const GameSetting& s) const;
  // Sets and saves at once (the file keeps its other lines and comments).
  bool Set(const GameSetting& s, const std::string& value);
  // The value as shown ("On", "60 fps", "75%"...).
  std::string Text(const GameSetting& s) const;
  // Steps a choice or slider (toggles flip).
  bool Step(const GameSetting& s, int step);
  bool Loaded() const { return !file_.empty(); }

 private:
  std::filesystem::path file_;
  std::vector<std::string> lines_;
  std::map<std::string, std::string> values_;
  bool Save();
};

}  // namespace launcher
