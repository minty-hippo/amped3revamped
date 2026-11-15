// amped3 - the Night Pass (night_pass.h).

#include "night_pass.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "challenges.h"
#include "notice.h"

REXCVAR_DEFINE_BOOL(native_tod_night_pass_lock, true, "GPU",
                    "Time of day: Northstar keeps its own hour (the game's) until the Night "
                    "Pass is earned; off: the day and night run there from the start");
REXCVAR_DECLARE(bool, trace_challenges);
REXCVAR_DECLARE(bool, native_time_of_day);

namespace amped3::night_pass {
namespace {

constexpr const char* kMountain = "northstar";

std::mutex g_mutex;
std::filesystem::path g_file;
std::atomic<bool> g_earned{false};
std::atomic<bool> g_at_mountain{false};
std::string g_level;  // the last level seen
std::chrono::steady_clock::time_point g_next_read{};
bool g_logged = false;     // this load's challenges logged
bool g_told = false;       // this load's notice shown

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

void Remember() {
  if (g_file.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(g_file.parent_path(), ec);
  std::ofstream f(g_file);
  if (f) f << "earned\n";
}

// What the registry says (true: earned).
bool EarnedNow(const std::vector<challenges::Info>& list) {
  for (const auto& c : list) {
    const std::string n = Lower(c.name);
    if (n.find("night") == std::string::npos) continue;
    // The pass itself (a challenge or reward named for it): done.
    const bool pass = n.find("nightpass") != std::string::npos ||
                      n.find("night pass") != std::string::npos ||
                      n.find("night_pass") != std::string::npos;
    if (pass && c.state >= 11) return true;
    // Northstar's night riding: open (idle or further), not locked.
    if (!pass && c.level == kMountain && c.state >= 2) return true;
  }
  return false;
}

}  // namespace

void Install(const std::filesystem::path& folder) {
  std::lock_guard lock(g_mutex);
  g_file = folder / "night_pass.txt";
  std::error_code ec;
  g_earned = std::filesystem::exists(g_file, ec);
  if (g_earned.load()) REXLOG_INFO("Night Pass: earned (remembered)");
}

void Tick() {
  std::lock_guard lock(g_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (now < g_next_read) return;
  g_next_read = now + std::chrono::milliseconds(500);
  const std::string level = challenges::LoadedLevel();
  if (level != g_level) {
    g_level = level;
    g_logged = false;
    g_told = false;
  }
  g_at_mountain = level == kMountain;
  if (!g_at_mountain.load()) return;
  // (Nothing more to learn this load: earned, and logged if asked.)
  if (g_earned.load() && (g_logged || !REXCVAR_GET(trace_challenges))) return;
  const std::vector<challenges::Info> list = challenges::List();
  // (The initiators come with the mountain's entities: wait for them.)
  const bool ready = std::any_of(list.begin(), list.end(),
                                 [](const auto& c) { return c.level == kMountain && c.state >= 0; });
  if (!ready) return;
  if (!g_logged && REXCVAR_GET(trace_challenges)) {
    g_logged = true;
    for (const auto& c : list) {
      if (c.level == kMountain || Lower(c.name).find("night") != std::string::npos) {
        REXLOG_INFO("Night Pass: challenge '{}' ({}) state {}", c.name, c.level, c.state);
      }
    }
  }
  if (!g_earned.load() && EarnedNow(list)) {
    g_earned = true;
    Remember();
    REXLOG_INFO("Night Pass: earned");
    notice::Show("Night Pass earned: day and night now run at Northstar");
  }
  if (!g_told && Locked() && REXCVAR_GET(native_time_of_day)) {
    g_told = true;
    notice::Show("Time of day: Northstar keeps its own hour until you earn the Night Pass");
  }
}

bool Earned() { return g_earned.load(); }

bool Locked() {
  return REXCVAR_GET(native_tod_night_pass_lock) && g_at_mountain.load() && !g_earned.load();
}

}  // namespace amped3::night_pass
