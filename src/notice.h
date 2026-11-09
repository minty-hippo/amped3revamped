// amped3 - a short notice at the top of the screen ("Revamped Graphics: off"),
// gone after a couple of seconds. For keyboard shortcuts that change a
// setting with no menu open.

#pragma once

#include <functional>
#include <string>

namespace rex::ui {
class ImGuiDrawer;
}

namespace amped3::notice {

// After the UI is set up: `post_ui` runs a function on the UI thread.
void Install(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_ui);

// From any thread.
void Show(std::string text);

}  // namespace amped3::notice
