// amped3 - fixes for Amped 3's own code, applied through mid-asm hooks
// declared in amped3_functions.toml. Each fix has a setting in the
// "Amped 3/Patches" category, so it shows up in Options -> Enhancements.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xevent.h>
#include <rex/system/xmemory.h>

#include "pad_bridge.h"
#include "xinput_triggers.h"
#include "frame_limit.h"

REXCVAR_DEFINE_BOOL(fix_render_shutdown_race, true, "Amped 3/Patches",
                    "Fixes the hang on the copyright screen after the intro videos. The game "
                    "wakes its render thread before telling it to quit; on a PC the thread "
                    "can go back to sleep in between, so the game waits for it forever.");

REXCVAR_DEFINE_BOOL(fix_fade_stop, true, "Amped 3/Patches",
                    "Fixes the loading-screen music playing on underneath the game and coming "
                    "back over the menu music. The game ignores a request to fade a sound out "
                    "and stop it while that sound is still fading in; PC loading screens end "
                    "before the loading music has finished fading in, so it was never stopped.");

REXCVAR_DEFINE_BOOL(fix_stuck_tip_frame_rate, true, "Amped 3/Patches",
                    "Fixes the \"Press Back to reset your boarder\" tip popping up over and over "
                    "above 30 fps. The game decides you are stuck when you stay close to a point "
                    "that trails behind you; above 30 fps that point keeps up with you, so slow "
                    "riding counts as stuck. This makes it trail you as it does at 30 fps.");

REXCVAR_DEFINE_BOOL(fix_sled_turn_frame_rate, true, "Amped 3/Patches",
                    "Fixes sleds turning on the spot faster above 30 fps (twice as fast at "
                    "60) at the start of a run. The game turns the sled by a fixed angle each "
                    "frame; this makes it a turning speed instead, the same as at 30 fps.");

REXCVAR_DEFINE_BOOL(fix_cinematic_frame_rate, true, "Amped 3/Patches",
                    "Fixes characters in in-engine cutscenes moving too fast above 60 fps "
                    "(twice as fast at 120). The camera and dialogue were already on time.");

REXCVAR_DEFINE_BOOL(fix_sled_crash_score_frame_rate, true, "Amped 3/Patches",
                    "Fixes sled crash scores dropping above 30 fps. A crash scores by how hard "
                    "the rider hits things, measured from the physics contacts, and at higher "
                    "frame rates the physics reports fewer and softer contacts for the same "
                    "crash (about a third of the total at 120 fps). This scales the contacts "
                    "back up; how far is set by \"Sled crash score compensation\".");

REXCVAR_DEFINE_DOUBLE(sled_crash_score_compensation, 0.7, "Amped 3/Tuning",
                      "How much fix_sled_crash_score_frame_rate scales sled crash hits up "
                      "above 30 fps: each hit is multiplied by (fps / 30) to this power. "
                      "0 = no change, 1 = in proportion to the frame rate (x2 at 60 fps, x4 "
                      "at 120). 0.7, measured, matches 30 fps scoring.")
    .range(0.0, 1.0);

namespace {

uint32_t LoadGuestU32(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
}

float LoadGuestF32(rex::memory::Memory* memory, uint32_t address) {
  const uint32_t bits = LoadGuestU32(memory, address);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void StoreGuestU32(rex::memory::Memory* memory, uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(address), value);
}

}  // namespace

// Hook at 0x821854F8, in the render system's shutdown (sub_821854A8), just
// before it calls SetEvent(renderer->CommitRender):
//
//   renderer->running = 0
//   SetEvent(renderer->CommitRender)        <- hook runs here
//   renderer->Stop(wait = true)             // SetEvent(owner->quit); join thread
//
// The render thread loops "wait CommitRender; render; exit if quit is set".
// Woken by CommitRender, it can check quit before Stop sets it and go back to
// waiting on CommitRender, which is never set again: the main thread then
// waits forever in the join. On the Xbox the main thread keeps the CPU until
// Stop has run, so the game never hit this. Setting quit here, first, makes
// the woken thread always see it.
//
// r11 = renderer object; renderer+4 = thread owner block; owner+92 = handle of
// the quit event (manual-reset, so setting it early is harmless: Stop sets it
// again and then closes it).
void Amped3_FixRenderShutdownRace(PPCRegister& r11) {
  if (!REXCVAR_GET(fix_render_shutdown_race)) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !r11.u32) return;
  auto* memory = kernel->memory();
  const uint32_t owner = LoadGuestU32(memory, r11.u32 + 4);
  if (!owner) return;
  const uint32_t quit_handle = LoadGuestU32(memory, owner + 92);
  auto quit = kernel->object_table()->LookupObject<rex::system::XEvent>(quit_handle);
  if (!quit) {
    REXLOG_WARN("fix_render_shutdown_race: quit event {:08X} not found", quit_handle);
    return;
  }
  quit->Set(0, false);
  REXLOG_INFO("fix_render_shutdown_race: render thread told to quit before wake-up");
}

// Hook at 0x82423238, the start of the routine that starts a volume fade on a
// sound (cue) - used for fade-ins when a cue starts, for fade-out-and-stop
// requests, and for category fades:
//
//   r3 = cue; its fade state F is at cue+108:
//        F+4 time, F+8 start, F+12 target, F+20 current volume, F+24 step,
//        F+32 cue id, F+36 action when done (0 none, 1 stop, 2 pause),
//        F+40 state (2 = fading)
//   r4 = request: +8 cue id, +12 time, +16 start volume, +20 target,
//        +24 action when done
//
// When F is already fading the routine keeps the old fade (it only recomputes
// the step) and the request is lost. "Loading Music" starts with a 7.5-unit
// fade-in and is asked to fade out and stop when the level has loaded; on the
// console loading always outlasted the fade-in, on a PC it doesn't, so the
// stop was dropped and the song kept playing (silenced in game by its
// category's volume, audible again on the menu).
//
// A request that ends by stopping or pausing the cue now replaces a fade
// that doesn't: its start volume becomes the current volume (no jump) and the
// state is cleared so the routine takes the request as a new fade.
void Amped3_FixFadeStop(PPCRegister& r3, PPCRegister& r4) {
  if (!REXCVAR_GET(fix_fade_stop)) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !r3.u32 || !r4.u32) return;
  auto* memory = kernel->memory();
  const uint32_t fade = r3.u32 + 108;
  const uint32_t request = r4.u32;
  if (LoadGuestU32(memory, fade + 40) != 2) return;
  const uint32_t action = LoadGuestU32(memory, request + 24);
  if (action == 0 || LoadGuestU32(memory, fade + 36) == action) return;
  StoreGuestU32(memory, request + 16, LoadGuestU32(memory, fade + 20));
  StoreGuestU32(memory, fade + 40, 0);
  REXLOG_INFO("fix_fade_stop: cue {} was still fading; replaced by a fade-out and {}",
              LoadGuestU32(memory, fade + 32), action == 1 ? "stop" : "pause");
}

