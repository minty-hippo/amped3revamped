// amped3 - rail trick blend timing (diagnostics).
//
// The rail substate (vtable 0x8205C484) blends between its trick poses with
// animation-tree parameters (+4 value, +8 start, +12 target, +16 rate),
// retargeted by sub_823C5E58 from two places in the substate's code
// (0x82267224 and 0x822676D0, hooked just before the call: r3 = the parameter,
// f1 = the new target). The parameters then move toward their targets as the
// animation tree updates.
//
// This logs, for each retarget, how long the parameter takes to get there, in
// milliseconds and in frames, with the frame rate - to compare rail trick
// transitions at different frame rates.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <map>
#include <string>
#include <algorithm>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(trace_rail_blend, false, "Amped 3/Diagnostics",
                    "Log how long the rail trick pose blends take (to the log file), to compare "
                    "frame rates.");

namespace amped3::rail_trace {
namespace {

using Clock = std::chrono::steady_clock;

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

float LoadF(uint8_t* base, uint32_t address) {
  const uint32_t bits = rex::memory::load_and_swap<uint32_t>(base + address);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

struct Blend {
  uint32_t param = 0;
  uint32_t site = 0;
  float start = 0, target = 0, rate = 0;
  Clock::time_point began{};
  uint64_t frame0 = 0;
};

std::mutex g_mutex;
std::vector<Blend> g_blends;
uint64_t g_frame = 0;
Clock::time_point g_last_tick{};
double g_dt_avg = 0;  // seconds per game frame (smoothed)

bool Valid(uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u; }

void Note(uint32_t site, uint32_t param, float target) {
  if (!REXCVAR_GET(trace_rail_blend) || !Valid(param)) return;
  auto* memory = GuestMemory();
  if (!memory) return;
  uint8_t* base = memory->virtual_membase();
  Blend b;
  b.param = param;
  b.site = site;
  b.start = LoadF(base, param + 4);
  b.target = target;
  b.rate = LoadF(base, param + 16);
  b.began = Clock::now();
  std::lock_guard lock(g_mutex);
  b.frame0 = g_frame;
  if (std::fabs(b.start - target) < 0.01f) return;  // nowhere to go
  for (Blend& o : g_blends) {
    if (o.param == param) {
      o = b;  // retargeted before it got there: start over
      return;
    }
  }
  if (g_blends.size() < 32) g_blends.push_back(b);
}

// Every animation parameter step (sub_823C5C10: r3 = the parameter, f1 = the
// time step it's given): how many steps of each size, per 5 s, and the rates
// of the parameters stepped by each size.
struct StepStat {
  uint64_t count = 0;
  float rates[4] = {};
  int nrates = 0;
};
std::map<int, StepStat> g_steps;  // key: step in 0.1 ms
Clock::time_point g_steps_since{};

}  // namespace

void NoteStep(uint32_t param, double step) {
  if (!REXCVAR_GET(trace_rail_blend) || !Valid(param)) return;
  auto* memory = GuestMemory();
  if (!memory) return;
  const float rate = LoadF(memory->virtual_membase(), param + 16);
  std::lock_guard lock(g_mutex);
  StepStat& st = g_steps[int(std::lround(step * 10000.0))];
  ++st.count;
  bool seen = false;
  for (int i = 0; i < st.nrates; ++i) seen |= std::fabs(st.rates[i] - rate) < 1e-4f;
  if (!seen && st.nrates < 4) st.rates[st.nrates++] = rate;
}

// Game thread, once a frame (the game state machine's tick).
void Tick(uint8_t* base) {
  if (!REXCVAR_GET(trace_rail_blend)) return;
  const auto now = Clock::now();
  std::lock_guard lock(g_mutex);
  if (g_last_tick != Clock::time_point{}) {
    const double dt = std::chrono::duration<double>(now - g_last_tick).count();
    if (dt > 0 && dt < 0.25) g_dt_avg = g_dt_avg > 0 ? g_dt_avg * 0.95 + dt * 0.05 : dt;
  }
  g_last_tick = now;
  ++g_frame;
  if (g_steps_since == Clock::time_point{}) g_steps_since = now;
  if (now - g_steps_since > std::chrono::seconds(5)) {
    std::string line;
    int shown = 0;
    std::vector<std::pair<uint64_t, int>> order;
    for (auto& [k, st] : g_steps) order.push_back({st.count, k});
    std::sort(order.rbegin(), order.rend());
    for (auto& [count, k] : order) {
      if (shown++ == 8) break;
      const StepStat& st = g_steps[k];
      line += fmt::format("  {:.1f} ms x{} (rates", k / 10.0, count);
      for (int i = 0; i < st.nrates; ++i) line += fmt::format(" {:.3g}", st.rates[i]);
      line += ")";
    }
    REXLOG_INFO("anim steps over 5 s at {:.0f} fps ({} frames):{}", g_dt_avg > 0 ? 1.0 / g_dt_avg : 0.0,
                g_frame, line);
    g_steps.clear();
    g_steps_since = now;
  }
  for (size_t i = 0; i < g_blends.size();) {
    Blend& b = g_blends[i];
    const float v = LoadF(base, b.param + 4);
    const double ms = std::chrono::duration<double, std::milli>(now - b.began).count();
    const bool done = std::fabs(v - b.target) < 0.01f;
    if (done || ms > 3000.0) {
      REXLOG_INFO("rail blend: param {:08X} (site {:08X}) {:.2f} -> {:.2f} (rate {:.3f}) {} in "
                  "{:.0f} ms / {} frames, at {:.0f} fps",
                  b.param, b.site, b.start, b.target, b.rate, done ? "reached" : "NOT reached",
                  ms, g_frame - b.frame0, g_dt_avg > 0 ? 1.0 / g_dt_avg : 0.0);
      g_blends.erase(g_blends.begin() + i);
    } else {
      ++i;
    }
  }
}

}  // namespace amped3::rail_trace

// Midasm hooks just before the rail substate's two retargeting calls
// (bl sub_823C5E58): r3 = the parameter, f1 = the new target.
void Amped3_RailBlendA(PPCRegister& r3, PPCRegister& f1) {
  amped3::rail_trace::Note(0x82267224, r3.u32, float(f1.f64));
}
void Amped3_RailBlendB(PPCRegister& r3, PPCRegister& f1) {
  amped3::rail_trace::Note(0x822676D0, r3.u32, float(f1.f64));
}
// Midasm hook at the start of the animation parameter step (sub_823C5C10).
void Amped3_AnimParamStep(PPCRegister& r3, PPCRegister& f1) {
  amped3::rail_trace::NoteStep(r3.u32, f1.f64);
}
