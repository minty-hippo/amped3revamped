// amped3-launcher - the subtitle editor (see subtitle_editor.h).

#include "subtitle_editor.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "platform.h"
#include "process.h"

namespace launcher {

namespace sf = amped3::subtitle_file;

namespace {

// As the game shows them: white, then the yellow, pink and green of its menus.
const ImU32 kSpeaker[4] = {IM_COL32(255, 255, 255, 255), IM_COL32(250, 232, 74, 255),
                           IM_COL32(246, 96, 240, 255), IM_COL32(128, 224, 40, 255)};
const ImU32 kGold = IM_COL32(255, 204, 0, 255);
const ImU32 kDim = IM_COL32(255, 255, 255, 130);
const ImU32 kRed = IM_COL32(255, 80, 70, 255);

constexpr size_t kPeakBuckets = 4096;
constexpr double kQueueAhead = 0.12;  // seconds of sound queued ahead while playing
constexpr double kGrainMs = 85.0;     // a scrub's bit of sound
constexpr size_t kUndoDepth = 200;

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const char* KindLabel(sf::Kind k) {
  return k == sf::Kind::kStory ? "story" : k == sf::Kind::kVideo ? "movie" : "speech";
}

ImU32 WithAlpha(ImU32 c, int a) { return (c & ~IM_COL32_A_MASK) | (ImU32(a) << IM_COL32_A_SHIFT); }

// 83.25 s -> "1:23.25"; under a minute "23.25".
std::string Clock(double ms, bool fine = true) {
  if (ms < 0) ms = 0;
  const int total_cs = int(ms / 10.0 + 0.5);
  const int m = total_cs / 6000, s = (total_cs / 100) % 60, cs = total_cs % 100;
  char b[32];
  if (fine) {
    if (m) std::snprintf(b, sizeof(b), "%d:%02d.%02d", m, s, cs);
    else std::snprintf(b, sizeof(b), "%d.%02d", s, cs);
  } else {
    if (m) std::snprintf(b, sizeof(b), "%d:%02d", m, s);
    else std::snprintf(b, sizeof(b), "%ds", s);
  }
  return b;
}

// The WAV, decoded to float samples (its own rate and channels), with peaks
// for the waveform. False with *error.
bool Decode(const fs::path& wav, SubtitleEditor::Audio* a, std::string* error) {
  SDL_AudioSpec spec;
  Uint8* buf = nullptr;
  Uint32 len = 0;
  if (!SDL_LoadWAV(PathText(wav).c_str(), &spec, &buf, &len)) {
    *error = std::string("Couldn't read the sound: ") + SDL_GetError();
    return false;
  }
  SDL_AudioSpec dst = spec;
  dst.format = SDL_AUDIO_F32;
  Uint8* out = nullptr;
  int out_len = 0;
  const bool ok = SDL_ConvertAudioSamples(&spec, buf, int(len), &dst, &out, &out_len);
  SDL_free(buf);
  if (!ok) {
    *error = std::string("Couldn't convert the sound: ") + SDL_GetError();
    return false;
  }
  a->channels = std::max(1, dst.channels);
  a->freq = std::max(1, dst.freq);
  a->pcm.assign(reinterpret_cast<float*>(out), reinterpret_cast<float*>(out) + out_len / 4);
  SDL_free(out);
  a->frames = a->pcm.size() / size_t(a->channels);
  a->peaks.assign(kPeakBuckets * 2, 0.0f);
  if (a->frames) {
    for (size_t b = 0; b < kPeakBuckets; ++b) {
      const size_t f0 = b * a->frames / kPeakBuckets, f1 = std::max(f0 + 1, (b + 1) * a->frames / kPeakBuckets);
      float lo = 0, hi = 0;
      for (size_t f = f0; f < f1 && f < a->frames; ++f) {
        float v = 0;
        for (int c = 0; c < a->channels; ++c) v += a->pcm[f * a->channels + c];
        v /= float(a->channels);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
      a->peaks[b * 2] = lo;
      a->peaks[b * 2 + 1] = hi;
    }
  }
  return true;
}

}  // namespace

struct SubtitleEditor::Loader {
  std::mutex m;
  int state = 1;  // 1 working, 2 ready, 3 failed
  std::string source;
  std::shared_ptr<Audio> audio;
  std::string error;
};

SubtitleEditor::~SubtitleEditor() { StopAudio(); }

bool SubtitleEditor::Open(const fs::path& install_dir, const fs::path& source_dir, std::string* error) {
  install_dir_ = install_dir;
  source_dir_ = source_dir;
  file_ = install_dir / "subtitles" / sf::kFileName;
  std::ifstream f(file_, std::ios::binary);
  if (!f) {
    *error = "No subtitles made yet: Make subtitles first.";
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  if (!sf::Parse(text, &clips_, error)) return false;
  // The file's own comments (who made it, with what), kept on save.
  header_.clear();
  std::istringstream lines(text);
  std::string line;
  std::getline(lines, line);
  while (std::getline(lines, line) && !line.empty() && line[0] == '#') {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("# Lines:", 0) != 0) header_ += line + "\n";
  }
  selected_ = -1;
  undo_.clear();
  redo_.clear();
  Select(clips_.empty() ? -1 : 0);
  dirty_ = false;
  status_.clear();
  return true;
}

fs::path SubtitleEditor::ToolPath() const {
#if defined(_WIN32)
  const char* name = "amped3-subtitles.exe";
#else
  const char* name = "amped3-subtitles";
#endif
  std::error_code ec;
  if (fs::exists(install_dir_ / name, ec)) return install_dir_ / name;
  for (const auto& entry : fs::directory_iterator(source_dir_ / "out" / "build", ec)) {
    if (fs::exists(entry.path() / name, ec)) return entry.path() / name;
  }
  return {};
}

void SubtitleEditor::Save() {
  const fs::path tmp = file_.string() + ".new";
  {
    std::ofstream f(tmp, std::ios::binary);
    f << sf::Format(clips_, header_);
    if (!f) {
      status_ = "Couldn't save " + PathText(file_) + ".";
      return;
    }
  }
  std::error_code ec;
  fs::rename(tmp, file_, ec);
  if (ec) {
    fs::copy_file(tmp, file_, fs::copy_options::overwrite_existing, ec);
    fs::remove(tmp, ec);
  }
  if (ec) {
    status_ = "Couldn't save " + PathText(file_) + " (" + ec.message() + ").";
    return;
  }
  dirty_ = false;
  status_ = "Saved. The game picks it up within a couple of seconds.";
}

bool SubtitleEditor::Back() {
  const double now = ImGui::GetTime();
  if (!dirty_ || now < back_until_) {
    StopAudio();
    return true;
  }
  back_until_ = now + 5.0;
  status_ = "Unsaved changes: Save, or Back again to leave without saving.";
  return false;
}

bool SubtitleEditor::Playing() const { return playing_ || (loader_ && loader_->state == 1); }

void SubtitleEditor::Select(int clip) {
  if (clip == selected_) return;
  StopPlaying();
  selected_ = clip;
  line_ = -1;
  playhead_ms_ = 0;
  view_ms_ = view_len_ms_ = 0;
  drag_ = 0;
  // Its sound, a moment after (not for every clip passed on the way).
  fetch_at_ = ImGui::GetTime() + 0.2;
}

// ---- the sound ------------------------------------------------------------------

void SubtitleEditor::Fetch(const std::string& source) {
  if (audio_ && audio_source_ == source) return;
  if (loader_ && loader_->source == source) return;
  auto p = std::make_shared<Loader>();
  p->source = source;
  loader_ = p;
  const fs::path dir = install_dir_ / "subtitles" / "work" / "play";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const uint64_t h = sf::Fnv1a64(reinterpret_cast<const uint8_t*>(source.data()), source.size());
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.wav", static_cast<unsigned long long>(h));
  const fs::path wav = dir / name;
  std::vector<std::string> argv;
  if (!fs::exists(wav, ec)) {
    const fs::path tool = ToolPath();
    if (tool.empty()) {
      p->state = 3;
      p->error = "The subtitle maker isn't built (Update first): no sound to play.";
      return;
    }
    argv = {PathText(tool), "--game", PathText(install_dir_ / "game"), "--extract", source, "--wav", PathText(wav)};
  }
  std::thread([p, argv, wav] {
    std::string error;
    if (!argv.empty()) {
      std::string output, err;
      const int code = RunCapture(argv, &output, &err);
      std::error_code e2;
      if (code != 0 || !fs::exists(wav, e2)) {
        std::lock_guard lock(p->m);
        p->state = 3;
        p->error = "Couldn't get the sound (" + (err.empty() ? output.substr(0, 200) : err) + ").";
        return;
      }
    }
    auto a = std::make_shared<Audio>();
    const bool ok = Decode(wav, a.get(), &error);
    std::lock_guard lock(p->m);
    if (ok) {
      p->audio = a;
      p->state = 2;
    } else {
      p->state = 3;
      p->error = error;
    }
  }).detach();
}

void SubtitleEditor::UpdateAudio() {
  if (fetch_at_ > 0 && ImGui::GetTime() >= fetch_at_) {
    fetch_at_ = 0;
    if (selected_ >= 0 && selected_ < int(clips_.size())) Fetch(clips_[size_t(selected_)].source);
  }
  if (!loader_) return;
  const auto p = loader_;  // (outlives the lock)
  std::lock_guard lock(p->m);
  if (p->state == 2) {
    StopPlaying();
    audio_ = p->audio;
    audio_source_ = p->source;
  } else if (p->state == 3) {
    status_ = p->error;
  } else {
    return;
  }
  loader_.reset();
}

// The output stream, for the sound's rate and channels.
static bool EnsureStream(SDL_AudioStream** stream, int* channels, int* freq, const SubtitleEditor::Audio& a,
                         std::string* status) {
  if (*stream && *channels == a.channels && *freq == a.freq) return true;
  if (*stream) SDL_DestroyAudioStream(*stream);
  *stream = nullptr;
  if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
  SDL_AudioSpec spec{SDL_AUDIO_F32, a.channels, a.freq};
  *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!*stream) {
    *status = std::string("No sound output: ") + SDL_GetError();
    return false;
  }
  *channels = a.channels;
  *freq = a.freq;
  SDL_ResumeAudioStreamDevice(*stream);
  return true;
}

bool SubtitleEditor::Ready() const {
  return audio_ && selected_ >= 0 && selected_ < int(clips_.size()) &&
         audio_source_ == clips_[size_t(selected_)].source && audio_->frames > 0;
}

double SubtitleEditor::PlayPositionMs() const {
  if (!stream_ || !audio_) return playhead_ms_;
  const size_t bytes_per_frame = size_t(audio_->channels) * 4;
  const size_t queued = size_t(std::max(0, SDL_GetAudioStreamQueued(stream_))) / bytes_per_frame;
  size_t at = feed_frame_ > queued ? feed_frame_ - queued : 0;
  // (Looping: what's queued may run from the line's end back to its start.)
  size_t lo, hi;
  if (LoopSpan(&lo, &hi) && feed_frame_ >= lo && feed_frame_ - lo < queued) at = hi - (queued - (feed_frame_ - lo));
  return at * 1000.0 / audio_->freq;
}

// Looping the picked line: its span in frames (false: not looping).
bool SubtitleEditor::LoopSpan(size_t* f0, size_t* f1) const {
  if (!loop_ || !audio_ || selected_ < 0 || selected_ >= int(clips_.size())) return false;
  const auto& c = clips_[size_t(selected_)];
  if (line_ < 0 || line_ >= int(c.lines.size())) return false;
  const auto& l = c.lines[size_t(line_)];
  *f0 = std::min(audio_->frames, size_t(double(l.start_ms) * audio_->freq / 1000.0));
  *f1 = std::min(audio_->frames, size_t(double(l.end_ms) * audio_->freq / 1000.0));
  return *f1 > *f0 + size_t(audio_->freq / 20);
}

void SubtitleEditor::Feed() {
  if (!playing_ || !stream_ || !audio_) return;
  const Audio& a = *audio_;
  const size_t bpf = size_t(a.channels) * 4;
  size_t queued = size_t(std::max(0, SDL_GetAudioStreamQueued(stream_))) / bpf;
  const size_t want = size_t(a.freq * kQueueAhead * std::max(1.0f, speed_));
  size_t lo = 0, hi = a.frames;
  const bool looping = LoopSpan(&lo, &hi);
  while (queued < want && (looping || feed_frame_ < a.frames)) {
    if (looping && (feed_frame_ >= hi || feed_frame_ < lo)) feed_frame_ = lo;
    const size_t n = std::min(size_t(a.freq / 25), (looping ? hi : a.frames) - feed_frame_);
    SDL_PutAudioStreamData(stream_, a.pcm.data() + feed_frame_ * a.channels, int(n * bpf));
    feed_frame_ += n;
    queued += n;
  }
  if (feed_frame_ >= a.frames && queued == 0) {
    playing_ = false;
    playhead_ms_ = a.Ms();
  }
}

void SubtitleEditor::StartPlaying() {
  if (!Ready()) {
    if (loader_) status_ = "Getting the sound...";
    return;
  }
  if (!EnsureStream(&stream_, &stream_channels_, &stream_freq_, *audio_, &status_)) return;
  if (playhead_ms_ >= audio_->Ms() - 30.0) playhead_ms_ = 0;
  size_t lo, hi;
  if (LoopSpan(&lo, &hi)) {
    const double a = lo * 1000.0 / audio_->freq, b = hi * 1000.0 / audio_->freq;
    if (playhead_ms_ < a || playhead_ms_ >= b - 30.0) playhead_ms_ = a;
  }
  SDL_ClearAudioStream(stream_);
  SDL_SetAudioStreamFrequencyRatio(stream_, speed_);
  feed_frame_ = std::min(audio_->frames, size_t(playhead_ms_ * audio_->freq / 1000.0));
  playing_ = true;
  Feed();
}

void SubtitleEditor::StopPlaying() {
  if (playing_) playhead_ms_ = PlayPositionMs();
  playing_ = false;
  if (stream_) SDL_ClearAudioStream(stream_);
}

// A bit of the sound at `ms` (faded in and out: no clicks), heard while the
// playhead is dragged.
void SubtitleEditor::ScrubTo(double ms) {
  playhead_ms_ = ms;
  last_scrub_ms_ = ms;
  if (!Ready() || !EnsureStream(&stream_, &stream_channels_, &stream_freq_, *audio_, &status_)) return;
  const Audio& a = *audio_;
  const size_t f0 = std::min(a.frames, size_t(ms * a.freq / 1000.0));
  const size_t n = std::min(a.frames - f0, size_t(kGrainMs * a.freq / 1000.0));
  if (!n) return;
  std::vector<float> grain(a.pcm.begin() + std::ptrdiff_t(f0 * a.channels),
                           a.pcm.begin() + std::ptrdiff_t((f0 + n) * a.channels));
  const size_t ramp = std::max<size_t>(1, std::min(n / 2, size_t(a.freq / 200)));  // 5 ms
  for (size_t f = 0; f < n; ++f) {
    float g = 1.0f;
    if (f < ramp) g = float(f) / float(ramp);
    if (n - 1 - f < ramp) g = std::min(g, float(n - 1 - f) / float(ramp));
    for (int c = 0; c < a.channels; ++c) grain[f * a.channels + c] *= g;
  }
  SDL_ClearAudioStream(stream_);
  SDL_SetAudioStreamFrequencyRatio(stream_, 1.0f);
  SDL_PutAudioStreamData(stream_, grain.data(), int(grain.size() * 4));
}

void SubtitleEditor::StopAudio() {
  playing_ = false;
  if (stream_) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
  stream_channels_ = stream_freq_ = 0;
}

// ---- undo ---------------------------------------------------------------------

void SubtitleEditor::Remember() {
  if (selected_ < 0 || selected_ >= int(clips_.size())) return;
  undo_.push_back({selected_, clips_[size_t(selected_)]});
  if (undo_.size() > kUndoDepth) undo_.erase(undo_.begin());
  redo_.clear();
}

void SubtitleEditor::Undo(bool redo) {
  auto& from = redo ? redo_ : undo_;
  auto& to = redo ? undo_ : redo_;
  if (from.empty()) {
    status_ = redo ? "Nothing to redo." : "Nothing to undo.";
    return;
  }
  Step s = std::move(from.back());
  from.pop_back();
  if (s.clip < 0 || s.clip >= int(clips_.size())) return;
  to.push_back({s.clip, clips_[size_t(s.clip)]});
  clips_[size_t(s.clip)] = std::move(s.before);
  Select(s.clip);
  line_ = std::min(line_, int(clips_[size_t(s.clip)].lines.size()) - 1);
  dirty_ = true;
  status_ = redo ? "Redone." : "Undone.";
}

// ---- splitting and joining ------------------------------------------------------

bool SubtitleEditor::SplitAtPlayhead() {
  if (selected_ < 0 || selected_ >= int(clips_.size())) return false;
  sf::Clip& c = clips_[size_t(selected_)];
  // The picked line, or the one under the playhead.
  int i = line_;
  if (i < 0 || i >= int(c.lines.size()) || playhead_ms_ <= c.lines[size_t(i)].start_ms ||
      playhead_ms_ >= c.lines[size_t(i)].end_ms) {
    i = -1;
    for (int k = 0; k < int(c.lines.size()); ++k) {
      if (playhead_ms_ > c.lines[size_t(k)].start_ms && playhead_ms_ < c.lines[size_t(k)].end_ms) i = k;
    }
  }
  if (i < 0) {
    status_ = "Put the playhead inside the line to split.";
    return false;
  }
  Remember();
  sf::Line& l = c.lines[size_t(i)];
  const uint32_t at = uint32_t(playhead_ms_);
  // The words: split at the space nearest the same share of the text.
  const double share = double(at - l.start_ms) / double(l.end_ms - l.start_ms);
  const size_t want = size_t(share * double(l.text.size()));
  size_t cut = std::string::npos;
  for (size_t k = 0; k < l.text.size(); ++k) {
    if (l.text[k] != ' ') continue;
    if (cut == std::string::npos ||
        std::llabs(static_cast<long long>(k) - static_cast<long long>(want)) <
            std::llabs(static_cast<long long>(cut) - static_cast<long long>(want))) {
      cut = k;
    }
  }
  sf::Line second = l;
  second.start_ms = at;
  l.end_ms = at;
  if (cut != std::string::npos) {
    second.text = l.text.substr(cut + 1);
    l.text = l.text.substr(0, cut);
  } else {
    second.text = "";
  }
  c.lines.insert(c.lines.begin() + i + 1, second);
  line_ = i + 1;
  status_ = "Split.";
  return true;
}

bool SubtitleEditor::JoinNext() {
  if (selected_ < 0 || selected_ >= int(clips_.size())) return false;
  sf::Clip& c = clips_[size_t(selected_)];
  if (line_ < 0 || line_ + 1 >= int(c.lines.size())) {
    status_ = "No line after it to join.";
    return false;
  }
  Remember();
  sf::Line& a = c.lines[size_t(line_)];
  const sf::Line& b = c.lines[size_t(line_) + 1];
  a.start_ms = std::min(a.start_ms, b.start_ms);
  a.end_ms = std::max(a.end_ms, b.end_ms);
  a.text = a.text.empty() ? b.text : b.text.empty() ? a.text : a.text + " " + b.text;
  c.lines.erase(c.lines.begin() + line_ + 1);
  status_ = "Joined.";
  return true;
}

// ---- the timeline -----------------------------------------------------------------

bool SubtitleEditor::DrawTimeline(ImFont* font, float px, float unit) {
  sf::Clip& c = clips_[size_t(selected_)];
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool changed = false;
  const bool ready = Ready();
  // How long: the sound, or the clip as written, or its last line.
  double total = std::max<double>(c.duration_ms, 1000.0);
  if (ready) total = std::max(total, audio_->Ms());
  for (const auto& l : c.lines) total = std::max<double>(total, l.end_ms + 250.0);
  if (view_len_ms_ <= 0 || view_len_ms_ > total) {
    view_ms_ = 0;
    view_len_ms_ = total;
  }
  view_ms_ = std::clamp(view_ms_, 0.0, std::max(0.0, total - view_len_ms_));

  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float w = std::max(100.0f, ImGui::GetContentRegionAvail().x);
  const float ruler_h = px * 1.2f, lane_h = px * 2.2f, wave_h = px * 4.5f;
  const float h = ruler_h + lane_h + wave_h;
  const float lane_y = p0.y + ruler_h, wave_y = lane_y + lane_h;
  ImGui::InvisibleButton("##timeline", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
  const bool hovered = ImGui::IsItemHovered();
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
  auto to_x = [&](double ms) { return p0.x + float((ms - view_ms_) / view_len_ms_ * w); };
  auto to_ms = [&](float x) { return view_ms_ + double(x - p0.x) / w * view_len_ms_; };
  const float mx = io.MousePos.x, my = io.MousePos.y;
  const float edge = 6.0f * unit;

  // What the mouse is over in the lines' lane: a line's start (2), end (3)
  // or body (4).
  int hit = 0, hit_line = -1;
  if (my >= lane_y && my < wave_y) {
    for (int i = int(c.lines.size()) - 1; i >= 0 && !hit; --i) {
      const float x0 = to_x(c.lines[size_t(i)].start_ms), x1 = to_x(c.lines[size_t(i)].end_ms);
      if (mx < x0 - edge || mx > x1 + edge) continue;
      hit_line = i;
      hit = std::fabs(mx - x0) <= edge ? 2 : std::fabs(mx - x1) <= edge ? 3 : 4;
    }
  }

  // ---- input ----
  if (ImGui::IsItemActivated()) {
    if (hit) {
      drag_ = hit;
      drag_line_ = hit_line;
      line_ = hit_line;
      drag_grab_ms_ = to_ms(mx) - c.lines[size_t(hit_line)].start_ms;
      snap_playhead_ms_ = playhead_ms_;
      Remember();
      if (io.MouseClickedCount[0] >= 2 && hit == 4) {
        // (Double-click a line: play it.)
        playhead_ms_ = c.lines[size_t(hit_line)].start_ms;
        StartPlaying();
        drag_ = 0;
      }
    } else if (my >= lane_y && my < wave_y && io.MouseClickedCount[0] >= 2) {
      // Double-click empty lane: a new line there.
      Remember();
      sf::Line l;
      l.start_ms = uint32_t(std::max(0.0, to_ms(mx)));
      l.end_ms = l.start_ms + 2000;
      size_t at = 0;
      while (at < c.lines.size() && c.lines[at].start_ms < l.start_ms) ++at;
      if (at > 0) l.speaker = c.lines[at - 1].speaker;
      if (at < c.lines.size()) l.end_ms = std::min(l.end_ms, std::max(l.start_ms + 300, c.lines[at].start_ms));
      c.lines.insert(c.lines.begin() + std::ptrdiff_t(at), l);
      line_ = int(at);
      changed = true;
    } else {
      drag_ = 1;
      was_playing_ = playing_;
      StopPlaying();
      ScrubTo(std::clamp(to_ms(mx), 0.0, total));
    }
  }
  if (ImGui::IsItemActive() && drag_) {
    const double ms = std::clamp(to_ms(mx), 0.0, total);
    if (drag_ == 1) {
      if (std::fabs(ms - last_scrub_ms_) > view_len_ms_ / w * 0.5) ScrubTo(ms);
    } else if (drag_line_ >= 0 && drag_line_ < int(c.lines.size())) {
      sf::Line& l = c.lines[size_t(drag_line_)];
      const uint32_t s0 = l.start_ms, e0 = l.end_ms;
      // Edges snap to the other lines' and to where the playhead was (within
      // a few pixels; Alt: not).
      auto snap = [&](double t) {
        if (io.KeyAlt) return t;
        const double reach = 7.0 * unit / w * view_len_ms_;
        double best = t, best_d = reach;
        auto consider = [&](double v) {
          if (std::fabs(v - t) < best_d) {
            best_d = std::fabs(v - t);
            best = v;
          }
        };
        consider(snap_playhead_ms_);
        for (int k = 0; k < int(c.lines.size()); ++k) {
          if (k == drag_line_) continue;
          consider(c.lines[size_t(k)].start_ms);
          consider(c.lines[size_t(k)].end_ms);
        }
        return best;
      };
      if (drag_ == 2) {
        l.start_ms = uint32_t(std::clamp(snap(ms), 0.0, double(l.end_ms) - 100.0));
      } else if (drag_ == 3) {
        l.end_ms = uint32_t(std::clamp(snap(ms), double(l.start_ms) + 100.0, total));
      } else {
        const double dur = double(l.end_ms - l.start_ms);
        double st = ms - drag_grab_ms_;
        // (Whichever end is nearer something.)
        const double by_start = snap(st) - st, by_end = snap(st + dur) - (st + dur);
        st += std::fabs(by_start) > 0 && (std::fabs(by_end) == 0 || std::fabs(by_start) <= std::fabs(by_end)) ? by_start : by_end;
        st = std::clamp(st, 0.0, std::max(0.0, total - dur));
        l.start_ms = uint32_t(st);
        l.end_ms = uint32_t(st + dur);
      }
      if (l.start_ms != s0 || l.end_ms != e0) {
        changed = true;
        // Heard where the edge now is.
        if (drag_ != 4) ScrubTo(drag_ == 2 ? l.start_ms : std::max(0.0, double(l.end_ms) - kGrainMs));
      }
    }
    // Dragged past either side: the view follows.
    if (mx > p0.x + w) view_ms_ += view_len_ms_ * 0.02;
    if (mx < p0.x) view_ms_ -= view_len_ms_ * 0.02;
  }
  if (ImGui::IsItemDeactivated()) {
    if (drag_ == 1 && was_playing_) StartPlaying();
    if (drag_ >= 2 && drag_line_ >= 0 && drag_line_ < int(c.lines.size())) {
      // (Kept in order of their start.)
      const sf::Line moved = c.lines[size_t(drag_line_)];
      std::stable_sort(c.lines.begin(), c.lines.end(),
                       [](const sf::Line& a, const sf::Line& b) { return a.start_ms < b.start_ms; });
      for (int i = 0; i < int(c.lines.size()); ++i) {
        if (c.lines[size_t(i)].start_ms == moved.start_ms && c.lines[size_t(i)].text == moved.text) line_ = i;
      }
    }
    drag_ = 0;
    drag_line_ = -1;
  }
  if (hovered && !drag_ && (hit == 2 || hit == 3)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (drag_ == 2 || drag_ == 3) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (hovered && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)) {
    if (io.KeyShift || io.MouseWheelH != 0.0f) {
      const float steps = io.MouseWheelH != 0.0f ? -io.MouseWheelH : -io.MouseWheel;
      view_ms_ += steps * view_len_ms_ * 0.12;
    } else {
      const double anchor = to_ms(mx);
      const double len = std::clamp(view_len_ms_ * std::pow(0.8, double(io.MouseWheel)), 300.0, total);
      view_ms_ = anchor - double(mx - p0.x) / w * len;
      view_len_ms_ = len;
    }
    view_ms_ = std::clamp(view_ms_, 0.0, std::max(0.0, total - view_len_ms_));
  }
  // Playing: the view keeps the playhead in sight.
  if (playing_ && !drag_ && (playhead_ms_ > view_ms_ + view_len_ms_ * 0.95 || playhead_ms_ < view_ms_)) {
    view_ms_ = std::clamp(playhead_ms_ - view_len_ms_ * 0.05, 0.0, std::max(0.0, total - view_len_ms_));
  }

  // ---- drawing ----
  dl->PushClipRect(p0, ImVec2(p0.x + w, p0.y + h), true);
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(12, 20, 46, 255), 4.0f * unit);
  dl->AddRectFilled(ImVec2(p0.x, wave_y), ImVec2(p0.x + w, p0.y + h), IM_COL32(6, 12, 30, 255));
  // The ruler: a step that leaves room for its labels.
  {
    const double steps[] = {100, 250, 500, 1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000};
    double step = steps[std::size(steps) - 1];
    for (double s : steps) {
      if (s / view_len_ms_ * w >= px * 4.5f) {
        step = s;
        break;
      }
    }
    const double first = std::floor(view_ms_ / step) * step;
    for (double t = first; t <= view_ms_ + view_len_ms_; t += step) {
      const float x = to_x(t);
      dl->AddLine(ImVec2(x, p0.y + ruler_h * 0.55f), ImVec2(x, p0.y + h), IM_COL32(255, 255, 255, 28));
      const std::string label = Clock(t, step < 1000);
      dl->AddText(font, px * 0.75f, ImVec2(x + 3 * unit, p0.y + 1 * unit), kDim, label.c_str());
    }
  }
  // The waveform (min / max per pixel).
  const float mid = wave_y + wave_h * 0.5f, amp = wave_h * 0.46f;
  if (ready) {
    const Audio& a = *audio_;
    const double audio_ms = a.Ms();
    for (int x = 0; x < int(w); ++x) {
      const double t0 = to_ms(p0.x + x), t1 = to_ms(p0.x + x + 1);
      if (t1 < 0 || t0 > audio_ms) continue;
      size_t b0 = size_t(std::clamp(t0 / audio_ms, 0.0, 1.0) * (kPeakBuckets - 1));
      size_t b1 = size_t(std::clamp(t1 / audio_ms, 0.0, 1.0) * (kPeakBuckets - 1));
      float lo = 0, hi = 0;
      // Zoomed in past the buckets: the samples themselves.
      if (b1 <= b0 + 1 && (t1 - t0) * a.freq / 1000.0 < double(a.frames) / kPeakBuckets) {
        const size_t f0 = size_t(std::max(0.0, t0) * a.freq / 1000.0);
        const size_t f1 = std::min(a.frames, std::max(f0 + 1, size_t(t1 * a.freq / 1000.0)));
        for (size_t f = f0; f < f1 && f < a.frames; ++f) {
          float v = 0;
          for (int ch = 0; ch < a.channels; ++ch) v += a.pcm[f * a.channels + ch];
          v /= float(a.channels);
          lo = std::min(lo, v);
          hi = std::max(hi, v);
        }
      } else {
        for (size_t b = b0; b <= b1; ++b) {
          lo = std::min(lo, a.peaks[b * 2]);
          hi = std::max(hi, a.peaks[b * 2 + 1]);
        }
      }
      const float y0 = mid - std::min(1.0f, hi) * amp, y1 = mid - std::max(-1.0f, lo) * amp;
      dl->AddLine(ImVec2(p0.x + x + 0.5f, y0), ImVec2(p0.x + x + 0.5f, std::max(y1, y0 + 1.0f)),
                  IM_COL32(110, 170, 255, 200));
    }
  } else {
    const char* msg = loader_ ? "Getting the sound..." : "No sound";
    dl->AddText(font, px, ImVec2(p0.x + px, mid - px * 0.5f), kDim, msg);
  }
  // The lines, in the speaker's colour (bad times: red).
  for (int i = 0; i < int(c.lines.size()); ++i) {
    const sf::Line& l = c.lines[size_t(i)];
    const float x0 = to_x(l.start_ms), x1 = std::max(to_x(l.end_ms), x0 + 2.0f);
    if (x1 < p0.x || x0 > p0.x + w) continue;
    const ImU32 col = kSpeaker[std::clamp(l.speaker, 0, 3)];
    const bool sel = i == line_;
    const bool bad = l.end_ms <= l.start_ms ||
                     (i + 1 < int(c.lines.size()) && c.lines[size_t(i) + 1].start_ms < l.end_ms);
    const ImVec2 a(x0, lane_y + 3 * unit), b(x1, wave_y - 3 * unit);
    dl->AddRectFilled(a, b, WithAlpha(col, sel ? 150 : 70), 3.0f * unit);
    // Its span shaded over the waveform too.
    dl->AddRectFilled(ImVec2(x0, wave_y), ImVec2(x1, p0.y + h), WithAlpha(col, sel ? 34 : 14));
    dl->AddRect(a, b, bad ? kRed : sel ? kGold : WithAlpha(col, 200), 3.0f * unit, 0, sel || bad ? 2.0f : 1.0f);
    dl->PushClipRect(ImVec2(x0 + 3 * unit, a.y), ImVec2(x1 - 3 * unit, b.y), true);
    dl->AddText(font, px * 0.85f, ImVec2(x0 + 5 * unit, a.y + (b.y - a.y - px * 0.85f) * 0.5f),
                sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 210), l.text.c_str());
    dl->PopClipRect();
  }
  // The playhead.
  {
    const float x = to_x(playhead_ms_);
    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + h), kGold, 2.0f);
    dl->AddTriangleFilled(ImVec2(x - 6 * unit, p0.y), ImVec2(x + 6 * unit, p0.y), ImVec2(x, p0.y + 8 * unit), kGold);
  }
  // Where the mouse is, while over it.
  if (hovered && !drag_) {
    dl->AddLine(ImVec2(mx, p0.y), ImVec2(mx, p0.y + h), IM_COL32(255, 255, 255, 60));
  }
  dl->PopClipRect();

  // The whole clip in a strip, the view in it (drag to move the view).
  const ImVec2 q0 = ImGui::GetCursorScreenPos();
  const float strip_h = 7.0f * unit;
  ImGui::InvisibleButton("##overview", ImVec2(w, strip_h + 4 * unit));
  dl->AddRectFilled(ImVec2(q0.x, q0.y + 2 * unit), ImVec2(q0.x + w, q0.y + 2 * unit + strip_h),
                    IM_COL32(24, 36, 70, 255), strip_h * 0.5f);
  for (const auto& l : c.lines) {
    const float x0 = q0.x + float(l.start_ms / total) * w, x1 = q0.x + float(l.end_ms / total) * w;
    dl->AddRectFilled(ImVec2(x0, q0.y + 3 * unit), ImVec2(std::max(x1, x0 + 1), q0.y + 1 * unit + strip_h),
                      WithAlpha(kSpeaker[std::clamp(l.speaker, 0, 3)], 90));
  }
  const float v0 = q0.x + float(view_ms_ / total) * w, v1 = q0.x + float((view_ms_ + view_len_ms_) / total) * w;
  dl->AddRect(ImVec2(v0, q0.y + 1 * unit), ImVec2(std::max(v1, v0 + 4), q0.y + 3 * unit + strip_h), kGold,
              strip_h * 0.5f, 0, 1.5f);
  dl->AddLine(ImVec2(q0.x + float(playhead_ms_ / total) * w, q0.y),
              ImVec2(q0.x + float(playhead_ms_ / total) * w, q0.y + strip_h + 4 * unit), kGold);
  if (ImGui::IsItemActive()) {
    view_ms_ = std::clamp(double(io.MousePos.x - q0.x) / w * total - view_len_ms_ * 0.5, 0.0,
                          std::max(0.0, total - view_len_ms_));
  }

  // What's heard now, as the game shows it.
  {
    ImGui::Spacing();
    const ImVec2 b0 = ImGui::GetCursorScreenPos();
    const float bh = px * 3.6f;
    ImGui::Dummy(ImVec2(w, bh));
    dl->AddRectFilled(b0, ImVec2(b0.x + w, b0.y + bh), IM_COL32(0, 0, 0, 200), 6.0f * unit);
    std::vector<const sf::Line*> now;
    for (const auto& l : c.lines) {
      if (playhead_ms_ >= l.start_ms && playhead_ms_ < l.end_ms) now.push_back(&l);
    }
    const float tpx = px * 1.25f;
    if (now.empty()) {
      const char* t = "(no line here)";
      const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0, t);
      dl->AddText(font, px, ImVec2(b0.x + (w - ts.x) * 0.5f, b0.y + (bh - ts.y) * 0.5f), kDim, t);
    } else {
      float y = b0.y + (bh - tpx * 1.2f * float(std::min<size_t>(now.size(), 2))) * 0.5f;
      for (size_t k = 0; k < now.size() && k < 2; ++k) {
        const char* t = now[k]->text.c_str();
        const ImVec2 ts = font->CalcTextSizeA(tpx, w - 20 * unit, 0, t);
        const float x = b0.x + std::max(10 * unit, (w - ts.x) * 0.5f);
        dl->AddText(font, tpx, ImVec2(x + 2, y + 2), IM_COL32(0, 0, 0, 255), t);
        dl->AddText(font, tpx, ImVec2(x, y), kSpeaker[std::clamp(now[k]->speaker, 0, 3)], t);
        y += tpx * 1.2f;
      }
    }
  }
  return changed;
}

