// amped3-launcher - the subtitle editor (see subtitle_editor.h).

#include "subtitle_editor.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
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

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const char* KindLabel(sf::Kind k) {
  return k == sf::Kind::kStory ? "story" : k == sf::Kind::kVideo ? "movie" : "speech";
}

}  // namespace

struct SubtitleEditor::Player {
  std::mutex m;
  int state = 0;  // 0 idle, 1 making the WAV, 2 ready to play, 3 failed
  fs::path wav;
  uint32_t from_ms = 0;
  std::string error;
  uint64_t started_ticks = 0;  // SDL ticks when playing began
  uint32_t started_ms = 0;
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
  selected_ = clips_.empty() ? -1 : 0;
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

bool SubtitleEditor::Playing() const { return stream_ != nullptr || (player_ && player_->state == 1); }

void SubtitleEditor::StopAudio() {
  if (stream_) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
  player_.reset();
}

// The sound as a WAV (made once, by the subtitle maker, from the game's
// files), then played from `from_ms`.
void SubtitleEditor::Play(const std::string& source, uint32_t from_ms) {
  StopAudio();
  auto p = std::make_shared<Player>();
  player_ = p;
  p->from_ms = from_ms;
  const fs::path dir = install_dir_ / "subtitles" / "work" / "play";
  std::error_code ec;
  fs::create_directories(dir, ec);
  uint64_t h = sf::Fnv1a64(reinterpret_cast<const uint8_t*>(source.data()), source.size());
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.wav", static_cast<unsigned long long>(h));
  p->wav = dir / name;
  if (fs::exists(p->wav, ec)) {
    p->state = 2;
    return;
  }
  const fs::path tool = ToolPath();
  if (tool.empty()) {
    p->state = 3;
    p->error = "The subtitle maker isn't built (Update first).";
    return;
  }
  p->state = 1;
  const std::vector<std::string> argv = {PathText(tool), "--game", PathText(install_dir_ / "game"), "--extract",
                                         source, "--wav", PathText(p->wav)};
  std::thread([p, argv] {
    std::string output, error;
    const int code = RunCapture(argv, &output, &error);
    std::lock_guard lock(p->m);
    std::error_code e2;
    if (code == 0 && fs::exists(p->wav, e2)) {
      p->state = 2;
    } else {
      p->state = 3;
      p->error = "Couldn't get the sound (" + (error.empty() ? output.substr(0, 200) : error) + ").";
    }
  }).detach();
}

bool SubtitleEditor::Draw(ImFont* font, float unit) {
  // Playing: start once the WAV is there.
  if (player_) {
    std::lock_guard lock(player_->m);
    if (player_->state == 2 && !stream_) {
      if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
      SDL_AudioSpec spec;
      Uint8* buf = nullptr;
      Uint32 len = 0;
      if (SDL_LoadWAV(PathText(player_->wav).c_str(), &spec, &buf, &len)) {
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (stream_) {
          const Uint32 frame = Uint32(SDL_AUDIO_FRAMESIZE(spec));
          Uint32 at = Uint32(uint64_t(player_->from_ms) * spec.freq / 1000) * frame;
          at = std::min(at, len);
          SDL_PutAudioStreamData(stream_, buf + at, int(len - at));
          SDL_FlushAudioStream(stream_);
          SDL_ResumeAudioStreamDevice(stream_);
          player_->started_ticks = SDL_GetTicks();
          player_->started_ms = player_->from_ms;
        } else {
          status_ = std::string("No sound output: ") + SDL_GetError();
        }
        SDL_free(buf);
      } else {
        status_ = std::string("Couldn't read the sound: ") + SDL_GetError();
      }
      player_->state = 4;  // playing (or given up)
    } else if (player_->state == 3) {
      status_ = player_->error;
      player_->state = 0;
    }
  }
  if (stream_ && SDL_GetAudioStreamQueued(stream_) == 0) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
  // Where the sound is (for the line being heard).
  int64_t play_ms = -1;
  if (stream_ && player_) {
    play_ms = int64_t(player_->started_ms) + int64_t(SDL_GetTicks() - player_->started_ticks);
  }

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
  if (ImGui::Button("Back")) close = Back();
  if (!status_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImColor(kGold), "%s", status_.c_str());
  }

