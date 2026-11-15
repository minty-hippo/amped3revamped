// amped3 - the game's song popup (xm_radio.h).

#include "xm_radio.h"

#include <cstdint>
#include <cstring>
#include <string>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace amped3::xm_radio {
namespace {

bool g_scanned = false;

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

bool Readable(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  auto* heap = memory->LookupHeap(address);
  if (!heap) return false;
  return heap->QueryRangeAccess(address, address + size - 1) != rex::memory::PageAccess::kNoAccess;
}

// Printable ASCII text at p (up to its end), or "".
std::string TextAt(const uint8_t* p, size_t max) {
  std::string s;
  for (size_t i = 0; i < max && p[i]; ++i) {
    if (p[i] < 32 || p[i] > 126) return {};
    s += char(p[i]);
  }
  return s;
}

}  // namespace

void Tick() {
  if (g_scanned) return;
  auto* memory = GuestMemory();
  if (!memory) return;
  uint8_t* base = memory->virtual_membase();
  // The image: 0x82000000 on (its code and read-only data), in 64 KB pages.
  constexpr uint32_t kStart = 0x82000000u, kEnd = 0x83000000u, kPage = 0x10000u;
  if (!Readable(memory, kStart, 4)) return;
  g_scanned = true;
  const char* keys[] = {"XMRadio", "SongTitle", "BandName", "MusicType", "XMRADIO"};
  int found = 0;
  for (uint32_t page = kStart; page < kEnd; page += kPage) {
    if (!Readable(memory, page, kPage)) continue;
    const uint8_t* p = base + page;
    for (uint32_t i = 0; i < kPage; ++i) {
      // The start of a string: after a zero byte.
      if (i > 0 && p[i - 1] != 0) continue;
      if (page + i + 64 >= kEnd || (i + 64 >= kPage && !Readable(memory, page + kPage, 64))) continue;
      const std::string s = TextAt(p + i, 64);
      if (s.size() < 6) continue;
      for (const char* k : keys) {
        if (s.find(k) == std::string::npos) continue;
        REXLOG_INFO("XM Radio popup: string '{}' at {:08X} (lis {}, addi {})", s, page + i,
                    int16_t(((page + i) + 0x8000) >> 16), int16_t((page + i) & 0xFFFF));
        ++found;
        break;
      }
    }
  }
  REXLOG_INFO("XM Radio popup: {} strings found in the game's image", found);
}

}  // namespace amped3::xm_radio
