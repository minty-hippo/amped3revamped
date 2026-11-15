// amped3 - the game's song popup (xm_radio.h).

#include "xm_radio.h"

#include "challenges.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace amped3::xm_radio {
namespace {

bool g_scanned = false;
bool g_scanned_refs = false;
// The strings found (address, text): pointers to them searched for too.
std::vector<std::pair<uint32_t, std::string>> g_found;

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
        for (const auto& [a, text] : g_found) {
          if (v != a) continue;
          REXLOG_INFO("XM Radio popup: '{}' pointed to from {:08X}", text, uint32_t(page) + i);
          ++logged;
        }
      }
    }
  }
  REXLOG_INFO("XM Radio popup: {} pointers found", logged);
}

// ---- the popup's object, at run time --------------------------------------
// Its class (sub_8253D970 makes one): vtable 0x82883830; +136, +137 flags,
// +140 state (2: showing), +144 timer, +148 its animations, +152 / +156 /
// +160 its text nodes (band, genre, song: sub_8253DC28 looks them up by
// name). Found by its vtable in the game's heap; its nodes' text found by
// following their pointers to UTF-16 text.
constexpr uint32_t kVtable = 0x82883830u;
uint32_t g_object = 0;
std::vector<uint32_t> g_objects;  // every object with its vtable (one shows)
double g_next_look = 0.0, g_next_find = 0.0;
bool g_text_logged = false;

uint32_t Be32(uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// UTF-16BE text at a (printable, 2..64 characters, shown with a "u:"), or
// 8-bit text (3..64), or "".
std::string Utf16At(rex::memory::Memory* memory, uint8_t* base, uint32_t a) {
  if (!Readable(memory, a, 2)) return {};
  {
    std::string s;
    bool ok = true;
    for (int i = 0; i < 64; ++i) {
      if (!Readable(memory, a + i, 1)) {
        ok = false;
        break;
      }
      const uint8_t c = base[a + i];
      if (c == 0) break;
      if (c < 32 || c > 126) {
        ok = false;
        break;
      }
      s += char(c);
    }
    if (ok && s.size() >= 3) return s;
  }
  std::string s;
  for (int i = 0; i < 64; ++i) {
    if (!Readable(memory, a + i * 2, 2)) return {};
    const uint16_t c = uint16_t(base[a + i * 2] << 8 | base[a + i * 2 + 1]);
    if (c == 0) break;
    if (c < 32 || c > 126) return {};
    s += char(c);
  }
  return s.size() >= 2 ? "u:" + s : std::string();
}

void LookAtObject(rex::memory::Memory* memory, double now) {
  uint8_t* base = memory->virtual_membase();
  if (g_objects.empty() || now >= g_next_find) {
    // Find them: objects whose first word is the class's vtable (every
    // 20 s: made and freed with the mountain).
    g_next_find = now + 20.0;
    g_objects.clear();
    // (Its state a small number: not just a pointer to the vtable.)
    const uint32_t ranges[][2] = {{0x82000000u, 0x83000000u}, {0x40000000u, 0x80000000u}, {0xA0000000u, 0xE0000000u}};
    for (const auto& r : ranges) {
      for (uint64_t page = r[0]; page < r[1] && g_objects.size() < 16; page += 0x1000u) {
        if (!Readable(memory, uint32_t(page), 0x1000u)) continue;
        for (uint32_t i = 0; i + 0x100 <= 0x1000u; i += 4) {
          const uint32_t a = uint32_t(page) + i;
          if (Be32(base, a) == kVtable && Be32(base, a + 140) <= 4) g_objects.push_back(a);
        }
      }
    }
    std::string list;
    for (uint32_t o : g_objects) list += fmt::format(" {:08X} (state {})", o, Readable(memory, o, 0x100) ? Be32(base, o + 140) : 0);
    REXLOG_INFO("XM Radio popup: objects:{}", list);
  }
  // The one showing (state 2).
  g_object = 0;
  for (uint32_t o : g_objects) {
    if (Readable(memory, o, 0x100) && Be32(base, o) == kVtable && Be32(base, o + 140) == 2) g_object = o;
  }
  if (!g_object) return;
  // Each text node: its words, and any that lead to UTF-16 text (directly,
  // or one pointer further).
  bool any = false;
  std::string report;
  for (uint32_t field : {152u, 156u, 160u}) {
    const uint32_t node = Be32(base, g_object + field);
    report += fmt::format(" | +{} node {:08X}:", field, node);
    if (!node || !Readable(memory, node, 0x100)) continue;
    for (uint32_t off = 0; off < 0x100; off += 4) {
      const uint32_t v = Be32(base, node + off);
      if (v < 0x40000000u || v >= 0xC0000000u) continue;
      std::string t = Utf16At(memory, base, v);
      if (!t.empty()) {
        report += fmt::format(" [+{} -> '{}']", off, t);
        any = true;
        continue;
      }
      if (!Readable(memory, v, 0x40)) continue;
      for (uint32_t o2 = 0; o2 < 0x40; o2 += 4) {
        const uint32_t w = Be32(base, v + o2);
        if (w < 0x40000000u || w >= 0xC0000000u) continue;
        t = Utf16At(memory, base, w);
        if (!t.empty()) {
          report += fmt::format(" [+{} -> +{} -> '{}']", off, o2, t);
          any = true;
        }
      }
    }
  }
  const uint32_t state = Be32(base, g_object + 140);
  // (Logged while it shows, text found or not.)
  if ((any || state == 2) && !g_text_logged) {
    g_text_logged = true;
    std::string words;
    for (uint32_t i = 0; i < 64; ++i) words += fmt::format("{:08X} ", Be32(base, g_object + i * 4));
    REXLOG_INFO("XM Radio popup: object {:08X} (state {}): {}", g_object, state, words);
    REXLOG_INFO("XM Radio popup: text{}", report);
    // The nodes themselves.
    for (uint32_t field : {152u, 156u, 160u}) {
      const uint32_t node = Be32(base, g_object + field);
      if (!node || !Readable(memory, node, 0x100)) continue;
      std::string nw;
      for (uint32_t i = 0; i < 64; ++i) nw += fmt::format("{:08X} ", Be32(base, node + i * 4));
      REXLOG_INFO("XM Radio popup: node +{} {:08X}: {}", field, node, nw);
      // What its pointers lead to (16 words each).
      for (uint32_t i = 0; i < 64; ++i) {
        const uint32_t v = Be32(base, node + i * 4);
        if (v < 0x40000000u || v >= 0xC0000000u || !Readable(memory, v, 64)) continue;
        std::string tw;
        for (uint32_t k = 0; k < 16; ++k) tw += fmt::format("{:08X} ", Be32(base, v + k * 4));
        REXLOG_INFO("XM Radio popup:   node +{} word +{} -> {:08X}: {}", field, i * 4, v, tw);
      }
    }
  }
}

}  // namespace

void Tick() {
  // On a mountain, every couple of seconds: the popup's object and its text.
  if (g_scanned && !g_text_logged) {
    if (auto* memory = GuestMemory(); memory && !challenges::LoadedLevel().empty()) {
      const double now = double(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count()) /
                         1000.0;
      if (now >= g_next_look) {
        g_next_look = now + 0.5;
        LookAtObject(memory, now);
      }
    }
  }
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
  // (The song popup is the "Chiron": ARTIST: / SONG: in the string table.)
  const char* keys[] = {"XMRadio", "SongTitle", "BandName", "MusicType", "XMRADIO", "Chiron", "chiron", "CHIRON"};
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
        g_found.push_back({page + i, s});
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
