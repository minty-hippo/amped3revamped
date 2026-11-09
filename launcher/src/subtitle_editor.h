// amped3-launcher - the subtitle editor: corrects what the subtitle maker
// wrote (subtitles/amped3.subtitles in the install folder): the words, the
// times, and who says each line (the speaker colours). A clip that's been
// corrected is marked so, and making the subtitles again keeps it as it is.
// The game reads the file again within a couple of seconds of a save.
//
// Play: the sound itself (the subtitle maker writes it out as a WAV from the
// game's files, kept in subtitles/work/play), from the start or from a line.

#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../src/subtitle_file.h"

struct ImFont;
struct SDL_AudioStream;

namespace launcher {

namespace fs = std::filesystem;

class SubtitleEditor {
 public:
  ~SubtitleEditor();
  // Reads the file; false (with *error) if there's none to edit.
  bool Open(const fs::path& install_dir, const fs::path& source_dir, std::string* error);
  // Inside the launcher's ImGui frame. Returns true when it wants to close.
  bool Draw(ImFont* font, float unit);
  // Back (Esc / B): closes, unless there are unsaved changes (then a second
  // Back within a few seconds closes without saving).
  bool Back();
  bool Playing() const;

 private:
  void Save();
  void Play(const std::string& source, uint32_t from_ms);
  void StopAudio();
  fs::path ToolPath() const;

  fs::path install_dir_, source_dir_, file_;
  std::vector<amped3::subtitle_file::Clip> clips_;
  std::string header_;  // the file's comment lines, kept
  int selected_ = -1;
  int filter_ = 0;  // 0 all, 1 story, 2 movies, 3 speech, 4 corrected
  char search_[128] = {};
  bool dirty_ = false;
  double back_until_ = 0;
  std::string status_;

  // Playing: the WAV made by the tool (on a thread), then SDL audio.
  struct Player;
  std::shared_ptr<Player> player_;
  SDL_AudioStream* stream_ = nullptr;
};

}  // namespace launcher
