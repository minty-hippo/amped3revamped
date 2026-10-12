// amped3 - audio diagnostics (setting trace_audio).
//
// Logs the requests Amped 3's sound code ("Gaia" audio controller) queues
// for its audio worker thread - cue plays, cue stops, category pause/resume
// and category volume changes, with the guest return address of the caller -
// and watches the XMA decoder's contexts to log which compressed streams
// (music is the long-running stereo ones) are being decoded and when they
// start and stop. Used to find why the in-game song keeps playing after
// returning to the main menu.

#pragma once

namespace rex {
class Runtime;
}

namespace amped3::audio_trace {

void Start(rex::Runtime* runtime);
void Stop();

}  // namespace amped3::audio_trace
