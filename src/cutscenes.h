// amped3 - story cutscene player (the options menu's Cutscenes tab).
//
// Plays any of the game's story cutscenes ("<name>.grover" scenes) on demand,
// and skips or restarts the one playing, for quick iteration on cutscene
// rendering fixes.
//
// How it works: the game plays a story scene in its game state 20 (the
// challenge intro). Entering that state freezes the riders, plays the scene
// named by the current challenge and, when the scene ends or is skipped,
// goes back to riding (state 13). A Play request from the menu is handed to
// the game thread, which asks the game's own state machine for state 20 with
// the chosen scene played in place of the challenge's (and restores it
// afterwards); the scene ends back in riding, as the game's own do. A Play
// request waits until the game is riding (or in a scene). Skip stops the scene
// the way the game's own skip does.

#pragma once

namespace amped3::cutscenes {

// UI thread: draws the Cutscenes tab's contents. Returns true when the menu
// should close (a scene was started, so it isn't hidden behind the menu).
bool DrawTab();

}  // namespace amped3::cutscenes