// ---- the page ---------------------------------------------------------------------

bool SubtitleEditor::Draw(ImFont* font, float unit) {
  UpdateAudio();
  // Testing: AMPED3_EDITOR_SEEK=<ms>[,<line>][,<zoom ms>] once the sound's in.
  if (static bool seeked = false; !seeked && Ready()) {
    seeked = true;
    if (const char* s = std::getenv("AMPED3_EDITOR_SEEK")) {
      double ms = 0, zoom = 0;
      int ln = -1;
      std::sscanf(s, "%lf,%d,%lf", &ms, &ln, &zoom);
      line_ = ln;
      if (zoom > 0) {
        view_len_ms_ = zoom;
        view_ms_ = std::max(0.0, ms - zoom * 0.3);
      }
      ScrubTo(ms);
    }
  }
  Feed();
  if (playing_) playhead_ms_ = PlayPositionMs();

  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 display = io.DisplaySize;
  const float px = 17.0f * unit;
  const float pad = 16.0f * unit;
  bool close = false;

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(display);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(8, 14, 34, 255));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(24, 36, 70, 255));
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 56, 100, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70, 90, 150, 255));
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(255, 204, 0, 60));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(255, 204, 0, 40));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * unit);
  ImGui::Begin("##subtitle-editor", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PushFont(font, px);

  // The top bar.
  ImGui::PushFont(font, px * 1.4f);
  ImGui::TextColored(ImColor(kGold), "Subtitle editor");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::TextColored(ImColor(kDim), "  %s", PathText(file_).c_str());
  const char* filters[] = {"All", "Story", "Movies", "Riding", "Corrected"};
  ImGui::SetNextItemWidth(150 * unit);
  ImGui::Combo("##filter", &filter_, filters, 5);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(260 * unit);
  ImGui::InputTextWithHint("##search", "Search the words or the sound", search_, sizeof(search_));
  ImGui::SameLine();
  ImGui::BeginDisabled(!dirty_);
  if (ImGui::Button(dirty_ ? "Save" : "Saved")) Save();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(undo_.empty());
  if (ImGui::Button("Undo")) Undo(false);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(redo_.empty());
  if (ImGui::Button("Redo")) Undo(true);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Back")) close = Back();
  if (!status_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImColor(kGold), "%s", status_.c_str());
  }

  // The clips.
  const float list_w = display.x * 0.30f;
  const float body_h = display.y - ImGui::GetCursorPosY() - pad;
  ImGui::BeginChild("##clips", ImVec2(list_w, body_h), ImGuiChildFlags_Borders);
  const std::string needle = Lower(search_);
  visible_.clear();
  for (int i = 0; i < int(clips_.size()); ++i) {
    const sf::Clip& c = clips_[size_t(i)];
    if (filter_ == 1 && c.kind != sf::Kind::kStory) continue;
    if (filter_ == 2 && c.kind != sf::Kind::kVideo) continue;
    if (filter_ == 3 && c.kind != sf::Kind::kSpeech) continue;
    if (filter_ == 4 && !c.edited) continue;
    if (!needle.empty()) {
      bool hit = Lower(c.source).find(needle) != std::string::npos;
      for (const auto& l : c.lines) hit = hit || Lower(l.text).find(needle) != std::string::npos;
      if (!hit) continue;
    }
    std::string label = std::string(c.edited ? "* " : "") + "[" + KindLabel(c.kind) + "] " +
                        (c.lines.empty() ? std::string("(no lines)") : c.lines[0].text);
    std::string src = c.source;
    if (src.size() > 60) src = "..." + src.substr(src.size() - 57);
    ImGui::PushID(i);
    visible_.push_back(i);
    if (ImGui::Selectable("##clip", selected_ == i, 0, ImVec2(0, px * 2.3f))) Select(i);
    if (selected_ == i && scroll_to_selected_) {
      ImGui::SetScrollHereY(0.4f);
      scroll_to_selected_ = false;
    }
    ImGui::SameLine(px * 0.4f);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label.c_str());
    ImGui::TextColored(ImColor(kDim), "%s   %s", src.c_str(), Clock(c.duration_ms, false).c_str());
    ImGui::EndGroup();
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::SameLine();

  // The selected clip: its timeline, then its lines.
  ImGui::BeginChild("##clip", ImVec2(0, body_h), ImGuiChildFlags_Borders);
  if (selected_ >= 0 && selected_ < int(clips_.size())) {
    bool changed = false;
    {
      sf::Clip& c = clips_[size_t(selected_)];
      ImGui::TextColored(ImColor(kDim), "%s   %s, %s", c.source.c_str(), KindLabel(c.kind),
                         Clock(c.duration_ms).c_str());
      // The transport.
      if (ImGui::Button(playing_ ? "Pause" : "Play")) playing_ ? StopPlaying() : StartPlaying();
      ImGui::SameLine();
      if (ImGui::Button("|<")) {
        StopPlaying();
        playhead_ms_ = 0;
      }
      ImGui::SameLine();
      ImGui::Text("%s / %s", Clock(playhead_ms_).c_str(),
                  Clock(Ready() ? audio_->Ms() : double(c.duration_ms)).c_str());
      ImGui::SameLine();
      {
        // Slower, to catch every word (the pitch drops with it).
        const float speeds[] = {0.5f, 0.75f, 1.0f};
        const char* names[] = {"0.5x", "0.75x", "1x"};
        int cur = speed_ <= 0.5f ? 0 : speed_ < 1.0f ? 1 : 2;
        ImGui::SetNextItemWidth(70 * unit);
        if (ImGui::Combo("##speed", &cur, names, 3)) {
          speed_ = speeds[cur];
          if (playing_) {
            StopPlaying();
            StartPlaying();
          }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playing speed");
      }
      ImGui::SameLine();
      if (ImGui::Checkbox("Loop line", &loop_) && playing_) {
        StopPlaying();
        StartPlaying();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play the picked line over and over (L)");
      ImGui::SameLine();
      if (ImGui::Button("Fit")) view_len_ms_ = 0;
      const bool have_line = line_ >= 0 && line_ < int(c.lines.size());
      ImGui::BeginDisabled(!have_line);
      if (ImGui::Button("Start here [")) {
        Remember();
        c.lines[size_t(line_)].start_ms = uint32_t(playhead_ms_);
        if (c.lines[size_t(line_)].end_ms <= c.lines[size_t(line_)].start_ms) c.lines[size_t(line_)].end_ms = uint32_t(playhead_ms_ + 1500);
        changed = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("End here ]")) {
        Remember();
        c.lines[size_t(line_)].end_ms = uint32_t(std::max(playhead_ms_, double(c.lines[size_t(line_)].start_ms) + 100.0));
        changed = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("Split /")) changed |= SplitAtPlayhead();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Split the picked line in two at the playhead (its words too)");
      ImGui::SameLine();
      if (ImGui::Button("Join next")) changed |= JoinNext();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Join the picked line and the one after it (M)");
      ImGui::EndDisabled();
      ImGui::SameLine();
      bool edited = c.edited;
      if (ImGui::Checkbox("Corrected by hand", &edited)) {
        Remember();
        c.edited = edited;
        dirty_ = true;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Kept as it is when the subtitles are made again.");
    }
    changed |= DrawTimeline(font, px, unit);
    sf::Clip& c = clips_[size_t(selected_)];

    // The lines.
    ImGui::BeginChild("##lines", ImVec2(0, 0));
    ImGui::TextColored(ImColor(kDim), "     Speaker   Start s    End s     Words");
    int remove = -1, insert_after = -2;
    bool at_playhead = false;
    for (int i = 0; i < int(c.lines.size()); ++i) {
      sf::Line& l = c.lines[size_t(i)];
      ImGui::PushID(i);
      const bool hearing = playhead_ms_ >= double(l.start_ms) && playhead_ms_ < double(l.end_ms);
      const bool sel = i == line_;
      // Pick (and hear) the line: the playhead to its start.
      ImGui::PushStyleColor(ImGuiCol_Button, sel ? IM_COL32(255, 204, 0, 120) : IM_COL32(40, 56, 100, 255));
      if (ImGui::Button(">")) {
        line_ = i;
        playhead_ms_ = l.start_ms > 250 ? l.start_ms - 250 : 0;
        StartPlaying();
      }
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play from this line");
      ImGui::SameLine();
      // The speaker: a swatch of their colour.
      const int sp = std::clamp(l.speaker, 0, 3);
      ImGui::PushStyleColor(ImGuiCol_Button, kSpeaker[sp]);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSpeaker[sp]);
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
      char who[8];
      std::snprintf(who, sizeof(who), " %d ", sp + 1);
      if (ImGui::Button(who)) {
        Remember();
        l.speaker = (sp + 1) % sf::kSpeakers;
        changed = true;
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        Remember();
        l.speaker = (sp + sf::kSpeakers - 1) % sf::kSpeakers;
        changed = true;
      }
      ImGui::PopStyleColor(3);
      ImGui::SameLine();
      const bool bad = l.end_ms <= l.start_ms ||
                       (i + 1 < int(c.lines.size()) && c.lines[size_t(i) + 1].start_ms < l.end_ms);
      float start = l.start_ms / 1000.0f, end = l.end_ms / 1000.0f;
      if (bad) ImGui::PushStyleColor(ImGuiCol_Text, kRed);
      ImGui::SetNextItemWidth(80 * unit);
      if (ImGui::DragFloat("##start", &start, 0.01f, 0.0f, 36000.0f, "%.2f")) {
        l.start_ms = uint32_t(std::max(0.0f, start) * 1000.0f + 0.5f);
        changed = true;
      }
      if (ImGui::IsItemActivated()) {
        Remember();
        line_ = i;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag, or double-click to type (seconds)");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(80 * unit);
      if (ImGui::DragFloat("##end", &end, 0.01f, 0.0f, 36000.0f, "%.2f")) {
        l.end_ms = uint32_t(std::max(0.0f, end) * 1000.0f + 0.5f);
        changed = true;
      }
      if (ImGui::IsItemActivated()) {
        Remember();
        line_ = i;
      }
      if (bad) ImGui::PopStyleColor();
      if (bad && ImGui::IsItemHovered()) ImGui::SetTooltip("Ends before it starts, or runs into the next line");
      ImGui::SameLine();
      char buf[1024];
      std::snprintf(buf, sizeof(buf), "%s", l.text.c_str());
      ImGui::SetNextItemWidth(-px * 4.2f);
      int pushed = 0;
      if (hearing) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 204, 0, 70));
        ++pushed;
      } else if (sel) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(60, 80, 140, 255));
        ++pushed;
      }
      ImGui::PushStyleColor(ImGuiCol_Text, kSpeaker[sp]);
      ++pushed;
      if (ImGui::InputText("##text", buf, sizeof(buf))) {
        l.text = sf::OneLine(buf);
        changed = true;
      }
      if (ImGui::IsItemActivated()) {
        Remember();
        line_ = i;
      }
      ImGui::PopStyleColor(pushed);
      ImGui::SameLine();
      if (ImGui::Button("+")) insert_after = i;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("A new line after this one");
      ImGui::SameLine();
      if (ImGui::Button("x")) remove = i;
      ImGui::PopID();
    }
    if (ImGui::Button("+ Add a line")) insert_after = int(c.lines.size()) - 1;
    ImGui::SameLine();
    if (ImGui::Button("+ Add one at the playhead")) {
      int after = -1;
      while (after + 1 < int(c.lines.size()) && c.lines[size_t(after) + 1].start_ms <= playhead_ms_) ++after;
      insert_after = after;
      at_playhead = true;
    }
    if (remove >= 0) {
      Remember();
      c.lines.erase(c.lines.begin() + remove);
      if (line_ >= int(c.lines.size())) line_ = int(c.lines.size()) - 1;
      changed = true;
    }
    if (insert_after >= -1) {
      Remember();
      sf::Line l;
      const size_t at = size_t(insert_after + 1);
      l.start_ms = insert_after >= 0 ? c.lines[size_t(insert_after)].end_ms : 0;
      if (at_playhead) l.start_ms = uint32_t(playhead_ms_);
      l.end_ms = l.start_ms + 2000;
      if (at < c.lines.size()) l.end_ms = std::min(l.end_ms, std::max(l.start_ms + 300, c.lines[at].start_ms));
      l.speaker = insert_after >= 0 ? c.lines[size_t(insert_after)].speaker : 0;
      c.lines.insert(c.lines.begin() + std::ptrdiff_t(at), l);
      line_ = int(at);
      changed = true;
    }
    if (changed) {
      c.edited = true;
      dirty_ = true;
      status_.clear();
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "Timeline: click or drag to scrub (you hear it as you go), drag a line to move it or its ends to "
        "retime it, double-click a line to play it or an empty spot to add one; wheel zooms, Shift+wheel "
        "pans. Space plays / pauses, Left / Right step (Shift: a second), [ and ] set the picked line's "
        "start and end at the playhead, Up / Down pick a line, / splits it at the playhead, M joins it "
        "to the next, L loops it. Dragged edges snap to other lines and the playhead (Alt: not). "
        "Speakers: 1 white, 2 yellow, 3 pink, 4 green (click for the next, right-click for the one "
        "before, or the keys 1 to 4). Page Up / Down: the previous / next clip. Ctrl+Z / Ctrl+Y undo "
        "and redo, Ctrl+S saves.");
    ImGui::PopStyleColor();
    ImGui::EndChild();
  } else {
    ImGui::TextColored(ImColor(kDim), "Choose a clip on the left.");
  }
  ImGui::EndChild();

  ImGui::PopFont();
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(6);

  // Keys (not while typing).
  if (dirty_ && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save();
  if (!io.WantTextInput && selected_ >= 0 && selected_ < int(clips_.size())) {
    sf::Clip& c = clips_[size_t(selected_)];
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) Undo(io.KeyShift);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Undo(true);
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) playing_ ? StopPlaying() : StartPlaying();
    const double step = io.KeyShift ? 1000.0 : 100.0;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
      const bool was = playing_;
      StopPlaying();
      const double to = playhead_ms_ + (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? step : -step);
      ScrubTo(std::max(0.0, to));
      if (was) StartPlaying();
    }
    if (!c.lines.empty() && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))) {
      line_ = std::clamp(line_ + (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : -1), 0, int(c.lines.size()) - 1);
      StopPlaying();
      ScrubTo(c.lines[size_t(line_)].start_ms);
      // (Into view.)
      if (view_len_ms_ > 0 && (playhead_ms_ < view_ms_ || playhead_ms_ > view_ms_ + view_len_ms_)) {
        view_ms_ = std::max(0.0, playhead_ms_ - view_len_ms_ * 0.2);
      }
    }
    const bool have_line = line_ >= 0 && line_ < int(c.lines.size());
    // Speaker of the picked line: 1 to 4.
    const ImGuiKey digits[4] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4};
    for (int k = 0; k < 4 && have_line; ++k) {
      if (!io.KeyCtrl && ImGui::IsKeyPressed(digits[k], false) && c.lines[size_t(line_)].speaker != k) {
        Remember();
        c.lines[size_t(line_)].speaker = k;
        c.edited = dirty_ = true;
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_L, false)) {
      loop_ = !loop_;
      if (playing_) {
        StopPlaying();
        StartPlaying();
      }
    }
    if (have_line && ImGui::IsKeyPressed(ImGuiKey_Slash, false) && SplitAtPlayhead()) c.edited = dirty_ = true;
    if (have_line && ImGui::IsKeyPressed(ImGuiKey_M, false) && JoinNext()) c.edited = dirty_ = true;
    // The next / previous clip in the list.
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown) || ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
      const bool down = ImGui::IsKeyPressed(ImGuiKey_PageDown);
      auto it = std::find(visible_.begin(), visible_.end(), selected_);
      int to = -1;
      if (it == visible_.end()) {
        if (!visible_.empty()) to = visible_.front();
      } else if (down && it + 1 != visible_.end()) {
        to = *(it + 1);
      } else if (!down && it != visible_.begin()) {
        to = *(it - 1);
      }
      if (to >= 0) {
        Select(to);
        scroll_to_selected_ = true;
        return close;
      }
    }
    if (have_line && ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) {
      Remember();
      sf::Line& l = c.lines[size_t(line_)];
      l.start_ms = uint32_t(playhead_ms_);
      if (l.end_ms <= l.start_ms) l.end_ms = l.start_ms + 1500;
      c.edited = dirty_ = true;
    }
    if (have_line && ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) {
      Remember();
      sf::Line& l = c.lines[size_t(line_)];
      l.end_ms = uint32_t(std::max(playhead_ms_, double(l.start_ms) + 100.0));
      c.edited = dirty_ = true;
    }
  }
  return close;
}

}  // namespace launcher