// Hook at 0x822136CC, in the tutor's "boarder stuck" check (the caller of
// event 18, TutorResetBoarder):
//
//   d2 = |position - reference|^2      (f6; reference = rider+220..228)
//   if (d2 < 3.61)  stuck_time += dt     (f0 = 3.61, f1 = dt)
//   else            stuck_time -= dt
//   stuck_time > 3 s  ->  show "Press Back to reset your boarder"
//
// The reference is a point that trails the rider (see the fix below); the
// first version of this fix took it for last frame's position and scaled
// the threshold, which only half worked.
namespace {
struct StuckTrail {
  double ref[3] = {};
  bool valid = false;
  std::chrono::steady_clock::time_point last{};
};
std::unordered_map<uint32_t, StuckTrail> g_stuck_trail;
}  // namespace

REXCVAR_DEFINE_BOOL(trace_stuck_tip, false, "Amped 3/Diagnostics",
                    "Log the \"reset your boarder\" stuck check (to the log file).");

namespace {
struct StuckTipTrace {
  std::chrono::steady_clock::time_point start{};
  uint32_t calls = 0;
  uint32_t stuck_calls = 0;
  double dist_sum = 0;
  double threshold = 0;
  double dt_sum = 0;
  float ref[3] = {};
  uint32_t ref_changes = 0;
  uint32_t calls_since_change = 0;
  uint32_t max_calls_between = 0;
  double pos_step_sum = 0;   // how far the rider's position moved since the last check
  double ref_step_sum = 0;   // how far the reference moved since the last check
  double ref_to_last_pos = 0;  // distance from the reference to the last check's position
} g_stuck_tip;
float g_stuck_ref[3] = {};
float g_stuck_pos[3] = {};
bool g_stuck_have = false;
}  // namespace

