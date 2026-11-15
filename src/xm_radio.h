// amped3 - the game's song popup (the XM Radio box, Game.rui's XMRadio scene:
// band, song title, genre, clock), to show the port's own alerts in
// (weather). Its text is filled in by the game's code when a track starts;
// finding where is the first step: Scan() logs where the popup's field names
// sit in the game's image, so the code that uses them can be found in the
// recompiled sources.

#pragma once

namespace amped3::xm_radio {

// Every so often (the watcher thread): once the game's image is loaded,
// logs (once) every string in it naming the popup's parts.
void Tick();

}  // namespace amped3::xm_radio
