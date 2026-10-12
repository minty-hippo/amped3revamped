// amped3 - challenge launcher (the options menu's Challenges tab).
//
// Lists every challenge the game knows (story missions included), grouped by
// mountain, and starts any one of the loaded mountain's on demand: the rider
// is put at the challenge's start and its initiator is switched to
// "activate", as when the player triggers it in the world. Story challenges
// that were already played (locked or done) start the same way.
//
// How it works (default.xex):
//   0x8288F0A8 -> +48   the challenge registry: +8 entries, +12 count. An
//                       entry: +8 name, +12 level name ("global" for some),
//                       +96 its initiator entity (only while its mountain is
//                       loaded; cleared when the entity goes).
//   entity +244         the initiator: a state machine (+8 states, +60 state,
//                       +64 requested, +68 request pending) whose states are
//                       Init, Locked, Idle, ActivateChallenge, ... ; +72 the
//                       entry, +76 the entity. Idle goes to ActivateChallenge
//                       (3) when the player triggers the challenge.
//   sub_82172C78(entity) puts the rider at the entity's start point (+308
//                       position, +320 orientation).
// Requests are carried out on the game thread (the game state machine's tick)
// while the game is riding.

#pragma once

#include <cstdint>

namespace amped3::challenges {

// UI thread: draws the Challenges tab. Returns true when the menu should close.
bool DrawTab();

// Game thread, every frame (the game state machine's tick; r1 / r13 of that
// code, for calling the game's functions).
void GameTick(uint8_t* base, uint32_t r1, uint32_t r13);

}  // namespace amped3::challenges