void Amped3_FixStuckTipFrameRate(PPCRegister& f0, PPCRegister& f1, PPCRegister& f6,
                                 PPCRegister& r11) {
  const double dt = f1.f64;
  if (REXCVAR_GET(fix_stuck_tip_frame_rate) && dt > 0.0 && dt < 1.0 / 32.0 && r11.u32) {
    // The check compares the rider with a reference point that trails it
    // (rider+220..228). At 30 fps that point follows the rider like a lerp
    // of 2.33% a frame - about 1.4 s behind - so a rider who keeps moving
    // stays far from it; at high frame rates it ends up a frame or two
    // behind, so any slow riding counts as stuck. Measured in the logs: at
    // 30 fps the point moves 0.0233 x its distance each frame (steady over
    // distances from 0.3 to 400); at 120 fps it sits 0.24 units behind.
    // Above 30 fps the check uses its own trailing point, following the
    // rider by the same amount per second as at 30 fps, with the original
    // threshold.
    if (auto* kernel = rex::system::kernel_state()) {
      auto* memory = kernel->memory();
      float pos[3];
      for (int i = 0; i < 3; ++i) pos[i] = LoadGuestF32(memory, r11.u32 + 64 + i * 4);
      auto& trail = g_stuck_trail[r11.u32];
      const auto now = std::chrono::steady_clock::now();
      if (!trail.valid || now - trail.last > std::chrono::milliseconds(500)) {
        for (int i = 0; i < 3; ++i) trail.ref[i] = LoadGuestF32(memory, r11.u32 + 220 + i * 4);
        trail.valid = true;
      }
      trail.last = now;
      const double follow = 1.0 - std::pow(1.0 - 0.0233, dt * 30.0);
      double d2 = 0;
      for (int i = 0; i < 3; ++i) {
        trail.ref[i] += (pos[i] - trail.ref[i]) * follow;
        const double d = pos[i] - trail.ref[i];
        d2 += d * d;
      }
      f6.f64 = d2;
    }
  }
  if (!REXCVAR_GET(trace_stuck_tip)) return;
  // f6 = squared distance from the reference position, f0 = the threshold
  // (as compared), r11 = the rider (+1636 = time counted as stuck).
  auto& t = g_stuck_tip;
  const auto now = std::chrono::steady_clock::now();
  if (!t.calls) t.start = now;
  // How often the reference position (rider+220..228) changes.
  if (auto* kernel = rex::system::kernel_state(); kernel && r11.u32) {
    float ref[3], pos[3];
    for (int i = 0; i < 3; ++i) {
      ref[i] = LoadGuestF32(kernel->memory(), r11.u32 + 220 + i * 4);
      pos[i] = LoadGuestF32(kernel->memory(), r11.u32 + 64 + i * 4);
    }
    auto dist = [](const float* a, const float* b) {
      const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
      return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    if (g_stuck_have) {
      t.pos_step_sum += dist(pos, g_stuck_pos);
      t.ref_step_sum += dist(ref, g_stuck_ref);
      t.ref_to_last_pos += dist(ref, g_stuck_pos);
    }
    std::memcpy(g_stuck_pos, pos, sizeof(pos));
    g_stuck_have = true;
    t.calls_since_change++;
    if (std::memcmp(ref, g_stuck_ref, sizeof(ref)) != 0) {
      std::memcpy(g_stuck_ref, ref, sizeof(ref));
      t.ref_changes++;
      t.max_calls_between = std::max(t.max_calls_between, t.calls_since_change);
      t.calls_since_change = 0;
    }
  }
  t.calls++;
  if (f6.f64 < f0.f64) t.stuck_calls++;
  t.dist_sum += std::sqrt(std::max(0.0, f6.f64));
  t.threshold = std::sqrt(std::max(0.0, f0.f64));
  t.dt_sum += dt;
  const double wall = std::chrono::duration<double>(now - t.start).count();
  if (wall >= 0.5) {
    float stuck_time = 0;
    if (auto* kernel = rex::system::kernel_state(); kernel && r11.u32) {
      stuck_time = LoadGuestF32(kernel->memory(), r11.u32 + 1636);
    }
    REXLOG_INFO(
        "stuck check: {} calls in {:.2f} s (game {:.3f} s), {} stuck; distance avg {:.3f} "
        "(threshold {:.3f}); stuck time {:.2f} s; reference moved {} times (longest gap {} "
        "calls); per check: rider moved {:.3f}, reference moved {:.3f}, reference to last "
        "position {:.3f}",
        t.calls, wall, t.dt_sum, t.stuck_calls, t.dist_sum / t.calls, t.threshold, stuck_time,
        t.ref_changes, t.max_calls_between, t.pos_step_sum / t.calls, t.ref_step_sum / t.calls,
        t.ref_to_last_pos / t.calls);
    const uint32_t since = t.calls_since_change;
    t = {};
    t.calls_since_change = since;
  }
}

// Turning on the spot before a sled run (the sled "wait" state, update
// sub_82259078 with f1 = dt):
//
//   if (|stick| > dead zone)
//     sled transform = rotate(sled transform, stick * -3 degrees)   <- per frame
//
// The update never reads dt, so the sled turns 90 degrees a second at 30 fps
// and 180 at 60. The first hook (0x82259090, after the prologue) keeps dt;
// the second (0x82259164, just before the rotation is built, f1 = angle)
// scales the angle by dt * 30.
namespace {
double g_sled_wait_dt = 0.0;
}  // namespace

void Amped3_SledWaitDt(PPCRegister& f1) { g_sled_wait_dt = f1.f64; }

void Amped3_FixSledWaitTurnFrameRate(PPCRegister& f1) {
  if (!REXCVAR_GET(fix_sled_turn_frame_rate)) return;
  const double dt = g_sled_wait_dt;
  if (!(dt > 0.0 && dt < 0.25)) return;
  f1.f64 = f1.f64 * dt * 30.0;
}

// ---------------------------------------------------------------------------
// Diagnostics: sled crash scoring (setting trace_sled_crash).
//
//   sub_823AC118  ragdoll update, once a frame (f1 = dt; hook after "mr r23,
//                 r3" at 0x823AC168: f29 = dt, r23 = ragdoll)
//   sub_823AD790  a contact on a ragdoll bone: hit[bone][kind] += |projected
//                 velocity| (hook at 0x823AD818, the add: f1 = value,
//                 r25 = kind)
//   sub_823AD848  scoring event, every m_timeBetweenScoringEvents seconds
//                 (ragdoll+492): the bone with the largest hit sum scores by
//                 its sum against the thresholds at ragdoll+436..448
//                 (hook at 0x823AD868: r22 = ragdoll)
//
// One log line per scoring event: frames and time since the last one, the
// contacts added, and the sums.
//
// Measured at 30 and 120 fps (same course, several crashes each): per 0.36 s
// scoring interval, 32.5 contacts averaging 4.4 at 30 fps against 14.6
// averaging 2.8 at 120 fps - a total about 3.5 times smaller, so the best
// bone rarely reaches the "soft" (60) or "medium" (160) thresholds. The total
// shrinks with the frame time, so fix_sled_crash_score_frame_rate multiplies
// each contact by (fps / 30)^sled_crash_score_compensation.
//
// Calibrated from about 8 crash runs each at 30, 60 and 120 fps (700 scoring
// events): the power that makes the share of events reaching each threshold
// (20/60/160/180) match 30 fps is 0.70 for both 60 fps (x1.63) and 120 fps
// (x2.64). (Averages alone gave 0.5, but scoring counts thresholds.)
REXCVAR_DEFINE_BOOL(trace_sled_crash, false, "Amped 3/Diagnostics",
                    "Log sled crash hit scoring (to the log file). For the lower crash scores "
                    "at high frame rates.");

namespace {
struct SledCrashTrace {
  uint32_t frames = 0;
  double time = 0;
  uint32_t hits = 0;
  double hit_sum = 0;   // as the game reported them
  double hit_max = 0;
  double factor = 1.0;  // compensation applied to the last contact
  bool thresholds_logged = false;
} g_sled_trace;

}  // namespace

namespace {
double g_sled_crash_dt = 0.0;  // frame time of the last ragdoll update
}  // namespace

void Amped3_TraceSledCrashFrame(PPCRegister& f29, PPCRegister& r23) {
  (void)r23;
  g_sled_crash_dt = f29.f64;
  if (!REXCVAR_GET(trace_sled_crash)) return;
  g_sled_trace.frames++;
  g_sled_trace.time += f29.f64;
}

void Amped3_TraceSledCrashHit(PPCRegister& f1, PPCRegister& r25) {
  (void)r25;
  // The ragdoll's own time step is the game's (sled crash mode runs the
  // game at 0.7 speed), so the frame rate comes from the real frame time.
  const double frame = amped3::frame_limit::FrameSeconds();
  const double raw = f1.f64;
  double factor = 1.0;
  if (REXCVAR_GET(fix_sled_crash_score_frame_rate) && frame > 0.0 && frame < 1.0 / 32.0) {
    const double strength = std::clamp(REXCVAR_GET(sled_crash_score_compensation), 0.0, 1.0);
    factor = std::pow(1.0 / (frame * 30.0), strength);
    f1.f64 = raw * factor;
  }
  if (!REXCVAR_GET(trace_sled_crash)) return;
  g_sled_trace.hits++;
  g_sled_trace.hit_sum += raw;
  g_sled_trace.hit_max = std::max(g_sled_trace.hit_max, raw);
  g_sled_trace.factor = factor;
}

void Amped3_TraceSledCrashScore(PPCRegister& r22) {
  if (!REXCVAR_GET(trace_sled_crash)) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !r22.u32) return;
  auto* memory = kernel->memory();
  const uint32_t ragdoll = r22.u32;
  if (!g_sled_trace.thresholds_logged) {
    g_sled_trace.thresholds_logged = true;
    REXLOG_INFO(
        "sled crash: thresholds hard {:.2f} medium {:.2f} soft {:.2f} very soft {:.2f}, "
        "scoring every {:.3f} s",
        LoadGuestF32(memory, ragdoll + 436), LoadGuestF32(memory, ragdoll + 440),
        LoadGuestF32(memory, ragdoll + 444), LoadGuestF32(memory, ragdoll + 448),
        LoadGuestF32(memory, ragdoll + 492));
  }
  // Per-bone sums of the four kinds, as the scoring computes them.
  double best = 0;
  int best_bone = -1;
  for (int bone = 0; bone < 16; ++bone) {
    double sum = 0;
    for (int k = 0; k < 4; ++k) {
      sum += std::fabs(LoadGuestF32(memory, ragdoll + 500 + (bone * 4 + k) * 4));
    }
    if (sum > best) {
      best = sum;
      best_bone = bone;
    }
  }
  if (g_sled_trace.hits || best > 0) {
    const double frame = amped3::frame_limit::FrameSeconds();
    REXLOG_INFO(
        "sled crash: score event at {:.0f} fps after {} frames / {:.3f} s: {} contacts, "
        "raw sum {:.2f}, raw max {:.2f}; best bone {} raw sum {:.2f}, scored {:.2f} "
        "(x{:.3f})",
        frame > 0 ? 1.0 / frame : 0.0, g_sled_trace.frames, g_sled_trace.time, g_sled_trace.hits,
        g_sled_trace.hit_sum, g_sled_trace.hit_max, best_bone, best / g_sled_trace.factor, best,
        g_sled_trace.factor);
  }
  const bool logged = g_sled_trace.thresholds_logged;
  g_sled_trace = {};
  g_sled_trace.thresholds_logged = logged;
}

