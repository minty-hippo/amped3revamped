// amped3 - freecam: fly the camera around the 3D scene (native renderer).
//
// Turned on with F6 or both sticks pressed in (L3 + R3). The renderer draws
// the scene from the freecam (src/native/renderer.cpp, SetFreecam) with the
// HUD left out; the game stops seeing the controller, which flies the camera.
// With the game paused (Start) it's a photo mode; while riding the game goes
// on underneath (the rider coasts). The game only draws what its own camera
// sees, so flying far from it shows gaps.
//
// Controller: left stick move, right stick look, LT / RT down / up,
// LB / RB zoom out / in, A (held) faster, X (held) slower, Y back to the
// game's camera, View show / hide the controls, B or L3 + R3 leave.
// Keyboard: W A S D move, Q / E down / up, arrows or the mouse (right button
// held) look, Z / C zoom, Shift faster, Alt slower, R back to the game's
// camera, F6 or Esc leave.

#pragma once

#include <functional>

#include "pad_bridge.h"

namespace rex::ui {
class ImGuiDrawer;
}

namespace amped3::freecam {

struct Host {
  rex::ui::ImGuiDrawer* drawer = nullptr;
  std::function<void(std::function<void()>)> post_ui;  // run on the UI thread
  // The game stops seeing the controller (true), or gets it back once every
  // button is up (false).
  std::function<void(bool)> block_game_input;
};

void Install(Host host);

// On or off (any thread). Off when the native renderer isn't drawing.
void Toggle();
bool Active();

// The controller, from the app's watcher thread (every few ms): flies the
// camera while on.
void OnPad(const pad::Snapshot& pad);

}  // namespace amped3::freecam
