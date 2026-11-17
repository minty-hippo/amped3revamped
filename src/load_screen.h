// amped3 - loading into a mountain: the pipelines' precompiling progress
// under the loading screen's slides, and, once the mountain is loaded, the
// loading screen kept up (the native renderer holds its last picture) until
// X (or Enter / a click) is pressed to continue.

#pragma once

#include <cstdint>
#include <functional>

namespace rex::ui {
class ImGuiDrawer;
}

namespace amped3::load_screen {

struct Host {
  rex::ui::ImGuiDrawer* drawer = nullptr;
  std::function<void(std::function<void()>)> post_ui;  // runs a function on the UI thread
  std::function<void(bool)> block_game_input;           // true: the game sees an idle pad
};
void Install(Host host);

// From the native renderer's worker, each frame: the precompiling's progress
// (shown while done < total), and whether the mountain is loaded and waiting
// for the player to continue.
void Update(uint32_t done, uint32_t total, bool waiting);

// The player pressed to continue since the wait began (taken: true once).
bool TakeContinue();

// Waiting for the player (the game's input stays blocked meanwhile).
bool Waiting();

}  // namespace amped3::load_screen