// ---------------------------------------------------------------------------
// Diagnostics: sled speed (setting trace_sled_speed).
//
// Hook at 0x8225AE54 in the sled ground update (sub_8225AE10, f24 = the
// game's time step), where r11 = the sled body's Havok motion: +64 position,
// +208 linear velocity. Once a second of real time it logs how far the sled
// moved per real second and its velocity, with the frame rate and the
// game's time step, to compare speed and acceleration across frame rates.
//
// Measured (5-6 runs each down the same course at 30, 60 and 120 fps): the
// same acceleration (about 10, 19, 27, 34 units per second over the first
// four seconds) and the same top speed (40) at every frame rate; the sled's
// speed does not depend on the frame rate.
REXCVAR_DEFINE_BOOL(trace_sled_speed, false, "Amped 3/Diagnostics",
                    "Log the sled's speed on the ground once a second (to the log file). For "
                    "checking sled speed at different frame rates.");

namespace {
struct SledSpeedTrace {
  std::chrono::steady_clock::time_point start{};
  std::chrono::steady_clock::time_point last{};
  float last_pos[3] = {};
  bool have_pos = false;
  uint32_t frames = 0;
  double game_time = 0;
  double distance = 0;
  double speed_sum = 0;
  double speed_max = 0;
  double speed_first = -1;
} g_sled_speed;
}  // namespace

void Amped3_TraceSledSpeed(PPCRegister& r11, PPCRegister& f24) {
  if (!REXCVAR_GET(trace_sled_speed)) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !r11.u32) return;
  auto* memory = kernel->memory();
  auto& t = g_sled_speed;
  const auto now = std::chrono::steady_clock::now();
  // A gap (menu, crash, another state) starts a new measurement.
  if (t.frames && now - t.last > std::chrono::milliseconds(250)) t = {};
  if (!t.frames) t.start = now;
  t.last = now;
  float pos[3], vel[3];
  for (int i = 0; i < 3; ++i) {
    pos[i] = LoadGuestF32(memory, r11.u32 + 64 + i * 4);
    vel[i] = LoadGuestF32(memory, r11.u32 + 208 + i * 4);
  }
  if (t.have_pos) {
    const double dx = pos[0] - t.last_pos[0], dy = pos[1] - t.last_pos[1],
                 dz = pos[2] - t.last_pos[2];
    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d < 50.0) t.distance += d;  // ignore teleports (restarts)
  }
  std::memcpy(t.last_pos, pos, sizeof(pos));
  t.have_pos = true;
  const double speed = std::sqrt(double(vel[0]) * vel[0] + double(vel[1]) * vel[1] +
                                 double(vel[2]) * vel[2]);
  if (t.speed_first < 0) t.speed_first = speed;
  t.frames++;
  t.game_time += f24.f64;
  t.speed_sum += speed;
  t.speed_max = std::max(t.speed_max, speed);
  const double wall = std::chrono::duration<double>(now - t.start).count();
  if (wall >= 1.0) {
    REXLOG_INFO(
        "sled speed: {} frames in {:.2f} s ({:.0f} fps), game time {:.3f} s: moved {:.1f} "
        "({:.1f}/s real); velocity first {:.1f} avg {:.1f} max {:.1f} last {:.1f}",
        t.frames, wall, t.frames / wall, t.game_time, t.distance, t.distance / wall,
        t.speed_first, t.speed_sum / t.frames, t.speed_max, speed);
    const SledSpeedTrace keep = t;
    t = {};
    t.start = now;
    t.last = now;
    std::memcpy(t.last_pos, keep.last_pos, sizeof(pos));
    t.have_pos = true;
  }
}

// In-engine cutscene animations running fast above 60 fps.
//
// The cutscene player's advance (sub_8257F8A0) keeps its time in 60 fps frames
// and walks from the current time to this frame's target in steps: up to the
// next whole frame, whole frames, then the rest. The actors are moved by the
// sum of the steps. The first step always goes to the next whole frame, even
// when the target is before it, which never happens at 30 or 60 fps (a frame
// is at least one whole step). Above 60 fps it happens every frame, so the
// actors moved a full 1/60 s per frame: twice as fast at 120 fps. This clamps
// the first step to the target when the target is in the same frame.
//
// Hook at 0x8257F9E0 ("fmr f31, f25", the first-step case): r28 = this step's
// frame, r24 = the target's whole frame, f31 = the current time, f30 = the
// target (both in frames), f28 = the step in frames, f25 = the step in seconds.
void Amped3_FixCinematicFirstStep(PPCRegister& r24, PPCRegister& r28, PPCRegister& f25,
                                  PPCRegister& f28, PPCRegister& f30, PPCRegister& f31) {
  if (!REXCVAR_GET(fix_cinematic_frame_rate)) return;
  if (r28.s32 != r24.s32) return;  // the target is past the next whole frame
  const float frames = std::max(float(f30.f64) - float(f31.f64), 0.0f);
  if (frames >= float(f28.f64)) return;
  f28.f64 = frames;
  f25.f64 = double(frames * (1.0f / 60.0f));
}

// ---------------------------------------------------------------------------
// Diagnostics: the rider's tuck (setting trace_tuck).
//
// The rider's input mapper asks the gamepad whether action 2 (both triggers)
// is held and hands the answer to a delegate on the rider's controls
// (+140; hook at "mtctr r5" before the call: r3 = the delegate, r4 = held).
// Logs the delegate's target (function, object) and, around each change of
// the tuck, which words of that object change over the following frames:
// what the game keeps for the tuck and how it eases in and out.
// ---------------------------------------------------------------------------

REXCVAR_DEFINE_BOOL(trace_tuck, false, "Amped 3/Diagnostics",
                    "Log how the game handles the tuck (both triggers) to the log file: after "
                    "about 10 tucks, the rider values that follow it.");

