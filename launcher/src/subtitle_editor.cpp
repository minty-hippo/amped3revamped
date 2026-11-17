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

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "platform.h"
#include "process.h"
#include "../../src/subtitle_draw.h"

namespace launcher {

namespace sf = amped3::subtitle_file;

namespace {

// As the game shows them: white, then the yellow, pink and green of its
// menus, then six more (subtitle_file.h).
const struct SpeakerColors {
  ImU32 c[sf::kSpeakers];
  SpeakerColors() {
    for (int i = 0; i < sf::kSpeakers; ++i) {
      c[i] = IM_COL32(sf::kSpeakerRgb[i][0], sf::kSpeakerRgb[i][1], sf::kSpeakerRgb[i][2], 255);
    }
  }
  ImU32 operator[](int i) const { return c[std::clamp(i, 0, sf::kSpeakers - 1)]; }
} kSpeaker;
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

struct SubtitleEditor::VideoLoader {
  std::mutex m;
  int state = 1;  // 1 working, 2 ready, 3 failed
  std::string source;
  std::shared_ptr<Video> video;
  std::string error;
};

SubtitleEditor::~SubtitleEditor() { Shutdown(); }

void SubtitleEditor::Shutdown() {
  StopAudio();
  if (video_tex_) SDL_DestroyTexture(video_tex_);
  video_tex_ = nullptr;
  video_tex_frame_ = -1;
}

void SubtitleEditor::SetRenderer(SDL_Renderer* renderer) {
  if (renderer == renderer_) return;
  if (video_tex_) SDL_DestroyTexture(video_tex_);
  video_tex_ = nullptr;
  video_tex_frame_ = -1;
  renderer_ = renderer;
}

std::string SubtitleEditor::Named(const sf::Clip& c, const sf::Line& l) const {
  const std::string& n = c.names[std::clamp(l.speaker, 0, sf::kSpeakers - 1)];
  return n.empty() ? l.text : n + ": " + l.text;
}

// ---- the movie preview ---------------------------------------------------------

namespace {

bool ReadVideoIndex(const fs::path& dir, SubtitleEditor::Video* v) {
  std::ifstream in(dir / "index.txt");
  std::string a, b, c, d;
  if (!(in >> a >> v->fps >> b >> v->frames >> c >> v->width >> d >> v->height)) return false;
  v->dir = dir;
  return a == "fps" && v->fps > 0 && v->frames > 0 && v->width > 0 && v->height > 0;
}

}  // namespace

void SubtitleEditor::FetchVideo(const std::string& source) {
  if (video_ && video_source_ == source) return;
  if (video_loader_ && video_loader_->source == source) return;
  auto p = std::make_shared<VideoLoader>();
  p->source = source;
  video_loader_ = p;
  const uint64_t h = sf::Fnv1a64(reinterpret_cast<const uint8_t*>(source.data()), source.size());
  char name[40];
  std::snprintf(name, sizeof(name), "%016llx_frames", static_cast<unsigned long long>(h));
  const fs::path dir = install_dir_ / "subtitles" / "work" / "play" / name;
  std::vector<std::string> argv;
  std::error_code ec;
  if (!fs::exists(dir / "index.txt", ec)) {
    const fs::path tool = ToolPath();
    if (tool.empty()) {
      p->state = 3;
      p->error = "The subtitle maker isn't built (Update first): no movie to show.";
      return;
    }
    argv = {PathText(tool), "--game", PathText(install_dir_ / "game"), "--movie-frames", source, "--frames",
            PathText(dir)};
  }
  std::thread([p, argv, dir] {
    if (!argv.empty()) {
      std::string output, err;
      const int code = RunCapture(argv, &output, &err);
      std::error_code e2;
      if (code != 0 || !fs::exists(dir / "index.txt", e2)) {
        std::lock_guard lock(p->m);
        p->state = 3;
        p->error = "Couldn't get the movie's pictures (" + (err.empty() ? output.substr(0, 200) : err) +
                   "): the subtitle maker may need updating.";
        return;
      }
    }
    auto v = std::make_shared<Video>();
    const bool ok = ReadVideoIndex(dir, v.get());
    std::lock_guard lock(p->m);
    if (ok) {
      p->video = v;
      p->state = 2;
    } else {
      p->state = 3;
      p->error = "The movie's pictures couldn't be read.";
    }
  }).detach();
}

void SubtitleEditor::UpdateVideo() {
  if (!video_loader_) return;
  const auto p = video_loader_;
  std::lock_guard lock(p->m);
  if (p->state == 2) {
    video_ = p->video;
    video_source_ = p->source;
    video_tex_frame_ = -1;
  } else if (p->state == 3) {
    status_ = p->error;
  } else {
    return;
  }
  video_loader_.reset();
}

bool SubtitleEditor::VideoFrame(int* width, int* height) {
  if (!renderer_ || !video_ || selected_ < 0 || selected_ >= int(clips_.size()) ||
      clips_[size_t(selected_)].source != video_source_) {
    return false;
  }
  const Video& v = *video_;
  if (!video_tex_) {
    video_tex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, v.width, v.height);
    if (!video_tex_) return false;
    SDL_SetTextureScaleMode(video_tex_, SDL_SCALEMODE_LINEAR);
    video_tex_frame_ = -1;
  }
  float tw = 0, th = 0;
  SDL_GetTextureSize(video_tex_, &tw, &th);
  if (int(tw) != v.width || int(th) != v.height) {
    SDL_DestroyTexture(video_tex_);
    video_tex_ = nullptr;
    return VideoFrame(width, height);
  }
  // The picture at the playhead (the sound and the pictures start together).
  const int frame = std::clamp(int(playhead_ms_ / 1000.0 * v.fps), 0, v.frames - 1);
  if (frame != video_tex_frame_) {
    char name[32];
    std::snprintf(name, sizeof(name), "frame_%05d.jpg", frame);
    std::ifstream in(v.dir / name, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = bytes.empty() ? nullptr : stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comp, 4);
    if (px && w == v.width && h == v.height) {
      SDL_UpdateTexture(video_tex_, nullptr, px, w * 4);
      video_tex_frame_ = frame;
    }
    if (px) stbi_image_free(px);
  }
  *width = v.width;
  *height = v.height;
  return video_tex_frame_ >= 0;
}

// ---- speakers' names ---------------------------------------------------------------

bool SubtitleEditor::DrawNames(float px, float unit) {
  sf::Clip& c = clips_[size_t(selected_)];
  // The names typed in anywhere, offered (gathered now and then).
  if (ImGui::GetTime() >= names_scan_at_) {
    names_scan_at_ = ImGui::GetTime() + 1.0;
    known_names_.clear();
    for (const sf::Clip& k : clips_) {
      for (const std::string& n : k.names) {
        if (!n.empty() && std::find(known_names_.begin(), known_names_.end(), n) == known_names_.end()) {
          known_names_.push_back(n);
        }
      }
    }
    std::sort(known_names_.begin(), known_names_.end());
  }
  // The speakers shown: those who say anything here or are named, and any
  // added (+, up to ten).
  const int used = Speakers(c);
  bool changed = false;
  const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(ImColor(kDim), "Speakers");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Who each speaker is: shown before their lines (\"Name: line\") when the game's Speaker "
                      "names setting is on. Leave one empty to show none. + adds a speaker (up to ten).");
  }
  const float field_w = 130 * unit + ImGui::GetFrameHeight() + 2 * unit;
  for (int i = 0; i < used; ++i) {
    // (Onto the next row when this one's full.)
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + field_w > right) {
      ImGui::AlignTextToFramePadding();
    } else {
      ImGui::SameLine();
    }
    ImGui::PushID(1000 + i);
    char hint[16];
    std::snprintf(hint, sizeof(hint), "Speaker %d", i + 1);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", c.names[i].c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, kSpeaker[i]);
    ImGui::SetNextItemWidth(130 * unit);
    if (ImGui::InputTextWithHint("##name", hint, buf, sizeof(buf))) {
      std::string n = sf::OneLine(buf);
      n.erase(std::remove(n.begin(), n.end(), '|'), n.end());
      c.names[i] = n;
      changed = true;
    }
    if (ImGui::IsItemActivated()) Remember();
    ImGui::PopStyleColor();
    // Pick one already typed in elsewhere.
    ImGui::SameLine(0, 2 * unit);
    if (ImGui::ArrowButton("##pick", ImGuiDir_Down)) ImGui::OpenPopup("##names");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A name used in other clips");
    if (ImGui::BeginPopup("##names")) {
      if (known_names_.empty()) ImGui::TextColored(ImColor(kDim), "No names typed in yet");
      for (const std::string& n : known_names_) {
        if (ImGui::Selectable(n.c_str(), n == c.names[i])) {
          Remember();
          c.names[i] = n;
          changed = true;
        }
      }
      if (!c.names[i].empty() && ImGui::Selectable("(no name)")) {
        Remember();
        c.names[i].clear();
        changed = true;
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  if (used < sf::kSpeakers) {
    ImGui::SameLine();
    if (ImGui::Button("+##speaker")) {
      extra_speakers_ = used + 1;
      extra_speakers_clip_ = selected_;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Another speaker (up to ten)");
  }
  (void)px;
  return changed;
}

// ---- the game's look for the subtitles (amped3.toml) --------------------------------

namespace {
const GameSetting* Find(const char* key) {
  for (const GameSetting& g : SubtitleSettings()) {
    if (std::strcmp(g.key, key) == 0) return &g;
  }
  return nullptr;
}
}  // namespace

amped3::subtitle_draw::Style SubtitleEditor::GameStyle() const {
  amped3::subtitle_draw::Style st;
  auto get = [&](const char* key, int fallback) {
    const GameSetting* g = Find(key);
    return g ? std::atoi(game_.Get(*g).c_str()) : fallback;
  };
  st.size = get("subtitles_size", st.size);
  st.background = get("subtitles_background", st.background);
  st.box = get("subtitles_box", st.box);
  st.edge = get("subtitles_edge", st.edge);
  st.position = get("subtitles_position", st.position);
  st.width = get("subtitles_width", st.width);
  return st;
}

bool SubtitleEditor::GameColors() const {
  const GameSetting* g = Find("subtitles_speaker_colors");
  return !g || game_.Get(*g) == "true";
}

bool SubtitleEditor::GameNames() const {
  const GameSetting* g = Find("subtitles_speaker_names");
  return !g || game_.Get(*g) == "true";
}

// The Look popup: the game's subtitle settings, changed here (saved to the
// game's settings at once: the game picks them up when it next starts).
void SubtitleEditor::DrawLook(float unit) {
  if (!ImGui::BeginPopup("##look")) return;
  ImGui::TextColored(ImColor(kGold), "How the subtitles look in game");
  ImGui::TextColored(ImColor(kDim), "Saved to the game's settings (it reads them when it starts).");
  ImGui::Separator();
  for (const GameSetting& g : SubtitleSettings()) {
    if (std::strcmp(g.key, "subtitles") == 0) continue;
    ImGui::PushID(g.key);
    const std::string v = game_.Get(g);
    ImGui::SetNextItemWidth(220 * unit);
    switch (g.kind) {
      case GameSetting::Kind::kToggle: {
        bool on = v == "true";
        if (ImGui::Checkbox(g.label, &on)) game_.Set(g, on ? "true" : "false");
        break;
      }
      case GameSetting::Kind::kChoice: {
        int cur = 0;
        for (int k = 0; k < int(g.choices.size()); ++k) {
          if (v == g.choices[size_t(k)].first) cur = k;
        }
        if (ImGui::BeginCombo(g.label, g.choices.empty() ? "" : g.choices[size_t(cur)].second)) {
          for (int k = 0; k < int(g.choices.size()); ++k) {
            if (ImGui::Selectable(g.choices[size_t(k)].second, k == cur)) game_.Set(g, g.choices[size_t(k)].first);
          }
          ImGui::EndCombo();
        }
        break;
      }
      case GameSetting::Kind::kSlider: {
        int n = std::atoi(v.c_str());
        if (ImGui::SliderInt(g.label, &n, int(g.min), int(g.max), "%d%%")) game_.Set(g, std::to_string(n));
        break;
      }
    }
    ImGui::PopID();
  }
  ImGui::Separator();
  if (ImGui::Button("As they were")) {
    for (const GameSetting& g : SubtitleSettings()) {
      if (std::strcmp(g.key, "subtitles") != 0) game_.Set(g, g.fallback);
    }
  }
  ImGui::EndPopup();
}

// ---- the layout (kept in subtitles/work/editor.txt) --------------------------------

void SubtitleEditor::LoadLayout() {
  std::ifstream in(install_dir_ / "subtitles" / "work" / "editor.txt");
  std::string k;
  float v = 0;
  while (in >> k >> v) {
    if (k == "zoom") zoom_ = std::clamp(v, 0.5f, 3.0f);
    if (k == "list") list_frac_ = std::clamp(v, 0.12f, 0.6f);
    if (k == "preview") preview_frac_ = std::clamp(v, 0.08f, 0.75f);
  }
}

void SubtitleEditor::SaveLayout() {
  layout_dirty_ = false;
  std::error_code ec;
  fs::create_directories(install_dir_ / "subtitles" / "work", ec);
  std::ofstream out(install_dir_ / "subtitles" / "work" / "editor.txt");
  out << "zoom " << zoom_ << "\nlist " << list_frac_ << "\npreview " << preview_frac_ << "\n";
}

void SubtitleEditor::SetZoom(float z) {
  zoom_ = std::clamp(z, 0.5f, 3.0f);
  SaveLayout();
}

// The speakers a clip has room for: everyone who says a line or is named,
// and those added with + (at least one).
int SubtitleEditor::Speakers(const sf::Clip& c) const {
  int n = std::max(1, extra_speakers_clip_ == selected_ ? extra_speakers_ : 0);
  for (const auto& l : c.lines) n = std::max(n, std::clamp(l.speaker, 0, sf::kSpeakers - 1) + 1);
  for (int i = 0; i < sf::kSpeakers; ++i) {
    if (!c.names[i].empty()) n = std::max(n, i + 1);
  }
  return std::min(n, sf::kSpeakers);
}

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
  LoadLayout();
  game_.Load(install_dir_ / "amped3.toml");
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
    if (selected_ >= 0 && selected_ < int(clips_.size())) {
      Fetch(clips_[size_t(selected_)].source);
      if (clips_[size_t(selected_)].kind == sf::Kind::kVideo) FetchVideo(clips_[size_t(selected_)].source);
    }
  }
  UpdateVideo();
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
  // or body (4), or where two lines meet (5: both, the left's end and the
  // right's start together).
  int hit = 0, hit_line = -1;
  // Lines that meet (one ends where the next starts, or within a few
  // pixels): around the joint, the left part takes the left line's end,
  // the right part the right line's start, the middle both.
  auto joint_x = [&](int i) { return (to_x(c.lines[size_t(i)].end_ms) + to_x(c.lines[size_t(i) + 1].start_ms)) * 0.5f; };
  auto meets = [&](int i) {
    return i >= 0 && i + 1 < int(c.lines.size()) &&
           std::fabs(to_x(c.lines[size_t(i) + 1].start_ms) - to_x(c.lines[size_t(i)].end_ms)) <= edge * 2.0f;
  };
  int joint = -1, joint_side = 0;  // the joint the mouse is at: -1 left, 0 both, 1 right
  if (my >= lane_y && my < wave_y) {
    for (int i = 0; i + 1 < int(c.lines.size()); ++i) {
      if (!meets(i)) continue;
      const float xb = joint_x(i), dx = mx - xb;
      if (std::fabs(dx) > edge * 2.0f) continue;
      joint = i;
      joint_side = std::fabs(dx) <= edge * 0.6f ? 0 : dx < 0 ? -1 : 1;
      hit = joint_side == 0 ? 5 : joint_side < 0 ? 3 : 2;
      hit_line = joint_side > 0 ? i + 1 : i;
      break;
    }
  }
  if (my >= lane_y && my < wave_y && !hit) {
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
      drag_joint_side_ = joint >= 0 ? joint_side : 2;
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
          if (k == drag_line_ || (drag_ == 5 && k == drag_line_ + 1)) continue;
          consider(c.lines[size_t(k)].start_ms);
          consider(c.lines[size_t(k)].end_ms);
        }
        return best;
      };
      if (drag_ == 5 && drag_line_ + 1 < int(c.lines.size())) {
        // Where they meet, both together.
        sf::Line& r = c.lines[size_t(drag_line_) + 1];
        const uint32_t r0 = r.start_ms;
        const double t = std::clamp(snap(ms), double(l.start_ms) + 100.0, double(r.end_ms) - 100.0);
        l.end_ms = uint32_t(t);
        r.start_ms = uint32_t(t);
        if (r.start_ms != r0) changed = true;
      } else if (drag_ == 2) {
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
        if (drag_ != 4) ScrubTo(drag_ == 2 ? l.start_ms : drag_ == 5 ? l.end_ms : std::max(0.0, double(l.end_ms) - kGrainMs));
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
  if (hovered && !drag_ && (hit == 2 || hit == 3 || hit == 5)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (drag_ == 2 || drag_ == 3 || drag_ == 5) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  // (Which joint and side the marks below show: the one dragged, or under
  // the mouse.)
  int mark_joint = joint, mark_side = joint_side;
  if (drag_ >= 2 && drag_ <= 5 && drag_joint_side_ != 2) {
    mark_side = drag_joint_side_;
    mark_joint = drag_joint_side_ > 0 ? drag_line_ - 1 : drag_line_;
  } else if (drag_ || !hovered) {
    mark_joint = -1;
  }
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
    const ImU32 col = kSpeaker[std::clamp(l.speaker, 0, sf::kSpeakers - 1)];
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
                sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 210), Named(c, l).c_str());
    dl->PopClipRect();
  }
  // Where lines meet: a small mark on the joint; at the mouse (or while
  // dragged) which side moves: the left line's end, the right's start, or
  // both (arrows out of the joint, and the edge(s) lit).
  for (int i = 0; i + 1 < int(c.lines.size()); ++i) {
    if (!meets(i)) continue;
    const float xb = joint_x(i), ym = (lane_y + wave_y) * 0.5f;
    if (xb < p0.x - edge || xb > p0.x + w + edge) continue;
    const bool on = i == mark_joint;
    const float r = (on ? 5.0f : 3.5f) * unit;
    dl->AddQuadFilled(ImVec2(xb, ym - r), ImVec2(xb + r, ym), ImVec2(xb, ym + r), ImVec2(xb - r, ym),
                      on ? kGold : IM_COL32(255, 255, 255, 150));
    if (!on) continue;
    const float a = 7.0f * unit, top = lane_y + 3 * unit, bot = wave_y - 3 * unit;
    auto arrow = [&](float dir) {  // a triangle pointing out of the joint
      const float x = xb + dir * (r + 2 * unit);
      dl->AddTriangleFilled(ImVec2(x + dir * a, ym), ImVec2(x, ym - a * 0.7f), ImVec2(x, ym + a * 0.7f), kGold);
    };
    if (mark_side <= 0) {
      arrow(-1.0f);
      const float x1 = to_x(c.lines[size_t(i)].end_ms);
      dl->AddLine(ImVec2(x1, top), ImVec2(x1, bot), kGold, 3.0f * unit);
    }
    if (mark_side >= 0) {
      arrow(1.0f);
      const float x0 = to_x(c.lines[size_t(i) + 1].start_ms);
      dl->AddLine(ImVec2(x0, top), ImVec2(x0, bot), kGold, 3.0f * unit);
    }
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
                      WithAlpha(kSpeaker[std::clamp(l.speaker, 0, sf::kSpeakers - 1)], 90));
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

  // What's heard now, as the game shows it: over the movie's picture at the
  // playhead (a movie), or on its own.
  {
    ImGui::Spacing();
    const ImVec2 b0 = ImGui::GetCursorScreenPos();
    int vw = 0, vh = 0;
    const bool movie = c.kind == sf::Kind::kVideo && VideoFrame(&vw, &vh);
    // (A movie: as tall as dragged, the bar under it.)
    const float want_bh = std::max(px * 4.0f, preview_frac_ * ImGui::GetIO().DisplaySize.y);
    // (Room kept for a few of the lines under it.)
    const float room = ImGui::GetContentRegionAvail().y - px * 10.0f;
    const float bh = movie ? std::max(px * 4.0f, std::min({want_bh, w * float(vh) / float(vw), room})) : px * 3.6f;
    ImGui::Dummy(ImVec2(w, bh));
    dl->AddRectFilled(b0, ImVec2(b0.x + w, b0.y + bh), IM_COL32(0, 0, 0, 200), 6.0f * unit);
    float text_bottom = b0.y + bh;
    float text_w = w;
    float text_x0 = b0.x;
    if (movie) {
      const float iw = bh * float(vw) / float(vh);
      const ImVec2 i0(b0.x + (w - iw) * 0.5f, b0.y);
      dl->AddImage(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(video_tex_))), i0, ImVec2(i0.x + iw, b0.y + bh));
      text_bottom = b0.y + bh * 0.93f;
      // (Wrapped to the picture, or the box when the picture's narrow.)
      text_w = std::max(iw, std::min(w, bh * 16.0f / 9.0f));
      text_x0 = b0.x + (w - text_w) * 0.5f;
    } else if (c.kind == sf::Kind::kVideo && (video_loader_ && video_loader_->source == c.source)) {
      const char* t = "Getting the movie's pictures...";
      dl->AddText(font, px * 0.8f, ImVec2(b0.x + 8 * unit, b0.y + 4 * unit), kDim, t);
    }
    std::vector<const sf::Line*> now;
    for (const auto& l : c.lines) {
      if (playhead_ms_ >= l.start_ms && playhead_ms_ < l.end_ms) now.push_back(&l);
    }
    const float tpx = movie ? std::max(px * 0.9f, bh * 0.055f) : px * 1.25f;
    if (movie) {
      // Exactly as the game shows them (subtitle_draw.h, the game's own look
      // settings), over the game's 16:9 picture scaled to this one.
      const float pw = std::min(w, bh * 16.0f / 9.0f), ph = pw * 9.0f / 16.0f;
      const ImVec2 pic0(b0.x + (w - pw) * 0.5f, b0.y + (bh - ph) * 0.5f);
      std::vector<amped3::subtitle_draw::Line> show;
      for (size_t k = 0; k < now.size() && k < 3; ++k) {
        show.push_back({GameNames() ? Named(c, *now[k]) : now[k]->text,
                        GameColors() ? kSpeaker[now[k]->speaker] : IM_COL32(255, 255, 255, 255)});
      }
      dl->PushClipRect(b0, ImVec2(b0.x + w, b0.y + bh), true);
      amped3::subtitle_draw::Draw(dl, font, GameStyle(), pic0, pw, ph, show, 0.0f);
      dl->PopClipRect();
    } else if (now.empty()) {
      if (!movie) {
        const char* t = "(no line here)";
        const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0, t);
        dl->AddText(font, px, ImVec2(b0.x + (w - ts.x) * 0.5f, b0.y + (bh - ts.y) * 0.5f), kDim, t);
      }
    } else {
      // Wrapped into rows as the game does (each row centred, on its own
      // shading), the last at the bottom.
      struct Row {
        std::string text;
        ImU32 col;
      };
      std::vector<Row> rows;
      const float max_w = text_w * (movie ? 0.85f : 0.95f);
      for (size_t k = 0; k < now.size() && k < 3; ++k) {
        const std::string t = Named(c, *now[k]);
        const ImU32 col = kSpeaker[std::clamp(now[k]->speaker, 0, sf::kSpeakers - 1)];
        std::string row;
        size_t pos = 0;
        while (pos < t.size()) {
          size_t sp = t.find(' ', pos);
          if (sp == std::string::npos) sp = t.size();
          const std::string word = t.substr(pos, sp - pos);
          const std::string attempt = row.empty() ? word : row + " " + word;
          if (!row.empty() && font->CalcTextSizeA(tpx, FLT_MAX, 0, attempt.c_str()).x > max_w) {
            rows.push_back({row, col});
            row = word;
          } else {
            row = attempt;
          }
          pos = sp + 1;
        }
        if (!row.empty()) rows.push_back({row, col});
      }
      const float line_h = tpx * 1.22f;
      float y = movie ? text_bottom - line_h * float(rows.size()) : b0.y + (bh - line_h * float(rows.size())) * 0.5f;
      y = std::max(y, b0.y + 2 * unit);
      dl->PushClipRect(b0, ImVec2(b0.x + w, b0.y + bh), true);
      for (const Row& r : rows) {
        const ImVec2 ts = font->CalcTextSizeA(tpx, FLT_MAX, 0, r.text.c_str());
        const float x = text_x0 + (text_w - ts.x) * 0.5f;
        if (movie) {
          dl->AddRectFilled(ImVec2(x - tpx * 0.4f, y - tpx * 0.08f), ImVec2(x + ts.x + tpx * 0.4f, y + line_h - tpx * 0.08f),
                            IM_COL32(0, 0, 0, 150), tpx * 0.2f);
        }
        dl->AddText(font, tpx, ImVec2(x + 2, y + 2), IM_COL32(0, 0, 0, 255), r.text.c_str());
        dl->AddText(font, tpx, ImVec2(x, y), r.col, r.text.c_str());
        y += line_h;
      }
      dl->PopClipRect();
    }
    if (movie) {
      // The bar under the picture: drag to make it bigger or smaller.
      ImGui::InvisibleButton("##preview-split", ImVec2(w, 8.0f * unit));
      if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      if (ImGui::IsItemActive() && io.MouseDelta.y != 0) {
        preview_frac_ = std::clamp(bh / io.DisplaySize.y + io.MouseDelta.y / io.DisplaySize.y, 0.08f, 0.75f);
        layout_dirty_ = true;
      }
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      dl->AddLine(ImVec2(a.x + w * 0.45f, (a.y + b.y) * 0.5f), ImVec2(a.x + w * 0.55f, (a.y + b.y) * 0.5f),
                  ImGui::IsItemActive() ? kGold : kDim, 2.0f * unit);
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
  // (Sized with the display's scale and the zoom.)
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f * unit, 3.0f * unit));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * unit, 5.0f * unit));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 14.0f * unit);
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
  // The text's size (as well as the display's scale): Ctrl+= / Ctrl+- / Ctrl+0.
  if (ImGui::Button("A-")) SetZoom(zoom_ / 1.1f);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smaller (Ctrl+-)");
  ImGui::SameLine(0, 2 * unit);
  if (ImGui::Button("A+")) SetZoom(zoom_ * 1.1f);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bigger (Ctrl+=; Ctrl+0: as it was)");
  ImGui::SameLine();
  if (ImGui::Button("Back")) close = Back();
  if (!status_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImColor(kGold), "%s", status_.c_str());
  }

  // The clips (as wide as dragged: the bar between).
  const float list_w = std::clamp(list_frac_, 0.12f, 0.6f) * display.x;
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
                        (c.lines.empty() ? std::string("(no lines)") : Named(c, c.lines[0]));
    std::string src = c.source;
    if (src.size() > 60) src = "..." + src.substr(src.size() - 57);
    ImGui::PushID(i);
    visible_.push_back(i);
    if (ImGui::Selectable("##clip", selected_ == i, 0, ImVec2(0, px * 2.3f))) Select(i);
    const bool row_hot = ImGui::IsItemHovered() || selected_ == i;
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    if (selected_ == i && scroll_to_selected_) {
      ImGui::SetScrollHereY(0.4f);
      scroll_to_selected_ = false;
    }
    // Its first line: too long for the list, it scrolls by while picked or
    // under the mouse (to and fro).
    {
      ImDrawList* ldl = ImGui::GetWindowDrawList();
      const float x0 = r0.x + px * 0.4f, x1 = r1.x - px * 0.3f;
      const float tw = ImGui::CalcTextSize(label.c_str()).x;
      float shift = 0;
      if (tw > x1 - x0 && row_hot) {
        const float over = tw - (x1 - x0), speed = 45.0f * unit;
        const double period = 2.0 * (over / speed + 1.2);
        const double t = std::fmod(ImGui::GetTime(), period);
        const double half = period * 0.5;
        const double u = t < half ? t : period - t;  // there and back, a pause at each end
        shift = float(std::clamp((u - 0.6) * speed, 0.0, double(over)));
      }
      ldl->PushClipRect(ImVec2(x0, r0.y), ImVec2(x1, r1.y), true);
      ldl->AddText(ImVec2(x0 - shift, r0.y + px * 0.1f), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
      char sub[160];
      std::snprintf(sub, sizeof(sub), "%s   %s", src.c_str(), Clock(c.duration_ms, false).c_str());
      ldl->AddText(ImVec2(x0, r0.y + px * 1.2f), kDim, sub);
      ldl->PopClipRect();
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  // The bar between: drag to widen or narrow the list.
  ImGui::SameLine(0, 0);
  ImGui::InvisibleButton("##split", ImVec2(8.0f * unit, body_h));
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (ImGui::IsItemActive() && io.MouseDelta.x != 0) {
    list_frac_ = std::clamp(list_frac_ + io.MouseDelta.x / display.x, 0.12f, 0.6f);
    layout_dirty_ = true;
  }
  {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2((a.x + b.x) * 0.5f, a.y + body_h * 0.45f),
                                        ImVec2((a.x + b.x) * 0.5f, a.y + body_h * 0.55f),
                                        ImGui::IsItemActive() ? kGold : kDim, 2.0f * unit);
  }
  if (layout_dirty_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) SaveLayout();
  ImGui::SameLine(0, 0);

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
      ImGui::SameLine();
      if (ImGui::Button("Look...")) ImGui::OpenPopup("##look");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the subtitles look in game (the preview shows it)");
      DrawLook(unit);
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
    changed |= DrawNames(px, unit);
    changed |= DrawTimeline(font, px, unit);
    sf::Clip& c = clips_[size_t(selected_)];

    // The lines.
    ImGui::BeginChild("##lines", ImVec2(0, 0));
    int remove = -1, insert_after = -2;
    bool at_playhead = false;
    // The speaker buttons all as wide as the widest (the columns line up).
    float speaker_w = ImGui::CalcTextSize(" 10 ").x;
    for (int k = 0; k < sf::kSpeakers; ++k) {
      if (!c.names[k].empty()) {
        char t[48];
        std::snprintf(t, sizeof(t), " %d %.24s ", k + 1, c.names[k].c_str());
        speaker_w = std::max(speaker_w, ImGui::CalcTextSize(t).x);
      }
    }
    speaker_w += ImGui::GetStyle().FramePadding.x * 2;
    {
      // The columns' heads, over them.
      const ImGuiStyle& st = ImGui::GetStyle();
      const float x0 = ImGui::GetCursorPosX();
      const float arrow_w = ImGui::CalcTextSize(">").x + st.FramePadding.x * 2 + st.ItemSpacing.x;
      const float cols[4] = {x0 + arrow_w, x0 + arrow_w + speaker_w + st.ItemSpacing.x,
                             x0 + arrow_w + speaker_w + st.ItemSpacing.x * 2 + 80 * unit,
                             x0 + arrow_w + speaker_w + st.ItemSpacing.x * 3 + 160 * unit};
      const char* heads[4] = {"Speaker", "Start s", "End s", "Words"};
      for (int k = 0; k < 4; ++k) {
        if (k) ImGui::SameLine();
        ImGui::SetCursorPosX(cols[k] + st.FramePadding.x);
        ImGui::TextColored(ImColor(kDim), "%s", heads[k]);
      }
    }
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
      const int sp = std::clamp(l.speaker, 0, sf::kSpeakers - 1);
      ImGui::PushStyleColor(ImGuiCol_Button, kSpeaker[sp]);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSpeaker[sp]);
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
      char who[48];
      if (c.names[sp].empty()) {
        std::snprintf(who, sizeof(who), " %d ", sp + 1);
      } else {
        std::snprintf(who, sizeof(who), " %d %.24s ", sp + 1, c.names[sp].c_str());
      }
      // (Through the clip's speakers and one more.)
      const int slots = std::min(sf::kSpeakers, Speakers(c) + 1);
      if (ImGui::Button(who, ImVec2(speaker_w, 0))) {
        Remember();
        l.speaker = (sp + 1) % slots;
        changed = true;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Speaker: click for the next, right-click for the one before (keys 1 to 9, 0 for 10)");
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        Remember();
        l.speaker = (sp + slots - 1) % slots;
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
      // Wrapped to the box (as tall as its words need); Enter is done.
      const float box_w = ImGui::CalcItemWidth();
      const ImVec2 fp = ImGui::GetStyle().FramePadding;
      const float text_h = ImGui::CalcTextSize(buf, nullptr, false, std::max(10.0f, box_w - fp.x * 2 - 2 * unit)).y;
      if (ImGui::InputTextMultiline("##text", buf, sizeof(buf),
                                    ImVec2(box_w, std::max(ImGui::GetFrameHeight(), text_h + fp.y * 2 + 1)),
                                    ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CtrlEnterForNewLine |
                                        ImGuiInputTextFlags_NoHorizontalScroll)) {
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
        "Speakers: 1 white, 2 yellow, 3 pink, 4 green, then sky blue, orange, lavender, coral, mint and "
        "sand (click for the next, right-click for the one before, or the keys 1 to 9 and 0); + adds "
        "one, up to ten; type their names in above the timeline (or pick one used before) "
        "to show them before their lines. Movies: the picture at the playhead, under the timeline. "
        "Page Up / Down: the previous / next clip. Ctrl+Z / Ctrl+Y undo and redo, Ctrl+S saves.");
    ImGui::PopStyleColor();
    ImGui::EndChild();
  } else {
    ImGui::TextColored(ImColor(kDim), "Choose a clip on the left.");
  }
  ImGui::EndChild();

  ImGui::PopFont();
  ImGui::End();
  ImGui::PopStyleVar(5);
  ImGui::PopStyleColor(6);

  // Keys (not while typing).
  if (dirty_ && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save();
  if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false))) {
    SetZoom(zoom_ * 1.1f);
  }
  if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false))) {
    SetZoom(zoom_ / 1.1f);
  }
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0, false)) SetZoom(1.0f);
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
    const ImGuiKey digits[sf::kSpeakers] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5,
                                            ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9, ImGuiKey_0};
    for (int k = 0; k < sf::kSpeakers && have_line; ++k) {
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
