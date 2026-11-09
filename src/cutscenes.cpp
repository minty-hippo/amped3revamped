// amped3 - story cutscene player. See cutscenes.h.
//
// What the game's code shows (default.xex):
//   0x828901E0    the game's state machine: +8 current state, +12 requested
//                 state, +16 request pending (byte), +132 state table.
//                 State 13 = riding, state 20 = challenge intro (a scene).
//   sub_8215B0B0  state machine tick (game thread, every frame): switches to
//                 a requested state (Exit of the old, Enter of the new), then
//                 runs the state's Update.
//   sub_8215CED0  state 20 Enter: freezes the riders and plays the current
//                 challenge's scene (challenge (0x8288F0A8) + 72, a string)
//                 as "%s.grover"; scene-specific music cues are keyed by
//                 that same name.
//   sub_8215D420  state 20 Update: ticks the scene; when it has stopped
//                 (ended, or skipped with the button) clears challenge + 68
//                 and asks for state 13.
//   sub_8215D1D8  state 20 Exit: stops the scene, lets the riders go.
//   sub_824B9AB8  GroverPlayer::Play(player (0x8288FB70), const RString&):
//                 loads and starts the scene; returns false if it can't.
//   sub_824B9C10  GroverPlayer::Stop(player): +12 = playing, +16 = name.
//   sub_82154A58  RString(const char*): the game's string constructor.
//   sub_82583450  the in-engine cinematic object's LoadCinematic() (traced).
//
// A menu Play: on the game thread (in the state machine tick) the scene
// name is made into a game string, and state 20 is requested. Enter then
// finds a neutral name in place of the challenge's (so the game's scene-keyed
// music cues stay out of it) and Play is handed the real one; Exit puts the
// challenge's own name back (and challenge + 68 when the scene was started while riding).
// Skip calls Stop, so the state's own Update ends the scene on that frame.

#include "cutscenes.h"

#include "challenges.h"

