// amped3 - controller access for the port's own UI. See pad_bridge.h.

#include "pad_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/system/xtypes.h>

namespace amped3::pad {

using rex::X_RESULT;  // X_ERROR_SUCCESS expands to an unqualified X_RESULT cast
using rex::X_STATUS;

namespace {
rex::input::InputSystem* g_input = nullptr;
thread_local bool t_bypass = false;

std::mutex g_latest_mutex;
Snapshot g_latest;

// PressForGame: a controller of the port's own. Added after the active
// callback is set, so it has none: the game sees it while its input is
// blocked (the replay pauses the game with it while the controller drives
// the replay).
std::atomic<int64_t> g_press_until_ns{0};
std::atomic<uint16_t> g_press_buttons{0};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

class PressDriver final : public rex::input::InputDriver {
 public:
  PressDriver() : InputDriver(nullptr, 2) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kDevice;
    info.name = "Amped 3 Revamped";
    info.synthetic = true;
    out.push_back(info);
  }
  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out) {
      std::memset(out, 0, sizeof(*out));
      const bool on = NowNs() < g_press_until_ns.load();
      out->gamepad.buttons = on ? g_press_buttons.load() : uint16_t(0);
      out->packet_number = on ? 1u : 0u;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                 rex::input::X_INPUT_CAPABILITIES* caps) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;      // gamepad
      caps->sub_type = 0x01;
      caps->gamepad.buttons = 0xFFFF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId, uint32_t,
                              rex::input::X_INPUT_KEYSTROKE*) override {
    return X_ERROR_EMPTY;
  }

 private:
  static constexpr rex::input::DeviceId kDevice = static_cast<rex::input::DeviceId>(0xA3D0'0001u);
};

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
  input->AddDriver(std::make_unique<PressDriver>());
}

void PressForGame(uint16_t buttons, int ms) {
  g_press_buttons = buttons;
  g_press_until_ns = NowNs() + int64_t(ms) * 1000000;
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
