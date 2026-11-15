// amped3-launcher - the subtitle editor: corrects what the subtitle maker
// wrote (subtitles/amped3.subtitles in the install folder): the words, the
// times, and who says each line (the speaker colours). A clip that's been
// corrected is marked so, and making the subtitles again keeps it as it is.
// The game reads the file again within a couple of seconds of a save.
//
// The sound: the subtitle maker writes it out as a WAV from the game's files
// (kept in subtitles/work/play), fetched as soon as a clip is chosen. A
// timeline shows its waveform with each line on it (in the speaker's colour):
// click or drag anywhere on it to scrub (the sound heard as you go), drag a
// line to move it or its ends to retime it, wheel to zoom, Shift+wheel to
// pan. Space plays and pauses from the playhead; the line being heard shows
// below as the game shows it. Edits can be undone (Ctrl+Z / Ctrl+Y).

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
  // Before SDL quits: the sound output closed.
  void Shutdown() { StopAudio(); }

  // The sound of a clip, decoded (float samples, interleaved).
  struct Audio {
    std::vector<float> pcm;
    int channels = 1;
    int freq = 16000;
    size_t frames = 0;
    std::vector<float> peaks;  // per bucket: min, max (kPeakBuckets of them)
    double Ms() const { return frames * 1000.0 / freq; }
  };

 private:
  void Save();
  void Select(int clip);
  // The selected clip's sound: fetched (made on a thread when it isn't yet).
  void Fetch(const std::string& source);
  void UpdateAudio();
  // Playing from the playhead; scrubbing (a short bit at a time).
  void StartPlaying();
  void StopPlaying();
  void ScrubTo(double ms);
  void StopAudio();
  void Feed();
  double PlayPositionMs() const;
  bool Ready() const;  // the selected clip's sound is here
  fs::path ToolPath() const;
  // Undo: the selected clip as it was before an edit.
  void Remember();
  void Undo(bool redo);

  // The timeline and the preview under it (the selected clip); true when a
  // line was changed.
  bool DrawTimeline(ImFont* font, float px, float unit);

  fs::path install_dir_, source_dir_, file_;
  std::vector<amped3::subtitle_file::Clip> clips_;
  std::string header_;  // the file's comment lines, kept
  int selected_ = -1;
  int line_ = -1;  // the selected line of the selected clip
  int filter_ = 0;  // 0 all, 1 story, 2 movies, 3 speech, 4 corrected
  char search_[128] = {};
  bool dirty_ = false;
  double back_until_ = 0;
  std::string status_;

  // The sound: the WAV made by the tool (on a thread), decoded.
  struct Loader;
  std::shared_ptr<Loader> loader_;
  std::shared_ptr<Audio> audio_;
  std::string audio_source_;  // whose sound audio_ is
  double fetch_at_ = 0;       // when to fetch (a moment after a clip is chosen)

  // Output: one stream, fed a little ahead.
  SDL_AudioStream* stream_ = nullptr;
  int stream_channels_ = 0, stream_freq_ = 0;
  bool playing_ = false;
  size_t feed_frame_ = 0;   // the next frame to queue
  double playhead_ms_ = 0;  // where the playhead is (follows the sound while playing)

  // The timeline's view, and what's being dragged on it.
  double view_ms_ = 0, view_len_ms_ = 0;  // 0: the whole clip
  int drag_ = 0;        // 0 none, 1 scrubbing, 2 a line's start, 3 its end, 4 the whole line
  int drag_line_ = -1;
  double drag_grab_ms_ = 0;
  bool was_playing_ = false;
  double last_scrub_ms_ = -1;

  struct Step {
    int clip;
    amped3::subtitle_file::Clip before;
  };
  std::vector<Step> undo_, redo_;
};

}  // namespace launcher