namespace {
// Every frame (the delegate is called once a frame), the tuck state and three
// objects are sampled: the delegate's object (the rider's controls), the rider
// (controls + 12) and its physics (rider + 96, which holds the tuck flag at
// +2400). After enough tucks, each 4-byte field read as a float is correlated
// with the tuck state; the fields that follow it (a crouch blend, a speed
// factor) come out on top.
struct TuckTrace {
  uint32_t fn = 0, self = 0;
  int held = -1;
  uint32_t toggles = 0;
  uint32_t reports = 0;
  uint64_t branch_on = 0, branch_off = 0, frames_on = 0;
  std::vector<uint8_t> held_log;              // per frame
  std::vector<std::vector<uint8_t>> frames;   // per frame: the three objects
};
TuckTrace g_tuck;
constexpr uint32_t kTuckSelfBytes = 2048;
constexpr uint32_t kTuckRiderBytes = 4096;
constexpr uint32_t kTuckPhysBytes = 4096;
constexpr uint32_t kTuckCarveBytes = 512;
constexpr uint32_t kTuckBytes = kTuckSelfBytes + kTuckRiderBytes + kTuckPhysBytes + kTuckCarveBytes;
// The rider's carve substate (it picks the physical pose), from a hook in its
// update (0x8224CE44, r31).
std::atomic<uint32_t> g_carve{0};
// Runs of the tuck branch of the physics function that reads the tuck flag
// (0x82225AB0), counted per frame (no longer hooked: it never ran).
std::atomic<uint32_t> g_tuck_branch{0};
// The local rider and its physics (from the tuck delegate): the hooks run for
// every rider.
std::atomic<uint32_t> g_tuck_rider{0}, g_tuck_phys{0};
constexpr size_t kTuckMaxFrames = 60 * 90;  // 90 s at 60 fps

bool ReadGuestInto(rex::memory::Memory* memory, uint32_t address, uint32_t size, uint8_t* out) {
  if (address < 0x10000) return false;
  auto* heap = memory->LookupHeap(address);
  if (!heap || heap->QueryRangeAccess(address, address + size - 1) ==
                   rex::memory::PageAccess::kNoAccess) {
    return false;
  }
  std::memcpy(out, memory->TranslateVirtual<const uint8_t*>(address), size);
  return true;
}

void TuckReport() {
  auto& t = g_tuck;
  const size_t n = t.frames.size();
  struct Hit {
    const char* object;
    uint32_t offset;
    double r, mean_on, mean_off, lo, hi;
  };
  std::vector<Hit> hits;
  auto swap = [](uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
  };
  for (uint32_t at = 0; at + 4 <= kTuckBytes; at += 4) {
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, on = 0, off = 0, non = 0, lo = 1e30, hi = -1e30;
    bool ok = true;
    for (size_t f = 0; f < n && ok; ++f) {
      uint32_t u;
      std::memcpy(&u, t.frames[f].data() + at, 4);
      u = swap(u);
      float v;
      std::memcpy(&v, &u, 4);
      if (!std::isfinite(v) || std::fabs(v) > 1e7f) ok = false;
      const double x = v, y = t.held_log[f];
      sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y;
      if (y > 0) { on += x; ++non; } else { off += x; }
      lo = std::min(lo, x);
      hi = std::max(hi, x);
    }
    if (ok && hi - lo < 1e-6) {
      // Tiny values: integers (pose numbers, counts) read as floats. Redo as ints.
      sx = sy = sxx = syy = sxy = on = off = non = 0;
      lo = 1e30;
      hi = -1e30;
      for (size_t f = 0; f < n; ++f) {
        uint32_t u;
        std::memcpy(&u, t.frames[f].data() + at, 4);
        const double x = int32_t(swap(u)), y = t.held_log[f];
        sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y;
        if (y > 0) { on += x; ++non; } else { off += x; }
        lo = std::min(lo, x);
        hi = std::max(hi, x);
      }
      if (hi - lo < 0.5 || hi - lo > 1000) continue;
    }
    if (!ok) continue;
    const double vx = n * sxx - sx * sx, vy = n * syy - sy * sy;
    if (vx <= 0 || vy <= 0) continue;
    const double r = (n * sxy - sx * sy) / std::sqrt(vx * vy);
    const uint32_t b1 = kTuckSelfBytes, b2 = b1 + kTuckRiderBytes, b3 = b2 + kTuckPhysBytes;
    const char* object = at < b1 ? "controls" : at < b2 ? "rider" : at < b3 ? "physics" : "carve";
    const uint32_t offset = at < b1 ? at : at < b2 ? at - b1 : at < b3 ? at - b2 : at - b3;
    hits.push_back({object, offset, r, non ? on / non : 0, n - non ? off / (n - non) : 0, lo, hi});
  }
  std::sort(hits.begin(), hits.end(),
            [](const Hit& a, const Hit& b) { return std::fabs(a.r) > std::fabs(b.r); });
  REXLOG_INFO("tuck trace: report {} ({} frames, {} tucks): tuck branch ran {} times tucked "
              "({} frames), {} times not; fields that follow the tuck",
              t.reports + 1, n, t.toggles / 2, t.branch_on, t.frames_on, t.branch_off);
  t.branch_on = t.branch_off = t.frames_on = 0;
  for (size_t i = 0; i < hits.size() && i < 40; ++i) {
    const Hit& h = hits[i];
    REXLOG_INFO("tuck trace:   {} +{}: r={:+.3f}  tucked avg {:g}, not {:g}  (range {:g}..{:g})",
                h.object, h.offset, h.r, h.mean_on, h.mean_off, h.lo, h.hi);
  }
}
}  // namespace

void Amped3_TraceTuck(PPCRegister& r3, PPCRegister& r4) {
  if (!r3.u32) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return;
  auto* memory = kernel->memory();
  // The local rider (the one a controller drives), for the analog tuck.
  if (const uint32_t controls = LoadGuestU32(memory, r3.u32 + 16)) {
    g_tuck_rider.store(LoadGuestU32(memory, controls + 12));
  }
  if (!REXCVAR_GET(trace_tuck)) return;
  auto& t = g_tuck;
  const uint32_t fn = LoadGuestU32(memory, r3.u32);
  const uint32_t self = LoadGuestU32(memory, r3.u32 + 16);
  const int held = (r4.u32 & 0xFF) ? 1 : 0;
  if (fn != t.fn || self != t.self) {
    t.fn = fn;
    t.self = self;
    t.frames.clear();
    t.held_log.clear();
    t.toggles = 0;
    REXLOG_INFO("tuck trace: delegate {:08X}: function {:08X}, object {:08X}, invoker {:08X}",
                r3.u32, fn, self, LoadGuestU32(memory, r3.u32 + 20));
  }
  if (!self || t.reports >= 3) return;
  const uint32_t rider = LoadGuestU32(memory, self + 12);
  const uint32_t phys = rider ? LoadGuestU32(memory, rider + 96) : 0;
  std::vector<uint8_t> frame(kTuckBytes);
  if (!ReadGuestInto(memory, self, kTuckSelfBytes, frame.data()) ||
      !ReadGuestInto(memory, rider, kTuckRiderBytes, frame.data() + kTuckSelfBytes) ||
      !ReadGuestInto(memory, phys, kTuckPhysBytes,
                     frame.data() + kTuckSelfBytes + kTuckRiderBytes)) {
    return;
  }
  g_tuck_rider.store(rider);
  g_tuck_phys.store(phys);
  const uint32_t carve = g_carve.load();
  if (!carve || !ReadGuestInto(memory, carve, kTuckCarveBytes,
                               frame.data() + kTuckSelfBytes + kTuckRiderBytes + kTuckPhysBytes)) {
    std::memset(frame.data() + kTuckSelfBytes + kTuckRiderBytes + kTuckPhysBytes, 0,
                kTuckCarveBytes);
  }
  const uint32_t branch = g_tuck_branch.exchange(0);
  t.branch_on += held ? branch : 0;
  t.branch_off += held ? 0 : branch;
  t.frames_on += held;
  if (held != t.held && t.held >= 0) {
    ++t.toggles;
    if (t.toggles <= 4 || t.toggles % 10 == 0) {
      REXLOG_INFO("tuck trace: tuck {} (change {}, rider {:08X}, physics {:08X}, carve {:08X}; "
                  "physics pose {} / {} blend {:g}, path {:08X})",
                  held ? "on" : "off", t.toggles, rider, phys, carve,
                  LoadGuestU32(memory, phys + 1512), LoadGuestU32(memory, phys + 1516),
                  LoadGuestF32(memory, phys + 1520), LoadGuestU32(memory, phys + 2404));
    }
  }
  t.held = held;
  t.frames.push_back(std::move(frame));
  t.held_log.push_back(uint8_t(held));
  // A report after 20 changes (10 tucks) over at least 20 s, or a full buffer.
  if ((t.toggles >= 20 && t.frames.size() >= 1200) || t.frames.size() >= kTuckMaxFrames) {
    if (t.toggles >= 4) {
      TuckReport();
      ++t.reports;
    }
    t.frames.clear();
    t.held_log.clear();
    t.toggles = 0;
  }
}



