// amped3 - the game's song popup, for the port's alerts (xm_radio.h).

#include "xm_radio.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REX_EXTERN(__imp__sub_8253DE00);  // the popup's show

namespace amped3::xm_radio {
namespace {

constexpr uint32_t kVtable = 0x82083830u;
constexpr uint32_t kNodes[3] = {152u, 160u, 156u};  // line 1 (band), 2 (song), 3 (label)
constexpr uint32_t kText = 52u;                      // a node's shown string
constexpr uint32_t kState = 140u;                    // 2: showing
constexpr size_t kMax[3] = {18, 18, 13};             // characters before "..."
constexpr uint32_t kSlot = 128u;                     // our strings: 3 slots

std::atomic<uint32_t> g_object{0};  // the popup's object (from the hooks)
std::mutex g_mutex;
bool g_pending = false;
std::string g_lines[3];
uint32_t g_strings = 0;  // our strings' guest memory (3 slots)

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

bool Readable(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  auto* heap = memory->LookupHeap(address);
  if (!heap) return false;
  if (heap->QueryRangeAccess(address, address + size - 1) == rex::memory::PageAccess::kNoAccess) return false;
  for (uint32_t a = address & ~0xFFFu; a < address + size; a += 0x1000u) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(a, &info) || !(info.state & rex::memory::kMemoryAllocationCommit)) return false;
  }
  return true;
}

// The popup's object, if it's still one (on this mountain).
uint32_t LiveObject(rex::memory::Memory* memory, uint8_t* base) {
  const uint32_t o = g_object.load();
  if (!o || !Readable(memory, o, 0xA4) || Load32(base, o) != kVtable) return 0;
  for (uint32_t n : kNodes) {
    const uint32_t node = Load32(base, o + n);
    if (!node || !Readable(memory, node, 0x60)) return 0;
  }
  return o;
}

// Our string in slot i: header (the game's allocator, as its own strings
// have; length; capacity; a reference count never run down), then the text.
uint32_t PutString(uint8_t* base, int i, const std::string& text, uint32_t allocator) {
  const uint32_t at = g_strings + uint32_t(i) * kSlot;
  std::string t = text;
  if (t.size() > kMax[i]) t = t.substr(0, kMax[i] - 3) + "...";
  const uint32_t cap = (kSlot - 16) / 2 - 1;
  if (t.size() > cap) t.resize(cap);
  Store32(base, at + 0, allocator);
  Store32(base, at + 4, uint32_t(t.size()));
  Store32(base, at + 8, cap);
  Store32(base, at + 12, 0x10000000u);
  uint8_t* c = base + at + 16;
  for (size_t k = 0; k < t.size(); ++k) {
    c[k * 2] = 0;
    c[k * 2 + 1] = uint8_t(t[k]);
  }
  c[t.size() * 2] = 0;
  c[t.size() * 2 + 1] = 0;
  return at + 16;
}

}  // namespace

bool Available() {
  auto* memory = GuestMemory();
  return memory && LiveObject(memory, memory->virtual_membase()) != 0;
}

void Alert(const std::string& line1, const std::string& line2, const std::string& line3) {
  std::lock_guard lock(g_mutex);
  g_lines[0] = line1;
  g_lines[1] = line2;
  g_lines[2] = line3;
  g_pending = true;
}

void GameTick(uint8_t* base, uint32_t r1, uint32_t r13) {
  std::string lines[3];
  {
    std::lock_guard lock(g_mutex);
    if (!g_pending) return;
    auto* memory = GuestMemory();
    if (!memory) return;
    const uint32_t o = LiveObject(memory, base);
    // (Not over a song's own: after it.)
    if (!o || Load32(base, o + kState) == 2) return;
    if (!g_strings) {
      g_strings = memory->SystemHeapAlloc(kSlot * 3);
      if (!g_strings) return;
    }
    g_pending = false;
    for (int i = 0; i < 3; ++i) lines[i] = g_lines[i];
    for (int i = 0; i < 3; ++i) {
      const uint32_t node = Load32(base, o + kNodes[i]);
      // (The allocator word from the node's own string.)
      const uint32_t old = Load32(base, node + kText);
      const uint32_t allocator = old >= 16 && Readable(memory, old - 16, 16) ? Load32(base, old - 16) : 0;
      Store32(base, node + kText, PutString(base, i, lines[i], allocator));
    }
    REXLOG_INFO("Song popup: alert '{}' / '{}' / '{}'", lines[0], lines[1], lines[2]);
  }
  // Shown as the game shows it at a new track.
  {
    const uint32_t o = g_object.load();
    PPCContext ctx{};
    ctx.r1.u64 = (r1 - 0x2000) & ~0xFull;
    ctx.r13.u64 = r13;
    ctx.r3.u64 = o;
    __imp__sub_8253DE00(ctx, base);
  }
}

}  // namespace amped3::xm_radio

// The popup's node lookup (sub_8253DC28, at its start: r3 = the object) and
// its show (sub_8253DE00, at its start: r3 = the object): which object it is.
void Amped3_XmRadioSetup(PPCRegister& r3) { amped3::xm_radio::g_object = r3.u32; }
void Amped3_XmRadioShow(PPCRegister& r3) { amped3::xm_radio::g_object = r3.u32; }
