// amped3 - night lights (time of day): lamps and lit windows placed around
// each mountain, coming on as it gets dark (native renderer: the light on the
// world and a glow round each). Kept per mountain in lights\<id>.txt next to
// the logs folder, one light a line: x y z  r g b  radius intensity.
//
// Placed from the freecam (freecam.cpp): the point at the middle of the
// screen, in one of a few kinds (a lamp, a floodlight, a window, a fire).

#pragma once

#include <filesystem>
#include <string>

namespace amped3::night_lights {

void Install(const std::filesystem::path& folder);
// Each frame or so (any thread): the mountain's lights loaded when it
// changes, handed to the renderer.
void Tick();
// Freecam editing. Each returns a short line for a notice.
std::string Place();          // at the middle of the screen, the current kind
std::string RemoveNearest();  // the one nearest the middle of the screen
std::string NextKind();
std::string KindName();
int Count();

}  // namespace amped3::night_lights
