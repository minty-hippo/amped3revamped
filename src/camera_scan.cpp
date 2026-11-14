// amped3 - camera scan (F8, diagnostics): where the game keeps its camera.
//
// For the noclip freecam (the game itself drawing from the freecam, so the
// whole world is there to explore), the port has to set the game's camera,
// which means knowing where it lives. The native renderer knows the camera
// exactly each frame (the lighting pass's position and view); this scans the
// game's committed memory for that position (three big-endian floats) and its
// forward and right vectors, and writes every place found, with the values
// around it, to logs\amped3_camera_scan_<time>.txt. Best with the game paused
// (the memory then holds what the frame was drawn with); two scans from
// different spots tell the camera from copies that only happened to match.

#include "camera_scan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/kernel.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "live_renderer.h"
#include "notice.h"

namespace amped3::camera_scan {
namespace {

std::filesystem::path g_dir;
std::atomic<bool> g_busy{false};

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

float F(const uint8_t* p) {
  const uint32_t b = rex::memory::load_and_swap<uint32_t>(p);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

struct Hit {
  uint32_t address;
  const char* what;
  float sign;  // the vector as found (1) or negated (-1)
};

void Scan(const float cam[9]) {
  auto* memory = GuestMemory();
  if (!memory) return;
  std::vector<Hit> hits;
  // What to look for: the position, and each axis either way round.
  struct Target {
    const char* what;
    float v[3];
    float tol;
    float sign;
  };
  std::vector<Target> targets = {{"position", {cam[0], cam[1], cam[2]}, 0.02f, 1.0f}};
  const char* names[2] = {"forward", "right"};
  for (int a = 0; a < 2; ++a) {
    for (float sign : {1.0f, -1.0f}) {
      targets.push_back({names[a],
                         {sign * cam[3 + a * 3], sign * cam[4 + a * 3], sign * cam[5 + a * 3]},
                         0.003f,
                         sign});
    }
  }
  uint64_t scanned = 0;
  // Guest virtual memory below the physical mirrors (0xC0000000 and up show
  // the same memory as 0xA0000000 again).
  uint32_t address = 0x10000;
  while (address < 0xC0000000u && hits.size() < 4000) {
    auto* heap = memory->LookupHeap(address);
    if (!heap) {
      address = (address + 0x10000) & ~0xFFFFu;
      if (!address) break;
      continue;
    }
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(address, &info) || !info.region_size) {
      address = (address + heap->page_size()) & ~(heap->page_size() - 1);
      continue;
    }
    const uint32_t start = info.base_address, size = info.region_size;
    const bool committed = (info.state & rex::memory::kMemoryAllocationCommit) != 0 &&
                           heap->QueryRangeAccess(start, start + size - 1) !=
                               rex::memory::PageAccess::kNoAccess;
    if (committed && size >= 12) {
      const uint8_t* p = memory->TranslateVirtual(start);
      scanned += size;
      for (uint32_t off = 0; off + 12 <= size && hits.size() < 4000; off += 4) {
        const float x = F(p + off);
        if (!std::isfinite(x)) continue;
        for (const Target& t : targets) {
          // (Written as "not within": a NaN is never within.)
          if (!(std::fabs(x - t.v[0]) <= t.tol)) continue;
          if (!(std::fabs(F(p + off + 4) - t.v[1]) <= t.tol)) continue;
          if (!(std::fabs(F(p + off + 8) - t.v[2]) <= t.tol)) continue;
          hits.push_back({start + off, t.what, t.sign});
        }
      }
    }
    const uint32_t next = start + size;
    if (next <= address) break;
    address = next;
  }
  // The report.
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  const auto path = g_dir / (std::string("amped3_camera_scan_") + stamp + ".txt");
  std::ofstream out(path);
  char line[512];
  std::snprintf(line, sizeof(line),
                "# Amped 3 camera scan %s\n# position %.4f %.4f %.4f  forward %.4f %.4f %.4f  "
                "right %.4f %.4f %.4f\n# game state %d, scene still %d, %.0f MB scanned, %zu "
                "places\n",
                stamp, cam[0], cam[1], cam[2], cam[3], cam[4], cam[5], cam[6], cam[7], cam[8],
                live::GameState(), int(live::SceneStill()), double(scanned) / 1048576.0,
                hits.size());
  out << line;
  for (const Hit& h : hits) {
    std::snprintf(line, sizeof(line), "\n%08X %s%s\n", h.address, h.sign < 0 ? "-" : "", h.what);
    out << line;
    // 16 floats before and 32 after, four to a line (with their addresses).
    const uint32_t from = h.address >= 64 ? h.address - 64 : 0;
    for (uint32_t a = from; a < h.address + 128; a += 16) {
      auto* heap = memory->LookupHeap(a);
      if (!heap || heap->QueryRangeAccess(a, a + 15) == rex::memory::PageAccess::kNoAccess) {
        continue;
      }
      const uint8_t* p = memory->TranslateVirtual(a);
      std::snprintf(line, sizeof(line), "  %08X: %12.5g %12.5g %12.5g %12.5g   %08X %08X %08X %08X\n",
                    a, F(p), F(p + 4), F(p + 8), F(p + 12),
                    rex::memory::load_and_swap<uint32_t>(p),
                    rex::memory::load_and_swap<uint32_t>(p + 4),
                    rex::memory::load_and_swap<uint32_t>(p + 8),
                    rex::memory::load_and_swap<uint32_t>(p + 12));
      out << line;
    }
  }
  REXLOG_INFO("Camera scan: {} places ({} MB scanned) written to {}", hits.size(),
              scanned >> 20, path.string());
  notice::Show("Camera scan: " + std::to_string(hits.size()) + " places written to logs");
}

}  // namespace

void Install(const std::filesystem::path& log_dir) { g_dir = log_dir; }

void Run() {
  float cam[9];
  if (!live::GameCamera(cam)) {
    notice::Show("Camera scan: no 3D scene seen yet (native renderer)");
    return;
  }
  if (g_busy.exchange(true)) return;
  std::thread([c = std::vector<float>(cam, cam + 9)] {
    Scan(c.data());
    g_busy = false;
  }).detach();
}

}  // namespace amped3::camera_scan
