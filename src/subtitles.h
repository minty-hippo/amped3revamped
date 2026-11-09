// amped3 - subtitles for the game's dialogue (settings subtitles,
// subtitles_size; Options > Audio > Subtitles).
//
// The lines come from subtitles/amped3.subtitles next to the exe, made at
// install on the player's PC by listening to their own copy of the game
// (tools/subtitles, the installer's Subtitles step; src/subtitle_file.h).
// Without that file there's nothing to show and nothing runs.
//
// While the game plays, a thread looks at the audio hardware's XMA decoder
// contexts (as the SDK's decoder reads them, in guest memory) every 20 ms.
// When one starts on new data, the start of that data is hashed and looked
// up: a sound in the file is playing, and its lines are shown at their times
// from where it is (the decoder's read position for a sound held whole in
// memory, else the time it has spent decoding, so pausing the game pauses its
// subtitles too). Shown: story cutscene dialogue and movies' sound tracks
// (subtitles = 1), or every line of speech the game plays (2).

#pragma once

#include <filesystem>
#include <functional>

namespace rex {
class Runtime;
namespace ui {
class ImGuiDrawer;
}
}  // namespace rex

namespace amped3::subtitles {

// After the runtime is set up. `folder` holds amped3.subtitles; `post_ui`
// runs a function on the UI thread (the overlay is made and removed there).
void Start(rex::Runtime* runtime, rex::ui::ImGuiDrawer* drawer, std::filesystem::path folder,
           std::function<void(std::function<void()>)> post_ui);
void Stop();

// For the options menu: how many sounds have subtitles (0: no file).
int ClipCount();

}  // namespace amped3::subtitles
