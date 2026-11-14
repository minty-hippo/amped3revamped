// amped3 - instant replay: watch the last seconds of riding again (native
// renderer; live_renderer.cpp keeps the frames, setting replay_seconds).
//
// F7, or both sticks pressed in and held 0.6 s (a tap is the freecam; D-pad
// down or View in a combo reached the game first: a snowball, a resume). While riding, the game is paused for it
// (Start, pressed for it) and resumed after. Freecam (F6 / L3 + R3) works
// in the replay too.
//
// Controller: A play / pause, LB / RB slower / faster, LT / RT (held) back /
// forward, Y from the start, View show / hide the controls, B leave.
// Keyboard: Space play / pause, [ ] slower / faster, Left / Right back /
// forward, Home from the start, H show / hide the controls, Esc or F7 leave.
// The controls are a bar along the bottom (help_bar.h).

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
// Rows the replay's bar takes at the bottom (UI thread; for a bar above it).
int BarRows();

}  // namespace amped3::replay
