// amped3 - the game's menus with the port's additions (see gui_patch_core.h).
//
// The game's files are never changed. Its menu packages are indexed by a
// directory the port can't rewrite reliably, so the additions are made in
// memory as the game loads them, by two hooks (amped3_functions.toml):
//
//   Amped3_GuiLuaLoad      Rui_ReadLuaFile (sub_821A0A70), just before it
//                          compiles a scene file it has read (bl
//                          sub_82718EE0 = luaL_loadbuffer: r4 = the bytes,
//                          r5 = their size, r6 = the file's name): the pause
//                          menu's Options scene gets the Revamped Graphics
//                          row (patched bytes in a guest buffer instead).
//   Amped3_StringTableLoaded  the string table loader (sub_821B16D0), once
//                          it has read a table (r30 = the table: +12 buffer,
//                          +16 fonts, +20 count, +24 offsets, +28 strings;
//                          r29 = its size): the row's label is added to a
//                          copy, which the table is pointed at.

#pragma once

namespace amped3::gui_patch {

// Whether the pause menu's Options has been given the Revamped Graphics row.
bool Applied();

}  // namespace amped3::gui_patch
