// amped3 - subtitles for the game's dialogue (see subtitles.h).

#include "subtitles.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/audio/audio_system.h>
#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "graphics_panel.h"
#include "subtitle_file.h"

REXCVAR_DEFINE_INT32(subtitles, 1, "Amped 3/Subtitles",
                     "Subtitles for the game's dialogue, made at install on this PC "
                     "(subtitles\\amped3.subtitles): 0 off, 1 story cutscenes and movies, "
                     "2 all speech (riders and announcers while riding too)")
    .range(0, 2);
REXCVAR_DEFINE_INT32(subtitles_size, 100, "Amped 3/Subtitles",
                     "Subtitle text size, in percent of the standard size")
    .range(50, 200);
REXCVAR_DEFINE_BOOL(trace_subtitles, false, "Amped 3/Diagnostics",
                    "Log which sounds the subtitles recognise as they start, and the ones "
                    "they don't (to the log file).");

namespace amped3::subtitles {
namespace {

namespace sf = amped3::subtitle_file;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kContexts = 320;
constexpr uint32_t kPacket = 2048;

// ---- the file ------------------------------------------------------------------

std::vector<sf::Clip> g_clips;
// (length, hash) -> clip index.
std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, int>>> g_index;
std::unordered_map<std::string, int> g_movies;  // movie file name (lower case, no extension) -> clip
std::atomic<int> g_clip_count{0};

uint64_t IndexKey(uint32_t length, uint64_t hash) { return hash ^ (uint64_t(length) * 0x9E3779B97F4A7C15ull); }

bool Load(const std::filesystem::path& file) {
  std::ifstream f(file, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  std::string error;
  std::vector<sf::Clip> clips;
  if (!sf::Parse(ss.str(), &clips, &error)) {
    REXLOG_WARN("Subtitles: {} isn't usable: {}", file.string(), error);
    return false;
  }
  g_clips = std::move(clips);
  g_index.clear();
  g_movies.clear();
  size_t lines = 0;
  for (size_t i = 0; i < g_clips.size(); ++i) {
    lines += g_clips[i].lines.size();
    if (!g_clips[i].file.empty()) g_movies[g_clips[i].file] = int(i);
    for (const sf::Key& k : g_clips[i].keys) {
      g_index[IndexKey(k.length, k.hash)].push_back({k.length, int(i)});
    }
  }
  g_clip_count = int(g_clips.size());
  REXLOG_INFO("Subtitles: {} sounds with {} lines ({})", g_clips.size(), lines, file.string());
  return true;
}

int Find(uint32_t length, uint64_t hash) {
  auto it = g_index.find(IndexKey(length, hash));
  if (it == g_index.end()) return -1;
  for (const auto& [len, clip] : it->second) {
    if (len == length) return clip;
  }
  return -1;
}

// ---- shown lines (monitor thread -> UI thread) ---------------------------------

std::mutex g_shown_mutex;
std::vector<std::string> g_shown;

// ---- the overlay ---------------------------------------------------------------

class Overlay final : public rex::ui::ImGuiDialog {
 public:
  explicit Overlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::vector<std::string> lines;
    {
      std::lock_guard lock(g_shown_mutex);
      lines = g_shown;
    }
    if (lines.empty()) return;
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (w < 64 || h < 64) return;
    // The game's 16:9 picture (letterboxed in other window shapes).
    const float pic_w = std::min(w, h * 16.0f / 9.0f);
    const float pic_h = std::min(h, w * 9.0f / 16.0f);
    const float px = std::max(12.0f, pic_h * 0.042f * float(REXCVAR_GET(subtitles_size)) / 100.0f);
    ImFont* font = amped3::UiFont(px);
    const float max_w = pic_w * 0.78f;

    // Wrapped rows, the newest line at the bottom.
    std::vector<std::string> rows;
    for (const std::string& line : lines) {
      std::string row;
      size_t pos = 0;
      while (pos < line.size()) {
        size_t sp = line.find(' ', pos);
        if (sp == std::string::npos) sp = line.size();
        const std::string word = line.substr(pos, sp - pos);
        const std::string attempt = row.empty() ? word : row + " " + word;
        if (!row.empty() && font->CalcTextSizeA(px, FLT_MAX, 0, attempt.c_str()).x > max_w) {
          rows.push_back(row);
          row = word;
        } else {
          row = attempt;
        }
        pos = sp + 1;
      }
      if (!row.empty()) rows.push_back(row);
    }
    if (rows.empty()) return;
    const float line_h = px * 1.22f;
    const float pad_x = px * 0.45f, pad_y = px * 0.18f;
    float y = (h + pic_h) * 0.5f - pic_h * 0.075f - line_h * float(rows.size());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (const std::string& row : rows) {
      const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0, row.c_str());
      const float x = (w - size.x) * 0.5f;
      dl->AddRectFilled(ImVec2(x - pad_x, y - pad_y), ImVec2(x + size.x + pad_x, y + line_h - pad_y),
                        IM_COL32(0, 0, 0, 150), px * 0.2f);
      const float o = std::max(1.0f, px / 22.0f);
      dl->AddText(font, px, ImVec2(x + o, y + o), IM_COL32(0, 0, 0, 200), row.c_str());
      dl->AddText(font, px, ImVec2(x, y), IM_COL32(255, 255, 255, 255), row.c_str());
      y += line_h;
    }
  }
};

rex::ui::ImGuiDrawer* g_drawer = nullptr;
std::unique_ptr<Overlay> g_overlay;  // UI thread
std::function<void(std::function<void()>)> g_post_ui;

// ---- watching the decoder ------------------------------------------------------

struct Track {
  int clip = -1;
  double elapsed = 0;   // seconds into the clip
  double last_change = 0;
  bool whole = false;   // the whole sound in buffer 0 (not streamed)
};

struct Ctx {
  bool initialized = false;
  uint32_t words[10] = {};
  uint32_t in0 = 0, in0_count = 0;
  Track track;
};

std::thread g_thread;
std::atomic<bool> g_running{false};

const uint8_t* Readable(const uint8_t* host, size_t size) {
  if (!host) return nullptr;
  size_t length = size;
  rex::memory::PageAccess access;
  if (!rex::memory::QueryProtect(const_cast<uint8_t*>(host), length, access)) return nullptr;
  if (access == rex::memory::PageAccess::kNoAccess || length < size) return nullptr;
  return host;
}

uint32_t U32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// The clip whose data the buffer at `host` (`bytes` long) starts with, or -1.
int Identify(const uint8_t* host, uint32_t bytes) {
  if (!host || bytes < kPacket) return -1;
  const uint32_t n_max = std::min(bytes, sf::kMaxKeyLength);
  if (!Readable(host, n_max)) return -1;
  // Longest key first (the most certain).
  for (uint32_t n : {16384u, 8192u, 4096u, 2048u}) {
    if (n > bytes) continue;
    const int c = Find(n, sf::Fnv1a64(host, n));
    if (c >= 0) return c;
  }
  if (bytes < sf::kMaxKeyLength) return Find(bytes, sf::Fnv1a64(host, bytes));
  return -1;
}

void Monitor(rex::Runtime* runtime) {
  std::array<Ctx, kContexts> ctxs{};
  const auto t0 = Clock::now();
  double prev = 0;
  bool overlay_wanted = false;
  double last_shown = -10;
  double movie_poll = -1, movie_start = 0;
  int movie_clip = -1;
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const double now = std::chrono::duration<double>(Clock::now() - t0).count();
    const double dt = std::min(0.1, now - prev);
    prev = now;
    const int mode = REXCVAR_GET(subtitles);
    std::vector<std::string> shown;
    auto* audio = static_cast<rex::audio::AudioSystem*>(runtime->audio_system());
    auto* decoder = audio ? audio->xma_decoder() : nullptr;
    auto* memory = runtime->memory();
    if (mode > 0 && decoder && memory && decoder->context_array_ptr()) {
      const uint8_t* base = memory->TranslatePhysical<uint8_t*>(decoder->context_array_ptr());
      for (uint32_t i = 0; i < kContexts; ++i) {
        const uint8_t* raw = base + 64 * i;
        Ctx& c = ctxs[i];
        uint32_t w[10];
        for (int k = 0; k < 10; ++k) w[k] = U32(raw + 4 * k);
        const bool initialized = w[7] != 0;  // output buffer pointer
        if (!initialized) {
          c = Ctx{};
          continue;
        }
        const rex::audio::XMA_CONTEXT_DATA d(raw);
        const bool changed = !c.initialized || std::memcmp(w, c.words, sizeof(w)) != 0;
        std::memcpy(c.words, w, sizeof(w));
        const bool fresh = !c.initialized;
        c.initialized = true;
        const uint32_t in0 = d.input_buffer_0_ptr;
        const uint32_t in0_count = d.input_buffer_0_packet_count;
        if (in0 && (fresh || in0 != c.in0 || in0_count != c.in0_count)) {
          c.in0 = in0;
          c.in0_count = in0_count;
          const uint8_t* host = memory->TranslatePhysical<uint8_t*>(in0);
          const int clip = Identify(host, in0_count * kPacket);
          if (clip >= 0) {
            const sf::Clip& cl = g_clips[size_t(clip)];
            if (c.track.clip != clip || c.track.elapsed > 1.0) {
              c.track = Track{clip, 0.0, now, uint64_t(in0_count) * kPacket >= cl.bytes};
              // Already into it (seen late): from the read position.
              if (!d.current_buffer && cl.bytes) {
                const double frac = double(d.input_buffer_read_offset) / (double(cl.bytes) * 8.0);
                if (frac > 0.02 && frac < 1.0) c.track.elapsed = frac * cl.duration_ms / 1000.0;
              }
              if (REXCVAR_GET(trace_subtitles)) {
                REXLOG_INFO("Subtitles: ctx {} plays {} ({} lines, {:.1f} s)", i, cl.source, cl.lines.size(),
                            cl.duration_ms / 1000.0);
              }
            }
          } else {
            // A sound held whole that's followed by other data: it's over
            // (streamed sounds move on to new data all the time).
            if (c.track.whole) c.track = Track{};
          }
          if (clip < 0 && REXCVAR_GET(trace_subtitles) && in0_count) {
            const uint8_t* p = Readable(host, 16);
            REXLOG_INFO("Subtitles: ctx {} new data not recognised ({:08X} x{} packets, {} Hz {}, starts {})", i,
                        in0, in0_count, d.sample_rate, d.is_stereo ? "stereo" : "mono",
                        p ? fmt::format("{:02X}{:02X}{:02X}{:02X}", p[0], p[1], p[2], p[3]) : "?");
          }
        }
        Track& t = c.track;
        if (t.clip < 0) continue;
        const sf::Clip& cl = g_clips[size_t(t.clip)];
        // Where it is: the read position while the whole sound sits in
        // buffer 0, else the time it has spent decoding.
        if (changed) t.last_change = now;
        if (!d.current_buffer && uint64_t(in0_count) * kPacket >= cl.bytes && cl.bytes) {
          const double frac = double(d.input_buffer_read_offset) / (double(cl.bytes) * 8.0);
          t.elapsed = std::clamp(frac, 0.0, 1.0) * cl.duration_ms / 1000.0;
        } else if (now - t.last_change < 0.3) {
          t.elapsed += dt;
        }
        if (t.elapsed * 1000.0 > cl.duration_ms + 1500.0) {
          t = Track{};
          continue;
        }
        if (mode == 1 && cl.kind == sf::Kind::kSpeech) continue;
        const uint32_t ms = uint32_t(t.elapsed * 1000.0);
        for (const sf::Line& l : cl.lines) {
          if (ms >= l.start_ms && ms < l.end_ms) shown.push_back(l.text);
        }
      }
    }
    // Movies: the one whose file is open, from when it was opened (Bink
    // reads its movie as it plays and closes it at the end).
    if (mode > 0 && !g_movies.empty() && runtime->kernel_state() && now - movie_poll >= 0.1) {
      movie_poll = now;
      int open_clip = -1;
      for (auto& f : runtime->kernel_state()->object_table()->GetObjectsByType<rex::system::XFile>()) {
        std::string name = f->name();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".bik") != 0) continue;
        auto it = g_movies.find(name.substr(0, name.size() - 4));
        if (it != g_movies.end()) open_clip = it->second;
      }
      if (open_clip != movie_clip) {
        movie_clip = open_clip;
        movie_start = now;
        if (open_clip >= 0 && REXCVAR_GET(trace_subtitles)) {
          REXLOG_INFO("Subtitles: movie {} ({} lines)", g_clips[size_t(open_clip)].source,
                      g_clips[size_t(open_clip)].lines.size());
        }
      }
    }
    if (mode > 0 && movie_clip >= 0) {
      const uint32_t ms = uint32_t((now - movie_start) * 1000.0);
      for (const sf::Line& l : g_clips[size_t(movie_clip)].lines) {
        if (ms >= l.start_ms && ms < l.end_ms) shown.push_back(l.text);
      }
    }
    if (shown.size() > 3) shown.erase(shown.begin(), shown.end() - 3);
    {
      std::lock_guard lock(g_shown_mutex);
      g_shown = shown;
    }
    // The overlay exists only while there's something to show (and a little
    // after, so it isn't remade between lines).
    if (!shown.empty()) last_shown = now;
    const bool want = now - last_shown < 3.0;
    if (want != overlay_wanted && g_post_ui) {
      overlay_wanted = want;
      g_post_ui([want] {
        if (want && !g_overlay && g_drawer) {
          g_overlay = std::make_unique<Overlay>(g_drawer);
        } else if (!want) {
          g_overlay.reset();
        }
      });
    }
  }
}

}  // namespace

void Start(rex::Runtime* runtime, rex::ui::ImGuiDrawer* drawer, std::filesystem::path folder,
           std::function<void(std::function<void()>)> post_ui) {
  if (!runtime || g_running.load()) return;
  const auto file = folder / sf::kFileName;
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    REXLOG_INFO("Subtitles: none made ({} missing; the installer's Subtitles step makes it)", file.string());
    return;
  }
  if (!Load(file) || g_clips.empty()) return;
  g_drawer = drawer;
  g_post_ui = std::move(post_ui);
  g_running = true;
  g_thread = std::thread(Monitor, runtime);
}

void Stop() {
  if (g_running.exchange(false) && g_thread.joinable()) g_thread.join();
  g_overlay.reset();
  g_post_ui = nullptr;
  g_drawer = nullptr;
}

int ClipCount() { return g_clip_count.load(); }

}  // namespace amped3::subtitles
