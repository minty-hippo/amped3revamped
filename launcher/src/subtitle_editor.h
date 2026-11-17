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

#include "../../src/subtitle_draw.h"
#include "../../src/subtitle_file.h"
#include "game_settings.h"

struct ImFont;
struct SDL_AudioStream;
struct SDL_Renderer;
struct SDL_Texture;

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
  // Before SDL quits: the sound output closed (and the movie's picture let go).
  void Shutdown();
  // Where the movie preview's pictures are drawn (null: none).
  void SetRenderer(SDL_Renderer* renderer);
  // The text's size, times the display's scale (A- / A+, Ctrl+= / Ctrl+-).
  float Zoom() const { return zoom_; }
  // The window recreated or resized: the preview's picture uploaded again.
  void Refresh() { video_tex_frame_ = -1; }

  // A movie's pictures for the preview (the subtitle maker writes them out:
  // JPEGs, kPreviewFps a second at most).
  struct Video {
    fs::path dir;
    double fps = 15;
    int frames = 0, width = 0, height = 0;
  };

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
  bool LoopSpan(size_t* f0, size_t* f1) const;
  // The picked line (or the one under the playhead) cut in two there; the
  // picked line and the next made one. False (and why, in the status) if
  // there's nothing to do.
  bool SplitAtPlayhead();
  bool JoinNext();
  fs::path ToolPath() const;
  // Undo: the selected clip as it was before an edit.
  void Remember();
  void Undo(bool redo);

  // The timeline and the preview under it (the selected clip); true when a
  // line was changed.
  bool DrawTimeline(ImFont* font, float px, float unit);
  // The selected clip's speakers' names (typed in; offered from the other
  // clips'); true when one changed.
  bool DrawNames(float px, float unit);
  int Speakers(const amped3::subtitle_file::Clip& c) const;
  // A movie's pictures: fetched (made on a thread when they aren't yet).
  void FetchVideo(const std::string& source);
  void UpdateVideo();
  // The movie's picture at the playhead, uploaded; false if there's none.
  bool VideoFrame(int* width, int* height);
  // A line's text with its speaker's name first (when named).
  std::string Named(const amped3::subtitle_file::Clip& c, const amped3::subtitle_file::Line& l) const;

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

  // The movie preview: its pictures (made by the tool on a thread), the one
  // shown uploaded into a texture.
  struct VideoLoader;
  std::shared_ptr<VideoLoader> video_loader_;
  std::shared_ptr<Video> video_;
  std::string video_source_;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* video_tex_ = nullptr;
  int video_tex_frame_ = -1;
  std::vector<std::string> known_names_;  // every name typed in, for picking
  int extra_speakers_ = 0, extra_speakers_clip_ = -1;  // speakers added with + (that clip)
  // The layout: the text's size, the list's width and the movie preview's
  // height (shares of the window), kept between runs.
  float zoom_ = 1.0f, list_frac_ = 0.30f, preview_frac_ = 0.28f;
  bool layout_dirty_ = false;
  void LoadLayout();
  // The game's settings: how the subtitles look there (the preview's too).
  GameConfig game_;
  amped3::subtitle_draw::Style GameStyle() const;
  bool GameColors() const;
  bool GameNames() const;
  void DrawLook(float unit);
  void SaveLayout();
  void SetZoom(float z);
  double names_scan_at_ = -1;

  // Output: one stream, fed a little ahead.
  SDL_AudioStream* stream_ = nullptr;
  int stream_channels_ = 0, stream_freq_ = 0;
  bool playing_ = false;
  size_t feed_frame_ = 0;   // the next frame to queue
  double playhead_ms_ = 0;  // where the playhead is (follows the sound while playing)

  // The timeline's view, and what's being dragged on it.
  double view_ms_ = 0, view_len_ms_ = 0;  // 0: the whole clip
  int drag_ = 0;        // 0 none, 1 scrubbing, 2 a line's start, 3 its end, 4 the whole line,
                        // 5 where two lines meet (drag_line_'s end and the next's start)
  int drag_joint_side_ = 2;  // dragging at a joint: -1 its left, 0 both, 1 its right (2: not)
  int drag_line_ = -1;
  double drag_grab_ms_ = 0;
  bool was_playing_ = false;
  double last_scrub_ms_ = -1;
  double snap_playhead_ms_ = 0;  // where the playhead was when a drag began
  float speed_ = 1.0f;           // playing speed
  bool loop_ = false;            // the picked line over and over
  std::vector<int> visible_;     // the clips listed, in order (PageUp / PageDown)

  // The list's groups (the player's own: labelled, in their order, their
  // clips in order), kept in subtitles/groups.txt by source. Clips in no
  // group are listed after them, as the file has them.
  struct Group {
    std::string label;
    std::vector<std::string> sources;
    bool open = true;
  };
  std::vector<Group> groups_;
  bool ungrouped_open_ = true;
  int renaming_group_ = -1;  // its label being typed
  bool rename_focus_ = false;
  char rename_buf_[128] = {};
  std::vector<bool> unsaved_;  // per clip: changed since the last save
  void LoadGroups();
  void SaveGroups();
  int GroupOf(const std::string& source, int* pos = nullptr) const;
  void MoveClip(const std::string& source, int group, int pos);  // group -1: out of them all
  void NewGroup(const std::string& first_source);
  void DrawClipList(float px, float unit);
  bool scroll_to_selected_ = false;

  struct Step {
    int clip;
    amped3::subtitle_file::Clip before;
  };
  std::vector<Step> undo_, redo_;
};

}  // namespace launcher
