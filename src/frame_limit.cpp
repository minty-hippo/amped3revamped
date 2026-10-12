// amped3 - frame rate cap (see frame_limit.h).

#include "frame_limit.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

REXCVAR_DEFINE_STRING(frame_rate_limit, "30", "Display",
                      "Frame rate cap: 30, 40, 60, 90, 120, or 0 for unlocked. 30 is the "
                      "console's own pacing (gameplay at 30, menus at 60, following the "
                      "monitor); the others pace the game on the PC.");

namespace amped3::frame_limit {
namespace {

using Clock = std::chrono::steady_clock;

std::string g_applied;           // the setting last applied
double g_fps = 0;                // 0: no pacing here
Clock::time_point g_next{};      // when the next frame may start
Clock::time_point g_last_swap{};
std::atomic<double> g_frame_seconds{0.0};

#if defined(_WIN32)
// A high-resolution waitable timer sleeps to within ~0.5 ms without raising
// the system timer resolution for the whole process.
HANDLE Timer() {
  static HANDLE timer = [] {
    HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    if (!t) t = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    return t;
  }();
  return timer;
}
#endif

// Sleeps until `when`: coarse sleep, then a short spin for the last stretch.
void WaitUntil(Clock::time_point when) {
  constexpr auto kSpin = std::chrono::microseconds(1000);
  auto now = Clock::now();
  if (when - now > kSpin) {
    const auto sleep = when - now - kSpin;
#if defined(_WIN32)
    if (HANDLE t = Timer()) {
      LARGE_INTEGER due;
      due.QuadPart =
          -std::max<LONGLONG>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(sleep).count() / 100);
      if (SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(t, INFINITE);
      } else {
        std::this_thread::sleep_for(sleep);
      }
    } else {
      std::this_thread::sleep_for(sleep);
    }
#else
    std::this_thread::sleep_for(sleep);
#endif
  }
  while (Clock::now() < when) std::this_thread::yield();
}

void Apply(const std::string& value) {
  int fps = 30;
  try {
    fps = std::stoi(value);
  } catch (...) {
  }
  if (fps != 0 && fps != 30 && fps != 40 && fps != 60 && fps != 90 && fps != 120) fps = 30;
  // 30: the console's own pacing (it runs gameplay at every second 60 Hz
  // vertical blank). Otherwise the vblank throttle is off and the cap (if
  // any) is ours.
  rex::cvar::SetFlagByName("vsync", fps == 30 ? "true" : "false");
  g_fps = fps == 30 ? 0 : fps;
  g_next = Clock::now();
  if (fps == 0) {
    REXLOG_INFO("Frame rate: unlocked");
  } else {
    REXLOG_INFO("Frame rate: capped at {} fps{}", fps, fps == 30 ? " (console pacing)" : "");
  }
}

}  // namespace

double FrameSeconds() { return g_frame_seconds.load(std::memory_order_relaxed); }

void OnSwap() {
  {
    const auto now = Clock::now();
    if (g_last_swap != Clock::time_point{}) {
      const double s = std::chrono::duration<double>(now - g_last_swap).count();
      if (s > 0.0 && s < 0.25) {
        const double old = g_frame_seconds.load(std::memory_order_relaxed);
        g_frame_seconds.store(old > 0.0 ? old + (s - old) * 0.25 : s, std::memory_order_relaxed);
      }
    }
    g_last_swap = now;
  }
  const std::string value = REXCVAR_GET(frame_rate_limit);
  if (value != g_applied) {
    // First run with this setting: someone who had switched the vertical-blank
    // throttle off (the old way to unlock the frame rate) stays unlocked.
    if (g_applied.empty() && value == "30" && rex::cvar::GetFlagByName("vsync") == "false") {
      rex::cvar::SetFlagByName("frame_rate_limit", "0");
      g_applied = "0";
      Apply("0");
    } else {
      g_applied = value;
      Apply(value);
    }
  }
  if (g_fps <= 0) return;
  const auto interval = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(1.0 / g_fps));
  WaitUntil(g_next);
  // The next slot follows this one, so short and long frames even out; after
  // a long stall (loading) the schedule restarts from now instead of letting
  // frames catch up in a burst.
  const auto now = Clock::now();
  g_next += interval;
  if (g_next < now - interval) g_next = now;
}

}  // namespace amped3::frame_limit
