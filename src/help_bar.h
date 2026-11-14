// amped3 - the controls along the bottom of the screen (freecam, instant
// replay): each a short "button: action" item, laid out in rows from the
// bottom up, shown until toggled off (View, or H) and remembered that way.

#pragma once

#include <string>
#include <vector>

struct ImGuiIO;

namespace amped3::help_bar {

// UI thread, inside an ImGui frame. `title` heads the bar (a mode's name);
// `lift` rows of space are left free below it (for another bar). Returns the
// rows used (for a bar drawn above it).
int Draw(const ImGuiIO& io, const std::string& title, const std::vector<std::string>& items,
         int lift = 0);

bool Visible();
void Toggle();

}  // namespace amped3::help_bar
