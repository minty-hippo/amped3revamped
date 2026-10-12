// amped3 - the Graphics row in the game's own Options menu (see
// options_graphics.cpp; the menu itself is patched by gui_patch.cpp).

#pragma once

#include <functional>

namespace amped3::options_graphics {

// Called (on the game thread) when A is pressed on the Graphics row.
void SetOpenPanel(std::function<void()> open);

}  // namespace amped3::options_graphics