// ---- Diagnostics (trace_tuck_anim): the carve substate's animation trees ----
//
// carve + 312: per animation mode (carve + 228: 0 carving, 2, 3 tucked, 4),
// 20 bytes: + 0 the mode's tree, + 8 an array mapping the carve's parameter
// ids (1..6) to the tree's parameter indices, + 12 how many. A tree keeps its
// parameters at tree + 44, 44 bytes each, + 4 the parameter (value at + 4,
// target at + 12). Logged once, with any names found, to see whether a
// tree has a crouch parameter.
REXCVAR_DEFINE_BOOL(trace_tuck_anim, false, "Amped 3/Diagnostics",
                    "Log the rider's carve animation trees once (their parameters and names).");

namespace {
bool GuestReadable(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  if (address < 0x10000) return false;
  auto* heap = memory->LookupHeap(address);
  return heap && heap->QueryRangeAccess(address, address + size - 1) !=
                     rex::memory::PageAccess::kNoAccess;
}
std::string GuestCString(rex::memory::Memory* memory, uint32_t address) {
  std::string out;
  for (uint32_t i = 0; i < 64; ++i) {
    if (!GuestReadable(memory, address + i, 1)) return {};
    const char c = *memory->TranslateVirtual<const char*>(address + i);
    if (!c) break;
    if (c < 32 || c > 126) return {};
    out += c;
  }
  return out.size() >= 3 ? out : std::string{};
}
// Strings an object points to (directly or through one more pointer).
std::string StringsIn(rex::memory::Memory* memory, uint32_t object, uint32_t bytes) {
  std::string out;
  if (!GuestReadable(memory, object, bytes)) return " (unreadable)";
  for (uint32_t off = 0; off < bytes; off += 4) {
    const uint32_t p = LoadGuestU32(memory, object + off);
    if (p < 0x40000000) continue;
    std::string str = GuestCString(memory, p);
    if (str.empty() && GuestReadable(memory, p, 4)) str = GuestCString(memory, LoadGuestU32(memory, p));
    if (!str.empty()) out += fmt::format(" +{}='{}'", off, str);
  }
  return out;
}
void DumpCarveTrees(rex::memory::Memory* memory, uint32_t carve) {
  const uint32_t modes = LoadGuestU32(memory, carve + 312);
  REXLOG_INFO("tuck anim: carve {:08X}, mode {}, current tree {:08X}, mode table {:08X}, "
              "mode objects {:08X}",
              carve, LoadGuestU32(memory, carve + 228), LoadGuestU32(memory, carve + 24), modes,
              LoadGuestU32(memory, carve + 328));
  for (uint32_t m = 0; m < 6 && GuestReadable(memory, modes + m * 20, 20); ++m) {
    const uint32_t e = modes + m * 20;
    const uint32_t tree = LoadGuestU32(memory, e);
    const uint32_t ids = LoadGuestU32(memory, e + 8);
    const int count = int(LoadGuestU32(memory, e + 12));
    std::string map;
    for (int k = 0; k < count && k < 16 && GuestReadable(memory, ids + 4 * k, 4); ++k) {
      map += fmt::format(" {}->{}", k, int32_t(LoadGuestU32(memory, ids + 4 * k)));
    }
    REXLOG_INFO("tuck anim: mode {}: tree {:08X} (+4 {:08X}, +16 {:08X}), ids:{}; tree strings:{}",
                m, tree, LoadGuestU32(memory, e + 4), LoadGuestU32(memory, e + 16), map,
                tree ? StringsIn(memory, tree, 128) : std::string(" -"));
    const uint32_t mobjs = LoadGuestU32(memory, carve + 328);
    if (GuestReadable(memory, mobjs + 4 * m, 4)) {
      const uint32_t mo = LoadGuestU32(memory, mobjs + 4 * m);
      REXLOG_INFO("tuck anim:   mode object {:08X}: +0 {:08X} +4 {:08X} +8 {:08X} +12 {:08X};{}", mo,
                  mo ? LoadGuestU32(memory, mo) : 0, mo ? LoadGuestU32(memory, mo + 4) : 0,
                  mo ? LoadGuestU32(memory, mo + 8) : 0, mo ? LoadGuestU32(memory, mo + 12) : 0,
                  mo ? StringsIn(memory, mo, 64) : std::string(" -"));
    }
    if (!tree) continue;
    const uint32_t params = LoadGuestU32(memory, tree + 44);
    for (uint32_t i = 0; i < 24 && GuestReadable(memory, params + i * 44, 44); ++i) {
      const uint32_t entry = params + i * 44;
      const uint32_t param = LoadGuestU32(memory, entry + 4);
      if (!GuestReadable(memory, param, 20)) break;
      REXLOG_INFO("tuck anim:   param {}: {:08X} value {:g} target {:g};{}{}", i, param,
                  LoadGuestF32(memory, param + 4), LoadGuestF32(memory, param + 12),
                  StringsIn(memory, entry, 44), StringsIn(memory, param, 64));
    }
  }
}
}  // namespace

