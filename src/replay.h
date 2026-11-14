// amped3 - instant replay: watch the last seconds of riding again (native
// renderer; live_renderer.cpp keeps the frames, setting replay_seconds).
//
// F7, or hold View and press RB. While riding, the game is paused for it
// (Start, pressed for it) and resumed after. Freecam (F6 / L3 + R3) works
// in the replay too.
//
// Controller: A play / pause, LB / RB slower / faster, LT / RT (held) back /
// forward, Y from the start, View show / hide the controls, B leave.
// Keyboard: Space play / pause, [ ] slower / faster, Left / Right back /
// forward, Home from the start, Esc or F7 leave.

#pragma once

#include <functional>

#include "pad_bridge.h"

namespace rex::ui {
class ImGuiDrawer;
}

namespace amped3::replay {

struct Host {
  rex::ui::ImGuiDrawer* drawer = nullptr;
  std::function<void(std::function<void()>)> post_ui;
  std::function<void(bool)> block_game_input;
};

void Install(Host host);
void Toggle();  // any thread
bool Active();
// The controller, from the app's watcher thread (ignored while the freecam
// has it).
void OnPad(const pad::Snapshot& pad);

}  // namespace amped3::replay