  // The clips.
  const float list_w = display.x * 0.34f;
  const float body_h = display.y - ImGui::GetCursorPosY() - pad;
  ImGui::BeginChild("##clips", ImVec2(list_w, body_h), ImGuiChildFlags_Borders);
  const std::string needle = Lower(search_);
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
    if (ImGui::Selectable("##clip", selected_ == i, 0, ImVec2(0, px * 2.3f))) selected_ = i;
    ImGui::SameLine(px * 0.4f);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label.c_str());
    ImGui::TextColored(ImColor(kDim), "%s", src.c_str());
    ImGui::EndGroup();
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::SameLine();

  // The selected clip's lines.
  ImGui::BeginChild("##lines", ImVec2(0, body_h), ImGuiChildFlags_Borders);
  if (selected_ >= 0 && selected_ < int(clips_.size())) {
    sf::Clip& c = clips_[size_t(selected_)];
    ImGui::TextColored(ImColor(kDim), "%s   %s, %.1f s", c.source.c_str(), KindLabel(c.kind), c.duration_ms / 1000.0);
    if (ImGui::Button("Play")) Play(c.source, 0);
    ImGui::SameLine();
    if (ImGui::Button("Stop")) StopAudio();
    ImGui::SameLine();
    if (player_ && player_->state == 1) {
      ImGui::TextColored(ImColor(kDim), "getting the sound...");
      ImGui::SameLine();
    }
    bool edited = c.edited;
    if (ImGui::Checkbox("Corrected by hand (kept when the subtitles are made again)", &edited)) {
      c.edited = edited;
      dirty_ = true;
    }
    ImGui::Separator();
    ImGui::TextColored(ImColor(kDim), "Speaker (click to change)   Start s   End s   Words");
    bool changed = false;
    int remove = -1, insert_after = -2;
    for (int i = 0; i < int(c.lines.size()); ++i) {
      sf::Line& l = c.lines[size_t(i)];
      ImGui::PushID(i);
      const bool hearing = play_ms >= 0 && play_ms >= int64_t(l.start_ms) && play_ms < int64_t(l.end_ms);
      if (ImGui::Button(">")) Play(c.source, l.start_ms > 300 ? l.start_ms - 300 : 0);
      ImGui::SameLine();
      // The speaker: a swatch of their colour.
      const int sp = std::clamp(l.speaker, 0, 3);
      ImGui::PushStyleColor(ImGuiCol_Button, kSpeaker[sp]);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSpeaker[sp]);
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
      char who[8];
      std::snprintf(who, sizeof(who), " %d ", sp + 1);
      if (ImGui::Button(who)) {
        l.speaker = (sp + 1) % sf::kSpeakers;
        changed = true;
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        l.speaker = (sp + sf::kSpeakers - 1) % sf::kSpeakers;
        changed = true;
      }
      ImGui::PopStyleColor(3);
      ImGui::SameLine();
      float start = l.start_ms / 1000.0f, end = l.end_ms / 1000.0f;
      ImGui::SetNextItemWidth(80 * unit);
      if (ImGui::InputFloat("##start", &start, 0, 0, "%.2f")) {
        l.start_ms = uint32_t(std::max(0.0f, start) * 1000.0f + 0.5f);
        changed = true;
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(80 * unit);
      if (ImGui::InputFloat("##end", &end, 0, 0, "%.2f")) {
        l.end_ms = uint32_t(std::max(0.0f, end) * 1000.0f + 0.5f);
        changed = true;
      }
      ImGui::SameLine();
      char buf[1024];
      std::snprintf(buf, sizeof(buf), "%s", l.text.c_str());
      ImGui::SetNextItemWidth(-px * 4.2f);
      if (hearing) ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 204, 0, 70));
      ImGui::PushStyleColor(ImGuiCol_Text, kSpeaker[sp]);
      if (ImGui::InputText("##text", buf, sizeof(buf))) {
        l.text = sf::OneLine(buf);
        changed = true;
      }
      ImGui::PopStyleColor(hearing ? 2 : 1);
      ImGui::SameLine();
      if (ImGui::Button("+")) insert_after = i;
      ImGui::SameLine();
      if (ImGui::Button("x")) remove = i;
      ImGui::PopID();
    }
    if (ImGui::Button("+ Add a line")) insert_after = int(c.lines.size()) - 1;
    if (remove >= 0) {
      c.lines.erase(c.lines.begin() + remove);
      changed = true;
    }
    if (insert_after >= -1) {
      sf::Line l;
      const size_t at = size_t(insert_after + 1);
      l.start_ms = insert_after >= 0 ? c.lines[size_t(insert_after)].end_ms : 0;
      l.end_ms = l.start_ms + 2000;
      l.speaker = insert_after >= 0 ? c.lines[size_t(insert_after)].speaker : 0;
      l.text = "";
      c.lines.insert(c.lines.begin() + std::ptrdiff_t(at), l);
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
        "Speakers are numbered in the order they first speak in the clip: 1 white, 2 yellow, 3 pink, "
        "4 green (click a number for the next, right-click for the one before). Times are seconds from "
        "the start of the sound; > plays from that line. Ctrl+S saves.");
    ImGui::PopStyleColor();
  } else {
    ImGui::TextColored(ImColor(kDim), "Choose a clip on the left.");
  }
  ImGui::EndChild();

  ImGui::PopFont();
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(6);
  // Ctrl+S saves.
  if (dirty_ && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save();
  return close;
}

}  // namespace launcher
