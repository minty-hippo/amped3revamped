// amped3 - controller access for the port's own UI.
//
// The game reads controllers through the SDK's InputSystem (SDL3 underneath:
// Xbox, PlayStation, Switch Pro and most other pads, with hot-plug). While the
// options menu is open the game must see an idle controller, but the menu
// itself still needs the real state. The SDK's "active" callback is consulted
// on the calling thread, so a thread-local bypass lets the menu read through
// while every guest thread sees neutral input.
//
// SDL lives statically inside rexruntime.dll, so this goes through the SDK's
// exported InputSystem rather than calling SDL from the executable (which
// would talk to a second, uninitialized copy of SDL).

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace rex::input {
class InputSystem;
}

namespace amped3::pad {

struct Snapshot {
  uint16_t buttons = 0;  // X_INPUT_GAMEPAD_* bits, merged across all pads
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;  // strongest deflection across pads
  int connected = 0;                      // user slots reporting a device
};

// Replaces the SDK's input-active callback. Guest threads get neutral input
// while `block_guest` is true or while an SDK overlay wants the mouse;
// `overlay_wants_input` mirrors the SDK's own rule for its F3/F4/` overlays.
void Install(rex::input::InputSystem* input, const std::atomic<bool>* block_guest,
             std::function<bool()> overlay_wants_input);

// Reads all four user slots, bypassing the guest block. Call from a worker
// thread only: on the UI thread the SDK's driver would pump SDL events in the
// middle of the ImGui frame.
Snapshot Read();

// The app's watcher thread publishes each Read(); UI code uses Latest().
void Publish(const Snapshot& snapshot);
Snapshot Latest();

}  // namespace amped3::pad
