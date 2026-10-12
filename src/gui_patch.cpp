// amped3 - the game's menus with the port's additions (see gui_patch.h).

#include "gui_patch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gui_patch_core.h"

REXCVAR_DEFINE_BOOL(patch_game_menus, true, "Amped 3/Patches",
                    "Add the port's entries to the game's own menus (Revamped Graphics in the "
                    "pause menu's Options), made in memory as the game loads them");

namespace amped3::gui_patch {
namespace {

std::atomic<bool> g_applied{false};
std::mutex g_mutex;

// Patched copies in guest memory, by file, reused when the file is loaded
// again (it's patched again each time, from what the game read).
struct GuestCopy {
  uint32_t address = 0;
  uint32_t capacity = 0;
};
std::map<std::string, GuestCopy> g_copies;

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

uint32_t Load32(uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(base + address, value);
}

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool EndsWith(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::string GuestCString(uint8_t* base, uint32_t address, size_t max = 512) {
  std::string s;
  if (!address) return s;
  for (size_t i = 0; i < max; ++i) {
    const char c = char(base[address + i]);
    if (!c) break;
    s.push_back(c);
  }
  return s;
}

// `bytes` in guest memory (a buffer kept for `key`).
uint32_t ToGuest(uint8_t* base, const std::string& key, const Bytes& bytes) {
  auto* memory = GuestMemory();
  if (!memory || bytes.empty()) return 0;
  GuestCopy& copy = g_copies[key];
  if (copy.capacity < bytes.size()) {
    const uint32_t size = (uint32_t(bytes.size()) + 0xFFF) & ~0xFFFu;
    const uint32_t at = memory->SystemHeapAlloc(size);
    if (!at) return 0;
    copy.address = at;  // an earlier, smaller copy is left (a few KB, rarely)
    copy.capacity = size;
  }
  std::memcpy(base + copy.address, bytes.data(), bytes.size());
  return copy.address;
}

}  // namespace

bool Applied() { return g_applied.load(); }

}  // namespace amped3::gui_patch

using namespace amped3::gui_patch;

// Rui_ReadLuaFile, before it compiles the file it read (r4 = the bytes, r5 =
// their size, r6 = the file's name).
void Amped3_GuiLuaLoad(PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
  if (!REXCVAR_GET(patch_game_menus)) return;
  auto* memory = GuestMemory();
  if (!memory || !r4.u32 || !r5.u32 || r5.u32 > (16u << 20)) return;
  uint8_t* base = memory->virtual_membase();
  const std::string name = Lower(GuestCString(base, r6.u32));
  bool (*patch)(const Bytes&, Bytes*, std::string*) = nullptr;
  bool scene_data = false;
  if (EndsWith(name, "globaloptionsmenu2.scene")) {
    patch = PatchScene;
  } else if (EndsWith(name, "globaloptionsmenu2.scenedata")) {
    patch = PatchSceneData;
    scene_data = true;
  }
  if (!patch) {
    if (name.find("globaloptionsmenu2") != std::string::npos) {
      REXLOG_INFO("Menu patches: not patching '{}' (unexpected name)", name);
    }
    return;
  }
  std::lock_guard lock(g_mutex);
  const Bytes in(base + r4.u32, base + r4.u32 + r5.u32);
  Bytes out;
  std::string error;
  if (!patch(in, &out, &error)) {
    REXLOG_WARN("Menu patches: {}: {} (left as it is)", name, error);
    if (scene_data) g_applied = false;
    return;
  }
  const uint32_t at = ToGuest(base, name, out);
  if (!at) return;
  r4.u64 = at;
  r5.u64 = uint32_t(out.size());
  if (scene_data) {
    g_applied = true;
    REXLOG_INFO("Menu patches: Revamped Graphics added to the pause menu's Options");
  }
}

// The string table loader, once a table is read (r30 = the table, r29 = its
// size): the Revamped Graphics label added to the menus' table.
void Amped3_StringTableLoaded(PPCRegister& r29, PPCRegister& r30) {
  if (!REXCVAR_GET(patch_game_menus)) return;
  auto* memory = GuestMemory();
  if (!memory || !r30.u32) return;
  uint8_t* base = memory->virtual_membase();
  const uint32_t table = r30.u32;
  const uint32_t buffer = Load32(base, table + 12);
  const uint32_t size = r29.u32;
  if (!buffer || size < 0x100 || size > (16u << 20)) return;
  const Bytes in(base + buffer, base + buffer + size);
  // Only the menus' table (the one with the Options rows).
  static const char kRow[] = "Options_Button02";
  if (std::search(in.begin(), in.end(), kRow, kRow + sizeof(kRow) - 1) == in.end()) return;
  std::lock_guard lock(g_mutex);
  Bytes out;
  std::string error;
  if (!PatchStringTable(in, &out, &error)) {
    REXLOG_WARN("Menu patches: string table: {} (left as it is)", error);
    return;
  }
  if (out.size() == in.size()) return;  // already has it
  // A separate copy per table buffer (each language is its own table).
  const uint32_t at = ToGuest(base, "strings@" + std::to_string(buffer), out);
  if (!at) return;
  // The loader's own pointers into it, as it sets them: fonts at +4, the
  // count after them, then the offsets (count + 1) and the strings. The
  // original buffer stays the table's own (+12) to be freed with it.
  const uint32_t fonts = Load32(base, at);
  const uint32_t count_at = at + 4 + fonts * 48;
  const uint32_t count = Load32(base, count_at);
  Store32(base, table + 16, at + 4);
  Store32(base, table + 20, count);
  Store32(base, table + 24, count_at + 4);
  Store32(base, table + 28, count_at + 4 + (count + 1) * 4);
}
