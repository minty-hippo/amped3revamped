// amped3 - patches for the game's menus (pure functions; no runtime needed).
//
// The game's menus live in d:\GuiPackages\*.rui: zip archives of compiled
// Lua 5.0 chunks (scenes), binary action scripts and string tables. These functions
// take a package's bytes and return them with the port's additions, made
// from the user's own files at run time (no game data ships with the port):
//
//   Game.rui  Scenes/GlobalOptionsMenu2 (the pause menu's Options): the
//             unused "Cheat Codes" row becomes "Graphics", which opens the
//             port's Graphics panel.
//   Base.rui  StringTables/*/StringTable.dat: the row's label.
//   resources.dir  the two packages' listings, to match.
//
// tools/gui-patch runs them on the command line for testing.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace amped3::gui_patch {

using Bytes = std::vector<uint8_t>;

// Bumped whenever the patches change, so cached patched packages are rebuilt.
inline constexpr int kPatchVersion = 3;

// The label's string-table ID and text.
inline constexpr const char* kLabelId = "Options_AmpedGraphics";
inline constexpr const char* kLabelText = "Graphics";

// Whole packages. Return false (and say why in *error) if anything in the
// package isn't laid out as expected; *out is then unusable.
bool PatchGamePackage(const Bytes& in, Bytes* out, std::string* error);
bool PatchBasePackage(const Bytes& in, Bytes* out, std::string* error);

// d:\resources.dir lists every archive's entries (the game reads archives by
// it, not by their own directories): the listings of `archives` (name as in
// resources.dir, e.g. "GuiPackages/Base.rui", and the archive's new bytes)
// rewritten to match.
bool PatchResourceDirectory(const Bytes& in,
                            const std::vector<std::pair<std::string, const Bytes*>>& archives,
                            Bytes* out, std::string* error);

// The pieces (gui_patch.cpp applies them in memory as the game loads).
bool PatchStringTable(const Bytes& in, Bytes* out, std::string* error);
bool PatchActions(const Bytes& in, Bytes* out, std::string* error);
bool PatchScene(const Bytes& in, Bytes* out, std::string* error);
bool PatchSceneData(const Bytes& in, Bytes* out, std::string* error);
// A compiled scene chunk evaluated and compiled again, unchanged.
bool LuaChunkRecompiled(const Bytes& in, Bytes* out, std::string* error);

// Zip archives (deflate or stored entries).
struct ZipEntry {
  std::string name;
  Bytes data;          // uncompressed
  Bytes raw;           // the original compressed bytes (kept for unchanged entries)
  uint16_t method = 8;
  uint16_t time = 0, date = 0;
  uint32_t crc = 0;
  bool changed = false;
};
bool ReadZip(const Bytes& in, std::vector<ZipEntry>* entries, std::string* error);
Bytes WriteZip(const std::vector<ZipEntry>& entries);

}  // namespace amped3::gui_patch