namespace amped3::rail_trace {
void Tick(uint8_t* base);  // src/rail_trace.cpp
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
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

REXCVAR_DEFINE_BOOL(trace_cutscenes, true, "Amped 3/Diagnostics",
                    "Log the story cutscenes the game starts and stops (to the log file): "
                    "their names, and requests from the Cutscenes tab.");

REX_EXTERN(__imp__sub_8215D420);
REX_EXTERN(__imp__sub_824B9AB8);
REX_EXTERN(__imp__sub_824B9C10);
REX_EXTERN(__imp__sub_8215CED0);
REX_EXTERN(__imp__sub_8215D1D8);
REX_EXTERN(__imp__sub_82583450);
REX_EXTERN(__imp__sub_82154A58);

namespace amped3::cutscenes {
namespace {

constexpr uint32_t kStateMachine = 0x828901E0;  // lis -32119, 480
constexpr uint32_t kChallengePtr = 0x8288F0A8;  // current challenge (lis -32119, -3928)
constexpr uint32_t kGroverPtr = 0x8288FB70;     // the cutscene player (lis -32119, -1168)
constexpr uint32_t kIntroPlayReturn = 0x8215D13C;  // Play's return address in state 20 Enter
constexpr uint32_t kWorldPtr = 0x8288FB64;     // the game's world object (lis -32119, -1180)
constexpr int kStateRiding = 13;
constexpr int kStateScene = 20;

// The game's story scenes (Cinematics\Cinematics.res holds one "<name>.grover"
// each) and the mountain whose story plays them (ChallengeData's <mountain>_Flow
// scripts).
struct SceneInfo {
  const char* name;
  const char* mountain;
};
constexpr SceneInfo kScenes[] = {
    {"Scene_0.1", "Northstar"},      {"Scene_0.2", "Northstar"},
    {"scene_0.3a", "Northstar"},     {"scene_0.3c", "Northstar"},
    {"Scene_1.1.1", "Northstar"},    {"Scene_1.1.2", "Northstar"},
    {"Scene_1.1.4", "Northstar"},    {"Scene_1.1.6", "Northstar"},
    {"Scene_1.2.1", "Northstar"},    {"Scene_1.2.2a", "Northstar"},
    {"Scene_1.2.2b", "Northstar"},   {"Scene_1.2.4", "Northstar"},
    {"Scene_1.2.5", "Northstar"},    {"Scene_1.2.6a", "Northstar"},
    {"Scene_1.2.6b", "Northstar"},   {"Scene_1.3.0", "Northstar"},
    {"Scene_1.3.1", "Northstar"},    {"Scene_1.3.3a", "Northstar"},
    {"Scene_1.3.3b", "Northstar"},   {"Scene_1.3.9", "Northstar"},
    {"Scene_1.4.1", "Northstar"},    {"Scene_1.4.4", "Northstar"},
    {"Scene_1.4.5", "Northstar"},    {"scene_1.4.7", "Northstar"},
    {"Scene_2.1.1", "Snowbird"},     {"Scene_2.2.1", "Snowbird"},
    {"Scene_2.2.4", "Snowbird"},     {"Scene_2.2.6", "Snowbird"},
    {"Scene_2.2.8", "Snowbird"},     {"Scene_2.4.1", "Snowbird"},
    {"Scene_2.4.3", "Snowbird"},     {"scene_3.1.b.1", "Valle Nevado"},
    {"Scene_3.3.4", "Valle Nevado"}, {"Scene_3.3.6", "Valle Nevado"},
    {"scene_4.0.2", "Valle Nevado"}, {"Scene_4.3.1", "Valle Nevado"},
    {"Scene_4.3.3", "Valle Nevado"}, {"Scene_4.3.7", "Valle Nevado"},
    {"Scene_4.2.2", "Laax"},         {"Scene_4.4.1", "Avoriaz"},
    {"Scene_4.4.3", "Avoriaz"},      {"Scene_4.4.4", "Avoriaz"},
    {"Scene_4.4.5", "Avoriaz"},      {"Scene_4.4.7", "Avoriaz"},
    {"Scene_4.4.12", "Avoriaz"},     {"Scene_5.1.1", "Avoriaz"},
    {"Scene_5.1.3", "Avoriaz"},      {"Scene_5.1.5", "Avoriaz"},
    {"Scene_5.1.7", "Avoriaz"},      {"scene_5.2.1", "Zugspitze"},
    {"scene_5.2.2", "Zugspitze"},    {"scene_5.2.4a", "Zugspitze"},
    {"scene_5.2.4b", "Zugspitze"},   {"scene_5.2.5", "DCML"},
    {"scene_2.4.2", "Snowbird"},  // in the archive, but no story plays it
};
constexpr const char* kMountains[] = {"Northstar", "Snowbird", "Valle Nevado", "Laax",
                                      "Avoriaz",   "Zugspitze", "DCML"};

// The name a menu scene shows the game's scene-keyed music cues in place of
// its own: the game starts, stops or switches music for a few story scenes
// (0.2, 0.3/0.3a/0.3c, 5.2.1, 5.2.2) by name, which out of story order leaves
// the music broken. Only the cutscene player itself gets the real name.
constexpr const char* kNeutralName = "amped3_menu_scene";

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
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

// The game's string objects hold a pointer to their characters.
std::string GuestRString(uint8_t* base, uint32_t object) {
  return GuestText(base, Load32(base, object));
}

bool Tracing() { return REXCVAR_GET(trace_cutscenes); }

std::string StripGrover(std::string name) {
  constexpr std::string_view ext = ".grover";
  if (name.size() > ext.size()) {
    std::string tail = name.substr(name.size() - ext.size());
    std::transform(tail.begin(), tail.end(), tail.begin(), ::tolower);
    if (tail == ext) name.resize(name.size() - ext.size());
  }
  return name;
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

// Calls a guest function from inside an override, leaving the registers as
// they were (so the overridden function then runs with its own arguments).
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*),
                   uint32_t r3, uint32_t r4 = 0) {
  PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// ---- shared between the menu (UI thread) and the game thread ---------------

enum class Request { kNone, kPlay, kSkip };

struct Shared {
  std::mutex mutex;
  Request request = Request::kNone;
  std::string request_name;
  // Status, written by the game thread.
  std::string playing;     // the scene playing ("" none)
  bool playing_from_menu = false;
  std::string message;     // the last request's outcome
  std::vector<std::string> seen;  // scenes the game played this session
};
Shared& S() {
  static Shared s;
  return s;
}
std::atomic<int> g_state{-1};

// ---- game thread only -------------------------------------------------------

struct GameSide {
  // Guest buffers for game strings: [0] the neutral name, [1] "<name>.grover".
  uint32_t text[2] = {};
  uint32_t holder[2] = {};
  int last_state = -2;
  // A menu scene about to be entered (set when state 20 is requested).
  std::string pending_name;
  int pending_origin = -1;
  bool skip = false;  // stop the scene on the next state 20 Update
  int origin = -1;    // the state to go back to after the menu scene
  // The menu scene in state 20 now.
  bool active = false;
  uint32_t our_chars = 0;
  uint32_t saved_chars = 0;  // the challenge's own scene name
  uint32_t saved_68 = 0;
  bool restore_68 = false;
  std::string real_name;
  std::string queued_name;  // a Play request waiting for riding  // the menu scene (Play is given it in place of the neutral name)
} g;
// Guards `g`: menu requests are made on the UI thread, carried out on the
// game thread (state 20 Enter / Update / Exit).
std::mutex g_mutex;

uint8_t* GuestBase() {
  auto* memory = GuestMemory();
  return memory ? memory->virtual_membase() : nullptr;
}

void SetMessage(std::string text) {
  if (Tracing()) REXLOG_INFO("cutscene player: {}", text);
  std::lock_guard lock(S().mutex);
  S().message = std::move(text);
}

// Turns `name` into a game string (in Enter, where guest calls can be made).
// Returns the characters pointer; the string object is at g.holder[slot].
uint32_t MakeGameString(PPCContext& ctx, uint8_t* base, int slot, const std::string& name) {
  auto* memory = GuestMemory();
  if (!g.text[slot]) g.text[slot] = memory->SystemHeapAlloc(256);
  if (!g.holder[slot]) g.holder[slot] = memory->SystemHeapAlloc(16);
  if (!g.text[slot] || !g.holder[slot]) return 0;
  const std::string text = name.substr(0, 200);
  std::memcpy(base + g.text[slot], text.c_str(), text.size() + 1);
  Store32(base, g.holder[slot], 0);
  CallGuest(ctx, base, __imp__sub_82154A58, g.holder[slot], g.text[slot]);
  return Load32(base, g.holder[slot]);
}

// UI thread. Asks the game's state machine for state 20; Enter picks up the name.
void StartScene(uint8_t* base, const std::string& name, int state) {
  const uint32_t challenge = Load32(base, kChallengePtr);
  if (state < 0 || !challenge || !Readable(challenge + 64, 16)) {
    SetMessage(fmt::format("Can't play '{}' now: no mountain is loaded (game state {}).", name,
                           state));
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    g.queued_name = name;
  }
  SetMessage(state == kStateRiding || state == kStateScene
                 ? fmt::format("Starting '{}'.", name)
                 : fmt::format("'{}' starts once riding resumes (game state {}).", name, state));
}

// Game thread (state machine tick): starts a queued scene once the game is
// riding (13) or in a scene (20). Leaving other states for a scene runs their
// exit code (state 7, after a pause, resets the riders when left for
// anything but riding), so those are waited out.
void StartQueued(uint8_t* base, int state) {
  if (state != kStateRiding && state != kStateScene) return;
  if (base[kStateMachine + 16]) return;  // the game is switching states itself
  std::lock_guard lock(g_mutex);
  if (g.queued_name.empty()) return;
  g.pending_name = std::move(g.queued_name);
  g.queued_name.clear();
  // A restart keeps the first scene's origin (riding, or the game's own intro).
  g.pending_origin = (state == kStateScene && g.active) ? g.origin : state;
  Store32(base, kStateMachine + 12, kStateScene);
  base[kStateMachine + 16] = 1;
}

void SkipScene(int state) {
  if (state != kStateScene) {
    SetMessage("No cutscene is playing.");
    return;
  }
  std::lock_guard lock(g_mutex);
  g.skip = true;
}

// ---- scene list (UI thread) --------------------------------------------------

bool SameName(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }

// UI thread: carries out a button press (the game thread finishes it).
void Post(Request request, std::string name = {}) {
  uint8_t* base = GuestBase();
  if (!base) return;
  const int state = Readable(kStateMachine + 8, 4) ? int(Load32(base, kStateMachine + 8)) : -1;
  if (request == Request::kPlay) StartScene(base, name, state);
  if (request == Request::kSkip) SkipScene(state);
}

const char* StateName(int state) {
  switch (state) {
    case kStateRiding: return "riding";
    case kStateScene: return "cutscene";
    case -1: return "not started";
    default: return "other";
  }
}

}  // namespace

bool DrawTab() {
  bool close = false;

  int state = -1;
  if (uint8_t* base = GuestBase()) {
    const uint32_t challenge = Load32(base, kChallengePtr);
    state = challenge ? int(Load32(base, kStateMachine + 8)) : -1;
    static int logged = -2;
    if (state != logged && Tracing()) {
      REXLOG_INFO("cutscene player: game state {} (challenge {:08X}, state object {:08X})", state,
                  challenge, Load32(base, kStateMachine + 144));
      logged = state;
    }
  }
  g_state.store(state);
  std::string playing, message;
  bool from_menu = false;
  {
    std::lock_guard lock(S().mutex);
    playing = S().playing;
    from_menu = S().playing_from_menu;
    message = S().message;
  }
  const bool can_play = state >= 0;

  ImGui::Spacing();
  ImGui::SeparatorText("Now");
  ImGui::Text("Game: %s (state %d)", StateName(state), state);
  if (state == kStateScene && !playing.empty()) {
    ImGui::Text("Playing: %s%s", StripGrover(playing).c_str(), from_menu ? "  (from this menu)" : "");
  }
  ImGui::BeginDisabled(state != kStateScene);
  if (ImGui::Button("Skip cutscene")) Post(Request::kSkip);
  ImGui::SameLine();
  if (ImGui::Button("Restart cutscene") && !playing.empty()) {
    Post(Request::kPlay, StripGrover(playing));
    close = true;
  }
  ImGui::EndDisabled();
  if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());

