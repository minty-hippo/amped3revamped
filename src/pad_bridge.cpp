// amped3 - controller access for the port's own UI. See pad_bridge.h.

#include "pad_bridge.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>

#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/system/xtypes.h>

namespace amped3::pad {

using rex::X_RESULT;  // X_ERROR_SUCCESS expands to an unqualified X_RESULT cast

namespace {
rex::input::InputSystem* g_input = nullptr;
thread_local bool t_bypass = false;

std::mutex g_latest_mutex;
Snapshot g_latest;

int16_t Stronger(int16_t a, int16_t b) {
  return std::abs(static_cast<int>(b)) > std::abs(static_cast<int>(a)) ? b : a;
}
}  // namespace

void Install(rex::input::InputSystem* input, const std::atomic<bool>* block_guest,
             std::function<bool()> overlay_wants_input) {
  g_input = input;
  if (!input) {
    return;
  }
  input->SetActiveCallback([block_guest, overlay_wants_input = std::move(overlay_wants_input)]() {
    if (t_bypass) {
      return true;
    }
    if (block_guest && block_guest->load(std::memory_order_relaxed)) {
      return false;
    }
    return !(overlay_wants_input && overlay_wants_input());
  });
}

Snapshot Read() {
  Snapshot out;
  if (!g_input) {
    return out;
  }
  t_bypass = true;
  for (uint32_t user = 0; user < 4; ++user) {
    rex::input::X_INPUT_STATE state{};
    if (g_input->GetState(user, &state) != X_ERROR_SUCCESS) {
      continue;
    }
    ++out.connected;
    const auto& gp = state.gamepad;
    out.buttons |= static_cast<uint16_t>(gp.buttons);
    out.left_trigger = std::max(out.left_trigger, gp.left_trigger);
    out.right_trigger = std::max(out.right_trigger, gp.right_trigger);
    out.lx = Stronger(out.lx, static_cast<int16_t>(gp.thumb_lx));
    out.ly = Stronger(out.ly, static_cast<int16_t>(gp.thumb_ly));
    out.rx = Stronger(out.rx, static_cast<int16_t>(gp.thumb_rx));
    out.ry = Stronger(out.ry, static_cast<int16_t>(gp.thumb_ry));
  }
  t_bypass = false;
  return out;
}

void Publish(const Snapshot& snapshot) {
  std::lock_guard<std::mutex> lock(g_latest_mutex);
  g_latest = snapshot;
}

Snapshot Latest() {
  std::lock_guard<std::mutex> lock(g_latest_mutex);
  return g_latest;
}

}  // namespace amped3::pad