// ---- Analog tuck (setting analog_tuck) -------------------------------------
//
// Pulling both triggers makes the game tuck: the rider's carve substate
// (vtable 0x82059E6C, update sub_8224CE28) then picks physical pose 9 instead
// of its usual carving pose. The physical pose is a blend: physics + 1512 and
// + 1516 are two pose ids (from the table at 0x8287E4E4) and + 1520 how far
// from the first to the second, which the physics reads for the rider's drag
// and push (the speed gain) and the carve substate mirrors at + 232 / + 236 /
// + 240 (indices into that table, and the blend). The tuck itself is on/off
// (carve + 280, set while both triggers are pulled at all).
//
// Analog: while the game tucks and the strongest trigger isn't fully pulled,
// the update runs as if not tucking (so the usual carving pose is picked),
// then the blend is set from that pose to the tuck pose by the trigger.

REXCVAR_DEFINE_BOOL(analog_tuck, true, "Amped 3/Patches",
                    "Analog tuck: with both triggers pulled, how far the strongest one is pulled "
                    "sets how deep the rider tucks and how much speed it gains (the original "
                    "tucks fully as soon as both triggers are touched).");

REXCVAR_DEFINE_BOOL(trace_analog_tuck, false, "Amped 3/Diagnostics",
                    "Log the analog tuck's state once a second while a trigger is pulled.");

REX_EXTERN(__imp__sub_8224CE28);

namespace {
constexpr uint32_t kPoseTable = 0x8287E4E4;  // pose ids by pose index (lis -32120, -6940)
constexpr uint32_t kTuckPoseIndex = 9;

// The game's controllers' triggers (0..1), from its poll; up to four pads.
struct PadTriggers {
  std::atomic<uint32_t> object{0};
  std::atomic<float> strongest{0.0f};
};
PadTriggers g_pads[4];

// The strongest trigger pull across the pads (the game merges them too).
// From the controllers themselves (the port's pad watcher, every 8 ms): the
// values the game keeps can't be relied on for partial pulls.
// Mapped so that the tuck starts from nothing where the trigger starts to
// read (past XInput's usual 30/255 trigger threshold) and is full at 95% of
// the travel, leaving the end of the pull as a margin.
constexpr float kTuckPullStart = 30.0f / 255.0f;
constexpr float kTuckPullFull = 0.95f;
// The partial tuck's crouch, for the animation step: the carve substate it's
// for (0: none this frame) and how deep.
std::atomic<uint32_t> g_crouch_carve{0};
std::atomic<float> g_crouch_amount{0.0f};

float MapTuckPull(float raw) {
  return std::clamp((raw - kTuckPullStart) / (kTuckPullFull - kTuckPullStart), 0.0f, 1.0f);
}

// Diagnostics (trace_analog_tuck): how the controller's trigger values are
// spread while pulled, in 16 bins of 16, logged every 10 s.
void NoteTriggerTravel(uint8_t value) {
  static uint32_t bins[16] = {};
  static uint32_t samples = 0;
  static auto last = std::chrono::steady_clock::now();
  if (value) {
    ++bins[value >> 4];
    ++samples;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(10)) return;
  last = now;
  if (!samples) return;
  std::string line;
  for (int i = 0; i < 16; ++i) line += fmt::format(" {}", bins[i]);
  REXLOG_INFO("analog tuck: trigger values while pulled ({} samples, bins of 16 from 0 to 255):{}",
              samples, line);
  std::fill(std::begin(bins), std::end(bins), 0u);
  samples = 0;
}

// Both triggers (0..255): from XInput when an XInput controller is there
// (straight from Windows), else from the SDK's input layer.
struct TriggerPair {
  int left = 0, right = 0;
  bool known = false;
};
TriggerPair ReadTriggers() {
  const auto xi = amped3::xinput::Read();
  if (xi.left >= 0) return {xi.left, xi.right, true};
  const auto pad = amped3::pad::Latest();
  if (pad.connected) return {pad.left_trigger, pad.right_trigger, true};
  return {};
}

// The tuck amount (0..1) from the strongest trigger.
float TuckPull(const TriggerPair& t) {
  if (!t.known) {
    float a = 0.0f;  // no controller found: the game's own values
    for (auto& p : g_pads) {
      if (p.object.load()) a = std::max(a, p.strongest.load());
    }
    return MapTuckPull(a);
  }
  return MapTuckPull(std::max(t.left, t.right) / 255.0f);
}

// Both triggers pulled past the start of the range. The game itself only
// tucks once both are pulled nearly all the way (its trigger buttons), so
// the analog tuck starts its own from here.
bool BothPulled(const TriggerPair& t) {
  const int start = int(kTuckPullStart * 255.0f);
  return t.known && t.left > start && t.right > start;
}

void StoreGuestF32(uint8_t* base, uint32_t address, float value) {
  uint32_t u;
  std::memcpy(&u, &value, 4);
  rex::memory::store_and_swap<uint32_t>(base + address, u);
}
uint32_t Load32(uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}
float LoadF32(uint8_t* base, uint32_t address) {
  const uint32_t u = Load32(base, address);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
}  // namespace

// The game's controller poll, after it stored the triggers as 0..1 floats at
// controller + 160 (left) and + 164 (right).
void Amped3_TuckTriggers(PPCRegister& r31) {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !r31.u32) return;
  auto* memory = kernel->memory();
  const float lt = LoadGuestF32(memory, r31.u32 + 160);
  const float rt = LoadGuestF32(memory, r31.u32 + 164);
  const float strongest = std::isfinite(lt) && std::isfinite(rt) ? std::max(lt, rt) : 0.0f;
  if (REXCVAR_GET(trace_analog_tuck) && strongest > 0.0f) {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(1)) {
      const auto pad = amped3::pad::Latest();
      REXLOG_INFO("analog tuck: controller {:08X} triggers {:.2f} / {:.2f} (pads: {} / {} of 255); "
                  "local rider {:08X}, its carve substate {:08X}",
                  r31.u32, lt, rt, pad.left_trigger, pad.right_trigger, g_tuck_rider.load(),
                  g_carve.load());
      last = now;
    }
  }
  for (auto& p : g_pads) {
    uint32_t expected = 0;
    if (p.object.load() == r31.u32 || p.object.compare_exchange_strong(expected, r31.u32)) {
      p.strongest.store(strongest);
      return;
    }
  }
}

