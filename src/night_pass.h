// amped3 - the Night Pass: Northstar's time of day stays at the mountain's own
// hour (the game's) until the player has earned it, then the dynamic day and
// night runs there as on every other mountain.
//
// Earned is told from the game's challenge registry (challenges.h): a
// Northstar challenge named for the night pass that's done, or one named for
// the night that's no longer locked. Once seen it's remembered
// (night_pass.txt next to the logs folder) for good. Each time Northstar loads
// its challenges are logged (with trace_challenges), so the rule can be
// checked against a save that has it. The lock itself is a setting
// (native_tod_night_pass_lock).

#pragma once

#include <filesystem>

namespace amped3::night_pass {

void Install(const std::filesystem::path& folder);
// Every frame or so (the watcher thread): the mountain loaded, the registry
// read now and then.
void Tick();
// The time of day held at the mountain's hour now (Northstar, not earned).
bool Locked();
bool Earned();

}  // namespace amped3::night_pass
