// amped3 - analog trigger values straight from Windows' XInput. See
// xinput_triggers.h.

#include "xinput_triggers.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <mutex>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace amped3::xinput {
namespace {

// XINPUT_STATE, declared here to avoid depending on the SDK's xinput.h.
struct Gamepad {
  WORD buttons;
  BYTE left_trigger;
  BYTE right_trigger;
  SHORT lx, ly, rx, ry;
};
struct State {
  DWORD packet;
  Gamepad gamepad;
};
using GetStateFn = DWORD(WINAPI*)(DWORD, State*);

GetStateFn Load() {
  static GetStateFn fn = [] {
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
      if (HMODULE m = LoadLibraryA(dll)) {
        if (auto f = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"))) return f;
      }
    }
    return GetStateFn{nullptr};
  }();
  return fn;
}

std::mutex g_mutex;
bool g_connected[4] = {};
std::chrono::steady_clock::time_point g_next_probe{};

}  // namespace

Triggers Read() {
  Triggers out;
  GetStateFn get = Load();
  if (!get) return out;
  std::lock_guard lock(g_mutex);
  // XInputGetState on an empty slot is slow (it searches for a device), so
  // empty slots are probed only every 3 s.
  const auto now = std::chrono::steady_clock::now();
  const bool probe = now >= g_next_probe;
  if (probe) g_next_probe = now + std::chrono::seconds(3);
  for (DWORD i = 0; i < 4; ++i) {
    if (!g_connected[i] && !probe) continue;
    State s{};
    g_connected[i] = get(i, &s) == ERROR_SUCCESS;
    if (g_connected[i]) {
      out.left = std::max<int>(out.left, s.gamepad.left_trigger);
      out.right = std::max<int>(out.right, s.gamepad.right_trigger);
    }
  }
  return out;
}

}  // namespace amped3::xinput

#else

namespace amped3::xinput {
Triggers Read() { return {}; }
}  // namespace amped3::xinput

#endif
