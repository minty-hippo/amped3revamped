// amped3 - the game's song popup (xm_radio.h).

#include "xm_radio.h"

#include "challenges.h"

#include <cstdint>
#include <cstring>
#include <string>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace amped3::xm_radio {
namespace {

bool g_scanned = false;
bool g_scanned_refs = false;

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

bool Readable(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  auto* heap = memory->LookupHeap(address);
  if (!heap) return false;
  if (heap->QueryRangeAccess(address, address + size - 1) == rex::memory::PageAccess::kNoAccess) return false;
  // Committed (reserved pages can carry a protection with nothing behind them).
  for (uint32_t a = address & ~0xFFFu; a < address + size; a += 0x1000u) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(a, &info) || !(info.state & rex::memory::kMemoryAllocationCommit)) return false;
  }
  return true;
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

// On a mountain (the HUD's scenes made): where the game keeps pointers to
// the popup's field names and its scene (tables and objects), and what the
// scene's holder points at.
void ScanRefs(rex::memory::Memory* memory) {
  uint8_t* base = memory->virtual_membase();
  struct Want {
    uint32_t value;
    const char* what;
  };
  const Want wants[] = {
      {0x82046974u, "XMRadioBandTitleText"}, {0x8204698Cu, "XMRadioMusicTypeText"},
      {0x820469A4u, "XMRadioSongTitleText"}, {0x8205E514u, "\"XMRadio\""},
      {0x8253D960u, "the XMRadio holder's getter (sub_8253D960)"},
      {0x8286EAF8u, "the XMRadio holder (0x8286EAF8)"},
  };
  auto be32 = [&](uint32_t a) {
    const uint8_t* p = base + a;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  if (Readable(memory, 0x8286EAF8u, 16)) {
    REXLOG_INFO("XM Radio popup: holder 0x8286EAF8 = {:08X} {:08X} {:08X} {:08X}", be32(0x8286EAF8u),
                be32(0x8286EAFCu), be32(0x8286EB00u), be32(0x8286EB04u));
    // What it points at, its first words.
    const uint32_t obj = be32(0x8286EAF8u);
    if (obj && Readable(memory, obj, 4) && Readable(memory, obj + 252, 4)) {
      std::string line;
      for (uint32_t i = 0; i < 64; ++i) line += fmt::format("{:08X}{}", be32(obj + i * 4), i % 8 == 7 ? " | " : " ");
      REXLOG_INFO("XM Radio popup: object {:08X}: {}", obj, line);
    }
  }
  // (Page by page, 4 KB each, only those readable: a 64 KB range can have
  // holes.) The image's data first: tables of names are kept there.
  constexpr uint32_t kPage = 0x1000u;
  int logged = 0;
  const uint32_t ranges[][2] = {{0x82000000u, 0x83000000u}};
  for (const auto& r : ranges) {
    for (uint64_t page = r[0]; page < r[1] && logged < 80; page += kPage) {
      if (!Readable(memory, uint32_t(page), kPage)) continue;
      for (uint32_t i = 0; i < kPage; i += 4) {
        const uint32_t v = be32(uint32_t(page) + i);
        if ((v & 0xFF000000u) != 0x82000000u) continue;
        for (const auto& w : wants) {
          if (v != w.value) continue;
          REXLOG_INFO("XM Radio popup: {} pointed to from {:08X}", w.what, uint32_t(page) + i);
          ++logged;
        }
      }
    }
  }
  REXLOG_INFO("XM Radio popup: {} pointers found", logged);
}

}  // namespace

void Tick() {
  if (g_scanned && !g_scanned_refs) {
    if (auto* memory = GuestMemory(); memory && !challenges::LoadedLevel().empty()) {
      g_scanned_refs = true;
      ScanRefs(memory);
    }
    return;
  }
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
  // (The tables in the image's data are there from the start.)
  ScanRefs(memory);
}

}  // namespace amped3::xm_radio
