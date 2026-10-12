// amped3 - challenge launcher. See challenges.h.

#include "challenges.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(trace_challenges, true, "Amped 3/Diagnostics",
                    "Log the Challenges tab's requests and what the challenge's initiator does "
                    "after them (to the log file).");

REX_EXTERN(__imp__sub_82172C78);
REX_EXTERN(__imp__sub_824574B0);
REX_EXTERN(__imp__sub_82203470);
REX_EXTERN(__imp__sub_824939E8);

namespace amped3::challenges {
namespace {

constexpr uint32_t kStateMachine = 0x828901E0;  // the game state machine
constexpr uint32_t kManagerPtr = 0x8288F0A8;    // the challenge manager
constexpr int kStateRiding = 13;

// The initiator's states, in the order the game registers their classes.
constexpr const char* kInitiatorStates[] = {
    "init",    "locked",  "idle",      "activating", "help",       "active",       "results",
    "rewards", "saving",  "aborting",  "try again",  "done",       "done (improve)"};
constexpr uint32_t kStateActivate = 3;
constexpr int kStateActive = 5;
constexpr uint32_t kStateAborting = 9;
// A challenge under way: activating .. try again. The game runs one at a time,
// and a second one activated meanwhile waits forever.
bool Busy(int s) { return s >= 3 && s <= 10; }

const char* InitiatorStateName(int s) {
  return s >= 0 && s < int(std::size(kInitiatorStates)) ? kInitiatorStates[s] : "?";
}

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

uint8_t* GuestBase() {
  auto* memory = GuestMemory();
  return memory ? memory->virtual_membase() : nullptr;
}

bool Readable(uint32_t address, uint32_t size) {
  auto* memory = GuestMemory();
  if (!memory || address < 0x10000 || address + size < address) return false;
  auto* heap = memory->LookupHeap(address);
  if (!heap) return false;
  return heap->QueryRangeAccess(address, address + size - 1) != rex::memory::PageAccess::kNoAccess;
}

uint32_t Load32(uint8_t* base, uint32_t address) {
  if (!Readable(address, 4)) return 0;
  return rex::memory::load_and_swap<uint32_t>(base + address);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  if (Readable(address, 4)) rex::memory::store_and_swap<uint32_t>(base + address, value);
}

std::string GuestText(uint8_t* base, uint32_t address, size_t max = 96) {
  if (!Readable(address, 1)) return {};
  std::string s;
  for (size_t i = 0; i < max; ++i) {
    if (!Readable(address + uint32_t(i), 1)) break;
    const char c = char(base[address + i]);
    if (!c) break;
    if (c < 32 || c > 126) return {};
    s += c;
  }
  return s;
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

bool Tracing() { return REXCVAR_GET(trace_challenges); }

// ---- the registry -------------------------------------------------------------

struct Entry {
  uint32_t address = 0;
  std::string name;
  std::string level;      // lower case, as the game has it
  uint32_t entity = 0;    // its initiator entity (0: mountain not loaded)
  uint32_t initiator = 0;
  int state = -1;         // the initiator's state
};

std::vector<Entry> ReadRegistry(uint8_t* base) {
  std::vector<Entry> out;
  const uint32_t manager = Load32(base, kManagerPtr);
  const uint32_t registry = manager ? Load32(base, manager + 48) : 0;
  if (!registry) return out;
  const uint32_t list = Load32(base, registry + 8);
  const uint32_t count = Load32(base, registry + 12);
  if (!list || count > 4000 || !Readable(list, count * 4)) return out;
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Entry e;
    e.address = Load32(base, list + i * 4);
    if (!e.address || !Readable(e.address, 100)) continue;
    e.name = GuestText(base, Load32(base, e.address + 8));
    e.level = Lower(GuestText(base, Load32(base, e.address + 12), 32));
    e.entity = Load32(base, e.address + 96);
    if (e.entity && Readable(e.entity, 0x160)) {
      e.initiator = Load32(base, e.entity + 244);
      if (e.initiator && Readable(e.initiator, 0x80)) {
        e.state = int(Load32(base, e.initiator + 60));
      } else {
        e.initiator = 0;
      }
    } else {
      e.entity = 0;
    }
    if (!e.name.empty()) out.push_back(std::move(e));
  }
  return out;
}

// ---- shared between the menu (UI thread) and the game thread -------------------

struct Shared {
  std::mutex mutex;
  std::string queued;   // a challenge to start (by name)
  std::string message;  // the last request's outcome
};
Shared& S() {
  static Shared s;
  return s;
}

void SetMessage(std::string text) {
  if (Tracing()) REXLOG_INFO("challenges: {}", text);
  std::lock_guard lock(S().mutex);
  S().message = std::move(text);
}

// ---- game thread ---------------------------------------------------------------

// Calls a game function from the state machine tick: a fresh context on the
// game thread's stack, below the tick's frame.
uint32_t CallGame(uint8_t* base, uint32_t r1, uint32_t r13, void (*fn)(PPCContext&, uint8_t*),
                  uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0) {
  PPCContext ctx{};
  ctx.r1.u64 = (r1 - 0x2000) & ~0xFull;
  ctx.r13.u64 = r13;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  fn(ctx, base);
  return ctx.r3.u32;
}

// A request waiting for another challenge to end first.
struct Pending {
  std::string name;
  std::chrono::steady_clock::time_point deadline{};
  bool aborted = false;
} g_pending;
uint32_t g_message = 0;  // a guest buffer for messages to initiators

// Ends a challenge under way, as the game does: the active state takes
// message 5 (the pause menu's abort) to "aborting"; one still starting is
// switched there directly. Results / rewards / saving end by themselves.
void Abort(uint8_t* base, uint32_t r1, uint32_t r13, const Entry& e) {
  if (e.state == kStateActive) {
    if (!g_message) {
      if (auto* memory = GuestMemory()) g_message = memory->SystemHeapAlloc(64);
    }
    if (!g_message) return;
    std::memset(base + g_message, 0, 64);
    base[g_message + 16] = 5;
    CallGame(base, r1, r13, __imp__sub_824574B0, e.initiator, g_message);
  } else if (e.state == 3 || e.state == 4 || e.state == 10) {
    Store32(base, e.initiator + 64, kStateAborting);
    base[e.initiator + 68] = 1;
    base[e.initiator + 84] = 0;
  }
  if (Tracing()) {
    REXLOG_INFO("challenges: ending '{}' first ({} '{}')", e.name, e.state,
                InitiatorStateName(e.state));
  }
}

struct Watch {
  std::string name;
  uint32_t initiator = 0;
  int last_state = -2;
  std::chrono::steady_clock::time_point until{};
} g_watch;

// ---- the screen fade ------------------------------------------------------------
//
// sub_82203470(fader, type) is the game's fade command (the HUD's fade
// element, 0x8288F1FC -> +12 -> [0] -> +32): 4 to black, 5 to black at once,
// 6 from black, 7 from black at once (1 resets it). A story mission's success
// fades to black, plays its scene, and leaves fading back in to the next story
// step; replayed, the next step has already been played, so the screen would
// stay black. After a mission started here, a fade to black that nothing
// undoes for a while while riding (or in a scene) is undone.
constexpr uint32_t kFaderRoot = 0x8288F1FC;
std::mutex g_fade_mutex;
int g_fade_type = -1;
uint32_t g_fade_fader = 0;
std::chrono::steady_clock::time_point g_fade_time{};
std::chrono::steady_clock::time_point g_replay_until{};  // a mission started here, and after

uint32_t Fader(uint8_t* base) {
  uint32_t p = Load32(base, kFaderRoot);
  p = p ? Load32(base, p + 12) : 0;
  p = p ? Load32(base, p) : 0;
  return p ? Load32(base, p + 32) : 0;
}

// ---- the gameplay sound ------------------------------------------------------------
//
// A mission's success pauses the "Pauseable" sound categories (voice, effects,
// music: all of the riding sound); the next activity resumes them
// (sub_824939E8, the game's own resume; the byte at 0x8286F847 is set while
// they're paused). Replayed, there is no next activity, so after a mission
// started here has finished, sound still paused while riding is resumed.
constexpr uint32_t kSoundPausedFlag = 0x8286F847;
bool g_sound_armed = false;  // a mission started here hasn't had its sound checked yet
bool g_mission_ran = false;  // ... and it has been under way
std::chrono::steady_clock::time_point g_mission_over{};  // when it stopped being busy (0: not yet)

void RecoverSound(uint8_t* base, uint32_t r1, uint32_t r13, int game_state) {
  if (!g_sound_armed || !g_watch.initiator || !Readable(g_watch.initiator, 0x80)) return;
  const auto now = std::chrono::steady_clock::now();
  const int s = int(Load32(base, g_watch.initiator + 60));
  if (Busy(s)) g_mission_ran = true;
  if (!g_mission_ran || Busy(s) || game_state != kStateRiding) {
    g_mission_over = {};
    return;
  }
  if (g_mission_over == std::chrono::steady_clock::time_point{}) g_mission_over = now;
  if (now - g_mission_over < std::chrono::seconds(1)) return;
  g_sound_armed = false;
  if (!base[kSoundPausedFlag]) return;
  if (Tracing()) {
    REXLOG_INFO("challenges: '{}' is over and the riding sound is still paused; resuming it",
                g_watch.name);
  }
  CallGame(base, r1, r13, __imp__sub_824939E8, 0);
}

void RecoverFade(uint8_t* base, uint32_t r1, uint32_t r13, int game_state) {
  const auto now = std::chrono::steady_clock::now();
  if (now > g_replay_until) return;
  if (game_state != kStateRiding && game_state != 20) return;
  int type;
  std::chrono::steady_clock::time_point when;
  {
    std::lock_guard lock(g_fade_mutex);
    type = g_fade_type;
    when = g_fade_time;
  }
  if ((type != 4 && type != 5) || now - when < std::chrono::seconds(2)) return;
  const uint32_t fader = Fader(base);
  if (!fader || !Readable(fader, 64)) return;
  if (Tracing()) {
    REXLOG_INFO("challenges: the screen was left faded to black (fade {} {:.1f} s ago, game state "
                "{}); fading back in",
                type, std::chrono::duration<float>(now - when).count(), game_state);
  }
  CallGame(base, r1, r13, __imp__sub_82203470, fader, 6);
  std::lock_guard lock(g_fade_mutex);
  g_fade_type = 6;
  g_fade_time = now;
}

void Start(uint8_t* base, uint32_t r1, uint32_t r13, const std::string& name) {
  const std::vector<Entry> entries = ReadRegistry(base);
  auto it = std::find_if(entries.begin(), entries.end(),
                         [&](const Entry& e) { return e.name == name; });
  if (it == entries.end()) {
    SetMessage(fmt::format("'{}' isn't in the game's challenge list.", name));
    return;
  }
  if (!it->entity || !it->initiator) {
    SetMessage(fmt::format("'{}' belongs to another mountain ({}).", name, it->level));
    return;
  }
  const uint32_t sm = it->initiator;
  if (Tracing()) {
    REXLOG_INFO("challenges: start '{}' (entry {:08X}, entity {:08X}, initiator {:08X}, state {} "
                "'{}', requested {}, pending {}, states {})",
                name, it->address, it->entity, sm, it->state, InitiatorStateName(it->state),
                int(Load32(base, sm + 64)), base[sm + 68], Load32(base, sm + 12));
  }
  if (Load32(base, sm + 12) <= kStateActivate) {
    SetMessage(fmt::format("'{}': its initiator has no activate state.", name));
    return;
  }
  // To the challenge's start point, then activate it (as the initiator's Idle
  // state does when the player triggers it).
  CallGame(base, r1, r13, __imp__sub_82172C78, it->entity);
  Store32(base, sm + 64, kStateActivate);
  base[sm + 68] = 1;
  g_watch.name = name;
  g_watch.initiator = sm;
  g_watch.last_state = it->state;
  g_watch.until = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  g_replay_until = std::chrono::steady_clock::now() + std::chrono::minutes(15);
  g_sound_armed = true;
  g_mission_ran = false;
  g_mission_over = {};
  SetMessage(fmt::format("Starting '{}' (it was {}).", name, InitiatorStateName(it->state)));
}

}  // namespace

void GameTick(uint8_t* base, uint32_t r1, uint32_t r13) {
  const int game_state = int(Load32(base, kStateMachine + 8));
  // What the initiator of the last request does next (diagnostics).
  if (g_watch.initiator && Tracing()) {
    if (std::chrono::steady_clock::now() > g_watch.until || !Readable(g_watch.initiator, 0x80)) {
      g_watch.initiator = 0;
    } else {
      const int s = int(Load32(base, g_watch.initiator + 60));
      if (s != g_watch.last_state) {
        REXLOG_INFO("challenges: '{}' initiator state {} -> {} '{}' (game state {})",
                    g_watch.name, g_watch.last_state, s, InitiatorStateName(s), game_state);
        g_watch.last_state = s;
      }
    }
  }
  if (!base[kStateMachine + 16]) {
    RecoverFade(base, r1, r13, game_state);
    RecoverSound(base, r1, r13, game_state);
  }
  if (game_state != kStateRiding || base[kStateMachine + 16]) return;
  {
    std::lock_guard lock(S().mutex);
    if (!S().queued.empty()) {
      g_pending.name = std::move(S().queued);
      S().queued.clear();
      g_pending.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      g_pending.aborted = false;
    }
  }
  if (g_pending.name.empty()) return;
  // Another challenge under way: end it, and start once it has.
  bool busy = false;
  for (const Entry& e : ReadRegistry(base)) {
    if (!e.initiator || !Busy(e.state)) continue;
    busy = true;
    if (!g_pending.aborted) Abort(base, r1, r13, e);
  }
  g_pending.aborted = true;
  if (busy) {
    if (std::chrono::steady_clock::now() > g_pending.deadline) {
      SetMessage(fmt::format("Couldn't start '{}': the challenge under way didn't end.",
                             g_pending.name));
      g_pending.name.clear();
    }
    return;
  }
  const std::string name = std::move(g_pending.name);
  g_pending.name.clear();
  Start(base, r1, r13, name);
}

namespace {

struct MountainInfo {
  const char* level;  // as the game names it
  const char* title;
};
constexpr MountainInfo kMountainTitles[] = {
    {"northstar", "Northstar"}, {"snowbird", "Snowbird"},   {"vallenevado", "Valle Nevado"},
    {"laax", "Laax"},           {"avoriaz", "Avoriaz"},     {"zugspitze", "Zugspitze"},
    {"dcml", "DCML"},           {"global", "Any mountain"},
};

std::string Title(const std::string& level) {
  for (const auto& m : kMountainTitles) {
    if (level == m.level) return m.title;
  }
  return level.empty() ? "(none)" : level;
}

int Kind(const std::string& name) {  // 0 story, 1 challenges, 2 the rest
  if (name.rfind("ST_", 0) == 0) return 0;
  if (name.rfind("Awe_", 0) == 0 || name.rfind("Omni", 0) == 0) return 2;
  return 1;
}
constexpr const char* kKinds[] = {"Story", "Challenges", "Areas and free ride"};

}  // namespace

bool DrawTab() {
  bool close = false;
  uint8_t* base = GuestBase();
  // The list, refreshed twice a second.
  static std::vector<Entry> entries;
  static auto next = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (base && now >= next) {
    entries = ReadRegistry(base);
    next = now + std::chrono::milliseconds(500);
  }
  const int game_state = base ? int(Load32(base, kStateMachine + 8)) : -1;
  std::string message;
  {
    std::lock_guard lock(S().mutex);
    message = S().message;
  }
  std::string loaded;  // the mountain whose initiators are there
  for (const Entry& e : entries) {
    if (e.entity && e.level != "global") {
      loaded = e.level;
      break;
    }
  }

  ImGui::Spacing();
  ImGui::TextWrapped("Starts any challenge or story mission on the loaded mountain, including "
                     "story missions already played. You are put at its start and it begins as "
                     "if you had triggered it. Requests wait until you are riding; a challenge "
                     "already under way is ended first.");
  ImGui::Text("Mountain loaded: %s   (game state %d)", loaded.empty() ? "none" : Title(loaded).c_str(),
              game_state);
  if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
  if (entries.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f),
                       "The game's challenge list isn't loaded yet (load a mountain).");
    return false;
  }

  static char filter[64] = "";
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
  ImGui::InputTextWithHint("##filter", "filter by name", filter, sizeof(filter));
  const std::string f = Lower(filter);

  // Mountains in the game's order, the loaded one first.
  std::vector<std::string> levels;
  for (const Entry& e : entries) {
    if (std::find(levels.begin(), levels.end(), e.level) == levels.end()) levels.push_back(e.level);
  }
  std::stable_partition(levels.begin(), levels.end(),
                        [&](const std::string& l) { return l == loaded; });

  for (const std::string& level : levels) {
    int count = 0;
    for (const Entry& e : entries) count += e.level == level;
    const bool here = level == loaded || (level == "global" && !loaded.empty());
    const std::string header =
        fmt::format("{}  ({}){}###{}", Title(level), count, level == loaded ? "  - loaded" : "",
                    level);
    if (level == loaded) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (!ImGui::CollapsingHeader(header.c_str())) continue;
    ImGui::PushID(level.c_str());
    for (int kind = 0; kind < 3; ++kind) {
      bool any = false;
      for (const Entry& e : entries) any |= e.level == level && Kind(e.name) == kind;
      if (!any) continue;
      ImGui::SeparatorText(kKinds[kind]);
      if (!ImGui::BeginTable(kKinds[kind], 3,
                             ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        continue;
      }
      ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.6f);
      ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch, 0.25f);
      ImGui::TableSetupColumn("go", ImGuiTableColumnFlags_WidthStretch, 0.15f);
      for (const Entry& e : entries) {
        if (e.level != level || Kind(e.name) != kind) continue;
        if (!f.empty() && Lower(e.name).find(f) == std::string::npos) continue;
        ImGui::PushID(e.name.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::TableNextColumn();
        if (e.initiator) {
          ImGui::TextDisabled("%s", InitiatorStateName(e.state));
        } else {
          ImGui::TextDisabled(here ? "no start point" : "-");
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!e.initiator || game_state < 0);
        if (ImGui::SmallButton("Start")) {
          {
            std::lock_guard lock(S().mutex);
            S().queued = e.name;
          }
          SetMessage(game_state == kStateRiding
                         ? fmt::format("Starting '{}'.", e.name)
                         : fmt::format("'{}' starts once you are riding.", e.name));
          close = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (!here) ImGui::TextDisabled("Load %s to start these.", Title(level).c_str());
    ImGui::PopID();
  }
  return close;
}

}  // namespace amped3::challenges

// Midasm hook at the game's fade command (sub_82203470: r3 = the fader, r4 =
// the fade): remembers the last one, for RecoverFade.
void Amped3_FadeCommand(PPCRegister& r3, PPCRegister& r4) {
  using namespace amped3::challenges;
  const int type = int(r4.u32);
  {
    std::lock_guard lock(g_fade_mutex);
    g_fade_type = type;
    g_fade_fader = r3.u32;
    g_fade_time = std::chrono::steady_clock::now();
  }
  if (Tracing() && std::chrono::steady_clock::now() < g_replay_until) {
    uint8_t* base = GuestBase();
    REXLOG_INFO("challenges: fade {} (fader {:08X}, game state {})", type, r3.u32,
                base ? int(Load32(base, kStateMachine + 8)) : -1);
  }
}