  ImGui::Spacing();
  ImGui::SeparatorText("Scenes");
  if (!can_play) {
    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f),
                       "Scenes can be played once a mountain is loaded.");
  }
  // The loaded mountain: the game's world object (0x8288FB64) + 48 holds its
  // level name ("northstar", "vallenevado", ...), as used for the level's
  // string tables.
  std::string level;
  if (uint8_t* base = GuestBase()) {
    if (const uint32_t world = Load32(base, kWorldPtr)) level = Lower(GuestText(base, world + 48, 32));
  }
  auto same_mountain = [&](const char* mountain) {
    std::string m = Lower(mountain);
    m.erase(std::remove(m.begin(), m.end(), ' '), m.end());
    return !level.empty() && m == level;
  };
  static std::string logged_level = "?";
  if (level != logged_level && Tracing()) {
    REXLOG_INFO("cutscene player: mountain loaded '{}'", level);
    logged_level = level;
  }
  // Not one of the mountains (unreadable): don't restrict anything.
  if (std::none_of(std::begin(kMountains), std::end(kMountains), same_mountain)) level.clear();
  static bool allow_other = false;
  if (!level.empty()) {
    ImGui::Text("Mountain loaded: %s", level.c_str());
    ImGui::Checkbox("Allow scenes from other mountains (may crash)", &allow_other);
    ImGui::TextDisabled("They need characters and props the game loads with their own mountain:\n"
                        "some play without voices, some crash the game (1.2.5 on Snowbird).");
  }

  const std::string playing_bare = Lower(StripGrover(playing));
  for (const char* mountain : kMountains) {
    int count = 0;
    bool has_current = false;
    for (const SceneInfo& s : kScenes) {
      if (std::string_view(s.mountain) != mountain) continue;
      ++count;
      has_current |= state == kStateScene && Lower(s.name) == playing_bare;
    }
    const bool here = level.empty() || same_mountain(mountain);
    const std::string header = here && !level.empty()
                                   ? fmt::format("{}  ({})  - this mountain###{}", mountain, count,
                                                 mountain)
                                   : fmt::format("{}  ({})###{}", mountain, count, mountain);
    if (has_current || (here && !level.empty())) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (!ImGui::CollapsingHeader(header.c_str())) continue;
    if (ImGui::BeginTable(mountain, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("scene", ImGuiTableColumnFlags_WidthStretch, 0.8f);
      ImGui::TableSetupColumn("play", ImGuiTableColumnFlags_WidthStretch, 0.2f);
      for (const SceneInfo& s : kScenes) {
        if (std::string_view(s.mountain) != mountain) continue;
        ImGui::PushID(s.name);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (state == kStateScene && Lower(s.name) == playing_bare) {
          ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s  (playing)", s.name);
        } else {
          ImGui::TextUnformatted(s.name);
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!can_play || (!here && !allow_other));
        if (ImGui::SmallButton("Play")) {
          Post(Request::kPlay, s.name);
          close = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }

  ImGui::Spacing();
  static char typed[96] = "";
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
  ImGui::InputTextWithHint("##typed", "other scene name", typed, sizeof(typed));
  ImGui::SameLine();
  ImGui::BeginDisabled(!can_play || !typed[0]);
  if (ImGui::Button("Play##typed")) {
    Post(Request::kPlay, StripGrover(typed));
    close = true;
  }
  ImGui::EndDisabled();
  return close;
}

}  // namespace amped3::cutscenes

using namespace amped3::cutscenes;

// Midasm hook at the start of the state machine tick (0x8215B0D0, r31 = the
// state machine; game thread, every frame): takes the menu's requests before
// the game switches states. (A hook rather than an override: the game calls
// this function directly from code where an override isn't reached.)
void Amped3_CutsceneTick(PPCRegister& r1, PPCRegister& r13, PPCRegister& r31) {
  if (r31.u32 == kStateMachine) {
    if (auto* memory = GuestMemory()) {
      uint8_t* base = memory->virtual_membase();
      StartQueued(base, int(Load32(base, kStateMachine + 8)));
      amped3::challenges::GameTick(base, r1.u32, r13.u32);
      amped3::rail_trace::Tick(base);
    }
  }
  // Diagnostics: which state machines tick.
  static uint32_t seen[4] = {};
  for (uint32_t& v : seen) {
    if (v == r31.u32) return;
    if (!v) {
      v = r31.u32;
      if (Tracing()) REXLOG_INFO("cutscene player: state machine {:08X} ticks", v);
      return;
    }
  }
}

// GroverPlayer::Play(this = r3, file = r4: the RString "<name>.grover").
extern "C" REX_FUNC(sub_824B9AB8) {
  const bool from_intro = uint32_t(ctx.lr) == kIntroPlayReturn;
  if (from_intro && g.active && g.holder[1]) ctx.r4.u64 = g.holder[1];
  const std::string name = GuestRString(base, ctx.r4.u32);
  if (Tracing()) {
    REXLOG_INFO("cutscene trace: play '{}' (player {:08X}, called from {:08X}{})", name,
                ctx.r3.u32, uint32_t(ctx.lr), g.active ? ", from the menu" : "");
  }
  __imp__sub_824B9AB8(ctx, base);
  const bool loaded = (ctx.r3.u32 & 0xFF) != 0;
  {
    std::lock_guard lock(S().mutex);
    S().playing = loaded ? name : std::string{};
    S().playing_from_menu = from_intro && g.active;
    const std::string bare = StripGrover(name);
    if (loaded && std::none_of(S().seen.begin(), S().seen.end(),
                               [&](const std::string& s) { return Lower(s) == Lower(bare); })) {
      S().seen.push_back(bare);
    }
  }
  if (from_intro && g.active) {
    SetMessage(loaded ? fmt::format("Playing '{}'.", StripGrover(name))
                      : fmt::format("The game couldn't load '{}' (it may need its own mountain "
                                    "loaded, or the name is wrong).",
                                    StripGrover(name)));
  }
}

// GroverPlayer::Stop(this = r3).
extern "C" REX_FUNC(sub_824B9C10) {
  if (Tracing()) {
    const uint32_t player = ctx.r3.u32;
    REXLOG_INFO("cutscene trace: stop '{}' (playing {}, called from {:08X})",
                GuestRString(base, player + 16), Readable(player + 12, 1) ? base[player + 12] : 0,
                uint32_t(ctx.lr));
  }
  __imp__sub_824B9C10(ctx, base);
  std::lock_guard lock(S().mutex);
  S().playing.clear();
  S().playing_from_menu = false;
}

// State 20 (challenge intro) Enter (this = r3).
extern "C" REX_FUNC(sub_8215CED0) {
  const uint32_t challenge = Load32(base, kChallengePtr);
  std::string pending;
  int pending_origin = -1;
  {
    std::lock_guard lock(g_mutex);
    pending = std::move(g.pending_name);
    pending_origin = g.pending_origin;
    g.pending_name.clear();
    g.skip = false;
  }
  if (!pending.empty() && challenge) {
    const uint32_t chars = MakeGameString(ctx, base, 0, kNeutralName);
    const uint32_t real = MakeGameString(ctx, base, 1, pending + ".grover");
    std::lock_guard lock(g_mutex);
    if (chars && real) {
      g.real_name = pending;
      g.saved_chars = Load32(base, challenge + 72);
      g.saved_68 = Load32(base, challenge + 68);
      g.origin = pending_origin;
      g.restore_68 = g.origin != kStateScene;
      g.our_chars = chars;
      g.active = true;
      Store32(base, challenge + 72, chars);
    } else {
      SetMessage(fmt::format("Can't play '{}': the game couldn't make the name.", pending));
    }
  }
  if (Tracing()) {
    REXLOG_INFO("cutscene trace: challenge intro begins (challenge {:08X}, scene '{}'{}; "
                "challenge +68 {:08X})",
                challenge, challenge ? GuestRString(base, challenge + 72) : "",
                g.active ? fmt::format(", from the menu, back to state {}", g.origin) : "",
                challenge ? Load32(base, challenge + 68) : 0);
  }
  __imp__sub_8215CED0(ctx, base);
}

// State 20 (challenge intro) Update (this = r3): Skip stops the scene first,
// so the game's own Update ends it this frame as if it had been skipped.
extern "C" REX_FUNC(sub_8215D420) {
  bool skip = false;
  {
    std::lock_guard lock(g_mutex);
    skip = g.skip;
    g.skip = false;
  }
  if (skip) {
    if (const uint32_t player = Load32(base, kGroverPtr)) {
      CallGuest(ctx, base, __imp__sub_824B9C10, player);
      SetMessage("Skipped.");
    }
  }
  __imp__sub_8215D420(ctx, base);
}

// State 20 (challenge intro) Exit (this = r3).
extern "C" REX_FUNC(sub_8215D1D8) {
  if (Tracing()) REXLOG_INFO("cutscene trace: challenge intro ends");
  __imp__sub_8215D1D8(ctx, base);
  std::lock_guard lock(g_mutex);
  if (g.active) {
    const uint32_t challenge = Load32(base, kChallengePtr);
    if (challenge && Load32(base, challenge + 72) == g.our_chars) {
      Store32(base, challenge + 72, g.saved_chars);
    }
    if (challenge && g.restore_68) Store32(base, challenge + 68, g.saved_68);
    g.active = false;
  }
}

// In-engine cinematic: LoadCinematic() (this = r3, r4 = its argument).
extern "C" REX_FUNC(sub_82583450) {
  if (Tracing()) {
    std::string arg = GuestText(base, ctx.r4.u32);
    if (arg.empty()) arg = GuestRString(base, ctx.r4.u32);
    REXLOG_INFO("cutscene trace: LoadCinematic (object {:08X}, argument {:08X} '{}', called from "
                "{:08X})",
                ctx.r3.u32, ctx.r4.u32, arg, uint32_t(ctx.lr));
  }
  __imp__sub_82583450(ctx, base);
}
