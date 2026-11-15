// amped3 - the game's song popup (the XM Radio box: AirWare / Pioneer /
// xm2go, band, song and record label, sliding in at each new track), used for
// the port's own alerts (weather).
//
// The game's popup (Game.rui's XMRadio scene; its class in the recompiled
// code): one object a mountain, vtable 0x82083830, made by sub_8253D858 /
// sub_8253D970. sub_8253DC28 looks up its text nodes by name: +152 the band
// (line 1), +160 the song (line 2), +156 the label (line 3, cyan). Each node
// shows the game string at its +52: UTF-16BE characters after a 16-byte
// header (its allocator, length, capacity, reference count). sub_8253DE00
// shows the popup (state +140 = 2, a timer), sub_8253DFE0 ticks it (slides
// it in, then out).
//
// An alert: our own strings (never freed: a huge reference count) put in the
// three nodes, then the popup shown, from the game thread, when it isn't
// already showing a song. The next song puts its own back.

#pragma once

#include <cstdint>
#include <string>

namespace amped3::xm_radio {

// Whether the popup can be used now (its object seen on this mountain).
bool Available();
// Any thread: shows the popup with these lines (line 1 and 2 about 18
// characters, line 3 about 13; longer ones are cut with "...").
void Alert(const std::string& line1, const std::string& line2, const std::string& line3);

// The game thread, every frame (the state machine's tick).
void GameTick(uint8_t* base, uint32_t r1, uint32_t r13);

}  // namespace amped3::xm_radio
