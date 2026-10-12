// amped3 - the Graphics row in the game's own Options menu.
//
// The pause menu's Options (scene GlobalOptionsMenu2, patched in memory by
// gui_patch.cpp) gets a "Graphics" row where the unused "Cheat Codes" was. A
// on it calls this.ToggleHint() (the scene's only way to call into C++); a
// hook at ToggleHint's start sees the cursor on our row and opens the port's
// Graphics panel (graphics_panel.cpp) instead. The menu's C++ side is the
// game's COptions class:
//
//   +132 ButtonList (the rows), +44 of a list = its selection
//   +140 Media Callouts' row: 3 in the pause menu, 4 on the title screen
//   sub_822E6F68 activate, sub_822E76B8 ToggleHint

#include <cstdint>
#include <functional>
#include <mutex>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gui_patch.h"
#include "options_graphics.h"

namespace amped3::options_graphics {
namespace {

constexpr uint32_t kOurRow = 1;  // the row under Audio (was "Cheat Codes")
constexpr uint32_t kPauseMenuMediaRow = 3;

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

uint32_t Load32(uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}

std::mutex g_mutex;
std::function<void()> g_open_panel;

// The pause menu's Options while it's up (game thread only).
uint32_t g_options = 0;

}  // namespace

void SetOpenPanel(std::function<void()> open) {
  std::lock_guard lock(g_mutex);
  g_open_panel = std::move(open);
}

}  // namespace amped3::options_graphics

using namespace amped3::options_graphics;

// Activate, after the switches are set (r31 = COptions).
void Amped3_OptionsActivate(PPCRegister& r31) {
  g_options = 0;
  if (!amped3::gui_patch::Applied()) return;
  auto* memory = GuestMemory();
  if (!memory) return;
  // Only the pause menu's Options has our row (the title screen's has five
  // rows, Media Callouts the fifth).
  if (Load32(memory->virtual_membase(), r31.u32 + 140) == kPauseMenuMediaRow) {
    g_options = r31.u32;
  }
}

// ToggleHint, at its start (r3 = COptions): on our row, open the Graphics
// panel and return without toggling Hints.
bool Amped3_OptionsGraphicsRow(PPCRegister& r3) {
  if (!g_options || r3.u32 != g_options) return false;
  auto* memory = GuestMemory();
  if (!memory) return false;
  uint8_t* base = memory->virtual_membase();
  const uint32_t rows = Load32(base, g_options + 132);
  if (!rows || Load32(base, rows + 44) != kOurRow) return false;
  std::function<void()> open;
  {
    std::lock_guard lock(g_mutex);
    open = g_open_panel;
  }
  if (open) open();
  return true;
}
