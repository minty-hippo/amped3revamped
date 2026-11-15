// amped3 - night lights (time of day): lamps, floodlights, lit windows and
// campfires placed around each mountain, coming on as it gets dark (native
// renderer: the light on the world, a glow, and each one's fitting). Kept
// per mountain in lights\<id>.txt next to the logs folder.
//
// Placed from the freecam's light editor (freecam.cpp): a preview follows
// the cross at the middle of the screen, mounted on the surface there (out
// along its normal: a bulb on an arm, a floodlight on a wall or post aimed
// down and out, a window on the wall, a fire on the ground); placed as one
// light or on every copy of the object aimed at (each lift tower, post...:
// told by its mesh; copies light up as they come into view, and follow
// objects that move). Placed lights can be picked in turn (by object),
// turned, deleted; every change can be undone.

#pragma once

#include <filesystem>
#include <string>

namespace amped3::night_lights {

void Install(const std::filesystem::path& folder);
// Each frame or so (any thread): the mountain's lights loaded when it
// changes, copies found and followed, the preview moved, handed to the
// renderer.
void Tick();

// The light editor (the freecam). Each action returns a line for a notice.
void SetEditor(bool on);
bool EditorOn();
std::string Place();             // the preview, as one light
std::string PlaceOnEveryCopy();  // the preview, on every copy of the object under it
std::string NextKind(int step);  // the kind to place
std::string Rotate(float degrees);  // the preview, or the picked light (all copies)
std::string Select(int step);    // pick the next / previous placed light (by object)
bool HasSelection();
std::string Deselect();
std::string DeleteSelected();
std::string Undo();
// For the editor's panel: what's being placed, what's picked, how many.
std::string KindName();
std::string SelectionName();
int Count();

}  // namespace amped3::night_lights