// The carve substate's update (this = r3, f1 = the time step).
extern "C" REX_FUNC(sub_8224CE28) {
  const uint32_t carve = ctx.r3.u32;
  const uint32_t rider = carve ? Load32(base, carve + 12) : 0;
  const bool local = rider && rider == g_tuck_rider.load();
  if (local) g_carve.store(carve);
  if (local && REXCVAR_GET(trace_tuck_anim)) {
    static uint32_t dumped = 0;
    if (dumped != carve) {
      dumped = carve;
      if (auto* kernel = rex::system::kernel_state()) DumpCarveTrees(kernel->memory(), carve);
    }
  }
  const TriggerPair triggers = ReadTriggers();
  const float pull = TuckPull(triggers);
  if (REXCVAR_GET(trace_analog_tuck) && local) {
    // Every change of the pull (both pads' raw values), for the first 400.
    static int changes = 0;
    static int last_l = -1, last_r = -1, last_x = -2;
    const auto pad = amped3::pad::Latest();
    const int xi = amped3::xinput::StrongestTrigger();  // diagnostics
    if (changes < 400 && (std::abs(pad.left_trigger - last_l) >= 6 ||
                          std::abs(pad.right_trigger - last_r) >= 6 ||
                          std::abs(xi - last_x) >= 6)) {
      ++changes;
      last_l = pad.left_trigger;
      last_r = pad.right_trigger;
      last_x = xi;
      const uint32_t phys = Load32(base, rider + 96);
      float seen = 0.0f;
      for (auto& p : g_pads) {
        if (p.object.load()) seen = std::max(seen, p.strongest.load());
      }
      REXLOG_INFO("analog tuck: game sees {:.2f}; SDK pad {} / {}, XInput strongest {} -> pull {:.2f}, tuck flag {}, "
                  "physics pose {}/{} blend {:.2f}",
                  seen, pad.left_trigger, pad.right_trigger, xi, pull, base[carve + 280],
                  phys ? Load32(base, phys + 1512) : 0, phys ? Load32(base, phys + 1516) : 0,
                  phys ? LoadF32(base, phys + 1520) : 0.0f);
    }
  }
  if (false) {
    static auto last = std::chrono::steady_clock::now();
    static uint32_t calls = 0, flagged = 0;
    ++calls;
    flagged += base[carve + 280] ? 1 : 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(1) && pull > 0.0f) {
      const uint32_t phys = Load32(base, rider + 96);
      REXLOG_INFO("analog tuck: pull {:.2f}, carve {:08X} flag {} ({} of {} updates), mode {}, "
                  "pose {}/{} blend {:.2f}, physics pose {}/{} blend {:.2f}",
                  pull, carve, base[carve + 280], flagged, calls, Load32(base, carve + 228),
                  Load32(base, carve + 232), Load32(base, carve + 236), LoadF32(base, carve + 240),
                  phys ? Load32(base, phys + 1512) : 0, phys ? Load32(base, phys + 1516) : 0,
                  phys ? LoadF32(base, phys + 1520) : 0.0f);
      last = now;
      calls = flagged = 0;
    }
  }
  if (local) g_crouch_carve.store(0);  // set again below while tucking partway
  const bool game_tuck = base[carve + 280] != 0;
  // The game must see the triggers too (it doesn't while the options menu is
  // open, or in its own menus).
  float game_seen = 0.0f;
  for (auto& p : g_pads) {
    if (p.object.load()) game_seen = std::max(game_seen, p.strongest.load());
  }
  const bool tucking = game_tuck || (game_seen > 0.0f && BothPulled(triggers));
  // Eased toward the target (~0.1 s), so a trigger that flickers or a quick
  // release doesn't snap the crouch.
  static float eased = 0.0f;
  if (local) {
    const float target = tucking ? pull : 0.0f;
    const float dt = std::clamp(float(ctx.f1.f64), 0.0f, 0.1f);
    eased += (target - eased) * std::min(1.0f, dt / 0.1f);
    if (std::fabs(target - eased) < 0.002f) eased = target;
  }
  const float amount = eased;
  if (!REXCVAR_GET(analog_tuck) || !local || amount <= 0.0f || (game_tuck && amount >= 1.0f)) {
    __imp__sub_8224CE28(ctx, base);
    return;
  }
  // Pick the carving pose as if not tucking, then blend toward the tuck pose.
  base[carve + 280] = 0;
  __imp__sub_8224CE28(ctx, base);
  base[carve + 280] = game_tuck ? 1 : 0;
  // The crouch: set on the carving animation in the animation step (below).
  g_crouch_carve.store(carve);
  g_crouch_amount.store(amount);
  const uint32_t phys = Load32(base, rider + 96);
  if (!phys) return;
  const float blend = LoadF32(base, carve + 240);
  const uint32_t from_index = Load32(base, carve + (blend < 0.5f ? 232 : 236));
  const uint32_t from_id = Load32(base, phys + (blend < 0.5f ? 1512 : 1516));
  const uint32_t tuck_id = Load32(base, kPoseTable + 4 * kTuckPoseIndex);
  rex::memory::store_and_swap<uint32_t>(base + phys + 1512, from_id);
  rex::memory::store_and_swap<uint32_t>(base + phys + 1516, tuck_id);
  StoreGuestF32(base, phys + 1520, amount);
  rex::memory::store_and_swap<uint32_t>(base + carve + 232, from_index);
  rex::memory::store_and_swap<uint32_t>(base + carve + 236, kTuckPoseIndex);
  StoreGuestF32(base, carve + 240, amount);
}

// The carve substate's animation step (sub_8224D390), at its end (r31 = the
// carve substate). Its normal carving animation (mode 0, "Carve_Regular")
// blends in crouch animations by its "Crouch" parameter (carve parameter id
// 5: the game sets it to 0 or 1, eased, for the crouch before a jump). The
// game's own tuck is a different animation (mode 3), only at a full pull;
// below that, the partial tuck sets the crouch to the tuck amount.
void Amped3_TuckCrouch(PPCRegister& r31) {
  const uint32_t carve = r31.u32;
  if (!carve || carve != g_crouch_carve.load()) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return;
  auto* memory = kernel->memory();
  uint8_t* base = memory->virtual_membase();
  constexpr uint32_t kCrouchId = 5;
  const uint32_t mode = Load32(base, carve + 228);
  if (mode != 0) return;  // only the regular carving animation has it
  const uint32_t tree = Load32(base, carve + 24);
  const uint32_t entry = Load32(base, carve + 312) + mode * 20;
  if (!tree || Load32(base, entry) != tree || Load32(base, entry + 12) <= kCrouchId) return;
  const int32_t index = int32_t(Load32(base, Load32(base, entry + 8) + 4 * kCrouchId));
  if (index < 0) return;
  const uint32_t param = Load32(base, Load32(base, tree + 44) + 44 * uint32_t(index) + 4);
  if (!param) return;
  const float amount = g_crouch_amount.load();
  // Keep the game's own crouch (before a jump) when it's deeper.
  const float value = std::max(LoadF32(base, param + 4), amount);
  const float target = std::max(LoadF32(base, param + 12), amount);
  StoreGuestF32(base, param + 4, value);
  StoreGuestF32(base, param + 12, target);
}
